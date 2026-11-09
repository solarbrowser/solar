/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/gui/Session.h"

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <cstdio>
#include <deque>
#include <future>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <thread>

#include "solar/gui/Chrome.h"
#include "solar/gui/Settings.h"
#include "solar/net/HttpClient.h"
#include "solar/url/Origin.h"
#include "solar/url/Parser.h"
#include "solar/url/Serializer.h"

namespace solar::gui {

namespace {

constexpr size_t kMostNavigations = 64;
constexpr size_t kLargestFile = size_t{256} << 20;

// A window's size, as the page is told it for a zoom: the page is laid out for a window `zoom` times smaller and drawn `zoom` times larger.
std::string ZoomedGeometry(const std::string& bytes, double zoom) {
  net::ipc::Reader in(bytes);
  wire::Geometry g;
  if (!wire::Read(in, g) || zoom == 1.0) return bytes;
  g.width = std::max(1, static_cast<int>(g.width / zoom));
  g.height = std::max(1, static_cast<int>(g.height / zoom));
  g.density = static_cast<float>(std::clamp(g.density * zoom, 0.25, 16.0));
  net::ipc::Writer out;
  wire::Write(out, g);
  return out.bytes();
}

// The host of an address (what zoom is kept by).
std::string HostOf(const std::string& address) {
  const size_t scheme = address.find("://");
  if (scheme == std::string::npos) return "";
  const size_t start = scheme + 3, end = address.find_first_of("/?#", start);
  return address.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

constexpr double kZoomLevels[] = {0.25, 0.33, 0.5, 0.67, 0.75, 0.8, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0, 4.0, 5.0};

// The site of an address: the web's registrable domain, and for the browser's own pages (solar://newtab/) the page.
std::string SiteFor(const url::Url& address) {
  if (address.scheme == "solar") return "solar:" + url::Serialize(address);
  return url::SiteOf(address);
}

std::string NormalAddress(const std::string& text) {
  std::optional<url::Url> parsed = url::Parse(text);
  return parsed ? url::Serialize(*parsed) : text;
}


// Where files are kept that are downloaded: the user's folder for them.
std::string DownloadDirectory() {
  namespace fs = std::filesystem;
  if (const char* forced = std::getenv("SOLAR_DOWNLOAD_DIR")) return forced;
  if (const std::string chosen = Settings::Instance().DownloadDirectory(); !chosen.empty()) return chosen;
#if defined(_WIN32)
  if (const char* profile = std::getenv("USERPROFILE")) return (fs::path(profile) / "Downloads").string();
#else
  const char* home = std::getenv("HOME");
  if (!home) return "";
#if !defined(__APPLE__)
  // (The folder the user's desktop says: a line XDG_DOWNLOAD_DIR="$HOME/Downloads" in ~/.config/user-dirs.dirs.)
  std::ifstream dirs((std::getenv("XDG_CONFIG_HOME") ? std::string(std::getenv("XDG_CONFIG_HOME")) : std::string(home) + "/.config") + "/user-dirs.dirs");
  std::string line;
  while (std::getline(dirs, line)) {
    if (line.rfind("XDG_DOWNLOAD_DIR=", 0) != 0) continue;
    std::string value = line.substr(17);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
    const std::string marker = "$HOME";
    if (value.rfind(marker, 0) == 0) value = std::string(home) + value.substr(marker.size());
    if (!value.empty() && value[0] == '/') return value;
  }
#endif
  return (fs::path(home) / "Downloads").string();
#endif
  return "";
}

// The name a download is kept under: the one the server says, or the last part of the address; with nothing in it that is a path, a control, or a dot to start with.
std::string DownloadName(const std::string& disposition, const url::Url& address) {
  std::string name;
  const auto unescape = [](const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '%' && i + 2 < text.size() + 0 && std::isxdigit(static_cast<unsigned char>(text[i + 1])) && std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
        out += static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16));
        i += 2;
      } else {
        out += text[i];
      }
    }
    return out;
  };
  const size_t star = disposition.find("filename*=");
  if (star != std::string::npos) {
    std::string value = disposition.substr(star + 10);
    value = value.substr(0, value.find(';'));
    const size_t quote = value.find("''");
    name = unescape(quote == std::string::npos ? value : value.substr(quote + 2));
  }
  if (name.empty()) {
    const size_t plain = disposition.find("filename=");
    if (plain != std::string::npos) {
      std::string value = disposition.substr(plain + 9);
      if (!value.empty() && value[0] == '"') {
        const size_t end = value.find('"', 1);
        value = value.substr(1, end == std::string::npos ? std::string::npos : end - 1);
      } else {
        value = value.substr(0, value.find(';'));
      }
      name = value;
    }
  }
  if (name.empty()) {
    const std::string path = address.path.empty() ? "" : url::Serialize(address);
    const size_t query = path.find_first_of("?#");
    const std::string clean = path.substr(0, query);
    const size_t slash = clean.rfind('/');
    name = unescape(slash == std::string::npos ? clean : clean.substr(slash + 1));
  }
  std::string safe;
  for (unsigned char c : name) {
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c < 0x20) safe += '_';
    else safe += static_cast<char>(c);
  }
  while (!safe.empty() && (safe.front() == '.' || safe.front() == ' ')) safe.erase(safe.begin());
  if (safe.size() > 150) safe.resize(150);
  return safe.empty() ? "download" : safe;
}

// A file that is not there yet: name, name (1), name (2) ...
std::string FreePath(const std::string& directory, const std::string& name) {
  namespace fs = std::filesystem;
  std::error_code error;
  const fs::path base = fs::path(directory) / name;
  if (!fs::exists(base, error)) return base.string();
  const std::string stem = base.stem().string(), extension = base.extension().string();
  for (int i = 1; i < 10000; ++i) {
    const fs::path candidate = fs::path(directory) / (stem + " (" + std::to_string(i) + ")" + extension);
    if (!fs::exists(candidate, error)) return candidate.string();
  }
  return base.string();
}

// What is shown as a page and what is a file to keep: an attachment is kept; so is what is not text, a picture, a sound, a film or one of the few kinds of documents that are read as pages.
bool IsDownload(const net::HttpResponseHead& head) {
  if (head.status < 200 || head.status >= 300 || head.status == 204) return false;
  const auto disposition = head.Header("content-disposition");
  if (disposition) {
    std::string lower(*disposition);
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.rfind("attachment", 0) == 0) return true;
  }
  const auto typeHeader = head.Header("content-type");
  if (!typeHeader) return false;
  std::string type(*typeHeader);
  type = type.substr(0, type.find(';'));
  for (char& c : type) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  while (!type.empty() && type.back() == ' ') type.pop_back();
  if (type.empty() || type.rfind("text/", 0) == 0 || type.rfind("image/", 0) == 0 || type.rfind("audio/", 0) == 0 || type.rfind("video/", 0) == 0) return false;
  for (const char* page : {"application/xhtml+xml", "application/xml", "application/json", "application/javascript", "application/x-javascript", "application/ecmascript", "application/rss+xml", "application/atom+xml", "application/ld+json", "application/manifest+json"}) {
    if (type == page) return false;
  }
  return type.rfind("application/", 0) == 0 || type.rfind("font/", 0) == 0 || type.rfind("model/", 0) == 0 || type.rfind("multipart/", 0) == 0;
}
}  // namespace

