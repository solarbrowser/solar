#pragma once

#include <cstdint>
#include <memory>

#include "solar/net/Socket.h"

namespace solar::net::internal {

// Waiting for sockets to become ready, which is what epoll and kqueue do and what the readiness
// backend is built on.
class Poller {
 public:
  static constexpr unsigned kRead = 1;
  static constexpr unsigned kWrite = 2;

  struct Event {
    uint64_t id;
    bool readable;
    bool writable;
    bool failed;
  };

  virtual ~Poller() = default;

  virtual const char* Name() const = 0;

  // Watches `socket` for `interest` (kRead, kWrite, both, or 0 for neither), replacing what it was
  // watched for. `id` is what comes back with its events.
  virtual bool Set(SocketHandle socket, uint64_t id, unsigned interest) = 0;
  virtual void Remove(SocketHandle socket) = 0;
  // Waits up to `timeoutMs` (-1: for as long as it takes). The number of events, or -1 on failure.
  virtual int Wait(int timeoutMs, Event* events, int maxEvents) = 0;
};

// The platform's poller; null where there is none or it cannot start.
std::unique_ptr<Poller> MakePoller();

}  // namespace solar::net::internal
