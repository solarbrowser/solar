// The rules of fetching as the network service applies them to what a page asks (PageRequest): CORS and its preflights, the headers a browser writes itself, the filtering of a
// response, Opaque Response Blocking, redirects. Run through the HttpClient and through the service on its own thread: the page hears the same.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "solar/net/HttpClient.h"
#include "solar/net/NetworkThread.h"
#include "solar/net/PageFetch.h"
#include "solar/url/Parser.h"
#include "solar/url/Serializer.h"
#include "support/TestLoop.h"
#include "support/TestServer.h"

using solar::net::PageRequest;
using solar::net::PageResponse;
using solar::net::RequestCredentials;
using solar::net::RequestMode;
using solar::net::ResponseType;
using solar::test::TestServer;

namespace {

int total = 0, failed = 0;
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
std::string Method(const std::string& head) { return head.substr(0, head.find(' ')); }
std::string Response(const std::string& status, const std::string& body, const std::string& extra = "") {
  return "HTTP/1.1 " + status + "\r\n" + extra + "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}
bool Has(const std::string& head, const std::string& needle) {
  std::string lower = head, wanted = needle;
  for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (char& c : wanted) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return lower.find(wanted) != std::string::npos;
}

// A server that keeps what it was asked.
struct Recording {
  std::mutex mutex;
  std::vector<std::string> heads;
  std::function<std::string(const std::string& head)> respond;
  void Add(const std::string& head) {
    std::lock_guard<std::mutex> lock(mutex);
    heads.push_back(head);
  }
  size_t Count() { std::lock_guard<std::mutex> lock(mutex); return heads.size(); }
  std::string Head(size_t i) { std::lock_guard<std::mutex> lock(mutex); return i < heads.size() ? heads[i] : ""; }
};

TestServer::Script Serves(Recording& log) {
  return [&log](int client) {
    while (true) {
      const std::string head = TestServer::ReadHead(client);
      if (head.empty()) break;
      log.Add(head);
      TestServer::SendAll(client, log.respond(head));
    }
  };
}

struct Heard : solar::net::PageFetchHandler {
  std::thread::id pageThread = std::this_thread::get_id();
  bool offThread = false;
  std::optional<PageResponse> response;
  std::string body;
  bool ended = false;
  std::optional<std::string> error;
  int terminal = 0;
  void Note() { if (std::this_thread::get_id() != pageThread) offThread = true; }
  void OnResponse(const PageResponse& r) override { Note(); response = r; }
  void OnBody(std::span<const uint8_t> data) override { Note(); body.append(reinterpret_cast<const char*>(data.data()), data.size()); }
  void OnEnd() override { Note(); ended = true; ++terminal; }
  void OnError(std::string_view message) override { Note(); error = std::string(message); ++terminal; }
  int restarts = 0;
  void OnRestart() override {
    Note();
    ++restarts;
    response.reset();
    body.clear();
  }
  bool HeaderIs(const std::string& name, const std::string& value) const {
    if (!response) return false;
    for (const auto& [n, v] : response->headers) {
      std::string lower = n;
      for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (lower == name && v == value) return true;
    }
    return false;
  }
  bool HasHeader(const std::string& name) const {
    if (!response) return false;
    for (const auto& [n, v] : response->headers) {
      std::string lower = n;
      for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (lower == name) return true;
    }
    return false;
  }
};

// The network, either way, and the loop the page is run by.
struct Env {
  std::unique_ptr<solar::net::Loop> loop = solar::test::MakeLoop();
  std::unique_ptr<solar::net::HttpClient> inlineClient;
  std::unique_ptr<solar::net::ThreadedNetworkService> threaded;
  solar::net::NetworkService* service = nullptr;
  explicit Env(bool thread) {
    if (thread) {
      threaded = std::make_unique<solar::net::ThreadedNetworkService>(*loop);
      service = threaded.get();
    } else {
      inlineClient = std::make_unique<solar::net::HttpClient>(*loop);
      service = inlineClient.get();
    }
  }
  void Run() { loop->Run(); }
};

PageRequest Ask(const std::string& page, const std::string& url, RequestMode mode = RequestMode::Cors) {
  PageRequest request;
  request.url = *solar::url::Parse(url);
  request.pageUrl = *solar::url::Parse(page);
  request.mode = mode;
  return request;
}

void RunAll(bool thread) {
  const std::string via = thread ? " (service thread)" : " (client)";
  {
    // The same origin: all of the response is the page's.
    Recording log;
    log.respond = [](const std::string&) { return Response("200 OK", "hello", "X-Secret: yes\r\nContent-Type: text/plain\r\n"); };
    TestServer server(Serves(log));
    Env env(thread);
    Heard heard;
    env.service->StartFetch(Ask(Origin(server) + "/page", Origin(server) + "/data", RequestMode::Cors), heard);
    env.Run();
    Check("a same-origin fetch is a basic response, whole" + via, heard.response && heard.response->type == ResponseType::Basic && heard.response->status == 200 && heard.body == "hello" && heard.HeaderIs("x-secret", "yes") && heard.ended, heard.error.value_or(heard.body));
    Check("and no Origin is written for a GET that stays at home" + via, !Has(log.Head(0), "Origin:"), log.Head(0));
    Check("it is told from the page's thread, once" + via, !heard.offThread && heard.terminal == 1);
  }
  {
    // Another origin, cors mode: refused without the server's say, allowed with it, with only the headers it exposes.
    Recording log;
    log.respond = [](const std::string& head) {
      if (Path(head) == "/open") return Response("200 OK", "shared", "Access-Control-Allow-Origin: *\r\nAccess-Control-Expose-Headers: x-shown\r\nX-Shown: a\r\nX-Hidden: b\r\nContent-Type: text/plain\r\n");
      return Response("200 OK", "private");
    };
    TestServer server(Serves(log));
    TestServer page(Serves(log));
    Env env(thread);
    Heard closed, open;
    env.service->StartFetch(Ask(Origin(page) + "/", Origin(server) + "/closed"), closed);
    env.service->StartFetch(Ask(Origin(page) + "/", Origin(server) + "/open"), open);
    env.Run();
    Check("a cors response without Access-Control-Allow-Origin is an error" + via, !closed.response && closed.error == std::optional<std::string>("Failed to fetch: the response was not allowed by CORS"), closed.error.value_or("answered"));
    Check("one with it is a cors response" + via, open.response && open.response->type == ResponseType::Cors && open.body == "shared", open.error.value_or(open.body));
    Check("of which the page sees the safelisted headers and those the server exposes, no others" + via, open.HeaderIs("x-shown", "a") && open.HasHeader("content-type") && !open.HasHeader("x-hidden"));
    bool origin = false, site = false;
    for (size_t i = 0; i < log.Count(); ++i) {
      origin = origin || Has(log.Head(i), "Origin: " + Origin(page));
      site = site || Has(log.Head(i), "Sec-Fetch-Mode: cors");
    }
    Check("the Origin of the page goes with a cross-origin request, and the Sec-Fetch headers the browser writes" + via, origin && site, log.Head(0));
  }
  {
    // A request that is not simple asks first.
    Recording log;
    log.respond = [](const std::string& head) {
      if (Method(head) == "OPTIONS") {
        if (Path(head) == "/denied") return Response("200 OK", "", "Access-Control-Allow-Origin: *\r\n");
        return Response("204 No Content", "", "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: PUT\r\nAccess-Control-Allow-Headers: x-custom\r\nAccess-Control-Max-Age: 60\r\n");
      }
      return Response("200 OK", "stored", "Access-Control-Allow-Origin: *\r\n");
    };
    TestServer server(Serves(log));
    TestServer page(Serves(log));
    Env env(thread);
    PageRequest request = Ask(Origin(page) + "/", Origin(server) + "/put");
    request.method = "PUT";
    request.headers = {{"X-Custom", "1"}};
    request.body = std::make_shared<const std::string>("payload");
    Heard allowed;
    env.service->StartFetch(request, allowed);
    env.Run();
    Check("a PUT with a custom header asks first, and goes when it is allowed" + via, allowed.response && allowed.body == "stored" && log.Count() == 2 && Method(log.Head(0)) == "OPTIONS" && Method(log.Head(1)) == "PUT", std::to_string(log.Count()));
    Check("the preflight names what the request wants" + via, Has(log.Head(0), "Access-Control-Request-Method: PUT") && Has(log.Head(0), "Access-Control-Request-Headers: x-custom"), log.Head(0));
    Heard again;
    env.service->StartFetch(request, again);
    env.Run();
    Check("what the preflight allowed is remembered for the next" + via, again.response && log.Count() == 3, std::to_string(log.Count()));
    PageRequest denied = request;
    denied.url = *solar::url::Parse(Origin(server) + "/denied");
    Heard refused;
    env.service->StartFetch(denied, refused);
    env.Run();
    Check("a preflight that does not allow it ends the fetch" + via, !refused.response && refused.error && refused.error->find("CORS preflight failed") != std::string::npos, refused.error.value_or("answered"));
  }
  {
    // No-cors: the page is given nothing of a response from another origin, and no body.
    Recording log;
    log.respond = [](const std::string& head) {
      if (Path(head) == "/page") return Response("200 OK", "<html>secret</html>", "Content-Type: text/html\r\nX-Content-Type-Options: nosniff\r\n");
      return Response("200 OK", "binary-ish", "Content-Type: image/png\r\n");
    };
    TestServer server(Serves(log));
    TestServer page(Serves(log));
    Env env(thread);
    Heard opaque, blocked;
    env.service->StartFetch(Ask(Origin(page) + "/", Origin(server) + "/image", RequestMode::NoCors), opaque);
    env.service->StartFetch(Ask(Origin(page) + "/", Origin(server) + "/page", RequestMode::NoCors), blocked);
    env.Run();
    Check("a no-cors response from another origin is opaque, with no status, no headers and no body" + via, opaque.response && opaque.response->type == ResponseType::Opaque && opaque.response->status == 0 && opaque.response->headers.empty() && opaque.body.empty() && opaque.ended, opaque.error.value_or(""));
    Check("one that Opaque Response Blocking blocks is an error" + via, !blocked.response && blocked.error && blocked.error->find("ORB") != std::string::npos, blocked.error.value_or("answered"));
  }
  {
    // What the page loads for itself is given whole, and what Opaque Response Blocking blocks is not.
    Recording log;
    log.respond = [](const std::string& head) {
      if (Path(head) == "/image") return Response("200 OK", "PNGDATA", "Content-Type: image/png\r\nX-Other: 1\r\n");
      if (Path(head) == "/html") return Response("404 Not Found", "<html>nope</html>", "Content-Type: text/html\r\nX-Content-Type-Options: nosniff\r\n");
      return Response("200 OK", "whatever", "Content-Type: text/html\r\n");
    };
    TestServer server(Serves(log));
    TestServer page(Serves(log));
    Env env(thread);
    auto load = [&](const char* path, solar::net::RequestDestination destination, Heard& heard) {
      PageRequest request = Ask(Origin(page) + "/", Origin(server) + path, RequestMode::NoCors);
      request.destination = destination;
      request.credentials = RequestCredentials::Include;
      env.service->StartFetch(request, heard);
    };
    Heard image, blocked, other;
    load("/image", solar::net::RequestDestination::Image, image);
    load("/html", solar::net::RequestDestination::Image, blocked);
    load("/html", solar::net::RequestDestination::Other, other);
    env.Run();
    Check("a picture the page loads for itself is given whole: its status, its headers, its body" + via, image.response && image.response->type != ResponseType::Opaque && image.response->status == 200 && image.HeaderIs("x-other", "1") && image.body == "PNGDATA", image.error.value_or(image.body));
    Check("and the Sec-Fetch-Dest says what it is for" + via, [&] { for (size_t i = 0; i < log.Count(); ++i) if (Path(log.Head(i)) == "/image") return Has(log.Head(i), "Sec-Fetch-Dest: image"); return false; }());
    Check("HTML for a picture is blocked, though the page is given the errors it makes" + via, !blocked.response && blocked.error && blocked.error->find("ORB") != std::string::npos, blocked.error.value_or("answered"));
    Check("a load that is not yet named is not blocked" + via, other.response && other.body == "<html>nope</html>", other.error.value_or(other.body));
  }
  {
    // The document of a frame is a navigation: nothing of CORS or of ORB, and the metadata says so.
    Recording log;
    log.respond = [](const std::string&) { return Response("200 OK", "<html>a frame</html>", "Content-Type: text/html\r\nX-Content-Type-Options: nosniff\r\n"); };
    TestServer server(Serves(log));
    TestServer page(Serves(log));
    Env env(thread);
    PageRequest request = Ask(Origin(page) + "/", Origin(server) + "/frame", RequestMode::Navigate);
    request.destination = solar::net::RequestDestination::Document;
    request.credentials = RequestCredentials::Include;
    Heard heard;
    env.service->StartFetch(request, heard);
    env.Run();
    Check("the document of a frame is given whole, whatever it is" + via, heard.response && heard.body == "<html>a frame</html>" && heard.response->type != ResponseType::Opaque, heard.error.value_or(heard.body));
    Check("with Sec-Fetch-Mode navigate and Sec-Fetch-Dest iframe" + via, Has(log.Head(0), "Sec-Fetch-Mode: navigate") && Has(log.Head(0), "Sec-Fetch-Dest: iframe"), log.Head(0));
  }
  {
    // The Origin of a WebSocket handshake is the page's own address' origin, not what the page says.
    Recording log;
    log.respond = [](const std::string&) { return Response("400 Bad Request", "no"); };
    TestServer server(Serves(log));
    Env env(thread);
    struct Socket : solar::net::WebSocketHandler {
      bool closed = false;
      void OnOpen(const std::string&, const std::string&) override {}
      void OnMessage(std::string, bool) override {}
      void OnClosed(bool, uint16_t, const std::string&, const std::string&) override { closed = true; }
    } socket;
    solar::net::WebSocketOptions options;
    options.origin = "http://evil.example";
    options.initiator = *solar::url::Parse("http://page.example/some/page");
    env.service->ConnectWebSocket(*solar::url::Parse("ws://127.0.0.1:" + std::to_string(server.port()) + "/"), socket, options);
    env.Run();
    Check("a WebSocket handshake carries the origin of the page the service knows" + via, Has(log.Head(0), "Origin: http://page.example") && !Has(log.Head(0), "evil.example") && socket.closed, log.Head(0));
  }
  {
    // A font or a module is asked for with CORS: the server has to allow the page.
    Recording log;
    log.respond = [](const std::string& head) {
      if (Path(head) == "/allowed") return Response("200 OK", "FONT", "Access-Control-Allow-Origin: *\r\n");
      return Response("200 OK", "FONT");
    };
    TestServer server(Serves(log));
    TestServer page(Serves(log));
    Env env(thread);
    auto load = [&](const char* path, RequestCredentials credentials, Heard& heard) {
      PageRequest request = Ask(Origin(page) + "/", Origin(server) + path, RequestMode::Cors);
      request.destination = solar::net::RequestDestination::Font;
      request.credentials = credentials;
      env.service->StartFetch(request, heard);
    };
    Heard allowed, closed, withCookies;
    load("/allowed", RequestCredentials::SameOrigin, allowed);
    load("/closed", RequestCredentials::SameOrigin, closed);
    load("/allowed", RequestCredentials::Include, withCookies);
    env.Run();
    Check("a font from a server that allows everyone is the page's" + via, allowed.response && allowed.body == "FONT", allowed.error.value_or(allowed.body));
    Check("one from a server that does not say is refused" + via, !closed.response && closed.error && closed.error->find("CORS") != std::string::npos, closed.error.value_or("answered"));
    Check("and \"everyone\" is not enough for a request that carries credentials" + via, !withCookies.response && withCookies.error, withCookies.error.value_or("answered"));
  }
  {
    // A response that stalls is asked for again, for a page that can start over.
    std::atomic<int> connections{0};
    TestServer server([&](int client) {
      const int n = ++connections;
      if (TestServer::ReadHead(client).empty()) return;
      if (n == 1) {
        TestServer::SendAll(client, "HTTP/1.1 200 OK\r\nContent-Type: text/css\r\nContent-Length: 20\r\n\r\npartial");
        TestServer::WaitForClose(client);
        return;
      }
      TestServer::SendAll(client, Response("200 OK", "the whole style", "Content-Type: text/css\r\n"));
    });
    Env env(thread);
    PageRequest request = Ask(Origin(server) + "/", Origin(server) + "/slow", RequestMode::NoCors);
    request.destination = solar::net::RequestDestination::Style;
    request.restartable = true;
    request.stall = std::chrono::milliseconds(200);
    request.timeout = std::chrono::milliseconds(3000);
    Heard heard;
    env.service->StartFetch(request, heard);
    env.Run();
    Check("a stalled response is asked for again and the page hears it from its start" + via, heard.restarts == 1 && heard.body == "the whole style" && heard.ended && connections.load() == 2, heard.error.value_or(heard.body) + " restarts " + std::to_string(heard.restarts));
  }
  {
    // A redirect in cors mode: each server along the way has to allow it.
    Recording log;
    log.respond = [](const std::string& head) {
      if (Path(head) == "/start") return Response("302 Found", "", "Location: /end\r\nAccess-Control-Allow-Origin: *\r\n");
      if (Path(head) == "/start-quiet") return Response("302 Found", "", "Location: /end\r\n");
      return Response("200 OK", "landed", "Access-Control-Allow-Origin: *\r\n");
    };
    TestServer server(Serves(log));
    TestServer page(Serves(log));
    Env env(thread);
    Heard good, bad;
    env.service->StartFetch(Ask(Origin(page) + "/", Origin(server) + "/start"), good);
    env.service->StartFetch(Ask(Origin(page) + "/", Origin(server) + "/start-quiet"), bad);
    env.Run();
    Check("a redirect that the server allows is followed" + via, good.response && good.body == "landed" && good.response->redirected, good.error.value_or(good.body));
    Check("one that it does not is an error" + via, !bad.response && bad.error == std::optional<std::string>("Failed to fetch: a redirect was not allowed by CORS"), bad.error.value_or("followed"));
  }
  {
    // The referrer under the policy, and a request that must not leave home.
    Recording log;
    log.respond = [](const std::string&) { return Response("200 OK", "x", "Access-Control-Allow-Origin: *\r\n"); };
    TestServer server(Serves(log));
    Env env(thread);
    PageRequest request = Ask(Origin(server) + "/dir/page?q=1#frag", Origin(server) + "/r");
    request.sendsReferrer = true;
    request.referrerPolicy = "origin";
    Heard heard;
    env.service->StartFetch(request, heard);
    PageRequest none = Ask(Origin(server) + "/dir/page", Origin(server) + "/none");
    Heard silent;
    env.service->StartFetch(none, silent);
    PageRequest sameOrigin = Ask(Origin(server) + "/", "http://localhost:" + std::to_string(server.port()) + "/x", RequestMode::SameOrigin);
    Heard refused;
    env.service->StartFetch(sameOrigin, refused);
    env.Run();
    bool referer = false, quiet = true;
    for (size_t i = 0; i < log.Count(); ++i) {
      if (Path(log.Head(i)) == "/r") referer = Has(log.Head(i), "Referer: " + Origin(server) + "/");
      if (Path(log.Head(i)) == "/none") quiet = !Has(log.Head(i), "Referer:");
    }
    Check("the Referer is made from the page's address under the policy" + via, referer, log.Head(0));
    Check("and there is none for a request that has no referrer" + via, quiet);
    Check("a same-origin request to another origin is refused, and nothing is sent" + via, !refused.response && refused.error == std::optional<std::string>("Failed to fetch: a same-origin request to another origin"), refused.error.value_or("answered"));
  }
  {
    // Cancelled.
    std::atomic<bool> release{false};
    TestServer server([&](int client) {
      TestServer::ReadHead(client);
      while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    });
    Env env(thread);
    Heard heard;
    solar::net::FetchHandle handle = env.service->StartFetch(Ask(Origin(server) + "/", Origin(server) + "/hang"), heard);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    handle.Cancel();
    Check("a fetch the page cancels hears an abort at once, once" + via, heard.error == std::optional<std::string>("aborted") && heard.terminal == 1);
    env.Run();
    Check("and nothing after" + via, heard.terminal == 1);
    release = true;
  }
}

}  // namespace

int main() {
  RunAll(false);
  RunAll(true);
  std::printf("page fetch: %d/%d passed\n", total - failed, total);
  return failed == 0 ? 0 : 1;
}
