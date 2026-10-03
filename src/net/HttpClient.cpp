#include "solar/net/HttpClient.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <list>
#include <optional>
#include <unordered_map>

#include "ConnectRace.h"
#include "Http2.h"
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
  // Connections hold races that hold lookups with this resolver, so it is destroyed after them.
  std::shared_ptr<Resolver> resolver;
  std::shared_ptr<HstsStore> hsts;
  uint64_t nextId = 1;
  std::unordered_map<uint64_t, std::unique_ptr<internal::Exchange>> exchanges;
  std::list<std::unique_ptr<internal::HttpConnection>> connections;
  std::unordered_map<std::string, std::vector<internal::HttpConnection*>> idle;
  // Connections that speak HTTP/2, which take new requests while they are busy.
  std::unordered_map<std::string, std::vector<internal::HttpConnection*>> h2;
  // An https connection still being made, whose protocol is not yet known: a request to the same
  // origin waits for it rather than opening a second one an HTTP/2 server has no use for.
  std::unordered_map<std::string, internal::HttpConnection*> connecting;
  bool reapQueued = false;

  Impl(Loop& l, HttpClientOptions o)
      : loop(l),
        options(std::move(o)),
        resolver(options.resolver ? options.resolver : MakeSystemResolver(l)),
        hsts(options.hsts ? options.hsts : std::make_shared<HstsStore>()) {}
  ~Impl();

  internal::HttpConnection* TakeIdle(const std::string& key);
  internal::HttpConnection* TakeH2(const std::string& key);
  void Release(internal::HttpConnection* connection);
  void RemoveIdle(internal::HttpConnection* connection);
  void RemoveH2(internal::HttpConnection* connection);
  void StopConnecting(internal::HttpConnection* connection);
  void StartIdle(internal::HttpConnection* connection);
  void StopIdle(internal::HttpConnection* connection);
  void ScheduleReap();
  void Reap();
};

namespace {

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
  for (const char* forbidden : {"host", "content-length", "transfer-encoding", "connection", "upgrade", "accept-encoding", "keep-alive", "te",
                                "trailer", "proxy-connection"}) {
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

  // Lives until this connection does: it is on the stack when it reports its result.
  std::unique_ptr<ConnectRace> race;
  std::unique_ptr<TlsLayer> tls;
  std::unique_ptr<Http2Session> h2;  // set once the handshake has chosen HTTP/2
  Connection* connection = nullptr;
  Transport* transport = nullptr;

  // One exchange at a time with HTTP/1.1; with HTTP/2 one per open stream, and while an https
  // connection is still being made, everything that is waiting for it.
  std::vector<Exchange*> users;
  std::string failure;  // why it closed, when it is this side that knows
  bool offerHttp2 = false;
  bool established = false;
  bool dead = false;
  bool idle = false;
  Loop::TimerId idleTimer = 0;

  explicit HttpConnection(HttpClient::Impl& i) : impl(i) {}

  void Begin(const std::string& host, uint16_t port);
  void Close() {
    if (transport) {
      impl.RemoveH2(this);
      if (h2) h2->Shutdown();
      transport->Close();
    } else if (race && !dead) {
      // Still looking for an address to connect to: nothing to close, only to stop.
      dead = true;
      race.reset();
      impl.StopConnecting(this);
      impl.ScheduleReap();
    }
  }
  void Attach(Exchange* user) { users.push_back(user); }
  void Detach(Exchange* user) { users.erase(std::remove(users.begin(), users.end(), user), users.end()); }
  // After an HTTP/2 stream has come or gone: idle if it has no more, finished if nothing is left.
  void UpdateIdle();
  void OnRaceWon(Connection* winner);
  void OnRaceFailed(const std::string& message);

  void OnConnected() override;
  void OnData(std::span<const uint8_t> data) override;
  void OnClosed(int error) override;
};

// One fetch: a request and the redirects that follow it, each a hop on some connection.
struct Exchange : Http1ResponseParser::Sink, Http2Stream {
  HttpClient::Impl& impl;
  const uint64_t id;
  FetchHandler& handler;
  FetchOptions options;
  url::Url current;
  int redirects = 0;

