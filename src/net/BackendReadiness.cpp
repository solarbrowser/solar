#include "LoopCore.h"
#include "Poller.h"

#ifndef _WIN32

#include <cstring>

namespace solar::net::internal {

#if !defined(__linux__) && !defined(__APPLE__) && !defined(__FreeBSD__) && !defined(__NetBSD__) && !defined(__OpenBSD__) && !defined(__DragonFly__)
std::unique_ptr<Poller> MakePoller() { return nullptr; }
#endif

namespace {

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;  // the socket option SO_NOSIGPIPE does it instead
#endif

constexpr size_t kReceiveBufferSize = 64 * 1024;

struct Data {
  bool connecting = false;
  unsigned interest = 0;
};

Data& Of(ConnectionState& c) { return *static_cast<Data*>(c.backendData); }

// A backend that waits until a socket is ready and then reads or writes it, as epoll and kqueue
// allow. The bytes land in one buffer shared by every connection, so a connection needs none of
// its own and the handler is given a view of it.
class ReadinessBackend final : public Backend {
 public:
  ReadinessBackend(Loop::Impl& core, std::unique_ptr<Poller> poller)
      : core_(core), poller_(std::move(poller)), buffer_(kReceiveBufferSize) {}

  ~ReadinessBackend() override {
    if (wake_[0] >= 0) {
      poller_->Remove(wake_[0]);
      ::close(wake_[0]);
      ::close(wake_[1]);
    }
  }

  bool Initialize() {
    if (!MakeWakePipe(wake_)) return false;
    return poller_->Set(wake_[0], kWakeId, Poller::kRead);
  }

  void Wake() override {
    const char byte = 1;
    // A full pipe already has a wake-up in it, so a failed write loses nothing.
    [[maybe_unused]] const ssize_t written = ::write(wake_[1], &byte, 1);
  }

  const char* Name() const override { return poller_->Name(); }

