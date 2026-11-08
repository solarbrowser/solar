/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/gui/Session.h"

#include <unistd.h>

#include <condition_variable>
#include <deque>
#include <future>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <thread>

#include "solar/net/HttpClient.h"
#include "solar/url/Origin.h"
#include "solar/url/Parser.h"
#include "solar/url/Serializer.h"

namespace solar::gui {

namespace {

constexpr size_t kMostNavigations = 64;
constexpr size_t kLargestFile = size_t{256} << 20;

std::string NormalAddress(const std::string& text) {
  std::optional<url::Url> parsed = url::Parse(text);
  return parsed ? url::Serialize(*parsed) : text;
}

}  // namespace

struct Navigation;

// A renderer, as the session has it.
struct Host {
  uint32_t id = 0;
  long child = -1;
  std::string site;  // the one it is made for
  std::unique_ptr<net::ipc::Channel> link;
  std::unique_ptr<BrowserServices::Tab> tab;
  std::thread reader;
  bool current = false;      // (the loop's thread)
  bool gone = false;         // (the loop's thread)

  // The first navigation of a renderer made for an address is the one that address is for: what was asked of it, or a response that is already on its way.
  struct Expected {
    std::string url, method = "GET", body, bodyType;
    wire::Navigate::Kind kind = wire::Navigate::Kind::New;
    int entry = -1;
    std::shared_ptr<Navigation> buffered;
  };
  std::optional<Expected> expected;
  // What a navigation is for the history, until the page says it has committed to it.
  struct Info {
    wire::Navigate::Kind kind;
    int entry;
  };
  std::map<uint32_t, Info> infos;
  std::set<uint32_t> active;  // the ids of the navigations that are going on
};

struct Session::Impl : std::enable_shared_from_this<Session::Impl> {
  BrowserServices& services;
  Launcher launcher;
  Events events;
  const bool allowFiles;

  mutable std::mutex mutex;  // the hosts, the history and what is kept for the ones to come
  std::map<uint32_t, std::shared_ptr<Host>> hosts;
  uint32_t nextHost = 1;
  std::atomic<uint32_t> current{0};
  uint32_t pending = 0;
  std::vector<std::string> entries;
  int index = -1;
  std::optional<std::string> resize;  // the last size the window said (the ones that come are told it)
  int exitCode = 0;
  bool closing = false;

  // The navigations that are going on (the loop's thread).
  std::map<uint64_t, std::shared_ptr<Navigation>> navigations;
  uint64_t nextNavigation = 1;

  // What ends renderers is done on a thread of its own (the tab of one waits for the loop, which is not to be waited for from it).
  std::thread janitor;
  std::mutex janitorMutex;
  std::condition_variable janitorWake;
  std::deque<std::function<void()>> janitorJobs;
  bool janitorStop = false;

  Impl(BrowserServices& s, Launcher l, Events e, bool files) : services(s), launcher(std::move(l)), events(std::move(e)), allowFiles(files) {
    janitor = std::thread([this] {
      for (;;) {
        std::function<void()> job;
        {
          std::unique_lock<std::mutex> lock(janitorMutex);
          janitorWake.wait(lock, [&] { return janitorStop || !janitorJobs.empty(); });
          if (janitorJobs.empty()) return;
          job = std::move(janitorJobs.front());
          janitorJobs.pop_front();
        }
        job();
      }
    });
  }
  ~Impl() {
    {
      std::lock_guard<std::mutex> lock(janitorMutex);
      janitorStop = true;
    }
    janitorWake.notify_all();
    janitor.join();
  }
  void Janitor(std::function<void()> job) {
    {
      std::lock_guard<std::mutex> lock(janitorMutex);
      janitorJobs.push_back(std::move(job));
    }
    janitorWake.notify_one();
  }

  std::shared_ptr<Host> Find(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = hosts.find(id);
    return found == hosts.end() ? nullptr : found->second;
  }

  void SendTo(Host& host, wire::ToRenderer type, const std::string& bytes) { host.link->SendRaw(static_cast<uint8_t>(type), bytes); }
  void NavError(Host& host, uint32_t id, const std::string& message) {
    net::ipc::Writer out;
    out.U32(id);
    out.String(message);
    SendTo(host, wire::ToRenderer::NavError, out.bytes());
  }

