#include <cstdio>
#include <string>
#include <vector>

#include "solar/net/AddressRace.h"

namespace {

using solar::net::ResolveResult;
using solar::net::SocketAddress;
using solar::net::internal::AddressRace;
using Family = SocketAddress::Family;
using Timer = AddressRace::Timer;

int total = 0;
int failed = 0;

SocketAddress Address(Family family, int n) {
  SocketAddress a;
  a.family = family;
  a.bytes[0] = static_cast<uint8_t>(n);
  return a;
}

ResolveResult Found(std::initializer_list<SocketAddress> list) { return ResolveResult{std::vector<SocketAddress>(list), ""}; }
ResolveResult Nothing() { return ResolveResult{}; }
ResolveResult Error(const std::string& text) { return ResolveResult{{}, text}; }

SocketAddress V6(int n) { return Address(Family::IPv6, n); }
SocketAddress V4(int n) { return Address(Family::IPv4, n); }

// Records what the race asks for, as text, and lets a test fire the timers it has been given.
class Harness : public AddressRace::Delegate {
 public:
  explicit Harness(AddressRace::Config config = {}) : race(*this, config) {}

  AddressRace::AttemptId StartAttempt(const SocketAddress& address) override {
    const auto id = ++nextId;
    log.push_back("start " + std::to_string(id) + ":" + (address.family == Family::IPv6 ? "v6#" : "v4#") + std::to_string(address.bytes[0]));
    return id;
  }
  void CancelAttempt(AddressRace::AttemptId id) override { log.push_back("cancel " + std::to_string(id)); }
  void StartTimer(Timer which, std::chrono::milliseconds delay) override {
    running[Index(which)] = true;
    log.push_back(std::string("timer ") + Name(which) + "=" + std::to_string(delay.count()));
  }
  void StopTimer(Timer which) override {
    running[Index(which)] = false;
    log.push_back(std::string("stop ") + Name(which));
  }
  void Succeeded(AddressRace::AttemptId id) override { log.push_back("won " + std::to_string(id)); }
  void Failed(const std::string& message) override { log.push_back("failed " + message); }

  void Fire(Timer which) {
    if (!running[Index(which)]) {
      log.push_back(std::string("ERROR ") + Name(which) + " timer fired but is not running");
      return;
    }
    running[Index(which)] = false;
    race.TimerFired(which);
  }

  static int Index(Timer t) { return t == Timer::ResolutionDelay ? 0 : 1; }
  static const char* Name(Timer t) { return t == Timer::ResolutionDelay ? "resolution" : "next"; }

  AddressRace race;
  std::vector<std::string> log;
  bool running[2] = {false, false};
  AddressRace::AttemptId nextId = 0;
};

std::string Join(const std::vector<std::string>& lines) {
  std::string out;
  for (const std::string& line : lines) out += (out.empty() ? "" : " | ") + line;
  return out;
}

void Expect(const char* name, const Harness& h, const std::vector<std::string>& want) {
  ++total;
  if (h.log == want) return;
  ++failed;
  std::printf("FAIL %s\n  want: %s\n  got:  %s\n", name, Join(want).c_str(), Join(h.log).c_str());
}

}  // namespace

