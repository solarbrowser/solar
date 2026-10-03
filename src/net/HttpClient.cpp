#include "solar/net/HttpClient.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <list>
#include <optional>
#include <unordered_map>

#include "Socket.h"
#include "solar/url/Origin.h"
#include "solar/url/Parser.h"
#include "solar/url/Serializer.h"
#include "solar/url/UrlApi.h"
#include "solar/url/Utf8.h"

namespace solar::net {

namespace internal {
struct Exchange;
struct HttpConnection;
}  // namespace internal

struct HttpClient::Impl {
  Loop& loop;
  HttpClientOptions options;
  uint64_t nextId = 1;
  std::unordered_map<uint64_t, std::unique_ptr<internal::Exchange>> exchanges;
  std::list<std::unique_ptr<internal::HttpConnection>> connections;
  std::unordered_map<std::string, std::vector<internal::HttpConnection*>> idle;
  bool reapQueued = false;

  Impl(Loop& l, HttpClientOptions o) : loop(l), options(o) {}
  ~Impl();

  internal::HttpConnection* TakeIdle(const std::string& key);
  void Release(internal::HttpConnection* connection);
  void RemoveIdle(internal::HttpConnection* connection);
  void ScheduleReap();
  void Reap();
};

namespace {

// Blocks while the system resolver runs. This stands in until DNS moves off the loop's thread.
bool Resolve(const std::string& host, uint16_t port, std::vector<SocketAddress>& out, std::string& error) {
  InitializeSockets();
  std::string name = host;
  if (name.size() >= 2 && name.front() == '[' && name.back() == ']') name = name.substr(1, name.size() - 2);

  addrinfo hints{};
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICSERV;
  addrinfo* results = nullptr;
  const int code = ::getaddrinfo(name.c_str(), std::to_string(port).c_str(), &hints, &results);
  if (code != 0) {
#ifdef _WIN32
    error = std::string("cannot resolve ") + host + ": " + ErrorMessage(code);
#else
    error = std::string("cannot resolve ") + host + ": " + ::gai_strerror(code);
#endif
    return false;
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
    out.push_back(address);
  }
  ::freeaddrinfo(results);
  if (out.empty()) {
    error = "no usable address for " + host;
    return false;
  }
  return true;
}

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 0x20) : c; }

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return Lower(x) == Lower(y); });
}

bool IsTokenChar(char c) {
  if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
  return std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
}

// Why a header may not be sent, or empty when it may.
std::string RefuseHeader(const std::string& name, const std::string& value) {
  if (name.empty() || !std::all_of(name.begin(), name.end(), IsTokenChar)) return "invalid header name";
  for (char c : value) {
    const unsigned char u = static_cast<unsigned char>(c);
    if ((u < 0x20 && u != '\t') || u == 0x7F) return "invalid header value";
  }
  for (const char* forbidden : {"host", "content-length", "transfer-encoding", "connection", "upgrade", "accept-encoding"}) {
    if (EqualsIgnoreCase(name, forbidden)) return "the " + name + " header is not the caller's to set";
  }
  return "";
}

// The Fetch Standard decodes a Location header as Latin-1 before parsing it as a URL.
std::string IsomorphicDecode(std::string_view bytes) {
  std::string out;
  for (char c : bytes) url::AppendUtf8(out, static_cast<unsigned char>(c));
  return out;
}

bool IsRedirectStatus(int status) { return status == 301 || status == 302 || status == 303 || status == 307 || status == 308; }

bool ConnectionHeaderSaysClose(const HttpResponseHead& head) {
  for (const auto& [name, value] : head.headers) {
    if (!EqualsIgnoreCase(name, "connection")) continue;
    size_t start = 0;
    while (start <= value.size()) {
      size_t end = value.find(',', start);
      if (end == std::string::npos) end = value.size();
      std::string_view token(value.data() + start, end - start);
      while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) token.remove_prefix(1);
      while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) token.remove_suffix(1);
      if (EqualsIgnoreCase(token, "close")) return true;
      start = end + 1;
    }
  }
  return false;
}

}  // namespace

namespace internal {

// A connection that outlives one request. It is the handler of the connection below it (or of its
// TLS layer) and passes what happens to whichever exchange is using it, if any.
struct HttpConnection : ConnectionHandler {
  HttpClient::Impl& impl;
  std::string key;
  std::string tlsName;  // empty for plain HTTP
  std::shared_ptr<TlsContext> tlsContext;
  std::vector<SocketAddress> addresses;
  size_t nextAddress = 0;

  std::unique_ptr<TlsLayer> tls;
  // A layer that has called OnClosed is still on the stack; it lives until this connection does.
  std::vector<std::unique_ptr<TlsLayer>> retired;
  Connection* connection = nullptr;
  Transport* transport = nullptr;

