#include <chrono>
#include <cstdio>
#include <string>

#include "solar/net/Hsts.h"

namespace {

using namespace std::chrono_literals;
using solar::net::HstsStore;

int total = 0;
int failed = 0;

void Check(const std::string& name, bool ok) {
  ++total;
  if (ok) return;
  ++failed;
  std::printf("FAIL %s\n", name.c_str());
}

// A store whose clock the test moves.
struct Clocked {
  std::chrono::system_clock::time_point now = std::chrono::system_clock::time_point{} + 1000h * 24;
  HstsStore store{[this] { return now; }};
};

bool Accepts(const std::string& header) {
  Clocked c;
  c.store.Note("example.com", header);
  return c.store.Covers("example.com");
}

}  // namespace

int main() {
  {
    Clocked c;
    c.store.Note("example.com", "max-age=3600");
    Check("the host itself is covered", c.store.Covers("example.com"));
    Check("a subdomain is not, without includeSubDomains", !c.store.Covers("www.example.com"));
    Check("another host is not", !c.store.Covers("example.org"));
    Check("a host that merely ends the same way is not", !c.store.Covers("notexample.com"));
    Check("the domain above is not", !c.store.Covers("com"));
  }
  {
    Clocked c;
    c.store.Note("example.com", "max-age=3600; includeSubDomains");
    Check("a subdomain is covered with includeSubDomains", c.store.Covers("www.example.com"));
    Check("however deep", c.store.Covers("a.b.c.example.com"));
    Check("but a lookalike is not", !c.store.Covers("notexample.com") && !c.store.Covers("example.com.evil.net"));
    Check("and nor is the parent", !c.store.Covers("com"));
  }
  {
    Clocked c;
    c.store.Note("sub.example.com", "max-age=3600; includeSubDomains");
    Check("an entry for a subdomain does not cover the domain", !c.store.Covers("example.com"));
  }
  {
    Clocked c;
    c.store.Note("example.com", "max-age=3600; includeSubDomains");
    c.now += 3599s;
    Check("covered until the max-age is up", c.store.Covers("example.com") && c.store.Covers("www.example.com"));
    c.now += 2s;
    Check("then not", !c.store.Covers("example.com") && !c.store.Covers("www.example.com"));
  }
  {
    Clocked c;
    c.store.Note("example.com", "max-age=3600");
    c.store.Note("example.com", "max-age=0");
    Check("max-age=0 forgets the host", !c.store.Covers("example.com"));
  }
  {
    Clocked c;
    c.store.Note("example.com", "max-age=3600; includeSubDomains");
    c.store.Note("example.com", "max-age=60");
    Check("a new header replaces the old policy", c.store.Covers("example.com") && !c.store.Covers("www.example.com"));
    c.now += 120s;
    Check("including how long it lasts", !c.store.Covers("example.com"));
  }
  {
    Clocked c;
    c.store.Note("example.com", "max-age=3600");
    c.store.Note("example.com", "garbage");
    Check("a header that is not valid changes nothing", c.store.Covers("example.com"));
  }

  // What is a valid header.
  Check("quoted max-age", Accepts("max-age=\"31536000\""));
  Check("names are not case sensitive", Accepts("Max-Age=5; INCLUDESUBDOMAINS"));
  Check("spaces and empty directives around", Accepts(" ;; max-age=5 ;\t includeSubDomains ; "));
  Check("a directive nobody knows is skipped", Accepts("max-age=5; preload; future=thing; other=\"q;uoted\""));
  Check("includeSubDomains first", Accepts("includeSubDomains; max-age=5"));
  Check("max-age too big for 31 bits still counts", Accepts("max-age=99999999999999999999999"));
  Check("no max-age", !Accepts("includeSubDomains"));
  Check("empty", !Accepts(""));
  Check("max-age twice", !Accepts("max-age=5; max-age=6"));
  Check("includeSubDomains twice", !Accepts("max-age=5; includeSubDomains; includeSubDomains"));
  Check("max-age without a value", !Accepts("max-age"));
  Check("max-age with an empty value", !Accepts("max-age="));
  Check("max-age that is not a number", !Accepts("max-age=abc"));
  Check("max-age that is negative", !Accepts("max-age=-1"));
  Check("max-age with a fraction", !Accepts("max-age=1.5"));
  Check("a quote that never closes", !Accepts("max-age=\"5"));
  Check("a character a token cannot hold", !Accepts("max-age=5, includeSubDomains"));
  Check("a comma where a semicolon belongs", !Accepts("max-age=5 includeSubDomains"));

  // Hosts that no certificate speaks for.
  {
    Clocked c;
    c.store.Note("127.0.0.1", "max-age=3600");
    c.store.Note("[::1]", "max-age=3600");
    c.store.Note("", "max-age=3600");
    Check("an IP address is never noted", !c.store.Covers("127.0.0.1") && !c.store.Covers("[::1]") && !c.store.Covers(""));
  }

  std::printf("hsts: %d/%d passed\n", total - failed, total);
  return failed == 0 ? 0 : 1;
}
