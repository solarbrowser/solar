#include "solar/net/HttpCache.h"

#include <algorithm>
#include <optional>

#include "solar/net/HttpDate.h"

namespace solar::net {

namespace {

using SystemClock = std::chrono::system_clock;
using namespace std::chrono_literals;

// What a heuristic may make of a Last-Modified date, at most (RFC 9111 §4.2.2 suggests a tenth).
constexpr auto kMaxHeuristicFreshness = 24h;

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 0x20) : c; }

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return Lower(x) == Lower(y); });
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

// Every field named `name`, joined with commas as HTTP says repeated fields may be.
std::string Combined(const std::vector<std::pair<std::string, std::string>>& headers, std::string_view name) {
  std::string out;
  for (const auto& [key, value] : headers) {
    if (!EqualsIgnoreCase(key, name)) continue;
    if (!out.empty()) out += ", ";
    out += value;
  }
  return out;
}

std::optional<std::string_view> First(const std::vector<std::pair<std::string, std::string>>& headers, std::string_view name) {
  for (const auto& [key, value] : headers) {
    if (EqualsIgnoreCase(key, name)) return std::string_view(value);
  }
  return std::nullopt;
}

// The directives of Cache-Control that decide what is done with a response.
struct CacheControl {
  std::optional<int64_t> maxAge;  // the smallest, if there are several
  bool noStore = false;
  bool noCache = false;
  bool isPublic = false;
};

CacheControl ParseCacheControl(const std::vector<std::pair<std::string, std::string>>& headers) {
  CacheControl out;
  const std::string text = Combined(headers, "cache-control");
  size_t i = 0;
  while (i < text.size()) {
    // A directive is a token, and may have = and a token or a quoted string, which may hold commas.
    size_t end = i;
    bool quoted = false;
    for (; end < text.size() && (quoted || text[end] != ','); ++end) {
      if (text[end] == '"') quoted = !quoted;
      if (quoted && text[end] == '\\') ++end;
    }
    const std::string_view directive = Trim(std::string_view(text).substr(i, end - i));
    i = end + 1;
    const size_t eq = directive.find('=');
    const std::string_view name = Trim(directive.substr(0, eq));
    std::string_view value = eq == std::string_view::npos ? std::string_view() : Trim(directive.substr(eq + 1));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);

    if (EqualsIgnoreCase(name, "no-store")) {
      out.noStore = true;
    } else if (EqualsIgnoreCase(name, "no-cache")) {
      out.noCache = true;  // with field names it means less, but all of it is the safer reading
    } else if (EqualsIgnoreCase(name, "public")) {
      out.isPublic = true;
    } else if (EqualsIgnoreCase(name, "max-age")) {
      // One that is not a plain number is as good as an age of nothing: the response is stale.
      int64_t seconds = 0;
      bool valid = !value.empty();
      for (char c : value) {
        if (c < '0' || c > '9') valid = false;
        else seconds = std::min<int64_t>(seconds * 10 + (c - '0'), int64_t{1} << 40);
      }
      if (!valid) seconds = 0;
      out.maxAge = out.maxAge ? std::min(*out.maxAge, seconds) : seconds;
    }
  }
  return out;
}

bool IsCacheableByDefault(int status) {
  switch (status) {
    case 200: case 203: case 204: case 300: case 301: case 308: case 404: case 405: case 410: case 414: case 501: return true;
    default: return false;
  }
}

SystemClock::time_point DateOf(const CacheEntry& entry) {
  if (auto date = First(entry.headers, "date")) {
    if (auto parsed = ParseHttpDate(*date)) return *parsed;
  }
  return entry.responseTime;
}

// Seconds a response stays fresh from the moment it was made, RFC 9111 §4.2.1.
std::chrono::seconds FreshnessLifetime(const CacheEntry& entry, const CacheControl& control) {
  if (control.maxAge) return std::chrono::seconds(std::min<int64_t>(*control.maxAge, int64_t{1} << 40));
  const SystemClock::time_point date = DateOf(entry);
  if (auto expires = First(entry.headers, "expires")) {
    // A date that cannot be read, such as "0", says the response is already stale.
    const auto parsed = ParseHttpDate(*expires);
    return parsed && *parsed > date ? std::chrono::duration_cast<std::chrono::seconds>(*parsed - date) : 0s;
  }
  if (auto modified = First(entry.headers, "last-modified"); modified && IsCacheableByDefault(entry.status)) {
    const auto parsed = ParseHttpDate(*modified);
    if (parsed && *parsed < date) return std::min<std::chrono::seconds>(std::chrono::duration_cast<std::chrono::seconds>(date - *parsed) / 10, kMaxHeuristicFreshness);
  }
  return 0s;
}

