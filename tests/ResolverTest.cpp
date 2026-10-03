#include <atomic>
#include <chrono>
#include <cstdio>
#include <optional>
#include <thread>
#include <vector>

#include "solar/net/Resolver.h"
#include "support/TestLoop.h"

namespace {

using namespace std::chrono_literals;
using solar::net::Loop;
using solar::net::ResolveResult;
using solar::net::Resolver;
using solar::net::SocketAddress;
using solar::net::SystemResolverOptions;
using Family = SocketAddress::Family;

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

// The addresses as text, for comparing and for saying what was wrong.
std::string Show(const ResolveResult& r) {
  std::string out;
  for (const SocketAddress& a : r.addresses) {
    if (!out.empty()) out += ",";
    if (a.family == Family::IPv4) {
      out += std::to_string(a.bytes[0]) + "." + std::to_string(a.bytes[1]) + "." + std::to_string(a.bytes[2]) + "." + std::to_string(a.bytes[3]);
    } else {
      out += "v6:" + std::to_string(a.bytes[0]) + "..." + std::to_string(a.bytes[15]);
    }
    out += ":" + std::to_string(a.port);
  }
  return r.error.empty() ? out : "error: " + r.error;
}

// Resolves one name and runs the loop until it answers.
ResolveResult ResolveNow(Loop& loop, Resolver& resolver, const std::string& host, Family family, uint16_t port = 80) {
  ResolveResult result;
  bool answered = false;
  resolver.Resolve(host, port, family, [&](ResolveResult r) {
    result = std::move(r);
    answered = true;
  });
  loop.Run();
  if (!answered) result.error = "no answer";
  return result;
}

}  // namespace

