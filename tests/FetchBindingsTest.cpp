// fetch() end to end: a script in a Quanta realm, the network layer, and servers on the loopback. The
// tests themselves are tests/js/Fetch.js, run by the same harness as the WPT files.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "quanta/Embed.h"
#include "solar/net/HttpClient.h"
#include "solar/url/Parser.h"
#include "solar/web/DomBindings.h"
#include "solar/web/FetchBindings.h"
#include "solar/web/FetchHost.h"
#include "solar/web/JsEventLoop.h"
#include "solar/web/UrlBindings.h"
#include "support/TestLoop.h"
#include "support/TestServer.h"

namespace {

namespace qe = Quanta::Embed;
using solar::test::TestServer;
using namespace std::chrono_literals;

bool ReadFile(const std::string& path, std::string& out) {
  std::ifstream file(path);
  if (!file) return false;
  std::stringstream buffer;
  buffer << file.rdbuf();
  out = buffer.str();
  return true;
}

std::string JsonEscape(const std::string& text) {
  std::string out = "\"";
  for (unsigned char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c < 0x20) {
      char buffer[8];
      std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
      out += buffer;
    } else {
      out += static_cast<char>(c);
    }
  }
  return out + "\"";
}

struct Request {
  std::string head;
  std::string body;
  std::string method, path;

