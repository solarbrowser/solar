#include "solar/net/AddressRace.h"

namespace solar::net::internal {

void AddressRace::Answered(Family family, ResolveResult result) {
  if (finished_) return;
  Side& side = Of(family);
  side.answered = true;
  side.error = std::move(result.error);
  for (const SocketAddress& address : result.addresses) side.queue.push_back(address);

  // The IPv6 answer arriving is what the wait was for, whatever it holds.
  if (family == Family::IPv6) Stop(Timer::ResolutionDelay);
  Advance();
}

void AddressRace::AttemptSucceeded(AttemptId id) {
  if (finished_ || attempts_.count(id) == 0) return;
  attempts_.erase(id);
  Finish();
  for (const auto& [other, active] : attempts_) delegate_.CancelAttempt(other);
  attempts_.clear();
  delegate_.Succeeded(id);
}

void AddressRace::AttemptFailed(AttemptId id, const std::string& error) {
  if (finished_ || attempts_.erase(id) == 0) return;
  lastAttemptError_ = error;
  // The next attempt is not made to wait out the delay after one that is already over.
  Stop(Timer::NextAttempt);
  Advance();
}

void AddressRace::TimerFired(Timer which) {
  if (finished_) return;
  if (which == Timer::ResolutionDelay) {
    resolutionTimerRunning_ = false;
    resolutionDelayPassed_ = true;
  } else {
    attemptTimerRunning_ = false;
  }
  Advance();
}

void AddressRace::Abandon() {
  if (finished_) return;
  Finish();
  for (const auto& [id, active] : attempts_) delegate_.CancelAttempt(id);
  attempts_.clear();
}

void AddressRace::Advance() {
  if (finished_) return;

  if (!HaveAddresses()) {
    if (attempts_.empty() && AllAnswered()) {
      Finish();
      // What the attempts said is more to the point than that a lookup had nothing, if any were made.
      if (!lastAttemptError_.empty()) {
        delegate_.Failed("connection failed: " + lastAttemptError_);
      } else {
        delegate_.Failed(!v4_.error.empty() ? v4_.error : (!v6_.error.empty() ? v6_.error : "no address to connect to"));
      }
    }
    return;
  }

  if (!started_) {
    // Only IPv4 is known and IPv6 might still come: give it a moment, once.
    if (!v6_.answered && !resolutionDelayPassed_) {
      if (!resolutionTimerRunning_) {
        resolutionTimerRunning_ = true;
        delegate_.StartTimer(Timer::ResolutionDelay, config_.resolutionDelay);
      }
      return;
    }
  } else if (attemptTimerRunning_) {
    return;  // the one before has not had its time
  }
  StartNext();
}

void AddressRace::StartNext() {
  // IPv6 first, and then the families take turns.
  Family family;
  if (!haveLast_ || last_ == Family::IPv4) {
    family = !v6_.queue.empty() ? Family::IPv6 : Family::IPv4;
  } else {
    family = !v4_.queue.empty() ? Family::IPv4 : Family::IPv6;
  }
  Side& side = Of(family);
  const SocketAddress address = side.queue.front();
  side.queue.pop_front();

  started_ = true;
  haveLast_ = true;
  last_ = family;
  Stop(Timer::ResolutionDelay);

  const AttemptId id = delegate_.StartAttempt(address);
  attempts_[id] = true;

  // Another may be wanted if there is one in hand or still to come.
  if (HaveAddresses() || !AllAnswered()) {
    attemptTimerRunning_ = true;
    delegate_.StartTimer(Timer::NextAttempt, config_.attemptDelay);
  }
}

void AddressRace::Stop(Timer which) {
  bool& running = which == Timer::ResolutionDelay ? resolutionTimerRunning_ : attemptTimerRunning_;
  if (!running) return;
  running = false;
  delegate_.StopTimer(which);
}

void AddressRace::Finish() {
  finished_ = true;
  Stop(Timer::ResolutionDelay);
  Stop(Timer::NextAttempt);
}

}  // namespace solar::net::internal