  Exchange* exchange = nullptr;
  bool established = false;
  bool dead = false;
  bool idle = false;
  Loop::TimerId idleTimer = 0;

  explicit HttpConnection(HttpClient::Impl& i) : impl(i) {}

  void ConnectNext();
  void Close() {
    if (transport) transport->Close();
  }

  void OnConnected() override;
  void OnData(std::span<const uint8_t> data) override;
  void OnClosed(int error) override;
};

// One fetch: a request and the redirects that follow it, each a hop on some connection.
struct Exchange : Http1ResponseParser::Sink {
  HttpClient::Impl& impl;
  const uint64_t id;
  FetchHandler& handler;
  FetchOptions options;
  url::Url current;
  int redirects = 0;

  HttpConnection* conn = nullptr;
  std::unique_ptr<Http1ResponseParser> parser;
  std::unique_ptr<BodyDecoder> decoder;  // null when the body is delivered as it came
  std::string request;
  bool finished = false;
  bool reusedConnection = false;
  bool staleRetried = false;
  bool anyResponseByte = false;
  bool hopComplete = false;
  bool redirecting = false;
  std::optional<std::string> location;
  std::optional<HttpResponseHead> head;
  Loop::TimerId timer = 0;

  Exchange(HttpClient::Impl& i, uint64_t identifier, FetchHandler& h, FetchOptions o, const url::Url& u)
      : impl(i), id(identifier), handler(h), options(std::move(o)), current(u) {}

  void Begin();
  void StartHop(bool forceFresh);
  std::string BuildRequest() const;
  void EndHop(bool clean);
  void FollowRedirect();
  void Fail(const std::string& message);
  void Finish();

  void OnConnectionReady();
  void OnData(std::span<const uint8_t> data);
  void OnConnectionClosed(int error, const std::string& tlsFailure);

