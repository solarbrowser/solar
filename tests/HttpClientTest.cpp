#include <atomic>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "solar/net/HttpClient.h"
#include "solar/url/Parser.h"
#include "solar/url/Serializer.h"
#include "support/Compress.h"
#include "support/TestLoop.h"
#include "support/TestServer.h"

namespace {

using namespace std::chrono_literals;
using solar::net::FetchHandle;
using solar::net::FetchOptions;
using solar::net::HttpClient;
using solar::net::HttpClientOptions;
using solar::net::HttpResponseHead;
using solar::net::Loop;
using solar::test::TestServer;

struct Result {
  std::optional<int> status;
  std::string finalUrl;
  std::string body;
  int heads = 0;
  bool ended = false;
  std::optional<std::string> error;
  int terminalCalls = 0;
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
  }
  void OnError(std::string_view message) override {
    ++result.terminalCalls;
    result.error = std::string(message);
  }
  Result result;
};

// Counts the body and keeps none of it, and checks it is the byte it should be.
class Counting : public solar::net::FetchHandler {
 public:
  void OnResponseHead(const HttpResponseHead&, const solar::url::Url&) override {}
  void OnBody(std::span<const uint8_t> data) override {
    ++calls;
    bytes += data.size();
    for (uint8_t byte : data) allZero = allZero && byte == 0;
  }
  void OnEnd() override { ++terminalCalls; ended = true; }
  void OnError(std::string_view message) override { ++terminalCalls; error = std::string(message); }
  size_t bytes = 0;
  size_t calls = 0;
  bool allZero = true;
  bool ended = false;
  int terminalCalls = 0;
  std::optional<std::string> error;
};

// A loop and a client that outlive several fetches, so a connection can be reused between them.
struct Session {
  std::unique_ptr<Loop> loop;
  std::unique_ptr<HttpClient> client;  // declared after the loop, so destroyed before it

  explicit Session(HttpClientOptions options = {}) : loop(solar::test::MakeLoop()), client(std::make_unique<HttpClient>(*loop, options)) {}

  Result Get(const std::string& url, FetchOptions options = {}) {
    Collector collector;
    client->Fetch(*solar::url::Parse(url), collector, std::move(options));
    loop->Run();
    return collector.result;
  }
};

int total = 0;
int failed = 0;

void Check(const std::string& name, bool ok, const std::string& detail = "") {
  ++total;
  if (ok) return;
  ++failed;
  std::printf("FAIL %s %s\n", name.c_str(), detail.c_str());
}

std::string Origin(const TestServer& server) { return "http://127.0.0.1:" + std::to_string(server.port()); }

std::string Path(const std::string& head) {
  const size_t start = head.find(' ');
  const size_t end = head.find(' ', start + 1);
  return head.substr(start + 1, end - start - 1);
}

std::string Response(const std::string& status, const std::string& body, const std::string& extra = "") {
  return "HTTP/1.1 " + status + "\r\n" + extra + "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

std::string Redirect(const std::string& status, const std::string& location, const std::string& body = "") {
  return Response(status, body, "Location: " + location + "\r\n");
}

struct Log {
  std::mutex mutex;
  std::vector<std::string> heads;
  std::atomic<int> connections{0};
  std::atomic<int> closed{0};

  void Add(const std::string& head) {
    std::lock_guard<std::mutex> lock(mutex);
    heads.push_back(head);
  }
  size_t Count() {
    std::lock_guard<std::mutex> lock(mutex);
    return heads.size();
  }
  std::string Head(size_t i) {
    std::lock_guard<std::mutex> lock(mutex);
    return i < heads.size() ? heads[i] : "";
  }
};

// Answers requests on one connection, as many as arrive, until the client closes it.
TestServer::Script Serves(Log& log, std::function<std::string(const std::string& path)> respond, bool closeAfterEach = false) {
  return [&log, respond = std::move(respond), closeAfterEach](int client) {
    ++log.connections;
    while (true) {
      std::string head = TestServer::ReadHead(client);
      if (head.empty()) break;
      log.Add(head);
      TestServer::SendAll(client, respond(Path(head)));
      if (closeAfterEach) break;
    }
    ++log.closed;
  };
}

bool Has(const std::string& head, const std::string& text) { return head.find(text) != std::string::npos; }

std::string Sample() {
  std::string s;
  for (int i = 0; i < 4000; ++i) s += "sample line " + std::to_string(i * 7919 % 1000) + "\n";
  return s;
}

}  // namespace

