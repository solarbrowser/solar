#include <atomic>
#include <cstdio>
#include <optional>
#include <string>

#include "solar/net/HttpClient.h"
#include "solar/url/Parser.h"
#include "solar/url/Serializer.h"
#include "support/Pki.h"
#include "support/TestLoop.h"
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
  std::string finalUrl;
  bool ended = false;
  std::optional<std::string> error;
  int terminalCalls = 0;
};

class Collector : public solar::net::FetchHandler {
 public:
  void OnResponseHead(const solar::net::HttpResponseHead&, const solar::url::Url& url) override { result.finalUrl = solar::url::Serialize(url); }
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
  auto loop = solar::test::MakeLoop();
  auto parsed = solar::url::Parse(url);
  Collector collector;
  FetchOptions options;
  options.tls = std::move(tls);
  options.timeout = timeout;
  solar::net::HttpClient client(*loop);
  client.Fetch(*parsed, collector, options);
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

  // Keep-alive and redirects over TLS: one handshake serves every request on the connection.
  {
    std::atomic<int> handshakes{0};
    TlsTestServer server(pki.Issue({"127.0.0.1"}), [&](ssl_st* connection) {
      ++handshakes;
      while (true) {
        std::string head = TlsTestServer::ReadHead(connection);
        if (head.empty()) break;
        const size_t start = head.find(' ');
        const std::string path = head.substr(start + 1, head.find(' ', start + 1) - start - 1);
        if (path == "/start") {
          TlsTestServer::SendAll(connection, "HTTP/1.1 302 Found\r\nLocation: /final\r\nContent-Length: 0\r\n\r\n");
        } else {
          TlsTestServer::SendAll(connection, "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(path.size()) + "\r\n\r\n" + path);
        }
      }
    });

    auto loop = solar::test::MakeLoop();
    solar::net::HttpClient client(*loop);
    FetchOptions options;
    options.tls = trusting;

    Collector first;
    client.Fetch(*solar::url::Parse(Url(server) + "start"), first, options);
    loop->Run();
    Collector second;
    client.Fetch(*solar::url::Parse(Url(server) + "again"), second, options);
    loop->Run();
    Check("a redirect over TLS", first.result.ended && first.result.body == "/final", first.result.error.value_or(first.result.body));
    Check("a second request over TLS", second.result.ended && second.result.body == "/again", second.result.error.value_or(""));
    Check("one TLS handshake served all three requests", handshakes == 1, std::to_string(handshakes));
  }

  // HSTS: a host that has asked for https is not reached over http again.
  {
    std::atomic<int> plainRequests{0};
    TlsTestServer server(pki.Issue({"localhost"}), [&](ssl_st* connection) {
      while (true) {
        const std::string head = TlsTestServer::ReadHead(connection);
        if (head.empty()) break;
        const size_t start = head.find(' ');
        const std::string path = head.substr(start + 1, head.find(' ', start + 1) - start - 1);
        std::string extra;
        if (path == "/set") extra = "Strict-Transport-Security: max-age=3600\r\n";
        if (path == "/clear") extra = "Strict-Transport-Security: max-age=0\r\n";
        if (path == "/invalid") extra = "Strict-Transport-Security: nonsense\r\n";
        TlsTestServer::SendAll(connection, "HTTP/1.1 200 OK\r\n" + extra + "Content-Length: " + std::to_string(path.size()) + "\r\n\r\n" + path);
      }
    });
    const std::string port = std::to_string(server.port());
    auto loop = solar::test::MakeLoop();
    solar::net::HttpClient client(*loop);
    FetchOptions options;
    options.tls = trusting;
    options.timeout = std::chrono::seconds(5);
    const auto fetch = [&](const std::string& url) {
      Collector c;
      client.Fetch(*solar::url::Parse(url), c, options);
      loop->Run();
      return c.result;
    };

    Result before = fetch("http://localhost:" + port + "/x");
    Check("without a policy, http to a TLS port goes nowhere", !before.ended && before.error, before.error.value_or(""));
    Result learn = fetch("https://localhost:" + port + "/invalid");
    Result stillNot = fetch("http://localhost:" + port + "/x");
    Check("an invalid header sets no policy", learn.ended && !stillNot.ended, stillNot.error.value_or(stillNot.body));

    Result set = fetch("https://localhost:" + port + "/set");
    Check("the header comes with the response", set.ended && set.body == "/set", set.error.value_or(""));
    Result upgraded = fetch("http://localhost:" + port + "/x");
    Check("then http is changed to https before it is sent", upgraded.ended && upgraded.body == "/x" && upgraded.finalUrl == "https://localhost:" + port + "/x",
          upgraded.error.value_or(upgraded.finalUrl));

    // The upgrade applies to every hop of a redirect, not just the first.
    TestServer plain([&](int socket) {
      ++plainRequests;
      TestServer::ReadHead(socket);
      TestServer::SendAll(socket, "HTTP/1.1 302 Found\r\nLocation: http://localhost:" + port + "/landed\r\nContent-Length: 0\r\n\r\n");
    });
    Result hop = fetch("http://127.0.0.1:" + std::to_string(plain.port()) + "/");
    Check("a redirect to a host with a policy is upgraded", hop.ended && hop.body == "/landed" && hop.finalUrl == "https://localhost:" + port + "/landed",
          hop.error.value_or(hop.finalUrl));

    Result cleared = fetch("https://localhost:" + port + "/clear");
    Result gone = fetch("http://localhost:" + port + "/x");
    Check("max-age=0 takes the policy away", cleared.ended && !gone.ended, gone.error.value_or(gone.body));
  }
  {
    // Over plain http a header proves nothing: anyone on the path could have added it.
    TestServer server([](int socket) {
      while (true) {
        const std::string head = TestServer::ReadHead(socket);
        if (head.empty()) break;
        TestServer::SendAll(socket, "HTTP/1.1 200 OK\r\nStrict-Transport-Security: max-age=3600\r\nContent-Length: 2\r\n\r\nhi");
      }
    });
    auto loop = solar::test::MakeLoop();
    solar::net::HttpClient client(*loop);
    FetchOptions options;
    const std::string url = "http://localhost:" + std::to_string(server.port()) + "/";
    Collector first;
    client.Fetch(*solar::url::Parse(url), first, options);
    loop->Run();
    Collector second;
    client.Fetch(*solar::url::Parse(url), second, options);
    loop->Run();
    Check("a policy over plain http is ignored", first.result.ended && second.result.ended && second.result.finalUrl == url, second.result.finalUrl);
  }
  {
    // A policy for an address is not honoured either, whatever the response says.
    TlsTestServer server(pki.Issue({"127.0.0.1"}), [](ssl_st* connection) {
      while (!TlsTestServer::ReadHead(connection).empty()) {
        TlsTestServer::SendAll(connection, "HTTP/1.1 200 OK\r\nStrict-Transport-Security: max-age=3600\r\nContent-Length: 2\r\n\r\nhi");
      }
    });
    auto loop = solar::test::MakeLoop();
    solar::net::HttpClient client(*loop);
    FetchOptions options;
    options.tls = trusting;
    options.timeout = std::chrono::seconds(5);
    Collector first;
    client.Fetch(*solar::url::Parse("https://127.0.0.1:" + std::to_string(server.port()) + "/"), first, options);
    loop->Run();
    Collector second;
    client.Fetch(*solar::url::Parse("http://127.0.0.1:" + std::to_string(server.port()) + "/"), second, options);
    loop->Run();
    Check("an IP address gets no policy", first.result.ended && !second.result.ended, second.result.error.value_or(second.result.body));
  }

  // A request body over TLS.
  {
    TlsTestServer server(pki.Issue({"127.0.0.1"}), [](ssl_st* connection) {
      const std::string head = TlsTestServer::ReadHead(connection);
      size_t length = 0;
      if (const size_t at = head.find("Content-Length: "); at != std::string::npos) length = std::stoul(head.substr(at + 16));
      const std::string body = TlsTestServer::ReadBytes(connection, length);
      const std::string reply = head.substr(0, head.find(' ')) + " " + body;
      TlsTestServer::SendAll(connection, "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(reply.size()) + "\r\n\r\n" + reply);
    });
    auto loop = solar::test::MakeLoop();
    solar::net::HttpClient client(*loop);
    FetchOptions options;
    options.tls = trusting;
    options.method = "POST";
    options.body = std::make_shared<const std::string>(std::string(300000, 'q'));  // several TLS records
    Collector c;
    client.Fetch(*solar::url::Parse(Url(server)), c, options);
    loop->Run();
    Check("a large POST body over TLS", c.result.ended && c.result.body == "POST " + std::string(300000, 'q'), c.result.error.value_or(std::to_string(c.result.body.size())));
  }

  std::printf("tls: %d/%d passed\n", total - failed, total);
  return failed == 0 ? 0 : 1;
}