  HttpConnection* conn = nullptr;
  int32_t streamId = 0;  // on an HTTP/2 connection, once the request is out
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
  std::vector<std::pair<std::string, std::string>> RequestFields() const;
  std::string BuildRequest() const;
  void Dispatch();
  void EndHop(bool clean);
  void FollowRedirect();
  void Fail(const std::string& message);
  void Finish();

  void OnConnectionReady();
  void OnData(std::span<const uint8_t> data);
  void OnConnectionClosed(int error, const std::string& failure);

  void OnEnd() override;
  void OnReset(const std::string& reason, bool retryable) override;
  void OnHead(const HttpResponseHead& h) override {
    if (finished) return;
    anyResponseByte = true;
    head = h;
    // Only a response over TLS, whose certificate has been checked, may set the policy.
    if (current.scheme == "https" && current.host) {
      if (auto policy = h.Header("strict-transport-security")) impl.hsts->Note(*current.host, *policy);
    }
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

void HttpConnection::Begin(const std::string& host, uint16_t port) {
  std::string name = host;
  if (name.size() >= 2 && name.front() == '[') name = name.substr(1, name.size() - 2);
  race = std::make_unique<ConnectRace>(
      impl.loop, *impl.resolver, std::move(name), port, AddressRace::Config{}, [this](Connection* winner) { OnRaceWon(winner); },
      [this](const std::string& message) { OnRaceFailed(message); });
  race->Start();
}

void HttpConnection::OnRaceWon(Connection* winner) {
  connection = winner;
  if (tlsName.empty()) {
    winner->SetHandler(this);
    transport = winner;
    OnConnected();
    return;
  }
  tls = std::make_unique<TlsLayer>(tlsContext, tlsName, *this);
  if (offerHttp2) tls->OfferHttp2();
  winner->SetHandler(tls.get());
  tls->Attach(winner);
  transport = tls.get();
  tls->OnConnected();  // the handshake starts now; OnConnected reaches this connection when it is done
}

void HttpConnection::OnRaceFailed(const std::string& message) {
  dead = true;
  impl.StopConnecting(this);
  std::vector<Exchange*> affected = std::move(users);
  users.clear();
  for (Exchange* user : affected) user->OnConnectionClosed(0, message);
  impl.ScheduleReap();
}

void HttpConnection::OnConnected() {
  established = true;
  impl.StopConnecting(this);
  if (tls && tls->negotiatedProtocol() == "h2") {
    h2 = std::make_unique<Http2Session>(*transport);
    if (!h2->Start()) {
      failure = "cannot start HTTP/2";
      Close();
      return;
    }
    impl.h2[key].push_back(this);
  }

  std::vector<Exchange*> waiting = users;
  if (!h2 && waiting.size() > 1) {
    // The server speaks HTTP/1.1, so this connection serves only the first; the rest get their own.
    users.resize(1);
    for (size_t i = 1; i < waiting.size(); ++i) {
      waiting[i]->conn = nullptr;
      waiting[i]->StartHop(true);
    }
    waiting.resize(1);
  }
  for (Exchange* user : waiting) user->OnConnectionReady();
}

void HttpConnection::OnData(std::span<const uint8_t> data) {
  if (h2) {
    if (auto error = h2->Receive(data)) {
      failure = *error;
      Close();
      return;
    }
    UpdateIdle();
    return;
  }
  if (!users.empty()) {
    users.front()->OnData(data);
  } else {
    Close();  // nobody asked for it: whatever this is, the connection can no longer be trusted
  }
}

void HttpConnection::UpdateIdle() {
  if (!h2 || dead || !connection) return;
  if (h2->Finished()) {
    Close();
  } else if (users.empty() && h2->active() == 0) {
    if (!idle) impl.StartIdle(this);
  } else if (idle) {
    impl.StopIdle(this);
  }
}

void HttpConnection::OnClosed(int error) {
  transport = nullptr;
  connection = nullptr;

  dead = true;
  if (idleTimer != 0) {
    impl.loop.CancelTimer(idleTimer);
    idleTimer = 0;
  }
  if (idle) impl.RemoveIdle(this);
  impl.RemoveH2(this);
  impl.StopConnecting(this);
  const std::string reason = !failure.empty() ? failure : (tls ? tls->failure() : std::string());
  std::vector<Exchange*> affected = std::move(users);
  users.clear();
  for (Exchange* user : affected) user->OnConnectionClosed(error, reason);
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
  if (!RefuseHeader("User-Agent", impl.options.userAgent).empty()) {
    Fail("invalid User-Agent");
    return;
  }
  timer = impl.loop.PostDelayed(options.timeout, [this] {
    timer = 0;
    Fail("timed out");
  });
  StartHop(false);
}

std::vector<std::pair<std::string, std::string>> Exchange::RequestFields() const {
  std::vector<std::pair<std::string, std::string>> fields = options.headers;
  bool hasUserAgent = false;
  bool hasAccept = false;
  for (const auto& [name, value] : fields) {
    hasUserAgent = hasUserAgent || EqualsIgnoreCase(name, "user-agent");
    hasAccept = hasAccept || EqualsIgnoreCase(name, "accept");
  }
  if (!hasUserAgent) fields.emplace_back("User-Agent", impl.options.userAgent);
  if (!hasAccept) fields.emplace_back("Accept", "*/*");
  fields.emplace_back("Accept-Encoding", "gzip, deflate, br, zstd");
  return fields;
}

std::string Exchange::BuildRequest() const {
  std::string target = url::SerializePath(current);
  if (current.query) target += "?" + *current.query;

  std::string out = "GET " + target + " HTTP/1.1\r\nHost: " + url::GetHost(current) + "\r\n";
  for (const auto& [name, value] : RequestFields()) out += name + ": " + value + "\r\n";
  out += "\r\n";
  return out;
}

void Exchange::StartHop(bool forceFresh) {
  // A host that has asked for https is not spoken to in the clear, whatever the URL says. The port
  // stays as it is: a URL's default port is not in it, so port 80 was never there to change.
  if (current.scheme == "http" && current.host && impl.hsts->Covers(*current.host)) current.scheme = "https";
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

  const bool http2 = secure && impl.options.http2;
  if (!forceFresh) {
    if (http2) {
      if (HttpConnection* shared = impl.TakeH2(key)) {
        conn = shared;
        conn->Attach(this);
        reusedConnection = true;
        Dispatch();
        return;
      }
    }
    if (HttpConnection* pooled = impl.TakeIdle(key)) {
      conn = pooled;
      conn->Attach(this);
      reusedConnection = true;
      conn->transport->Send(std::move(request));
      return;
    }
    if (http2) {
      if (auto it = impl.connecting.find(key); it != impl.connecting.end()) {
        conn = it->second;
        conn->Attach(this);
        return;
      }
    }
  }

  auto fresh = std::make_unique<HttpConnection>(impl);
  conn = fresh.get();
  conn->key = std::move(key);
  conn->tlsContext = std::move(context);
  conn->offerHttp2 = http2;
  if (http2 && !forceFresh) impl.connecting[conn->key] = conn;
  if (secure) {
    conn->tlsName = *current.host;
    if (conn->tlsName.size() >= 2 && conn->tlsName.front() == '[') conn->tlsName = conn->tlsName.substr(1, conn->tlsName.size() - 2);
  }
  conn->Attach(this);
  impl.connections.push_back(std::move(fresh));
  conn->Begin(*current.host, port);
}

// The connection is ready for this exchange's request, which goes out in whatever way it speaks.
void Exchange::Dispatch() {
  if (!conn->h2) {
    conn->transport->Send(std::move(request));
    return;
  }
  std::string target = url::SerializePath(current);
  if (current.query) target += "?" + *current.query;
  Http2Request wire;
  wire.scheme = current.scheme;
  wire.authority = url::GetHost(current);
  wire.path = std::move(target);
  for (auto& [name, value] : RequestFields()) {
    for (char& c : name) c = Lower(c);  // HTTP/2 names are lower case
    wire.headers.emplace_back(std::move(name), std::move(value));
  }
  streamId = conn->h2->Submit(wire, *this);
  if (streamId > 0) {
    conn->UpdateIdle();
    return;
  }
  // The connection cannot take another stream after all: ask a new one, once.
  streamId = 0;
  conn->Detach(this);
  conn = nullptr;
  if (staleRetried) {
    Fail("cannot send the request over HTTP/2");
    return;
  }
  staleRetried = true;
  StartHop(true);
}

void Exchange::OnEnd() {
  if (finished) return;
  OnComplete();  // finishes the decoding, which can still fail the fetch
  if (finished) return;
  hopComplete = false;
  EndHop(true);
}

void Exchange::OnReset(const std::string& reason, bool retryable) {
  streamId = 0;
  if (finished) return;
  if (conn) {
    conn->Detach(this);
    conn->UpdateIdle();
    conn = nullptr;
  }
  if (!anyResponseByte && !staleRetried && (retryable || reusedConnection)) {
    staleRetried = true;
    StartHop(true);
    return;
  }
  Fail(reason);
}

void Exchange::OnConnectionReady() {
  if (finished) return;
  Dispatch();
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

void Exchange::OnConnectionClosed(int error, const std::string& failure) {
  const bool http2 = conn && conn->h2;
  conn = nullptr;
  streamId = 0;
  if (finished) return;
  if (reusedConnection && !anyResponseByte && !staleRetried) {
    // An idle connection the server had already closed: asking again is safe for a GET.
    staleRetried = true;
    StartHop(true);
    return;
  }
  if (!failure.empty()) {
    Fail(failure);
    return;
  }
  if (error != 0) {
    Fail("connection failed: " + ErrorMessage(error));
    return;
  }
  if (http2) {
    Fail("the connection closed before the response was complete");
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
    used->Detach(this);
    if (used->h2) {
      // A stream ending does not end the connection, which other streams may be using.
      streamId = 0;
      used->UpdateIdle();
    } else {
      const bool reusable = clean && head && head->minorVersion == 1 && !ConnectionHeaderSaysClose(*head) && !parser->closeDelimited();
      if (reusable) {
        impl.Release(used);
      } else {
        used->Close();
      }
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
    used->Detach(this);
    if (used->h2) {
      if (streamId > 0) used->h2->Reset(streamId);
      streamId = 0;
      used->UpdateIdle();
    } else if (used->users.empty()) {
      used->Close();  // nothing else is waiting for what this connection might become
    }
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
    StopIdle(candidate);
    return candidate;
  }
  return nullptr;
}

void HttpClient::Impl::Release(internal::HttpConnection* connection) {
  idle[connection->key].push_back(connection);
  StartIdle(connection);
}

void HttpClient::Impl::StartIdle(internal::HttpConnection* connection) {
  connection->idle = true;
  connection->connection->SetIdle(true);
  // Background: waiting for a request that may never come must not keep Loop::Run from returning.
  connection->idleTimer = loop.PostDelayed(options.idleTimeout, [connection] {
    connection->idleTimer = 0;
    connection->Close();
  }, true);
}

void HttpClient::Impl::StopIdle(internal::HttpConnection* connection) {
  if (connection->idleTimer != 0) loop.CancelTimer(connection->idleTimer);
  connection->idleTimer = 0;
  connection->idle = false;
  connection->connection->SetIdle(false);
}

internal::HttpConnection* HttpClient::Impl::TakeH2(const std::string& key) {
  auto it = h2.find(key);
  if (it == h2.end()) return nullptr;
  for (internal::HttpConnection* candidate : it->second) {
    if (candidate->dead || !candidate->h2 || !candidate->h2->Available()) continue;
    if (candidate->idle) StopIdle(candidate);
    return candidate;
  }
  return nullptr;
}

void HttpClient::Impl::RemoveH2(internal::HttpConnection* connection) {
  auto it = h2.find(connection->key);
  if (it == h2.end()) return;
  it->second.erase(std::remove(it->second.begin(), it->second.end(), connection), it->second.end());
}

void HttpClient::Impl::StopConnecting(internal::HttpConnection* connection) {
  auto it = connecting.find(connection->key);
  if (it != connecting.end() && it->second == connection) connecting.erase(it);
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
  for (auto& [key, connections] : impl_->h2) {
    for (internal::HttpConnection* connection : std::vector<internal::HttpConnection*>(connections)) {
      if (connection->idle) connection->Close();
    }
  }
}

void FetchHandle::Cancel() {
  if (!client_) return;
  auto it = client_->impl_->exchanges.find(id_);
  if (it != client_->impl_->exchanges.end()) it->second->Fail("aborted");
}

}  // namespace solar::net