struct Navigation;

// A renderer, as the session has it.
struct Host {
  uint32_t id = 0;
  long child = -1;
  std::string site;  // the one it is made for
  std::unique_ptr<net::ipc::Channel> link;
  std::unique_ptr<BrowserServices::Tab> tab_;  // (what the network service of the renderer is)
  std::thread reader;
  bool chrome = false;       // the browser's interface, not a page
  uint32_t tab = 0;          // the tab it is a renderer of
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
  std::atomic<uint32_t> chromeId{0};
  // A tab: its own history, the renderer that has its page now, and the one that is on its way to it.
  struct TabState {
    uint32_t id = 0;
    uint32_t current = 0, pending = 0;  // renderers
    std::vector<std::string> entries;
    int index = -1;
    std::string title;      // what the page is called
    bool pinned = false;    // kept at the start of the tabs, small
    double zoom = 1.0;      // how much larger the page is drawn than it is laid out
    bool loading = false;   // an address is being gone to
  };
  std::map<uint32_t, TabState> tabs;
  struct Download {
    uint32_t id = 0;
    std::string url, name, path;
    uint64_t received = 0, total = 0;
    enum class State { Running, Done, Failed, Cancelled } state = State::Running;
    uint64_t serial = 0;  // of the navigation that is bringing it
    bool asking = false;  // the user is being asked where it is to be kept (it is in a file of its own until then)
    std::string chosen;   // where the user said
  };
  std::vector<Download> downloads;  // (under the lock; the oldest first)
  uint32_t nextDownload = 1;
  std::map<std::string, double> siteZoom;  // what the user chose for a site (its host), for the pages of it
  std::vector<uint32_t> order;  // the tabs, as they are shown from left to right
  uint32_t active = 0;
  uint32_t nextTab = 1;
  std::atomic<uint32_t> shownHost{0};  // the renderer of the active tab's page: what the window shows
  bool windowControls = false, maximized = false;  // the interface draws the buttons of the window (the system's bar is not there)
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

  std::shared_ptr<Host> StartHost(uint32_t tab, const std::string& site, const std::string& url, std::optional<Host::Expected> expected, bool chrome = false);
  void StartChrome(const std::string& geometry, const std::string& address);
  void OnChromeNavigate(const std::shared_ptr<Host>& host, const wire::Navigate& request);
  void OnInternalNavigate(const std::shared_ptr<Host>& host, const wire::Navigate& request);
  void Serve(Host& host, uint32_t id, const std::string& address, const std::string& document);
  void SetLoading(uint32_t tab, bool value);
  void ApplySiteZoom(const std::shared_ptr<Host>& host);
  void ChangeZoom(int step);
  // The tab of a renderer that was made for an address that turned out to be a file has no page: it is given the page of a new tab (or, if it has one, let be).
  void AbandonPending(const std::shared_ptr<Host>& host);
  void FinishDownload(uint32_t id, Download::State state);
  void MoveChosen(uint32_t id);
  std::string DownloadsJsonLocked(size_t most);
  void DownloadProgress(uint32_t id, uint64_t received);
  void PushState();
  uint32_t NewTab(const std::string& address, bool activate);
  // The icons of the pages: asked for by the renderer of a tab, fetched here, kept, and given to the interface for the tab.
  void OnFavicon(const std::shared_ptr<Host>& host, const std::string& address);
  void PushIcon(uint32_t tab, const std::string& address, const std::string& data);
  std::map<std::string, std::string> icons;  // the address -> a data: address of the picture (the loop's thread)
  std::map<std::string, std::vector<uint32_t>> iconsWaiting;  // the fetches that are going on, and the tabs that wait for them
  std::vector<std::shared_ptr<struct IconFetch>> iconFetches;
  void CloseTab(uint32_t tab);
  void Activate(uint32_t tab);
  // (the lock is held by the caller)
  TabState* ActiveTab() {
    const auto found = tabs.find(active);
    return found == tabs.end() ? nullptr : &found->second;
  }
  void Retire(const std::shared_ptr<Host>& host, bool takeCode = false);
  void OnNavigate(const std::shared_ptr<Host>& host, wire::Navigate request);
  void OnCommitted(const std::shared_ptr<Host>& host, const std::string& address, uint32_t id);
  void OnHistoryGo(const std::shared_ptr<Host>& host, int delta);
  void GoToEntry(uint32_t tab, int entry);
  void GoNew(uint32_t tab, const std::string& address);
  void OnGone(uint32_t id);
  void StartNavigation(const std::shared_ptr<Host>& host, uint32_t id, const url::Url& address, const std::string& method, const std::string& body, const std::string& bodyType, wire::Navigate::Kind kind, int entry);
  void Transfer(uint32_t tab, const std::shared_ptr<Host>& from, uint32_t id, Host::Expected expected);
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
  // When the response is a file: where it is going, and the download that is it.
  std::ofstream file;
  uint32_t downloadId = 0;
  uint64_t received = 0;
  std::chrono::steady_clock::time_point lastPush{};

  struct Event {
    enum class Type { Head, Body, End, Error } type;
    int status = 0;
    std::string text, address;
  };
  std::vector<Event> buffered;

  void Emit(Event event) {
    if (downloadId) return;  // (a file is not the renderer's)
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
        if (host.current) session->SetLoading(host.tab, false);
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
      if (event.type == Event::Type::Head) host->tab_->AllowOrigin(url::SerializeOrigin(*url::Parse(event.address)));
      Emit(std::move(event));
    }
  }

  void OnResponseHead(const net::HttpResponseHead& head, const url::Url& finalUrl) override {
    std::string type;
    if (auto value = head.Header("content-type")) type = std::string(*value);
    if (!downloadId && IsDownload(head)) {
      BeginDownload(head, finalUrl);
      return;
    }
    Event event{Event::Type::Head, head.status, type, url::Serialize(finalUrl)};
    if (target && SiteFor(finalUrl) != target->site) {
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
      session->Transfer(from->tab, from, fromId, std::move(expected));
      return;
    }
    if (target) target->tab_->AllowOrigin(url::SerializeOrigin(finalUrl));
    Emit(std::move(event));
  }
  void BeginDownload(const net::HttpResponseHead& head, const url::Url& finalUrl);
  void OnBody(std::span<const uint8_t> data) override {
    if (downloadId) {
      file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
      received += data.size();
      const auto now = std::chrono::steady_clock::now();
      if (!file) {
        handle.Cancel();
        session->FinishDownload(downloadId, Session::Impl::Download::State::Failed);
        downloadId = 0;
        Finish();
        return;
      }
      if (now - lastPush > std::chrono::milliseconds(150)) {
        lastPush = now;
        session->DownloadProgress(downloadId, received);
      }
      return;
    }
    bodySent = true;
    Emit(Event{Event::Type::Body, 0, std::string(reinterpret_cast<const char*>(data.data()), data.size()), ""});
  }
  void OnEnd() override {
    if (downloadId) {
      file.close();
      session->DownloadProgress(downloadId, received);
      session->FinishDownload(downloadId, Session::Impl::Download::State::Done);
      Finish();
      return;
    }
    Emit(Event{Event::Type::End, 0, "", ""});
    Finish();
  }
  void OnError(std::string_view message) override {
    if (downloadId) {
      file.close();
      session->FinishDownload(downloadId, Session::Impl::Download::State::Failed);
      Finish();
      return;
    }
    Emit(Event{Event::Type::Error, 0, std::string(message), ""});
    Finish();
  }
  bool OnRestart() override {
    if (downloadId) return false;  // (what was written is not taken back)
    // (Asked for again from the start, as long as the renderer has none of the body to take back.)
    if (bodySent || !target) return false;
    net::ipc::Writer out;
    out.U32(id);
    session->SendTo(*target, wire::ToRenderer::NavRestart, out.bytes());
    return true;
  }
};

