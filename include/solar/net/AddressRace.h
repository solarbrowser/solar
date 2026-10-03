#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <string>

#include "solar/net/Loop.h"
#include "solar/net/Resolver.h"

namespace solar::net::internal {

// Connecting to a name that has several addresses, as RFC 8305 (Happy Eyeballs) describes, as a
// state machine that knows nothing of sockets or clocks. The owner tells it what happened, and
// the Delegate is how it makes things happen.
//
// IPv6 is preferred, but only for 50 ms: if the IPv4 answer is in and the IPv6 one is not, that is
// all it waits. Attempts are started one after another with 250 ms between them rather than all
// at once, alternating between the two families, and an attempt that fails does not make the next
// wait. The first to connect wins and the rest are cancelled.
class AddressRace {
 public:
  using Family = SocketAddress::Family;
  using AttemptId = uint64_t;
  enum class Timer { ResolutionDelay, NextAttempt };

  class Delegate {
   public:
    virtual ~Delegate() = default;
    virtual AttemptId StartAttempt(const SocketAddress& address) = 0;
    virtual void CancelAttempt(AttemptId id) = 0;
    // Starts a timer of this kind, replacing one that is running.
    virtual void StartTimer(Timer which, std::chrono::milliseconds delay) = 0;
    virtual void StopTimer(Timer which) = 0;
    // `id` connected first. Every other attempt has already been cancelled.
    virtual void Succeeded(AttemptId id) = 0;
    // Nothing could be reached.
    virtual void Failed(const std::string& message) = 0;
  };

  struct Config {
    std::chrono::milliseconds resolutionDelay{50};
    std::chrono::milliseconds attemptDelay{250};
  };

  explicit AddressRace(Delegate& delegate) : delegate_(delegate) {}
  AddressRace(Delegate& delegate, Config config) : delegate_(delegate), config_(config) {}

  // The answer of the lookup for `family`. Both lookups must be answered, even if with nothing.
  void Answered(Family family, ResolveResult result);
  void AttemptSucceeded(AttemptId id);
  void AttemptFailed(AttemptId id, const std::string& error);
  void TimerFired(Timer which);
  // Gives up: cancels what is running and says nothing more.
  void Abandon();

  bool finished() const { return finished_; }

 private:
  struct Side {
    bool answered = false;
    std::deque<SocketAddress> queue;  // addresses not yet tried
    std::string error;
  };

  Side& Of(Family family) { return family == Family::IPv6 ? v6_ : v4_; }
  bool HaveAddresses() const { return !v6_.queue.empty() || !v4_.queue.empty(); }
  bool AllAnswered() const { return v6_.answered && v4_.answered; }
  void Advance();
  void StartNext();
  void Stop(Timer which);
  void Finish();

  Delegate& delegate_;
  Config config_;
  Side v6_;
  Side v4_;

  std::map<AttemptId, bool> attempts_;  // those started and not yet finished
  bool started_ = false;                // some attempt has been started
  bool haveLast_ = false;
  Family last_ = Family::IPv6;          // the family of the most recent attempt
  bool resolutionTimerRunning_ = false;
  bool resolutionDelayPassed_ = false;
  bool attemptTimerRunning_ = false;
  bool finished_ = false;
  std::string lastAttemptError_;
};

}  // namespace solar::net::internal