  void OnHead(const HttpResponseHead& h) override {
    if (finished) return;
    head = h;
    if (IsRedirectStatus(h.status)) {
      if (auto target = h.Header("location")) {
        location = std::string(*target);
        redirecting = true;  // the redirect response itself is not shown to the handler
        return;
      }
    }
    decoder = BodyDecoder::Create(h, options.maxDecodedBodyBytes);
    handler.OnResponseHead(h, current);
  }
  void OnBody(std::span<const uint8_t> data) override {
    if (finished || redirecting) return;
    if (!decoder) {
      handler.OnBody(data);
      return;
    }
    // A handler that finished the fetch from inside OnBody ends the decoding too.
    if (auto error = decoder->Decode(data, [this](std::span<const uint8_t> decoded) {
          handler.OnBody(decoded);
          return !finished;
        })) {
      Fail(*error);
    }
  }
  void OnComplete() override {
    if (finished) return;
    if (decoder) {
      // The body has ended, and a compressed one must have ended with it.
      if (auto error = decoder->Finish([this](std::span<const uint8_t> decoded) {
            handler.OnBody(decoded);
            return !finished;
          })) {
        Fail(*error);
        return;
      }
    }
    hopComplete = true;
  }
};

void HttpConnection::ConnectNext() {
  const SocketAddress& address = addresses[nextAddress++];
  if (tlsName.empty()) {
    connection = impl.loop.Connect(address, this);
    transport = connection;
    return;
  }
  if (tls) retired.push_back(std::move(tls));
  tls = std::make_unique<TlsLayer>(tlsContext, tlsName, *this);
  connection = impl.loop.Connect(address, tls.get());
  tls->Attach(connection);
  transport = tls.get();
}

void HttpConnection::OnConnected() {
  established = true;
  if (exchange) exchange->OnConnectionReady();
}

void HttpConnection::OnData(std::span<const uint8_t> data) {
  if (exchange) {
    exchange->OnData(data);
  } else {
    Close();  // nobody asked for it: whatever this is, the connection can no longer be trusted
  }
}

void HttpConnection::OnClosed(int error) {
  transport = nullptr;
  connection = nullptr;
  const bool tlsFailed = tls && !tls->failure().empty();
  if (exchange && !established && !tlsFailed && error != 0 && nextAddress < addresses.size()) {
    ConnectNext();  // this address could not be reached; try the next
    return;
  }

  dead = true;
  if (idleTimer != 0) {
    impl.loop.CancelTimer(idleTimer);
    idleTimer = 0;
  }
  if (idle) impl.RemoveIdle(this);
  const std::string tlsFailure = tls ? tls->failure() : std::string();
  if (exchange) {
    Exchange* user = exchange;
    exchange = nullptr;
    user->OnConnectionClosed(error, tlsFailure);
  }
  impl.ScheduleReap();
}

void Exchange::Begin() {
  if (current.scheme != "http" && current.scheme != "https") {
    Fail("unsupported scheme: " + current.scheme);
    return;
  }
  if (!current.host || current.host->empty()) {
    Fail("the URL has no host");
    return;
  }
  for (const auto& [name, value] : options.headers) {
    const std::string reason = RefuseHeader(name, value);
    if (!reason.empty()) {
      Fail(reason);
      return;
    }
  }
  timer = impl.loop.PostDelayed(options.timeout, [this] {
    timer = 0;
    Fail("timed out");
  });
  StartHop(false);
}

std::string Exchange::BuildRequest() const {
  std::string target = url::SerializePath(current);
  if (current.query) target += "?" + *current.query;

  std::string out = "GET " + target + " HTTP/1.1\r\nHost: " + url::GetHost(current) + "\r\n";
  bool hasUserAgent = false;
  bool hasAccept = false;
  for (const auto& [name, value] : options.headers) {
    hasUserAgent = hasUserAgent || EqualsIgnoreCase(name, "user-agent");
    hasAccept = hasAccept || EqualsIgnoreCase(name, "accept");
    out += name + ": " + value + "\r\n";
  }
  if (!hasUserAgent) out += "User-Agent: Solar\r\n";
  if (!hasAccept) out += "Accept: */*\r\n";
  out += "Accept-Encoding: gzip, deflate, br, zstd\r\n\r\n";
  return out;
}

void Exchange::StartHop(bool forceFresh) {
  hopComplete = false;
  redirecting = false;
  location.reset();
  head.reset();
  decoder.reset();
  anyResponseByte = false;
  reusedConnection = false;
  parser = std::make_unique<Http1ResponseParser>(*this);
  request = BuildRequest();

  const bool secure = current.scheme == "https";
  const uint16_t port = current.port.value_or(secure ? 443 : 80);
  std::shared_ptr<TlsContext> context;
  if (secure) context = options.tls ? options.tls : DefaultTlsContext();
  std::string key = current.scheme + "://" + *current.host + ":" + std::to_string(port);
  if (secure) key += "|" + std::to_string(reinterpret_cast<uintptr_t>(context.get()));

  if (!forceFresh) {
    if (HttpConnection* pooled = impl.TakeIdle(key)) {
      conn = pooled;
      conn->exchange = this;
      reusedConnection = true;
      conn->transport->Send(std::move(request));
      return;
    }
  }

  std::vector<SocketAddress> addresses;
  std::string error;
  if (!Resolve(*current.host, port, addresses, error)) {
    Fail(error);
    return;
  }
  auto fresh = std::make_unique<HttpConnection>(impl);
  conn = fresh.get();
  conn->key = std::move(key);
  conn->tlsContext = std::move(context);
  conn->addresses = std::move(addresses);
  if (secure) {
    conn->tlsName = *current.host;
    if (conn->tlsName.size() >= 2 && conn->tlsName.front() == '[') conn->tlsName = conn->tlsName.substr(1, conn->tlsName.size() - 2);
  }
  conn->exchange = this;
  impl.connections.push_back(std::move(fresh));
  conn->ConnectNext();
}

void Exchange::OnConnectionReady() {
  if (finished) return;
  conn->transport->Send(std::move(request));
}

void Exchange::OnData(std::span<const uint8_t> data) {
  if (finished) return;
  anyResponseByte = true;
  const std::optional<std::string> error = parser->Feed(data);
  if (finished) return;  // a decoding failure, or a handler that cancelled, ended it from inside Feed
  if (hopComplete) {
    hopComplete = false;
    // Bytes after a complete response mean the connection is out of step with the server.
    EndHop(!error);
    return;
  }
  if (error) Fail(*error);
}

void Exchange::OnConnectionClosed(int error, const std::string& tlsFailure) {
  conn = nullptr;
  if (finished) return;
  if (reusedConnection && !anyResponseByte && !staleRetried) {
    // An idle connection the server had already closed: asking again is safe for a GET.
    staleRetried = true;
    StartHop(true);
    return;
  }
  if (!tlsFailure.empty()) {
    Fail(tlsFailure);
    return;
  }
  if (error != 0) {
    Fail("connection failed: " + ErrorMessage(error));
    return;
  }
  if (const auto parseError = parser->Finish()) {
    Fail(*parseError);
    return;
  }
  if (hopComplete) {
    hopComplete = false;
    EndHop(false);
  }
}

void Exchange::EndHop(bool clean) {
  HttpConnection* used = conn;
  conn = nullptr;
  if (used && !used->dead) {
    used->exchange = nullptr;
    const bool reusable = clean && head && head->minorVersion == 1 && !ConnectionHeaderSaysClose(*head) &&
                          !parser->closeDelimited();
    if (reusable) {
      impl.Release(used);
    } else {
      used->Close();
    }
  }
  if (redirecting) {
    FollowRedirect();
  } else {
    Finish();
  }
}

void Exchange::FollowRedirect() {
  if (++redirects > options.maxRedirects) {
    Fail("too many redirects");
    return;
  }
  std::optional<url::Url> next = url::Parse(IsomorphicDecode(*location), &current);
  if (!next) {
    Fail("redirect to an invalid URL");
    return;
  }
  if (next->scheme != "http" && next->scheme != "https") {
    Fail("redirect to an unsupported scheme: " + next->scheme);
    return;
  }
  if (!next->fragment) next->fragment = current.fragment;
  if (url::SerializeOrigin(*next) != url::SerializeOrigin(current)) {
    std::erase_if(options.headers, [](const auto& header) { return EqualsIgnoreCase(header.first, "authorization"); });
  }
  current = std::move(*next);
  StartHop(false);
}

void Exchange::Fail(const std::string& message) {
  if (finished) return;
  finished = true;
  if (timer != 0) {
    impl.loop.CancelTimer(timer);
    timer = 0;
  }
  if (conn) {
    HttpConnection* used = conn;
    conn = nullptr;
    used->exchange = nullptr;
    used->Close();
  }
  handler.OnError(message);
  impl.ScheduleReap();
}

void Exchange::Finish() {
  finished = true;
  if (timer != 0) {
    impl.loop.CancelTimer(timer);
    timer = 0;
  }
  handler.OnEnd();
  impl.ScheduleReap();
}

}  // namespace internal

