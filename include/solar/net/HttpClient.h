/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

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
#include "solar/net/HttpCache.h"
#include "solar/net/Loop.h"
#include "solar/net/Resolver.h"
#include "solar/net/Tls.h"
#include "solar/net/WebSocketTypes.h"
#include "solar/net/UserAgent.h"
#include "solar/url/Url.h"

namespace solar::net {

class FetchHandler {
 public:
  virtual ~FetchHandler() = default;

  // The server redirected to `next` and the fetch is about to follow, with redirect mode Follow.
  // `redirect` is the response that did it. `headers` are the request's headers for the next hop, to
  // change as the embedder's rules want (a cross-origin redirect turns Origin into null, say). False
  // stops the fetch, which then ends with OnError.
  virtual bool OnRedirect(const HttpResponseHead& redirect, const url::Url& next, std::vector<std::pair<std::string, std::string>>& headers) {
    (void)redirect;
    (void)next;
    (void)headers;
    return true;
  }
  // A handler that cannot answer OnRedirect at once (it is in another thread, or another process, from the fetch) says so here, and is told of the redirect by OnReceiveRedirect
  // instead, with the headers the next hop would carry; the fetch waits, and goes on when the answer comes by NetworkService::FollowRedirect (the headers it gives are the
  // next hop's), or ends with an error when the answer is no. Nothing else of the fetch waits with it.
  virtual bool DefersRedirects() const { return false; }
  virtual void OnReceiveRedirect(const HttpResponseHead& redirect, const url::Url& next, const std::vector<std::pair<std::string, std::string>>& headers) {
    (void)redirect;
    (void)next;
    (void)headers;
  }

  // `finalUrl` is where the response came from after any redirects, with the fragment the
  // standard carries across them. `head` is the response as the server sent it, so a body that
  // was compressed still lists its Content-Encoding and Content-Length; OnBody delivers it decoded.
  virtual void OnResponseHead(const HttpResponseHead& head, const url::Url& finalUrl) = 0;
  // A view into the buffer the kernel filled, valid only until this returns.
  virtual void OnBody(std::span<const uint8_t> data) = 0;
  // Exactly one of OnEnd and OnError is called, and it is the last call.
  virtual void OnEnd() = 0;
  virtual void OnError(std::string_view message) = 0;
  // The client wants to ask for the response again, from the start, because it stopped coming (no byte for a while). A handler that keeps what it was given in one place and
  // can take it away says true after it has (OnResponseHead and OnBody come again); the default keeps what the handler has seen as it is: the fetch goes on to its end or its timeout.
  virtual bool OnRestart() { return false; }
  // A handler that cannot answer at once whether the response may be asked for again (it is in another thread, or process) says so, and is told by OnRestartRequested when the
  // response stalls; the fetch waits (nothing else does) for NetworkService::AnswerRestart, and asks again if it is allowed (the handler has then dropped what it had, as OnRestart
  // has it do), or goes on as it was if not.
  virtual bool DefersRestarts() const { return false; }
  virtual void OnRestartRequested() {}
};

// What a redirect response does to a fetch (Fetch Standard, request's redirect mode).
enum class RedirectMode {
  Follow,  // asked for again at the new address, up to maxRedirects times
  Error,   // the fetch fails
  Manual,  // the redirect response itself is the response
};

struct FetchOptions {
  // GET, HEAD, POST, PUT, DELETE, OPTIONS and PATCH are the ones to use. get, post, head, delete,
  // options and put are written in capitals as the Fetch Standard has it; any other name is sent
  // as it is. CONNECT, TRACE and TRACK are refused, and so is anything that is not a token.
  std::string method = "GET";
  // The request body. It is shared and must not change while the fetch runs, because a redirect
  // that keeps the method (307, 308) and a retry send it again. GET and HEAD have none. A POST or
  // PUT without one says Content-Length: 0. Content-Type is the caller's to give in `headers`.
  std::shared_ptr<const std::string> body;
  // For the whole fetch, redirects included.
  std::chrono::milliseconds timeout{30000};
  // How long a response may be without a byte before the client asks for it again (twice at most), for a handler that takes that (see FetchHandler::OnRestart).
  std::chrono::milliseconds stall{8000};
  // For https. Null means the shared context that trusts the system's roots.
  std::shared_ptr<TlsContext> tls;
  // Sent with every request. Host, Content-Length, Transfer-Encoding, Connection and Upgrade are
  // the connection's to decide and are refused; a name or value that could break the request into
  // two is refused too. Authorization is dropped when a redirect leaves the origin.
  std::vector<std::pair<std::string, std::string>> headers;
  int maxRedirects = 20;
  RedirectMode redirect = RedirectMode::Follow;
  // Whether the request carries the jar's cookies and the responses to it may set some. The Cookie
  // header is the client's to write, like Host, and is refused in `headers`.
  bool useCookies = true;
  // The page on whose behalf this is fetched. A request to a site other than the page's, or that
  // is redirected through one, is cross-site, and SameSite cookies are held back from it. Unset
  // means the user went there themselves, which is never cross-site.
  std::optional<url::Url> initiator;
  // The request loads a page into the top-level browsing context, where Lax cookies go cross-site.
  bool topLevelNavigation = false;
  // How the cache is used. Conditional headers in `headers`, and a Cache-Control or Pragma that
  // says no-cache, take it away from Default as the Fetch Standard has it.
  CacheMode cache = CacheMode::Default;
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
  // The responses kept for later requests. When there is none the client keeps one of its own.
  std::shared_ptr<HttpCache> cache;
};

class NetworkService;
class HttpClient;
class WebSocketClient;
struct PageRequest;
class PageFetchHandler;
class PreflightCache;

// What a fetch in flight is held by: Cancel() ends it with OnError("aborted") and closes its connection, unless it has already finished. Safe to call again, and safe once the
// fetch is over.
class FetchHandle {
 public:
  FetchHandle() = default;
  void Cancel();
  // The number the service knows the fetch by.
  uint64_t id() const { return id_; }

