#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "solar/net/Loop.h"

namespace solar::net {

struct ResolveResult {
  std::vector<SocketAddress> addresses;
  // Set when the lookup failed. Empty addresses with no error means the name has none of the
  // family that was asked for, which is not a failure: many names have no IPv6 address.
  std::string error;
};

// Turns a host name into addresses without making the loop wait for it. Everything here is for
// the loop's own thread.
class Resolver {
 public:
  using Callback = std::function<void(ResolveResult)>;
  using RequestId = uint64_t;

  virtual ~Resolver() = default;

  // Looks up `host`'s addresses of one family, with `port` filled in. The callback runs on the
  // loop's thread, from Loop::Run, never from inside this call.
  virtual RequestId Resolve(const std::string& host, uint16_t port, SocketAddress::Family family, Callback callback) = 0;
  // After this the callback is not called, and Run no longer waits for the lookup. Does nothing
  // for a request that has finished.
  virtual void Cancel(RequestId id) = 0;
};

struct SystemResolverOptions {
  size_t threads = 4;
  // How long an answer is reused. The system's resolver does not say how long a record is good
  // for, so this is a fixed time.
  std::chrono::milliseconds cacheTtl{60000};
  // How a name is looked up, on one of the threads. The system's resolver if empty; a test puts a
  // slow or scripted one here.
  std::function<ResolveResult(const std::string& host, uint16_t port, SocketAddress::Family family)> lookup;
};

// Looks names up with the system's resolver on a few threads of its own, answers address literals
// without any, and keeps answers for a while. Destroy it from the loop's thread before the loop;
// that waits for lookups already running, since a resolver call cannot be interrupted.
std::unique_ptr<Resolver> MakeSystemResolver(Loop& loop, SystemResolverOptions options = {});

}  // namespace solar::net
