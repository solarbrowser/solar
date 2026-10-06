#include "solar/web/TimerHost.h"

#include <algorithm>
#include <vector>

namespace solar::web {

namespace qe = Quanta::Embed;

qe::TimerProvider TimerHost::Provider() {
  qe::TimerProvider provider;
  provider.schedule = [this](qe::Task task) { Schedule(std::move(task)); };
  provider.cancel = [this](qe::Realm*, int64_t id) {
    // A cancelled task is dropped when it comes up, which is when its handle is let go; the ones that are there for good
    // are those a realm cancelled, and the queue does not keep them long.
    for (auto it = queue_.begin(); it != queue_.end();) {
      if (it->second.Id() == id && it->second.IsCancelled()) it = queue_.erase(it);
      else ++it;
    }
  };
  return provider;
}

void TimerHost::Schedule(qe::Task task) {
  double delay = std::max(0.0, task.DelayMs());
  // A timer nested five deep, or a repeating one that has run as often, waits at least four milliseconds.
  if (task.NestingLevel() >= 5) delay = std::max(delay, 4.0);
  queue_.emplace(Clock::now() + std::chrono::microseconds(static_cast<int64_t>(delay * 1000)), std::move(task));
}

bool TimerHost::RunDue() {
  bool ran = false;
  const Clock::time_point now = Clock::now();
  // What is due when this starts, in order; the tasks it runs may add more, which wait for the next turn if they are
  // not due yet.
  std::vector<qe::Task> due;
  for (auto it = queue_.begin(); it != queue_.end() && it->first <= now;) {
    due.push_back(std::move(it->second));
    it = queue_.erase(it);
  }
  for (qe::Task& task : due) {
    if (task.IsCancelled()) continue;
    task.Run();
    ran = true;
    // A repeating task stays armed, and is scheduled for its next run.
    if (task.IsRepeating() && !task.IsCancelled()) Schedule(task);
  }
  return ran;
}

std::optional<int64_t> TimerHost::NextDelayMs() const {
  // Cancelled tasks are not waited for.
  for (const auto& [due, task] : queue_) {
    if (task.IsCancelled()) continue;
    const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(due - Clock::now()).count();
    return std::max<int64_t>(0, wait);
  }
  return std::nullopt;
}

}  // namespace solar::web