  std::shared_ptr<Host> StartHost(const std::string& site, const std::string& url, std::optional<Host::Expected> expected);
  void Retire(const std::shared_ptr<Host>& host, bool takeCode = false);
  void OnNavigate(const std::shared_ptr<Host>& host, wire::Navigate request);
  void OnCommitted(const std::shared_ptr<Host>& host, const std::string& address, uint32_t id);
  void OnHistoryGo(const std::shared_ptr<Host>& host, int delta);
  void GoToEntry(int entry);
  void GoNew(const std::string& address);
  void OnGone(uint32_t id);
  void StartNavigation(const std::shared_ptr<Host>& host, uint32_t id, const url::Url& address, const std::string& method, const std::string& body, const std::string& bodyType, wire::Navigate::Kind kind, int entry);
  void Transfer(const std::shared_ptr<Host>& from, uint32_t id, Host::Expected expected);
  void Swap(const std::shared_ptr<Host>& host);
};

// A navigation the browser is doing for a renderer: the fetch, and what comes of it said to the renderer. A response that turns out to be of another site (a redirect that left the site)
// is not the renderer's: it is kept (the head, and what follows) for the renderer made for that site, so that it is not asked for twice (an address may be good for once).
struct Navigation : net::FetchHandler {
  Session::Impl* session = nullptr;
  uint64_t serial = 0;
  std::shared_ptr<Host> target;  // null while it waits for the renderer it is to go to
  uint32_t id = 0;
  net::FetchHandle handle;
  wire::Navigate::Kind kind = wire::Navigate::Kind::New;
  int entry = -1;
  bool bodySent = false, ended = false, alive = true;

  struct Event {
    enum class Type { Head, Body, End, Error } type;
    int status = 0;
    std::string text, address;
  };
  std::vector<Event> buffered;

  void Emit(Event event) {
    if (!target) {
      buffered.push_back(std::move(event));
      return;
    }
    Host& host = *target;
    net::ipc::Writer out;
    switch (event.type) {
      case Event::Type::Head:
        wire::Write(out, wire::NavHead{id, event.status, event.text, event.address});
        session->SendTo(host, wire::ToRenderer::NavHead, out.bytes());
        break;
      case Event::Type::Body:
        out.U32(id);
        out.String(event.text);
        session->SendTo(host, wire::ToRenderer::NavBody, out.bytes());
        break;
      case Event::Type::End:
        out.U32(id);
        session->SendTo(host, wire::ToRenderer::NavEnd, out.bytes());
        host.active.erase(id);
        break;
      case Event::Type::Error:
        out.U32(id);
        out.String(event.text);
        session->SendTo(host, wire::ToRenderer::NavError, out.bytes());
        host.active.erase(id);
        break;
    }
  }
  void Finish() {
    ended = true;
    // (Let go of, but not from inside its own call.)
    session->services.Post([keep = session->shared_from_this(), serial = serial] { keep->navigations.erase(serial); });
  }

  // The renderer made for the site of the response is here: what was kept goes to it.
  void Attach(const std::shared_ptr<Host>& host, uint32_t navigationId) {
    target = host;
    id = navigationId;
    host->active.insert(navigationId);
    host->infos[navigationId] = {kind, entry};
    std::vector<Event> kept = std::move(buffered);
    buffered.clear();
    for (Event& event : kept) {
      if (event.type == Event::Type::Head) host->tab->AllowOrigin(url::SerializeOrigin(*url::Parse(event.address)));
      Emit(std::move(event));
    }
  }

