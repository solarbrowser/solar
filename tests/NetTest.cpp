#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "solar/net/Fetch.h"
#include "solar/url/Parser.h"
#include "support/TestServer.h"

namespace {

using solar::net::FetchOptions;
using solar::net::HttpResponseHead;
using solar::test::TestServer;

struct Result {
  std::optional<HttpResponseHead> head;
  std::string body;
  size_t bodyCalls = 0;
  bool ended = false;
  std::optional<std::string> error;
  int terminalCalls = 0;
};

class Collector : public solar::net::FetchHandler {
 public:
  void OnResponseHead(const HttpResponseHead& head) override { result.head = head; }
  void OnBody(std::span<const uint8_t> data) override {
    ++result.bodyCalls;
    result.body.append(reinterpret_cast<const char*>(data.data()), data.size());
  }
  void OnEnd() override {
    ++result.terminalCalls;
    result.ended = true;
  }
  void OnError(std::string_view message) override {
    ++result.terminalCalls;
    result.error = std::string(message);
  }
  Result result;
};

int total = 0;
int failed = 0;

void Check(const std::string& name, bool ok, const std::string& detail = "") {
  ++total;
  if (ok) return;
  ++failed;
  std::printf("FAIL %s %s\n", name.c_str(), detail.c_str());
}

Result Get(const std::string& url, FetchOptions options = {}) {
  auto loop = solar::net::Loop::Create();
  if (!loop) {
    std::printf("cannot create the loop (is io_uring available?)\n");
    std::exit(2);
  }
  auto parsed = solar::url::Parse(url);
  Collector collector;
  solar::net::Fetch(*loop, *parsed, collector, options);
  loop->Run();
  return collector.result;
}

std::string Url(const TestServer& server, const std::string& path = "/") {
  return "http://127.0.0.1:" + std::to_string(server.port()) + path;
}

std::string Respond(const std::string& head, const std::string& body) { return "HTTP/1.1 " + head + "\r\n\r\n" + body; }

// A server that reads the request and sends `response` as it is.
TestServer::Script Replies(std::string response) {
  return [response = std::move(response)](int client) {
    TestServer::ReadHead(client);
    TestServer::SendAll(client, response);
  };
}

}  // namespace

int main() {
  {
    TestServer server(Replies("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\nhello"));
    Result r = Get(Url(server));
    Check("content-length response", r.ended && !r.error && r.head && r.head->status == 200 && r.body == "hello");
    Check("headers are delivered", r.head && r.head->Header("content-type") == "text/plain");
    Check("one terminal call", r.terminalCalls == 1);
  }
  {
    TestServer server(Replies("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n"));
    Result r = Get(Url(server));
    Check("chunked response", r.ended && r.body == "hello world", r.error.value_or(""));
  }
  {
    TestServer server(Replies("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nuntil the end"));
    Result r = Get(Url(server));
    Check("body delimited by close", r.ended && r.body == "until the end", r.error.value_or(""));
  }
  {
    TestServer server(Replies("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi"));
    Result r = Get(Url(server));
    Check("interim response", r.ended && r.head && r.head->status == 200 && r.body == "hi");
  }
  {
    std::string request;
    std::mutex mutex;
    TestServer server([&](int client) {
      std::string head = TestServer::ReadHead(client);
      {
        std::lock_guard<std::mutex> lock(mutex);
        request = head;
      }
      TestServer::SendAll(client, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    });
    Get(Url(server, "/a/b/../c?x=1&y=%0d%0aInjected:%20yes#fragment"));
    std::lock_guard<std::mutex> lock(mutex);
    Check("request line", request.starts_with("GET /a/c?x=1&y=%0d%0aInjected:%20yes HTTP/1.1\r\n"), request);
    Check("host header", request.find("\r\nHost: 127.0.0.1:" + std::to_string(server.port()) + "\r\n") != std::string::npos, request);
    Check("no fragment and no injected header", request.find("fragment") == std::string::npos && request.find("\r\nInjected:") == std::string::npos);
  }
  {
    // The head and the body arrive a byte at a time, so nothing lines up with a buffer.
    TestServer server([](int client) {
      TestServer::ReadHead(client);
      TestServer::SendSlowly(client, "HTTP/1.1 200 OK\r\nX-A: b\r\nContent-Length: 4\r\n\r\ndata", 1, 1);
    });
    Result r = Get(Url(server));
    Check("response sent a byte at a time", r.ended && r.body == "data" && r.head && r.head->Header("x-a") == "b", r.error.value_or(""));
  }
  {
    std::string big(3 * 1024 * 1024, '\0');
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>(i % 251);
    TestServer server(Replies("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(big.size()) + "\r\n\r\n" + big));
    Result r = Get(Url(server));
    Check("large body is intact", r.ended && r.body == big, r.error.value_or(""));
    Check("large body arrives in pieces", r.bodyCalls > 10, std::to_string(r.bodyCalls));
  }

  // Responses the client must refuse.
  const std::vector<std::pair<std::string, std::string>> bad = {
      {"garbage status line", "NOT HTTP\r\n\r\n"},
      {"both framings", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n"},
      {"conflicting lengths", "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\nhi!"},
      {"truncated body", "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhello"},
      {"bad chunk", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n"},
      {"closed at once", ""},
  };
  for (const auto& [name, response] : bad) {
    TestServer server(Replies(response));
    Result r = Get(Url(server));
    Check("refuses: " + name, r.error.has_value() && !r.ended && r.terminalCalls == 1, r.ended ? "was accepted" : "");
  }
  {
    // The first response is complete, so it stands; what follows it is dropped with the connection.
    TestServer server(Replies("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nokHTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"));
    Result r = Get(Url(server));
    Check("bytes after a complete response are dropped", r.ended && r.body == "ok" && r.terminalCalls == 1);
  }
  {
    TestServer server(Replies("HTTP/1.1 200 OK\r\nX: " + std::string(80 * 1024, 'a') + "\r\n\r\n"));
    Result r = Get(Url(server));
    Check("refuses an oversized head", r.error.has_value() && !r.head);
  }

  // Failures that are not the server's response.
  {
    uint16_t port;
    {
      TestServer closed([](int) {});
      port = closed.port();
    }
    Result r = Get("http://127.0.0.1:" + std::to_string(port) + "/");
    Check("connection refused", r.error.has_value() && r.error->find("refused") != std::string::npos, r.error.value_or("no error"));
  }
  {
    TestServer server([](int client) {
      TestServer::ReadHead(client);
      TestServer::WaitForClose(client);  // never answers
    });
    FetchOptions options;
    options.timeout = std::chrono::milliseconds(150);
    Result r = Get(Url(server), options);
    Check("timeout", r.error == std::optional<std::string>("timed out"), r.error.value_or("no error"));
  }
  {
    Result r = Get("ftp://127.0.0.1:1/");
    Check("other schemes are refused", r.error.has_value() && r.error->find("unsupported scheme") != std::string::npos);
  }
  {
    TestServer server(Replies("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nv6"), true);
    if (server.ok()) {
      Result r = Get("http://[::1]:" + std::to_string(server.port()) + "/");
      Check("IPv6 loopback", r.ended && r.body == "v6", r.error.value_or(""));
    }
  }

  std::printf("net: %d/%d passed\n", total - failed, total);
  return failed == 0 ? 0 : 1;
}
