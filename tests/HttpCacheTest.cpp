#include <chrono>
#include <cstdio>
#include <string>

#include "solar/net/HttpCache.h"
#include "support/DateText.h"

namespace {

using namespace std::chrono_literals;
using solar::net::CacheMode;
using solar::net::HttpCache;
using solar::net::HttpCacheOptions;
using solar::net::HttpResponseHead;
using solar::net::RequestFields;
using solar::test::At;
using solar::test::FormatDate;
using Freshness = HttpCache::Freshness;
using Time = std::chrono::system_clock::time_point;

int total = 0;
int failed = 0;

void Check(const std::string& name, bool ok, const std::string& detail = "") {
  ++total;
  if (ok) return;
  ++failed;
  std::printf("FAIL %s %s\n", name.c_str(), detail.c_str());
}

HttpResponseHead Head(int status, std::vector<std::pair<std::string, std::string>> headers) {
  HttpResponseHead head;
  head.status = status;
  head.headers = std::move(headers);
  return head;
}

// A cache whose clock the test moves, and the time at which things happen.
struct Clocked {
  Time now = At(2025, 3, 10, 12);
  HttpCache cache;
  explicit Clocked(HttpCacheOptions options = HttpCacheOptions()) : cache(options, [this] { return now; }) {}

  // Stores a response as if it arrived at `now`, with the request and response a moment apart.
  void Put(const std::string& key, const HttpResponseHead& head, const std::string& body = "body", const RequestFields& request = {}) {
    cache.Store(key, request, head, body, now, now);
  }
  HttpCache::Freshness Look(const std::string& key, const RequestFields& request = {}, CacheMode mode = CacheMode::Default) {
    return cache.Find(key, request, mode).freshness;
  }
};

}  // namespace

