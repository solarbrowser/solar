// solar-gui: a page in a window. The page is loaded as the tests load one (a realm with every API of a window, a document, its scripts and style
// sheets fetched as it asks), laid out for the size of the window, painted as a display list and drawn by Skia into a texture SDL shows.
#include <SDL3/SDL.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "include/core/SkCanvas.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"
#include "include/core/SkStream.h"
#include "include/encode/SkPngEncoder.h"
#include "quanta/Embed.h"
#include "solar/css/CssBindings.h"
#include "solar/css/MediaQuery.h"
#include "solar/css/Style.h"
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
  void OnEnd() override {}
  void OnError(std::string_view message) override {
    failed = true;
    error = std::string(message);
  }
  int status = 0;
  bool failed = false;
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

  std::optional<std::string> Load(const std::string& url) override {
    if (url.starts_with("file://")) {
      std::string contents;
      if (!ReadFile(FileOfAddress(url), contents)) return std::nullopt;
      return contents;
    }
    std::optional<solar::url::Url> parsed = solar::url::Parse(url);
    if (!parsed || (parsed->scheme != "http" && parsed->scheme != "https")) return std::nullopt;
    Collector collector;
    solar::net::FetchOptions options;
    client_.Fetch(*parsed, collector, options);
    loop_.Run();
    if (collector.failed || collector.status < 200 || collector.status >= 300) {
      std::fprintf(stderr, "  [%d%s] %s\n", collector.status, collector.failed ? " failed" : "", url.c_str());
      return std::nullopt;
    }
    return collector.body;
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
  solar::html::SetFrameEnvironment(&browser);

  // The window.
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  SDL_Window* window = SDL_CreateWindow("Solar", 1100, 760, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
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

  // Loads the page at `where` into a new realm, and runs its scripts.
  qe::Realm* realm = nullptr;
  solar::dom::Document* document = nullptr;
  const auto navigate = [&](const std::string& where) -> bool {
    std::fprintf(stderr, "loading %s\n", where.c_str());
    const auto started = Clock::now();
    std::string markup, type = "text/html";
    if (where.starts_with("file://")) {
      if (!ReadFile(FileOfAddress(where), markup)) {
        std::fprintf(stderr, "cannot read %s\n", where.c_str());
        return false;
      }
    } else {
      std::optional<solar::url::Url> parsed = solar::url::Parse(where);
      if (!parsed) {
        std::fprintf(stderr, "invalid address: %s\n", where.c_str());
        return false;
      }
      Collector collector;
      client.Fetch(*parsed, collector, solar::net::FetchOptions());
      loop->Run();
      if (collector.failed) {
        std::fprintf(stderr, "fetch failed: %s\n", collector.error.c_str());
        return false;
      }
      markup = std::move(collector.body);
      if (!collector.address.empty()) address = collector.address;
      else address = where;
      if (collector.contentType.find("xml") != std::string::npos) type = "application/xhtml+xml";
    }
    if (where.starts_with("file://")) address = where;
    browser.SetPageUrl(address);
    browser.SetContentType(address, type);
    realm = browser.CreateRealm();
    solar::html::RunInRealm(*realm, [&] {
      document = solar::dom::NewDocument(realm->GetContext(), true);
      document->url = address;
      solar::html::InstallWindow(*realm, document);
    });
    solar::html::LoadPage(*realm, document, markup, type);
    isolate->PerformMicrotaskCheckpoint();
    events.Run(std::chrono::milliseconds(1500));
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    std::fprintf(stderr, "loaded in %.0f ms\n", ms);
    SDL_SetWindowTitle(window, ("Solar - " + address).c_str());
    return true;
  };
  if (!navigate(address)) return 1;

  solar::paint::SkiaPainter painter;
  sk_sp<SkSurface> surface;
  SDL_Texture* texture = nullptr;
  int surfaceW = 0, surfaceH = 0;
  bool dirty = true;
  uint64_t lastVersion = 0;
  const bool timing = std::getenv("SOLAR_GUI_TIMES") != nullptr;

  const auto currentTree = [&]() -> solar::layout::Tree* { return solar::layout::UpdateLayout(realm->GetContext(), document); };
  const auto scrollBy = [&](double dx, double dy) {
    solar::layout::Tree* tree = currentTree();
    if (!tree) return;
    double maxX = 0, maxY = 0;
    solar::layout::ScrollRange(*tree, nullptr, maxX, maxY);
    auto& at = tree->scroll[document];
    at.first = std::clamp(at.first + dx, 0.0, maxX);
    at.second = std::clamp(at.second + dy, 0.0, maxY);
    solar::layout::ApplySticky(*tree);
    dirty = true;
  };

  bool running = true;
  while (running) {
    SDL_Event event;
    bool any = SDL_WaitEventTimeout(&event, dirty ? 0 : 16);
    while (any) {
      switch (event.type) {
        case SDL_EVENT_QUIT: running = false; break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
          setViewport();
          dirty = true;
          break;
        case SDL_EVENT_MOUSE_WHEEL: scrollBy(-event.wheel.x * 60, -event.wheel.y * 60); break;
        case SDL_EVENT_KEY_DOWN: {
          int w = 0, h = 0;
          SDL_GetWindowSize(window, &w, &h);
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
          solar::layout::Tree* tree = currentTree();
          if (tree) {
            const std::vector<solar::dom::Element*> elements = solar::layout::ElementsAtPoint(*tree, event.motion.x, event.motion.y);
            solar::dom::SetHoverElement(document, elements.empty() ? nullptr : elements.front());
          }
          break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
          if (event.button.button == SDL_BUTTON_LEFT && document->hoverElement) solar::dom::SetPressedElement(document, document->hoverElement);
          break;
        case SDL_EVENT_MOUSE_BUTTON_UP: {
          solar::dom::Element* pressed = document->pressedElement;
          solar::dom::SetPressedElement(document, nullptr);
          if (event.button.button == SDL_BUTTON_LEFT && pressed) {
            // A link: the address it has, against the page's.
            for (solar::dom::Node* n = pressed; n; n = n->parentNode) {
              if (!n->IsElement()) continue;
              solar::dom::Element* element = static_cast<solar::dom::Element*>(n);
              if (!element->IsHtml("a")) continue;
              if (const solar::dom::Attr* href = element->FindAttribute("", "href")) {
                std::optional<solar::url::Url> base = solar::url::Parse(address);
                std::optional<solar::url::Url> target = base ? solar::url::Parse(href->value, &*base) : solar::url::Parse(href->value);
                if (target && (target->scheme == "http" || target->scheme == "https" || target->scheme == "file")) {
                  const std::string next = solar::url::Serialize(*target);
                  if (navigate(next)) {
                    address = next;
                    dirty = true;
                  }
                }
              }
              break;
            }
          }
          break;
        }
        default: break;
      }
      any = SDL_PollEvent(&event);
    }
    events.Run(std::chrono::milliseconds(1));

    const uint64_t version = solar::dom::TreeVersion() * 1000003 + solar::css::StyleVersion();
    if (version != lastVersion) {
      lastVersion = version;
      dirty = true;
    }
    if (!dirty) continue;
    dirty = false;

    const auto t0 = Clock::now();
    solar::layout::Tree* tree = currentTree();
    if (!tree) continue;
    const auto t1 = Clock::now();
    const solar::paint::DisplayList list = solar::paint::BuildDisplayList(realm->GetContext(), *tree);
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
    SkPixmap pixmap;
    surface->peekPixels(&pixmap);
    SDL_UpdateTexture(texture, nullptr, pixmap.addr(), static_cast<int>(pixmap.rowBytes()));
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);
    const auto t3 = Clock::now();
    // SOLAR_GUI_SHOT=file.png: the first frame is written to a file, and the program ends (for looking at what it paints without a screen).
    if (const char* shot = std::getenv("SOLAR_GUI_SHOT")) {
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
      std::fprintf(stderr, "frame: layout %.1f ms, display list %.1f ms (%zu items), draw %.1f ms\n", ms(t0, t1), ms(t1, t2), list.items.size(), ms(t2, t3));
    }
  }

  if (texture) SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  solar::html::SetFrameEnvironment(nullptr);
  return 0;
}
