#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "solar/net/Http1Parser.h"

namespace solar::net {

// How a fetch uses the cache, as the Fetch Standard names the choices.
enum class CacheMode {
  Default,       // a fresh response is used; a stale one is checked with the server first
  NoStore,       // neither looked at nor added to
  Reload,        // not looked at, but what comes back is kept
  NoCache,       // always checked with the server first
  ForceCache,    // any response there is, stale or not; the network only if there is none
  OnlyIfCached,  // the same, and an error rather than the network
};

using RequestFields = std::vector<std::pair<std::string, std::string>>;

// A response as it was kept. Never changed once made; a revalidation makes a new one.
struct CacheEntry {
  std::string key;
  int status = 0;
  std::vector<std::pair<std::string, std::string>> headers;
  std::shared_ptr<const std::string> body;  // as it came, still in its content coding
  // The request headers the response varies on (its Vary header), lower case, and what they were.
  RequestFields varied;
  std::chrono::system_clock::time_point requestTime;   // when the request that got it was sent
  std::chrono::system_clock::time_point responseTime;  // when its head arrived

  HttpResponseHead Head() const;
  // What a request to check this with the server carries: If-None-Match and If-Modified-Since,
  // for those of ETag and Last-Modified that the response has.
  RequestFields Validators() const;
  size_t size() const;
};

struct HttpCacheOptions {
  size_t maxBytes = size_t{64} << 20;
  // A response larger than this is not kept, however much room there is.
  size_t maxEntryBytes = size_t{8} << 20;
};

// A private HTTP cache (RFC 9111) for GET responses, in memory, least recently used out first.
class HttpCache {
 public:
  using Clock = std::function<std::chrono::system_clock::time_point()>;

  // `clock` is for tests; the system's by default.
  explicit HttpCache(HttpCacheOptions options = HttpCacheOptions(), Clock clock = nullptr);

  enum class Freshness { Miss, Fresh, Stale };
  struct Found {
    Freshness freshness = Freshness::Miss;
    std::shared_ptr<const CacheEntry> entry;
  };

  // What the cache has for a GET to `key` that is sent with `request`'s header fields.
  Found Find(const std::string& key, const RequestFields& request, CacheMode mode);

  // Whether a response is to be kept, asked before its body is collected.
  bool Storable(const RequestFields& request, const HttpResponseHead& head, CacheMode mode) const;
  // Keeps a response that was Storable and has arrived whole. Does nothing if it turns out too big.
  void Store(const std::string& key, const RequestFields& request, const HttpResponseHead& head, std::string body,
             std::chrono::system_clock::time_point requestTime, std::chrono::system_clock::time_point responseTime);
  // A 304 answered the check of `old`: its fields are brought up to date, it is fresh again, and
  // the entry that now stands in the cache for it is returned.
  std::shared_ptr<const CacheEntry> Freshen(const std::shared_ptr<const CacheEntry>& old, const HttpResponseHead& notModified,
                                            std::chrono::system_clock::time_point requestTime, std::chrono::system_clock::time_point responseTime);
  // The server said a response is not to be kept, so one kept before is let go.
  void Remove(const std::string& key, const RequestFields& request);

  // A response over this is not kept, so there is no point gathering its body.
  size_t maxEntryBytes() const { return options_.maxEntryBytes; }
  size_t entries() const { return lru_.size(); }
  size_t bytes() const { return bytes_; }
  std::chrono::system_clock::time_point Now() const { return clock_(); }

 private:
  using List = std::list<std::shared_ptr<const CacheEntry>>;

  List::iterator FindVariant(const std::string& key, const RequestFields& request);
  void Insert(std::shared_ptr<const CacheEntry> entry);
  void Erase(List::iterator it);

  HttpCacheOptions options_;
  Clock clock_;
  List lru_;  // most recently used first
  std::unordered_map<std::string, std::vector<List::iterator>> variants_;
  size_t bytes_ = 0;
};

}  // namespace solar::net
