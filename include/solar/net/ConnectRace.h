#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "solar/net/AddressRace.h"
#include "solar/net/Loop.h"
#include "solar/net/Resolver.h"

namespace solar::net::internal {

// Looks a name up and connects to it, racing its addresses as AddressRace says. This is AddressRace
// put to work: the lookups are the Resolver's, the attempts are real connections, and the timers
// are the loop's. The first connection to be made is handed to `connected`, which then owns it.
//
// It can be destroyed at any time, even with lookups, attempts and timers out; whatever comes back
// afterwards finds it gone and does nothing.
class ConnectRace final : public AddressRace::Delegate {
 public:
  using OnConnected = std::function<void(Connection* winner)>;
  using OnFailed = std::function<void(const std::string& message)>;

  ConnectRace(Loop& loop, Resolver& resolver, std::string host, uint16_t port, AddressRace::Config config, OnConnected connected, OnFailed failed);
  ~ConnectRace() override;

  ConnectRace(const ConnectRace&) = delete;
  ConnectRace& operator=(const ConnectRace&) = delete;

  // Begins both lookups. Nothing is reported from inside this call.
  void Start();

 private:
  struct Attempt;

  // What the things that can outlive the race look at. When the race is gone `race` is null.
  struct Shared {
    ConnectRace* race;
  };

  AddressRace::AttemptId StartAttempt(const SocketAddress& address) override;
  void CancelAttempt(AddressRace::AttemptId id) override;
  void StartTimer(AddressRace::Timer which, std::chrono::milliseconds delay) override;
  void StopTimer(AddressRace::Timer which) override;
  void Succeeded(AddressRace::AttemptId id) override;
  void Failed(const std::string& message) override;

  Loop& loop_;
  Resolver& resolver_;
  std::string host_;
  uint16_t port_;
  OnConnected connected_;
  OnFailed failed_;

  std::shared_ptr<Shared> shared_;
  AddressRace race_;
  AddressRace::AttemptId nextAttempt_ = 0;
  std::map<AddressRace::AttemptId, Attempt*> attempts_;
  Loop::TimerId timers_[2] = {0, 0};
  Resolver::RequestId lookups_[2] = {0, 0};  // IPv6, IPv4; 0 once answered
};

}  // namespace solar::net::internal
