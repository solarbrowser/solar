#include <atomic>
#include <csignal>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "solar/net/HttpClient.h"
#include "solar/url/Parser.h"
#include "solar/url/Serializer.h"
#include "support/Compress.h"
#include "support/Http2TestServer.h"
#include "support/TestLoop.h"

namespace {

using namespace std::chrono_literals;
using solar::net::FetchHandle;
using solar::net::FetchOptions;
using solar::net::HttpClient;
using solar::net::HttpClientOptions;
using solar::net::HttpResponseHead;
using solar::net::Loop;
using solar::test::H2Reply;
using solar::test::H2Request;
using solar::test::Http2TestServer;
using solar::test::TestPki;
using solar::test::TlsTestServer;

struct Result {
  std::optional<int> status;
  std::string finalUrl;
  std::string body;
  int heads = 0;
  bool ended = false;
  std::optional<std::string> error;
  int terminalCalls = 0;
  std::chrono::steady_clock::time_point finishedAt{};
};

class Collector : public solar::net::FetchHandler {
 public:
  void OnResponseHead(const HttpResponseHead& head, const solar::url::Url& url) override {
    result.status = head.status;
    result.finalUrl = solar::url::Serialize(url);
    ++result.heads;
  }
  void OnBody(std::span<const uint8_t> data) override { result.body.append(reinterpret_cast<const char*>(data.data()), data.size()); }
  void OnEnd() override {
    ++result.terminalCalls;
    result.ended = true;
    result.finishedAt = std::chrono::steady_clock::now();
  }
  void OnError(std::string_view message) override {
    ++result.terminalCalls;
    result.error = std::string(message);
    result.finishedAt = std::chrono::steady_clock::now();
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

// A loop and a client that trust the test authority and outlive several fetches.
struct Session {
  std::unique_ptr<Loop> loop;
  std::unique_ptr<HttpClient> client;  // destroyed before the loop
  FetchOptions fetch;

  Session(const TestPki& pki, HttpClientOptions options = {}) : loop(solar::test::MakeLoop()) {
    solar::net::TlsOptions tls;
    tls.extraRootsPem = pki.rootPem();
    fetch.tls = solar::net::TlsContext::Create(tls);
    client = std::make_unique<HttpClient>(*loop, std::move(options));
  }

  void Start(const std::string& url, Collector& collector) { client->Fetch(*solar::url::Parse(url), collector, fetch); }

  Result Get(const std::string& url) {
    Collector collector;
    Start(url, collector);
    loop->Run();
    return collector.result;
  }
};

std::string Url(const Http2TestServer& server, const std::string& path = "/") { return "https://127.0.0.1:" + std::to_string(server.port()) + path; }

std::string Mentioned(const Result& r) { return r.error.value_or("ok"); }

bool Contains(const std::optional<std::string>& text, const std::string& part) { return text && text->find(part) != std::string::npos; }

long long Ms(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
}

std::string Pattern(size_t size) {
  std::string out(size, '\0');
  for (size_t i = 0; i < size; ++i) out[i] = static_cast<char>('a' + (i * 7 + i / 251) % 26);
  return out;
}

H2Reply Plain(const std::string& body) {
  H2Reply reply;
  reply.body = body;
  return reply;
}

}  // namespace

int main() {
#ifndef _WIN32
  // The server side writes through OpenSSL, which does not ask to be spared a SIGPIPE when the
  // client has already hung up on a stream it cancelled.
  std::signal(SIGPIPE, SIG_IGN);
#endif
  TestPki pki;
  const auto identity = pki.Issue({"127.0.0.1"});

  // The basics: a request, what it carries, what comes back.
  {
    Http2TestServer server(identity, [](const H2Request& r) { return Plain("hello " + r.path); });
    Session s(pki);
    Result r = s.Get(Url(server, "/greeting?x=1"));
    Check("a GET over HTTP/2", r.ended && r.status == 200 && r.body == "hello /greeting?x=1" && r.heads == 1 && r.terminalCalls == 1, Mentioned(r));
    const auto seen = server.requests();
    Check("the server saw one request", seen.size() == 1);
    if (!seen.empty()) {
      const H2Request& request = seen[0];
      Check("the pseudo-headers", request.scheme == "https" && request.authority == "127.0.0.1:" + std::to_string(server.port()), request.authority);
      Check("the default headers are there, in lower case",
            request.Header("user-agent").starts_with("Solar/") && request.Header("accept") == "*/*" &&
                request.Header("accept-encoding") == "gzip, deflate, br, zstd");
      bool connectionSpecific = false;
      for (const auto& [name, value] : request.headers) {
        connectionSpecific = connectionSpecific || name == "host" || name == "connection" || name == "transfer-encoding" || name == "keep-alive";
        for (char c : name) connectionSpecific = connectionSpecific || (c >= 'A' && c <= 'Z');
      }
      Check("no connection-specific or upper-case headers", !connectionSpecific);
    }
  }
  {
    Http2TestServer server(identity, [](const H2Request& r) { return Plain(r.Header("x-probe")); });
    Session s(pki);
    s.fetch.headers = {{"X-Probe", "mixed Case Value"}};
    Result r = s.Get(Url(server));
    Check("a header the caller sets goes out, its name in lower case", r.ended && r.body == "mixed Case Value", Mentioned(r));
  }

  // One connection serves many requests, one after another and at once.
  {
    Http2TestServer server(identity, [](const H2Request& r) { return Plain("echo " + r.path); });
    Session s(pki);
    Result a = s.Get(Url(server, "/a"));
    Result b = s.Get(Url(server, "/b"));
    Check("two fetches in turn", a.ended && b.ended && a.body == "echo /a" && b.body == "echo /b", Mentioned(a) + " " + Mentioned(b));
    Check("share one connection", server.connections() == 1, std::to_string(server.connections()));
  }
  {
    Http2TestServer server(identity, [](const H2Request& r) {
      H2Reply reply = Plain("echo " + r.path);
      reply.delay = 150ms;  // long enough for all of them to be in flight together
      return reply;
    });
    Session s(pki);
    std::vector<Collector> collectors(20);
    for (size_t i = 0; i < collectors.size(); ++i) s.Start(Url(server, "/item" + std::to_string(i)), collectors[i]);
    s.loop->Run();
    bool all = true;
    for (size_t i = 0; i < collectors.size(); ++i) all = all && collectors[i].result.ended && collectors[i].result.body == "echo /item" + std::to_string(i);
    Check("20 fetches at once each get their own response", all);
    Check("on one connection", server.connections() == 1, std::to_string(server.connections()));
    Check("and the server had them open together", server.maxConcurrent() == 20, std::to_string(server.maxConcurrent()));
  }
  {
    Http2TestServer server(identity, [](const H2Request& r) {
      H2Reply reply = Plain(r.path);
      if (r.path == "/slow") reply.delay = 400ms;
      return reply;
    });
    Session s(pki);
    Collector slow;
    Collector fast;
    s.Start(Url(server, "/slow"), slow);
    s.Start(Url(server, "/fast"), fast);
    s.loop->Run();
    Check("a slow response does not hold up a fast one on the same connection",
          fast.result.ended && slow.result.ended && fast.result.finishedAt < slow.result.finishedAt &&
              Ms(fast.result.finishedAt, slow.result.finishedAt) > 200 && server.connections() == 1,
          std::to_string(server.connections()));
  }

  // Redirects and bodies.
  {
    Http2TestServer server(identity, [](const H2Request& r) {
      H2Reply reply = Plain("");
      if (r.path == "/a") {
        reply.status = 301;
        reply.headers = {{"location", "/b"}};
        reply.body = "THE REDIRECT BODY";
      } else if (r.path == "/b") {
        reply.status = 302;
        reply.headers = {{"location", "/c"}};
      } else {
        reply.body = "done";
      }
      return reply;
    });
    Session s(pki);
    Result r = s.Get(Url(server, "/a"));
    Check("redirects are followed", r.ended && r.body == "done" && r.finalUrl == Url(server, "/c") && r.heads == 1, Mentioned(r) + " " + r.finalUrl);
    Check("on the same connection", server.connections() == 1, std::to_string(server.connections()));
  }
  {
    const std::string big = Pattern(5 << 20);  // far beyond the windows, so they have to be reopened
    Http2TestServer server(identity, [&](const H2Request&) { return Plain(big); });
    Session s(pki);
    Result r = s.Get(Url(server));
    Check("a 5 MiB body arrives whole", r.ended && r.body == big, Mentioned(r) + " " + std::to_string(r.body.size()));
  }
  {
    const std::string text = Pattern(200000);
    Http2TestServer server(identity, [&](const H2Request&) {
      H2Reply reply = Plain(solar::test::Gzip(text));
      reply.headers = {{"content-encoding", "gzip"}};
      return reply;
    });
    Session s(pki);
    Result r = s.Get(Url(server));
    Check("a compressed body is decoded", r.ended && r.body == text, Mentioned(r));
  }
  {
    Http2TestServer server(identity, [](const H2Request&) {
      H2Reply reply = Plain("after the interim response");
      reply.interim = {103};
      reply.trailers = {{"x-trailer", "ignored"}};
      return reply;
    });
    Session s(pki);
    Result r = s.Get(Url(server));
    Check("an interim response is not shown and trailers are not an error",
          r.ended && r.status == 200 && r.heads == 1 && r.body == "after the interim response" && r.terminalCalls == 1,
          Mentioned(r) + " status=" + std::to_string(r.status.value_or(-1)) + " heads=" + std::to_string(r.heads) + " body=" + r.body +
              " calls=" + std::to_string(r.terminalCalls));
  }

  // A stream that goes wrong is that stream's problem.
  {
    Http2TestServer server(identity, [](const H2Request& r) {
      H2Reply reply = Plain(r.path == "/reset" ? Pattern(100000) : "fine");
      if (r.path == "/reset") reply.resetAfter = 1000;
      else reply.delay = 100ms;
      return reply;
    });
    Session s(pki);
    Collector reset;
    Collector other;
    s.Start(Url(server, "/reset"), reset);
    s.Start(Url(server, "/other"), other);
    s.loop->Run();
    Check("a reset stream is an error that says so", !reset.result.ended && Contains(reset.result.error, "reset"), Mentioned(reset.result));
    Check("the stream beside it is unharmed", other.result.ended && other.result.body == "fine", Mentioned(other.result));
    Check("on the one connection", server.connections() == 1, std::to_string(server.connections()));
  }
  for (size_t claimed : {size_t(3), size_t(100)}) {
    Http2TestServer server(identity, [&](const H2Request&) {
      H2Reply reply = Plain("12345");
      reply.contentLength = claimed;
      return reply;
    });
    Session s(pki);
    Result r = s.Get(Url(server));
    Check("a content-length that is wrong is an error (" + std::to_string(claimed) + ")", !r.ended && r.error, Mentioned(r));
  }
  {
    Http2TestServer server(identity, [](const H2Request& r) {
      H2Reply reply = Plain("ok");
      if (r.path == "/many") {
        for (int i = 0; i < 40; ++i) reply.headers.emplace_back("x-filler-" + std::to_string(i), std::string(2000, 'v'));  // 80 KB, a field at a time
      }
      if (r.path == "/one") reply.headers = {{"x-big", std::string(100 * 1024, 'v')}};
      return reply;
    });
    Session s(pki);
    Result many = s.Get(Url(server, "/many"));
    Check("response headers that add up to too much are refused", !many.ended && Contains(many.error, "too large") && many.heads == 0, Mentioned(many));
    Result after = s.Get(Url(server, "/fine"));
    Check("and only that stream suffers", after.ended && after.body == "ok" && server.connections() == 1, Mentioned(after) + " " + std::to_string(server.connections()));
    Result one = s.Get(Url(server, "/one"));
    Check("a single field beyond what HTTP/2 can carry is an error too", !one.ended && one.error && one.heads == 0, Mentioned(one));
    Result recovered = s.Get(Url(server, "/fine"));
    Check("and the client carries on", recovered.ended && recovered.body == "ok", Mentioned(recovered));
  }
  {
    std::atomic<bool> refused{false};
    Http2TestServer server(identity, [&](const H2Request& r) {
      H2Reply reply = Plain("served " + r.path);
      if (r.path == "/second" && !refused.exchange(true)) reply.goAwayInstead = true;
      return reply;
    });
    Session s(pki);
    Result first = s.Get(Url(server, "/first"));
    Result second = s.Get(Url(server, "/second"));
    Check("a request the server refuses with GOAWAY goes to a new connection", first.ended && second.ended && second.body == "served /second", Mentioned(second));
    Check("which is the second", server.connections() == 2, std::to_string(server.connections()));
  }
  {
    Http2TestServer server(identity, [](const H2Request&) {
      H2Reply reply;
      reply.closeConnection = true;
      return reply;
    });
    Session s(pki);
    Result r = s.Get(Url(server));
    Check("a connection dropped mid-request is an error, not a hang", !r.ended && r.error, Mentioned(r));
    Check("with one terminal call", r.terminalCalls == 1, std::to_string(r.terminalCalls));
  }

  // Cancelling, limits and idleness.
  {
    Http2TestServer server(identity, [](const H2Request& r) {
      H2Reply reply = Plain(r.path);
      reply.delay = 300ms;
      return reply;
    });
    Session s(pki);
    Collector cancelled;
    Collector kept;
    FetchHandle handle = s.client->Fetch(*solar::url::Parse(Url(server, "/cancelled")), cancelled, s.fetch);
    s.Start(Url(server, "/kept"), kept);
    s.loop->PostDelayed(50ms, [&] { handle.Cancel(); });
    s.loop->Run();
    Check("cancelling one stream ends it", cancelled.result.error == std::string("aborted") && cancelled.result.terminalCalls == 1,
          Mentioned(cancelled.result));
    Check("the other stream finishes", kept.result.ended && kept.result.body == "/kept", Mentioned(kept.result));
    Check("on the same connection", server.connections() == 1, std::to_string(server.connections()));
    Check("and the server was told to drop the cancelled one", server.resets() == 1, std::to_string(server.resets()));
  }
  {
    solar::test::H2ServerOptions options;
    options.maxConcurrentStreams = 2;
    Http2TestServer server(identity, [](const H2Request& r) {
      H2Reply reply = Plain(r.path);
      reply.delay = 60ms;
      return reply;
    }, options);
    Session s(pki);
    std::vector<Collector> collectors(6);
    for (size_t i = 0; i < collectors.size(); ++i) s.Start(Url(server, "/n" + std::to_string(i)), collectors[i]);
    s.loop->Run();
    bool all = true;
    for (size_t i = 0; i < collectors.size(); ++i) all = all && collectors[i].result.ended && collectors[i].result.body == "/n" + std::to_string(i);
    Check("more requests than the server allows at once all get through", all);
    Check("never more than it allows on one connection", server.maxConcurrent() <= 2, std::to_string(server.maxConcurrent()));
  }
  {
    Http2TestServer server(identity, [](const H2Request&) { return Plain("again"); });
    HttpClientOptions options;
    options.idleTimeout = 100ms;
    Session s(pki, options);
    Result a = s.Get(Url(server));
    // Loop::Run has returned with the connection still open; once it has idled out, a new one is made.
    s.loop->PostDelayed(400ms, [] {});
    s.loop->Run();
    Result b = s.Get(Url(server));
    Check("an idle connection is closed after the idle timeout", a.ended && b.ended && server.connections() == 2, std::to_string(server.connections()));
  }

  // Cookies over HTTP/2: the same jar, the headers one to a field.
  {
    Http2TestServer server(identity, [](const H2Request& r) {
      H2Reply reply = Plain("cookie=" + r.Header("cookie"));
      if (r.path == "/set") reply.headers = {{"set-cookie", "a=1"}, {"set-cookie", "b=2; Secure; HttpOnly"}};
      return reply;
    });
    Session s(pki);
    s.Get(Url(server, "/set"));
    Result r = s.Get(Url(server, "/after"));
    Check("cookies set over HTTP/2 come back on it", r.ended && r.body == "cookie=a=1; b=2", Mentioned(r) + " " + r.body);
  }

  // The cache over HTTP/2: a fresh response needs no stream, a stale one is checked on a new one.
  {
    Http2TestServer server(identity, [](const H2Request& r) {
      if (r.path == "/fresh") {
        H2Reply reply = Plain("fresh");
        reply.headers = {{"cache-control", "max-age=3600"}};
        return reply;
      }
      if (!r.Header("if-none-match").empty()) {
        H2Reply reply;
        reply.status = 304;
        reply.headers = {{"etag", "\"e\""}, {"cache-control", "max-age=0"}};
        return reply;
      }
      H2Reply reply = Plain("checked");
      reply.headers = {{"etag", "\"e\""}, {"cache-control", "max-age=0"}};
      return reply;
    });
    Session s(pki);
    s.Get(Url(server, "/fresh"));
    Result fresh = s.Get(Url(server, "/fresh"));
    Check("a fresh response is served without a request over HTTP/2", fresh.ended && fresh.body == "fresh" && server.requests().size() == 1, Mentioned(fresh));
    s.Get(Url(server, "/stale"));
    Result stale = s.Get(Url(server, "/stale"));
    const auto seen = server.requests();
    Check("a stale one is checked with a conditional request", stale.ended && stale.status == 200 && stale.body == "checked" && seen.size() == 3 &&
                                                                  seen[2].Header("if-none-match") == "\"e\"",
          Mentioned(stale) + " " + std::to_string(seen.size()));
  }

  // Methods and request bodies over HTTP/2.
  {
    Http2TestServer server(identity, [](const H2Request& r) {
      H2Reply reply = Plain(r.method + "|" + r.path + "|" + r.Header("content-length") + "|" + r.Header("content-type") + "|" + r.body);
      return reply;
    });
    Session s(pki);
    const auto send = [&](const std::string& method, std::shared_ptr<const std::string> body = nullptr) {
      solar::net::FetchOptions options = s.fetch;
      options.method = method;
      options.body = std::move(body);
      options.headers = {{"Content-Type", "text/plain"}};
      Collector c;
      s.client->Fetch(*solar::url::Parse(Url(server, "/m")), c, options);
      s.loop->Run();
      return c.result;
    };
    Result post = send("POST", std::make_shared<const std::string>("hello"));
    Check("a POST with a body", post.ended && post.body == "POST|/m|5|text/plain|hello", Mentioned(post) + " " + post.body);
    Result empty = send("POST");
    Check("a POST without one says its length is 0", empty.ended && empty.body == "POST|/m|0|text/plain|", Mentioned(empty) + " " + empty.body);
    Result get = send("GET");
    Check("a GET says nothing of length", get.ended && get.body == "GET|/m||text/plain|", Mentioned(get) + " " + get.body);
    Result put = send("put", std::make_shared<const std::string>("p"));
    Check("put in capitals", put.body == "PUT|/m|1|text/plain|p", put.body);
    Check("all on one connection", server.connections() == 1, std::to_string(server.connections()));
  }
  {
    std::string big(3 << 20, '\0');
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>((i * 13 + i / 97) % 251);
    Http2TestServer server(identity, [&](const H2Request& r) { return Plain(r.body == big ? "same" : "different " + std::to_string(r.body.size())); });
    Session s(pki);
    solar::net::FetchOptions options = s.fetch;
    options.method = "POST";
    options.body = std::make_shared<const std::string>(big);
    Collector c;
    s.client->Fetch(*solar::url::Parse(Url(server)), c, options);
    s.loop->Run();
    Check("a 3 MiB body goes through the flow-control windows whole", c.result.ended && c.result.body == "same", Mentioned(c.result) + " " + c.result.body);
  }
  {
    std::vector<Collector> collectors(10);
    Http2TestServer server(identity, [](const H2Request& r) {
      H2Reply reply = Plain("got " + r.body);
      reply.delay = 100ms;
      return reply;
    });
    Session s(pki);
    for (size_t i = 0; i < collectors.size(); ++i) {
      solar::net::FetchOptions options = s.fetch;
      options.method = "POST";
      options.body = std::make_shared<const std::string>("body" + std::to_string(i));
      s.client->Fetch(*solar::url::Parse(Url(server)), collectors[i], options);
    }
    s.loop->Run();
    bool all = true;
    for (size_t i = 0; i < collectors.size(); ++i) all = all && collectors[i].result.ended && collectors[i].result.body == "got body" + std::to_string(i);
    Check("ten POSTs at once each carry their own body", all && server.connections() == 1, std::to_string(server.connections()));
  }
  for (int status : {303, 307}) {
    Http2TestServer server(identity, [&](const H2Request& r) {
      if (r.path == "/start") {
        H2Reply reply = Plain("");
        reply.status = status;
        reply.headers = {{"location", "/next"}};
        return reply;
      }
      return Plain(r.method + " " + r.body);
    });
    Session s(pki);
    solar::net::FetchOptions options = s.fetch;
    options.method = "POST";
    options.body = std::make_shared<const std::string>("payload");
    Collector c;
    s.client->Fetch(*solar::url::Parse(Url(server, "/start")), c, options);
    s.loop->Run();
    Check("a " + std::to_string(status) + " redirect over HTTP/2", c.result.ended && c.result.body == (status == 307 ? "POST payload" : "GET "), Mentioned(c.result) + " " + c.result.body);
  }
  {
    std::atomic<bool> refused{false};
    Http2TestServer server(identity, [&](const H2Request& r) {
      H2Reply reply = Plain("served " + r.method + " " + r.body);
      if (r.path == "/second" && !refused.exchange(true)) reply.goAwayInstead = true;
      return reply;
    });
    Session s(pki);
    s.Get(Url(server, "/first"));
    solar::net::FetchOptions options = s.fetch;
    options.method = "POST";
    options.body = std::make_shared<const std::string>("kept");
    Collector c;
    s.client->Fetch(*solar::url::Parse(Url(server, "/second")), c, options);
    s.loop->Run();
    Check("a POST the server refused with GOAWAY is sent again, body and all", c.result.ended && c.result.body == "served POST kept" && server.connections() == 2,
          Mentioned(c.result) + " " + c.result.body + " " + std::to_string(server.connections()));
  }
  {
    Http2TestServer server(identity, [](const H2Request& r) {
      H2Reply reply;
      reply.headers = {{"x-kind", r.method}};
      reply.contentLength = 1000;
      return reply;
    });
    Session s(pki);
    solar::net::FetchOptions options = s.fetch;
    options.method = "HEAD";
    Collector c;
    s.client->Fetch(*solar::url::Parse(Url(server)), c, options);
    s.loop->Run();
    Check("a HEAD response over HTTP/2 ends at its head", c.result.ended && c.result.body.empty() && c.result.status == 200, Mentioned(c.result));
  }
  {
    // Giving up on an upload frees its stream and leaves the connection to the others.
    std::string big(8 << 20, 'u');
    Http2TestServer server(identity, [](const H2Request& r) { return Plain("done " + std::to_string(r.body.size())); });
    Session s(pki);
    solar::net::FetchOptions options = s.fetch;
    options.method = "POST";
    options.body = std::make_shared<const std::string>(big);
    Collector cancelled;
    solar::net::FetchHandle handle = s.client->Fetch(*solar::url::Parse(Url(server, "/up")), cancelled, options);
    Collector other;
    s.Start(Url(server, "/other"), other);
    s.loop->PostDelayed(5ms, [&] { handle.Cancel(); });
    s.loop->Run();
    Check("a cancelled upload is an error", cancelled.result.error == std::string("aborted") && cancelled.result.terminalCalls == 1, Mentioned(cancelled.result));
    Check("the other stream is unharmed", other.result.ended && other.result.body.starts_with("done"), Mentioned(other.result));
  }

  // HTTP/1.1 servers still work, and HTTP/2 can be turned off.
  {
    TlsTestServer server(identity, [](ssl_st* connection) {
      while (true) {
        const std::string head = TlsTestServer::ReadHead(connection);
        if (head.empty()) break;
        TlsTestServer::SendAll(connection, "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nh1ok");
      }
    });
    Session s(pki);
    std::vector<Collector> collectors(5);
    for (Collector& c : collectors) s.Start("https://127.0.0.1:" + std::to_string(server.port()) + "/", c);
    s.loop->Run();
    bool all = true;
    for (Collector& c : collectors) all = all && c.result.ended && c.result.body == "h1ok";
    Check("an HTTP/1.1 server gets every one of five simultaneous fetches", all, Mentioned(collectors[0].result));
    Check("which settled on http/1.1", server.alpn() == "http/1.1", server.alpn());
  }
  {
    TlsTestServer server(identity, [](ssl_st* connection) {
      TlsTestServer::ReadHead(connection);
      TlsTestServer::SendAll(connection, "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nh1ok");
    }, true, {"h2", "http/1.1"});
    HttpClientOptions options;
    options.http2 = false;
    Session s(pki, options);
    Result r = s.Get("https://127.0.0.1:" + std::to_string(server.port()) + "/");
    Check("with HTTP/2 turned off only HTTP/1.1 is offered", r.ended && r.body == "h1ok" && server.alpn() == "http/1.1", Mentioned(r) + " " + server.alpn());
  }

  std::printf("http2: %d/%d passed\n", total - failed, total);
  return failed == 0 ? 0 : 1;
}
