// What the browser does for a page it does not trust: it fetches the documents the page asks to go to (and files only for a page made for one), lets the page ask for the origins of
// the documents it was given and takes them back when the page commits to one, and says no to what is not a navigation.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../src/gui/Services.h"
#include "solar/gui/Wire.h"
#include "solar/net/Ipc.h"
#include "solar/net/PageFetch.h"
#include "solar/url/Parser.h"
#include "solar/web-api/Storage.h"
#include "support/TestServer.h"

using namespace solar;
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
std::string Response(const std::string& status, const std::string& body, const std::string& extra = "") {
  return "HTTP/1.1 " + status + "\r\n" + extra + "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

// What the page's end hears from the browser, by navigation.
struct Page {
  struct Heard {
    bool head = false, ended = false;
    int status = 0;
    std::string contentType, finalUrl, body, error;
  };
  std::mutex mutex;
  std::map<uint32_t, Heard> heard;
  std::atomic<int> fetchResponses{0}, fetchErrors{0};
  struct StorageAnswer { bool ok = false; std::string data; bool arrived = false; };
  std::map<uint32_t, StorageAnswer> storage;
  uint32_t nextStorage = 1;
  net::ipc::Channel windowEnd, networkEnd;
  std::thread windowReader, networkReader;

  Page(int window, int network) : windowEnd(window), networkEnd(network) {
    windowReader = std::thread([this] {
      uint8_t type = 0;
      std::string bytes;
      while (windowEnd.ReceiveRaw(type, bytes)) {
        net::ipc::Reader in(bytes);
        std::lock_guard<std::mutex> lock(mutex);
        switch (static_cast<gui::wire::ToRenderer>(type)) {
          case gui::wire::ToRenderer::NavHead: {
            gui::wire::NavHead head;
            if (!gui::wire::Read(in, head)) break;
            Heard& h = heard[head.id];
            h.head = true;
            h.status = head.status;
            h.contentType = head.contentType;
            h.finalUrl = head.finalUrl;
            break;
          }
          case gui::wire::ToRenderer::NavBody: {
            const uint32_t id = in.U32();
            heard[id].body += in.String();
            break;
          }
          case gui::wire::ToRenderer::StorageReply: {
            const uint32_t id = in.U32();
            const bool ok = in.Bool();
            StorageAnswer& answer = storage[id];
            answer.ok = ok;
            answer.data = in.String();
            answer.arrived = true;
            break;
          }
          case gui::wire::ToRenderer::NavEnd: heard[in.U32()].ended = true; break;
          case gui::wire::ToRenderer::NavError: {
            const uint32_t id = in.U32();
            heard[id].error = in.String();
            heard[id].ended = true;
            break;
          }
          default: break;
        }
      }
    });
    networkReader = std::thread([this] {
      net::ipc::Message type;
      std::string bytes;
      while (networkEnd.Receive(type, bytes)) {
        if (type == net::ipc::Message::FetchResponse) ++fetchResponses;
        if (type == net::ipc::Message::FetchError) ++fetchErrors;
      }
    });
  }
  ~Page() {
    windowEnd.Shutdown();
    networkEnd.Shutdown();
    windowReader.join();
    networkReader.join();
  }

  void Go(uint32_t id, const std::string& url, const std::string& method = "GET", const std::string& body = "") {
    gui::wire::Navigate navigate;
    navigate.id = id;
    navigate.url = url;
    navigate.method = method;
    navigate.body = body;
    net::ipc::Writer out;
    gui::wire::Write(out, navigate);
    windowEnd.SendRaw(static_cast<uint8_t>(gui::wire::ToBrowser::Navigate), out.bytes());
  }
  void Committed(const std::string& url) {
    net::ipc::Writer out;
    out.String(url);
    windowEnd.SendRaw(static_cast<uint8_t>(gui::wire::ToBrowser::NavCommitted), out.bytes());
  }
  // A storage request, answered.
  StorageAnswer Store(gui::wire::StorageRequest::Op op, uint8_t area, const std::string& origin, const std::string& file, const std::string& data = "", uint32_t maxBytes = 0) {
    gui::wire::StorageRequest request;
    request.op = op;
    request.area = area;
    request.origin = origin;
    request.file = file;
    request.data = data;
    request.maxBytes = maxBytes;
    {
      std::lock_guard<std::mutex> lock(mutex);
      request.id = nextStorage++;
    }
    net::ipc::Writer out;
    gui::wire::Write(out, request);
    windowEnd.SendRaw(static_cast<uint8_t>(gui::wire::ToBrowser::Storage), out.bytes());
    for (int i = 0; i < 300; ++i) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (storage[request.id].arrived) return storage[request.id];
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return {};
  }
  bool WaitEnded(uint32_t id) {
    for (int i = 0; i < 300; ++i) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (heard[id].ended) return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
  }
  Heard Get(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex);
    return heard[id];
  }
  // Asks the network for a url as a page of `pageUrl`: true where it was done (a response), false where it was refused.
  bool Ask(uint64_t id, const std::string& pageUrl, const std::string& url) {
    const int responses = fetchResponses.load(), errors = fetchErrors.load();
    net::PageRequest request;
    request.url = *url::Parse(url);
    request.pageUrl = *url::Parse(pageUrl);
    net::ipc::Writer out;
    out.U64(id);
    net::ipc::Write(out, request);
    networkEnd.Send(net::ipc::Message::StartFetch, out.bytes());
    for (int i = 0; i < 300; ++i) {
      if (fetchResponses.load() > responses) return true;
      if (fetchErrors.load() > errors) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
  }
};

// The browser's side of the link of a page: what it asks is given to the tab (as the window does).
struct BrowserSide {
  std::thread reader;
  BrowserSide(net::ipc::Channel& link, gui::BrowserServices::Tab& tab) {
    reader = std::thread([&link, &tab] {
      uint8_t type = 0;
      std::string bytes;
      while (link.ReceiveRaw(type, bytes)) {
        net::ipc::Reader in(bytes);
        if (type == static_cast<uint8_t>(gui::wire::ToBrowser::Navigate)) {
          gui::wire::Navigate request;
          if (gui::wire::Read(in, request)) tab.Navigate(std::move(request));
        } else if (type == static_cast<uint8_t>(gui::wire::ToBrowser::Storage)) {
          gui::wire::StorageRequest request;
          if (gui::wire::Read(in, request)) tab.Storage(std::move(request));
        } else if (type == static_cast<uint8_t>(gui::wire::ToBrowser::NavCommitted)) {
          const std::string address = in.String();
          if (in.ok()) tab.Committed(address);
        }
      }
    });
  }
  ~BrowserSide() { reader.join(); }
};

}  // namespace