int main() {
  {
    Harness h;
    h.race.Answered(Family::IPv6, Found({V6(1)}));
    h.race.Answered(Family::IPv4, Found({V4(1)}));
    h.Fire(Timer::NextAttempt);
    h.race.AttemptSucceeded(1);
    Expect("IPv6 first, IPv4 after the delay, the first to connect wins", h,
           {"start 1:v6#1", "timer next=250", "start 2:v4#1", "cancel 2", "won 1"});
  }
  {
    Harness h;
    h.race.Answered(Family::IPv4, Found({V4(1)}));
    h.race.Answered(Family::IPv6, Found({V6(1)}));
    Expect("IPv4 answers first: IPv6 is waited for, and then goes first", h,
           {"timer resolution=50", "stop resolution", "start 1:v6#1", "timer next=250"});
  }
  {
    Harness h;
    h.race.Answered(Family::IPv4, Found({V4(1)}));
    h.Fire(Timer::ResolutionDelay);
    h.race.Answered(Family::IPv6, Found({V6(1)}));
    h.Fire(Timer::NextAttempt);
    Expect("IPv6 too late for the delay: IPv4 goes first and IPv6 follows", h,
           {"timer resolution=50", "start 1:v4#1", "timer next=250", "start 2:v6#1"});
  }
  {
    Harness h;
    h.race.Answered(Family::IPv6, Found({V6(1)}));
    h.race.Answered(Family::IPv4, Found({V4(1)}));
    Expect("IPv6 answers first: it starts at once, IPv4 is not waited for", h, {"start 1:v6#1", "timer next=250"});
  }
  {
    Harness h;
    h.race.Answered(Family::IPv6, Found({V6(1)}));
    h.race.Answered(Family::IPv4, Found({V4(1)}));
    h.race.AttemptFailed(1, "refused");
    Expect("a failed attempt does not make the next one wait", h, {"start 1:v6#1", "timer next=250", "stop next", "start 2:v4#1"});
  }
  {
    Harness h;
    h.race.Answered(Family::IPv6, Found({V6(1), V6(2)}));
    h.race.Answered(Family::IPv4, Found({V4(1), V4(2)}));
    h.Fire(Timer::NextAttempt);
    h.Fire(Timer::NextAttempt);
    h.Fire(Timer::NextAttempt);
    Expect("the families take turns", h,
           {"start 1:v6#1", "timer next=250", "start 2:v4#1", "timer next=250", "start 3:v6#2", "timer next=250", "start 4:v4#2"});
  }
  {
    Harness h;
    h.race.Answered(Family::IPv6, Found({V6(1), V6(2), V6(3)}));
    h.race.Answered(Family::IPv4, Nothing());
    h.Fire(Timer::NextAttempt);
    h.Fire(Timer::NextAttempt);
    Expect("with only one family, its addresses are tried in turn", h,
           {"start 1:v6#1", "timer next=250", "start 2:v6#2", "timer next=250", "start 3:v6#3"});
  }

  // Failure.
  {
    Harness h;
    h.race.Answered(Family::IPv6, Found({V6(1)}));
    h.race.Answered(Family::IPv4, Found({V4(1)}));
    h.race.AttemptFailed(1, "refused");
    h.race.AttemptFailed(2, "timed out");
    Expect("when every attempt fails, the last reason is given", h,
           {"start 1:v6#1", "timer next=250", "stop next", "start 2:v4#1", "failed connection failed: timed out"});
  }
  {
    Harness h;
    h.race.Answered(Family::IPv6, Found({V6(1)}));
    h.race.AttemptFailed(1, "refused");
    h.race.Answered(Family::IPv4, Nothing());
    Expect("it waits for the other lookup before giving up", h, {"start 1:v6#1", "timer next=250", "stop next", "failed connection failed: refused"});
  }
  {
    Harness h;
    h.race.Answered(Family::IPv6, Error("v6 lookup failed"));
    h.race.Answered(Family::IPv4, Error("v4 lookup failed"));
    Expect("both lookups failing is reported as a lookup failure", h, {"failed v4 lookup failed"});
  }
  {
    Harness h;
    h.race.Answered(Family::IPv6, Nothing());
    h.race.Answered(Family::IPv4, Nothing());
    Expect("no addresses at all", h, {"failed no address to connect to"});
  }
  {
    Harness h;
    h.race.Answered(Family::IPv4, Error("v4 failed"));
    h.race.Answered(Family::IPv6, Found({V6(1)}));
    Expect("one lookup failing and the other succeeding is enough", h, {"start 1:v6#1"});
  }

  // Only one family exists.
  {
    Harness h;
    h.race.Answered(Family::IPv6, Nothing());
    h.race.Answered(Family::IPv4, Found({V4(1)}));
    Expect("no IPv6 address: IPv4 starts at once", h, {"start 1:v4#1"});
  }
  {
    Harness h;
    h.race.Answered(Family::IPv4, Found({V4(1)}));
    h.race.Answered(Family::IPv6, Nothing());
    Expect("IPv6 answering that it has nothing ends the wait", h, {"timer resolution=50", "stop resolution", "start 1:v4#1"});
  }

  // Timing.
  {
    Harness h;
    h.race.Answered(Family::IPv6, Found({V6(1)}));
    h.Fire(Timer::NextAttempt);
    h.race.Answered(Family::IPv4, Found({V4(1)}));
    Expect("an answer after the delay has gone is tried at once", h, {"start 1:v6#1", "timer next=250", "start 2:v4#1"});
  }
  {
    Harness h(AddressRace::Config{std::chrono::milliseconds(10), std::chrono::milliseconds(20)});
    h.race.Answered(Family::IPv4, Found({V4(1), V4(2)}));
    h.Fire(Timer::ResolutionDelay);
    Expect("the delays are the configured ones", h, {"timer resolution=10", "start 1:v4#1", "timer next=20"});
  }

  // After the end.
  {
    Harness h;
    h.race.Answered(Family::IPv6, Found({V6(1)}));
    h.race.AttemptSucceeded(1);
    h.race.AttemptFailed(2, "late");
    h.race.AttemptSucceeded(3);
    h.race.TimerFired(Timer::NextAttempt);
    h.race.Answered(Family::IPv4, Found({V4(1)}));
    Expect("nothing happens after the race is won", h, {"start 1:v6#1", "timer next=250", "stop next", "won 1"});
  }
  {
    Harness h;
    h.race.Answered(Family::IPv6, Found({V6(1)}));
    h.race.AttemptFailed(99, "not an attempt");
    h.race.AttemptSucceeded(99);
    Expect("an attempt it never started is ignored", h, {"start 1:v6#1", "timer next=250"});
  }
  {
    Harness h;
    h.race.Answered(Family::IPv6, Found({V6(1)}));
    h.race.Answered(Family::IPv4, Found({V4(1)}));
    h.Fire(Timer::NextAttempt);
    h.race.Abandon();
    h.race.AttemptSucceeded(1);
    h.race.Answered(Family::IPv4, Found({V4(2)}));
    Expect("abandoning cancels what is running and ends it", h, {"start 1:v6#1", "timer next=250", "start 2:v4#1", "cancel 1", "cancel 2"});
  }
  {
    Harness h;
    h.race.Answered(Family::IPv6, Found({V6(1)}));
    h.race.Answered(Family::IPv4, Found({V4(1)}));
    h.race.Abandon();
    Expect("abandoning stops the timer", h, {"start 1:v6#1", "timer next=250", "stop next", "cancel 1"});
  }

  std::printf("address race: %d/%d passed\n", total - failed, total);
  return failed == 0 ? 0 : 1;
}