  void OnResponseHead(const net::HttpResponseHead& head, const url::Url& finalUrl) override {
    std::string type;
    if (auto value = head.Header("content-type")) type = std::string(*value);
    Event event{Event::Type::Head, head.status, type, url::Serialize(finalUrl)};
    if (target && url::SiteOf(finalUrl) != target->site) {
      // The server sent the page to another site: the renderer does not have the document; the one that is made for that site does.
      std::shared_ptr<Host> from = target;
      const uint32_t fromId = id;
      target = nullptr;
      from->active.erase(fromId);
      from->infos.erase(fromId);
      buffered.push_back(std::move(event));
      Host::Expected expected;
      expected.url = url::Serialize(finalUrl);
      expected.kind = kind;
      expected.entry = entry;
      expected.buffered = session->navigations[serial];
      session->Transfer(from, fromId, std::move(expected));
      return;
    }
    if (target) target->tab->AllowOrigin(url::SerializeOrigin(finalUrl));
    Emit(std::move(event));
  }
  void OnBody(std::span<const uint8_t> data) override {
    bodySent = true;
    Emit(Event{Event::Type::Body, 0, std::string(reinterpret_cast<const char*>(data.data()), data.size()), ""});
  }
  void OnEnd() override {
    Emit(Event{Event::Type::End, 0, "", ""});
    Finish();
  }
  void OnError(std::string_view message) override {
    Emit(Event{Event::Type::Error, 0, std::string(message), ""});
    Finish();
  }
  bool OnRestart() override {
    // (Asked for again from the start, as long as the renderer has none of the body to take back.)
    if (bodySent || !target) return false;
    net::ipc::Writer out;
    out.U32(id);
    session->SendTo(*target, wire::ToRenderer::NavRestart, out.bytes());
    return true;
  }
};

// ---- Hosts ----

std::shared_ptr<Host> Session::Impl::StartHost(const std::string& site, const std::string& url, std::optional<Host::Expected> expected) {
  auto windowPair = net::ipc::Channel::MakePair();
  auto networkPair = net::ipc::Channel::MakePair();
  if (windowPair.first < 0 || networkPair.first < 0) return nullptr;
  auto host = std::make_shared<Host>();
  host->site = site;
  host->expected = std::move(expected);
  // (The renderer's ends go to the launcher, which takes them over.)
  host->child = launcher.start(windowPair.second, networkPair.second, url);
  if (host->child < 0) {
    ::close(windowPair.first);
    ::close(networkPair.first);
    return nullptr;
  }
  host->link = std::make_unique<net::ipc::Channel>(windowPair.first);
  host->tab = services.OpenTab(networkPair.first, *host->link);
  std::optional<std::string> size;
  {
    std::lock_guard<std::mutex> lock(mutex);
    host->id = nextHost++;
    hosts[host->id] = host;
    size = resize;
  }
  if (size) SendTo(*host, wire::ToRenderer::Resize, *size);
  host->reader = std::thread([this, raw = host.get(), id = host->id] {
    uint8_t type = 0;
    std::string bytes;
    int fd = -1;
    while (raw->link->ReceiveRaw(type, bytes, &fd)) {
      events.message(id, type, std::move(bytes), fd);
      bytes.clear();
      fd = -1;
    }
    services.Post([keep = shared_from_this(), id] { keep->OnGone(id); });
  });
  return host;
}

void Session::Impl::Retire(const std::shared_ptr<Host>& host, bool takeCode) {
  if (host->gone) return;
  host->gone = true;
  host->current = false;
  if (host->expected && host->expected->buffered) {
    // (A response that was on its way to this renderer has nowhere to go.)
    host->expected->buffered->handle.Cancel();
    navigations.erase(host->expected->buffered->serial);
  }
  host->expected.reset();
  for (auto it = navigations.begin(); it != navigations.end();) {
    if (it->second->target == host) {
      it->second->handle.Cancel();
      it = navigations.erase(it);
    } else {
      ++it;
    }
  }
  {
    std::lock_guard<std::mutex> lock(mutex);
    hosts.erase(host->id);
    if (pending == host->id) pending = 0;
  }
  SendTo(*host, wire::ToRenderer::Shutdown, "");
  Janitor([this, host, takeCode] {
    host->link->Shutdown();
    if (host->reader.joinable()) host->reader.join();
    host->tab.reset();
    const int code = launcher.finish(host->child);
    if (takeCode) {
      std::lock_guard<std::mutex> lock(mutex);
      exitCode = code;
    }
  });
}

void Session::Impl::OnGone(uint32_t id) {
  std::shared_ptr<Host> host = Find(id);
  if (!host || host->gone) return;
  const bool wasCurrent = host->current;
  Retire(host, wasCurrent);
  bool fatal = wasCurrent;
  if (!wasCurrent && current.load() == 0) {
    // (A renderer that goes before there is any page: there is none to show, and none to come, unless another is on its way.)
    std::lock_guard<std::mutex> lock(mutex);
    fatal = pending == 0;
  }
  if (fatal) events.gone(id, true);
  else events.gone(id, false);
}

void Session::Impl::Swap(const std::shared_ptr<Host>& host) {
  std::shared_ptr<Host> old = Find(current.load());
  host->current = true;
  current.store(host->id);
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (pending == host->id) pending = 0;
  }
  events.shown(host->id);
  if (old && old != host) Retire(old);
}