int main() {
  // ---- Redirects ----
  {
    Log log;
    TestServer server(Serves(log, [](const std::string& path) {
      if (path == "/a") return Redirect("301 Moved Permanently", "/b", "THE REDIRECT BODY");
      if (path == "/b") return Redirect("302 Found", "/c");
      return Response("200 OK", "done");
    }));
    Session s;
    Result r = s.Get(Origin(server) + "/a");
    Check("redirect chain is followed", r.ended && r.body == "done" && r.status == 200 && r.finalUrl == Origin(server) + "/c", r.error.value_or(r.finalUrl));
    Check("only the final response is shown", r.heads == 1 && r.terminalCalls == 1);
    Check("a redirect's body is not shown", r.body.find("REDIRECT") == std::string::npos);
    Check("one connection serves every hop", log.connections == 1 && log.Count() == 3, std::to_string(log.connections));
  }
  for (const char* status : {"303 See Other", "307 Temporary Redirect", "308 Permanent Redirect"}) {
    Log log;
    TestServer server(Serves(log, [&](const std::string& path) {
      return path == "/ok" ? Response("200 OK", "ok") : Redirect(status, "/ok");
    }));
    Session s;
    Result r = s.Get(Origin(server) + "/start");
    Check(std::string("follows ") + status, r.ended && r.body == "ok" && r.status == 200, r.error.value_or(""));
  }
  {
    Log log;
    TestServer* self = nullptr;
    TestServer server(Serves(log, [&](const std::string& path) {
      const std::string origin = Origin(*self);
      if (path == "/dir/start") return Redirect("302", "../up");
      if (path == "/up") return Redirect("302", "//127.0.0.1:" + std::to_string(self->port()) + "/schemeless");
      if (path == "/schemeless") return Redirect("302", origin + "/absolute");
      if (path == "/absolute") return Redirect("302", "?q=1");
      return Response("200 OK", path);
    }));
    self = &server;
    Session s;
    Result r = s.Get(Origin(server) + "/dir/start");
    Check("relative, scheme-relative, absolute and query-only targets", r.ended && r.body == "/absolute?q=1", r.error.value_or(r.body));
  }
  {
    Log log;
    TestServer server(Serves(log, [](const std::string& path) {
      if (path == "/a") return Redirect("302", "/b");
      if (path == "/b") return Redirect("302", "/c#new");
      return Response("200 OK", "x");
    }));
    Session s;
    Result carried = s.Get(Origin(server) + "/a#frag");
    Check("a redirect with a fragment replaces it", carried.finalUrl == Origin(server) + "/c#new", carried.finalUrl);
    Result kept = s.Get(Origin(server) + "/b#orig");
    Check("a redirect without one keeps the original", kept.finalUrl == Origin(server) + "/c#new", kept.finalUrl);
    Check("the fragment is never sent", !Has(log.Head(0), "#") && !Has(log.Head(0), "frag"), log.Head(0));
    Log plain;
    TestServer plainServer(Serves(plain, [](const std::string& path) { return path == "/a" ? Redirect("302", "/b") : Response("200 OK", "x"); }));
    Result inherited = s.Get(Origin(plainServer) + "/a#keep");
    Check("the original fragment survives a redirect that has none", inherited.finalUrl == Origin(plainServer) + "/b#keep", inherited.finalUrl);
  }
  {
    Log log;
    TestServer server(Serves(log, [](const std::string&) { return Redirect("302", "/loop"); }));
    Session s;
    FetchOptions options;
    options.maxRedirects = 3;
    Result r = s.Get(Origin(server) + "/loop", options);
    Check("a redirect loop is cut off", r.error == std::optional<std::string>("too many redirects") && !r.ended, r.error.value_or(""));
    Check("the limit is the number of redirects followed", log.Count() == 4, std::to_string(log.Count()));
  }
  for (const char* target : {"javascript:alert(1)", "ftp://127.0.0.1/x", "http://", "file:///etc/passwd", "data:text/plain,x"}) {
    Log log;
    TestServer server(Serves(log, [&](const std::string&) { return Redirect("302", target); }));
    Session s;
    Result r = s.Get(Origin(server) + "/");
    Check(std::string("redirect to ") + target + " is refused", r.error.has_value() && !r.ended, r.error.value_or("followed"));
  }
  {
    Log log;
    TestServer server(Serves(log, [](const std::string& path) {
      if (path == "/none") return Response("301 Moved Permanently", "none");
      return Response("300 Multiple Choices", "choices", "Location: /elsewhere\r\n");
    }));
    Session s;
    Result noLocation = s.Get(Origin(server) + "/none");
    Check("a redirect status without Location is a response", noLocation.ended && noLocation.status == 301 && noLocation.body == "none");
    Result multiple = s.Get(Origin(server) + "/many");
    Check("300 is not followed", multiple.ended && multiple.status == 300 && multiple.body == "choices");
  }
  {
    // Authorization must not follow a redirect to another origin.
    Log destination;
    TestServer other(Serves(destination, [](const std::string&) { return Response("200 OK", "arrived"); }));
    Log origin;
    TestServer server(Serves(origin, [&](const std::string& path) {
      if (path == "/away") return Redirect("302", Origin(other) + "/target");
      if (path == "/here") return Redirect("302", "/target");
      return Response("200 OK", "local");
    }));
    Session s;
    FetchOptions options;
    options.headers = {{"Authorization", "Bearer secret"}, {"X-Custom", "kept"}};
    Result away = s.Get(Origin(server) + "/away", options);
    Check("the redirect to another origin is followed", away.ended && away.body == "arrived", away.error.value_or(""));
    Check("Authorization is dropped there", !Has(destination.Head(0), "Authorization") && !Has(destination.Head(0), "secret"), destination.Head(0));
    Check("other headers are kept there", Has(destination.Head(0), "X-Custom: kept"), destination.Head(0));
    Check("Authorization was sent to the first origin", Has(origin.Head(0), "Authorization: Bearer secret"), origin.Head(0));

    Result here = s.Get(Origin(server) + "/here", options);
    Check("the same origin keeps Authorization", here.ended && Has(origin.Head(origin.Count() - 1), "Authorization: Bearer secret"), origin.Head(origin.Count() - 1));
  }

  // ---- Headers ----
  {
    Log log;
    TestServer server(Serves(log, [](const std::string&) { return Response("200 OK", "x"); }));
    Session s;
    FetchOptions options;
    options.headers = {{"User-Agent", "Custom/1"}, {"Accept", "text/html"}, {"X-Two", "2"}};
    Result r = s.Get(Origin(server) + "/", options);
    const std::string head = log.Head(0);
    Check("custom headers are sent", r.ended && Has(head, "X-Two: 2\r\n") && Has(head, "User-Agent: Custom/1\r\n") && Has(head, "Accept: text/html\r\n"), head);
    Check("a given header replaces the default", !Has(head, "User-Agent: Solar") && !Has(head, "Accept: */*"), head);

    const std::vector<std::pair<std::string, std::string>> refused = {
        {"X", "a\r\nInjected: yes"}, {"X", "a\nb"}, {"X", std::string("a\0b", 3)}, {"Bad Name", "v"}, {"", "v"}, {"X:Y", "v"},
        {"Host", "evil.example"}, {"Connection", "upgrade"}, {"Content-Length", "5"}, {"Transfer-Encoding", "chunked"}, {"Accept-Encoding", "gzip"}};
    const size_t before = log.Count();
    for (const auto& [name, value] : refused) {
      FetchOptions bad;
      bad.headers = {{name, value}};
      Result refusedResult = s.Get(Origin(server) + "/", bad);
      Check("refuses the header '" + name + "'", refusedResult.error.has_value() && !refusedResult.ended, refusedResult.error.value_or("sent"));
    }
    Check("a refused header sends nothing", log.Count() == before, std::to_string(log.Count() - before));
  }

  // ---- User-Agent ----
  {
    const std::string fallback = solar::net::DefaultUserAgent();
    Check("the default claims to be Chrome and says what it is", fallback.starts_with("Mozilla/5.0 (") && fallback.find(" Chrome/") != std::string::npos &&
              fallback.ends_with(" Solar/Developer") && fallback.find_first_of("\r\n") == std::string::npos, fallback);

    Log log;
    TestServer server(Serves(log, [](const std::string&) { return Response("200 OK", "x"); }));
    Session defaults;
    defaults.Get(Origin(server) + "/");
    Check("a request carries the default", Has(log.Head(0), "User-Agent: " + fallback + "\r\n"), log.Head(0));

    HttpClientOptions configured;
    configured.userAgent = "Custom/9.9 (from settings)";
    Session custom(configured);
    custom.Get(Origin(server) + "/");
    Check("the client's setting replaces it", Has(log.Head(1), "User-Agent: Custom/9.9 (from settings)\r\n") && !Has(log.Head(1), "Chrome/"), log.Head(1));

    FetchOptions own;
    own.headers = {{"User-Agent", "PerFetch/1"}};
    custom.Get(Origin(server) + "/", own);
    Check("a fetch's own header wins over the setting", Has(log.Head(2), "User-Agent: PerFetch/1\r\n") && !Has(log.Head(2), "Custom/9.9"), log.Head(2));

    HttpClientOptions injected;
    injected.userAgent = "Bad\r\nInjected: yes";
    Session refused(injected);
    const size_t before = log.Count();
    Result bad = refused.Get(Origin(server) + "/");
    Check("a User-Agent that would break the request is refused", bad.error == std::optional<std::string>("invalid User-Agent") && log.Count() == before,
          bad.error.value_or("sent"));
  }

  // ---- Connections ----
  {
    Log log;
    TestServer server(Serves(log, [](const std::string& path) { return Response("200 OK", path); }));
    Session s;
    Result first = s.Get(Origin(server) + "/one");
    Result second = s.Get(Origin(server) + "/two");
    Check("keep-alive: both succeed", first.body == "/one" && second.body == "/two");
    Check("keep-alive: one connection", log.connections == 1, std::to_string(log.connections));
  }
  {
    Log log;
    TestServer server(Serves(log, [](const std::string&) { return Response("200 OK", "x", "Connection: close\r\n"); }, true));
    Session s;
    s.Get(Origin(server) + "/");
    Result second = s.Get(Origin(server) + "/");
    Check("Connection: close is not reused", second.ended && log.connections == 2, std::to_string(log.connections));
  }
  {
    Log log;
    TestServer server([&](int client) {
      ++log.connections;
      TestServer::ReadHead(client);
      TestServer::SendAll(client, "HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nhi");
    });
    Session s;
    s.Get(Origin(server) + "/");
    Result second = s.Get(Origin(server) + "/");
    Check("HTTP/1.0 is not reused", second.ended && log.connections == 2, std::to_string(log.connections));
  }
  {
    Log log;
    TestServer server([&](int client) {
      ++log.connections;
      TestServer::ReadHead(client);
      TestServer::SendAll(client, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nuntil close");
    });
    Session s;
    Result first = s.Get(Origin(server) + "/");
    s.Get(Origin(server) + "/");
    Check("a body that runs until close is not reused", first.body == "until close" && log.connections == 2, std::to_string(log.connections));
  }
  {
    // The server closes each connection after answering without saying so, so the client's idle
    // connection is dead by the time it is used again.
    Log log;
    TestServer server(Serves(log, [](const std::string& path) { return Response("200 OK", path); }, true));
    Session s;
    Result first = s.Get(Origin(server) + "/one");
    Result second = s.Get(Origin(server) + "/two");
    Check("a connection the server closed is replaced", first.body == "/one" && second.body == "/two" && second.ended, second.error.value_or(""));
    Check("replaced with a fresh one", log.connections == 2, std::to_string(log.connections));
  }
  {
    Log a, b;
    TestServer serverA(Serves(a, [](const std::string&) { return Response("200 OK", "a"); }));
    TestServer serverB(Serves(b, [](const std::string&) { return Response("200 OK", "b"); }));
    Session s;
    s.Get(Origin(serverA) + "/");
    s.Get(Origin(serverB) + "/");
    Result again = s.Get(Origin(serverA) + "/");
    Check("connections are kept per origin", again.body == "a" && a.connections == 1 && b.connections == 1, std::to_string(a.connections) + "/" + std::to_string(b.connections));
  }
  {
    Log log;
    TestServer server(Serves(log, [](const std::string&) { return Response("200 OK", "x"); }));
    HttpClientOptions options;
    options.idleTimeout = 100ms;
    Session s(options);
    s.Get(Origin(server) + "/");
    s.loop->PostDelayed(400ms, [] {});  // lets the idle timeout pass inside Run
    s.loop->Run();
    Check("an idle connection is closed after its timeout", log.closed == 1, std::to_string(log.closed));
    Result again = s.Get(Origin(server) + "/");
    Check("and the next request opens another", again.ended && log.connections == 2, std::to_string(log.connections));
  }
  {
    Log log;
    TestServer server(Serves(log, [](const std::string&) { return Response("200 OK", "x"); }));
    Session s;
    s.Get(Origin(server) + "/");
    s.client->CloseIdleConnections();
    s.loop->PostDelayed(100ms, [] {});
    s.loop->Run();
    Check("CloseIdleConnections closes them", log.closed == 1, std::to_string(log.closed));
  }
  {
    // A server that sends something nobody asked for on an idle connection.
    Log log;
    TestServer server([&](int client) {
      ++log.connections;
      TestServer::ReadHead(client);
      TestServer::SendAll(client, Response("200 OK", "x"));
      std::this_thread::sleep_for(50ms);
      TestServer::SendAll(client, "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nevil");
      TestServer::WaitForClose(client);
      ++log.closed;
    });
    Session s;
    s.Get(Origin(server) + "/");
    s.loop->PostDelayed(200ms, [] {});
    s.loop->Run();
    Check("unsolicited data closes an idle connection", log.closed == 1, std::to_string(log.closed));
    Result next = s.Get(Origin(server) + "/");
    Check("and it is not used for the next request", next.ended && log.connections == 2, std::to_string(log.connections));
  }

  {
    // A second response smuggled in behind the first leaves the connection out of step with the
    // server, so it must not be used again.
    Log log;
    TestServer server([&](int client) {
      const int index = log.connections++;
      TestServer::ReadHead(client);
      if (index == 0) {
        TestServer::SendAll(client, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nokHTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nevil");
        TestServer::WaitForClose(client);
        return;
      }
      TestServer::SendAll(client, Response("200 OK", "real"));
    });
    Session s;
    Result first = s.Get(Origin(server) + "/");
    FetchOptions options;
    options.timeout = 1000ms;
    Result second = s.Get(Origin(server) + "/", options);
    Check("a smuggled response is not delivered", first.body == "ok", first.body);
    Check("and the connection is not reused", second.ended && second.body == "real" && log.connections == 2, second.error.value_or(second.body));
  }

  // ---- Content-Encoding ----
  {
    std::string original;
    for (int i = 0; i < 3000; ++i) original += "line " + std::to_string(i) + " of a body that compresses well\n";
    struct Case {
      const char* coding;
      std::string (*encode)(std::string_view);
    };
    for (const Case& c : {Case{"gzip", solar::test::Gzip}, Case{"deflate", solar::test::ZlibDeflate}, Case{"br", solar::test::Brotli},
                          Case{"zstd", solar::test::Zstd}}) {
      Log log;
      const std::string packed = c.encode(original);
      TestServer server(Serves(log, [&](const std::string&) {
        return Response("200 OK", packed, std::string("Content-Encoding: ") + c.coding + "\r\n");
      }));
      Session s;
      Collector collector;
      s.client->Fetch(*solar::url::Parse(Origin(server) + "/"), collector);
      s.loop->Run();
      Check(std::string("decodes ") + c.coding, collector.result.ended && collector.result.body == original, collector.result.error.value_or("wrong body"));
      Check(std::string("Accept-Encoding offers ") + c.coding, Has(log.Head(0), "Accept-Encoding: gzip, deflate, br, zstd\r\n"), log.Head(0));
    }
  }
  {
    // Content-Length is the compressed length, and the head says so; the body is the decoded one.
    const std::string original(10000, 'x');
    const std::string packed = solar::test::Gzip(original);
    Log log;
    TestServer server(Serves(log, [&](const std::string&) { return Response("200 OK", packed, "Content-Encoding: gzip\r\n"); }));
    Session s;
    Collector collector;
    std::optional<HttpResponseHead> seen;
    struct Head : Collector {
      std::optional<HttpResponseHead>* out;
      void OnResponseHead(const HttpResponseHead& h, const solar::url::Url& u) override {
        *out = h;
        Collector::OnResponseHead(h, u);
      }
    } withHead;
    withHead.out = &seen;
    s.client->Fetch(*solar::url::Parse(Origin(server) + "/"), withHead);
    s.loop->Run();
    Check("the head keeps Content-Encoding and Content-Length", seen && seen->Header("content-encoding") == "gzip" &&
              seen->Header("content-length") == std::to_string(packed.size()));
    Check("and the body is decoded", withHead.result.body == original && original.size() != packed.size());
  }
  {
    const std::string original = std::string(50000, 'q') + "tail";
    const std::string packed = solar::test::Brotli(solar::test::Gzip(original));
    Log log;
    TestServer server(Serves(log, [&](const std::string&) { return Response("200 OK", packed, "Content-Encoding: gzip, br\r\n"); }));
    Session s;
    Result r = s.Get(Origin(server) + "/");
    Check("two codings are undone in reverse", r.ended && r.body == original, r.error.value_or("wrong body"));
  }
  {
    // Chunked on the outside, gzip inside: the chunk edges are nowhere near the gzip's.
    const std::string original(200000, 'z');
    const std::string packed = solar::test::Gzip(original);
    TestServer server([&](int client) {
      TestServer::ReadHead(client);
      std::string out = "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nTransfer-Encoding: chunked\r\n\r\n";
      for (size_t at = 0; at < packed.size(); at += 13) {
        const std::string piece = packed.substr(at, 13);
        char size[16];
        std::snprintf(size, sizeof(size), "%zx", piece.size());
        out += std::string(size) + "\r\n" + piece + "\r\n";
      }
      out += "0\r\n\r\n";
      TestServer::SendAll(client, out);
    });
    Session s;
    Result r = s.Get(Origin(server) + "/");
    Check("gzip inside chunked", r.ended && r.body == original, r.error.value_or("wrong body"));
  }
  {
    const std::string original = "slow and compressed";
    const std::string packed = solar::test::Gzip(original);
    TestServer server([&](int client) {
      TestServer::ReadHead(client);
      TestServer::SendSlowly(client, Response("200 OK", packed, "Content-Encoding: gzip\r\n"), 1, 1);
    });
    Session s;
    Result r = s.Get(Origin(server) + "/");
    Check("a compressed response sent a byte at a time", r.ended && r.body == original, r.error.value_or(""));
  }
  {
    // 128 MiB that never exists in one piece: it passes through in the decoder's chunks.
    const std::string packed = solar::test::Gzip(std::string(128u << 20, '\0'));
    Log log;
    TestServer server(Serves(log, [&](const std::string&) { return Response("200 OK", packed, "Content-Encoding: gzip\r\n"); }));
    Session s;
    Counting counting;
    s.client->Fetch(*solar::url::Parse(Origin(server) + "/"), counting);
    s.loop->Run();
    Check("a large body streams through", counting.ended && counting.bytes == (128u << 20) && counting.allZero, counting.error.value_or(std::to_string(counting.bytes)));
    Check("in many pieces", counting.calls > 1000, std::to_string(counting.calls));
  }
  {
    // A decompression bomb: kilobytes in, gigabytes out, stopped at the limit.
    const std::string packed = solar::test::Gzip(std::string(256u << 20, '\0'));
    Log log;
    TestServer server(Serves(log, [&](const std::string&) { return Response("200 OK", packed, "Content-Encoding: gzip\r\n"); }));
    Session s;
    Counting counting;
    FetchOptions options;
    options.maxDecodedBodyBytes = 1 << 20;
    s.client->Fetch(*solar::url::Parse(Origin(server) + "/"), counting, options);
    s.loop->Run();
    Check("a bomb is stopped at the limit", counting.error && counting.error->find("size limit") != std::string::npos && !counting.ended, counting.error.value_or("not stopped"));
    Check("having delivered no more than the limit", counting.bytes <= (1u << 20), std::to_string(counting.bytes));
    Check("with one terminal call", counting.terminalCalls == 1);

    // The connection that carried a bomb is not trusted again.
    Result next = s.Get(Origin(server) + "/");
    Check("and its connection is not reused", log.connections == 2, std::to_string(log.connections));
  }
  {
    // The server says what it likes about the length; the compressed stream still has to be whole.
    const std::string packed = solar::test::Gzip(std::string(100000, 'k'));
    const std::string cut = packed.substr(0, packed.size() - 6);
    Log log;
    TestServer server(Serves(log, [&](const std::string&) { return Response("200 OK", cut, "Content-Encoding: gzip\r\n"); }));
    Session s;
    Result r = s.Get(Origin(server) + "/");
    Check("a truncated compressed body is an error", r.error && r.error->find("truncated") != std::string::npos && !r.ended, r.error.value_or("accepted"));
  }
  {
    std::string corrupt = solar::test::Gzip(Sample());
    for (size_t i = corrupt.size() / 2; i < corrupt.size() / 2 + 6; ++i) corrupt[i] = static_cast<char>(corrupt[i] ^ 0xFF);
    Log log;
    TestServer server(Serves(log, [&](const std::string&) { return Response("200 OK", corrupt, "Content-Encoding: gzip\r\n"); }));
    Session s;
    Result r = s.Get(Origin(server) + "/");
    Check("corrupt compressed data is an error", r.error.has_value() && !r.ended && r.terminalCalls == 1, r.error.value_or("accepted"));
  }
  {
    Log log;
    TestServer server(Serves(log, [](const std::string&) { return Response("200 OK", "raw bytes", "Content-Encoding: snappy\r\n"); }));
    Session s;
    Result r = s.Get(Origin(server) + "/");
    Check("a coding that is not understood leaves the body alone", r.ended && r.body == "raw bytes", r.error.value_or(""));
  }
  {
    Log log;
    TestServer server(Serves(log, [](const std::string&) { return Response("200 OK", "", "Content-Encoding: gzip\r\n"); }));
    Session s;
    Result r = s.Get(Origin(server) + "/");
    Check("an empty body with a Content-Encoding is fine", r.ended && r.body.empty(), r.error.value_or(""));
  }
  {
    // A redirect's own body is never decoded, so a bad one does not matter.
    Log log;
    TestServer server(Serves(log, [](const std::string& path) {
      if (path == "/a") return Response("302 Found", "this is not gzip", "Location: /b\r\nContent-Encoding: gzip\r\n");
      return Response("200 OK", "arrived");
    }));
    Session s;
    Result r = s.Get(Origin(server) + "/a");
    Check("a redirect with a Content-Encoding body is followed", r.ended && r.body == "arrived", r.error.value_or(""));
  }
  {
    Log log;
    const std::string packed = solar::test::Zstd(std::string(5000, 'r'));
    TestServer server(Serves(log, [&](const std::string&) { return Response("200 OK", packed, "Content-Encoding: zstd\r\n"); }));
    Session s;
    s.Get(Origin(server) + "/");
    Result again = s.Get(Origin(server) + "/");
    Check("a decoded response leaves its connection reusable", again.ended && log.connections == 1, std::to_string(log.connections));
  }

  // ---- Cancellation ----
  {
    Log log;
    TestServer server([&](int client) {
      ++log.connections;
      TestServer::ReadHead(client);
      TestServer::WaitForClose(client);  // never answers
      ++log.closed;
    });
    Session s;
    Collector collector;
    FetchHandle handle = s.client->Fetch(*solar::url::Parse(Origin(server) + "/"), collector);
    s.loop->PostDelayed(60ms, [&] { handle.Cancel(); });
    s.loop->Run();
    Check("cancel ends the fetch with 'aborted'", collector.result.error == std::optional<std::string>("aborted") && collector.result.terminalCalls == 1,
          collector.result.error.value_or("no error"));
    s.loop->PostDelayed(100ms, [] {});
    s.loop->Run();
    Check("cancel closes the connection", log.closed == 1, std::to_string(log.closed));
    handle.Cancel();
    Check("cancelling again does nothing", collector.result.terminalCalls == 1);
  }
  {
    Log log;
    TestServer server(Serves(log, [](const std::string&) { return Response("200 OK", "x"); }));
    Session s;
    Collector collector;
    FetchHandle handle = s.client->Fetch(*solar::url::Parse(Origin(server) + "/"), collector);
    handle.Cancel();  // before anything has started
    s.loop->Run();
    Check("cancel before the fetch starts", collector.result.error == std::optional<std::string>("aborted") && log.connections == 0,
          std::to_string(log.connections));
  }
  {
    Log log;
    TestServer server(Serves(log, [](const std::string&) { return Response("200 OK", "x"); }));
    Session s;
    Collector collector;
    FetchHandle handle = s.client->Fetch(*solar::url::Parse(Origin(server) + "/"), collector);
    s.loop->Run();
    handle.Cancel();
    Check("cancelling a finished fetch changes nothing", collector.result.ended && collector.result.terminalCalls == 1);
  }

  // ---- Many at once ----
  {
    Log log;
    TestServer server(Serves(log, [](const std::string& path) { return Response("200 OK", "echo " + path); }));
    Session s;
    std::vector<Collector> collectors(30);
    for (size_t i = 0; i < collectors.size(); ++i) {
      s.client->Fetch(*solar::url::Parse(Origin(server) + "/item" + std::to_string(i)), collectors[i]);
    }
    s.loop->Run();
    bool all = true;
    for (size_t i = 0; i < collectors.size(); ++i) {
      all = all && collectors[i].result.ended && collectors[i].result.body == "echo /item" + std::to_string(i);
    }
    Check("30 fetches at once each get their own response", all);
    Check("each used a connection of its own", log.connections == 30, std::to_string(log.connections));
    Result reuse = s.Get(Origin(server) + "/after");
    Check("one of them is reused afterwards", reuse.ended && log.connections == 30, std::to_string(log.connections));
  }

  std::printf("http client: %d/%d passed\n", total - failed, total);
  return failed == 0 ? 0 : 1;
}