// How old the response is now, RFC 9111 §4.2.3.
std::chrono::seconds CurrentAge(const CacheEntry& entry, SystemClock::time_point now) {
  const auto seconds = [](auto d) { return std::chrono::duration_cast<std::chrono::seconds>(d); };
  const auto apparent = std::max(0s, seconds(entry.responseTime - DateOf(entry)));
  std::chrono::seconds age = 0s;
  if (auto value = First(entry.headers, "age")) {
    int64_t n = 0;
    bool valid = !value->empty();
    for (char c : *value) {
      if (c < '0' || c > '9') valid = false;
      else n = std::min<int64_t>(n * 10 + (c - '0'), int64_t{1} << 40);
    }
    if (valid) age = std::chrono::seconds(n);
  }
  const auto corrected = age + seconds(entry.responseTime - entry.requestTime);
  return std::max(apparent, corrected) + std::max(0s, seconds(now - entry.responseTime));
}

// The values a request has for the names a response varies on.
RequestFields VariedValues(const RequestFields& request, const std::vector<std::pair<std::string, std::string>>& responseHeaders) {
  RequestFields varied;
  const std::string vary = Combined(responseHeaders, "vary");
  size_t i = 0;
  while (i <= vary.size()) {
    size_t end = vary.find(',', i);
    if (end == std::string::npos) end = vary.size();
    std::string name(Trim(std::string_view(vary).substr(i, end - i)));
    i = end + 1;
    if (name.empty()) continue;
    for (char& c : name) c = Lower(c);
    std::string value = Combined(request, name);
    varied.emplace_back(std::move(name), std::move(value));
  }
  return varied;
}

bool VaryAsterisk(const std::vector<std::pair<std::string, std::string>>& headers) {
  const std::string vary = Combined(headers, "vary");
  size_t i = 0;
  while (i <= vary.size()) {
    size_t end = vary.find(',', i);
    if (end == std::string::npos) end = vary.size();
    if (Trim(std::string_view(vary).substr(i, end - i)) == "*") return true;
    i = end + 1;
  }
  return false;
}

}  // namespace

HttpResponseHead CacheEntry::Head() const {
  HttpResponseHead head;
  head.status = status;
  head.headers = headers;
  return head;
}

RequestFields CacheEntry::Validators() const {
  RequestFields fields;
  if (auto etag = First(headers, "etag")) fields.emplace_back("If-None-Match", std::string(*etag));
  if (auto modified = First(headers, "last-modified")) fields.emplace_back("If-Modified-Since", std::string(*modified));
  return fields;
}

size_t CacheEntry::size() const {
  size_t n = key.size() + 256 + (body ? body->size() : 0);
  for (const auto& [name, value] : headers) n += name.size() + value.size() + 4;
  for (const auto& [name, value] : varied) n += name.size() + value.size();
  return n;
}

HttpCache::HttpCache(HttpCacheOptions options, Clock clock)
    : options_(options), clock_(clock ? std::move(clock) : Clock([] { return SystemClock::now(); })) {}

HttpCache::List::iterator HttpCache::FindVariant(const std::string& key, const RequestFields& request) {
  auto found = variants_.find(key);
  if (found == variants_.end()) return lru_.end();
  for (List::iterator it : found->second) {
    bool same = true;
    for (const auto& [name, value] : (*it)->varied) same = same && Combined(request, name) == value;
    if (same) return it;
  }
  return lru_.end();
}

void HttpCache::Insert(std::shared_ptr<const CacheEntry> entry) {
  bytes_ += entry->size();
  lru_.push_front(entry);
  variants_[entry->key].push_back(lru_.begin());
  while (bytes_ > options_.maxBytes && !lru_.empty()) Erase(std::prev(lru_.end()));
}

void HttpCache::Erase(List::iterator it) {
  auto found = variants_.find((*it)->key);
  auto& list = found->second;
  list.erase(std::remove(list.begin(), list.end(), it), list.end());
  if (list.empty()) variants_.erase(found);
  bytes_ -= (*it)->size();
  lru_.erase(it);
}