// ---- Navigation ----

void Session::Impl::Transfer(const std::shared_ptr<Host>& from, uint32_t id, Host::Expected expected) {
  // The page that asked stays as it is; the one made for the site gets the navigation as its first.
  if (from && !from->gone) {
    net::ipc::Writer out;
    out.U32(id);
    SendTo(*from, wire::ToRenderer::NavTransferred, out.bytes());
  }
  std::optional<url::Url> parsed = url::Parse(expected.url);
  if (!parsed) return;
  // (A renderer on its way to a document that has not come yet is let go: the user asked for another.)
  std::shared_ptr<Host> before;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (pending) {
      const auto found = hosts.find(pending);
      if (found != hosts.end()) before = found->second;
    }
  }
  if (before && before != from) Retire(before);
  const std::string address = expected.url;
  std::shared_ptr<Host> host = StartHost(url::SiteOf(*parsed), address, std::move(expected));
  if (!host) return;
  std::lock_guard<std::mutex> lock(mutex);
  pending = host->id;
}

void Session::Impl::StartNavigation(const std::shared_ptr<Host>& host, uint32_t id, const url::Url& address, const std::string& method, const std::string& body, const std::string& bodyType,
                                    wire::Navigate::Kind kind, int entry) {
  auto navigation = std::make_shared<Navigation>();
  navigation->session = this;
  navigation->serial = nextNavigation++;
  navigation->target = host;
  navigation->id = id;
  navigation->kind = kind;
  navigation->entry = entry;
  host->active.insert(id);
  host->infos[id] = {kind, entry};
  navigations[navigation->serial] = navigation;
  const std::string text = url::Serialize(address);
  if (address.scheme == "file") {
    std::ifstream file(FileOfAddress(text), std::ios::binary);
    std::stringstream contents;
    if (!file) return navigation->OnError("cannot read the file");
    contents << file.rdbuf();
    const std::string data = contents.str();
    if (data.size() > kLargestFile) return navigation->OnError("the file is too large");
    host->tab->AllowOrigin(url::SerializeOrigin(address));
    navigation->Emit({Navigation::Event::Type::Head, 200, "text/html", text});
    if (!data.empty()) navigation->OnBody(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(data.data()), data.size()));
    navigation->OnEnd();
    return;
  }
  net::FetchOptions options;
  if (method == "POST") {
    options.method = "POST";
    options.body = std::make_shared<const std::string>(body);
    if (!bodyType.empty()) options.headers.emplace_back("Content-Type", bodyType);
  }
  navigation->handle = services.Fetch(address, *navigation, std::move(options));
}

void Session::Impl::OnNavigate(const std::shared_ptr<Host>& host, wire::Navigate request) {
  if (host->gone) return;
  if (host->active.size() >= kMostNavigations || host->active.count(request.id)) return NavError(*host, request.id, "too many navigations");
  std::optional<url::Url> parsed = url::Parse(request.url);
  if (!parsed) return NavError(*host, request.id, "invalid address");
  std::string method = request.method, body = request.body, bodyType = request.bodyType;
  wire::Navigate::Kind kind = request.kind;
  int entry = request.entry;
  // The first navigation of a renderer that was made for an address is that address's, as the browser kept it.
  if (host->expected) {
    Host::Expected expected = std::move(*host->expected);
    host->expected.reset();
    if (NormalAddress(expected.url) == url::Serialize(*parsed)) {
      if (expected.buffered) {
        expected.buffered->Attach(host, request.id);
        return;
      }
      method = expected.method;
      body = expected.body;
      bodyType = expected.bodyType;
      kind = expected.kind;
      entry = expected.entry;
    }
  }
  if (parsed->scheme == "file") {
    if (!allowFiles) return NavError(*host, request.id, "this page may not open files");
  } else if (parsed->scheme != "http" && parsed->scheme != "https") {
    return NavError(*host, request.id, "unsupported scheme");
  }
  if (kind == wire::Navigate::Kind::Traversal) {
    // (A step along the history is to an entry there is, for the address the entry has.)
    std::lock_guard<std::mutex> lock(mutex);
    if (entry < 0 || entry >= static_cast<int>(entries.size()) || entries[entry] != url::Serialize(*parsed)) {
      kind = wire::Navigate::Kind::New;
      entry = -1;
    }
  }
  const std::string site = url::SiteOf(*parsed);
  if (site.empty() || site != host->site) {
    Host::Expected expected;
    expected.url = url::Serialize(*parsed);
    expected.method = method;
    expected.body = body;
    expected.bodyType = bodyType;
    expected.kind = kind;
    expected.entry = entry;
    Transfer(host, request.id, std::move(expected));
    return;
  }
  StartNavigation(host, request.id, *parsed, method, body, bodyType, kind, entry);
}