// The fetch of an icon: it is small (at most 128 KiB), and an image.
struct IconFetch : net::FetchHandler {
  Session::Impl* session = nullptr;
  std::string address, type, body;
  int status = 0;
  bool tooLarge = false;
  net::FetchHandle handle;
  void OnResponseHead(const net::HttpResponseHead& head, const url::Url&) override {
    status = head.status;
    if (auto value = head.Header("content-type")) type = std::string(*value);
  }
  void OnBody(std::span<const uint8_t> data) override {
    if (body.size() + data.size() > (size_t{128} << 10)) {
      tooLarge = true;
      return;
    }
    body.append(reinterpret_cast<const char*>(data.data()), data.size());
  }
  void OnEnd() override;
  void OnError(std::string_view) override { Done(""); }
  void Done(const std::string& data);
};

namespace {
std::string Base64(const std::string& in) {
  static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  for (size_t i = 0; i < in.size(); i += 3) {
    const uint32_t n = (static_cast<uint8_t>(in[i]) << 16) | (i + 1 < in.size() ? static_cast<uint8_t>(in[i + 1]) << 8 : 0) | (i + 2 < in.size() ? static_cast<uint8_t>(in[i + 2]) : 0);
    out += table[(n >> 18) & 63];
    out += table[(n >> 12) & 63];
    out += i + 1 < in.size() ? table[(n >> 6) & 63] : '=';
    out += i + 2 < in.size() ? table[n & 63] : '=';
  }
  return out;
}
}  // namespace

// ---- Hosts ----

std::shared_ptr<Host> Session::Impl::StartHost(uint32_t tab, const std::string& site, const std::string& url, std::optional<Host::Expected> expected, bool chrome) {
  auto windowPair = net::ipc::Channel::MakePair();
  auto networkPair = net::ipc::Channel::MakePair();
  if (windowPair.first < 0 || networkPair.first < 0) return nullptr;
  auto host = std::make_shared<Host>();
  host->site = site;
  host->chrome = chrome;
  host->tab = tab;
  host->expected = std::move(expected);
  // (The renderer's ends go to the launcher, which takes them over.)
  host->child = launcher.start(windowPair.second, networkPair.second, url);
  if (host->child < 0) {
    ::close(windowPair.first);
    ::close(networkPair.first);
    return nullptr;
  }
  host->link = std::make_unique<net::ipc::Channel>(windowPair.first);
  host->tab_ = services.OpenTab(networkPair.first, *host->link);
  std::optional<std::string> size;
  {
    std::lock_guard<std::mutex> lock(mutex);
    host->id = nextHost++;
    hosts[host->id] = host;
    size = resize;
  }
  if (size && !chrome) {
    double zoom = 1.0;
    {
      std::lock_guard<std::mutex> lock(mutex);
      const auto found = tabs.find(tab);
      if (found != tabs.end()) zoom = found->second.zoom;
    }
    SendTo(*host, wire::ToRenderer::Resize, ZoomedGeometry(*size, zoom));
  }
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
    const auto found = tabs.find(host->tab);
    if (found != tabs.end() && found->second.pending == host->id) found->second.pending = 0;
  }
  SendTo(*host, wire::ToRenderer::Shutdown, "");
  Janitor([this, host, takeCode] {
    host->link->Shutdown();
    if (host->reader.joinable()) host->reader.join();
    host->tab_.reset();
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
  if (host->chrome) {
    // (The interface's process is gone: the window goes on without a bar. A new one may already be there: it is not the one that is gone.)
    uint32_t expected = id;
    chromeId.compare_exchange_strong(expected, 0);
    events.gone(id, false);
    return;
  }
  const uint32_t tab = host->tab;
  bool closeTab = false;
  {
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = tabs.find(tab);
    if (found != tabs.end()) {
      if (found->second.current == id) {
        found->second.current = 0;
        closeTab = true;
      } else if (found->second.pending == id) {
        found->second.pending = 0;
        // (A renderer that goes before there is any page in the tab: there is nothing to show, and nothing to come.)
        closeTab = found->second.current == 0;
      }
    }
  }
  if (!closeTab) {
    SetLoading(tab, false);
    events.gone(id, false);
    return;
  }
  CloseTab(tab);
  bool last;
  {
    std::lock_guard<std::mutex> lock(mutex);
    last = tabs.empty();
  }
  events.gone(id, last);
}

void Session::Impl::Swap(const std::shared_ptr<Host>& host) {
  std::shared_ptr<Host> old;
  bool shown = false;
  {
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = tabs.find(host->tab);
    if (found == tabs.end()) return;
    TabState& t = found->second;
    const auto before = hosts.find(t.current);
    if (before != hosts.end()) old = before->second;
    t.current = host->id;
    if (t.pending == host->id) t.pending = 0;
    t.title.clear();
    shown = active == t.id;
    if (shown) shownHost.store(host->id);
  }
  host->current = true;
  if (shown) events.shown(host->id);
  if (old && old != host) Retire(old);
}

uint32_t Session::Impl::NewTab(const std::string& address, bool activate) {
  std::string text = address.empty() ? kNewTabAddress : address;
  std::optional<url::Url> parsed = url::Parse(text);
  if (!parsed) {
    text = kNewTabAddress;
    parsed = url::Parse(text);
  }
  uint32_t id;
  {
    std::lock_guard<std::mutex> lock(mutex);
    id = nextTab++;
    tabs[id].id = id;
    order.push_back(id);
  }
  if (activate) Activate(id);
  else PushState();
  Host::Expected expected;
  expected.url = url::Serialize(*parsed);
  Transfer(id, nullptr, 0, std::move(expected));
  return id;
}

void Session::Impl::Activate(uint32_t tab) {
  uint32_t shown;
  {
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = tabs.find(tab);
    if (found == tabs.end()) return;
    active = tab;
    shown = found->second.current;
    shownHost.store(shown);
  }
  events.shown(shown);
  PushState();
}

void Session::Impl::CloseTab(uint32_t tab) {
  std::vector<std::shared_ptr<Host>> mine;
  uint32_t next = 0;
  bool wasActive = false, last = false;
  {
    std::lock_guard<std::mutex> lock(mutex);
    const auto where = std::find(order.begin(), order.end(), tab);
    if (where == order.end()) return;
    for (auto& [id, host] : hosts) {
      if (host->tab == tab && !host->chrome) mine.push_back(host);
    }
    const size_t position = static_cast<size_t>(where - order.begin());
    order.erase(where);
    tabs.erase(tab);
    wasActive = active == tab;
    last = order.empty();
    if (wasActive && !order.empty()) next = order[std::min(position, order.size() - 1)];
  }
  for (auto& host : mine) Retire(host, host->current && last);
  if (wasActive) {
    if (next) {
      Activate(next);
    } else {
      {
        std::lock_guard<std::mutex> lock(mutex);
        active = 0;
        shownHost.store(0);
      }
      events.shown(0);
    }
  }
  PushState();
}

// ---- Navigation ----

void Session::Impl::Transfer(uint32_t tab, const std::shared_ptr<Host>& from, uint32_t id, Host::Expected expected) {
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
    const auto found = tabs.find(tab);
    if (found == tabs.end()) return;
    if (found->second.pending) {
      const auto pending = hosts.find(found->second.pending);
      if (pending != hosts.end()) before = pending->second;
    }
  }
  if (before && before != from) Retire(before);
  const std::string address = expected.url;
  std::shared_ptr<Host> host = StartHost(tab, SiteFor(*parsed), address, std::move(expected));
  if (!host) return;
  bool kept = false;
  {
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = tabs.find(tab);
    if (found != tabs.end()) {
      found->second.pending = host->id;
      kept = true;
    }
  }
  if (!kept) Retire(host);  // (the tab was closed meanwhile)
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
    host->tab_->AllowOrigin(url::SerializeOrigin(address));
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
  if (host->chrome) return OnChromeNavigate(host, request);
  if (host->current) SetLoading(host->tab, true);
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
  if (parsed->scheme == "solar") {
    // The browser's own pages are served from its memory, to the renderer that was made for them and to no other.
    const std::string where = url::Serialize(*parsed);
    if ((where != kNewTabAddress && where != kSettingsAddress && where != kDownloadsAddress) || SiteFor(*parsed) != host->site) return NavError(*host, request.id, "unsupported scheme");
    host->infos[request.id] = {kind, entry};
    request.kind = kind;
    request.entry = entry;
    return OnInternalNavigate(host, request);
  }
  if (parsed->scheme == "file") {
    if (!allowFiles) return NavError(*host, request.id, "this page may not open files");
  } else if (parsed->scheme != "http" && parsed->scheme != "https") {
    return NavError(*host, request.id, "unsupported scheme");
  }
  if (kind == wire::Navigate::Kind::Traversal) {
    // (A step along the history is to an entry there is, for the address the entry has.)
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = tabs.find(host->tab);
    if (found == tabs.end() || entry < 0 || entry >= static_cast<int>(found->second.entries.size()) || found->second.entries[static_cast<size_t>(entry)] != url::Serialize(*parsed)) {
      kind = wire::Navigate::Kind::New;
      entry = -1;
    }
  }
  const std::string site = SiteFor(*parsed);
  if (site.empty() || site != host->site) {
    Host::Expected expected;
    expected.url = url::Serialize(*parsed);
    expected.method = method;
    expected.body = body;
    expected.bodyType = bodyType;
    expected.kind = kind;
    expected.entry = entry;
    Transfer(host->tab, host, request.id, std::move(expected));
    return;
  }
  StartNavigation(host, request.id, *parsed, method, body, bodyType, kind, entry);
}