 private:
  friend class NetworkService;
  friend class HttpClient;
  FetchHandle(NetworkService* service, uint64_t id) : service_(service), id_(id) {}
  NetworkService* service_ = nullptr;
  uint64_t id_ = 0;
};

// What the pages ask the network for: the one thing that stands between them and the connections, the cookies, the cache and the rules of fetching. Everything a page needs of the
// network goes through it, as messages in one direction (the request: a url, a handler to hear of the answer, and the options, which are data) and in the other (the answer, which
// the handler is told of from the loop the call was made from, never from inside the call). It is what the network will be behind when it is a thread of its own, and then a process.
class NetworkService {
 public:
  virtual ~NetworkService() = default;
  // A fetch. `handler` must outlive it.
  virtual FetchHandle Fetch(const url::Url& url, FetchHandler& handler, FetchOptions options = {}) = 0;
  // A fetch as a page asks for one (a Fetch Standard request): the service applies the rules of fetching to it and tells the handler what the page may be told. `handler` must outlive it.
  virtual FetchHandle StartFetch(PageRequest request, PageFetchHandler& handler) = 0;
  // Ends the fetch with that number (what FetchHandle::Cancel does).
  virtual void Cancel(uint64_t id) = 0;
  // The answer to OnReceiveRedirect of the fetch with that number: go on to the redirect's address with `headers`, or (`follow` false) end the fetch.
  virtual void FollowRedirect(uint64_t id, bool follow, std::vector<std::pair<std::string, std::string>> headers = {}) = 0;
  // The answer to OnRestartRequested of the fetch with that number.
  virtual void AnswerRestart(uint64_t id, bool allow) = 0;
  // Closes the connections waiting in the pool.
  virtual void CloseIdleConnections() = 0;
  // A WebSocket connection. `handler` must outlive it, which ends with its OnClosed.
  virtual WebSocketHandle ConnectWebSocket(const url::Url& url, WebSocketHandler& handler, WebSocketOptions options = {}) = 0;
  // What the handle of a connection asks of it.
  virtual void SendWebSocket(uint64_t id, std::string data, bool binary) = 0;
  virtual void CloseWebSocket(uint64_t id, uint16_t code, std::string reason) = 0;
  virtual void AbortWebSocket(uint64_t id) = 0;

  // What a service that makes the handles of its own needs: the handle is made by the service, which is the only one to know what a number means.
 protected:
  static FetchHandle MakeHandle(NetworkService* service, uint64_t id) { return FetchHandle(service, id); }
  static WebSocketHandle MakeWebSocketHandle(NetworkService* service, uint64_t id) { return WebSocketHandle(service, id); }
};

// Fetches over HTTP/1.1 (through TLS for https), following redirects as the Fetch Standard says,
// and keeps connections between requests. It must outlive the Loop::Run calls that serve its
// fetches, and be destroyed before the Loop.
class HttpClient : public NetworkService {
 public:
  explicit HttpClient(Loop& loop, HttpClientOptions options = {});
  ~HttpClient() override;

  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  // A GET. Everything is reported from Loop::Run, never from inside this call. `handler` must
  // outlive the fetch.
  FetchHandle Fetch(const url::Url& url, FetchHandler& handler, FetchOptions options = {}) override;
  FetchHandle StartFetch(PageRequest request, PageFetchHandler& handler) override;
  void Cancel(uint64_t id) override;
  void FollowRedirect(uint64_t id, bool follow, std::vector<std::pair<std::string, std::string>> headers = {}) override;
  void AnswerRestart(uint64_t id, bool allow) override;
  WebSocketHandle ConnectWebSocket(const url::Url& url, WebSocketHandler& handler, WebSocketOptions options = {}) override;
  void SendWebSocket(uint64_t id, std::string data, bool binary) override;
  void CloseWebSocket(uint64_t id, uint16_t code, std::string reason) override;
  void AbortWebSocket(uint64_t id) override;

  // Closes the connections waiting in the pool.
  void CloseIdleConnections() override;

  // What the client shares with whatever else makes connections for the same browser (WebSocket): the loop, the lookups, the cookies, the sites that
  // are https only, and the User-Agent it sends.
  Loop& loop() const;
  std::shared_ptr<Resolver> resolver() const;
  std::shared_ptr<CookieJar> cookies() const;
  std::shared_ptr<HstsStore> hsts() const;
  const std::string& userAgent() const;

  struct Impl;

 private:
  friend class FetchHandle;
  WebSocketClient& WebSockets();
  std::unique_ptr<Impl> impl_;
  std::unique_ptr<WebSocketClient> websockets_;  // (made when the first connection is asked for)
};

}  // namespace solar::net