int main() {
  // ---- How long a response is fresh ----
  {
    Clocked c;
    c.Put("k", Head(200, {{"Cache-Control", "max-age=60"}}));
    c.now += 59s;
    Check("fresh until max-age", c.Look("k") == Freshness::Fresh);
    c.now += 2s;
    Check("then stale", c.Look("k") == Freshness::Stale);
  }
  {
    Clocked c;
    c.Put("k", Head(200, {{"Cache-Control", "max-age=60"}, {"Age", "30"}}));
    c.now += 29s;
    Check("the Age header counts", c.Look("k") == Freshness::Fresh);
    c.now += 2s;
    Check("against max-age", c.Look("k") == Freshness::Stale);
  }
  {
    Clocked c;
    c.cache.Store("k", {}, Head(200, {{"Cache-Control", "max-age=60"}}), "x", c.now - 20s, c.now);  // the request took 20 s
    c.now += 39s;
    Check("the time the request took counts too", c.Look("k") == Freshness::Fresh);
    c.now += 2s;
    Check("so a slow response is stale sooner", c.Look("k") == Freshness::Stale);
  }
  {
    Clocked c;
    // The server's clock is 10 minutes behind ours: the Date it sends is older than it should be.
    c.Put("k", Head(200, {{"Cache-Control", "max-age=900"}, {"Date", FormatDate(c.now - 600s)}}));
    c.now += 299s;
    Check("a Date in the past makes it older than it looks", c.Look("k") == Freshness::Fresh);
    c.now += 2s;
    Check("by that much", c.Look("k") == Freshness::Stale);
  }
  {
    Clocked c;
    c.Put("k", Head(200, {{"Date", FormatDate(c.now)}, {"Expires", FormatDate(c.now + 100s)}}));
    c.now += 99s;
    Check("Expires is a date", c.Look("k") == Freshness::Fresh);
    c.now += 2s;
    Check("after which it is stale", c.Look("k") == Freshness::Stale);
  }
  {
    Clocked c;
    c.Put("k", Head(200, {{"Cache-Control", "max-age=10"}, {"Expires", FormatDate(c.now + 1000s)}}));
    c.now += 11s;
    Check("max-age beats Expires", c.Look("k") == Freshness::Stale);
  }
  {
    Clocked c;
    c.Put("k", Head(200, {{"Expires", "0"}, {"ETag", "\"x\""}}));
    Check("an Expires that is not a date means stale", c.Look("k") == Freshness::Stale);
  }
  {
    Clocked c;
    c.Put("k", Head(200, {{"Last-Modified", FormatDate(c.now - 120h)}, {"Date", FormatDate(c.now)}}));
    c.now += 11h;
    Check("without anything else, a tenth of the age since Last-Modified", c.Look("k") == Freshness::Fresh);
    c.now += 2h;
    Check("and no more", c.Look("k") == Freshness::Stale);
  }
  {
    Clocked c;
    c.Put("k", Head(200, {{"Last-Modified", FormatDate(c.now - 24 * 24h * 365)}, {"Date", FormatDate(c.now)}}));
    c.now += 25h;
    Check("a heuristic freshness stops at a day", c.Look("k") == Freshness::Stale);
  }
  {
    Clocked c;
    const auto head = Head(302, {{"Last-Modified", FormatDate(c.now - 120h)}});
    Check("a status that is not cacheable by default needs more than Last-Modified", !c.cache.Storable({}, head, CacheMode::Default));
  }

  // ---- What is kept ----
  {
    Clocked c;
    const auto ok = [&](const HttpResponseHead& h, const RequestFields& r = {}, CacheMode m = CacheMode::Default) { return c.cache.Storable(r, h, m); };
    Check("a plain response with a lifetime", ok(Head(200, {{"Cache-Control", "max-age=60"}})));
    Check("no-store is not", !ok(Head(200, {{"Cache-Control", "max-age=60, no-store"}})));
    Check("not in any case", !ok(Head(200, {{"Cache-Control", "NO-STORE"}})));
    Check("nor in the NoStore mode", !ok(Head(200, {{"Cache-Control", "max-age=60"}}), {}, CacheMode::NoStore));
    Check("nor when the request was authorized", !ok(Head(200, {{"Cache-Control", "max-age=60"}}), {{"Authorization", "Bearer x"}}));
    Check("Vary: * is not", !ok(Head(200, {{"Cache-Control", "max-age=60"}, {"Vary", "Accept, *"}})));
    Check("a partial response is not", !ok(Head(206, {{"Cache-Control", "max-age=60"}})));
    Check("nor a 304", !ok(Head(304, {{"Cache-Control", "max-age=60"}})));
    Check("an interim one is not", !ok(Head(103, {{"Cache-Control", "max-age=60"}})));
    Check("a 500 is not, unless it says how long", !ok(Head(500, {})) && ok(Head(500, {{"Cache-Control", "max-age=60"}})));
    Check("a 404 can be, with a lifetime", ok(Head(404, {{"Cache-Control", "max-age=60"}})));
    Check("one that is stale at once and cannot be checked is no use", !ok(Head(200, {{"Cache-Control", "max-age=0"}})) && !ok(Head(200, {})));
    Check("but with a validator it is", ok(Head(200, {{"Cache-Control", "max-age=0"}, {"ETag", "\"x\""}})) && ok(Head(200, {{"Last-Modified", "Mon, 01 Jan 2024 00:00:00 GMT"}})));
    Check("private is for this cache", ok(Head(200, {{"Cache-Control", "private, max-age=60"}})));
    Check("max-age that is no number means stale", !ok(Head(200, {{"Cache-Control", "max-age=abc"}})));
    Check("directives in several headers, the smaller max-age", !ok(Head(200, {{"Cache-Control", "max-age=100"}, {"Cache-Control", "max-age=0"}})));
    Check("a quoted max-age", ok(Head(200, {{"Cache-Control", "max-age=\"60\""}})));
    Check("a directive with a quoted comma", ok(Head(200, {{"Cache-Control", "no-cache=\"a,b\", max-age=60"}})));
  }
  {
    Clocked c;
    c.Put("k", Head(200, {{"Cache-Control", "no-cache, max-age=600"}, {"ETag", "\"x\""}}));
    Check("no-cache means it is checked even while it would be fresh", c.Look("k") == Freshness::Stale);
  }

  // ---- Modes ----
  {
    Clocked c;
    c.Put("k", Head(200, {{"Cache-Control", "max-age=60"}}));
    Check("Default uses a fresh response", c.Look("k", {}, CacheMode::Default) == Freshness::Fresh);
    Check("NoStore ignores the cache", c.Look("k", {}, CacheMode::NoStore) == Freshness::Miss);
    Check("so does Reload", c.Look("k", {}, CacheMode::Reload) == Freshness::Miss);
    Check("NoCache checks even a fresh one", c.Look("k", {}, CacheMode::NoCache) == Freshness::Stale);
    c.now += 1h;
    Check("ForceCache takes a stale one as it is", c.Look("k", {}, CacheMode::ForceCache) == Freshness::Fresh);
    Check("so does OnlyIfCached", c.Look("k", {}, CacheMode::OnlyIfCached) == Freshness::Fresh);
    Check("what is not there is a miss in every mode", c.Look("other", {}, CacheMode::ForceCache) == Freshness::Miss);
  }

  // ---- Variants ----
  {
    Clocked c;
    const RequestFields en = {{"Accept-Language", "en"}}, fr = {{"accept-language", "fr"}};
    const auto head = Head(200, {{"Cache-Control", "max-age=60"}, {"Vary", "Accept-Language"}});
    c.Put("k", head, "english", en);
    Check("a request that varies the same way finds it", c.Look("k", en) == Freshness::Fresh);
    Check("one that varies otherwise does not", c.Look("k", fr) == Freshness::Miss && c.Look("k", {}) == Freshness::Miss);
    c.Put("k", head, "french", fr);
    Check("both are kept", c.cache.entries() == 2 && *c.cache.Find("k", fr, CacheMode::Default).entry->body == "french" &&
                           *c.cache.Find("k", en, CacheMode::Default).entry->body == "english");
    c.Put("k", head, "english again", en);
    Check("a second response for the same variant replaces it", c.cache.entries() == 2 && *c.cache.Find("k", en, CacheMode::Default).entry->body == "english again");
  }
  {
    Clocked c;
    c.Put("k", Head(200, {{"Cache-Control", "max-age=60"}, {"Vary", "accept , ACCEPT-ENCODING"}}), "x", {{"Accept", "a"}, {"Accept-Encoding", "gzip"}});
    Check("every name in Vary counts, whatever its case", c.Look("k", {{"accept", "a"}, {"accept-encoding", "gzip"}}) == Freshness::Fresh &&
                                                           c.Look("k", {{"accept", "a"}, {"accept-encoding", "br"}}) == Freshness::Miss);
  }

  // ---- Checking with the server ----
  {
    Clocked c;
    c.Put("k", Head(200, {{"ETag", "W/\"v1\""}, {"Last-Modified", "Mon, 01 Jan 2024 00:00:00 GMT"}, {"Cache-Control", "max-age=0"}}));
    const auto validators = c.cache.Find("k", {}, CacheMode::Default).entry->Validators();
    Check("a check carries both validators", validators.size() == 2 && validators[0].first == "If-None-Match" && validators[0].second == "W/\"v1\"" &&
                                             validators[1].first == "If-Modified-Since" && validators[1].second == "Mon, 01 Jan 2024 00:00:00 GMT");
  }
  {
    Clocked c;
    c.Put("k", Head(200, {{"Cache-Control", "max-age=10"}, {"ETag", "\"v1\""}, {"Content-Type", "text/plain"}, {"X-Old", "1"}}), "the body");
    c.now += 20s;
    auto old = c.cache.Find("k", {}, CacheMode::Default);
    Check("stale, with the entry to check", old.freshness == Freshness::Stale && old.entry);
    const auto fresh = c.cache.Freshen(old.entry, Head(304, {{"Cache-Control", "max-age=100"}, {"Content-Type", "ignored"}, {"X-New", "2"}, {"Content-Length", "0"}}), c.now, c.now);
    Check("a 304 refreshes it", c.Look("k") == Freshness::Fresh && c.cache.entries() == 1);
    Check("keeping the body", *fresh->body == "the body");
    const auto head = fresh->Head();
    Check("taking the new fields", head.Header("x-new") == "2" && head.Header("cache-control") == "max-age=100" && head.Header("x-old") == "1");
    Check("but not those that describe the body", head.Header("content-type") == "text/plain" && !head.Header("content-length"));
    c.now += 99s;
    Check("for as long as the new lifetime", c.Look("k") == Freshness::Fresh);
    c.now += 2s;
    Check("and no longer", c.Look("k") == Freshness::Stale);
  }
  {
    Clocked c;
    c.Put("k", Head(200, {{"Cache-Control", "max-age=60"}}));
    c.cache.Remove("k", {});
    Check("a response can be let go", c.cache.entries() == 0 && c.cache.bytes() == 0 && c.Look("k") == Freshness::Miss);
  }

  // ---- Room ----
  {
    HttpCacheOptions options;
    options.maxBytes = 3000;
    Clocked c(options);
    const auto head = Head(200, {{"Cache-Control", "max-age=600"}});
    c.Put("a", head, std::string(1000, 'a'));
    c.Put("b", head, std::string(1000, 'b'));
    c.Look("a");  // a is used, so b is the one least recently used
    c.Put("c", head, std::string(1000, 'c'));
    Check("the least recently used goes first", c.Look("b") == Freshness::Miss && c.Look("a") == Freshness::Fresh && c.Look("c") == Freshness::Fresh);
    Check("and the size stays within the limit", c.cache.bytes() <= 3000, std::to_string(c.cache.bytes()));
  }
  {
    HttpCacheOptions options;
    options.maxEntryBytes = 500;
    Clocked c(options);
    c.Put("big", Head(200, {{"Cache-Control", "max-age=600"}}), std::string(1000, 'x'));
    c.Put("small", Head(200, {{"Cache-Control", "max-age=600"}}), "x");
    Check("a response over the entry limit is not kept", c.cache.entries() == 1 && c.Look("big") == Freshness::Miss && c.Look("small") == Freshness::Fresh);
  }
  {
    Clocked c;
    c.Put("k", Head(200, {{"Cache-Control", "max-age=60"}}), "one");
    c.Put("k", Head(200, {{"Cache-Control", "max-age=60"}}), "two");
    Check("the same response again replaces the first", c.cache.entries() == 1 && *c.cache.Find("k", {}, CacheMode::Default).entry->body == "two");
  }

  std::printf("http cache: %d/%d passed\n", total - failed, total);
  return failed == 0 ? 0 : 1;
}
