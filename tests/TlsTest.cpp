#include <cstdio>
#include <optional>
#include <string>

#include "solar/net/Fetch.h"
#include "solar/url/Parser.h"
#include "support/Pki.h"
#include "support/TestServer.h"
#include "support/TlsTestServer.h"

namespace {

using solar::net::FetchOptions;
using solar::net::TlsContext;
using solar::net::TlsOptions;
using solar::test::TestPki;
using solar::test::TestServer;
using solar::test::TlsTestServer;

struct Result {
  std::string body;
  bool ended = false;
  std::optional<std::string> error;
  int terminalCalls = 0;
};

class Collector : public solar::net::FetchHandler {
 public:
  void OnResponseHead(const solar::net::HttpResponseHead&) override {}
  void OnBody(std::span<const uint8_t> data) override { result.body.append(reinterpret_cast<const char*>(data.data()), data.size()); }
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

Result Get(const std::string& url, std::shared_ptr<TlsContext> tls, std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
  auto loop = solar::net::Loop::Create();
  auto parsed = solar::url::Parse(url);
  Collector collector;
  FetchOptions options;
  options.tls = std::move(tls);
  options.timeout = timeout;
  solar::net::Fetch(*loop, *parsed, collector, options);
  loop->Run();
  return collector.result;
}

std::shared_ptr<TlsContext> Trusting(const TestPki& pki) {
  TlsOptions options;
  options.extraRootsPem = pki.rootPem();
  return TlsContext::Create(options);
}

std::string Url(const TlsTestServer& server, const std::string& host = "127.0.0.1") {
  return "https://" + host + ":" + std::to_string(server.port()) + "/";
}

bool Mentions(const Result& r, const std::string& text) { return r.error && r.error->find(text) != std::string::npos; }

TlsTestServer::Script Hello(std::string body = "hello") {
  return [body = std::move(body)](ssl_st* connection) {
    TlsTestServer::ReadHead(connection);
    TlsTestServer::SendAll(connection, "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
  };
}

}  // namespace

int main() {
  TestPki pki;
  auto trusting = Trusting(pki);

  {
    TlsTestServer server(pki.Issue({"127.0.0.1", "localhost"}), Hello());
    Result r = Get(Url(server), trusting);
    Check("trusted certificate, by IP", r.ended && r.body == "hello", r.error.value_or(""));
    Check("TLS 1.2 or later", server.version() == "TLSv1.3" || server.version() == "TLSv1.2", server.version());
    Check("ALPN offers http/1.1", server.alpn() == "http/1.1", server.alpn());
    Check("no SNI for an IP literal", server.serverName().empty(), server.serverName());

    Result named = Get(Url(server, "localhost"), trusting);
    Check("trusted certificate, by name", named.ended && named.body == "hello", named.error.value_or(""));
    Check("SNI carries the name", server.serverName() == "localhost", server.serverName());
  }

  // Certificates that must be refused.
  {
    TlsTestServer server(pki.Issue({"127.0.0.1"}), Hello());
    Result r = Get(Url(server), TlsContext::Create());
    Check("untrusted authority", Mentions(r, "certificate verify failed") && !r.ended, r.error.value_or("accepted"));
  }
  {
    TestPki other;
    TlsTestServer server(pki.Issue({"127.0.0.1"}), Hello());
    Result r = Get(Url(server), Trusting(other));
    Check("a different authority is not trusted", Mentions(r, "certificate verify failed") && !r.ended, r.error.value_or("accepted"));
  }
  {
    TlsTestServer server(pki.Issue({"other.example"}), Hello());
    Result r = Get(Url(server), trusting);
    Check("name mismatch", Mentions(r, "mismatch") && !r.ended, r.error.value_or("accepted"));
  }
  {
    TlsTestServer server(pki.Issue({"127.0.0.2"}), Hello());
    Result r = Get(Url(server), trusting);
    Check("IP mismatch", Mentions(r, "mismatch") && !r.ended, r.error.value_or("accepted"));
  }
  {
    TlsTestServer server(pki.Issue({"127.0.0.1"}, -400, -30), Hello());
    Result r = Get(Url(server), trusting);
    Check("expired certificate", Mentions(r, "expired") && !r.ended, r.error.value_or("accepted"));
  }
  {
    TlsTestServer server(pki.Issue({"127.0.0.1"}, 10, 40), Hello());
    Result r = Get(Url(server), trusting);
    Check("certificate not yet valid", Mentions(r, "not yet valid") && !r.ended, r.error.value_or("accepted"));
  }
  {
    // A plain HTTP server answering a TLS hello.
    TestServer server([](int client) {
      TestServer::ReadHead(client);
      TestServer::SendAll(client, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n");
    });
    Result r = Get("https://127.0.0.1:" + std::to_string(server.port()) + "/", trusting);
    Check("HTTP spoken where TLS is expected", r.error.has_value() && !r.ended && r.body.empty(), r.error.value_or("accepted"));
  }
  {
    TestServer server([](int client) { TestServer::WaitForClose(client); });  // accepts and says nothing
    Result r = Get("https://127.0.0.1:" + std::to_string(server.port()) + "/", trusting, std::chrono::milliseconds(200));
    Check("handshake that never finishes", r.error == std::optional<std::string>("timed out"), r.error.value_or(""));
  }

  // Data over TLS.
  {
    std::string big(3 * 1024 * 1024, '\0');
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>(i % 251);
    TlsTestServer server(pki.Issue({"127.0.0.1"}), Hello(big));
    Result r = Get(Url(server), trusting);
    Check("large body is intact", r.ended && r.body == big, r.error.value_or(""));
  }
  {
    TlsTestServer server(pki.Issue({"127.0.0.1"}), [](ssl_st* connection) {
      TlsTestServer::ReadHead(connection);
      TlsTestServer::SendAll(connection, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n");
    });
    Result r = Get(Url(server), trusting);
    Check("chunked body", r.ended && r.body == "hello", r.error.value_or(""));
  }

  // Truncation: a body that runs until close is only whole if the close is a close_notify.
  const std::string untilClose = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nthe whole body";
  {
    TlsTestServer server(pki.Issue({"127.0.0.1"}), [&](ssl_st* c) {
      TlsTestServer::ReadHead(c);
      TlsTestServer::SendAll(c, untilClose);
    });
    Result r = Get(Url(server), trusting);
    Check("a body that ends with close_notify is complete", r.ended && r.body == "the whole body", r.error.value_or(""));
  }
  {
    TlsTestServer server(pki.Issue({"127.0.0.1"}), [&](ssl_st* c) {
      TlsTestServer::ReadHead(c);
      TlsTestServer::SendAll(c, untilClose);
    }, /*sendCloseNotify=*/false);
    Result r = Get(Url(server), trusting);
    Check("a body cut off without close_notify is an error", Mentions(r, "close_notify") && !r.ended, r.error.value_or("accepted"));
  }
  {
    TlsTestServer server(pki.Issue({"127.0.0.1"}), Hello(), /*sendCloseNotify=*/false);
    Result r = Get(Url(server), trusting);
    Check("a length-delimited body is complete without close_notify", r.ended && r.body == "hello" && r.terminalCalls == 1, r.error.value_or(""));
  }

  std::printf("tls: %d/%d passed\n", total - failed, total);
  return failed == 0 ? 0 : 1;
}
