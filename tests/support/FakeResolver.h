#pragma once

#include <map>
#include <string>
#include <utility>

#include "solar/net/Resolver.h"

namespace solar::test {

// A resolver whose answers, and how long they take, a test decides. Names it was not told about
// have no address of either family.
class FakeResolver : public solar::net::Resolver {
 public:
  struct Answer {
    solar::net::ResolveResult result;
    std::chrono::milliseconds delay{0};
  };

  explicit FakeResolver(solar::net::Loop& loop) : loop_(loop) {}

  void Set(const std::string& host, solar::net::SocketAddress::Family family, Answer answer) { answers_[{host, family}] = std::move(answer); }

  RequestId Resolve(const std::string& host, uint16_t port, solar::net::SocketAddress::Family family, Callback callback) override {
    ++lookups;
    Answer answer;
    if (auto it = answers_.find({host, family}); it != answers_.end()) answer = it->second;
    for (auto& address : answer.result.addresses) address.port = port;
    const RequestId id = ++nextId_;
    pending_[id] = loop_.PostDelayed(answer.delay, [this, id, result = std::move(answer.result), callback = std::move(callback)]() mutable {
      pending_.erase(id);
      callback(std::move(result));
    });
    return id;
  }

  void Cancel(RequestId id) override {
    auto it = pending_.find(id);
    if (it == pending_.end()) return;
    loop_.CancelTimer(it->second);
    pending_.erase(it);
    ++cancelled;
  }

  int lookups = 0;
  int cancelled = 0;

 private:
  solar::net::Loop& loop_;
  std::map<std::pair<std::string, solar::net::SocketAddress::Family>, Answer> answers_;
  std::map<RequestId, solar::net::Loop::TimerId> pending_;
  RequestId nextId_ = 0;
};

}  // namespace solar::test
