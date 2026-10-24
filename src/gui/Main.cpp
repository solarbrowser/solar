// solar: a page in a window. The page is loaded as the tests load one (a realm with every API of a window, a document, its scripts and style
// sheets fetched as it asks), laid out for the size of the window, painted as a display list and drawn by Skia into a texture SDL shows.
#include <SDL3/SDL.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"
#include "include/core/SkStream.h"
#include "include/encode/SkPngEncoder.h"
#include "quanta/Embed.h"
#include "solar/css/CssBindings.h"
#include "solar/css/MediaQuery.h"
#include "solar/css/Style.h"
#include "solar/dom/Damage.h"
#include "solar/dom/NodeBindings.h"
#include "solar/dom/NodeBindingsInternal.h"
#include "solar/font/Database.h"
#include "solar/html/Csp.h"
#include "solar/html/Errors.h"
#include "solar/html/Frames.h"
#include "solar/html/HtmlBindings.h"
#include "solar/html/Modules.h"
#include "solar/html/Parser.h"
#include "solar/layout/Layout.h"
#include "solar/net/HttpClient.h"
#include "solar/paint/Paint.h"
#include "solar/paint/SkiaPainter.h"
#include "solar/url/Normalizer.h"
#include "solar/url/Parser.h"
#include "solar/url/Serializer.h"
#include "solar/web/Console.h"
#include "solar/web/DomBindings.h"
#include "solar/web/FetchBindings.h"
#include "solar/web/FetchHost.h"
#include "solar/web/JsEventLoop.h"
#include "solar/web/Messaging.h"
#include "solar/web/TimerHost.h"
#include "solar/web/UrlBindings.h"

namespace {

namespace qe = Quanta::Embed;
using Clock = std::chrono::steady_clock;

bool ReadFile(const std::string& path, std::string& out) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  std::stringstream buffer;
  buffer << file.rdbuf();
  out = buffer.str();
  return true;
}

// What a fetch brought, collected: the page waits for it.
class Collector : public solar::net::FetchHandler {
 public:
  void OnResponseHead(const solar::net::HttpResponseHead& head, const solar::url::Url& finalUrl) override {
    status = head.status;
    if (auto type = head.Header("content-type")) contentType = std::string(*type);
    address = solar::url::Serialize(finalUrl);
  }
  void OnBody(std::span<const uint8_t> data) override { body.append(reinterpret_cast<const char*>(data.data()), data.size()); }
  void OnEnd() override { done = true; }
  void OnError(std::string_view message) override {
    failed = true;
    done = true;
    error = std::string(message);
  }
  int status = 0;
  bool failed = false, done = false;
  std::string body, contentType, address, error;
};

