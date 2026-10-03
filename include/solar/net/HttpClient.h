#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "solar/net/ContentDecoder.h"
#include "solar/net/Http1Parser.h"
#include "solar/net/Cookies.h"
#include "solar/net/Hsts.h"
#include "solar/net/Loop.h"
#include "solar/net/Resolver.h"
#include "solar/net/Tls.h"
#include "solar/net/UserAgent.h"
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
  // Whether the request carries the jar's cookies and the responses to it may set some. The Cookie
  // header is the client's to write, like Host, and is refused in `headers`.
  bool useCookies = true;
  // The page on whose behalf this is fetched. A request to a site other than the page's, or that
  // is redirected through one, is cross-site, and SameSite cookies are held back from it. Unset
  // means the user went there themselves, which is never cross-site.
  std::optional<url::Url> initiator;
  // The request loads a page into the top-level browsing context, where Lax cookies go cross-site.
  bool topLevelNavigation = false;
  // The most a compressed body may decode to. It is what stops a few kilobytes from becoming
  // gigabytes; a body that is not compressed is not limited by it.
  uint64_t maxDecodedBodyBytes = uint64_t{1} << 30;
};

struct HttpClientOptions {
  // How long a connection waits in the pool for another request before it is closed.
  std::chrono::milliseconds idleTimeout{60000};
  // Sent with every request unless a fetch gives its own User-Agent header. Where this comes from
  // is not the network layer's business: a settings file or the user's choice sets it.
  std::string userAgent = DefaultUserAgent();
  // Looks names up. When there is none the client makes a system one of its own; a caller that
  // wants the cache shared with others, or answers it controls, gives its own.
  std::shared_ptr<Resolver> resolver;
  // Offer HTTP/2 to https servers. When false only HTTP/1.1 is offered, one request to a connection.
  bool http2 = true;
  // The hosts that have asked to be reached over https only, whose http URLs are changed to https
  // before anything is sent. When there is none the client keeps one for its own lifetime.
  std::shared_ptr<HstsStore> hsts;
  // The cookies requests carry and responses set. When there is none the client keeps one for its
  // own lifetime.
  std::shared_ptr<CookieJar> cookies;
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
