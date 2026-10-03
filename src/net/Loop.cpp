#include "solar/net/Loop.h"

#include <liburing.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace solar::net {

namespace {

constexpr unsigned kRingEntries = 256;
constexpr unsigned kBufferCount = 256;  // a power of two, as the buffer ring requires
constexpr unsigned kBufferSize = 16 * 1024;
constexpr int kBufferGroup = 0;
constexpr unsigned kCompletionBatch = 64;

enum class Op { Connect, Recv, Send, Cancel };

struct ConnectionState;

// What an in-flight operation's user_data points at, so a completion finds its connection.
struct Tag {
  ConnectionState* connection = nullptr;
  Op op = Op::Connect;
};

}  // namespace

struct Loop::Impl {
  io_uring ring{};
  io_uring_buf_ring* bufferRing = nullptr;
  uint8_t* buffers = nullptr;

  std::unordered_set<ConnectionState*> connections;
  size_t busyConnections = 0;  // open and not idle
  std::vector<ConnectionState*> finalizable;
  bool stopped = false;

  using Clock = std::chrono::steady_clock;
  TimerId nextTimer = 1;
  std::set<std::pair<Clock::time_point, TimerId>> timerOrder;
  struct Timer {
    Clock::time_point when;
    Task task;
    bool background;
  };
  std::unordered_map<TimerId, Timer> timers;
  size_t foregroundTimers = 0;

  bool Pending() const { return busyConnections != 0 || foregroundTimers != 0; }

  io_uring_sqe* NextSqe() {
    io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
      io_uring_submit(&ring);
      sqe = io_uring_get_sqe(&ring);
    }
    return sqe;
  }

  void RecycleBuffer(unsigned id) {
    io_uring_buf_ring_add(bufferRing, buffers + size_t{id} * kBufferSize, kBufferSize, static_cast<unsigned short>(id),
                          io_uring_buf_ring_mask(kBufferCount), 0);
    io_uring_buf_ring_advance(bufferRing, 1);
  }

  void StartConnect(ConnectionState& c);
  void StartRecv(ConnectionState& c);
  void StartSend(ConnectionState& c);
  void Cancel(ConnectionState& c, Tag& target);
  void CloseConnection(ConnectionState& c);
  void QueueFinalize(ConnectionState& c);
  void Handle(const io_uring_cqe& cqe);
  void RunTimers();
  void DrainFinalizable();
};

namespace {

struct ConnectionState final : Connection {
  Loop::Impl* loop = nullptr;
  ConnectionHandler* handler = nullptr;
  int fd = -1;
  sockaddr_storage address{};
  socklen_t addressLength = 0;

  Tag connectTag{this, Op::Connect};
  Tag recvTag{this, Op::Recv};
  Tag sendTag{this, Op::Send};
  Tag cancelTag{this, Op::Cancel};

  // Operations submitted whose final completion has not arrived. Nothing is freed before 0.
  int inflight = 0;
  bool connectActive = false;
  bool recvActive = false;
  bool sendActive = false;
  bool connected = false;
  bool closing = false;
  bool finalizeQueued = false;
  bool idle = false;

  std::deque<std::string> sendQueue;
  size_t sendOffset = 0;
  int error = 0;
};

ConnectionState& State(Connection* connection) { return *static_cast<ConnectionState*>(connection); }

}  // namespace

void Loop::Impl::StartConnect(ConnectionState& c) {
  io_uring_sqe* sqe = NextSqe();
  io_uring_prep_connect(sqe, c.fd, reinterpret_cast<const sockaddr*>(&c.address), c.addressLength);
  io_uring_sqe_set_data(sqe, &c.connectTag);
  c.connectActive = true;
  ++c.inflight;
}

void Loop::Impl::StartRecv(ConnectionState& c) {
  io_uring_sqe* sqe = NextSqe();
  io_uring_prep_recv_multishot(sqe, c.fd, nullptr, 0, 0);
  sqe->flags |= IOSQE_BUFFER_SELECT;
  sqe->buf_group = kBufferGroup;
  io_uring_sqe_set_data(sqe, &c.recvTag);
  c.recvActive = true;
  ++c.inflight;
}

