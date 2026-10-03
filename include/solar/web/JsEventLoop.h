#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <thread>

#include "solar/net/Loop.h"

namespace solar::web {

// Runs the network's event loop and a script engine's promise jobs and timers as one. The engine's
// timers are its own and the network's loop is another; this is what makes a timer fire while a
// fetch is waiting, and a fetch finish while a timer is pending.
class JsEventLoop {
 public:
  struct Hooks {
    // Runs every queued promise job: an Isolate's or Runtime's PerformMicrotaskCheckpoint.
    std::function<void()> checkpoint;
    // Fires the timers that are due, without waiting for any that are not.
    std::function<void()> runDueTimers;
    // Milliseconds until the next timer is due; none when there is no timer.
    std::function<std::optional<int64_t>()> nextTimerDelayMs;
  };

  JsEventLoop(net::Loop& loop, Hooks hooks) : loop_(loop), hooks_(std::move(hooks)) {}
  ~JsEventLoop() {
    if (armed_) loop_.CancelTimer(armed_);
  }

  JsEventLoop(const JsEventLoop&) = delete;
  JsEventLoop& operator=(const JsEventLoop&) = delete;

  // For FetchHost::Config::afterScript: the promise jobs script has queued, and the timers it set.
  void AfterScript() {
    hooks_.checkpoint();
    Arm();
  }

  // Runs until the network is quiet and no timer is pending, or `limit` has passed.
  void Run(std::chrono::milliseconds limit = std::chrono::hours(24)) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
      Arm();
      loop_.Run();  // the network's turn, until nothing is in flight
      hooks_.checkpoint();
      const std::optional<int64_t> delay = hooks_.nextTimerDelayMs();
      if (!delay) break;
      // Nothing is in flight but a timer is pending: wait for it, and run it.
      if (*delay > 0) std::this_thread::sleep_for(std::min(std::chrono::milliseconds(*delay), std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())));
      hooks_.runDueTimers();
      hooks_.checkpoint();
    }
  }

 private:
  // Has the network's loop wake for the next timer, so that one fires on time while it is busy. The
  // timer is a background one: it does not keep the loop running by itself.
  void Arm() {
    if (armed_) loop_.CancelTimer(armed_);
    armed_ = 0;
    const std::optional<int64_t> delay = hooks_.nextTimerDelayMs();
    if (!delay) return;
    armed_ = loop_.PostDelayed(std::chrono::milliseconds(*delay), [this] {
      armed_ = 0;
      hooks_.runDueTimers();
      hooks_.checkpoint();
      Arm();
    }, /*background=*/true);
  }

  net::Loop& loop_;
  Hooks hooks_;
  net::Loop::TimerId armed_ = 0;
};

}  // namespace solar::web