int main() {
  {
    auto loop = solar::test::MakeLoop();
    auto resolver = solar::net::MakeSystemResolver(*loop);

    ResolveResult v4 = ResolveNow(*loop, *resolver, "127.0.0.1", Family::IPv4, 8080);
    Check("an IPv4 literal is its own answer", v4.error.empty() && v4.addresses.size() == 1 && v4.addresses[0].port == 8080 && Show(v4) == "127.0.0.1:8080", Show(v4));
    ResolveResult none = ResolveNow(*loop, *resolver, "127.0.0.1", Family::IPv6);
    Check("and has no address of the other family, which is not an error", none.error.empty() && none.addresses.empty(), Show(none));
    ResolveResult v6 = ResolveNow(*loop, *resolver, "::1", Family::IPv6, 443);
    Check("an IPv6 literal", v6.error.empty() && v6.addresses.size() == 1 && v6.addresses[0].family == Family::IPv6 && v6.addresses[0].port == 443, Show(v6));
    ResolveResult bracketed = ResolveNow(*loop, *resolver, "[::1]", Family::IPv6);
    Check("a bracketed IPv6 literal", bracketed.addresses.size() == 1, Show(bracketed));

    ResolveResult local = ResolveNow(*loop, *resolver, "localhost", Family::IPv4);
    Check("localhost has an IPv4 address", local.error.empty() && !local.addresses.empty() && local.addresses[0].bytes[0] == 127, Show(local));
    ResolveResult local6 = ResolveNow(*loop, *resolver, "localhost", Family::IPv6);
    bool allV6 = true;
    for (const SocketAddress& a : local6.addresses) allV6 = allV6 && a.family == Family::IPv6;
    Check("and any IPv6 one it has is IPv6", allV6, Show(local6));

    ResolveResult missing = ResolveNow(*loop, *resolver, "no-such-host.invalid", Family::IPv4);
    Check("a name that does not exist is an error", missing.error.find("cannot resolve") != std::string::npos && missing.addresses.empty(), Show(missing));
  }
  {
    // The answer comes from the loop, not from inside the call.
    auto loop = solar::test::MakeLoop();
    auto resolver = solar::net::MakeSystemResolver(*loop);
    bool insideCall = true;
    bool calledInside = false;
    std::thread::id answeredOn;
    resolver->Resolve("127.0.0.1", 1, Family::IPv4, [&](ResolveResult) {
      calledInside = insideCall;
      answeredOn = std::this_thread::get_id();
    });
    insideCall = false;
    loop->Run();
    Check("the callback is not called from inside Resolve", !calledInside);
    Check("and runs on the loop's thread", answeredOn == std::this_thread::get_id());
  }
  {
    // A lookup that takes a while does not stop the loop from doing other things.
    auto loop = solar::test::MakeLoop();
    SystemResolverOptions options;
    options.lookup = [](const std::string&, uint16_t port, Family) {
      std::this_thread::sleep_for(400ms);
      ResolveResult r;
      SocketAddress a;
      a.bytes = {10, 0, 0, 1};
      a.port = port;
      r.addresses.push_back(a);
      return r;
    };
    auto resolver = solar::net::MakeSystemResolver(*loop, options);

    int ticks = 0;
    bool answered = false;
    resolver->Resolve("slow.example", 80, Family::IPv4, [&](ResolveResult) { answered = true; });
    std::function<void()> tick = [&] {
      if (answered) return;
      ++ticks;
      loop->PostDelayed(20ms, tick);
    };
    loop->PostDelayed(20ms, tick);
    const auto start = std::chrono::steady_clock::now();
    loop->Run();
    Check("a slow lookup answers", answered);
    Check("while the loop kept running", ticks >= 10, std::to_string(ticks) + " ticks in " + std::to_string(Seconds(start)) + " s");
  }
  {
    // Lookups run side by side. How many are in a lookup at once says so without a clock, which a
    // loaded machine makes unreliable.
    auto loop = solar::test::MakeLoop();
    SystemResolverOptions options;
    options.threads = 4;
    std::atomic<int> inside{0};
    std::atomic<int> mostInside{0};
    options.lookup = [&](const std::string&, uint16_t port, Family) {
      const int now = ++inside;
      int seen = mostInside.load();
      while (now > seen && !mostInside.compare_exchange_weak(seen, now)) {
      }
      std::this_thread::sleep_for(50ms);
      --inside;
      ResolveResult r;
      SocketAddress a;
      a.bytes = {10, 0, 0, 2};
      a.port = port;
      r.addresses.push_back(a);
      return r;
    };
    auto resolver = solar::net::MakeSystemResolver(*loop, options);
    int answers = 0;
    for (int i = 0; i < 40; ++i) {
      resolver->Resolve("host" + std::to_string(i) + ".example", 80, Family::IPv4, [&](ResolveResult r) { answers += r.addresses.size() == 1; });
    }
    loop->Run();
    Check("forty lookups all answer", answers == 40, std::to_string(answers));
    Check("on several threads at once", mostInside >= 2, std::to_string(mostInside));
    Check("but no more than the resolver was given", mostInside <= 4, std::to_string(mostInside));
  }
  {
    // A cancelled lookup is not answered, and Run does not wait for it.
    auto loop = solar::test::MakeLoop();
    SystemResolverOptions options;
    options.lookup = [](const std::string&, uint16_t, Family) {
      std::this_thread::sleep_for(600ms);
      return ResolveResult{};
    };
    auto resolver = solar::net::MakeSystemResolver(*loop, options);
    bool answered = false;
    const auto id = resolver->Resolve("slow.example", 80, Family::IPv4, [&](ResolveResult) { answered = true; });
    loop->PostDelayed(20ms, [&] { resolver->Cancel(id); });
    const auto start = std::chrono::steady_clock::now();
    loop->Run();
    Check("a cancelled lookup is not answered", !answered);
    Check("and Run does not wait for it", Seconds(start) < 0.4, std::to_string(Seconds(start)) + " s");
    resolver->Cancel(id);  // already gone; must be harmless
    resolver.reset();      // waits for the thread that is still sleeping
    loop->Run();
    Check("a late answer after cancelling does nothing", !answered);
  }
  {
    // Answers are kept for a while.
    auto loop = solar::test::MakeLoop();
    std::atomic<int> lookups{0};
    SystemResolverOptions options;
    options.cacheTtl = 150ms;
    options.lookup = [&](const std::string&, uint16_t port, Family) {
      ++lookups;
      ResolveResult r;
      SocketAddress a;
      a.bytes = {10, 0, 0, 3};
      a.port = port;
      r.addresses.push_back(a);
      return r;
    };
    auto resolver = solar::net::MakeSystemResolver(*loop, options);
    ResolveNow(*loop, *resolver, "cached.example", Family::IPv4);
    ResolveResult again = ResolveNow(*loop, *resolver, "cached.example", Family::IPv4);
    Check("a repeated lookup is answered from the cache", lookups == 1 && again.addresses.size() == 1, std::to_string(lookups));
    ResolveNow(*loop, *resolver, "cached.example", Family::IPv6);
    Check("the other family is looked up on its own", lookups == 2, std::to_string(lookups));
    ResolveNow(*loop, *resolver, "cached.example", Family::IPv4, 8080);
    Check("and so is another port", lookups == 3, std::to_string(lookups));
    std::this_thread::sleep_for(200ms);
    ResolveNow(*loop, *resolver, "cached.example", Family::IPv4);
    Check("an old answer is looked up again", lookups == 4, std::to_string(lookups));
  }
  {
    // Failures are not kept.
    auto loop = solar::test::MakeLoop();
    std::atomic<int> lookups{0};
    SystemResolverOptions options;
    options.lookup = [&](const std::string&, uint16_t, Family) {
      ++lookups;
      return ResolveResult{{}, "no such host"};
    };
    auto resolver = solar::net::MakeSystemResolver(*loop, options);
    ResolveNow(*loop, *resolver, "gone.example", Family::IPv4);
    ResolveNow(*loop, *resolver, "gone.example", Family::IPv4);
    Check("a failed lookup is not cached", lookups == 2, std::to_string(lookups));
  }
  {
    // Destroying the resolver with a lookup out must not leave Run waiting for it.
    auto loop = solar::test::MakeLoop();
    SystemResolverOptions options;
    options.lookup = [](const std::string&, uint16_t, Family) {
      std::this_thread::sleep_for(150ms);
      return ResolveResult{};
    };
    bool answered = false;
    {
      auto resolver = solar::net::MakeSystemResolver(*loop, options);
      resolver->Resolve("slow.example", 80, Family::IPv4, [&](ResolveResult) { answered = true; });
    }
    const auto start = std::chrono::steady_clock::now();
    loop->Run();
    Check("Run does not wait for a lookup whose resolver is gone", Seconds(start) < 0.1 && !answered, std::to_string(Seconds(start)) + " s");
  }

  std::printf("resolver: %d/%d passed\n", total - failed, total);
  return failed == 0 ? 0 : 1;
}