void Session::Impl::OnCommitted(const std::shared_ptr<Host>& host, const std::string& address, uint32_t id) {
  if (host->gone) return;
  std::optional<url::Url> parsed = url::Parse(address);
  // (A renderer commits to documents of its own site: what it says it is on is checked.)
  if (!parsed || SiteFor(*parsed) != host->site) return;
  const auto found = host->infos.find(id);
  Host::Info info{wire::Navigate::Kind::New, -1};  // (a page that did it itself, as a service worker's answer: a new entry)
  if (found != host->infos.end()) {
    info = found->second;
    host->infos.erase(host->infos.begin(), std::next(found));
  } else if (id != 0) {
    return;
  }
  if (host->tab_) host->tab_->KeepOnlyOrigin(url::SerializeOrigin(*parsed));
  const std::string text = url::Serialize(*parsed);
  {
    std::lock_guard<std::mutex> lock(mutex);
    const auto tabFound = tabs.find(host->tab);
    if (tabFound == tabs.end()) return;
    TabState& t = tabFound->second;
    switch (info.kind) {
      case wire::Navigate::Kind::New:
        if (t.index < 0 || t.entries[static_cast<size_t>(t.index)] != text) {
          t.entries.resize(static_cast<size_t>(t.index + 1));
          t.entries.push_back(text);
          t.index = static_cast<int>(t.entries.size()) - 1;
        }
        break;
      case wire::Navigate::Kind::Reload:
        if (t.index >= 0) t.entries[static_cast<size_t>(t.index)] = text;
        break;
      case wire::Navigate::Kind::Traversal:
        if (info.entry >= 0 && info.entry < static_cast<int>(t.entries.size())) {
          t.index = info.entry;
          t.entries[static_cast<size_t>(t.index)] = text;
        }
        break;
    }
  }
  if (!host->current) Swap(host);
  ApplySiteZoom(host);
  SetLoading(host->tab, false);
}

// The tab has the zoom that was chosen for the site it is on (100% where none was), and its page is told when that is not what it was.
void Session::Impl::ApplySiteZoom(const std::shared_ptr<Host>& host) {
  std::string base;
  double zoom = 1.0;
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = tabs.find(host->tab);
    if (found == tabs.end()) return;
    TabState& t = found->second;
    const std::string address = t.index >= 0 && t.index < static_cast<int>(t.entries.size()) ? t.entries[static_cast<size_t>(t.index)] : "";
    const auto chosen = siteZoom.find(HostOf(address));
    zoom = chosen == siteZoom.end() ? 1.0 : chosen->second;
    if (zoom != t.zoom) {
      t.zoom = zoom;
      changed = true;
    }
    if (resize) base = *resize;
  }
  if (changed && !base.empty()) SendTo(*host, wire::ToRenderer::Resize, ZoomedGeometry(base, zoom));
  if (changed) PushState();
}

void Session::Impl::ChangeZoom(int step) {
  std::shared_ptr<Host> host;
  std::string base;
  double zoom = 1.0;
  {
    std::lock_guard<std::mutex> lock(mutex);
    TabState* t = ActiveTab();
    if (!t) return;
    size_t at = 0;
    for (size_t i = 0; i < std::size(kZoomLevels); ++i) {
      if (std::abs(kZoomLevels[i] - t->zoom) < std::abs(kZoomLevels[at] - t->zoom)) at = i;
    }
    if (step == 0) at = 7;
    else if (step > 0 && at + 1 < std::size(kZoomLevels)) ++at;
    else if (step < 0 && at > 0) --at;
    zoom = kZoomLevels[at];
    t->zoom = zoom;
    const std::string address = t->index >= 0 && t->index < static_cast<int>(t->entries.size()) ? t->entries[static_cast<size_t>(t->index)] : "";
    if (!HostOf(address).empty()) {
      if (zoom == 1.0) siteZoom.erase(HostOf(address));
      else siteZoom[HostOf(address)] = zoom;
    }
    const auto current = hosts.find(t->current);
    if (current != hosts.end()) host = current->second;
    if (resize) base = *resize;
  }
  if (host && !base.empty()) SendTo(*host, wire::ToRenderer::Resize, ZoomedGeometry(base, zoom));
  PushState();
}

