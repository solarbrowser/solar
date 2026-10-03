#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>

#include <cerrno>
#include <cstring>
#include <string>

#include "solar/net/Fetch.h"
#include "solar/url/Serializer.h"
#include "solar/url/UrlApi.h"

namespace solar::net {

namespace {

// Blocks while the system resolver runs. This stands in until DNS moves off the loop's thread.
bool Resolve(const std::string& host, uint16_t port, SocketAddress& out, std::string& error) {
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
  std::memcpy(&out.storage, results->ai_addr, results->ai_addrlen);
  out.length = results->ai_addrlen;
  ::freeaddrinfo(results);
  return true;
}

// One request on one connection. It deletes itself when the connection is over.
class Exchange : public ConnectionHandler, public Http1ResponseParser::Sink {
 public:
  Exchange(Loop& loop, FetchHandler& handler, std::string request, FetchOptions options)
      : loop_(loop), handler_(handler), request_(std::move(request)), options_(options), parser_(*this) {}

  void Start(const SocketAddress& address) {
    timer_ = loop_.PostDelayed(options_.timeout, [this] {
      timer_ = 0;
      Fail("timed out");
    });
    connection_ = loop_.Connect(address, this);
  }

  void OnConnected() override { connection_->Send(std::move(request_)); }

  void OnData(std::span<const uint8_t> data) override {
    if (finished_) return;
    if (auto error = parser_.Feed(data)) Fail(*error);
  }

  void OnClosed(int error) override {
    connection_ = nullptr;  // the loop has already destroyed it
    if (!finished_) {
      if (error != 0) {
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
    if (connection_) connection_->Close();
  }

 private:
  void Fail(const std::string& message) {
    if (finished_) return;
    finished_ = true;
    handler_.OnError(message);
    if (connection_) connection_->Close();
  }

  Loop& loop_;
  FetchHandler& handler_;
  std::string request_;
  FetchOptions options_;
  Http1ResponseParser parser_;
  Connection* connection_ = nullptr;
  Loop::TimerId timer_ = 0;
  bool finished_ = false;
};

}  // namespace

// An error found before there is a connection to report it from, still delivered from Run.
static void FailEarly(Loop& loop, FetchHandler& handler, std::string message) {
  loop.PostDelayed(std::chrono::milliseconds(0), [&handler, message = std::move(message)] { handler.OnError(message); });
}

void Fetch(Loop& loop, const url::Url& url, FetchHandler& handler, FetchOptions options) {
  if (url.scheme != "http") {
    FailEarly(loop, handler, "unsupported scheme: " + url.scheme + (url.scheme == "https" ? " (TLS is not implemented yet)" : ""));
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

  SocketAddress address;
  std::string error;
  if (!Resolve(*url.host, url.port.value_or(80), address, error)) {
    FailEarly(loop, handler, error);
    return;
  }

  (new Exchange(loop, handler, std::move(request), options))->Start(address);
}

}  // namespace solar::net