void Session::Impl::OnCommitted(const std::shared_ptr<Host>& host, const std::string& address, uint32_t id) {
  if (host->gone) return;
  std::optional<url::Url> parsed = url::Parse(address);
  // (A renderer commits to documents of its own site: what it says it is on is checked.)
  if (!parsed || url::SiteOf(*parsed) != host->site) return;
  const auto found = host->infos.find(id);
  Host::Info info{wire::Navigate::Kind::New, -1};  // (a page that did it itself, as a service worker's answer: a new entry)
  if (found != host->infos.end()) {
    info = found->second;
    host->infos.erase(host->infos.begin(), std::next(found));
  } else if (id != 0) {
    return;
  }
  host->tab->KeepOnlyOrigin(url::SerializeOrigin(*parsed));
  const std::string text = url::Serialize(*parsed);
  {
    std::lock_guard<std::mutex> lock(mutex);
    switch (info.kind) {
      case wire::Navigate::Kind::New:
        if (index < 0 || entries[index] != text) {
          entries.resize(static_cast<size_t>(index + 1));
          entries.push_back(text);
          index = static_cast<int>(entries.size()) - 1;
        }
        break;
      case wire::Navigate::Kind::Reload:
        if (index >= 0) entries[index] = text;
        break;
      case wire::Navigate::Kind::Traversal:
        if (info.entry >= 0 && info.entry < static_cast<int>(entries.size())) {
          index = info.entry;
          entries[index] = text;
        }
        break;
    }
  }
  if (!host->current) Swap(host);
}

void Session::Impl::GoToEntry(int entry) {
  std::string address;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (entry < 0 || entry >= static_cast<int>(entries.size())) return;
    address = entries[entry];
  }
  std::optional<url::Url> parsed = url::Parse(address);
  std::shared_ptr<Host> now = Find(current.load());
  if (!parsed) return;
  if (now && url::SiteOf(*parsed) == now->site) {
    net::ipc::Writer out;
    out.String(address);
    out.I32(entry);
    SendTo(*now, wire::ToRenderer::GoTo, out.bytes());
    return;
  }
  Host::Expected expected;
  expected.url = address;
  expected.kind = wire::Navigate::Kind::Traversal;
  expected.entry = entry;
  Transfer(nullptr, 0, std::move(expected));
}

void Session::Impl::GoNew(const std::string& address) {
  std::optional<url::Url> parsed = url::Parse(address);
  if (!parsed) return;
  std::shared_ptr<Host> now = Find(current.load());
  if (now && url::SiteOf(*parsed) == now->site) {
    net::ipc::Writer out;
    out.String(url::Serialize(*parsed));
    out.I32(-1);
    SendTo(*now, wire::ToRenderer::GoTo, out.bytes());
    return;
  }
  Host::Expected expected;
  expected.url = url::Serialize(*parsed);
  Transfer(nullptr, 0, std::move(expected));
}

void Session::Impl::OnHistoryGo(const std::shared_ptr<Host>& host, int delta) {
  if (host->gone || !host->current || (delta != -1 && delta != 1)) return;
  int target;
  {
    std::lock_guard<std::mutex> lock(mutex);
    target = index + delta;
  }
  GoToEntry(target);
}

// ---- Session ----