void Session::Impl::GoToEntry(uint32_t tab, int entry) {
  std::string address;
  std::shared_ptr<Host> now;
  {
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = tabs.find(tab);
    if (found == tabs.end() || entry < 0 || entry >= static_cast<int>(found->second.entries.size())) return;
    address = found->second.entries[static_cast<size_t>(entry)];
    const auto host = hosts.find(found->second.current);
    if (host != hosts.end()) now = host->second;
  }
  std::optional<url::Url> parsed = url::Parse(address);
  if (!parsed) return;
  SetLoading(tab, true);
  if (now && SiteFor(*parsed) == now->site) {
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
  Transfer(tab, nullptr, 0, std::move(expected));
}

void Session::Impl::GoNew(uint32_t tab, const std::string& address) {
  std::optional<url::Url> parsed = url::Parse(address);
  if (!parsed) return;
  std::shared_ptr<Host> now;
  {
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = tabs.find(tab);
    if (found == tabs.end()) return;
    const auto host = hosts.find(found->second.current);
    if (host != hosts.end()) now = host->second;
  }
  SetLoading(tab, true);
  if (now && SiteFor(*parsed) == now->site) {
    net::ipc::Writer out;
    out.String(url::Serialize(*parsed));
    out.I32(-1);
    SendTo(*now, wire::ToRenderer::GoTo, out.bytes());
    return;
  }
  Host::Expected expected;
  expected.url = url::Serialize(*parsed);
  Transfer(tab, nullptr, 0, std::move(expected));
}

void Session::Impl::OnHistoryGo(const std::shared_ptr<Host>& host, int delta) {
  if (host->gone || !host->current || (delta != -1 && delta != 1)) return;
  int target;
  {
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = tabs.find(host->tab);
    if (found == tabs.end()) return;
    target = found->second.index + delta;
  }
  GoToEntry(host->tab, target);
}

// ---- Downloads ----

void Navigation::BeginDownload(const net::HttpResponseHead& head, const url::Url& finalUrl) {
  const std::string directory = DownloadDirectory();
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  const std::string disposition = head.Header("content-disposition") ? std::string(*head.Header("content-disposition")) : "";
  const std::string suggested = FreePath(directory, DownloadName(disposition, finalUrl));
  // When the user is asked where it is to be kept, it comes into a file of its own in the meantime.
  const bool ask = Settings::Instance().AskWhereToSave() && session->events.askSave;
  const std::string path = ask ? FreePath(directory, std::filesystem::path(suggested).filename().string() + ".part") : suggested;
  file.open(path, std::ios::binary | std::ios::trunc);
  Session::Impl::Download record;
  record.url = url::Serialize(finalUrl);
  record.name = std::filesystem::path(suggested).filename().string();
  record.path = path;
  record.asking = ask;
  record.serial = serial;
  if (const auto length = head.Header("content-length")) record.total = static_cast<uint64_t>(std::strtoull(std::string(*length).c_str(), nullptr, 10));
  if (!file) {
    record.state = Session::Impl::Download::State::Failed;
  }
  {
    std::lock_guard<std::mutex> lock(session->mutex);
    record.id = session->nextDownload++;
    downloadId = record.id;
    session->downloads.push_back(record);
    if (session->downloads.size() > 30) session->downloads.erase(session->downloads.begin());
  }
  // The page that asked stays as it is; a renderer that was made for the address, and has no page, is let go of.
  if (target) {
    std::shared_ptr<Host> from = target;
    target = nullptr;
    net::ipc::Writer out;
    out.U32(id);
    session->SendTo(*from, wire::ToRenderer::NavTransferred, out.bytes());
    from->active.erase(id);
    from->infos.erase(id);
    session->AbandonPending(from);
  }
  session->PushState();
  if (ask && file) session->events.askSave(downloadId, suggested);
  if (!file) {
    handle.Cancel();
    Finish();
  }
}

void Session::Impl::AbandonPending(const std::shared_ptr<Host>& host) {
  if (host->gone) return;
  if (host->current) {
    // (The page stays: the address was a file.)
    SetLoading(host->tab, false);
    return;
  }
  const uint32_t tab = host->tab;
  Retire(host);
  bool empty = false;
  {
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = tabs.find(tab);
    empty = found != tabs.end() && found->second.current == 0 && found->second.pending == 0;
  }
  if (!empty) {
    SetLoading(tab, false);
    return;
  }
  // (A tab that was made for a file has no page: it has the page of a new tab.)
  Host::Expected expected;
  expected.url = kNewTabAddress;
  Transfer(tab, nullptr, 0, std::move(expected));
}

void Session::Impl::DownloadProgress(uint32_t id, uint64_t received) {
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (Download& d : downloads) {
      if (d.id == id) d.received = received;
    }
  }
  PushState();
}

void Session::Impl::FinishDownload(uint32_t id, Download::State state) {
  bool move = false;
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (Download& d : downloads) {
      if (d.id == id) {
        d.state = state;
        if (state == Download::State::Done && d.total == 0) d.total = d.received;
        move = state == Download::State::Done && d.asking && !d.chosen.empty();
        if (state != Download::State::Done && d.asking) {
          std::error_code error;
          std::filesystem::remove(d.path, error);
        }
      }
    }
  }
  if (move) MoveChosen(id);
  PushState();
}

// The file that was kept aside while the user was asked goes to the place that was chosen.
void Session::Impl::MoveChosen(uint32_t id) {
  std::string from, to;
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (Download& d : downloads) {
      if (d.id == id && d.asking && !d.chosen.empty()) {
        from = d.path;
        to = d.chosen;
      }
    }
  }
  if (from.empty()) return;
  std::error_code error;
  std::filesystem::create_directories(std::filesystem::path(to).parent_path(), error);
  std::filesystem::rename(from, to, error);
  if (error) {
    // (Another disk: copied, and the one that was aside taken away.)
    std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, error);
    if (!error) std::filesystem::remove(from);
  }
  std::lock_guard<std::mutex> lock(mutex);
  for (Download& d : downloads) {
    if (d.id == id) {
      if (error) {
        d.state = Download::State::Failed;
      } else {
        d.path = to;
        d.name = std::filesystem::path(to).filename().string();
        d.asking = false;
      }
    }
  }
}

// ---- The interface ----

namespace {
std::string JsonString(const std::string& text) {
  std::string out = "\"";
  for (unsigned char c : text) {
    if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
    else out += static_cast<char>(c);
  }
  return out + "\"";
}
}  // namespace