void Loop::Impl::StartSend(ConnectionState& c) {
  const std::string& front = c.sendQueue.front();
  io_uring_sqe* sqe = NextSqe();
  io_uring_prep_send(sqe, c.fd, front.data() + c.sendOffset, front.size() - c.sendOffset, MSG_NOSIGNAL);
  io_uring_sqe_set_data(sqe, &c.sendTag);
  c.sendActive = true;
  ++c.inflight;
}

void Loop::Impl::Cancel(ConnectionState& c, Tag& target) {
  io_uring_sqe* sqe = NextSqe();
  io_uring_prep_cancel64(sqe, reinterpret_cast<uint64_t>(&target), 0);
  io_uring_sqe_set_data(sqe, &c.cancelTag);
  ++c.inflight;
}

void Loop::Impl::QueueFinalize(ConnectionState& c) {
  if (c.finalizeQueued) return;
  c.finalizeQueued = true;
  finalizable.push_back(&c);
}

void Loop::Impl::CloseConnection(ConnectionState& c) {
  if (c.closing) return;
  c.closing = true;
  if (c.fd >= 0) ::shutdown(c.fd, SHUT_RDWR);
  if (c.connectActive) Cancel(c, c.connectTag);
  if (c.recvActive) Cancel(c, c.recvTag);
  if (c.inflight == 0) QueueFinalize(c);
}

void Loop::Impl::Handle(const io_uring_cqe& cqe) {
  Tag* tag = static_cast<Tag*>(io_uring_cqe_get_data(&cqe));
  ConnectionState& c = *tag->connection;
  const int result = cqe.res;
  const unsigned flags = cqe.flags;

  switch (tag->op) {
    case Op::Connect:
      c.connectActive = false;
      --c.inflight;
      if (!c.closing) {
        if (result < 0) {
          c.error = -result;
          CloseConnection(c);
        } else {
          c.connected = true;
          c.handler->OnConnected();
          if (!c.closing) {
            StartRecv(c);
            if (!c.sendQueue.empty() && !c.sendActive) StartSend(c);
          }
        }
      }
      break;

    case Op::Recv: {
      const bool more = flags & IORING_CQE_F_MORE;
      if (!more) {
        c.recvActive = false;
        --c.inflight;
      }
      if (result > 0 && (flags & IORING_CQE_F_BUFFER)) {
        const unsigned id = flags >> IORING_CQE_BUFFER_SHIFT;
        if (!c.closing) {
          c.handler->OnData(std::span<const uint8_t>(buffers + size_t{id} * kBufferSize, static_cast<size_t>(result)));
        }
        RecycleBuffer(id);
      } else if (result == 0) {
        CloseConnection(c);  // the peer closed its side, which is a clean end
      } else if (result < 0 && result != -ENOBUFS && result != -ECANCELED && !c.closing) {
        c.error = -result;
        CloseConnection(c);
      }
      // A multishot receive also stops when the buffer ring runs dry; ask for another.
      if (!more && !c.closing) StartRecv(c);
      break;
    }

    case Op::Send:
      c.sendActive = false;
      --c.inflight;
      if (c.closing) break;
      if (result < 0) {
        c.error = -result;
        CloseConnection(c);
        break;
      }
      c.sendOffset += static_cast<size_t>(result);
      if (c.sendOffset == c.sendQueue.front().size()) {
        c.sendQueue.pop_front();
        c.sendOffset = 0;
      }
      if (!c.sendQueue.empty()) StartSend(c);
      break;

    case Op::Cancel:
      --c.inflight;
      break;
  }

  if (c.closing && c.inflight == 0) QueueFinalize(c);
}

