// The network service on a thread of its own (ThreadedNetworkService): the same fetches as through the HttpClient, with the handlers told from the thread of the page's loop.

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
#include "solar/url/Parser.h"
#include "solar/url/Serializer.h"
#include "solar/url/Url.h"
#include "support/TestLoop.h"
#include "support/TestServer.h"

using solar::net::FetchHandle;
using solar::net::FetchOptions;
using solar::net::HttpResponseHead;
using solar::net::ThreadedNetworkService;
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
std::string Response(const std::string& status, const std::string& body, const std::string& extra = "") {
  return "HTTP/1.1 " + status + "\r\n" + extra + "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

TestServer::Script Serves(std::function<std::string(const std::string& path)> respond) {
  return [respond = std::move(respond)](int client) {
    while (true) {
      const std::string head = TestServer::ReadHead(client);
      if (head.empty()) break;
      TestServer::SendAll(client, respond(Path(head)));
    }
  };
}

// What a page's handler sees, and the thread it saw it on.
struct Seen : solar::net::FetchHandler {
  std::thread::id pageThread = std::this_thread::get_id();
  bool offThread = false;
  int heads = 0, status = 0, terminal = 0, redirects = 0, restarts = 0;
  std::string body, finalUrl;
  bool ended = false;
  std::optional<std::string> error;
  bool follow = true;       // the answer to a redirect asked at once
  bool allowRestart = true;
  std::function<void()> onHead;

  void Note() { if (std::this_thread::get_id() != pageThread) offThread = true; }
  bool OnRedirect(const HttpResponseHead&, const solar::url::Url&, std::vector<std::pair<std::string, std::string>>& headers) override {
    Note();
    ++redirects;
    headers.emplace_back("X-Hop", std::to_string(redirects));
    return follow;
  }
  void OnResponseHead(const HttpResponseHead& head, const solar::url::Url& url) override {
    Note();
    ++heads;
    status = head.status;
    finalUrl = solar::url::Serialize(url);
    if (onHead) onHead();
  }
  void OnBody(std::span<const uint8_t> data) override {
    Note();
    body.append(reinterpret_cast<const char*>(data.data()), data.size());
  }
  void OnEnd() override { Note(); ++terminal; ended = true; }
  void OnError(std::string_view message) override { Note(); ++terminal; error = std::string(message); }
  bool OnRestart() override {
    Note();
    ++restarts;
    if (allowRestart) body.clear();
    return allowRestart;
  }
};

// A page loop that is run until the fetches are over, as a page waits for what it asked.
struct Page {
  std::unique_ptr<solar::net::Loop> loop = solar::test::MakeLoop();
  std::unique_ptr<ThreadedNetworkService> service = std::make_unique<ThreadedNetworkService>(*loop);
  void Run() { loop->Run(); }
};

}  // namespace

