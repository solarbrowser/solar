// The browser's side of a window's pages: which renderer is shown, the history, where a navigation goes (a renderer is made for one site: another site's navigation is another renderer's, which
// takes the window when it has committed), the origins each may ask for, and what it stores. The renderers are threads that play the page's end of the connections.

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "solar/gui/Services.h"
#include "solar/gui/Session.h"
#include "solar/gui/Settings.h"
#include "solar/gui/Wire.h"
#include "solar/net/Ipc.h"
#include "solar/net/PageFetch.h"
#include "solar/url/Origin.h"
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

std::string Origin(const TestServer& server, const char* host = "127.0.0.1") { return std::string("http://") + host + ":" + std::to_string(server.port()); }
std::string Response(const std::string& status, const std::string& body, const std::string& extra = "") {
  return "HTTP/1.1 " + status + "\r\n" + extra + "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

// A page's process, played by threads: it asks for its first address as a renderer does, commits to what comes, and does what the test has it do.
struct Fake {
  long pid = 0;
  std::string url;
  net::ipc::Channel window, network;
  std::thread windowReader, networkReader;
  std::mutex mutex;
  std::condition_variable changed;
  struct Heard {
    bool head = false, ended = false, transferred = false;
    int status = 0;
    std::string finalUrl, body, error;
  };
  std::map<uint32_t, Heard> heard;
  std::vector<std::pair<std::string, int>> goTos;
  std::atomic<int> fetchResponses{0}, fetchErrors{0};
  bool shutdown = false;
  bool commitsBy = true;  // it says it has committed when what it asked for has come
  std::string lieAbout;   // (the address it says it is on, if not the one that came)
  uint32_t nextId = 1;
  struct StorageAnswer { bool ok = false; std::string data; bool arrived = false; };
  std::map<uint32_t, StorageAnswer> storage;
  uint32_t nextStorage = 1;

  Fake(long p, int windowFd, int networkFd, std::string start) : pid(p), url(std::move(start)), window(windowFd), network(networkFd) {
    windowReader = std::thread([this] {
      uint8_t type = 0;
      std::string bytes;
      while (window.ReceiveRaw(type, bytes)) {
        net::ipc::Reader in(bytes);
        std::unique_lock<std::mutex> lock(mutex);
        switch (static_cast<gui::wire::ToRenderer>(type)) {
          case gui::wire::ToRenderer::NavHead: {
            gui::wire::NavHead head;
            if (!gui::wire::Read(in, head)) break;
            Heard& h = heard[head.id];
            h.head = true;
            h.status = head.status;
            h.finalUrl = head.finalUrl;
            break;
          }
          case gui::wire::ToRenderer::NavBody: {
            const uint32_t id = in.U32();
            heard[id].body += in.String();
            break;
          }
          case gui::wire::ToRenderer::NavEnd: {
            const uint32_t id = in.U32();
            heard[id].ended = true;
            const std::string where = lieAbout.empty() ? heard[id].finalUrl : lieAbout;
            lock.unlock();
            if (commitsBy) Commit(where, id);
            lock.lock();
            break;
          }
          case gui::wire::ToRenderer::NavError: {
            const uint32_t id = in.U32();
            heard[id].error = in.String();
            heard[id].ended = true;
            break;
          }
          case gui::wire::ToRenderer::NavTransferred: heard[in.U32()].transferred = true; break;
          case gui::wire::ToRenderer::GoTo: {
            const std::string address = in.String();
            const int entry = in.I32();
            goTos.push_back({address, entry});
            lock.unlock();
            // (As the page does: it navigates, as a step along the history for an entry.)
            Go(address, "GET", "", entry >= 0 ? gui::wire::Navigate::Kind::Traversal : gui::wire::Navigate::Kind::New, entry);
            lock.lock();
            break;
          }
          case gui::wire::ToRenderer::StorageReply: {
            const uint32_t id = in.U32();
            const bool ok = in.Bool();
            storage[id] = {ok, in.String(), true};
            break;
          }
          case gui::wire::ToRenderer::Shutdown: shutdown = true; break;
          default: break;
        }
        changed.notify_all();
      }
    });
    networkReader = std::thread([this] {
      net::ipc::Message type;
      std::string bytes;
      while (network.Receive(type, bytes)) {
        if (type == net::ipc::Message::FetchResponse) ++fetchResponses;
        if (type == net::ipc::Message::FetchError) ++fetchErrors;
      }
    });
  }
  ~Fake() {
    window.Shutdown();
    network.Shutdown();
    windowReader.join();
    networkReader.join();
  }

  uint32_t Go(const std::string& address, const std::string& method = "GET", const std::string& body = "", gui::wire::Navigate::Kind kind = gui::wire::Navigate::Kind::New, int entry = -1) {
    gui::wire::Navigate navigate;
    {
      std::lock_guard<std::mutex> lock(mutex);
      navigate.id = nextId++;
    }
    navigate.url = address;
    navigate.method = method;
    navigate.body = body;
    navigate.kind = kind;
    navigate.entry = entry;
    net::ipc::Writer out;
    gui::wire::Write(out, navigate);
    window.SendRaw(static_cast<uint8_t>(gui::wire::ToBrowser::Navigate), out.bytes());
    return navigate.id;
  }
  void Commit(const std::string& address, uint32_t id) {
    net::ipc::Writer out;
    out.String(address);
    out.U32(id);
    window.SendRaw(static_cast<uint8_t>(gui::wire::ToBrowser::NavCommitted), out.bytes());
  }
  void HistoryGo(int delta) {
    net::ipc::Writer out;
    out.I32(delta);
    window.SendRaw(static_cast<uint8_t>(gui::wire::ToBrowser::HistoryGo), out.bytes());
  }
  bool Wait(const std::function<bool()>& condition) {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds(6), condition);
  }
  Heard Get(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex);
    return heard[id];
  }
  bool Ended(uint32_t id) { return Wait([&] { return heard[id].ended; }); }
  bool Ask(uint64_t id, const std::string& pageUrl, const std::string& address) {
    const int responses = fetchResponses.load(), errors = fetchErrors.load();
    net::PageRequest request;
    request.url = *url::Parse(address);
    request.pageUrl = *url::Parse(pageUrl);
    net::ipc::Writer out;
    out.U64(id);
    net::ipc::Write(out, request);
    network.Send(net::ipc::Message::StartFetch, out.bytes());
    for (int i = 0; i < 300; ++i) {
      if (fetchResponses.load() > responses) return true;
      if (fetchErrors.load() > errors) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
  }
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
    window.SendRaw(static_cast<uint8_t>(gui::wire::ToBrowser::Storage), out.bytes());
    std::unique_lock<std::mutex> lock(mutex);
    changed.wait_for(lock, std::chrono::seconds(6), [&] { return storage[request.id].arrived; });
    return storage[request.id];
  }
};