  void Connect(ConnectionState& c) override {
    c.backendData = new Data();
    sockaddr_storage address;
    const int length = ToSockaddr(c.address, address);

    c.socket = ::socket(address.ss_family, SOCK_STREAM, 0);
    if (c.socket < 0) {
      immediate_.emplace_back(c.id, LastSocketError());
      return;
    }
    SetNonBlocking(c.socket);
    ::fcntl(c.socket, F_SETFD, FD_CLOEXEC);
    int one = 1;
    ::setsockopt(c.socket, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    ::setsockopt(c.socket, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    if (::connect(c.socket, reinterpret_cast<const sockaddr*>(&address), static_cast<socklen_t>(length)) == 0) {
      immediate_.emplace_back(c.id, 0);  // reported from Wait, like every other result
      return;
    }
    const int error = LastSocketError();
    if (error == EINPROGRESS || error == EINTR) {
      Of(c).connecting = true;
      Watch(c, Poller::kWrite);
    } else {
      immediate_.emplace_back(c.id, error);
    }
  }

  void StartReceive(ConnectionState& c) override { Watch(c, Of(c).interest | Poller::kRead); }

  void WantSend(ConnectionState& c) override { TrySend(c); }

  void Close(ConnectionState& c) override {
    if (c.socket != kInvalidSocket) {
      ::shutdown(c.socket, SHUT_RDWR);
      poller_->Remove(c.socket);
      Of(c).interest = 0;
    }
    core_.Quiescent(c);  // nothing is in flight: the next ready event, if any, is ignored
  }

  void Release(ConnectionState& c) override {
    if (c.socket != kInvalidSocket) CloseSocket(c.socket);
    delete static_cast<Data*>(c.backendData);
    c.backendData = nullptr;
  }

  bool Wait(int timeoutMs) override {
    if (!immediate_.empty()) {
      std::vector<std::pair<uint64_t, int>> batch;
      batch.swap(immediate_);
      for (const auto& [id, error] : batch) {
        auto it = core_.connections.find(id);
        if (it != core_.connections.end()) core_.Connected(*it->second, error);
      }
      timeoutMs = 0;
    }

    Poller::Event events[128];
    const int count = poller_->Wait(timeoutMs, events, 128);
    if (count < 0) return false;
    for (int i = 0; i < count; ++i) Handle(events[i]);
    return true;
  }

 private:
  void Watch(ConnectionState& c, unsigned interest) {
    Of(c).interest = interest;
    poller_->Set(c.socket, c.id, interest);
  }

  void Handle(const Poller::Event& event) {
    if (event.id == kWakeId) {
      char drained[64];
      while (::read(wake_[0], drained, sizeof(drained)) > 0) {
      }
      return;
    }
    // A connection may be gone by now, finalized by an earlier event of this same batch.
    auto it = core_.connections.find(event.id);
    if (it == core_.connections.end()) return;
    ConnectionState& c = *it->second;
    if (c.closing) return;
    Data& d = Of(c);

    if (d.connecting) {
      if (!event.writable) return;
      int error = 0;
      socklen_t length = sizeof(error);
      ::getsockopt(c.socket, SOL_SOCKET, SO_ERROR, &error, &length);
      d.connecting = false;
      Watch(c, 0);
      core_.Connected(c, error);
      return;
    }
    if (event.readable && (d.interest & Poller::kRead)) Receive(c);
    if (!c.closing && event.writable && (d.interest & Poller::kWrite)) TrySend(c);
  }

  void Receive(ConnectionState& c) {
    const ssize_t count = ::recv(c.socket, reinterpret_cast<char*>(buffer_.data()), buffer_.size(), 0);
    if (count > 0) {
      core_.Received(c, std::span<const uint8_t>(buffer_.data(), static_cast<size_t>(count)));
    } else if (count == 0) {
      core_.PeerClosed(c);
    } else {
      const int error = LastSocketError();
      if (error != EAGAIN && error != EWOULDBLOCK && error != EINTR) core_.ReceiveFailed(c, error);
    }
  }

  // Writes what the socket will take now. If it takes less than there is, writability is watched
  // and the rest goes when the socket can take more.
  void TrySend(ConnectionState& c) {
    Data& d = Of(c);
    while (!c.closing) {
      const std::string_view pending = core_.PendingSend(c);
      if (pending.empty()) {
        if (d.interest & Poller::kWrite) Watch(c, d.interest & ~Poller::kWrite);
        return;
      }
      const ssize_t count = ::send(c.socket, pending.data(), pending.size(), kSendFlags);
      if (count > 0) {
        core_.Sent(c, static_cast<size_t>(count));
        continue;
      }
      const int error = count < 0 ? LastSocketError() : EPIPE;
      if (error == EINTR) continue;
      if (error == EAGAIN || error == EWOULDBLOCK) {
        if (!(d.interest & Poller::kWrite)) Watch(c, d.interest | Poller::kWrite);
        return;
      }
      core_.SendFailed(c, error);
      return;
    }
  }

  // No connection has this id, which the loop counts up from 1.
  static constexpr uint64_t kWakeId = 0;

  Loop::Impl& core_;
  std::unique_ptr<Poller> poller_;
  std::vector<uint8_t> buffer_;
  int wake_[2] = {-1, -1};
  // Connects that finished or failed in the call that began them; Wait reports them.
  std::vector<std::pair<uint64_t, int>> immediate_;
};

}  // namespace

std::unique_ptr<Backend> MakeReadinessBackend(Loop::Impl& core) {
  std::unique_ptr<Poller> poller = MakePoller();
  if (!poller) return nullptr;
  auto backend = std::make_unique<ReadinessBackend>(core, std::move(poller));
  if (!backend->Initialize()) return nullptr;
  return backend;
}

}  // namespace solar::net::internal

#else

namespace solar::net::internal {
std::unique_ptr<Backend> MakeReadinessBackend(Loop::Impl&) { return nullptr; }
}  // namespace solar::net::internal

#endif