HttpClient::Impl::~Impl() {
  for (auto& [id, exchange] : exchanges) {
    if (exchange->timer != 0) loop.CancelTimer(exchange->timer);
  }
  for (auto& connection : connections) {
    if (connection->idleTimer != 0) loop.CancelTimer(connection->idleTimer);
  }
}

internal::HttpConnection* HttpClient::Impl::TakeIdle(const std::string& key) {
  auto it = idle.find(key);
  if (it == idle.end()) return nullptr;
  while (!it->second.empty()) {
    internal::HttpConnection* candidate = it->second.back();
    it->second.pop_back();
    if (candidate->dead) continue;
    loop.CancelTimer(candidate->idleTimer);
    candidate->idleTimer = 0;
    candidate->idle = false;
    candidate->connection->SetIdle(false);
    return candidate;
  }
  return nullptr;
}

void HttpClient::Impl::Release(internal::HttpConnection* connection) {
  connection->idle = true;
  connection->connection->SetIdle(true);
  idle[connection->key].push_back(connection);
  // Background: waiting for a request that may never come must not keep Loop::Run from returning.
  connection->idleTimer = loop.PostDelayed(options.idleTimeout, [connection] {
    connection->idleTimer = 0;
    connection->Close();
  }, true);
}

void HttpClient::Impl::RemoveIdle(internal::HttpConnection* connection) {
  auto& list = idle[connection->key];
  list.erase(std::remove(list.begin(), list.end(), connection), list.end());
  connection->idle = false;
}

void HttpClient::Impl::ScheduleReap() {
  if (reapQueued) return;
  reapQueued = true;
  loop.PostDelayed(std::chrono::milliseconds(0), [this] { Reap(); });
}

// Objects are destroyed from here, a timer, and never from inside one of their own callbacks.
void HttpClient::Impl::Reap() {
  reapQueued = false;
  connections.remove_if([](const std::unique_ptr<internal::HttpConnection>& c) { return c->dead; });
  for (auto it = exchanges.begin(); it != exchanges.end();) {
    it = it->second->finished ? exchanges.erase(it) : std::next(it);
  }
}

HttpClient::HttpClient(Loop& loop, HttpClientOptions options) : impl_(std::make_unique<Impl>(loop, options)) {}

HttpClient::~HttpClient() = default;

FetchHandle HttpClient::Fetch(const url::Url& url, FetchHandler& handler, FetchOptions options) {
  const uint64_t id = impl_->nextId++;
  impl_->exchanges.emplace(id, std::make_unique<internal::Exchange>(*impl_, id, handler, std::move(options), url));
  Impl* impl = impl_.get();
  impl->loop.PostDelayed(std::chrono::milliseconds(0), [impl, id] {
    auto it = impl->exchanges.find(id);
    if (it != impl->exchanges.end() && !it->second->finished) it->second->Begin();
  });
  return FetchHandle(this, id);
}

void HttpClient::CloseIdleConnections() {
  for (auto& [key, connections] : impl_->idle) {
    for (internal::HttpConnection* connection : std::vector<internal::HttpConnection*>(connections)) connection->Close();
  }
}

void FetchHandle::Cancel() {
  if (!client_) return;
  auto it = client_->impl_->exchanges.find(id_);
  if (it != client_->impl_->exchanges.end()) it->second->Fail("aborted");
}

}  // namespace solar::net