void IconFetch::OnEnd() {
  std::string lower;
  for (char c : type.substr(0, type.find(';'))) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  while (!lower.empty() && lower.back() == ' ') lower.pop_back();
  const bool image = lower.rfind("image/", 0) == 0 || lower == "application/octet-stream" || lower.empty();
  if (status != 200 || tooLarge || body.empty() || !image) return Done("");
  Done("data:" + (lower.rfind("image/", 0) == 0 ? lower : std::string("image/x-icon")) + ";base64," + Base64(body));
}

void IconFetch::Done(const std::string& data) {
  const std::string key = address;
  const std::string text = data;
  session->services.Post([impl = session->shared_from_this(), key, text, self = this] {
    // (What is kept is for the next tab that has the same icon; a miss is kept too, so that a site with none is not asked again and again.)
    if (impl->icons.size() > 96) impl->icons.clear();
    impl->icons[key] = text;
    const auto waiting = impl->iconsWaiting.find(key);
    if (waiting != impl->iconsWaiting.end()) {
      if (!text.empty()) {
        for (uint32_t tab : waiting->second) impl->PushIcon(tab, key, text);
      }
      impl->iconsWaiting.erase(waiting);
    }
    impl->iconFetches.erase(std::remove_if(impl->iconFetches.begin(), impl->iconFetches.end(), [self](const std::shared_ptr<IconFetch>& f) { return f.get() == self; }), impl->iconFetches.end());
  });
}

void Session::Impl::OnFavicon(const std::shared_ptr<Host>& host, const std::string& address) {
  if (host->gone || !host->current || host->chrome) return;
  std::optional<url::Url> parsed = url::Parse(address);
  if (!parsed || (parsed->scheme != "http" && parsed->scheme != "https")) return;
  const std::string key = url::Serialize(*parsed);
  const auto kept = icons.find(key);
  if (kept != icons.end()) {
    if (!kept->second.empty()) PushIcon(host->tab, key, kept->second);
    return;
  }
  auto& waiting = iconsWaiting[key];
  const bool started = !waiting.empty();
  if (std::find(waiting.begin(), waiting.end(), host->tab) == waiting.end()) waiting.push_back(host->tab);
  if (started) return;
  auto fetch = std::make_shared<IconFetch>();
  fetch->session = this;
  fetch->address = key;
  iconFetches.push_back(fetch);
  fetch->handle = services.Fetch(*parsed, *fetch, net::FetchOptions());
}

void Session::Impl::PushIcon(uint32_t tab, const std::string& address, const std::string& data) {
  std::shared_ptr<Host> bar = Find(chromeId.load());
  if (!bar || bar->gone) return;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!tabs.count(tab)) return;
  }
  SendTo(*bar, wire::ToRenderer::UiState, "{\"icon\":{\"tab\":" + std::to_string(tab) + ",\"url\":" + JsonString(address) + ",\"data\":" + JsonString(data) + "}}");
}

void Session::Impl::StartChrome(const std::string& geometry, const std::string& address) {
  // (An interface that is there is let go: the window has another layout of it.)
  if (std::shared_ptr<Host> before = Find(chromeId.load())) {
    chromeId.store(0);
    Retire(before);
  }
  std::shared_ptr<Host> host = StartHost(0, address, address, std::nullopt, true);
  if (!host) return;
  SendTo(*host, wire::ToRenderer::Resize, geometry);
  chromeId.store(host->id);
}

void Session::Impl::OnChromeNavigate(const std::shared_ptr<Host>& host, const wire::Navigate& request) {
  // The interface's process is given its one document, from the browser's own memory, and nothing else: it has no network to ask for more.
  if (request.url != host->site || !IsInterfaceAddress(request.url)) return NavError(*host, request.id, "the interface may not go anywhere");
  Serve(*host, request.id, request.url, request.url == kSideTabsAddress ? SideTabsDocument() : request.url == kSidebarAddress ? SidebarDocument() : ChromeDocument());
}

void Session::Impl::OnInternalNavigate(const std::shared_ptr<Host>& host, const wire::Navigate& request) {
  host->active.insert(request.id);
  const std::optional<url::Url> parsed = url::Parse(request.url);
  const std::string where = parsed ? url::Serialize(*parsed) : "";
  if (where == kSettingsAddress) Serve(*host, request.id, kSettingsAddress, SettingsDocument());
  else if (where == kDownloadsAddress) Serve(*host, request.id, kDownloadsAddress, DownloadsDocument());
  else Serve(*host, request.id, kNewTabAddress, NewTabDocument());
  host->active.erase(request.id);
}

void Session::Impl::Serve(Host& host, uint32_t id, const std::string& address, const std::string& document) {
  net::ipc::Writer head;
  wire::Write(head, wire::NavHead{id, 200, "text/html; charset=utf-8", address});
  SendTo(host, wire::ToRenderer::NavHead, head.bytes());
  net::ipc::Writer body;
  body.U32(id);
  body.String(document);
  SendTo(host, wire::ToRenderer::NavBody, body.bytes());
  net::ipc::Writer end;
  end.U32(id);
  SendTo(host, wire::ToRenderer::NavEnd, end.bytes());
}

void Session::Impl::SetLoading(uint32_t tab, bool value) {
  {
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = tabs.find(tab);
    if (found != tabs.end()) found->second.loading = value;
  }
  PushState();
}

// The downloads as a list in JSON, the newest first, `most` of them (the lock is held).
std::string Session::Impl::DownloadsJsonLocked(size_t most) {
  std::string json = "[";
  bool first = true;
  size_t count = 0;
  for (auto it = downloads.rbegin(); it != downloads.rend() && count < most; ++it, ++count) {
    if (!first) json += ",";
    first = false;
    const char* state = it->state == Download::State::Running ? "running" : it->state == Download::State::Done ? "done" : it->state == Download::State::Failed ? "failed" : "cancelled";
    json += "{\"id\":" + std::to_string(it->id) + ",\"name\":" + JsonString(it->name) + ",\"url\":" + JsonString(it->url) + ",\"received\":" + std::to_string(it->received) + ",\"total\":" + std::to_string(it->total) +
            ",\"state\":\"" + state + "\",\"asking\":" + (it->asking && it->chosen.empty() ? "true" : "false") + "}";
  }
  return json + "]";
}

