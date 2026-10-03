#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include "solar/net/Fetch.h"
#include "solar/url/Serializer.h"
#include "solar/url/UrlApi.h"

namespace solar::net {

namespace {

// Blocks while the system resolver runs. This stands in until DNS moves off the loop's thread.
bool Resolve(const std::string& host, uint16_t port, std::vector<SocketAddress>& out, std::string& error) {
  std::string name = host;
  if (name.size() >= 2 && name.front() == '[' && name.back() == ']') name = name.substr(1, name.size() - 2);

  addrinfo hints{};
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICSERV;
  addrinfo* results = nullptr;
  const int code = ::getaddrinfo(name.c_str(), std::to_string(port).c_str(), &hints, &results);
  if (code != 0) {
    error = std::string("cannot resolve ") + host + ": " + ::gai_strerror(code);
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

// One request on one connection. It deletes itself when the connection is over.
class Exchange : public ConnectionHandler, public Http1ResponseParser::Sink {
 public:
  Exchange(Loop& loop, FetchHandler& handler, std::string request, FetchOptions options)
      : loop_(loop), handler_(handler), request_(std::move(request)), options_(options), parser_(*this) {}

  // `tlsName` is empty for plain HTTP, and otherwise the name the server's certificate must carry.
  void Start(std::vector<SocketAddress> addresses, const std::string& tlsName) {
    addresses_ = std::move(addresses);
    tlsName_ = tlsName;
    timer_ = loop_.PostDelayed(options_.timeout, [this] {
      timer_ = 0;
      Fail("timed out");
    });
    ConnectToNext();
  }

  // With TLS this is the end of the handshake.
  void OnConnected() override {
    established_ = true;
    transport_->Send(std::move(request_));
  }

  void OnData(std::span<const uint8_t> data) override {
    if (finished_) return;
    if (auto error = parser_.Feed(data)) Fail(*error);
  }

  void OnClosed(int error) override {
    transport_ = nullptr;  // what it was has already been destroyed, or is about to be
    const bool tlsFailed = tls_ && !tls_->failure().empty();
    if (!finished_ && !established_ && !tlsFailed && error != 0 && next_ < addresses_.size()) {
      // The address could not be reached at all, so the next one is tried. A refusal by the
      // server after connecting is not a reason to ask another.
      tls_ = nullptr;
      ConnectToNext();
      return;
    }
    if (!finished_) {
      if (tls_ && !tls_->failure().empty()) {
        Fail(tls_->failure());
      } else if (error != 0) {
        Fail(std::string("connection failed: ") + std::strerror(error));
      } else if (auto parseError = parser_.Finish()) {
        Fail(*parseError);
      }
    }
    if (timer_ != 0) loop_.CancelTimer(timer_);
    delete this;
  }

  void OnHead(const HttpResponseHead& head) override { handler_.OnResponseHead(head); }
  void OnBody(std::span<const uint8_t> data) override { handler_.OnBody(data); }
  void OnComplete() override {
    if (finished_) return;
    finished_ = true;
    handler_.OnEnd();
    if (transport_) transport_->Close();
  }

 private:
  void ConnectToNext() {
    const SocketAddress& address = addresses_[next_++];
    if (tlsName_.empty()) {
      transport_ = loop_.Connect(address, this);
    } else {
      std::shared_ptr<TlsContext> context = options_.tls ? options_.tls : DefaultTlsContext();
      tls_ = new TlsLayer(std::move(context), tlsName_, *this);
      Connection* connection = loop_.Connect(address, tls_);
      tls_->Attach(connection);
      transport_ = tls_;
    }
  }

  void Fail(const std::string& message) {
    if (finished_) return;
    finished_ = true;
    handler_.OnError(message);
    if (transport_) transport_->Close();
  }

  Loop& loop_;
  FetchHandler& handler_;
  std::string request_;
  FetchOptions options_;
  Http1ResponseParser parser_;
  Transport* transport_ = nullptr;
  TlsLayer* tls_ = nullptr;  // owned by itself; valid until it has called OnClosed
  std::vector<SocketAddress> addresses_;
  size_t next_ = 0;
  std::string tlsName_;
  bool established_ = false;
  Loop::TimerId timer_ = 0;
  bool finished_ = false;
};

}  // namespace

// An error found before there is a connection to report it from, still delivered from Run.
static void FailEarly(Loop& loop, FetchHandler& handler, std::string message) {
  loop.PostDelayed(std::chrono::milliseconds(0), [&handler, message = std::move(message)] { handler.OnError(message); });
}

void Fetch(Loop& loop, const url::Url& url, FetchHandler& handler, FetchOptions options) {
  if (url.scheme != "http" && url.scheme != "https") {
    FailEarly(loop, handler, "unsupported scheme: " + url.scheme);
    return;
  }
  if (!url.host || url.host->empty()) {
    FailEarly(loop, handler, "the URL has no host");
    return;
  }

  std::string target = url::SerializePath(url);
  if (url.query) target += "?" + *url.query;
  std::string request = "GET " + target + " HTTP/1.1\r\n";
  request += "Host: " + url::GetHost(url) + "\r\n";
  request += "User-Agent: Solar\r\nAccept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n";

  std::vector<SocketAddress> addresses;
  std::string error;
  const bool secure = url.scheme == "https";
  if (!Resolve(*url.host, url.port.value_or(secure ? 443 : 80), addresses, error)) {
    FailEarly(loop, handler, error);
    return;
  }

  std::string tlsName;
  if (secure) {
    tlsName = *url.host;
    if (tlsName.size() >= 2 && tlsName.front() == '[') tlsName = tlsName.substr(1, tlsName.size() - 2);
  }
  (new Exchange(loop, handler, std::move(request), options))->Start(std::move(addresses), tlsName);
}

}  // namespace solar::net
