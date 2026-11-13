#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "solar/net/ContentFilter.h"

namespace {

using namespace solar::net;

int total = 0;
int failed = 0;

void Check(const std::string& name, bool ok) {
  ++total;
  if (ok) return;
  ++failed;
  std::printf("FAIL %s\n", name.c_str());
}

std::string HostOf(const std::string& url) {
  size_t b = url.find("://") + 3;
  size_t e = url.find_first_of("/?#:", b);
  return url.substr(b, e == std::string::npos ? std::string::npos : e - b);
}

bool Blocked(const ContentFilter& f, const std::string& url, const std::string& page, uint16_t type = kFilterScript, bool third = true) {
  const std::string host = HostOf(url);
  return f.Blocks({url, host, page, type, third});
}

ContentFilter Make(const std::string& list) {
  ContentFilter f;
  f.AddList(list);
  f.Finish();
  return f;
}

}  // namespace

int main(int argc, char** argv) {
  {
    ContentFilter f = Make("||ads.example.com^\n");
    Check("host", Blocked(f, "https://ads.example.com/x.js", "news.org"));
    Check("subdomain", Blocked(f, "https://a.b.ads.example.com/x.js", "news.org"));
    Check("other host", !Blocked(f, "https://example.com/x.js", "news.org"));
    Check("not a suffix of a label", !Blocked(f, "https://badads.example.com/x.js", "news.org"));
    Check("in the path is no host", !Blocked(f, "https://news.org/?u=https://ads.example.com/", "news.org"));
  }
  {
    ContentFilter f = Make("||example.com/ads/*$script,third-party\n");
    Check("path rule", Blocked(f, "https://example.com/ads/a.js", "x.org"));
    Check("type", !Blocked(f, "https://example.com/ads/a.png", "x.org", kFilterImage));
    Check("first party", !Blocked(f, "https://example.com/ads/a.js", "example.com", kFilterScript, false));
  }
  {
    ContentFilter f = Make("/banner/*/img^\n||cdn.com^$image\n&adunit=\n|https://track.\n.gif|\n");
    Check("wildcard", Blocked(f, "https://a.org/banner/300/img?x=1", "p.org"));
    Check("separator", !Blocked(f, "https://a.org/banner/300/imgs", "p.org"));
    Check("image only", Blocked(f, "https://cdn.com/a.png", "p.org", kFilterImage));
    Check("image only: script not", !Blocked(f, "https://cdn.com/a.js", "p.org", kFilterScript));
    Check("substring", Blocked(f, "https://a.org/x?y=1&adunit=3", "p.org"));
    Check("start anchor", Blocked(f, "https://track.a.org/x", "p.org"));
    Check("start anchor not inside", !Blocked(f, "https://a.org/?u=https://track.a.org/x", "p.org"));
    Check("end anchor", Blocked(f, "https://a.org/pixel.gif", "p.org", kFilterImage));
    Check("end anchor not", !Blocked(f, "https://a.org/pixel.gif?x", "p.org", kFilterImage));
  }
  {
    ContentFilter f = Make("||ads.com^\n@@||ads.com/ok.js\n||t.com^$domain=a.org|~b.a.org\n");
    Check("exception", !Blocked(f, "https://ads.com/ok.js", "p.org"));
    Check("exception only for its path", Blocked(f, "https://ads.com/bad.js", "p.org"));
    Check("domain in", Blocked(f, "https://t.com/x", "a.org"));
    Check("domain in (sub)", Blocked(f, "https://t.com/x", "c.a.org"));
    Check("domain out", !Blocked(f, "https://t.com/x", "other.org"));
    Check("domain excluded", !Blocked(f, "https://t.com/x", "b.a.org"));
  }
  {
    ContentFilter f = Make("! a comment\n[Adblock Plus 2.0]\nexample.com##.ad\n||a.com^$popup\n||b.com^$redirect=noop.js\n/ads\\d+/\n||c.com^$removeparam=x\n");
    Check("nothing but ones that are not understood", f.RuleCount() == 0);
  }
  {
    ContentFilter f = Make("||Example.COM^\n");
    Check("case", Blocked(f, "https://EXAMPLE.com/A.js", "p.org"));
  }
  // The real lists, where given: how many rules, how long, a few addresses.
  for (int i = 1; i < argc; ++i) {
    std::ifstream in(argv[i]);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ContentFilter f;
    auto t0 = std::chrono::steady_clock::now();
    f.AddList(text);
    f.Finish();
    auto t1 = std::chrono::steady_clock::now();
    std::printf("%s: %zu rules (%zu unindexed) in %lld ms\n", argv[i], f.RuleCount(), f.UnindexedCount(),
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()));
    const char* urls[] = {"https://securepubads.g.doubleclick.net/gampad/ads?x=1", "https://www.google-analytics.com/analytics.js", "https://example.com/index.js",
                          "https://static.example.com/app/main.js", "https://pagead2.googlesyndication.com/pagead/js/adsbygoogle.js"};
    for (const char* url : urls) std::printf("  %s -> %s\n", url, Blocked(f, url, "news.org") ? "BLOCK" : "allow");
    t0 = std::chrono::steady_clock::now();
    int n = 0;
    for (int k = 0; k < 20000; ++k) { n += Blocked(f, urls[k % 5], "news.org"); }
    t1 = std::chrono::steady_clock::now();
    std::printf("  20000 checks in %lld ms (%d blocked)\n", static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()), n);
  }
  std::printf("%d checks, %d failed\n", total, failed);
  return failed ? 1 : 0;
}