Session::Session(BrowserServices& services, Launcher launcher, Events events, bool allowFiles) : impl_(std::make_shared<Impl>(services, std::move(launcher), std::move(events), allowFiles)) {}

Session::~Session() { Close(); }

uint32_t Session::current() const { return impl_->current.load(); }

void Session::Open(const std::string& address) {
  impl_->services.Post([impl = impl_, address] {
    std::optional<url::Url> parsed = url::Parse(address);
    if (!parsed) return;
    Host::Expected expected;
    expected.url = url::Serialize(*parsed);
    impl->Transfer(nullptr, 0, std::move(expected));
  });
}

void Session::Go(const std::string& address) {
  impl_->services.Post([impl = impl_, address] { impl->GoNew(address); });
}

void Session::Back() {
  impl_->services.Post([impl = impl_] {
    int target;
    {
      std::lock_guard<std::mutex> lock(impl->mutex);
      target = impl->index - 1;
    }
    impl->GoToEntry(target);
  });
}

void Session::Forward() {
  impl_->services.Post([impl = impl_] {
    int target;
    {
      std::lock_guard<std::mutex> lock(impl->mutex);
      target = impl->index + 1;
    }
    impl->GoToEntry(target);
  });
}

bool Session::Send(uint32_t host, wire::ToRenderer type, const std::string& bytes) {
  std::shared_ptr<Host> found = impl_->Find(host);
  if (!found) return false;
  impl_->SendTo(*found, type, bytes);
  return true;
}

void Session::Broadcast(wire::ToRenderer type, const std::string& bytes) {
  std::vector<std::shared_ptr<Host>> all;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (type == wire::ToRenderer::Resize) impl_->resize = bytes;
    for (auto& [id, host] : impl_->hosts) all.push_back(host);
  }
  for (auto& host : all) impl_->SendTo(*host, type, bytes);
}

bool Session::Handle(uint32_t hostId, uint8_t type, const std::string& bytes) {
  std::shared_ptr<Host> host = impl_->Find(hostId);
  if (!host) return type == static_cast<uint8_t>(wire::ToBrowser::Navigate) || type == static_cast<uint8_t>(wire::ToBrowser::NavCommitted) || type == static_cast<uint8_t>(wire::ToBrowser::HistoryGo) ||
                    type == static_cast<uint8_t>(wire::ToBrowser::Storage);
  net::ipc::Reader in(bytes);
  switch (static_cast<wire::ToBrowser>(type)) {
    case wire::ToBrowser::Navigate: {
      wire::Navigate request;
      if (wire::Read(in, request)) impl_->services.Post([impl = impl_, host, request = std::move(request)]() mutable { impl->OnNavigate(host, std::move(request)); });
      return true;
    }
    case wire::ToBrowser::NavCommitted: {
      const std::string address = in.String(size_t{1} << 21);
      const uint32_t id = in.U32();
      if (in.ok()) impl_->services.Post([impl = impl_, host, address, id] { impl->OnCommitted(host, address, id); });
      return true;
    }
    case wire::ToBrowser::HistoryGo: {
      const int delta = in.I32();
      if (in.ok()) impl_->services.Post([impl = impl_, host, delta] { impl->OnHistoryGo(host, delta); });
      return true;
    }
    case wire::ToBrowser::Storage: {
      wire::StorageRequest request;
      if (wire::Read(in, request)) host->tab->Storage(std::move(request));
      return true;
    }
    default: return false;
  }
}

int Session::Close() {
  std::promise<void> done;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->closing) return impl_->exitCode;
    impl_->closing = true;
  }
  impl_->services.Post([impl = impl_, &done] {
    std::vector<std::shared_ptr<Host>> all;
    {
      std::lock_guard<std::mutex> lock(impl->mutex);
      for (auto& [id, host] : impl->hosts) all.push_back(host);
    }
    for (auto& host : all) impl->Retire(host, host->current);
    done.set_value();
  });
  done.get_future().wait();
  // (What the janitor was given is done before it is let go.)
  std::promise<void> flushed;
  impl_->Janitor([&flushed] { flushed.set_value(); });
  flushed.get_future().wait();
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->exitCode;
}

std::vector<std::string> Session::Entries() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->entries;
}

int Session::Index() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->index;
}

}  // namespace solar::gui