HttpCache::Found HttpCache::Find(const std::string& key, const RequestFields& request, CacheMode mode) {
  if (mode == CacheMode::NoStore || mode == CacheMode::Reload) return {};
  const auto it = FindVariant(key, request);
  if (it == lru_.end()) return {};

  std::shared_ptr<const CacheEntry> entry = *it;
  lru_.splice(lru_.begin(), lru_, it);  // used now; the iterators held in variants_ stay valid

  if (mode == CacheMode::ForceCache || mode == CacheMode::OnlyIfCached) return {Freshness::Fresh, entry};
  if (mode == CacheMode::NoCache) return {Freshness::Stale, entry};
  const CacheControl control = ParseCacheControl(entry->headers);
  if (control.noCache) return {Freshness::Stale, entry};
  return {FreshnessLifetime(*entry, control) > CurrentAge(*entry, clock_()) ? Freshness::Fresh : Freshness::Stale, entry};
}

bool HttpCache::Storable(const RequestFields& request, const HttpResponseHead& head, CacheMode mode) const {
  if (mode == CacheMode::NoStore) return false;
  // A response to a request that was authorized is for whoever it was authorized.
  if (!Combined(request, "authorization").empty()) return false;
  if (head.status < 200 || head.status == 206 || head.status == 304) return false;

  const CacheControl control = ParseCacheControl(head.headers);
  if (control.noStore || VaryAsterisk(head.headers)) return false;

  CacheEntry probe;
  probe.status = head.status;
  probe.headers = head.headers;
  probe.responseTime = clock_();
  probe.requestTime = probe.responseTime;
  const bool explicitFreshness = control.maxAge || First(head.headers, "expires") || control.isPublic;
  if (!IsCacheableByDefault(head.status) && !explicitFreshness) return false;
  // One that would be stale at once and could not be checked would never be of use.
  return FreshnessLifetime(probe, control) > 0s || !probe.Validators().empty();
}

void HttpCache::Store(const std::string& key, const RequestFields& request, const HttpResponseHead& head, std::string body,
                      SystemClock::time_point requestTime, SystemClock::time_point responseTime) {
  auto entry = std::make_shared<CacheEntry>();
  entry->key = key;
  entry->status = head.status;
  entry->headers = head.headers;
  entry->varied = VariedValues(request, head.headers);
  entry->requestTime = requestTime;
  entry->responseTime = responseTime;
  if (body.size() > options_.maxEntryBytes) return;
  entry->body = std::make_shared<const std::string>(std::move(body));
  if (entry->size() > options_.maxEntryBytes) return;

  if (auto old = FindVariant(key, request); old != lru_.end()) Erase(old);
  Insert(std::move(entry));
}

std::shared_ptr<const CacheEntry> HttpCache::Freshen(const std::shared_ptr<const CacheEntry>& old, const HttpResponseHead& notModified,
                                                     SystemClock::time_point requestTime, SystemClock::time_point responseTime) {
  auto entry = std::make_shared<CacheEntry>(*old);
  entry->requestTime = requestTime;
  entry->responseTime = responseTime;
  // The 304 speaks for the response, but not for what describes its body or the hop it came over.
  static const char* const kKept[] = {"content-encoding", "content-length", "content-range", "content-type", "transfer-encoding", "connection", "keep-alive"};
  for (const auto& [name, value] : notModified.headers) {
    if (std::any_of(std::begin(kKept), std::end(kKept), [&](const char* kept) { return EqualsIgnoreCase(name, kept); })) continue;
    std::erase_if(entry->headers, [&](const auto& header) { return EqualsIgnoreCase(header.first, name); });
  }
  for (const auto& [name, value] : notModified.headers) {
    if (std::any_of(std::begin(kKept), std::end(kKept), [&](const char* kept) { return EqualsIgnoreCase(name, kept); })) continue;
    entry->headers.emplace_back(name, value);
  }

  // It takes the old one's place, or just goes in if the old one has since been let go.
  if (auto it = std::find(lru_.begin(), lru_.end(), old); it != lru_.end()) Erase(it);
  std::shared_ptr<const CacheEntry> result = entry;
  Insert(result);
  return result;
}

void HttpCache::Remove(const std::string& key, const RequestFields& request) {
  if (auto it = FindVariant(key, request); it != lru_.end()) Erase(it);
}

void HttpCache::RemoveAll(const std::string& key) {
  auto found = variants_.find(key);
  if (found == variants_.end()) return;
  for (List::iterator it : std::vector<List::iterator>(found->second)) Erase(it);
}

}  // namespace solar::net
