#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "solar/net/ContentDecoder.h"
#include "solar/net/Http1Parser.h"
#include "solar/net/Loop.h"
#include "solar/net/Tls.h"
#include "solar/url/Url.h"

namespace solar::net {

class FetchHandler {
 public:
  virtual ~FetchHandler() = default;

  // `finalUrl` is where the response came from after any redirects, with the fragment the
  // standard carries across them. `head` is the response as the server sent it, so a body that
  // was compressed still lists its Content-Encoding and Content-Length; OnBody delivers it decoded.
  virtual void OnResponseHead(const HttpResponseHead& head, const url::Url& finalUrl) = 0;
  // A view into the buffer the kernel filled, valid only until this returns.
  virtual void OnBody(std::span<const uint8_t> data) = 0;
  // Exactly one of OnEnd and OnError is called, and it is the last call.
  virtual void OnEnd() = 0;
  virtual void OnError(std::string_view message) = 0;
};

struct FetchOptions {
  // For the whole fetch, redirects included.
  std::chrono::milliseconds timeout{30000};
  // For https. Null means the shared context that trusts the system's roots.
  std::shared_ptr<TlsContext> tls;
  // Sent with every request. Host, Content-Length, Transfer-Encoding, Connection and Upgrade are
  // the connection's to decide and are refused; a name or value that could break the request into
  // two is refused too. Authorization is dropped when a redirect leaves the origin.
  std::vector<std::pair<std::string, std::string>> headers;
  int maxRedirects = 20;
  // The most a compressed body may decode to. It is what stops a few kilobytes from becoming
  // gigabytes; a body that is not compressed is not limited by it.
  uint64_t maxDecodedBodyBytes = uint64_t{1} << 30;
};

struct HttpClientOptions {
  // How long a connection waits in the pool for another request before it is closed.
  std::chrono::milliseconds idleTimeout{60000};
};

class HttpClient;

class FetchHandle {
 public:
  FetchHandle() = default;
  // Ends the fetch with OnError("aborted") and closes its connection, unless it has already
  // finished. Safe to call again, and safe once the fetch is over.
  void Cancel();

 private:
  friend class HttpClient;
  FetchHandle(HttpClient* client, uint64_t id) : client_(client), id_(id) {}
  HttpClient* client_ = nullptr;
  uint64_t id_ = 0;
};

// Fetches over HTTP/1.1 (through TLS for https), following redirects as the Fetch Standard says,
// and keeps connections between requests. It must outlive the Loop::Run calls that serve its
// fetches, and be destroyed before the Loop.
class HttpClient {
 public:
  explicit HttpClient(Loop& loop, HttpClientOptions options = {});
  ~HttpClient();

  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  // A GET. Everything is reported from Loop::Run, never from inside this call. `handler` must
  // outlive the fetch.
  FetchHandle Fetch(const url::Url& url, FetchHandler& handler, FetchOptions options = {});

  // Closes the connections waiting in the pool.
  void CloseIdleConnections();

  struct Impl;

 private:
  friend class FetchHandle;
  std::unique_ptr<Impl> impl_;
};

}  // namespace solar::net