void Session::Impl::PushState() {
  // The page of the downloads is told them all, whether there is an interface or not.
  std::vector<std::shared_ptr<Host>> pages;
  std::string everything;
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& [id, host] : hosts) {
      if (!host->gone && host->site == std::string("solar:") + kDownloadsAddress) pages.push_back(host);
    }
    if (!pages.empty()) everything = "{\"downloads\":" + DownloadsJsonLocked(200) + "}";
  }
  for (auto& page : pages) SendTo(*page, wire::ToRenderer::UiState, everything);
  std::shared_ptr<Host> bar = Find(chromeId.load());
  if (!bar || bar->gone) return;
  std::string json;
  {
    std::lock_guard<std::mutex> lock(mutex);
    const auto addressOf = [](const TabState& t) { return t.index >= 0 && t.index < static_cast<int>(t.entries.size()) ? t.entries[static_cast<size_t>(t.index)] : std::string(); };
    json = "{\"tabs\":[";
    bool first = true;
    for (uint32_t id : order) {
      const TabState& t = tabs.at(id);
      if (!first) json += ",";
      first = false;
      json += "{\"id\":" + std::to_string(id) + ",\"title\":" + JsonString(t.title) + ",\"url\":" + JsonString(addressOf(t)) + ",\"loading\":" + (t.loading ? "true" : "false") + ",\"pinned\":" + (t.pinned ? "true" : "false") + "}";
    }
    json += "],\"active\":" + std::to_string(active);
    if (const TabState* t = ActiveTab()) {
      json += ",\"url\":" + JsonString(addressOf(*t)) + ",\"title\":" + JsonString(t->title) + ",\"back\":" + (t->index > 0 ? "true" : "false") + ",\"forward\":" + (t->index + 1 < static_cast<int>(t->entries.size()) ? "true" : "false") +
              ",\"loading\":" + (t->loading ? "true" : "false");
    }
    json += ",\"downloads\":" + DownloadsJsonLocked(8);
    if (const TabState* t = ActiveTab()) json += ",\"zoom\":" + std::to_string(static_cast<int>(std::lround(t->zoom * 100)));
    json += std::string(",\"controls\":") + (windowControls ? "true" : "false") + ",\"maximized\":" + (maximized ? "true" : "false") + "}";
  }
  SendTo(*bar, wire::ToRenderer::UiState, json);
}

// ---- Session ----

Session::Session(BrowserServices& services, Launcher launcher, Events events, bool allowFiles) : impl_(std::make_shared<Impl>(services, std::move(launcher), std::move(events), allowFiles)) {}

Session::~Session() { Close(); }

uint32_t Session::current() const { return impl_->shownHost.load(); }

void Session::Open(const std::string& address) {
  impl_->services.Post([impl = impl_, address] {
    if (!url::Parse(address)) return;
    impl->NewTab(address, true);
  });
}

void Session::NewTab(const std::string& address) {
  impl_->services.Post([impl = impl_, address] { impl->NewTab(address, true); });
}

void Session::ShowPage(const std::string& address) {
  impl_->services.Post([impl = impl_, address] {
    uint32_t found = 0;
    {
      std::lock_guard<std::mutex> lock(impl->mutex);
      for (uint32_t id : impl->order) {
        const Impl::TabState& t = impl->tabs.at(id);
        if (t.index >= 0 && t.index < static_cast<int>(t.entries.size()) && t.entries[static_cast<size_t>(t.index)] == address) found = id;
      }
    }
    if (found) impl->Activate(found);
    else impl->NewTab(address, true);
  });
}

void Session::CloseTab(uint32_t tab) {
  impl_->services.Post([impl = impl_, tab] {
    {
      std::lock_guard<std::mutex> lock(impl->mutex);
      if (!impl->tabs.count(tab)) return;
    }
    impl->CloseTab(tab);
    bool none;
    {
      std::lock_guard<std::mutex> lock(impl->mutex);
      none = impl->tabs.empty();
    }
    // (The last tab closed is the window: nothing is left to show.)
    if (none) impl->events.gone(0, true);
  });
}

void Session::Activate(uint32_t tab) {
  impl_->services.Post([impl = impl_, tab] { impl->Activate(tab); });
}

uint32_t Session::activeTab() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->active;
}

void Session::MoveTab(uint32_t tab, size_t index) {
  impl_->services.Post([impl = impl_, tab, index] {
    {
      std::lock_guard<std::mutex> lock(impl->mutex);
      const auto where = std::find(impl->order.begin(), impl->order.end(), tab);
      if (where == impl->order.end()) return;
      const auto found = impl->tabs.find(tab);
      const bool pinned = found != impl->tabs.end() && found->second.pinned;
      size_t pins = 0;
      for (uint32_t other : impl->order) {
        const auto o = impl->tabs.find(other);
        if (other != tab && o != impl->tabs.end() && o->second.pinned) ++pins;
      }
      impl->order.erase(where);
      // (The pinned tabs are the first ones: a tab stays on its own side of them.)
      const size_t place = pinned ? std::min(index, pins) : std::clamp(index, pins, impl->order.size());
      impl->order.insert(impl->order.begin() + static_cast<std::ptrdiff_t>(std::min(place, impl->order.size())), tab);
    }
    impl->PushState();
  });
}

void Session::PinTab(uint32_t tab, bool pinned) {
  impl_->services.Post([impl = impl_, tab, pinned] {
    {
      std::lock_guard<std::mutex> lock(impl->mutex);
      const auto found = impl->tabs.find(tab);
      const auto where = std::find(impl->order.begin(), impl->order.end(), tab);
      if (found == impl->tabs.end() || where == impl->order.end() || found->second.pinned == pinned) return;
      found->second.pinned = pinned;
      impl->order.erase(where);
      size_t pins = 0;
      for (uint32_t other : impl->order) {
        const auto o = impl->tabs.find(other);
        if (o != impl->tabs.end() && o->second.pinned) ++pins;
      }
      // (A tab that is pinned goes after the pinned ones; one that is not goes first among the others.)
      impl->order.insert(impl->order.begin() + static_cast<std::ptrdiff_t>(pins), tab);
    }
    impl->PushState();
  });
}

void Session::CycleTab(int delta) {
  impl_->services.Post([impl = impl_, delta] {
    uint32_t next = 0;
    {
      std::lock_guard<std::mutex> lock(impl->mutex);
      if (impl->order.size() < 2) return;
      const auto where = std::find(impl->order.begin(), impl->order.end(), impl->active);
      const int count = static_cast<int>(impl->order.size());
      const int at = where == impl->order.end() ? 0 : static_cast<int>(where - impl->order.begin());
      next = impl->order[static_cast<size_t>(((at + delta) % count + count) % count)];
    }
    impl->Activate(next);
  });
}

void Session::Go(const std::string& address) {
  impl_->services.Post([impl = impl_, address] {
    uint32_t tab;
    {
      std::lock_guard<std::mutex> lock(impl->mutex);
      tab = impl->active;
    }
    impl->GoNew(tab, address);
  });
}

void Session::Back() {
  impl_->services.Post([impl = impl_] {
    uint32_t tab;
    int target;
    {
      std::lock_guard<std::mutex> lock(impl->mutex);
      const Impl::TabState* t = impl->ActiveTab();
      if (!t) return;
      tab = t->id;
      target = t->index - 1;
    }
    impl->GoToEntry(tab, target);
  });
}

void Session::Forward() {
  impl_->services.Post([impl = impl_] {
    uint32_t tab;
    int target;
    {
      std::lock_guard<std::mutex> lock(impl->mutex);
      const Impl::TabState* t = impl->ActiveTab();
      if (!t) return;
      tab = t->id;
      target = t->index + 1;
    }
    impl->GoToEntry(tab, target);
  });
}

