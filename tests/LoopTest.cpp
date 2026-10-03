#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "support/TestLoop.h"

namespace {

using namespace std::chrono_literals;

int total = 0;
int failed = 0;

void Check(const std::string& name, bool ok, const std::string& detail = "") {
  ++total;
  if (ok) return;
  ++failed;
  std::printf("FAIL %s %s\n", name.c_str(), detail.c_str());
}

double Seconds(std::chrono::steady_clock::time_point since) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

}  // namespace

int main() {
  {
    // A task posted from another thread runs on the loop's thread, and wakes a loop that is
    // waiting with nothing else to do.
    auto loop = solar::test::MakeLoop();
    const std::thread::id loopThread = std::this_thread::get_id();
    std::thread::id ranOn;
    bool ran = false;
    loop->BeginExternalWork();
    loop->PostDelayed(30s, [] {}, /*background=*/true);  // a long timeout the wake-up must not wait for
    std::thread other([&] {
      std::this_thread::sleep_for(60ms);
      loop->Post([&] {
        ran = true;
        ranOn = std::this_thread::get_id();
        loop->EndExternalWork();
      });
    });
    const auto start = std::chrono::steady_clock::now();
    loop->Run();
    other.join();
    Check("a posted task runs", ran);
    Check("on the loop's thread", ranOn == loopThread);
    Check("and wakes a waiting loop promptly", Seconds(start) < 2.0, std::to_string(Seconds(start)) + " s");
  }
  {
    // Work that has been begun keeps Run from returning until it is ended.
    auto loop = solar::test::MakeLoop();
    bool ended = false;
    loop->BeginExternalWork();
    std::thread other([&] {
      std::this_thread::sleep_for(100ms);
      loop->Post([&] {
        ended = true;
        loop->EndExternalWork();
      });
    });
    loop->Run();
    other.join();
    Check("Run waits for external work", ended);
  }
  {
    // Run returns at once when there is nothing to wait for.
    auto loop = solar::test::MakeLoop();
    const auto start = std::chrono::steady_clock::now();
    loop->Run();
    Check("Run returns with nothing pending", Seconds(start) < 0.5);
  }
  {
    // Many threads, many tasks: none lost, each thread's in the order it posted them.
    auto loop = solar::test::MakeLoop();
    constexpr int kThreads = 8;
    constexpr int kEach = 2000;
    std::vector<std::vector<int>> seen(kThreads);
    std::atomic<int> remaining{kThreads * kEach};
    loop->BeginExternalWork();
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&, t] {
        for (int i = 0; i < kEach; ++i) {
          loop->Post([&, t, i] {
            seen[t].push_back(i);
            if (--remaining == 0) loop->EndExternalWork();
          });
        }
      });
    }
    loop->Run();
    for (std::thread& thread : threads) thread.join();
    bool inOrder = true;
    size_t count = 0;
    for (const auto& list : seen) {
      count += list.size();
      for (size_t i = 0; i < list.size(); ++i) inOrder = inOrder && list[i] == static_cast<int>(i);
    }
    Check("every posted task runs", count == kThreads * kEach, std::to_string(count));
    Check("each thread's tasks keep their order", inOrder);
  }
  {
    // A task may post another, and may start a timer.
    auto loop = solar::test::MakeLoop();
    std::string order;
    loop->Post([&] {
      order += "a";
      loop->Post([&] { order += "c"; });
      loop->PostDelayed(10ms, [&] { order += "d"; });
      order += "b";
    });
    loop->Run();
    Check("a task can post and start timers", order == "abcd", order);
  }

  std::printf("loop: %d/%d passed\n", total - failed, total);
  return failed == 0 ? 0 : 1;
}