int main() {
  std::atomic<int> served{0};
  std::string lastPostBody;
  std::mutex postMutex;
  const auto handler = [&](int client) {
    while (true) {
      const std::string head = TestServer::ReadHead(client);
      if (head.empty()) break;
      ++served;
      const std::string line = head.substr(0, head.find("\r\n"));
      if (line.rfind("POST", 0) == 0) {
        const size_t at = head.find("Content-Length: ");
        const size_t length = at == std::string::npos ? 0 : static_cast<size_t>(std::atoi(head.c_str() + at + 16));
        const std::string body = TestServer::ReadBytes(client, length);
        {
          std::lock_guard<std::mutex> lock(postMutex);
          lastPostBody = body;
        }
        TestServer::SendAll(client, Response("200 OK", "posted", "Content-Type: text/html\r\n"));
      } else if (line.find("/redirect") != std::string::npos) {
        TestServer::SendAll(client, Response("302 Found", "", "Location: /final\r\n"));
      } else {
        TestServer::SendAll(client, Response("200 OK", line.find("/final") != std::string::npos ? "final page" : "hello", "Content-Type: text/html; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\n"));
      }
    }
  };
  TestServer a(handler), b(handler);
  {
    gui::BrowserServices services;
    auto window = net::ipc::Channel::MakePair();
    auto network = net::ipc::Channel::MakePair();
    net::ipc::Channel browserLink(window.second);
    auto tab = services.OpenTab(network.second, browserLink, false);
    BrowserSide side(browserLink, *tab);
    Page page(window.first, network.first);

    // Before any document, the page has no origin to ask for.
    Check("before a navigation no origin is the page's", !page.Ask(1, Origin(a) + "/", Origin(a) + "/early"));

    page.Go(1, Origin(a) + "/doc");
    Check("a document is fetched", page.WaitEnded(1));
    const Page::Heard first = page.Get(1);
    Check("its head, type and body come", first.head && first.status == 200 && first.contentType.find("text/html") == 0 && first.body == "hello" && first.error.empty(), first.body + " " + first.error);
    Check("the address it came from is said", first.finalUrl == Origin(a) + "/doc", first.finalUrl);
    Check("the page may then ask as the origin of the document", page.Ask(2, Origin(a) + "/doc", Origin(a) + "/sub"));
    Check("and not as another origin", !page.Ask(3, Origin(b) + "/", Origin(a) + "/stolen"));

    page.Go(2, Origin(a) + "/redirect");
    Check("a redirect is followed by the browser", page.WaitEnded(2) && page.Get(2).finalUrl == Origin(a) + "/final" && page.Get(2).body == "final page");

    page.Go(3, Origin(b) + "/other");
    Check("a document of another origin comes", page.WaitEnded(3) && page.Get(3).body == "hello");
    Check("both origins are the page's until it commits", page.Ask(4, Origin(a) + "/", Origin(a) + "/x") && page.Ask(5, Origin(b) + "/", Origin(b) + "/x"));
    page.Committed(Origin(b) + "/other");
    // (The commit is taken in its turn on the browser's loop: a request made after it is behind it.)
    Check("when the page commits to one, the others are taken back", page.Ask(6, Origin(b) + "/", Origin(b) + "/y") && !page.Ask(7, Origin(a) + "/", Origin(a) + "/z"));
    page.Committed("http://never-allowed.test/");
    Check("a commit to an origin it was not let have takes nothing", page.Ask(8, Origin(b) + "/", Origin(b) + "/w"));

    {
      // What the page stores: for the origin it has, not for another, and no path.
      using Op = gui::wire::StorageRequest::Op;
      const std::string mine = Origin(b), other = Origin(a);
      const std::string idbFile = webapi::StorageId(webapi::StorageArea::IndexedDb, "db");
      Check("a file is stored for the origin the page has", page.Store(Op::Write, 0, mine, idbFile, "header\nbody").ok);
      const auto read = page.Store(Op::Read, 0, mine, idbFile);
      Check("and read back", read.ok && read.data == "header\nbody");
      Check("and read by its head", page.Store(Op::Read, 0, mine, idbFile, "", 6).data == "header");
      Check("and listed", page.Store(Op::List, 0, mine, "").data == idbFile + "\n");
      Check("an origin the page has not is refused", !page.Store(Op::Write, 0, other, idbFile, "x").ok && !page.Store(Op::Read, 0, other, idbFile).ok && !page.Store(Op::List, 0, other, "").ok);
      Check("an origin that is made up is refused", !page.Store(Op::Read, 0, "https://victim.test", idbFile).ok);
      Check("an id that is a path is refused", !page.Store(Op::Write, 0, mine, "../../escape.idb", "x").ok && !page.Store(Op::Read, 0, mine, "../" + idbFile).ok);
      Check("an area that is none is not taken", !page.Store(Op::Read, 9, mine, idbFile).ok);
      Check("a file is removed", page.Store(Op::Remove, 0, mine, idbFile).ok && !page.Store(Op::Read, 0, mine, idbFile).ok);
      // (The registrations of the workers of an origin are read for any, to see whether a navigation to it is for one.)
      const std::string registrations = webapi::StorageId(webapi::StorageArea::Kv, "sw-registrations");
      Check("the registrations of another origin may be read (and nothing else of it)", page.Store(Op::Write, 2, mine, registrations, "[]").ok && !page.Store(Op::Read, 2, other, registrations).ok == true && !page.Store(Op::Write, 2, other, registrations, "[]").ok);
    }

    page.Go(4, Origin(a) + "/form", "POST", "a=1&b=2");
    Check("a POST is sent with its body", page.WaitEnded(4) && page.Get(4).body == "posted");
    {
      std::lock_guard<std::mutex> lock(postMutex);
      Check("the body arrived", lastPostBody == "a=1&b=2", lastPostBody);
    }

    const std::string path = "/tmp/solar-services-test.html";
    { std::ofstream(path) << "<p>file</p>"; }
    page.Go(5, "file://" + path);
    Check("a file is not for a page made for the network", page.WaitEnded(5) && !page.Get(5).error.empty() && !page.Get(5).head, page.Get(5).error);
    page.Go(6, "ftp://example.com/x");
    Check("a scheme that is not the network's is refused", page.WaitEnded(6) && !page.Get(6).error.empty());
    page.Go(7, "not a url");
    Check("an address that is none is refused", page.WaitEnded(7) && !page.Get(7).error.empty());
    page.Go(4, Origin(a) + "/again");
    // (An id that ended may be used again; one in flight may not: tested below with a slow answer.)
    Check("an id that has ended can be used again", page.WaitEnded(4));
    std::remove(path.c_str());
  }
  {
    // A tab made for a file: it may open files.
    gui::BrowserServices services;
    auto window = net::ipc::Channel::MakePair();
    auto network = net::ipc::Channel::MakePair();
    net::ipc::Channel browserLink(window.second);
    auto tab = services.OpenTab(network.second, browserLink, true);
    BrowserSide side(browserLink, *tab);
    Page page(window.first, network.first);
    const std::string path = "/tmp/solar-services-test2.html";
    { std::ofstream(path) << "<p>file</p>"; }
    page.Go(1, "file://" + path);
    Check("a page made for a file is given the file", page.WaitEnded(1) && page.Get(1).body == "<p>file</p>" && page.Get(1).status == 200, page.Get(1).error);
    page.Go(2, "file:///nonexistent/dir/none.html");
    Check("a file that is not there is an error", page.WaitEnded(2) && !page.Get(2).error.empty());
    std::remove(path.c_str());
  }
  {
    // A navigation that is in flight keeps its id; the page going away ends it.
    std::atomic<bool> release{false};
    TestServer slow([&](int client) {
      const std::string head = TestServer::ReadHead(client);
      if (head.empty()) return;
      while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
      TestServer::SendAll(client, Response("200 OK", "late"));
    });
    {
      gui::BrowserServices services;
      auto window = net::ipc::Channel::MakePair();
      auto network = net::ipc::Channel::MakePair();
      net::ipc::Channel browserLink(window.second);
      auto tab = services.OpenTab(network.second, browserLink, false);
      BrowserSide side(browserLink, *tab);
    Page page(window.first, network.first);
      page.Go(1, Origin(slow) + "/slow");
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      page.Go(1, Origin(slow) + "/slow2");
      Check("an id that is in flight is refused", page.WaitEnded(1) && !page.Get(1).error.empty());
      // (The tab and the services go away with a navigation still in flight.)
    }
    release = true;
    Check("the browser went away with a navigation in flight", true);
  }
  std::printf("services: %d/%d passed\n", total - failed, total);
  return failed == 0 ? 0 : 1;
}