// A file: address is a file of this machine; the rest are the network's.
std::string FileOfAddress(const std::string& address) {
  std::string path = address.substr(7);  // file://
  const size_t cut = path.find_first_of("?#");
  if (cut != std::string::npos) path.resize(cut);
  std::string decoded;
  for (size_t i = 0; i < path.size(); ++i) {
    if (path[i] == '%' && i + 2 < path.size() && std::isxdigit(static_cast<unsigned char>(path[i + 1])) && std::isxdigit(static_cast<unsigned char>(path[i + 2]))) {
      decoded += static_cast<char>(std::stoi(path.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else {
      decoded += path[i];
    }
  }
  return decoded;
}

struct RealmBundle {
  std::unique_ptr<qe::Realm> realm;
  std::unique_ptr<solar::web::FetchHost> host;
  ~RealmBundle() {
    host.reset();
    solar::html::ForgetRealm(realm.get());
    realm.reset();
  }
};

// What the pages get from the program: realms of one isolate, and the resources of the network and of the disk.
class Browser : public solar::html::FrameEnvironment {
 public:
  Browser(qe::Isolate& isolate, solar::net::Loop& loop, solar::net::HttpClient& client, solar::web::JsEventLoop& events)
      : isolate_(isolate), loop_(loop), client_(client), events_(events) {}

  // A page that is left: its realm goes, and with it what only it kept.
  void ReleaseRealm(qe::Realm* realm) {
    // (What the realm's own data holds is not let go with the realm: the document it points at is let go first.)
    solar::html::RunInRealm(*realm, [&] { solar::dom::SetAssociatedDocument(realm->GetContext(), nullptr); });
    for (auto it = bundles_.begin(); it != bundles_.end(); ++it) {
      if ((*it)->realm.get() == realm) {
        bundles_.erase(it);
        break;
      }
    }
  }

  qe::Realm* CreateRealm() override {
    auto bundle = std::make_unique<RealmBundle>();
    qe::Isolate::RealmOptions options;
    options.installConsole = false;
    bundle->realm = isolate_.CreateRealm(options);
    qe::Realm& realm = *bundle->realm;
    solar::html::RunInRealm(realm, [&] {
      solar::web::InstallUrlApis(realm);
      solar::web::InstallConsoleApi(realm, solar::web::DefaultConsoleSink());
      solar::web::InstallDomApis(realm);
      solar::dom::InstallNodeApis(realm);
      solar::html::InstallHtmlApis(realm);
      solar::css::InstallSelectorApis(realm);
      solar::web::InstallFetchApis(realm);
      solar::web::FetchHost::Config config;
      config.loop = &loop_;
      config.client = &client_;
      config.pageUrl = *solar::url::Parse(pageUrl_.empty() ? "about:blank" : pageUrl_);
      config.afterScript = [this] { events_.AfterScript(); };
      bundle->host = std::make_unique<solar::web::FetchHost>(realm.GetContext(), config);
    });
    bundles_.push_back(std::move(bundle));
    return &realm;
  }

  // A resource in flight or here: what the network brought for an address, kept for as long as the program runs (pages share their scripts, styles and pictures).
  struct Resource : solar::net::FetchHandler {
    Browser* owner = nullptr;
    std::string url;
    int status = 0;
    bool done = false, failed = false;
    std::string body;
    Clock::time_point started = Clock::now();
    void OnResponseHead(const solar::net::HttpResponseHead& head, const solar::url::Url&) override { status = head.status; }
    void OnBody(std::span<const uint8_t> data) override { body.append(reinterpret_cast<const char*>(data.data()), data.size()); }
    void OnEnd() override { Finish(); }
    void OnError(std::string_view) override {
      failed = true;
      Finish();
    }
    void Finish() {
      done = true;
      if (owner->timing_) std::fprintf(stderr, "  fetch %5.0f ms  %7zu bytes  %s\n", std::chrono::duration<double, std::milli>(Clock::now() - started).count(), body.size(), url.substr(0, 100).c_str());
      if (owner->waiting_ == this) owner->loop_.Stop();
      if (owner->layoutImages_.erase(url)) {
        owner->sizesArrived_ = true;
        owner->arrivedUrls_.insert(url);
      }
      else owner->arrived_ = true;
    }
  };

  // Starts getting the resource at the address, if it is not being got already. Null for what is not for the network.
  Resource* Start(const std::string& url) {
    const auto found = resources_.find(url);
    if (found != resources_.end()) return found->second.get();
    std::optional<solar::url::Url> parsed = solar::url::Parse(url);
    if (!parsed || (parsed->scheme != "http" && parsed->scheme != "https")) return nullptr;
    auto resource = std::make_unique<Resource>();
    resource->owner = this;
    resource->url = url;
    Resource* raw = resource.get();
    resources_[url] = std::move(resource);
    client_.Fetch(*parsed, *raw, solar::net::FetchOptions());
    return raw;
  }

  void WaitFor(Resource* resource) {
    waiting_ = resource;
    const auto waitStart = Clock::now();
    while (!resource->done) {
      events_.Step(std::chrono::milliseconds(std::getenv("SOLAR_SLICE") ? std::atoi(std::getenv("SOLAR_SLICE")) : 8));
      if (pump_) pump_();
    }
    const double thisWait = std::chrono::duration<double, std::milli>(Clock::now() - waitStart).count();
    waited_ += thisWait;
    if (timing_ && thisWait > 300) std::fprintf(stderr, "  waited %.0f ms for %s\n", thisWait, resource->url.substr(0, 110).c_str());
    waiting_ = nullptr;
  }

  std::optional<std::string> Load(const std::string& url) override {
    if (url.starts_with("file://")) {
      std::string contents;
      if (!ReadFile(FileOfAddress(url), contents)) return std::nullopt;
      return contents;
    }
    Resource* resource = Start(url);
    if (!resource) return std::nullopt;
    WaitFor(resource);
    if (resource->failed || resource->status < 200 || resource->status >= 300) {
      std::fprintf(stderr, "  [%d%s] %s\n", resource->status, resource->failed ? " failed" : "", url.c_str());
      return std::nullopt;
    }
    return resource->body;
  }

  // The pictures that layout and painting asked for and that are on their way: they are fetched together, and the page is laid out again when they are here.
  bool RequestImage(const std::string& absolute) {
    if (absolute.starts_with("file://")) return false;
    const auto found = resources_.find(absolute);
    if (found != resources_.end() && found->second->done) return false;  // here (or failed): the loader has it
    if (!Start(absolute)) return false;
    if (!painting_) layoutImages_.insert(absolute);
    return true;
  }
  // Pictures that came since (the pictures are not waited for): the page is laid out again for the ones whose size layout wanted, and drawn again for the rest.
  bool TakeArrived() {
    const bool was = arrived_;
    arrived_ = false;
    return was;
  }
  bool TakeSizesArrived() {
    const bool was = sizesArrived_;
    sizesArrived_ = false;
    return was;
  }
  size_t CachedBytes() const {
    size_t total = 0;
    for (const auto& entry : resources_) total += entry.second->body.size();
    return total;
  }
  // What was fetched is kept for the next page to use; past `limit` bytes it is let go (what is being fetched stays).
  void TrimResources(size_t limit) {
    if (CachedBytes() <= limit) return;
    for (auto it = resources_.begin(); it != resources_.end();) {
      if (it->second->done) it = resources_.erase(it);
      else ++it;
    }
    scripts_.clear();
  }
  size_t ScriptCount() const { return scripts_.size(); }
  bool Loading() const {
    for (const auto& entry : resources_) {
      if (!entry.second->done) return true;
    }
    return false;
  }

  // What the page will ask for as it is parsed, found in the markup before that: stylesheets, scripts and pictures are started at once, so that
  // the network does them together (https://html.spec.whatwg.org/multipage/parsing.html#preload-scanner).
  void Prefetch(const std::string& markup, const std::string& baseAddress) {
    std::optional<solar::url::Url> base = solar::url::Parse(baseAddress);
    if (!base) return;
    std::string lower = markup;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    int started = 0;
    const auto attribute = [&](const std::string& tag, const std::string& tagLower, const char* name) -> std::string {
      size_t at = 0;
      const std::string key = std::string(name);
      while ((at = tagLower.find(key, at)) != std::string::npos) {
        const bool startOk = at > 0 && std::isspace(static_cast<unsigned char>(tagLower[at - 1]));
        size_t e = at + key.size();
        while (e < tagLower.size() && std::isspace(static_cast<unsigned char>(tagLower[e]))) ++e;
        if (startOk && e < tagLower.size() && tagLower[e] == '=') {
          ++e;
          while (e < tagLower.size() && std::isspace(static_cast<unsigned char>(tagLower[e]))) ++e;
          if (e >= tag.size()) return "";
          if (tag[e] == '"' || tag[e] == '\'') {
            const char q = tag[e++];
            const size_t end = tag.find(q, e);
            return tag.substr(e, end == std::string::npos ? std::string::npos : end - e);
          }
          const size_t end = tag.find_first_of(" \t\r\n>", e);
          return tag.substr(e, end == std::string::npos ? std::string::npos : end - e);
        }
        at += key.size();
      }
      return "";
    };
    const auto resolve = [&](std::string value) -> std::string {
      // (&amp; in an address of markup)
      for (size_t i; (i = value.find("&amp;")) != std::string::npos;) value.replace(i, 5, "&");
      std::optional<solar::url::Url> url = solar::url::Parse(value, &*base);
      return url ? solar::url::Serialize(*url) : std::string();
    };
    for (const char* name : {"<link", "<script"}) {  // (pictures wait: what the page needs to be made goes first)
      size_t at = 0;
      const std::string needle = name;
      while ((at = lower.find(needle, at)) != std::string::npos && started < 80) {
        const size_t end = lower.find('>', at);
        if (end == std::string::npos) break;
        const std::string tag = markup.substr(at, end - at + 1), tagLower = lower.substr(at, end - at + 1);
        at = end;
        std::string target;
        if (needle == "<link") {
          const std::string rel = attribute(tag, tagLower, "rel");
          if (rel.find("stylesheet") == std::string::npos && rel.find("preload") == std::string::npos) continue;
          if (rel.find("preload") != std::string::npos) {
            const std::string as = attribute(tag, tagLower, "as");
            if (as != "style" && as != "script" && as != "image") continue;
          }
          target = attribute(tag, tagLower, "href");
        } else if (needle == "<script") {
          target = attribute(tag, tagLower, "src");
        } else {
          target = attribute(tag, tagLower, "src");
          if (target.empty()) {
            std::string set = attribute(tag, tagLower, "srcset");
            const size_t cut = set.find_first_of(" ,");
            target = set.substr(0, cut);
          }
        }
        if (target.empty() || target.starts_with("data:")) continue;
        const std::string absolute = resolve(target);
        if (absolute.empty()) continue;
        if (Start(absolute)) ++started;
      }
    }
  }

  std::string ContentType(const std::string& url) override {
    const auto found = types_.find(url);
    return found != types_.end() ? found->second : solar::html::FrameEnvironment::ContentType(url);
  }

  std::shared_ptr<qe::Script> CompileScript(const std::string& address, const std::string& code) override {
    auto found = scripts_.find(address);
    if (found != scripts_.end() && found->second.first == code) return found->second.second;
    qe::CompileResult result = isolate_.CompileScript(code, address);
    if (!result.script) return nullptr;
    scripts_[address] = {code, result.script};
    return result.script;
  }

  void RunJobs() override { isolate_.PerformMicrotaskCheckpoint(); }
  void ScriptFailed(const std::string& message) override { std::fprintf(stderr, "  script error: %s\n", message.c_str()); }

  void SetPageUrl(const std::string& url) { pageUrl_ = url; }
  void SetContentType(const std::string& url, const std::string& type) { types_[url] = type; }

 private:
  qe::Isolate& isolate_;
  solar::net::Loop& loop_;
  solar::net::HttpClient& client_;
  solar::web::JsEventLoop& events_;
  std::string pageUrl_;
  std::map<std::string, std::string> types_;
  std::map<std::string, std::pair<std::string, std::shared_ptr<qe::Script>>> scripts_;
  std::vector<std::unique_ptr<RealmBundle>> bundles_;
  std::map<std::string, std::unique_ptr<Resource>> resources_;
  Resource* waiting_ = nullptr;
  bool arrived_ = false, sizesArrived_ = false;
  std::set<std::string> layoutImages_;

 public:
  std::set<std::string> arrivedUrls_;

 private:

 public:
  double waited_ = 0;  // ms spent waiting for what the page asked for
  bool painting_ = false;
  bool timing_ = false;
  std::function<void()> pump_;
  // A page asked to go somewhere (script, or a form): the window does it when the page's script has returned.
  struct Requested {
    std::string url, method, body, type;
    bool replace = false;
  };
  std::optional<Requested> requested_;
  void Navigate(solar::dom::Document*, const std::string& url, bool replace, const std::string& method, const std::string& body, const std::string& type) override {
    requested_ = Requested{url, method, body, type, replace};
  }
};

// The address the user typed as one: a path of a file becomes a file: address.
std::string AddressOf(const std::string& given) {
  if (given.find("://") != std::string::npos || given.starts_with("about:") || given.starts_with("data:")) return solar::url::Normalize(given);
  std::error_code error;
  if (std::filesystem::exists(given, error)) return "file://" + std::filesystem::absolute(given).string();
  return solar::url::Normalize("https://" + given);
}

}  // namespace

int main(int argc, char** argv) {
  const auto programStart = Clock::now();
  std::setbuf(stdout, nullptr);
  const std::string given = argc > 1 ? argv[1] : "https://example.com/";
  std::string address = AddressOf(given);

  // The page: an isolate, the network, and a window's worth of APIs in a realm.
  auto isolate = qe::Isolate::Create();
  isolate->SetModuleHooks(solar::html::MakeModuleHooks());
  isolate->SetCodeGenerationHooks(solar::html::csp::MakeCodeGenerationHooks());
  isolate->SetSerializationHooks(solar::web::MakeSerializationHooks());
  isolate->SetUncaughtExceptionHandler(solar::html::MakeUncaughtExceptionHandler());
  isolate->SetPromiseRejectionHandler(solar::html::MakeRejectionHandler());
  if (std::getenv("SOLAR_GUI_POSITIONS")) isolate->SetSourcePositionTracking(true);
  auto loop = solar::net::Loop::Create();
  if (!loop) {
    std::fprintf(stderr, "cannot start the event loop\n");
    return 1;
  }
  solar::net::HttpClient client(*loop);
  solar::web::TimerHost timers;
  isolate->SetTimerProvider(timers.Provider());
  solar::web::JsEventLoop events(*loop, {[&] { isolate->PerformMicrotaskCheckpoint(); }, [&] { timers.RunDue(); }, [&] { return timers.NextDelayMs(); }});
  Browser browser(*isolate, *loop, client, events);
  browser.timing_ = std::getenv("SOLAR_GUI_TIMES") != nullptr;
  solar::layout::SetImageRequestHandler([&](const std::string& absolute) { return browser.RequestImage(absolute); });
  solar::html::SetFrameEnvironment(&browser);

  // The window.
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  SDL_Window* window = SDL_CreateWindow("Solar", 1280, 800, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
  if (renderer) {
    const bool ok = SDL_SetRenderVSync(renderer, 1);
    int on = 0;
    SDL_GetRenderVSync(renderer, &on);
    if (std::getenv("SOLAR_GUI_TIMES")) std::fprintf(stderr, "vsync: asked %s, renderer says %d, driver %s\n", ok ? "ok" : "failed", on, SDL_GetRendererName(renderer));
  }
  if (!renderer) {
    std::fprintf(stderr, "window: %s\n", SDL_GetError());
    return 1;
  }

  // The size the page is laid out for is the window's, in CSS pixels.
  const auto setViewport = [&] {
    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);
    solar::css::MediaEnvironment environment = solar::css::CurrentMediaEnvironment();
    environment.width = environment.deviceWidth = w;
    environment.height = environment.deviceHeight = h;
    environment.resolution = SDL_GetWindowPixelDensity(window);
    solar::css::SetCurrentMediaEnvironment(environment);
  };
  setViewport();
  solar::css::SetOverlayScrollbars(true);
  SDL_StartTextInput(window);

  // Puts the document that came for `where` in a new realm, and runs its scripts (what it fetches is waited for, with the window kept alive).
  solar::paint::SkiaPainter painter;
  qe::Realm* realm = nullptr;
  solar::dom::Document* document = nullptr;
  bool dirty = true;
  solar::dom::Element* pressTarget = nullptr;
  int pressButton = 0;
  const bool timing_log = std::getenv("SOLAR_GUI_TIMES") != nullptr;
  const auto commit = [&](const std::string& where, std::string markup, std::string type, const Clock::time_point& started) {
    qe::Realm* left = realm;
    solar::dom::Document* leftDocument = document;
    pressTarget = nullptr;
    address = where;
    browser.SetPageUrl(address);
    browser.SetContentType(address, type);
    browser.Prefetch(markup, address);
    realm = browser.CreateRealm();
    solar::html::RunInRealm(*realm, [&] {
      document = solar::dom::NewDocument(realm->GetContext(), true);
      document->url = address;
      solar::html::InstallWindow(*realm, document);
    });
    const auto loadStart = Clock::now();
    if (timing_log) std::fprintf(stderr, "[%.0f ms] LoadPage begins\n", std::chrono::duration<double, std::milli>(loadStart.time_since_epoch()).count());
    solar::html::LoadPage(*realm, document, markup, type);
    if (timing_log) std::fprintf(stderr, "[%.0f ms] LoadPage ends\n", std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count());
    if (timing_log) std::fprintf(stderr, "  LoadPage took %.0f ms (%.0f ms of it waiting; scripts %.0f ms, style sheets parsed %.0f ms, layout %.0f ms)\n", std::chrono::duration<double, std::milli>(Clock::now() - loadStart).count(), browser.waited_, solar::html::ScriptMillis(), solar::css::SheetParseMillis(), solar::layout::LayoutMillis());
    isolate->PerformMicrotaskCheckpoint();
    const auto settle = Clock::now() + std::chrono::milliseconds(30);
    while (Clock::now() < settle) events.Step(std::chrono::milliseconds(8));
    browser.TakeArrived();
    browser.TakeSizesArrived();
    if (left) {
      // The page that was left: its timers stop, what hangs from its document is let go of, its realm goes, and what is no more used is collected.
      timers.DropRealm(left);
      if (leftDocument) solar::dom::DisposeDocument(leftDocument);
      if (leftDocument) solar::css::ForgetDocument(leftDocument);
      browser.ReleaseRealm(left);
      solar::layout::ForgetImages();
      painter.Forget();
      browser.TrimResources(48u << 20);
      isolate->CollectGarbage();
    }
    std::fprintf(stderr, "loaded in %.0f ms\n", std::chrono::duration<double, std::milli>(Clock::now() - started).count());
    SDL_SetWindowTitle(window, ("Solar - " + address).c_str());
    dirty = true;
  };

  // The document being fetched: the window goes on with the page it has until the new one is here.
  struct Navigation {
    std::string where;
    Collector collector;
    Clock::time_point started = Clock::now();
  };
  std::vector<std::unique_ptr<Navigation>> navigations;  // (a fetch cannot be taken back: the ones that were left are kept until they end)
  Navigation* current = nullptr;
  const auto navigate = [&](const std::string& where, const std::string& method = "GET", const std::string& body = "", const std::string& bodyType = "") -> bool {
    std::fprintf(stderr, "loading %s\n", where.c_str());
    if (where.starts_with("file://")) {
      std::string markup;
      if (!ReadFile(FileOfAddress(where), markup)) {
        std::fprintf(stderr, "cannot read %s\n", where.c_str());
        return false;
      }
      commit(where, std::move(markup), "text/html", Clock::now());
      return true;
    }
    std::optional<solar::url::Url> parsed = solar::url::Parse(where);
    if (!parsed) {
      std::fprintf(stderr, "invalid address: %s\n", where.c_str());
      return false;
    }
    auto navigation = std::make_unique<Navigation>();
    navigation->where = where;
    solar::net::FetchOptions options;
    if (method == "POST") {
      options.method = "POST";
      options.body = std::make_shared<const std::string>(body);
      if (!bodyType.empty()) options.headers.emplace_back("Content-Type", bodyType);
    }
    client.Fetch(*parsed, navigation->collector, options);
    current = navigation.get();
    navigations.push_back(std::move(navigation));
    SDL_SetWindowTitle(window, ("Loading " + where).c_str());
    return true;
  };
  // What the window does while a page is waited for: it keeps answering (to be closed, to be resized).
  bool quit = false;
  browser.pump_ = [&] {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_EVENT_QUIT) quit = true;
      if (event.type == SDL_EVENT_WINDOW_RESIZED || event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) setViewport();
    }
    if (quit) {
      SDL_Quit();
      std::_Exit(0);
    }
  };
  // The document that came: it is put in a realm, unless a newer address was asked for since.
  const auto settleNavigation = [&] {
    if (!current || !current->collector.done) return;
    Navigation* arrived = current;
    current = nullptr;
    Collector& collector = arrived->collector;
    if (collector.failed) {
      std::fprintf(stderr, "fetch failed: %s\n", collector.error.c_str());
      SDL_SetWindowTitle(window, ("Solar - " + address).c_str());
      return;
    }
    if (std::getenv("SOLAR_GUI_TIMES")) std::fprintf(stderr, "  document fetched at %.0f ms (%zu bytes)\n", std::chrono::duration<double, std::milli>(Clock::now() - arrived->started).count(), collector.body.size());
    const std::string type = collector.contentType.find("xml") != std::string::npos ? "application/xhtml+xml" : "text/html";
    commit(collector.address.empty() ? arrived->where : collector.address, std::move(collector.body), type, arrived->started);
  };
  if (!navigate(address)) return 1;
  // (The first page is waited for, as a program that starts with one.)
  while (current && !current->collector.done) {
    events.Step(std::chrono::milliseconds(8));
    browser.pump_();
  }
  settleNavigation();
  if (!document) return 1;

  sk_sp<SkSurface> surface;
  SDL_Texture* texture = nullptr;
  int surfaceW = 0, surfaceH = 0;
  Clock::time_point lastRelayout = Clock::now();
  uint64_t lastVersion = 0;
  const bool timing = std::getenv("SOLAR_GUI_TIMES") != nullptr;

  const auto currentTree = [&]() -> solar::layout::Tree* { return solar::layout::UpdateLayout(realm->GetContext(), document); };
  // The scrollbar of the window floats over the page: it shows while the page is scrolled (and under the pointer), then fades; it can be dragged.
  Clock::time_point scrollActive = Clock::now() - std::chrono::seconds(10);
  bool barDrag = false, barHover = false;
  double barGrab = 0;
  struct Bar {
    double top = 0, height = 0, track = 0, maxY = 0;
  };
  const auto barOf = [&](Bar& bar) -> bool {
    solar::layout::Tree* tree = currentTree();
    if (!tree) return false;
    double maxX = 0, maxY = 0;
    solar::layout::ScrollRange(*tree, nullptr, maxX, maxY);
    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);
    if (maxY <= 0.5 || h <= 0) return false;
    bar.track = h;
    bar.height = std::max(32.0, h * h / (h + maxY));
    bar.maxY = maxY;
    bar.top = tree->scroll[document].second / maxY * (h - bar.height);
    return true;
  };
  // A link: the address it has, against the page's (the nearest one at or above `from`).
  const auto followLink = [&](solar::dom::Element* from) {
    for (solar::dom::Node* n = from; n; n = n->parentNode) {
      if (!n->IsElement()) continue;
      solar::dom::Element* element = static_cast<solar::dom::Element*>(n);
      if (!element->IsHtml("a")) continue;
      if (const solar::dom::Attr* href = element->FindAttribute("", "href")) {
        std::optional<solar::url::Url> base = solar::url::Parse(address);
        std::optional<solar::url::Url> target = base ? solar::url::Parse(href->value, &*base) : solar::url::Parse(href->value);
        if (target && (target->scheme == "http" || target->scheme == "https" || target->scheme == "file")) navigate(solar::url::Serialize(*target));
      }
      break;
    }
  };
  // The text control that has the focus, if one has.
  const auto focusedTextControl = [&]() -> solar::dom::Element* {
    solar::dom::Element* focused = document ? document->focusedElement : nullptr;
    if (!focused) return nullptr;
    if (focused->IsHtml("textarea")) return focused;
    if (!focused->IsHtml("input")) return nullptr;
    const solar::dom::Attr* type = focused->FindAttribute("", "type");
    std::string t = type ? type->value : "";
    for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const char* const kText[] = {"", "text", "search", "tel", "url", "email", "password", "number"};
    for (const char* k : kText) if (t == k) return focused;
    return nullptr;
  };
  // What is under the pointer is what is hovered.
  double pointerX = 0, pointerY = 0;
  bool pointerKnown = false, hoverStale = false;
  Clock::time_point lastScrollInput = Clock::now() - std::chrono::seconds(10);
  const auto updateHover = [&] {
    hoverStale = false;
    if (!pointerKnown) return;
    solar::layout::Tree* tree = currentTree();
    if (!tree) return;
    const std::vector<solar::dom::Element*> elements = solar::layout::ElementsAtPoint(*tree, pointerX, pointerY);
    solar::dom::SetHoverElement(document, elements.empty() ? nullptr : elements.front());
  };
  // Scrolling goes toward where it was asked to, a part of the way each frame (it eases out), so that a wheel step is a glide and not a jump.
  double targetX = 0, targetY = 0;
  bool gliding = false;
  const solar::dom::Document* glidingIn = nullptr;
  Clock::time_point lastGlide = Clock::now();
  const bool scrollTestMode = std::getenv("SOLAR_GUI_SCROLL") != nullptr;
  const auto scrollBy = [&](double dx, double dy) {
    solar::layout::Tree* tree = currentTree();
    if (!tree) return;
    double maxX = 0, maxY = 0;
    solar::layout::ScrollRange(*tree, nullptr, maxX, maxY);
    auto& at = tree->scroll[document];
    if (scrollTestMode) {
      at.first = std::clamp(at.first + dx, 0.0, maxX);
      at.second = std::clamp(at.second + dy, 0.0, maxY);
      solar::layout::ApplySticky(*tree);
      dirty = true;
      scrollActive = Clock::now();
      return;
    }
    if (!gliding || glidingIn != document) {
      targetX = at.first;
      targetY = at.second;
      glidingIn = document;
    }
    targetX = std::clamp(targetX + dx, 0.0, maxX);
    targetY = std::clamp(targetY + dy, 0.0, maxY);
    if (!gliding) lastGlide = Clock::now();
    gliding = true;
    scrollActive = Clock::now();
    lastScrollInput = scrollActive;
    hoverStale = pointerKnown;
  };
  const auto glide = [&] {
    if (!gliding) return;
    solar::layout::Tree* tree = currentTree();
    if (!tree || glidingIn != document) {
      gliding = false;
      return;
    }
    const auto now = Clock::now();
    const double dt = std::chrono::duration<double>(now - lastGlide).count();
    lastGlide = now;
    const double k = 1 - std::exp(-dt / 0.07);
    auto& at = tree->scroll[document];
    at.first += (targetX - at.first) * k;
    at.second += (targetY - at.second) * k;
    if (std::abs(targetX - at.first) < 0.5 && std::abs(targetY - at.second) < 0.5) {
      at.first = targetX;
      at.second = targetY;
      gliding = false;
    }
    solar::layout::ApplySticky(*tree);
    dirty = true;
  };

  int autoScroll = std::getenv("SOLAR_GUI_SCROLL") ? std::atoi(std::getenv("SOLAR_GUI_SCROLL")) : 0;
  // (SOLAR_GUI_TOUR="address address ...": each is gone to in turn, a few seconds apart, and the memory in use is printed, for looking for what is kept.)
  std::vector<std::string> tour;
  if (const char* list = std::getenv("SOLAR_GUI_TOUR")) {
    std::istringstream words(list);
    for (std::string w; words >> w;) tour.push_back(w);
  }
  size_t tourAt = 0;
  Clock::time_point tourLast = Clock::now();
  const auto anonymousKb = [] {
    std::ifstream status("/proc/self/status");
    for (std::string line; std::getline(status, line);) {
      if (line.rfind("RssAnon:", 0) == 0) return std::atol(line.c_str() + 8);
    }
    return 0L;
  };
  // (SOLAR_GUI_SCRIPT="c:x,y;t:text;k:backspace;k:enter": what a user would do, one step a frame, before SOLAR_GUI_SHOT takes its picture.)
  std::vector<std::string> script;
  if (const char* given = std::getenv("SOLAR_GUI_SCRIPT")) {
    std::string all = given;
    for (size_t at = 0; at < all.size();) {
      size_t end = all.find(';', at);
      if (end == std::string::npos) end = all.size();
      script.push_back(all.substr(at, end - at));
      at = end + 1;
    }
    std::reverse(script.begin(), script.end());
  }
  int scriptQuiet = 0;
  int wheelLeft = std::getenv("SOLAR_GUI_WHEEL") ? std::atoi(std::getenv("SOLAR_GUI_WHEEL")) : 0;
  Clock::time_point lastWheel = Clock::now(), wheelDone;
  const bool scrollTest = autoScroll > 0;
  bool running = true;
  double barMs = 0, stepMs = 0, glideMs = 0;
  while (running) {
    const auto iterStart = Clock::now();
    barMs = stepMs = glideMs = 0;
    SDL_Event event;
    bool any = SDL_WaitEventTimeout(&event, dirty ? 0 : 8);
    while (any) {
      switch (event.type) {
        case SDL_EVENT_QUIT: running = false; break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
          setViewport();
          dirty = true;
          break;
        case SDL_EVENT_MOUSE_WHEEL: scrollBy(-event.wheel.x * 60, -event.wheel.y * 60); break;
        case SDL_EVENT_TEXT_INPUT: {
          if (solar::dom::Element* field = focusedTextControl()) {
            solar::html::RunInRealm(*realm, [&] { solar::html::TypeInto(document, field, event.text.text, "text"); });
            dirty = true;
          }
          break;
        }
        case SDL_EVENT_KEY_DOWN: {
          int w = 0, h = 0;
          SDL_GetWindowSize(window, &w, &h);
          if (solar::dom::Element* field = focusedTextControl()) {
            const char* kind = nullptr;
            switch (event.key.key) {
              case SDLK_BACKSPACE: kind = "backspace"; break;
              case SDLK_DELETE: kind = "delete"; break;
              case SDLK_LEFT: kind = "left"; break;
              case SDLK_RIGHT: kind = "right"; break;
              case SDLK_HOME: kind = "home"; break;
              case SDLK_END: kind = "end"; break;
              case SDLK_RETURN: case SDLK_KP_ENTER: kind = "enter"; break;
              default: break;
            }
            if (kind) {
              solar::html::RunInRealm(*realm, [&] { solar::html::TypeInto(document, field, "", kind); });
              dirty = true;
              break;
            }
            if (event.key.key != SDLK_ESCAPE && event.key.key != SDLK_PAGEDOWN && event.key.key != SDLK_PAGEUP) break;
          } else if (solar::dom::Element* focused = document ? document->focusedElement : nullptr;
                     focused && (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER || event.key.key == SDLK_SPACE) &&
                     (focused->IsHtml("button") || focused->IsHtml("input") || focused->IsHtml("a") || focused->IsHtml("summary") || focused->IsHtml("select"))) {
            // Enter presses a link, a button or a box; space the last two.
            if (focused->IsHtml("a") && event.key.key == SDLK_SPACE) {}
            else {
              bool follow = true;
              solar::html::RunInRealm(*realm, [&] { follow = solar::html::DispatchClick(document, focused, 0, 0, 0); });
              if (follow) followLink(focused);
              dirty = true;
              break;
            }
          }
          switch (event.key.key) {
            case SDLK_ESCAPE: running = false; break;
            case SDLK_DOWN: scrollBy(0, 60); break;
            case SDLK_UP: scrollBy(0, -60); break;
            case SDLK_PAGEDOWN: case SDLK_SPACE: scrollBy(0, h * 0.9); break;
            case SDLK_PAGEUP: scrollBy(0, -h * 0.9); break;
            case SDLK_HOME: scrollBy(0, -1e9); break;
            case SDLK_END: scrollBy(0, 1e9); break;
            default: break;
          }
          break;
        }
        case SDL_EVENT_MOUSE_MOTION: {
          int ww = 0, wh = 0;
          SDL_GetWindowSize(window, &ww, &wh);
          if (barDrag) {
            Bar bar;
            solar::layout::Tree* t = currentTree();
            if (t && barOf(bar) && bar.track > bar.height) {
              const double y = std::clamp((event.motion.y - barGrab) / (bar.track - bar.height), 0.0, 1.0) * bar.maxY;
              t->scroll[document].second = y;
              targetY = y;
              gliding = false;
              solar::layout::ApplySticky(*t);
              scrollActive = Clock::now();
              dirty = true;
            }
            break;
          }
          const bool over = event.motion.x >= ww - 16;
          if (over != barHover) {
            barHover = over;
            scrollActive = Clock::now();
            dirty = true;
          }
          pointerX = event.motion.x;
          pointerY = event.motion.y;
          pointerKnown = true;
          // (While the page is scrolling, what is under the pointer is looked at when it stops: a change of hover is a change of style, and a frame is not the time for one.)
          if (gliding || Clock::now() - lastScrollInput < std::chrono::milliseconds(150)) hoverStale = true;
          else updateHover();
          break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
          int ww = 0, wh = 0;
          SDL_GetWindowSize(window, &ww, &wh);
          Bar bar;
          if (event.button.button == SDL_BUTTON_LEFT && event.button.x >= ww - 16 && barOf(bar)) {
            if (event.button.y >= bar.top && event.button.y <= bar.top + bar.height) {
              barDrag = true;
              barGrab = event.button.y - bar.top;
            } else {
              scrollBy(0, event.button.y < bar.top ? -wh * 0.9 : wh * 0.9);
            }
            dirty = true;
            break;
          }
          if (hoverStale) updateHover();
          pointerX = event.button.x;
          pointerY = event.button.y;
          pointerKnown = true;
          updateHover();
          pressTarget = document->hoverElement;
          pressButton = event.button.button == SDL_BUTTON_LEFT ? 0 : event.button.button == SDL_BUTTON_MIDDLE ? 1 : 2;
          if (pressTarget) {
            bool proceed = true;
            solar::html::RunInRealm(*realm, [&] { proceed = solar::html::DispatchPointer(document, pressTarget, "mousedown", event.button.x, event.button.y, pressButton, 1 << pressButton); });
            // (A script may have taken the element away.)
            if (proceed && event.button.button == SDL_BUTTON_LEFT && document->hoverElement) solar::dom::SetPressedElement(document, document->hoverElement);
            // A press in a text field puts the caret between the letters nearest to it.
            if (proceed && event.button.button == SDL_BUTTON_LEFT && pressTarget == focusedTextControl() && pressTarget) {
              solar::layout::Tree* tree = currentTree();
              const auto boxes = tree ? tree->boxesOf.find(pressTarget) : decltype(tree->boxesOf.end()){};
              if (tree && boxes != tree->boxesOf.end() && !boxes->second.empty() && !pressTarget->IsHtml("textarea")) {
                const solar::layout::Box& box = *boxes->second.front();
                solar::html::ControlView view;
                const auto face = solar::layout::FaceOf(realm->GetContext(), *tree, box);
                bool read = false;
                solar::html::RunInRealm(*realm, [&] { read = solar::html::ReadControl(pressTarget, view); });
                if (read && face) {
                  const solar::layout::Rect where = solar::layout::AbsoluteBorderBox(box);
                  const double left = where.x - tree->scroll[document].first + box.ContentLeft();
                  const double want = event.button.x - left;
                  size_t units = 0, best = 0;
                  double bestDistance = std::fabs(want), pen = 0;
                  for (size_t i = 0; i < view.value.size();) {
                    const unsigned char c = static_cast<unsigned char>(view.value[i]);
                    const size_t length = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
                    i += length;
                    units += length == 4 ? 2 : 1;
                    pen = face->Measure(view.value.substr(0, i)) * box.style->fontSize;
                    if (std::fabs(want - pen) < bestDistance) { bestDistance = std::fabs(want - pen); best = units; }
                  }
                  solar::html::RunInRealm(*realm, [&] { solar::html::SetCaret(document, pressTarget, best, best); });
                }
              }
            }
            dirty = true;
          }
          break;
        }
        case SDL_EVENT_MOUSE_BUTTON_UP: {
          if (barDrag) {
            barDrag = false;
            dirty = true;
            break;
          }
          solar::dom::Element* pressed = document->pressedElement;
          solar::dom::SetPressedElement(document, nullptr);
          // The release goes to what is under the pointer; a click is the press and the release on the same element (or one inside the other).
          pointerX = event.button.x;
          pointerY = event.button.y;
          pointerKnown = true;
          updateHover();
          solar::dom::Element* released = document->hoverElement;
          solar::dom::Element* clicked = nullptr;
          if (pressTarget && released) {
            if (pressTarget == released) clicked = released;
            else {
              for (solar::dom::Node* n = released; n && !clicked; n = n->parentNode) if (n == pressTarget) clicked = pressTarget;
              for (solar::dom::Node* n = pressTarget; n && !clicked; n = n->parentNode) if (n == released) clicked = released;
            }
          }
          pressTarget = nullptr;
          bool follow = true;
          if (released) {
            solar::html::RunInRealm(*realm, [&] {
              solar::html::DispatchPointer(document, released, "mouseup", event.button.x, event.button.y, pressButton, 0);
              if (clicked) follow = solar::html::DispatchClick(document, clicked, event.button.x, event.button.y, pressButton);
            });
            dirty = true;
          }
          if (event.button.button == SDL_BUTTON_LEFT && clicked && follow) followLink(clicked);
          (void)pressed;
          break;
        }
        default: break;
      }
      any = SDL_PollEvent(&event);
    }
    if (!tour.empty() && !current && Clock::now() - tourLast > std::chrono::milliseconds(3500)) {
            std::fprintf(stderr, "memory: %ld kB in use (%zu pages seen; %zu nodes alive, resources %zu kB)\n", anonymousKb(), tourAt, solar::dom::LiveNodes(), browser.CachedBytes() / 1024);
      tourLast = Clock::now();
      if (tourAt < tour.size()) navigate(AddressOf(tour[tourAt++]));
      else running = false;
    }
    if (!script.empty() && Clock::now() - programStart > std::chrono::milliseconds(400)) {
      const std::string step = script.back();
      script.pop_back();
      scriptQuiet = 0;
      SDL_Event e{};
      if (step.rfind("c:", 0) == 0 || step.rfind("m:", 0) == 0 || step.rfind("p:", 0) == 0 || step.rfind("u:", 0) == 0) {
        float x = 0, y = 0;
        std::sscanf(step.c_str() + 2, "%f,%f", &x, &y);
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.x = x;
        e.motion.y = y;
        SDL_PushEvent(&e);
        if (step[0] == 'u') {
          SDL_Event up{};
          up.type = SDL_EVENT_MOUSE_BUTTON_UP;
          up.button.button = SDL_BUTTON_LEFT;
          up.button.x = x;
          up.button.y = y;
          SDL_PushEvent(&up);
        }
        if (step[0] == 'c' || step[0] == 'p') {
          SDL_Event down{};
          down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
          down.button.button = SDL_BUTTON_LEFT;
          down.button.x = x;
          down.button.y = y;
          SDL_PushEvent(&down);
          if (step[0] == 'c') {
            SDL_Event up = down;
            up.type = SDL_EVENT_MOUSE_BUTTON_UP;
            SDL_PushEvent(&up);
          }
        }
      } else if (step.rfind("t:", 0) == 0) {
        e.type = SDL_EVENT_TEXT_INPUT;
        static std::string typed;
        typed = step.substr(2);
        e.text.text = typed.c_str();
        SDL_PushEvent(&e);
      } else if (step.rfind("k:", 0) == 0) {
        e.type = SDL_EVENT_KEY_DOWN;
        const std::string name = step.substr(2);
        e.key.key = name == "backspace" ? SDLK_BACKSPACE : name == "enter" ? SDLK_RETURN : name == "left" ? SDLK_LEFT : name == "right" ? SDLK_RIGHT : name == "delete" ? SDLK_DELETE : name == "home" ? SDLK_HOME : name == "end" ? SDLK_END : name == "space" ? SDLK_SPACE : SDLK_UNKNOWN;
        SDL_PushEvent(&e);
      }
      dirty = true;
    } else if (script.empty()) {
      if (++scriptQuiet == 6) {
        dirty = true;
        // (SOLAR_GUI_EVAL="expression": what it comes to in the page is written out, once the script has been done.)
        if (const char* expression = std::getenv("SOLAR_GUI_EVAL")) {
          solar::html::RunInRealm(*realm, [&] {
            const auto result = realm->Evaluate(std::string("console.log('eval: ' + String(") + expression + "))", "eval");
            if (!result.ok) std::fprintf(stderr, "eval: %s\n", result.error.c_str());
          });
        }
      }
    }
    // (SOLAR_GUI_WHEEL=n: n wheel steps, one every 16 ms, for looking at how scrolling goes.)
    if (wheelLeft > 0 && Clock::now() - programStart > std::chrono::milliseconds(7000) && Clock::now() - lastWheel > std::chrono::milliseconds(16)) {
      --wheelLeft;
      lastWheel = Clock::now();
      SDL_Event wheel{};
      wheel.type = SDL_EVENT_MOUSE_WHEEL;
      wheel.wheel.y = -1;
      SDL_PushEvent(&wheel);
      if (std::getenv("SOLAR_GUI_MOTION")) {
        SDL_Event motion{};
        motion.type = SDL_EVENT_MOUSE_MOTION;
        motion.motion.x = 300 + (wheelLeft % 7) * 20;
        motion.motion.y = 150 + (wheelLeft % 13) * 40;
        SDL_PushEvent(&motion);
      }
      if (wheelLeft == 0) wheelDone = Clock::now();
    }
    if (wheelDone != Clock::time_point() && !gliding && Clock::now() - wheelDone > std::chrono::milliseconds(300)) running = false;
    if (hoverStale && !gliding && Clock::now() - lastScrollInput > std::chrono::milliseconds(150)) updateHover();
    const auto g0 = Clock::now();
    glide();
    glideMs = std::chrono::duration<double, std::milli>(Clock::now() - g0).count();
    const uint64_t beforeStep = solar::dom::DocumentVersion();
    const auto s0 = Clock::now();
    events.Step(std::chrono::milliseconds(4));
    stepMs = std::chrono::duration<double, std::milli>(Clock::now() - s0).count();
    if (timing && solar::dom::DocumentVersion() != beforeStep) std::fprintf(stderr, "step changed the document by %llu\n", (unsigned long long)(solar::dom::DocumentVersion() - beforeStep));
    if (browser.requested_) {
      const Browser::Requested request = std::move(*browser.requested_);
      browser.requested_.reset();
      navigate(request.url, request.method, request.body, request.type);
    }
    settleNavigation();
    // (SOLAR_GUI_SCROLL=n: the page is scrolled n times, a step each frame, for timing it.)
    if (autoScroll > 0 && !dirty && Clock::now() - programStart > std::chrono::milliseconds(std::getenv("SOLAR_GUI_SCROLL_AFTER") ? std::atoi(std::getenv("SOLAR_GUI_SCROLL_AFTER")) : 2500)) {
      --autoScroll;
      scrollBy(0, 120);
    }
    // Pictures that came: the page is laid out again with them (a few at a time, not for each).
    if (Clock::now() - lastRelayout > std::chrono::milliseconds(60)) {
      if (browser.TakeSizesArrived()) {
        if (timing) std::fprintf(stderr, "pictures came: laid out again\n");
        // Only the elements of those pictures are made again (the rest of the page is as it was), when they can be found.
        bool found = false;
        if (solar::layout::Tree* tree = solar::dom::DocumentVersion() ? document->layoutTree.get() : nullptr; tree && tree->root) {
          std::vector<const solar::layout::Box*> stack = {tree->root.get()};
          while (!stack.empty()) {
            const solar::layout::Box* box = stack.back();
            stack.pop_back();
            if (!box->imageUrl.empty() && browser.arrivedUrls_.count(box->imageUrl) && box->element()) {
              solar::dom::NoteDamage(box->element(), solar::dom::kDamageAttributes);
              found = true;
            }
            for (const auto& child : box->children) stack.push_back(child.get());
          }
        }
        browser.arrivedUrls_.clear();
        solar::dom::NoteTreeChange();
        (void)found;
        lastRelayout = Clock::now();
      }
      if (browser.TakeArrived()) dirty = true;
    }

    const uint64_t version = (solar::dom::DocumentVersion() * 1000003 + solar::css::StyleVersion()) * 1000003 + solar::dom::StateVersion();
    if (version != lastVersion) {
      if (timing) std::fprintf(stderr, "version: tree %llu style %llu\n", (unsigned long long)solar::dom::TreeVersion(), (unsigned long long)solar::css::StyleVersion());
      lastVersion = version;
      dirty = true;
    }
    if (!dirty) continue;
    dirty = false;

    const auto t0 = Clock::now();
    solar::layout::Tree* tree = currentTree();
    if (!tree) continue;
    const auto t1 = Clock::now();
    browser.painting_ = true;
    solar::paint::DisplayList list = solar::paint::BuildDisplayList(realm->GetContext(), *tree);
    browser.painting_ = false;
    const auto t2 = Clock::now();

    int pixelW = 0, pixelH = 0;
    SDL_GetWindowSizeInPixels(window, &pixelW, &pixelH);
    if (!surface || surfaceW != pixelW || surfaceH != pixelH) {
      surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(pixelW, pixelH));
      if (texture) SDL_DestroyTexture(texture);
      texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, pixelW, pixelH);
      surfaceW = pixelW;
      surfaceH = pixelH;
    }
    SkCanvas* canvas = surface->getCanvas();
    canvas->resetMatrix();
    canvas->clear(SK_ColorWHITE);
    const float density = SDL_GetWindowPixelDensity(window);
    canvas->scale(density, density);
    painter.Draw(list, *canvas);
    {
      const auto b0 = Clock::now();
      Bar bar;
      const double idle = std::chrono::duration<double>(Clock::now() - scrollActive).count();
      const double alpha = barHover || barDrag ? 1.0 : std::clamp(1.0 - (idle - 1.0) / 0.4, 0.0, 1.0);
      int ww = 0, wh = 0;
      SDL_GetWindowSize(window, &ww, &wh);
      if (alpha > 0 && barOf(bar)) {
        const float width = barHover || barDrag ? 10.f : 6.f;
        SkPaint paint;
        paint.setAntiAlias(true);
        if (barHover || barDrag) {
          paint.setColor(SkColorSetARGB(40, 0, 0, 0));
          canvas->drawRect(SkRect::MakeXYWH(ww - width - 4, 0, width + 4, wh), paint);
        }
        paint.setColor(SkColorSetARGB(static_cast<U8CPU>((barDrag ? 170 : 120) * alpha), 0, 0, 0));
        canvas->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(ww - width - 2, bar.top + 2, width, bar.height - 4), width / 2, width / 2), paint);
        if (alpha < 1.0 || idle < 1.4) dirty = true;
      }
      barMs = std::chrono::duration<double, std::milli>(Clock::now() - b0).count();
    }
    SkPixmap pixmap;
    surface->peekPixels(&pixmap);
    SDL_UpdateTexture(texture, nullptr, pixmap.addr(), static_cast<int>(pixmap.rowBytes()));
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);
    const auto t3 = Clock::now();
    // SOLAR_GUI_SHOT=file.png: the first frame is written to a file, and the program ends (for looking at what it paints without a screen).
    // (SOLAR_GUI_SETTLE=ms: not before the pictures had that long to come.)
    const char* settle = std::getenv("SOLAR_GUI_SETTLE");
    if (autoScroll > 0 || (std::getenv("SOLAR_GUI_SCRIPT") && (!script.empty() || scriptQuiet < 6))) {
    } else if (const char* shot = settle && Clock::now() - programStart < std::chrono::milliseconds(std::atoi(settle)) ? nullptr : std::getenv("SOLAR_GUI_SHOT")) {
      SkDynamicMemoryWStream out;
      if (SkPngEncoder::Encode(&out, pixmap, SkPngEncoder::Options())) {
        sk_sp<SkData> png = out.detachAsData();
        std::ofstream file(shot, std::ios::binary);
        file.write(static_cast<const char*>(png->data()), static_cast<std::streamsize>(png->size()));
      }
      running = false;
    }
    if (timing) {
      const auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
      std::fprintf(stderr, "frame: layout %.1f ms, display list %.1f ms (%zu items), draw %.1f ms; step %.1f, glide %.1f, scrollbar %.1f, iteration %.1f\n", ms(t0, t1), ms(t1, t2), list.items.size(), ms(t2, t3), stepMs, glideMs, barMs, ms(iterStart, Clock::now()));
    }
    if (scrollTest && autoScroll == 0) running = false;
  }

  if (texture) SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  solar::html::SetFrameEnvironment(nullptr);
  return 0;
}