void Loop::Impl::DrainFinalizable() {
  while (!finalizable.empty()) {
    std::vector<ConnectionState*> batch;
    batch.swap(finalizable);
    for (ConnectionState* c : batch) {
      if (c->fd >= 0) ::close(c->fd);
      ConnectionHandler* handler = c->handler;
      const int error = c->error;
      if (!c->idle) --busyConnections;
      connections.erase(c);
      delete c;
      handler->OnClosed(error);
    }
  }
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

Loop::Loop() : impl_(std::make_unique<Impl>()) {}

std::unique_ptr<Loop> Loop::Create() {
  std::unique_ptr<Loop> loop(new Loop());
  Impl& impl = *loop->impl_;
  if (io_uring_queue_init(kRingEntries, &impl.ring, 0) < 0) return nullptr;

  int error = 0;
  impl.bufferRing = io_uring_setup_buf_ring(&impl.ring, kBufferCount, kBufferGroup, 0, &error);
  if (!impl.bufferRing) {
    io_uring_queue_exit(&impl.ring);
    return nullptr;
  }
  impl.buffers = static_cast<uint8_t*>(std::aligned_alloc(4096, size_t{kBufferCount} * kBufferSize));
  for (unsigned id = 0; id < kBufferCount; ++id) {
    io_uring_buf_ring_add(impl.bufferRing, impl.buffers + size_t{id} * kBufferSize, kBufferSize,
                          static_cast<unsigned short>(id), io_uring_buf_ring_mask(kBufferCount), static_cast<int>(id));
  }
  io_uring_buf_ring_advance(impl.bufferRing, kBufferCount);
  return loop;
}

Loop::~Loop() {
  if (!impl_->bufferRing) return;  // Create failed part-way
  for (ConnectionState* c : impl_->connections) {
    if (c->fd >= 0) ::close(c->fd);
    delete c;
  }
  io_uring_free_buf_ring(&impl_->ring, impl_->bufferRing, kBufferCount, kBufferGroup);
  io_uring_queue_exit(&impl_->ring);
  std::free(impl_->buffers);
}

Connection* Loop::Connect(const SocketAddress& address, ConnectionHandler* handler) {
  auto* c = new ConnectionState();
  c->loop = impl_.get();
  c->handler = handler;
  impl_->connections.insert(c);
  ++impl_->busyConnections;

  if (address.family == SocketAddress::Family::IPv6) {
    auto* in6 = reinterpret_cast<sockaddr_in6*>(&c->address);
    in6->sin6_family = AF_INET6;
    in6->sin6_port = htons(address.port);
    std::memcpy(&in6->sin6_addr, address.bytes.data(), 16);
    c->addressLength = sizeof(sockaddr_in6);
  } else {
    auto* in4 = reinterpret_cast<sockaddr_in*>(&c->address);
    in4->sin_family = AF_INET;
    in4->sin_port = htons(address.port);
    std::memcpy(&in4->sin_addr, address.bytes.data(), 4);
    c->addressLength = sizeof(sockaddr_in);
  }

  c->fd = ::socket(c->address.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (c->fd < 0) {
    c->error = errno;
    impl_->CloseConnection(*c);  // reported from Run, like every other failure
    return c;
  }
  int one = 1;
  ::setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  impl_->StartConnect(*c);
  return c;
}

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
  Impl& impl = *impl_;
  impl.stopped = false;

  while (!impl.stopped) {
    impl.RunTimers();
    impl.DrainFinalizable();
    if (!impl.Pending() && impl.finalizable.empty()) break;
    if (impl.stopped) break;

    io_uring_submit(&impl.ring);

    io_uring_cqe* first = nullptr;
    int waited;
    if (impl.timerOrder.empty()) {
      waited = io_uring_wait_cqe(&impl.ring, &first);
    } else {
      auto remaining = impl.timerOrder.begin()->first - Impl::Clock::now();
      if (remaining < Impl::Clock::duration::zero()) remaining = Impl::Clock::duration::zero();
      const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count();
      __kernel_timespec timeout{ns / 1000000000, ns % 1000000000};
      waited = io_uring_wait_cqe_timeout(&impl.ring, &first, &timeout);
    }
    if (waited == -ETIME || waited == -EINTR) continue;
    if (waited < 0) break;

    io_uring_cqe* batch[kCompletionBatch];
    const unsigned count = io_uring_peek_batch_cqe(&impl.ring, batch, kCompletionBatch);
    for (unsigned i = 0; i < count; ++i) impl.Handle(*batch[i]);
    io_uring_cq_advance(&impl.ring, count);
  }
}

void Connection::Send(std::string data) {
  ConnectionState& c = State(this);
  if (c.closing || data.empty()) return;
  c.sendQueue.push_back(std::move(data));
  if (c.connected && !c.sendActive) c.loop->StartSend(c);
}

void Connection::Close() { State(this).loop->CloseConnection(State(this)); }

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