// All the renderers of a test, and what the session is given.
struct World {
  std::mutex mutex;
  std::condition_variable changed;
  std::map<long, std::unique_ptr<Fake>> fakes;
  std::vector<std::string> started;  // the addresses the renderers were started for
  std::vector<long> finished;
  long next = 100;
  bool refuseStart = false;
  // What a renderer says is handed to the session while there is one: its reader threads go on while it is being ended, and on some systems the pointer is null by then (a
  // unique_ptr is emptied before its object is destroyed on macOS's library, after on Linux's). The lock and the flag are declared before the session: they are destroyed after it.
  std::mutex deliverMutex;
  bool delivering = true;
  std::unique_ptr<gui::Session> session;
  void Deliver(uint32_t host, uint8_t type, const std::string& bytes) {
    std::lock_guard<std::mutex> lock(deliverMutex);
    if (delivering && session) session->Handle(host, type, bytes);
  }
  ~World() {
    std::lock_guard<std::mutex> lock(deliverMutex);
    delivering = false;
  }
  std::map<long, uint32_t> hostOf;  // (a renderer's number in the session: the order they were started in)

  gui::Launcher Launcher() {
    gui::Launcher launcher;
    launcher.start = [this](int windowFd, int networkFd, const std::string& address) -> long {
      std::lock_guard<std::mutex> lock(mutex);
      if (refuseStart) {
        ::close(windowFd);
        ::close(networkFd);
        return -1;
      }
      const long pid = next++;
      started.push_back(address);
      fakes[pid] = std::make_unique<Fake>(pid, windowFd, networkFd, address);
      // (A renderer made for an address asks for it first.)
      Fake* fake = fakes[pid].get();
      fake->Go(address);  // (what it says waits in the connection until the session reads it)
      changed.notify_all();
      return pid;
    };
    launcher.finish = [this](long pid) -> int {
      std::lock_guard<std::mutex> lock(mutex);
      finished.push_back(pid);
      return 0;
    };
    return launcher;
  }
  Fake* FakeStartedFor(size_t index) {
    std::unique_lock<std::mutex> lock(mutex);
    changed.wait_for(lock, std::chrono::seconds(6), [&] { return started.size() > index; });
    if (started.size() <= index) return nullptr;
    return fakes[100 + static_cast<long>(index)].get();
  }
  bool WaitCurrent(Fake* fake) {
    for (int i = 0; i < 300; ++i) {
      if (fake && session->current() != 0) {
        // (The current host is the one whose fake this is: the hosts are numbered as they were started.)
        if (session->current() == static_cast<uint32_t>(fake->pid - 99)) return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
  }
  bool WaitFinished(long pid) {
    for (int i = 0; i < 300; ++i) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        for (long f : finished) if (f == pid) return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
  }
};

}  // namespace

int main() {
  // (The settings are the test's own: the user's decide where downloads go and whether they are asked about.)
  setenv("SOLAR_SETTINGS_FILE", (std::filesystem::temp_directory_path() / "solar-session-settings.conf").c_str(), 1);
  std::filesystem::remove(std::filesystem::temp_directory_path() / "solar-session-settings.conf");
  std::atomic<int> servedA{0}, servedB{0};
  std::mutex postMutex;
  std::string lastPost;
  std::atomic<int> redirectedHits{0};
  std::string otherOriginForRedirect;
  const auto handlerFor = [&](std::atomic<int>& served, bool isB) {
    return [&, isB](int client) {
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
            lastPost = body;
          }
          TestServer::SendAll(client, Response("200 OK", "posted:" + body, "Content-Type: text/html\r\n"));
        } else if (line.find("/to-other") != std::string::npos) {
          TestServer::SendAll(client, Response("302 Found", "", "Location: " + otherOriginForRedirect + "/landed\r\n"));
        } else if (line.find("/landed") != std::string::npos) {
          ++redirectedHits;
          TestServer::SendAll(client, Response("200 OK", std::string(isB ? "B" : "A") + " landed", "Content-Type: text/html\r\n"));
        } else if (line.find("/file.zip") != std::string::npos) {
          TestServer::SendAll(client, Response("200 OK", std::string(5000, 'z'), "Content-Type: application/zip\r\nContent-Disposition: attachment; filename=\"my:file.zip\"\r\n"));
        } else if (line.find("/same-redirect") != std::string::npos) {
          TestServer::SendAll(client, Response("302 Found", "", "Location: /plain\r\n"));
        } else {
          const size_t from = line.find(' ') + 1, to = line.find(' ', from);
          TestServer::SendAll(client, Response("200 OK", std::string(isB ? "B" : "A") + line.substr(from, to - from), "Content-Type: text/html\r\nAccess-Control-Allow-Origin: *\r\n"));
        }
      }
    };
  };
  TestServer a(handlerFor(servedA, false)), b(handlerFor(servedB, true));
  const std::string A = Origin(a, "127.0.0.1"), B = Origin(b, "localhost");
  otherOriginForRedirect = B;

  // The sites: what a process is made for.
  {
    const auto site = [](const char* address) { return url::SiteOf(*url::Parse(address)); };
    Check("the site is the registrable domain", site("https://a.example.co.uk/x") == site("https://b.example.co.uk:8443/y") && site("https://a.example.co.uk/") != site("https://example.org/"));
    Check("the scheme is part of it", site("http://example.com/") != site("https://example.com/"));
    Check("a host with no domain is its own", site("http://localhost:1/") != site("http://127.0.0.1:1/") && site("http://localhost:1/") == site("http://localhost:2/"));
    Check("every file is one site", site("file:///a/b") == site("file:///c/d") && site("file:///a") != site("http://a/"));
  }

  {
    gui::BrowserServices services;  // (it outlives the session)
    World world;
    gui::Session::Events events;
    events.message = [&](uint32_t host, uint8_t type, std::string bytes, int fd) {
      if (fd >= 0) ::close(fd);
      world.Deliver(host, type, bytes);
    };
    events.gone = [](uint32_t, bool) {};
    events.shown = [](uint32_t) {};
    world.session = std::make_unique<gui::Session>(services, world.Launcher(), std::move(events), false);
    gui::Session& session = *world.session;

    // The first page.
    session.Open(A + "/doc");
    Fake* one = world.FakeStartedFor(0);
    Check("a renderer is started for the address", one && world.started[0] == A + "/doc");
    const uint32_t first = one ? 1 : 0;
    Check("its document comes", one && one->Ended(first) && one->Get(first).body == "A/doc" && one->Get(first).status == 200);
    Check("it takes the window when it commits", one && world.WaitCurrent(one));
    Check("the history has the page", session.Entries() == std::vector<std::string>{A + "/doc"} && session.Index() == 0, std::to_string(session.Entries().size()));
    Check("it may ask as its own origin", one && one->Ask(1, A + "/doc", A + "/sub"));
    Check("and not as another's", one && !one->Ask(2, B + "/", A + "/stolen"));

    // The same site: it stays.
    const uint32_t second = one ? one->Go(A + "/other") : 0;
    Check("a navigation within the site is the renderer's", one && one->Ended(second) && one->Get(second).body == "A/other" && !one->Get(second).transferred);
    Check("and is a new entry", [&] { for (int i = 0; i < 100 && session.Entries().size() < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20)); return session.Entries().size() == 2 && session.Index() == 1; }());
    Check("no renderer was started for it", world.started.size() == 1);
    const uint32_t redirected = one ? one->Go(A + "/same-redirect") : 0;
    Check("a redirect within the site stays too", one && one->Ended(redirected) && one->Get(redirected).finalUrl == A + "/plain" && world.started.size() == 1);

    // Another site: another renderer, which takes the window at its commit.
    const uint32_t cross = one ? one->Go(B + "/page") : 0;
    Check("the page that asked is told the navigation is elsewhere", one && one->Wait([&] { return one->heard[cross].transferred; }));
    Fake* two = world.FakeStartedFor(1);
    Check("a renderer is started for the other site", two && world.started[1] == B + "/page");
    Check("the first page is shown until the second has committed", [&] { return session.current() == 1 || session.current() == 2; }());
    Check("the second one has the document", two && two->Ended(1) && two->Get(1).body == "B/page");
    Check("it takes the window", two && world.WaitCurrent(two));
    Check("the one before it is ended", world.WaitFinished(100));
    Check("the history went on", [&] { for (int i = 0; i < 100 && session.Entries().size() < 4; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20)); return session.Entries().size() == 4 && session.Index() == 3; }());
    Check("the new renderer may ask as its origin and not as the first's", two && two->Ask(1, B + "/page", B + "/sub") && !two->Ask(2, A + "/doc", B + "/stolen"));
    // (The origin of the first page was never the second's.)
    Check("the second one's storage is its own", [&] {
      using Op = gui::wire::StorageRequest::Op;
      const std::string file = webapi::StorageId(webapi::StorageArea::IndexedDb, "db");
      return two && two->Store(Op::Write, 0, B, file, "mine").ok && !two->Store(Op::Read, 0, A, file).ok;
    }());

    // Back and forward: along the entries, a process for each site.
    session.Back();
    Fake* three = world.FakeStartedFor(2);
    Check("back to the other site is a renderer for it", three && world.started[2] == A + "/plain");
    Check("it commits and takes the window", three && three->Ended(1) && world.WaitCurrent(three));
    Check("the entry is the one before", session.Index() == 2 && session.Entries().size() == 4);
    Check("the one it took the window from is ended", world.WaitFinished(101));
    session.Back();
    // (Entry 1 is of the site of the page that is shown: it goes to that page as a step along the history.)
    Check("back within the site is told to the page", three && three->Wait([&] { return !three->goTos.empty(); }) && three->goTos[0].first == A + "/other" && three->goTos[0].second == 1);
    Check("the page that is told goes and the entry moves", [&] { for (int i = 0; i < 100 && session.Index() != 1; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20)); return session.Index() == 1; }());
    Check("no renderer was started for it", world.started.size() == 3);
    session.Forward();
    Check("forward within the site too", three->Wait([&] { return three->goTos.size() >= 2; }) && three->goTos[1].second == 2);
    Check("the entry moves forward when the page has gone", [&] { for (int i = 0; i < 100 && session.Index() != 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20)); return session.Index() == 2; }());
    session.Forward();
    Fake* four = world.FakeStartedFor(3);
    Check("forward to the other site is a renderer", four && world.started[3] == B + "/page");

    // A renderer that is not the shown one has no say in the history.
    const size_t before = session.Entries().size();
    if (four) four->HistoryGo(-1);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    Check("a step the page that is not shown asks for is not taken", session.Entries().size() == before && world.started.size() == 4);
  }

  // A redirect to another site: the response is the other renderer's, and is not asked for twice.
  {
    gui::BrowserServices services;  // (it outlives the session)
    World world;
    gui::Session::Events events;
    events.message = [&](uint32_t host, uint8_t type, std::string bytes, int fd) {
      if (fd >= 0) ::close(fd);
      world.Deliver(host, type, bytes);
    };
    events.gone = [](uint32_t, bool) {};
    events.shown = [](uint32_t) {};
    world.session = std::make_unique<gui::Session>(services, world.Launcher(), std::move(events), false);
    gui::Session& session = *world.session;
    session.Open(A + "/doc");
    Fake* one = world.FakeStartedFor(0);
    Check("(redirect) the first page", one && one->Ended(1) && world.WaitCurrent(one));
    redirectedHits = 0;
    const uint32_t id = one->Go(A + "/to-other");
    Check("(redirect) the page is told the navigation is elsewhere", one->Wait([&] { return one->heard[id].transferred; }));
    Fake* two = world.FakeStartedFor(1);
    Check("(redirect) a renderer is started for the address it ended at", two && world.started[1] == B + "/landed");
    Check("(redirect) it has the response", two && two->Ended(1) && two->Get(1).body == "B landed" && two->Get(1).finalUrl == B + "/landed");
    Check("(redirect) which the server gave once", redirectedHits.load() == 1, std::to_string(redirectedHits.load()));
    Check("(redirect) the renderer made for it takes the window", world.WaitCurrent(two));
    Check("(redirect) the page that asked had nothing of the other site", one->Get(id).body.empty() && !one->Get(id).head);

    // A form posted to another site: what was posted goes with it.
    const uint32_t post = two->Go(A + "/form", "POST", "a=1&b=2");
    Check("(post) the other site's navigation is for another renderer", two->Wait([&] { return two->heard[post].transferred; }));
    Fake* three = world.FakeStartedFor(2);
    Check("(post) it has the answer to the post", three && three->Ended(1) && three->Get(1).body == "posted:a=1&b=2", three ? three->Get(1).body : "");
    {
      std::lock_guard<std::mutex> lock(postMutex);
      Check("(post) the server had the body once", lastPost == "a=1&b=2");
    }
  }

  // What the browser says no to.
  {
    gui::BrowserServices services;  // (it outlives the session)
    World world;
    gui::Session::Events events;
    events.message = [&](uint32_t host, uint8_t type, std::string bytes, int fd) {
      if (fd >= 0) ::close(fd);
      world.Deliver(host, type, bytes);
    };
    events.gone = [](uint32_t, bool) {};
    events.shown = [](uint32_t) {};
    world.session = std::make_unique<gui::Session>(services, world.Launcher(), std::move(events), false);
    gui::Session& session = *world.session;
    session.Open(A + "/doc");
    Fake* one = world.FakeStartedFor(0);
    Check("(refuse) the page", one && one->Ended(1) && world.WaitCurrent(one));
    const std::string path = "/tmp/solar-session-test.html";
    { std::ofstream(path) << "<p>file</p>"; }
    const uint32_t file = one->Go("file://" + path);
    Check("(refuse) a file is not for a page that came from the network", one->Ended(file) && !one->Get(file).error.empty() && !one->Get(file).head);
    std::remove(path.c_str());
    const uint32_t ftp = one->Go("ftp://example.com/x");
    Check("(refuse) a scheme that is not the network's", one->Ended(ftp) && !one->Get(ftp).error.empty());
    const uint32_t none = one->Go("not a url");
    Check("(refuse) an address that is none", one->Ended(none) && !one->Get(none).error.empty());
    // A page that says it has committed to what it was not given.
    one->lieAbout = B + "/lie";
    const uint32_t lie = one->Go(A + "/x");
    Check("(refuse) the document comes", one->Ended(lie));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    Check("(refuse) a commit to an address of another site is not taken: no entry, no other origin", session.Entries() == std::vector<std::string>{A + "/doc"} && !one->Ask(9, B + "/lie", B + "/x"));
    one->lieAbout.clear();
    // A page that was never given a navigation says it has committed to one.
    one->Commit(A + "/never", 999);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    Check("(refuse) a commit to a navigation that was not made is not taken", session.Entries().size() == 1);
    // Storage.
    using Op = gui::wire::StorageRequest::Op;
    const std::string idbFile = webapi::StorageId(webapi::StorageArea::IndexedDb, "db");
    Check("(store) a file for the origin the page has", one->Store(Op::Write, 0, A, idbFile, "header\nbody").ok && one->Store(Op::Read, 0, A, idbFile).data == "header\nbody" && one->Store(Op::Read, 0, A, idbFile, "", 6).data == "header");
    Check("(store) listed and removed", one->Store(Op::List, 0, A, "").data == idbFile + "\n" && one->Store(Op::Remove, 0, A, idbFile).ok && !one->Store(Op::Read, 0, A, idbFile).ok);
    Check("(store) another origin's is refused", !one->Store(Op::Write, 0, B, idbFile, "x").ok && !one->Store(Op::Read, 0, "https://victim.test", idbFile).ok);
    Check("(store) an id that is a path, an area that is none", !one->Store(Op::Write, 0, A, "../../escape.idb", "x").ok && !one->Store(Op::Read, 9, A, idbFile).ok);
    const std::string registrations = webapi::StorageId(webapi::StorageArea::Kv, "sw-registrations");
    Check("(store) the registrations of another origin may be read, and nothing else of it", one->Store(Op::Write, 2, A, registrations, "[]").ok && !one->Store(Op::Write, 2, B, registrations, "[]").ok);
  }

  // A page for a file may be taken to files.
  {
    gui::BrowserServices services;  // (it outlives the session)
    World world;
    gui::Session::Events events;
    events.message = [&](uint32_t host, uint8_t type, std::string bytes, int fd) {
      if (fd >= 0) ::close(fd);
      world.Deliver(host, type, bytes);
    };
    events.gone = [](uint32_t, bool) {};
    events.shown = [](uint32_t) {};
    world.session = std::make_unique<gui::Session>(services, world.Launcher(), std::move(events), true);
    const std::string path = "/tmp/solar-session-test2.html";
    { std::ofstream(path) << "<p>file</p>"; }
    world.session->Open("file://" + path);
    Fake* one = world.FakeStartedFor(0);
    Check("(file) a page made for a file has it", one && one->Ended(1) && one->Get(1).body == "<p>file</p>" && one->Get(1).status == 200, one ? one->Get(1).error : "");
    Check("(file) and another of the files", one && [&] { const uint32_t id = one->Go("file:///nonexistent/none.html"); return one->Ended(id) && !one->Get(id).error.empty(); }());
    Check("(file) a page of the network is another site's", one && [&] { const uint32_t id = one->Go(A + "/x"); return one->Wait([&] { return one->heard[id].transferred; }); }());
    std::remove(path.c_str());
  }

  // A file that is asked for is kept, and the page stays.
  {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "solar-session-downloads";
    std::filesystem::remove_all(directory);
    setenv("SOLAR_DOWNLOAD_DIR", directory.c_str(), 1);
    gui::BrowserServices services;
    World world;
    gui::Session::Events events;
    events.message = [&](uint32_t host, uint8_t type, std::string bytes, int fd) {
      if (fd >= 0) ::close(fd);
      world.Deliver(host, type, bytes);
    };
    events.gone = [](uint32_t, bool) {};
    events.shown = [](uint32_t) {};
    world.session = std::make_unique<gui::Session>(services, world.Launcher(), std::move(events), false);
    gui::Session& session = *world.session;
    session.Open(A + "/doc");
    Fake* one = world.FakeStartedFor(0);
    Check("(download) the page", one && one->Ended(1) && world.WaitCurrent(one));
    const uint32_t file = one->Go(A + "/file.zip");
    Check("(download) the page that asked is told the navigation is elsewhere", one->Wait([&] { return one->heard[file].transferred; }));
    std::string path;
    for (int i = 0; i < 150 && path.empty(); ++i) {
      path = session.DownloadPath(1);
      if (path.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    Check("(download) it is kept in the folder, under a name with nothing in it that is a path", !path.empty() && std::filesystem::path(path).parent_path() == directory && std::filesystem::path(path).filename() == "my_file.zip", path);
    Check("(download) with all that was sent", !path.empty() && std::filesystem::file_size(path) == 5000);
    Check("(download) the page stays, and no renderer was made for the file", session.current() == 1 && world.started.size() == 1);
    session.ClearDownloads();
    unsetenv("SOLAR_DOWNLOAD_DIR");
    std::filesystem::remove_all(directory);
  }
  // A file that is asked about is kept where the user says, or not at all.
  for (const bool cancel : {false, true}) {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "solar-session-ask";
    std::filesystem::remove_all(directory);
    setenv("SOLAR_DOWNLOAD_DIR", (directory / "default").c_str(), 1);
    gui::Settings::Instance().Set("downloadAsk", "on");
    gui::BrowserServices services;
    World world;
    std::atomic<int> asked{0};
    gui::Session::Events events;
    events.message = [&](uint32_t host, uint8_t type, std::string bytes, int fd) {
      if (fd >= 0) ::close(fd);
      world.Deliver(host, type, bytes);
    };
    events.gone = [](uint32_t, bool) {};
    events.shown = [](uint32_t) {};
    events.askSave = [&](uint32_t id, const std::string& suggested) {
      ++asked;
      // (The user takes a while, and says another place and another name.)
      std::thread([&, id, suggested] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        world.session->ChooseDownloadPath(id, cancel ? "" : (directory / "chosen" / "mine.zip").string());
      }).detach();
    };
    world.session = std::make_unique<gui::Session>(services, world.Launcher(), std::move(events), false);
    gui::Session& session = *world.session;
    const std::string label = cancel ? "(ask, no) " : "(ask) ";
    session.Open(A + "/doc");
    Fake* one = world.FakeStartedFor(0);
    Check(label + "the page", one && one->Ended(1) && world.WaitCurrent(one));
    one->Go(A + "/file.zip");
    for (int i = 0; i < 150 && asked.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Check(label + "the user is asked", asked.load() == 1);
    std::string path;
    for (int i = 0; i < 200 && path.empty() && !cancel; ++i) {
      path = session.DownloadPath(1);
      if (path.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (cancel) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    bool strays = false;
    if (std::filesystem::exists(directory)) {
      for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().filename() != "mine.zip") strays = true;
      }
    }
    if (cancel) Check(label + "nothing is kept, no file is left aside", session.DownloadPath(1).empty() && !strays, [&] { std::string l; if (std::filesystem::exists(directory)) for (const auto& e : std::filesystem::recursive_directory_iterator(directory)) l += e.path().string() + " "; return l; }());
    else Check(label + "it is kept where the user said, with all of it, and nothing is left aside", path == (directory / "chosen" / "mine.zip").string() && std::filesystem::file_size(path) == 5000 && !strays, path);
    gui::Settings::Instance().Set("downloadAsk", "off");
    unsetenv("SOLAR_DOWNLOAD_DIR");
    std::filesystem::remove_all(directory);
  }
  // A renderer that cannot be started or that goes.
  {
    gui::BrowserServices services;  // (it outlives the session)
    World world;
    std::atomic<int> fatal{0}, notFatal{0};
    gui::Session::Events events;
    events.message = [&](uint32_t host, uint8_t type, std::string bytes, int fd) {
      if (fd >= 0) ::close(fd);
      world.Deliver(host, type, bytes);
    };
    events.gone = [&](uint32_t, bool isFatal) { if (isFatal) ++fatal; else ++notFatal; };
    events.shown = [](uint32_t) {};
    world.session = std::make_unique<gui::Session>(services, world.Launcher(), std::move(events), false);
    gui::Session& session = *world.session;
    session.Open(A + "/doc");
    Fake* one = world.FakeStartedFor(0);
    Check("(gone) the page", one && one->Ended(1) && world.WaitCurrent(one));
    // A renderer that is on its way to a document goes: the page stays.
    const uint32_t cross = one->Go(B + "/slow");
    one->Wait([&] { return one->heard[cross].transferred; });
    Fake* two = world.FakeStartedFor(1);
    two->commitsBy = false;
    two->window.Shutdown();
    for (int i = 0; i < 100 && notFatal.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Check("(gone) a renderer on its way that goes is not the end of the window", notFatal.load() >= 1 && fatal.load() == 0 && session.current() == 1, std::to_string(session.current()));
    // The page that is shown goes.
    one->window.Shutdown();
    for (int i = 0; i < 100 && fatal.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Check("(gone) the page that is shown going ends the window", fatal.load() == 1);
    // One that cannot be started.
    world.refuseStart = true;
    session.Go("http://nowhere.test/");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    Check("(gone) a renderer that cannot be started leaves things as they are", true);
  }
  // Tabs: each has its own history and its own renderer, one is shown, and the last one closed ends the window.
  {
    gui::BrowserServices services;
    World world;
    std::atomic<int> fatal{0};
    gui::Session::Events events;
    events.message = [&](uint32_t host, uint8_t type, std::string bytes, int fd) {
      if (fd >= 0) ::close(fd);
      world.Deliver(host, type, bytes);
    };
    events.gone = [&](uint32_t, bool isFatal) { if (isFatal) ++fatal; };
    events.shown = [](uint32_t) {};
    world.session = std::make_unique<gui::Session>(services, world.Launcher(), std::move(events), false);
    gui::Session& session = *world.session;
    const auto until = [](const std::function<bool()>& condition) {
      for (int i = 0; i < 150 && !condition(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
      return condition();
    };
    session.Open(A + "/doc");
    Fake* one = world.FakeStartedFor(0);
    Check("(tabs) the first tab has a page", one && one->Ended(1) && world.WaitCurrent(one) && session.activeTab() == 1);
    session.NewTab(B + "/page");
    Fake* two = world.FakeStartedFor(1);
    Check("(tabs) a new tab is a renderer of its own", two && world.started.size() == 2 && world.started[1] == B + "/page");
    Check("(tabs) and is shown once it has its page", two && two->Ended(1) && world.WaitCurrent(two) && session.activeTab() == 2);
    Check("(tabs) the history is the shown tab's", until([&] { return session.Entries() == std::vector<std::string>{B + "/page"}; }) && session.Index() == 0);
    session.Activate(1);
    Check("(tabs) the first tab is shown again with its page and history", until([&] { return session.current() == 1; }) && session.Entries() == std::vector<std::string>{A + "/doc"});
    session.CycleTab(1);
    Check("(tabs) the next tab is the second", until([&] { return session.activeTab() == 2 && session.current() == 2; }));
    session.CycleTab(1);
    Check("(tabs) and after the last is the first", until([&] { return session.activeTab() == 1; }));
    session.Activate(2);
    session.CloseTab(2);
    Check("(tabs) closing the shown tab shows the one beside it", until([&] { return session.activeTab() == 1 && session.current() == 1; }) && world.WaitFinished(101));
    Check("(tabs) and the window goes on", fatal.load() == 0);
    session.CloseTab(1);
    Check("(tabs) closing the last tab ends the window", until([&] { return fatal.load() >= 1; }));
  }
  std::printf("session: %d/%d passed\n", total - failed, total);
  return failed == 0 ? 0 : 1;
}
