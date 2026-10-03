#include "Poller.h"

#ifdef __linux__

#include <sys/epoll.h>

#include <unordered_set>

namespace solar::net::internal {

namespace {

class EpollPoller final : public Poller {
 public:
  ~EpollPoller() override {
    if (epoll_ >= 0) ::close(epoll_);
  }

  const char* Name() const override { return "epoll"; }

  bool Initialize() {
    epoll_ = ::epoll_create1(EPOLL_CLOEXEC);
    return epoll_ >= 0;
  }

  bool Set(SocketHandle socket, uint64_t id, unsigned interest) override {
    epoll_event event{};
    event.events = ((interest & kRead) ? EPOLLIN : 0u) | ((interest & kWrite) ? EPOLLOUT : 0u);
    event.data.u64 = id;
    const bool known = registered_.count(socket) != 0;
    if (::epoll_ctl(epoll_, known ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, socket, &event) != 0) return false;
    registered_.insert(socket);
    return true;
  }

  void Remove(SocketHandle socket) override {
    if (registered_.erase(socket) != 0) ::epoll_ctl(epoll_, EPOLL_CTL_DEL, socket, nullptr);
  }

  int Wait(int timeoutMs, Event* events, int maxEvents) override {
    epoll_event ready[128];
    const int count = ::epoll_wait(epoll_, ready, std::min(maxEvents, 128), timeoutMs);
    if (count < 0) return errno == EINTR ? 0 : -1;
    for (int i = 0; i < count; ++i) {
      const uint32_t bits = ready[i].events;
      const bool failed = bits & (EPOLLERR | EPOLLHUP);
      // An error or hang-up is something both a read and a write will find out about.
      events[i] = {ready[i].data.u64, (bits & EPOLLIN) || failed, (bits & EPOLLOUT) || failed, failed};
    }
    return count;
  }

 private:
  int epoll_ = -1;
  std::unordered_set<SocketHandle> registered_;
};

}  // namespace

std::unique_ptr<Poller> MakePoller() {
  auto poller = std::make_unique<EpollPoller>();
  if (!poller->Initialize()) return nullptr;
  return poller;
}

}  // namespace solar::net::internal

#endif
