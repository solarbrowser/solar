#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>

#include "quanta/Embed.h"

// The event loop's queue of tasks (https://html.spec.whatwg.org/#timers): what setTimeout, setInterval, setImmediate and
// Realm::EnqueueTask ask for, kept in the order they are due and run when the program's loop says so. The engine's
// own loop is not used: the delays are clamped as the standard has them (a timer nested five deep waits at least four
// milliseconds), and the program can see, run and hold back the tasks.
namespace solar::web {

class TimerHost {
 public:
  TimerHost() = default;
  TimerHost(const TimerHost&) = delete;
  TimerHost& operator=(const TimerHost&) = delete;
  ~TimerHost() { queue_.clear(); }

  // What to give an Isolate with SetTimerProvider. The host must outlive the Isolate's use of it.
  Quanta::Embed::TimerProvider Provider();

  // Runs the tasks that are due, in the order of their time, and the ones those make due; true if any ran.
  bool RunDue();
  // Milliseconds until the next task is due (0 if one already is); nothing when there is none.
  std::optional<int64_t> NextDelayMs() const;
  // How many tasks wait.
  size_t Size() const { return queue_.size(); }

 private:
  using Clock = std::chrono::steady_clock;
  void Schedule(Quanta::Embed::Task task);

  std::multimap<Clock::time_point, Quanta::Embed::Task> queue_;
};

}  // namespace solar::web
