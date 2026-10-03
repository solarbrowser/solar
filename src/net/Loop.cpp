#include <algorithm>

#include "LoopCore.h"

namespace solar::net {

using internal::ConnectionState;

namespace {

ConnectionState& State(Connection* connection) { return *static_cast<ConnectionState*>(connection); }

}  // namespace

// ---- What a backend reports ----

void Loop::Impl::Connected(ConnectionState& c, int error) {
  if (c.closing) return;
  if (error != 0) {
    c.error = error;
    CloseConnection(c);
    return;
  }
  c.connected = true;
  c.handler->OnConnected();
  if (c.closing) return;
  backend->StartReceive(c);
  if (!c.sendQueue.empty()) backend->WantSend(c);
}

void Loop::Impl::Received(ConnectionState& c, std::span<const uint8_t> data) {
  if (!c.closing) c.handler->OnData(data);
}

void Loop::Impl::PeerClosed(ConnectionState& c) {
  CloseConnection(c);  // the peer closed its side, which is a clean end
}

void Loop::Impl::ReceiveFailed(ConnectionState& c, int error) {
  if (c.closing) return;
  c.error = error;
  CloseConnection(c);
}

void Loop::Impl::Sent(ConnectionState& c, size_t bytes) {
  c.sendOffset += bytes;
  if (c.sendOffset == c.sendQueue.front().size()) {
    c.sendQueue.pop_front();
    c.sendOffset = 0;
  }
}

void Loop::Impl::SendFailed(ConnectionState& c, int error) {
  if (c.closing) return;
  c.error = error;
  CloseConnection(c);
}

void Loop::Impl::Quiescent(ConnectionState& c) {
  if (c.closing) QueueFinalize(c);
}

std::string_view Loop::Impl::PendingSend(const ConnectionState& c) const {
  if (c.sendQueue.empty()) return {};
  return std::string_view(c.sendQueue.front()).substr(c.sendOffset);
}

// ---- The core ----

void Loop::Impl::CloseConnection(ConnectionState& c) {
  if (c.closing) return;
  c.closing = true;
  backend->Close(c);
}

void Loop::Impl::QueueFinalize(ConnectionState& c) {
  if (c.finalizeQueued) return;
  c.finalizeQueued = true;
  finalizable.push_back(&c);
}

// Connections are destroyed from here, between callbacks, never from inside one of their own.
void Loop::Impl::DrainFinalizable() {
  while (!finalizable.empty()) {
    std::vector<ConnectionState*> batch;
    batch.swap(finalizable);
    for (ConnectionState* c : batch) {
      backend->Release(*c);
      ConnectionHandler* handler = c->handler;
      const int error = c->error;
      if (!c->idle) --busyConnections;
      connections.erase(c->id);
      delete c;
      handler->OnClosed(error);
    }
  }
}

bool Loop::Impl::HasPosted() {
  std::lock_guard<std::mutex> lock(postedMutex);
  return !posted.empty();
}

void Loop::Impl::RunPosted() {
  std::vector<Task> batch;
  {
    std::lock_guard<std::mutex> lock(postedMutex);
    batch.swap(posted);
  }
  for (Task& task : batch) task();
}

void Loop::Impl::RunTimers() {
  while (!timerOrder.empty() && timerOrder.begin()->first <= Clock::now()) {
    const TimerId id = timerOrder.begin()->second;
    timerOrder.erase(timerOrder.begin());
    Timer timer = std::move(timers[id]);
    timers.erase(id);
    if (!timer.background) --foregroundTimers;
    timer.task();
  }
}

// Milliseconds until the next timer, rounded up so the loop does not wake just short of it.
int Loop::Impl::TimeoutMs() const {
  if (timerOrder.empty()) return -1;
  const auto remaining = timerOrder.begin()->first - Clock::now();
  if (remaining <= Clock::duration::zero()) return 0;
  const auto ms = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
  return static_cast<int>(std::min<long long>(ms, 1 << 30));
}

// ---- The public interface ----

Loop::Loop() : impl_(std::make_unique<Impl>()) {}

std::unique_ptr<Loop> Loop::Create(LoopBackend choice) {
  InitializeSockets();
  std::unique_ptr<Loop> loop(new Loop());
  Impl& core = *loop->impl_;

#ifdef _WIN32
  if (choice == LoopBackend::Automatic) core.backend = internal::MakeIocpBackend(core);
#else
  if (choice == LoopBackend::Automatic || choice == LoopBackend::IoUring) core.backend = internal::MakeUringBackend(core);
  if (!core.backend && (choice == LoopBackend::Automatic || choice == LoopBackend::Readiness)) {
    core.backend = internal::MakeReadinessBackend(core);
  }
#endif
  if (!core.backend) return nullptr;
  return loop;
}

Loop::~Loop() {
  if (!impl_->backend) return;
  for (auto& [id, c] : impl_->connections) {
    impl_->backend->Release(*c);
    delete c;
  }
}

const char* Loop::backend_name() const { return impl_->backend->Name(); }

Connection* Loop::Connect(const SocketAddress& address, ConnectionHandler* handler) {
  auto* c = new ConnectionState();
  c->loop = impl_.get();
  c->handler = handler;
  c->address = address;
  c->id = impl_->nextConnectionId++;
  impl_->connections.emplace(c->id, c);
  ++impl_->busyConnections;
  impl_->backend->Connect(*c);
  return c;
}

void Loop::Post(Task task) {
  {
    std::lock_guard<std::mutex> lock(impl_->postedMutex);
    impl_->posted.push_back(std::move(task));
  }
  impl_->backend->Wake();
}

void Loop::BeginExternalWork() { ++impl_->externalWork; }
void Loop::EndExternalWork() { --impl_->externalWork; }

Loop::TimerId Loop::PostDelayed(std::chrono::milliseconds delay, Task task, bool background) {
  const TimerId id = impl_->nextTimer++;
  const auto when = Impl::Clock::now() + delay;
  impl_->timerOrder.emplace(when, id);
  impl_->timers.emplace(id, Impl::Timer{when, std::move(task), background});
  if (!background) ++impl_->foregroundTimers;
  return id;
}

bool Loop::CancelTimer(TimerId id) {
  auto it = impl_->timers.find(id);
  if (it == impl_->timers.end()) return false;
  impl_->timerOrder.erase({it->second.when, id});
  if (!it->second.background) --impl_->foregroundTimers;
  impl_->timers.erase(it);
  return true;
}

void Loop::Stop() { impl_->stopped = true; }

void Loop::Run() {
  Impl& core = *impl_;
  core.stopped = false;
  while (!core.stopped) {
    core.RunPosted();
    core.RunTimers();
    core.DrainFinalizable();
    if (!core.Pending() && core.finalizable.empty() && !core.HasPosted()) break;
    if (core.stopped) break;
    if (!core.backend->Wait(core.TimeoutMs())) break;
  }
}

void Connection::Send(std::string data) {
  ConnectionState& c = State(this);
  if (c.closing || data.empty()) return;
  c.sendQueue.push_back(std::move(data));
  if (c.connected) c.loop->backend->WantSend(c);
}

void Connection::Close() { State(this).loop->CloseConnection(State(this)); }

void Connection::SetHandler(ConnectionHandler* handler) { State(this).handler = handler; }

void Connection::SetIdle(bool idle) {
  ConnectionState& c = State(this);
  if (c.idle == idle) return;
  c.idle = idle;
  if (idle) {
    --c.loop->busyConnections;
  } else {
    ++c.loop->busyConnections;
  }
}

}  // namespace solar::net