  std::string Header(const std::string& name) const {
    std::string lowered = head;
    for (char& c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string wanted = "\r\n" + name + ": ";
    const size_t at = lowered.find(wanted);
    if (at == std::string::npos) return "";
    const size_t start = at + wanted.size();
    return head.substr(start, head.find("\r\n", start) - start);
  }
};

std::string Reply(const std::string& status, const std::string& body, const std::string& extra = "") {
  return "HTTP/1.1 " + status + "\r\n" + extra + "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

struct Servers {
  std::atomic<int> cacheable{0};
  std::atomic<int> preflights{0};
  std::mutex mutex;
  std::string lastPreflight = "{}";
  std::string originA, originB;
};

// Answers requests on one connection until the client closes it.
void Serve(int client, Servers& servers, bool isA) {
  while (true) {
    Request request;
    request.head = TestServer::ReadHead(client);
    if (request.head.empty()) break;
    request.method = request.head.substr(0, request.head.find(' '));
    const size_t start = request.head.find(' ') + 1;
    request.path = request.head.substr(start, request.head.find(' ', start) - start);
    const std::string length = request.Header("content-length");
    if (!length.empty()) request.body = TestServer::ReadBytes(client, std::stoul(length));

    const std::string cors = isA ? "" : "Access-Control-Allow-Origin: " + servers.originA + "\r\n";
    std::string response;
    const std::string& path = request.path;
    if (request.method == "OPTIONS" && !isA) {
      ++servers.preflights;
      {
        std::lock_guard<std::mutex> lock(servers.mutex);
        servers.lastPreflight = "{\"origin\":" + JsonEscape(request.Header("origin")) + ",\"method\":" + JsonEscape(request.Header("access-control-request-method")) +
                                ",\"headers\":" + JsonEscape(request.Header("access-control-request-headers")) + ",\"cookie\":" + JsonEscape(request.Header("cookie")) + "}";
      }
      const std::string allow = "Access-Control-Allow-Origin: " + servers.originA + "\r\n";
      if (path == "/preflight-500") {
        response = Reply("500 Internal Server Error", "", allow);
      } else if (path == "/preflight-redirect") {
        response = Reply("302 Found", "", allow + "Location: /text\r\n");
      } else if (path == "/preflight-no-origin") {
        response = Reply("204 No Content", "", "Access-Control-Allow-Methods: PUT\r\n");
      } else if (path == "/preflight-bad-headers") {
        response = Reply("204 No Content", "", allow + "Access-Control-Allow-Methods: PUT, DELETE\r\n");
      } else if (path == "/preflight-star") {
        response = Reply("204 No Content", "", allow + "Access-Control-Allow-Methods: *\r\nAccess-Control-Allow-Headers: *\r\n");
      } else {
        response = Reply("204 No Content", "", allow + "Access-Control-Allow-Methods: PUT, DELETE, PATCH\r\nAccess-Control-Allow-Headers: content-type, x-test, authorization\r\nAccess-Control-Max-Age: 60\r\n");
      }
      TestServer::SendAll(client, response);
      continue;
    }
    if (path == "/preflight-count") {
      response = Reply("200 OK", std::to_string(servers.preflights.load()), cors);
    } else if (path == "/preflight-last") {
      std::lock_guard<std::mutex> lock(servers.mutex);
      response = Reply("200 OK", servers.lastPreflight, cors + "Content-Type: application/json\r\n");
    } else if (path == "/text") {
      response = Reply("200 OK", "hello", cors + "Content-Type: text/plain\r\nX-Custom: yes\r\nSet-Cookie: hidden=1\r\n");
    } else if (path == "/json") {
      response = Reply("200 OK", "{\"a\":1,\"b\":[true,null]}", cors + "Content-Type: application/json\r\n");
    } else if (path == "/bytes") {
      std::string bytes(256, '\0');
      for (int i = 0; i < 256; ++i) bytes[i] = static_cast<char>(i);
      response = Reply("200 OK", bytes, cors + "Content-Type: application/octet-stream\r\n");
    } else if (path == "/echo") {
      response = Reply("200 OK",
                       "{\"method\":" + JsonEscape(request.method) + ",\"contentType\":" + JsonEscape(request.Header("content-type")) + ",\"length\":" +
                           JsonEscape(request.Header("content-length")) + ",\"body\":" + JsonEscape(request.body) + ",\"origin\":" + JsonEscape(request.Header("origin")) +
                           ",\"referer\":" + JsonEscape(request.Header("referer")) + ",\"cookie\":" + JsonEscape(request.Header("cookie")) + ",\"x\":" +
                           JsonEscape(request.Header("x-test")) + "}",
                       cors + "Content-Type: application/json\r\n");
    } else if (path.starts_with("/status/")) {
      response = Reply(path.substr(8) + " Status", "status body", cors);
    } else if (path.starts_with("/redirect/")) {
      const size_t slash = path.find('/', 10);
      const std::string code = path.substr(10, slash - 10);
      response = Reply(code + " Redirect", "redirecting", cors + "Location: " + path.substr(slash) + "\r\n");
    } else if (path == "/slow") {
      std::this_thread::sleep_for(800ms);
      response = Reply("200 OK", "late", cors);
    } else if (path == "/trickle") {
      TestServer::SendAll(client, "HTTP/1.1 200 OK\r\n" + cors + "Content-Length: 10\r\n\r\n01234");
      std::this_thread::sleep_for(800ms);
      response = "56789";
    } else if (path == "/cookie-set") {
      response = Reply("200 OK", "set", cors + "Set-Cookie: s=1; Path=/\r\n");
    } else if (path == "/cookie-get") {
      response = Reply("200 OK", request.Header("cookie").empty() ? "none" : request.Header("cookie"), cors);
    } else if (path == "/cacheable") {
      response = Reply("200 OK", std::to_string(++servers.cacheable), cors + "Cache-Control: max-age=60\r\n");
    } else if (path == "/cors-credentials") {
      response = Reply("200 OK", "with credentials", "Access-Control-Allow-Origin: " + servers.originA + "\r\nAccess-Control-Allow-Credentials: true\r\n");
    } else if (path == "/cors-star") {
      response = Reply("200 OK", "star", "Access-Control-Allow-Origin: *\r\n");
    } else if (path == "/cors-none") {
      response = Reply("200 OK", "no cors headers");
    } else if (path == "/cors-expose") {
      response = Reply("200 OK", "exposed", cors + "Access-Control-Expose-Headers: X-Exposed\r\nX-Exposed: 1\r\nX-Hidden: 2\r\nContent-Type: text/plain\r\n");
    } else {
      response = Reply("404 Not Found", "nothing here", cors);
    }
    TestServer::SendAll(client, response);
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::setbuf(stdout, nullptr);
  const std::string file = argc > 1 ? argv[1] : "tests/js/Fetch.js";
#ifndef _WIN32
  std::signal(SIGPIPE, SIG_IGN);
#endif
  std::string harness, test;
  if (!ReadFile("tests/wpt/harness.js", harness) || !ReadFile(file, test)) {
    std::fprintf(stderr, "cannot read the harness or %s\n", file.c_str());
    return 2;
  }

  Servers servers;
  TestServer serverA([&](int client) { Serve(client, servers, true); });
  TestServer serverB([&](int client) { Serve(client, servers, false); });
  servers.originA = "http://127.0.0.1:" + std::to_string(serverA.port());
  servers.originB = "http://localhost:" + std::to_string(serverB.port());

  // In the order they must be destroyed in, last first.
  auto runtime = qe::Runtime::Create();
  auto loop = solar::test::MakeLoop();
  solar::net::HttpClient client(*loop);
  solar::web::InstallUrlApis(*runtime);
  solar::web::InstallDomApis(*runtime);
  solar::web::InstallFetchApis(*runtime);
  solar::web::JsEventLoop events(*loop, {[&] { runtime->PerformMicrotaskCheckpoint(); }, [&] { runtime->RunDueTimers(); },
                                         [&] { return runtime->NextTimerDelayMs(); }});
  solar::web::FetchHost::Config config;
  config.loop = loop.get();
  config.client = &client;
  config.pageUrl = *solar::url::Parse(servers.originA + "/page");
  config.afterScript = [&] { events.AfterScript(); };
  solar::web::FetchHost host(runtime->GetContext(), config);

  const std::string setup = "globalThis.__skip = []; globalThis.__resources = {}; globalThis.SERVER = \"" + servers.originA + "\"; globalThis.OTHER = \"" +
                            servers.originB + "\";";
  qe::Runtime::Result result = runtime->Evaluate(setup + harness, "harness.js");
  if (result.ok) result = runtime->Evaluate(test, file);
  if (!result.ok) {
    std::printf("FAIL while loading: %s\n", result.error.c_str());
    return 1;
  }
  runtime->PerformMicrotaskCheckpoint();
  events.Run(60s);
  result = runtime->Evaluate("__wptFinish()", file);
  return result.ok ? 0 : 1;
}
