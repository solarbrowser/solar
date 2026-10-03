#include "solar/net/Resolver.h"

#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "solar/net/Socket.h"

namespace solar::net {

namespace {

ResolveResult SystemLookup(const std::string& host, uint16_t port, SocketAddress::Family family) {
  InitializeSockets();
  addrinfo hints{};
  hints.ai_family = family == SocketAddress::Family::IPv6 ? AF_INET6 : AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICSERV;
  addrinfo* results = nullptr;
  const int code = ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &results);

  ResolveResult result;
  if (code != 0) {
#ifdef _WIN32
    result.error = "cannot resolve " + host + ": " + ErrorMessage(code);
#else
    result.error = "cannot resolve " + host + ": " + ::gai_strerror(code);
#endif
    return result;
  }
  for (addrinfo* entry = results; entry; entry = entry->ai_next) {
    SocketAddress address;
    address.port = port;
    if (entry->ai_family == AF_INET6) {
      address.family = SocketAddress::Family::IPv6;
      std::memcpy(address.bytes.data(), &reinterpret_cast<sockaddr_in6*>(entry->ai_addr)->sin6_addr, 16);
    } else if (entry->ai_family == AF_INET) {
      address.family = SocketAddress::Family::IPv4;
      std::memcpy(address.bytes.data(), &reinterpret_cast<sockaddr_in*>(entry->ai_addr)->sin_addr, 4);
    } else {
      continue;
    }
    result.addresses.push_back(address);
  }
  ::freeaddrinfo(results);
  return result;
}

// An address written out: 127.0.0.1, ::1 or [::1]. Needs no lookup.
bool ParseLiteral(const std::string& host, uint16_t port, SocketAddress& out) {
  std::string name = host;
  if (name.size() >= 2 && name.front() == '[' && name.back() == ']') name = name.substr(1, name.size() - 2);
  out = SocketAddress{};
  out.port = port;
  if (::inet_pton(AF_INET, name.c_str(), out.bytes.data()) == 1) {
    out.family = SocketAddress::Family::IPv4;
    return true;
  }
  if (::inet_pton(AF_INET6, name.c_str(), out.bytes.data()) == 1) {
    out.family = SocketAddress::Family::IPv6;
    return true;
  }
  return false;
}

class SystemResolver final : public Resolver {
 public:
  SystemResolver(Loop& loop, SystemResolverOptions options)
      : loop_(loop), options_(std::move(options)), shared_(std::make_shared<Shared>(loop, options_.cacheTtl)) {
    const size_t count = options_.threads == 0 ? 1 : options_.threads;
    for (size_t i = 0; i < count; ++i) workers_.emplace_back([this] { Work(); });
  }

  ~SystemResolver() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    wake_.notify_all();
    for (std::thread& worker : workers_) worker.join();

    // Lookups that were never answered are given up, so Run does not wait for them for ever.
    for (size_t i = 0; i < shared_->requests.size(); ++i) loop_.EndExternalWork();
    shared_->requests.clear();
    shared_->alive = false;
  }

  RequestId Resolve(const std::string& host, uint16_t port, SocketAddress::Family family, Callback callback) override {
    const RequestId id = nextId_++;

    SocketAddress literal;
    if (ParseLiteral(host, port, literal)) {
      ResolveResult result;
      if (literal.family == family) result.addresses.push_back(literal);
      DeliverLater(id, std::move(callback), std::move(result));
      return id;
    }

    const std::string key = host + (family == SocketAddress::Family::IPv6 ? "/6" : "/4") + ":" + std::to_string(port);
    auto cached = shared_->cache.find(key);
    if (cached != shared_->cache.end()) {
      if (cached->second.expires > std::chrono::steady_clock::now()) {
        DeliverLater(id, std::move(callback), ResolveResult{cached->second.addresses, ""});
        return id;
      }
      shared_->cache.erase(cached);
    }

    loop_.BeginExternalWork();
    shared_->requests.emplace(id, Request{std::move(callback), key});
    {
      std::lock_guard<std::mutex> lock(mutex_);
      jobs_.push_back(Job{id, host, port, family});
    }
    wake_.notify_one();
    return id;
  }

  void Cancel(RequestId id) override {
    auto it = shared_->requests.find(id);
    if (it == shared_->requests.end()) return;
    shared_->requests.erase(it);
    loop_.EndExternalWork();
  }

 private:
  struct Job {
    RequestId id;
    std::string host;
    uint16_t port;
    SocketAddress::Family family;
  };

  struct Request {
    Callback callback;
    std::string cacheKey;
  };

  struct CacheEntry {
    std::vector<SocketAddress> addresses;
    std::chrono::steady_clock::time_point expires;
  };

  // What a completion posted to the loop looks at. It is shared so that one that runs after the
  // resolver is gone finds `alive` false and does nothing.
  struct Shared {
    Shared(Loop& l, std::chrono::milliseconds ttl) : loop(l), ttl(ttl) {}
    Loop& loop;
    std::chrono::milliseconds ttl;
    bool alive = true;
    std::unordered_map<RequestId, Request> requests;
    std::unordered_map<std::string, CacheEntry> cache;
  };

  // Answers that need no lookup still come back from Run, as every answer does.
  void DeliverLater(RequestId, Callback callback, ResolveResult result) {
    loop_.PostDelayed(std::chrono::milliseconds(0), [callback = std::move(callback), result = std::move(result)]() mutable {
      callback(std::move(result));
    });
  }

  static void Complete(const std::shared_ptr<Shared>& shared, RequestId id, ResolveResult result) {
    if (!shared->alive) return;
    auto it = shared->requests.find(id);
    if (it == shared->requests.end()) return;  // cancelled meanwhile
    Callback callback = std::move(it->second.callback);
    const std::string key = std::move(it->second.cacheKey);
    shared->requests.erase(it);
    shared->loop.EndExternalWork();

    if (result.error.empty() && !result.addresses.empty()) {
      constexpr size_t kMaxCached = 512;
      if (shared->cache.size() >= kMaxCached) shared->cache.clear();
      shared->cache[key] = CacheEntry{result.addresses, std::chrono::steady_clock::now() + shared->ttl};
    }
    callback(std::move(result));
  }

  void Work() {
    while (true) {
      Job job;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
        if (stopping_) return;
        job = std::move(jobs_.front());
        jobs_.pop_front();
      }
      ResolveResult result = options_.lookup ? options_.lookup(job.host, job.port, job.family) : SystemLookup(job.host, job.port, job.family);
      loop_.Post([shared = shared_, id = job.id, result = std::move(result)]() mutable { Complete(shared, id, std::move(result)); });
    }
  }

  Loop& loop_;
  SystemResolverOptions options_;
  std::shared_ptr<Shared> shared_;
  RequestId nextId_ = 1;

  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Job> jobs_;
  bool stopping_ = false;
  std::vector<std::thread> workers_;
};

}  // namespace

std::unique_ptr<Resolver> MakeSystemResolver(Loop& loop, SystemResolverOptions options) {
  return std::make_unique<SystemResolver>(loop, std::move(options));
}

}  // namespace solar::net
