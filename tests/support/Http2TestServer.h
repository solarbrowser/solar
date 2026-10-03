#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "TlsTestServer.h"

namespace solar::test {

using Fields = std::vector<std::pair<std::string, std::string>>;

struct H2Request {
  std::string path;
  std::string authority;
  std::string scheme;
  Fields headers;  // the ordinary ones, lower case

  std::string Header(const std::string& name) const {
    for (const auto& [key, value] : headers) {
      if (key == name) return value;
    }
    return "";
  }
};

struct H2Reply {
  int status = 200;
  Fields headers;
  std::string body;
  Fields trailers;
  std::vector<int> interim;  // informational statuses sent before the response, such as 103
  std::chrono::milliseconds delay{0};  // how long the server takes to answer
  std::optional<size_t> contentLength;  // what the content-length says, if not the body's size
  // After this many body bytes the server resets the stream instead of finishing it.
  std::optional<size_t> resetAfter;
  bool goAwayInstead = false;           // refuse the request: GOAWAY naming the stream before it as the last
  bool closeConnection = false;         // drop the connection without a word
  std::chrono::milliseconds pace{0};    // wait this long between 4 KiB pieces of the body
};

struct H2ServerOptions {
  uint32_t maxConcurrentStreams = 100;
};

// An HTTP/2 server over TLS, answering each request the way a function says. One thread serves
// each connection, so many streams of one connection are in flight together.
class Http2TestServer {
 public:
  using Handler = std::function<H2Reply(const H2Request&)>;

  Http2TestServer(const Identity& identity, Handler handler, H2ServerOptions options = {});
  ~Http2TestServer();

  bool ok() const { return tls_->ok(); }
  uint16_t port() const { return tls_->port(); }

  int connections() const { return connections_; }
  // The most streams that were being worked on at once on any one connection.
  int maxConcurrent() const { return maxConcurrent_; }
  // How many streams the client reset, as it does with one it no longer wants.
  int resets() const { return resets_; }
  std::vector<H2Request> requests() const;

 private:
  void Serve(ssl_st* connection);

  Handler handler_;
  H2ServerOptions options_;
  std::atomic<int> connections_{0};
  std::atomic<int> maxConcurrent_{0};
  std::atomic<int> resets_{0};
  mutable std::mutex mutex_;
  std::vector<H2Request> requests_;
  std::unique_ptr<TlsTestServer> tls_;  // last: its threads start using the rest at once
};

}  // namespace solar::test
