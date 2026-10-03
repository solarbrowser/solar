#include "Poller.h"

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)

#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>

#include <algorithm>
#include <unordered_map>

namespace solar::net::internal {

namespace {

class KqueuePoller final : public Poller {
 public:
  ~KqueuePoller() override {
    if (queue_ >= 0) ::close(queue_);
  }

  const char* Name() const override { return "kqueue"; }

  bool Initialize() {
    queue_ = ::kqueue();
    return queue_ >= 0;
  }

  // kqueue watches each direction as a filter of its own, so the change from the interest a
  // socket had to the one it is to have is added and deleted filter by filter.
  bool Set(SocketHandle socket, uint64_t id, unsigned interest) override {
    const unsigned had = interest_.count(socket) ? interest_[socket] : 0u;
    struct kevent changes[2];
    int count = 0;
    const struct {
      unsigned bit;
      int filter;
    } filters[] = {{kRead, EVFILT_READ}, {kWrite, EVFILT_WRITE}};
    for (const auto& f : filters) {
      const bool want = interest & f.bit;
      const bool was = had & f.bit;
      if (want && !was) {
        EV_SET(&changes[count++], static_cast<uintptr_t>(socket), f.filter, EV_ADD | EV_ENABLE, 0, 0, reinterpret_cast<void*>(static_cast<uintptr_t>(id)));
      } else if (!want && was) {
        EV_SET(&changes[count++], static_cast<uintptr_t>(socket), f.filter, EV_DELETE, 0, 0, nullptr);
      }
    }
    interest_[socket] = interest;
    if (count == 0) return true;
    return ::kevent(queue_, changes, count, nullptr, 0, nullptr) == 0;
  }

  void Remove(SocketHandle socket) override {
    auto it = interest_.find(socket);
    if (it == interest_.end()) return;
    struct kevent changes[2];
    int count = 0;
    if (it->second & kRead) EV_SET(&changes[count++], static_cast<uintptr_t>(socket), EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    if (it->second & kWrite) EV_SET(&changes[count++], static_cast<uintptr_t>(socket), EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    interest_.erase(it);
    if (count != 0) ::kevent(queue_, changes, count, nullptr, 0, nullptr);
  }

  int Wait(int timeoutMs, Event* events, int maxEvents) override {
    struct kevent ready[128];
    timespec timeout{timeoutMs / 1000, (timeoutMs % 1000) * 1000000L};
    const int count = ::kevent(queue_, nullptr, 0, ready, std::min(maxEvents, 128), timeoutMs < 0 ? nullptr : &timeout);
    if (count < 0) return errno == EINTR ? 0 : -1;
    for (int i = 0; i < count; ++i) {
      const bool failed = ready[i].flags & EV_ERROR;
      const uint64_t id = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ready[i].udata));
      // End of file is seen by reading, which returns 0.
      const bool readable = ready[i].filter == EVFILT_READ || failed;
      const bool writable = ready[i].filter == EVFILT_WRITE || failed;
      events[i] = {id, readable, writable, failed};
    }
    return count;
  }

 private:
  int queue_ = -1;
  std::unordered_map<SocketHandle, unsigned> interest_;
};

}  // namespace

std::unique_ptr<Poller> MakePoller() {
  auto poller = std::make_unique<KqueuePoller>();
  if (!poller->Initialize()) return nullptr;
  return poller;
}

}  // namespace solar::net::internal

#endif