void Session::Reload() {
  impl_->services.Post([impl = impl_] {
    uint32_t tab;
    int target;
    {
      std::lock_guard<std::mutex> lock(impl->mutex);
      const Impl::TabState* t = impl->ActiveTab();
      if (!t) return;
      tab = t->id;
      target = t->index;
    }
    impl->GoToEntry(tab, target);
  });
}

void Session::OpenChrome(const std::string& geometry, const std::string& address) {
  if (!IsInterfaceAddress(address)) return;
  impl_->services.Post([impl = impl_, geometry, address] { impl->StartChrome(geometry, address); });
}

uint32_t Session::chrome() const { return impl_->chromeId.load(); }

bool Session::IsSettings(uint32_t host) const {
  std::shared_ptr<Host> found = impl_->Find(host);
  return found && !found->gone && found->site == std::string("solar:") + kSettingsAddress;
}

void Session::SendChrome(wire::ToRenderer type, const std::string& bytes) { Send(impl_->chromeId.load(), type, bytes); }

void Session::SetTitle(uint32_t host, const std::string& text) {
  impl_->services.Post([impl = impl_, host, text] {
    {
      std::lock_guard<std::mutex> lock(impl->mutex);
      for (auto& [id, t] : impl->tabs) {
        if (t.current == host) t.title = text;
      }
    }
    impl->PushState();
  });
}

void Session::SetWindowFlags(bool controls, bool maximized) {
  impl_->services.Post([impl = impl_, controls, maximized] {
    {
      std::lock_guard<std::mutex> lock(impl->mutex);
      impl->windowControls = controls;
      impl->maximized = maximized;
    }
    impl->PushState();
  });
}

void Session::PushState() { impl_->services.Post([impl = impl_] { impl->PushState(); }); }

void Session::ChooseDownloadPath(uint32_t id, const std::string& path) {
  impl_->services.Post([impl = impl_, id, path] {
    bool cancel = path.empty();
    uint64_t serial = 0;
    bool finished = false;
    {
      std::lock_guard<std::mutex> lock(impl->mutex);
      for (Impl::Download& d : impl->downloads) {
        if (d.id != id || !d.asking) continue;
        if (cancel) {
          d.state = Impl::Download::State::Cancelled;
          serial = d.serial;
          std::error_code error;
          std::filesystem::remove(d.path, error);
          d.asking = false;
        } else {
          d.chosen = path;
          d.name = std::filesystem::path(path).filename().string();
          finished = d.state == Impl::Download::State::Done;
        }
      }
    }
    if (cancel && serial) {
      const auto found = impl->navigations.find(serial);
      if (found != impl->navigations.end()) {
        found->second->handle.Cancel();
        found->second->file.close();
        found->second->downloadId = 0;
        impl->navigations.erase(found);
      }
    }
    if (finished) impl->MoveChosen(id);
    impl->PushState();
  });
}

bool Session::IsDownloadsPage(uint32_t host) const {
  std::shared_ptr<Host> found = impl_->Find(host);
  return found && !found->gone && found->site == std::string("solar:") + kDownloadsAddress;
}

std::string Session::DownloadPath(uint32_t id) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  for (const Impl::Download& d : impl_->downloads) {
    if (d.id == id && d.state == Impl::Download::State::Done && !d.asking) return d.path;
  }
  return "";
}

void Session::CancelDownload(uint32_t id) {
  impl_->services.Post([impl = impl_, id] {
    uint64_t serial = 0;
    {
      std::lock_guard<std::mutex> lock(impl->mutex);
      for (Impl::Download& d : impl->downloads) {
        if (d.id == id && d.state == Impl::Download::State::Running) {
          d.state = Impl::Download::State::Cancelled;
          serial = d.serial;
        }
      }
    }
    const auto found = impl->navigations.find(serial);
    if (serial && found != impl->navigations.end()) {
      found->second->handle.Cancel();
      found->second->file.close();
      found->second->downloadId = 0;
      impl->navigations.erase(found);
    }
    {
      std::lock_guard<std::mutex> lock(impl->mutex);
      for (Impl::Download& d : impl->downloads) {
        if (d.id == id && d.asking && d.state == Impl::Download::State::Cancelled) {
          std::error_code error;
          std::filesystem::remove(d.path, error);
          d.asking = false;
        }
      }
    }
    impl->PushState();
  });
}

void Session::ClearDownloads() {
  impl_->services.Post([impl = impl_] {
    {
      std::lock_guard<std::mutex> lock(impl->mutex);
      impl->downloads.erase(std::remove_if(impl->downloads.begin(), impl->downloads.end(), [](const Impl::Download& d) { return d.state != Impl::Download::State::Running; }), impl->downloads.end());
    }
    impl->PushState();
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
    for (auto& [id, host] : impl_->hosts) if (!host->chrome) all.push_back(host);
  }
  for (auto& host : all) {
    if (type != wire::ToRenderer::Resize) {
      impl_->SendTo(*host, type, bytes);
      continue;
    }
    double zoom = 1.0;
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      const auto found = impl_->tabs.find(host->tab);
      if (found != impl_->tabs.end()) zoom = found->second.zoom;
    }
    impl_->SendTo(*host, type, ZoomedGeometry(bytes, zoom));
  }
}

void Session::Zoom(int step) {
  impl_->services.Post([impl = impl_, step] { impl->ChangeZoom(step); });
}

double Session::ActiveZoom() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->tabs.find(impl_->active);
  return found == impl_->tabs.end() ? 1.0 : found->second.zoom;
}

bool Session::Handle(uint32_t hostId, uint8_t type, const std::string& bytes) {
  std::shared_ptr<Host> host = impl_->Find(hostId);
  if (!host) return type == static_cast<uint8_t>(wire::ToBrowser::Navigate) || type == static_cast<uint8_t>(wire::ToBrowser::NavCommitted) || type == static_cast<uint8_t>(wire::ToBrowser::HistoryGo) ||
                    type == static_cast<uint8_t>(wire::ToBrowser::Storage);
  net::ipc::Reader in(bytes);
  if (host->chrome) {
    // The interface's process may navigate to its document, and commits to it; the rest is for the window (a command) or nothing.
    switch (static_cast<wire::ToBrowser>(type)) {
      case wire::ToBrowser::Navigate: {
        wire::Navigate request;
        if (wire::Read(in, request)) impl_->services.Post([impl = impl_, host, request = std::move(request)] { impl->OnChromeNavigate(host, request); });
        return true;
      }
      case wire::ToBrowser::NavCommitted:
      case wire::ToBrowser::HistoryGo:
      case wire::ToBrowser::Storage: return true;
      default: return false;
    }
  }
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
      if (wire::Read(in, request)) host->tab_->Storage(std::move(request));
      return true;
    }
    case wire::ToBrowser::Favicon: {
      const std::string address = bytes.substr(0, 4096);
      impl_->services.Post([impl = impl_, host, address] { impl->OnFavicon(host, address); });
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
  const auto found = impl_->tabs.find(impl_->active);
  return found == impl_->tabs.end() ? std::vector<std::string>() : found->second.entries;
}

int Session::Index() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->tabs.find(impl_->active);
  return found == impl_->tabs.end() ? -1 : found->second.index;
}

}  // namespace solar::gui