int main() {
  {
    TestServer server(Serves([](const std::string& path) { return Response("200 OK", "hello " + path); }));
    Page page;
    Seen seen;
    page.service->Fetch(*solar::url::Parse(Origin(server) + "/x"), seen);
    page.Run();
    Check("a fetch is answered", seen.ended && seen.status == 200 && seen.body == "hello /x" && seen.heads == 1, seen.error.value_or(seen.body));
    Check("and the handler is told from the thread of the page's loop", !seen.offThread);
    Check("the end is the last", seen.terminal == 1);
  }
  {
    // Many at once, and a body that is more than one read.
    const std::string big(3 << 20, 'z');
    TestServer server(Serves([&](const std::string& path) { return Response("200 OK", path == "/big" ? big : path); }));
    Page page;
    std::vector<std::unique_ptr<Seen>> all;
    for (int i = 0; i < 40; ++i) {
      all.push_back(std::make_unique<Seen>());
      page.service->Fetch(*solar::url::Parse(Origin(server) + (i % 10 == 0 ? "/big" : "/p" + std::to_string(i))), *all.back());
    }
    page.Run();
    bool every = true, intact = true;
    for (int i = 0; i < 40; ++i) {
      every = every && all[i]->ended && !all[i]->offThread && all[i]->terminal == 1;
      intact = intact && (i % 10 == 0 ? all[i]->body == big : all[i]->body == "/p" + std::to_string(i));
    }
    Check("forty fetches at once all end, each once, on the page's thread", every);
    Check("and what each came to is its own, a body of 3 MB whole", intact);
  }
  {
    // A redirect answered at once by a handler that does not defer: asked on the page's thread, and the headers it changes go on.
    std::mutex mutex;
    std::vector<std::string> heads;
    TestServer server(Serves([&](const std::string& path) {
      if (path == "/a") return Response("302 Found", "", "Location: /b\r\n");
      return Response("200 OK", "arrived");
    }));
    Page page;
    Seen seen;
    page.service->Fetch(*solar::url::Parse(Origin(server) + "/a"), seen);
    page.Run();
    Check("a redirect is asked of the handler and followed", seen.redirects == 1 && seen.ended && seen.body == "arrived" && seen.finalUrl == Origin(server) + "/b", seen.error.value_or(seen.finalUrl));
    Check("on the page's thread", !seen.offThread);
    Seen refused;
    refused.follow = false;
    page.service->Fetch(*solar::url::Parse(Origin(server) + "/a"), refused);
    page.Run();
    Check("a redirect it refuses ends the fetch", !refused.ended && refused.error == std::optional<std::string>("the redirect was refused"), refused.error.value_or("followed"));
  }
  {
    // A redirect answered later, by a handler that defers.
    struct Deferring : Seen {
      ThreadedNetworkService* service = nullptr;
      solar::net::FetchHandle handle;
      std::string next;
      bool DefersRedirects() const override { return true; }
      void OnReceiveRedirect(const HttpResponseHead&, const solar::url::Url& to, const std::vector<std::pair<std::string, std::string>>&) override {
        Note();
        ++redirects;
        next = solar::url::Serialize(to);
        service->FollowRedirect(handle.id(), true, {{"X-Page", "decided"}});
      }
    };
    TestServer server(Serves([&](const std::string& path) {
      if (path == "/a") return Response("302 Found", "", "Location: /b\r\n");
      return Response("200 OK", "arrived");
    }));
    Page page;
    Deferring seen;
    seen.service = page.service.get();
    seen.handle = page.service->Fetch(*solar::url::Parse(Origin(server) + "/a"), seen);
    page.Run();
    Check("a handler that defers is told of the redirect and answers it", seen.redirects == 1 && seen.next == Origin(server) + "/b" && seen.ended && seen.body == "arrived", seen.error.value_or(seen.body));
  }
  {
    // Cancelled: the handler hears of it before Cancel returns, and nothing after.
    std::atomic<bool> release{false};
    TestServer server([&](int client) {
      TestServer::ReadHead(client);
      while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    });
    Page page;
    Seen seen;
    FetchHandle handle = page.service->Fetch(*solar::url::Parse(Origin(server) + "/hang"), seen);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    handle.Cancel();
    Check("a cancelled fetch ends with an abort, at once", seen.error == std::optional<std::string>("aborted") && seen.terminal == 1);
    handle.Cancel();
    page.Run();
    Check("and is not told of again", seen.terminal == 1);
    release = true;
  }
  {
    // A response that stalls is asked for again, and what the handler had is dropped.
    std::atomic<int> connections{0};
    TestServer server([&](int client) {
      const int n = ++connections;
      if (TestServer::ReadHead(client).empty()) return;
      if (n == 1) {
        TestServer::SendAll(client, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\npart");  // (the rest never comes)
        TestServer::WaitForClose(client);
        return;
      }
      TestServer::SendAll(client, Response("200 OK", "the whole"));
    });
    Page page;
    Seen seen;
    FetchOptions options;
    options.stall = std::chrono::milliseconds(200);
    options.timeout = std::chrono::milliseconds(3000);
    page.service->Fetch(*solar::url::Parse(Origin(server) + "/slow"), seen, options);
    page.Run();
    Check("a stall is asked of the handler on the page's thread and the response asked for again", seen.restarts == 1 && connections.load() == 2, std::to_string(seen.restarts));
    Check("the handler sees the second response whole, not after the part of the first", seen.ended && seen.body == "the whole" && !seen.offThread, seen.error.value_or(seen.body));
  }
  std::printf("network thread: %d/%d passed\n", total - failed, total);
  return failed == 0 ? 0 : 1;
}
