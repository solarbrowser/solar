/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// solar: a page in a window. The page is loaded as the tests load one (a realm with every API of a window, a document, its scripts and style
// sheets fetched as it asks), laid out for the size of the window, painted as a display list and drawn by Skia into a texture SDL shows.
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <pthread.h>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstring>
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
#include "include/core/SkBitmap.h"
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
#include "solar/net/NetworkThread.h"
#include "solar/net/PageFetch.h"
#include "solar/paint/Paint.h"
#include "solar/gui/GpuSurface.h"
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
#include "solar/url/Origin.h"
#include "solar/web-api/CacheStore.h"
#include "solar/gui/SdlAudio.h"
#include "solar/media/Player.h"
#include "solar/web-api/Compression.h"
#include "solar/web-api/Crypto.h"
#include "solar/web-api/IndexedDb.h"
#include "solar/paint/Canvas.h"
#include "solar/web-api/ImageLoad.h"
#include "solar/web-api/Media.h"
#include "solar/web-api/WebSocket.h"
#include "solar/web-api/Worker.h"
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
  bool OnRestart() override {
    // (A page that is being parsed as it comes cannot be given again from the start.)
    if (streaming) return false;
    body.clear();
    status = 0;
    contentType.clear();
    return true;
  }
  void OnError(std::string_view message) override {
    failed = true;
    done = true;
    error = std::string(message);
  }
  int status = 0;
  bool failed = false, done = false;
  bool streaming = false;  // the page is parsed as it comes: the bytes that came are not taken back
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
    solar::web::ForgetMessagingRealm(realm.get());
    realm.reset();
  }
};

// What the pages get from the program: realms of one isolate, and the resources of the network and of the disk.
class Browser : public solar::html::FrameEnvironment, public solar::webapi::WorkerHost {
 public:
  Browser(qe::Isolate& isolate, solar::net::Loop& loop, solar::net::BrowserNetwork& client, solar::web::JsEventLoop& events)
      : isolate_(isolate), loop_(loop), client_(client), events_(events) {
    solar::webapi::SetWorkerHost(this);
  }
  ~Browser() override { solar::webapi::SetWorkerHost(nullptr); }

  // The workers of the pages: a realm each, with the APIs a worker has, of the same isolate.
  qe::Realm* CreateWorkerRealm(const std::string& scriptUrl) override {
    auto bundle = std::make_unique<RealmBundle>();
    qe::Isolate::RealmOptions options;
    options.installConsole = false;
    bundle->realm = isolate_.CreateRealm(options);
    qe::Realm& realm = *bundle->realm;
    solar::html::RunInRealm(realm, [&] {
      solar::web::InstallUrlApis(realm);
      solar::web::InstallConsoleApi(realm, solar::web::DefaultConsoleSink());
      solar::web::InstallDomApis(realm);
      solar::web::InstallFetchApis(realm);
      solar::web::FetchHost::Config config;
      config.loop = &loop_;
      config.client = &client_;
      config.pageUrl = *solar::url::Parse(scriptUrl.rfind("blob:", 0) == 0 || scriptUrl.rfind("data:", 0) == 0 || scriptUrl.empty() ? (pageUrl_.empty() ? "about:blank" : pageUrl_) : scriptUrl);
      config.afterScript = [this] { events_.AfterScript(); };
      bundle->host = std::make_unique<solar::web::FetchHost>(realm.GetContext(), config);
      solar::webapi::InstallWorkerApis(realm);
      solar::webapi::InstallIndexedDbNatives(realm);
      solar::webapi::InstallWebSocketNatives(realm);
      solar::webapi::InstallCryptoNatives(realm);
      solar::webapi::InstallCacheStoreNatives(realm);
      solar::webapi::InstallCompressionNatives(realm);
      solar::webapi::InstallCanvasNatives(realm);
      solar::webapi::InstallWebGlNatives(realm);
      solar::webapi::InstallWorkerScope(realm);
      // A service worker answers a navigation through this.
      Quanta::Context& wctx = realm.GetContext();
      qe::Set(wctx, qe::FromObject(wctx.get_global_object()), "__solarSwNavigateDone", qe::NewFunction(wctx, "__solarSwNavigateDone", 6, [this](Quanta::Context& c, Quanta::Value, qe::Args args, Quanta::Value) {
        if (args.size() < 6) return qe::Undefined();
        std::string body;
        if (auto bytes = qe::BytesOf(args[4])) body.assign(reinterpret_cast<const char*>(bytes->data()), bytes->size());
        NavigationAnswered(qe::ToUint32(c, args[0]), static_cast<int>(qe::ToNumber(c, args[1])), static_cast<int>(qe::ToNumber(c, args[2])), qe::ToWtf8(c, args[3]), std::move(body), qe::ToWtf8(c, args[5]));
        return qe::Undefined();
      }));
    });
    qe::Realm* raw = bundle->realm.get();
    bundles_.push_back(std::move(bundle));
    return raw;
  }
  void ReleaseWorkerRealm(qe::Realm* realm) override {
    if (dropTimers_) dropTimers_(realm);
    for (auto it = bundles_.begin(); it != bundles_.end(); ++it) {
      if ((*it)->realm.get() == realm) {
        bundles_.erase(it);
        break;
      }
    }
  }
  void RunLater(std::function<void()> task) override { loop_.Post(std::move(task)); }
  std::optional<std::string> LoadScript(const std::string& url) override {
    // (importScripts and the like are not the worker's to answer: it is the one asking.)
    const bool was = bypassWorker_;
    bypassWorker_ = true;
    auto text = Load(url);
    bypassWorker_ = was;
    return text;
  }
  std::function<void(qe::Realm*)> dropTimers_;  // (set where the timers are: the tasks of a worker that is let go are not to run)

  // A page that is left: its realm goes, and with it what only it kept.
  void ReleaseRealm(qe::Realm* realm) {
    // (What the realm's own data holds is not let go with the realm: the document it points at is let go first.)
    solar::html::RunInRealm(*realm, [&] { solar::dom::SetAssociatedDocument(realm->GetContext(), nullptr); });
    if (pageRealm_ == realm) pageRealm_ = nullptr;
    solar::webapi::ReleaseOwnedWorkers(realm);
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
      solar::webapi::InstallWorkerApis(realm);
      solar::webapi::InstallIndexedDbNatives(realm);
      solar::webapi::InstallWebSocketNatives(realm);
      solar::webapi::InstallCryptoNatives(realm);
      solar::webapi::InstallCacheStoreNatives(realm);
      solar::webapi::InstallCompressionNatives(realm);
      solar::webapi::InstallMediaNatives(realm);
      solar::webapi::InstallCanvasNatives(realm);
      solar::webapi::InstallWebGlNatives(realm);
      solar::webapi::InstallImageLoadNatives(realm);
      solar::web::FetchHost::Config config;
      config.loop = &loop_;
      config.client = &client_;
      config.pageUrl = *solar::url::Parse(pageUrl_.empty() ? "about:blank" : pageUrl_);
      config.afterScript = [this] { events_.AfterScript(); };
      bundle->host = std::make_unique<solar::web::FetchHost>(realm.GetContext(), config);
      // What the page's service worker script (src/web-api/js/serviceworker.js) tells the program: whether the page is controlled, and its answers for resources.
      Quanta::Context& pctx = realm.GetContext();
      Quanta::Value global = qe::FromObject(pctx.get_global_object());
      qe::Set(pctx, global, "__solarWindowFullscreen", qe::NewFunction(pctx, "__solarWindowFullscreen", 1, [this](Quanta::Context&, Quanta::Value, qe::Args args, Quanta::Value) {
        if (fullscreen_ && !args.empty()) fullscreen_(args[0].to_boolean());
        return qe::Undefined();
      }));
      qe::Set(pctx, global, "__solarPointerLock", qe::NewFunction(pctx, "__solarPointerLock", 1, [this](Quanta::Context&, Quanta::Value, qe::Args args, Quanta::Value) {
        if (pointerLock_ && !args.empty()) pointerLock_(args[0].to_boolean());
        return qe::Undefined();
      }));
      qe::Set(pctx, global, "__solarSwControlled", qe::NewFunction(pctx, "__solarSwControlled", 1, [this](Quanta::Context&, Quanta::Value, qe::Args args, Quanta::Value) {
        swControlled_ = !args.empty() && args[0].to_boolean();
        return qe::Undefined();
      }));
      qe::Set(pctx, global, "__solarSwResourceDone", qe::NewFunction(pctx, "__solarSwResourceDone", 5, [this](Quanta::Context& c, Quanta::Value, qe::Args args, Quanta::Value) {
        if (args.size() < 5) return qe::Undefined();
        std::string body;
        if (auto bytes = qe::BytesOf(args[4])) body.assign(reinterpret_cast<const char*>(bytes->data()), bytes->size());
        WorkerAnswered(qe::ToUint32(c, args[0]), static_cast<int>(qe::ToNumber(c, args[1])), static_cast<int>(qe::ToNumber(c, args[2])), qe::ToWtf8(c, args[3]), std::move(body));
        return qe::Undefined();
      }));
    });
    pageRealm_ = &realm;
    swControlled_ = false;
    bundles_.push_back(std::move(bundle));
    return &realm;
  }

  // A resource in flight or here: what the network brought for an address, kept for as long as the program runs (pages share their scripts, styles and pictures).
  // What a load is for: it decides how the network service asks for it (see net::RequestDestination). Fonts and module scripts are asked for with CORS, the rest without.
  enum class Kind { Generic, Script, Style, Image, Font, Module };
  static bool WithCors(Kind kind) { return kind == Kind::Font || kind == Kind::Module; }
  static std::string KeyOf(const std::string& url, Kind kind) { return WithCors(kind) ? url + "\x01" "cors" : url; }

  struct Resource : solar::net::PageFetchHandler {
    Browser* owner = nullptr;
    std::string url;
    Kind kind = Kind::Generic;
    int status = 0;
    bool done = false, failed = false;
    bool viaWorker = false;  // asked of the page's service worker (what it made is that page's, not to be kept for the next)
    std::string body, contentType;
    solar::net::FetchHandle handle;
    Clock::time_point started = Clock::now();
    void OnResponse(const solar::net::PageResponse& response) override {
      status = response.status;
      for (const auto& [name, value] : response.headers) {
        std::string lower = name;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == "content-type") contentType = value;
      }
    }
    void OnBody(std::span<const uint8_t> data) override { body.append(reinterpret_cast<const char*>(data.data()), data.size()); }
    void OnEnd() override { Finish(); }
    void OnRestart() override {
      body.clear();
      status = 0;
      contentType.clear();
    }
    std::string error;
    void OnError(std::string_view message) override {
      failed = true;
      error = std::string(message);
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

  // What the page asks the network for a resource of its own (a script, a style sheet, a picture, a font): a Fetch Standard request of the kind it is, from the page, which the
  // network service applies the rules of fetching to (CORS where the kind has it, Opaque Response Blocking where it has none).
  void FetchResource(Resource& resource) {
    std::optional<solar::url::Url> parsed = solar::url::Parse(resource.url);
    if (!parsed) return;
    solar::net::PageRequest request;
    request.url = *parsed;
    request.pageUrl = solar::url::Parse(pageUrl_).value_or(*parsed);
    request.restartable = true;
    // (A module and a font are asked for with the cookies of the page's own origin only, so that a server that allows everyone does: credentials "same-origin".)
    request.credentials = WithCors(resource.kind) ? solar::net::RequestCredentials::SameOrigin : solar::net::RequestCredentials::Include;
    request.mode = WithCors(resource.kind) ? solar::net::RequestMode::Cors : solar::net::RequestMode::NoCors;
    switch (resource.kind) {
      case Kind::Script: request.destination = solar::net::RequestDestination::Script; break;
      case Kind::Style: request.destination = solar::net::RequestDestination::Style; break;
      case Kind::Image: request.destination = solar::net::RequestDestination::Image; break;
      case Kind::Font: request.destination = solar::net::RequestDestination::Font; break;
      case Kind::Module: request.destination = solar::net::RequestDestination::Module; break;
      default: request.destination = solar::net::RequestDestination::Other; break;
    }
    resource.handle = client_.StartFetch(std::move(request), resource);
  }

  // Starts getting the resource at the address, if it is not being got already. Null for what is not for the network.
  Resource* Start(const std::string& url, Kind kind = Kind::Generic) {
    const std::string key = KeyOf(url, kind);
    const auto found = resources_.find(key);
    if (found != resources_.end()) return found->second.get();
    std::optional<solar::url::Url> parsed = solar::url::Parse(url);
    if (!parsed || (parsed->scheme != "http" && parsed->scheme != "https")) return nullptr;
    auto resource = std::make_unique<Resource>();
    resource->owner = this;
    resource->url = url;
    resource->kind = kind;
    Resource* raw = resource.get();
    resources_[key] = std::move(resource);
    if (swControlled_ && !bypassWorker_ && pageRealm_ && AskWorker(raw)) return raw;
    FetchResource(*raw);
    return raw;
  }


  // A navigation to an address a service worker is registered for is asked of the worker first (its fetch event, with mode "navigate"), whether or not any page is open: the
  // worker that is running is used, or the one that was kept is started. False where there is none (the network gets the request).
  struct PendingNavigation {
    Collector* collector;
    solar::url::Url url;
    solar::net::FetchOptions options;
  };
  bool TryWorkerNavigation(const solar::url::Url& url, const std::string& address, const solar::net::FetchOptions& options, Collector& collector) {
    if (url.scheme != "http" && url.scheme != "https") return false;
    const std::string origin = solar::url::SerializeOrigin(url);
    uint32_t id = 0;
    if (!solar::webapi::FindServiceWorker(origin, address, id)) {
      if (!solar::webapi::HasServiceWorkerFor(origin, address)) return false;
      id = solar::webapi::StartStoredServiceWorker(origin, address);
      if (id == 0) return false;
    }
    qe::Realm* realm = solar::webapi::WorkerRealmOf(id);
    if (!realm) return false;
    const uint32_t navigation = nextNavigation_++;
    // (The worker may answer before the call returns: the navigation is there to be answered by then.)
    pendingNavigations_[navigation] = PendingNavigation{&collector, url, options};
    bool taken = false;
    solar::html::RunInRealm(*realm, [&] {
      Quanta::Context& ctx = realm->GetContext();
      Quanta::Value function = qe::Get(ctx, qe::FromObject(ctx.get_global_object()), "__solarSwNavigate");
      if (qe::IsCallable(function)) {
        Quanta::Value body = qe::Undefined();
        if (options.body && !options.body->empty()) {
          body = qe::NewArrayBuffer(ctx, options.body->size());
          if (auto bytes = qe::MutableBytesOf(body)) std::memcpy(bytes->data(), options.body->data(), options.body->size());
        }
        std::string bodyType;
        for (const auto& header : options.headers) if (header.first == "Content-Type") bodyType = header.second;
        Quanta::Value arguments[] = {qe::FromNumber(navigation), qe::FromWtf8(ctx, address), qe::FromWtf8(ctx, options.method.empty() ? "GET" : options.method), body, qe::FromWtf8(ctx, bodyType)};
        qe::Call(ctx, function, qe::Undefined(), qe::Args(arguments, 5));
        taken = !qe::HasException(ctx);
      }
      if (qe::HasException(ctx)) ctx.clear_exception();
    });
    if (!taken) {
      pendingNavigations_.erase(navigation);
      return false;
    }
    return true;
  }
  void NavigationAnswered(uint32_t id, int mode, int status, const std::string& contentType, std::string body, const std::string& address) {
    const auto found = pendingNavigations_.find(id);
    if (found == pendingNavigations_.end()) return;
    PendingNavigation pending = std::move(found->second);
    pendingNavigations_.erase(found);
    Collector& collector = *pending.collector;
    if (mode == 1) {  // left to the network
      client_.Fetch(pending.url, collector, pending.options);
      return;
    }
    if (mode == 2) {
      collector.failed = true;
      collector.done = true;
      collector.error = "the service worker's answer was a network error";
      return;
    }
    collector.status = status;
    collector.contentType = contentType;
    collector.address = address;
    collector.body = std::move(body);
    collector.done = true;
  }

  // A page that a service worker controls asks it for each resource first (its fetch event); the worker's answer, or its leaving the request to the network, comes
  // back through __solarSwResourceDone. False where the page has no way to ask.
  bool AskWorker(Resource* resource) {
    const uint32_t id = nextSw_++;
    bool taken = false;
    solar::html::RunInRealm(*pageRealm_, [&] {
      Quanta::Context& ctx = pageRealm_->GetContext();
      Quanta::Value function = qe::Get(ctx, qe::FromObject(ctx.get_global_object()), "__solarSwResource");
      if (qe::IsCallable(function)) {
        Quanta::Value arguments[] = {qe::FromWtf8(ctx, resource->url), qe::FromNumber(id)};
        Quanta::Value result = qe::Call(ctx, function, qe::Undefined(), qe::Args(arguments, 2));
        taken = !qe::HasException(ctx) && result.to_boolean();
      }
      if (qe::HasException(ctx)) ctx.clear_exception();
    });
    if (!taken) return false;
    resource->viaWorker = true;
    swPending_[id] = resource->url;
    return true;
  }
  void WorkerAnswered(uint32_t id, int mode, int status, const std::string& contentType, std::string body) {
    const auto pending = swPending_.find(id);
    if (pending == swPending_.end()) return;
    const std::string url = pending->second;
    swPending_.erase(pending);
    Resource* resource = nullptr;
    for (auto& entry : resources_) {
      if (entry.second->url == url && entry.second->viaWorker) resource = entry.second.get();
    }
    if (!resource) return;
    if (mode == 1) {  // left to the network
      resource->viaWorker = false;
      FetchResource(*resource);
      return;
    }
    if (mode == 2) {
      resource->failed = true;
    } else {
      resource->status = status;
      resource->contentType = contentType;
      resource->body = std::move(body);
    }
    resource->Finish();
  }
  // The page is left: what its worker made is not for the next page.
  void DropWorkerResources() {
    for (auto it = resources_.begin(); it != resources_.end();) {
      if (it->second->viaWorker && it->second->done) it = resources_.erase(it);
      else ++it;
    }
    swControlled_ = false;
  }

  void WaitFor(Resource* resource) {
    // The page's own tasks run while a resource is waited for, even when layout is what asked for it: they see the tree as the page left it.
    solar::dom::CommittedViewSuspend suspend;
    waiting_ = resource;
    const auto waitStart = Clock::now();
    while (!resource->done) {
      // (In the middle of working out a style nothing of the page runs: its script would change what the computation holds.)
      static const bool tasksInStyle = std::getenv("SOLAR_TASKS_IN_STYLE") != nullptr;
      if (!tasksInStyle && solar::css::StyleComputationActive()) {
        events_.StepNetworkOnly(std::chrono::milliseconds(8));
        if (pump_) pump_();
        continue;
      }
      events_.Step(std::chrono::milliseconds(std::getenv("SOLAR_SLICE") ? std::atoi(std::getenv("SOLAR_SLICE")) : 8));
      if (pump_) pump_();
      // (What the page has so far can be shown while what it waits for comes, unless that is a style sheet.)
      if (idle_ && !resource->done && !solar::html::LoadingStyleSheet()) idle_();
    }
    const double thisWait = std::chrono::duration<double, std::milli>(Clock::now() - waitStart).count();
    waited_ += thisWait;
    if (timing_ && thisWait > 300) std::fprintf(stderr, "  waited %.0f ms for %s\n", thisWait, resource->url.substr(0, 110).c_str());
    waiting_ = nullptr;
  }

  void Prefetch(const std::string& url) override { Start(url, Kind::Module); }  // (the modules a module imports)

  std::optional<std::string> Load(const std::string& url) override { return LoadAs(url, Kind::Generic); }
  // A script or a style sheet, with no CORS (a response from another origin that Opaque Response Blocking blocks comes back with no text); a module and a font, with it.
  std::optional<std::string> LoadNoCors(const std::string& url, bool styleSheet) override { return LoadAs(url, styleSheet ? Kind::Style : Kind::Script); }
  std::optional<std::string> LoadModule(const std::string& url) override { return LoadAs(url, Kind::Module); }
  std::optional<std::string> LoadFont(const std::string& url) override { return LoadAs(url, Kind::Font); }

  std::optional<std::string> LoadAs(const std::string& url, Kind kind) {
    if (url.starts_with("file://")) {
      std::string contents;
      if (!ReadFile(FileOfAddress(url), contents)) return std::nullopt;
      return contents;
    }
    Resource* resource = Start(url, kind);
    if (!resource) return std::nullopt;
    WaitFor(resource);
    if (resource->failed || resource->status < 200 || resource->status >= 300) {
      std::fprintf(stderr, "  [%d%s] %s%s%s\n", resource->status, resource->failed ? " failed" : "", url.c_str(), resource->error.empty() ? "" : "  : ", resource->error.c_str());
      return std::nullopt;
    }
    return resource->body;
  }

  // The pictures that layout and painting asked for and that are on their way: they are fetched together, and the page is laid out again when they are here.
  bool RequestImage(const std::string& absolute) {
    if (absolute.starts_with("file://")) return false;
    const auto found = resources_.find(KeyOf(absolute, Kind::Image));
    if (found != resources_.end() && found->second->done) return false;  // here (or failed): the loader has it
    if (!Start(absolute, Kind::Image)) return false;
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
    scanStarted_ = 0;
    scanImages_ = false;
    ScanForResources(markup, baseAddress);
  }
  // The same for a page that comes in pieces: what came since is looked at, as far as the last tag that is all there (a tag cut in two is looked at when the rest comes).
  void BeginStreamScan() { scanFrom_ = 0, scanStarted_ = 0, scanImagesStarted_ = 0, scanImages_ = !std::getenv("SOLAR_NO_IMG_SCAN"); }
  void StreamScan(const std::string& body, const std::string& baseAddress, bool final) {
    if (scanFrom_ >= body.size()) return;
    std::string segment = body.substr(scanFrom_);
    if (!final) {
      const size_t lastEnd = segment.rfind('>');
      if (lastEnd == std::string::npos) return;
      segment.resize(lastEnd + 1);
    }
    scanFrom_ += segment.size();
    ScanForResources(segment, baseAddress);
  }

  // Waits for the document to have more than `have` bytes, or to be all here (the page's own tasks run meanwhile, and what the page has so far is shown).
  void WaitForDocument(const Collector& collector, size_t have) {
    solar::dom::CommittedViewSuspend suspend;
    const auto waitStart = Clock::now();
    while (collector.body.size() <= have && !collector.done) {
      events_.Step(std::chrono::milliseconds(std::getenv("SOLAR_SLICE") ? std::atoi(std::getenv("SOLAR_SLICE")) : 8));
      if (pump_) pump_();
      if (idle_ && collector.body.size() <= have && !collector.done && !solar::html::LoadingStyleSheet()) idle_();
    }
    waited_ += std::chrono::duration<double, std::milli>(Clock::now() - waitStart).count();
  }

  void ScanForResources(const std::string& markup, const std::string& baseAddress) {
    std::optional<solar::url::Url> base = solar::url::Parse(baseAddress);
    if (!base) return;
    std::string lower = markup;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    int& started = scanStarted_;
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
    // (Pictures wait: what the page needs to be made goes first. In a page that comes in pieces they are asked for too, as far as the network has room, the ones that are not lazy.)
    for (const char* name : {"<link", "<script", "<img"}) {
      if (std::string_view(name) == "<img" && !scanImages_) continue;
      size_t at = 0;
      const std::string needle = name;
      while ((at = lower.find(needle, at)) != std::string::npos && started < 80) {
        const size_t end = lower.find('>', at);
        if (end == std::string::npos) break;
        const std::string tag = markup.substr(at, end - at + 1), tagLower = lower.substr(at, end - at + 1);
        at = end;
        std::string target;
        Kind kind = Kind::Script;
        if (needle == "<link") {
          const std::string rel = attribute(tag, tagLower, "rel");
          if (rel.find("stylesheet") == std::string::npos && rel.find("preload") == std::string::npos) continue;
          if (rel.find("preload") != std::string::npos) {
            const std::string as = attribute(tag, tagLower, "as");
            if (as != "style" && as != "script" && as != "image") continue;
            kind = as == "style" ? Kind::Style : as == "image" ? Kind::Image : Kind::Script;
          } else {
            kind = Kind::Style;
          }
          target = attribute(tag, tagLower, "href");
        } else if (needle == "<script") {
          target = attribute(tag, tagLower, "src");
          if (attribute(tag, tagLower, "type") == "module") kind = Kind::Module;
        } else {
          kind = Kind::Image;
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
        if (needle == "<img") {
          if (attribute(tag, tagLower, "loading") == "lazy" || scanImagesStarted_ >= 24) continue;
          int inFlight = 0;
          for (const auto& entry : resources_) inFlight += entry.second->done ? 0 : 1;
          if (inFlight >= 8) continue;
          if (Start(absolute, Kind::Image)) ++scanImagesStarted_;
          continue;
        }
        if (Start(absolute, kind)) ++started;
      }
    }
  }

  std::string ContentType(const std::string& url) override {
    const auto found = types_.find(url);
    if (found != types_.end()) return found->second;
    for (const Kind kind : {Kind::Generic, Kind::Module}) {
      const auto resource = resources_.find(KeyOf(url, kind));
      if (resource != resources_.end() && resource->second && !resource->second->contentType.empty()) return resource->second->contentType;
    }
    return solar::html::FrameEnvironment::ContentType(url);
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
  size_t scanFrom_ = 0;  // how far a page that comes in pieces has been looked at for what it asks for
  int scanStarted_ = 0, scanImagesStarted_ = 0;
  bool scanImages_ = false;
  qe::Isolate& isolate_;
  solar::net::Loop& loop_;
  solar::net::BrowserNetwork& client_;
  solar::web::JsEventLoop& events_;
  std::string pageUrl_;
  std::map<std::string, std::string> types_;
  std::map<std::string, std::pair<std::string, std::shared_ptr<qe::Script>>> scripts_;
  std::vector<std::unique_ptr<RealmBundle>> bundles_;
  std::map<std::string, std::unique_ptr<Resource>> resources_;
  qe::Realm* pageRealm_ = nullptr;
  bool swControlled_ = false, bypassWorker_ = false;
  uint32_t nextSw_ = 1, nextNavigation_ = 1;
  std::map<uint32_t, PendingNavigation> pendingNavigations_;
  std::map<uint32_t, std::string> swPending_;
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
  std::function<void(bool)> pointerLock_;  // (set where the window is: the pointer is hidden and locked, or not)
  std::function<void(bool)> fullscreen_;  // (set where the window is: the whole window is made full screen, or not)
  std::function<void()> idle_;  // called while a resource is waited for
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

namespace {

// ---- The two threads ----
// The window lives on the thread the program starts on: it takes the events, keeps the scroll of the page (a wheel step is a glide), draws the
// display list the page made for the scroll it has now, and draws the scrollbar. The page (its scripts, style, layout and the making of the display
// list) is on a thread of its own, so that a script that runs long does not stop the page from scrolling. They tell each other through Shared.

struct UiEvent {
  SDL_Event event{};
  std::string text;  // a text input's text (the event's own is gone with the next poll)
  std::string clipboard;  // what the clipboard held when a key that pastes was pressed
};

// What a key of the keyboard is to a page: KeyboardEvent.key, .code and .keyCode.
struct KeyInfo {
  std::string key, code;
  int keyCode = 0;
  int modifiers = 0;  // 1 ctrl, 2 shift, 4 alt, 8 meta
};

KeyInfo DescribeKey(const SDL_KeyboardEvent& k) {
  KeyInfo info;
  if (k.mod & SDL_KMOD_CTRL) info.modifiers |= 1;
  if (k.mod & SDL_KMOD_SHIFT) info.modifiers |= 2;
  if (k.mod & SDL_KMOD_ALT) info.modifiers |= 4;
  if (k.mod & SDL_KMOD_GUI) info.modifiers |= 8;
  const auto set = [&](const char* key, const char* code, int keyCode) { info.key = key; info.code = code; info.keyCode = keyCode; };
  switch (k.key) {
    case SDLK_RETURN: case SDLK_KP_ENTER: set("Enter", "Enter", 13); return info;
    case SDLK_ESCAPE: set("Escape", "Escape", 27); return info;
    case SDLK_BACKSPACE: set("Backspace", "Backspace", 8); return info;
    case SDLK_TAB: set("Tab", "Tab", 9); return info;
    case SDLK_DELETE: set("Delete", "Delete", 46); return info;
    case SDLK_INSERT: set("Insert", "Insert", 45); return info;
    case SDLK_SPACE: set(" ", "Space", 32); return info;
    case SDLK_LEFT: set("ArrowLeft", "ArrowLeft", 37); return info;
    case SDLK_UP: set("ArrowUp", "ArrowUp", 38); return info;
    case SDLK_RIGHT: set("ArrowRight", "ArrowRight", 39); return info;
    case SDLK_DOWN: set("ArrowDown", "ArrowDown", 40); return info;
    case SDLK_HOME: set("Home", "Home", 36); return info;
    case SDLK_END: set("End", "End", 35); return info;
    case SDLK_PAGEUP: set("PageUp", "PageUp", 33); return info;
    case SDLK_PAGEDOWN: set("PageDown", "PageDown", 34); return info;
    case SDLK_LSHIFT: set("Shift", "ShiftLeft", 16); return info;
    case SDLK_RSHIFT: set("Shift", "ShiftRight", 16); return info;
    case SDLK_LCTRL: set("Control", "ControlLeft", 17); return info;
    case SDLK_RCTRL: set("Control", "ControlRight", 17); return info;
    case SDLK_LALT: set("Alt", "AltLeft", 18); return info;
    case SDLK_RALT: set("Alt", "AltRight", 18); return info;
    case SDLK_LGUI: set("Meta", "MetaLeft", 91); return info;
    case SDLK_RGUI: set("Meta", "MetaRight", 92); return info;
    case SDLK_CAPSLOCK: set("CapsLock", "CapsLock", 20); return info;
    default: break;
  }
  if (k.key >= SDLK_F1 && k.key <= SDLK_F12) {
    const int n = static_cast<int>(k.key - SDLK_F1) + 1;
    info.key = info.code = "F" + std::to_string(n);
    info.keyCode = 111 + n;
    return info;
  }
  if (k.key >= 'a' && k.key <= 'z') {
    const bool upper = (k.mod & SDL_KMOD_SHIFT) != 0 != ((k.mod & SDL_KMOD_CAPS) != 0);
    const char letter = static_cast<char>(upper ? k.key - 'a' + 'A' : k.key);
    info.key = std::string(1, letter);
    info.code = std::string("Key") + static_cast<char>(k.key - 'a' + 'A');
    info.keyCode = static_cast<int>(k.key - 'a') + 65;
    return info;
  }
  if (k.key >= '0' && k.key <= '9') {
    info.key = std::string(1, static_cast<char>(k.key));
    info.code = std::string("Digit") + static_cast<char>(k.key);
    info.keyCode = static_cast<int>(k.key);
    return info;
  }
  if (k.key >= 0x20 && k.key < 0x7f) {
    info.key = std::string(1, static_cast<char>(k.key));
    info.keyCode = static_cast<int>(k.key);
  }
  return info;
}

// ---- Editing: where the caret goes ----
// (The text is UTF-8 and the positions are bytes; whoever talks to the page converts to the UTF-16 units its selection counts in.)
bool IsWordByte(unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; }

size_t CharBefore(const std::string& s, size_t at) {
  if (at == 0) return 0;
  --at;
  while (at > 0 && (static_cast<unsigned char>(s[at]) & 0xC0) == 0x80) --at;
  return at;
}

size_t CharAfter(const std::string& s, size_t at) {
  if (at >= s.size()) return s.size();
  const unsigned char c = static_cast<unsigned char>(s[at]);
  return std::min(s.size(), at + (c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4));
}

// The start of the word at or before `at` (white space and punctuation before it are passed first), and the end of the word at or after it.
size_t WordStartBefore(const std::string& s, size_t at) {
  while (at > 0 && !IsWordByte(static_cast<unsigned char>(s[at - 1]))) --at;
  while (at > 0 && IsWordByte(static_cast<unsigned char>(s[at - 1]))) --at;
  return at;
}

size_t WordEndAfter(const std::string& s, size_t at) {
  while (at < s.size() && !IsWordByte(static_cast<unsigned char>(s[at]))) ++at;
  while (at < s.size() && IsWordByte(static_cast<unsigned char>(s[at]))) ++at;
  return at;
}

// The word that has `at` in it (or the white space run, or the one punctuation mark), for a double click.
void WordAround(const std::string& s, size_t at, size_t& from, size_t& to) {
  at = std::min(at, s.size());
  from = to = at;
  if (s.empty()) return;
  const size_t probe = at < s.size() ? at : at - 1;
  const bool word = IsWordByte(static_cast<unsigned char>(s[probe]));
  const bool space = s[probe] == ' ' || s[probe] == '\t';
  const auto same = [&](unsigned char c) { return word ? IsWordByte(c) : space ? (c == ' ' || c == '\t') : !IsWordByte(c) && c != ' ' && c != '\t'; };
  from = probe;
  to = probe + 1;
  while (from > 0 && same(static_cast<unsigned char>(s[from - 1]))) --from;
  while (to < s.size() && same(static_cast<unsigned char>(s[to]))) ++to;
}

size_t LineStartAt(const std::string& s, size_t at) {
  const size_t newline = at == 0 ? std::string::npos : s.rfind('\n', at - 1);
  return newline == std::string::npos ? 0 : newline + 1;
}

size_t LineEndAt(const std::string& s, size_t at) {
  const size_t newline = s.find('\n', at);
  return newline == std::string::npos ? s.size() : newline;
}

// A page in a frame that has more than its room: the wheel over it scrolls it, and the window has to know where it is to leave the page alone.
// (The place is the frame's content box, in the coordinates of the page, or of the window for what is held in the viewport.)
struct ScrollRegion {
  double x = 0, y = 0, width = 0, height = 0;
  bool fixed = false;
  double curX = 0, curY = 0, maxX = 0, maxY = 0;
};

struct Frame {
  std::shared_ptr<const solar::paint::DisplayList> list;
  double maxX = 0, maxY = 0;  // how far the page can be scrolled
  std::vector<ScrollRegion> regions;  // the frames that can be scrolled, the outer ones first
};

// The pointer is locked to an element of the page (the Pointer Lock API): the window hides the pointer and gives movements, and the page's mouse events are the locked element's.
std::atomic<bool> g_pointerLocked{false};

struct UiCommand {
  enum class Kind { Title, ScrollBy, ScrollTo, Forget, Shot, Quit, Clipboard, Fullscreen, PointerLock } kind = Kind::Title;
  std::string text;
  double x = 0, y = 0;
  bool instant = false;
};

class Shared {
 public:
  // The window, as the thread of the page needs to know it (in points, and the pixels they come to).
  std::atomic<int> width{1280}, height{800}, pixelWidth{1280}, pixelHeight{800};
  std::atomic<float> density{1.f};
  std::atomic<bool> quit{false}, pageDone{false};
  // The scroll of the page as the window has it; the serial moves with each change.
  std::atomic<double> scrollX{0}, scrollY{0};
  std::atomic<uint64_t> scrollSerial{0};
  std::atomic<bool> gliding{false};
  std::atomic<int64_t> scrollInputNs{0};  // when the user last asked for scrolling (the steady clock)

  // The window to the page.
  void PushEvent(UiEvent event) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      events_.push_back(std::move(event));
    }
    cv_.notify_one();
  }
  // The next event, waiting at most `wait` for one.
  bool WaitEvent(UiEvent& out, std::chrono::milliseconds wait) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (events_.empty() && wait.count() > 0) cv_.wait_for(lock, wait, [&] { return !events_.empty() || quit.load(); });
    if (events_.empty()) return false;
    out = std::move(events_.front());
    events_.pop_front();
    return true;
  }
  // The page to the window.
  void PostFrame(Frame frame) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      frame_ = std::move(frame);
      hasFrame_ = true;
    }
    WakeWindow();
  }
  bool TakeFrame(Frame& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!hasFrame_) return false;
    out = std::move(frame_);
    hasFrame_ = false;
    return true;
  }
  void Post(UiCommand command) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      commands_.push_back(std::move(command));
    }
    WakeWindow();
  }
  std::vector<UiCommand> TakeCommands() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<UiCommand> out(commands_.begin(), commands_.end());
    commands_.clear();
    return out;
  }
  void WakeWindow() {
    SDL_Event wake{};
    wake.type = SDL_EVENT_USER;
    SDL_PushEvent(&wake);
  }
  void WakePage() { cv_.notify_all(); }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<UiEvent> events_;
  std::deque<UiCommand> commands_;
  Frame frame_;
  bool hasFrame_ = false;
};

int64_t NowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }

// The window: events in, pixels out. Returns when the program is to end.
void RunWindow(Shared& shared, SDL_Window* window, SDL_Renderer* renderer, solar::gui::GpuSurface* gpu) {
  solar::paint::SkiaPainter painter;
  sk_sp<SkSurface> surface;
  SDL_Texture* texture = nullptr;
  int surfaceW = 0, surfaceH = 0;
  Frame frame;
  // Scrolling goes toward where it was asked to, a part of the way each frame (it eases out), so that a wheel step is a glide and not a jump.
  double x = 0, y = 0, targetX = 0, targetY = 0;
  bool gliding = false;
  Clock::time_point lastGlide = Clock::now();
  Clock::time_point scrollActive = Clock::now() - std::chrono::seconds(10);
  // The scrollbar of the window floats over the page: it shows while the page is scrolled (and under the pointer), then fades; it can be dragged.
  bool barDrag = false, barHover = false;
  float lockedAtX = 0, lockedAtY = 0;  // (where the pointer was when it was locked: it goes back there)
  double barGrab = 0;
  struct Bar {
    double top = 0, height = 0, track = 0, maxY = 0;
  };
  const auto barOf = [&](Bar& bar) -> bool {
    const int h = shared.height.load();
    if (frame.maxY <= 0.5 || h <= 0) return false;
    bar.track = h;
    bar.height = std::max(32.0, double(h) * h / (h + frame.maxY));
    bar.maxY = frame.maxY;
    bar.top = y / frame.maxY * (h - bar.height);
    return true;
  };
  bool dirty = true;
  bool running = true;
  std::string shotPath;
  const auto publish = [&] {
    shared.scrollX = x;
    shared.scrollY = y;
    ++shared.scrollSerial;
    shared.gliding = gliding;
    shared.WakePage();
  };
  const auto clampTargets = [&] {
    targetX = std::clamp(targetX, 0.0, std::max(0.0, frame.maxX));
    targetY = std::clamp(targetY, 0.0, std::max(0.0, frame.maxY));
  };
  const auto scrollBy = [&](double dx, double dy, bool instant) {
    if (!gliding) {
      targetX = x;
      targetY = y;
    }
    targetX += dx;
    targetY += dy;
    clampTargets();
    scrollActive = Clock::now();
    shared.scrollInputNs = NowNs();
    if (instant) {
      x = targetX;
      y = targetY;
      gliding = false;
      publish();
    } else {
      if (!gliding) lastGlide = Clock::now();
      gliding = true;
      shared.gliding = true;
    }
    dirty = true;
  };
  const auto scrollTo = [&](double nx, double ny) {
    targetX = x = std::clamp(nx, 0.0, std::max(0.0, frame.maxX));
    targetY = y = std::clamp(ny, 0.0, std::max(0.0, frame.maxY));
    gliding = false;
    publish();
    dirty = true;
  };
  const auto geometry = [&] {
    int w = 0, h = 0, pw = 0, ph = 0;
    SDL_GetWindowSize(window, &w, &h);
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    shared.width = w;
    shared.height = h;
    shared.pixelWidth = pw;
    shared.pixelHeight = ph;
    shared.density = SDL_GetWindowPixelDensity(window);
    // (SOLAR_GUI_DENSITY=2: as on a high-density screen, for timing what that costs without one.)
    if (const char* forced = gpu ? nullptr : std::getenv("SOLAR_GUI_DENSITY")) {
      const float d = static_cast<float>(std::atof(forced));
      if (d >= 1) {
        shared.density = d;
        shared.pixelWidth = static_cast<int>(w * d);
        shared.pixelHeight = static_cast<int>(h * d);
      }
    }
  };
  geometry();
  while (running && !shared.quit.load()) {
    const bool animating = solar::media::PlayingCount() > 0 || gliding || (std::chrono::duration<double>(Clock::now() - scrollActive).count() < 1.5 && frame.maxY > 0.5);
    SDL_Event event;
    bool any = SDL_WaitEventTimeout(&event, dirty || animating ? 0 : 50);
    while (any) {
      switch (event.type) {
        case SDL_EVENT_QUIT: running = false; break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
          geometry();
          UiEvent forward;
          forward.event = event;
          shared.PushEvent(std::move(forward));
          dirty = true;
          break;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
          const double stepX = -event.wheel.x * 60, stepY = -event.wheel.y * 60;
          // A frame under the pointer that can be scrolled that way takes the wheel (the page of the frame scrolls it); at its end, the page has it.
          bool frameTakes = false;
          for (auto it = frame.regions.rbegin(); it != frame.regions.rend() && !frameTakes; ++it) {
            const double px = event.wheel.mouse_x + (it->fixed ? 0 : x), py = event.wheel.mouse_y + (it->fixed ? 0 : y);
            if (px < it->x || py < it->y || px >= it->x + it->width || py >= it->y + it->height) continue;
            const bool moreX = stepX > 0 ? it->curX < it->maxX : stepX < 0 && it->curX > 0;
            const bool moreY = stepY > 0 ? it->curY < it->maxY : stepY < 0 && it->curY > 0;
            frameTakes = moreX || moreY;
          }
          if (!frameTakes) scrollBy(stepX, stepY, false);
          // (The page hears of it too, for what it does with the wheel besides scrolling; it cannot hold the scroll back.)
          UiEvent forward;
          forward.event = event;
          shared.PushEvent(std::move(forward));
          break;
        }
        case SDL_EVENT_TEXT_INPUT: {
          UiEvent forward;
          forward.event = event;
          forward.text = event.text.text ? event.text.text : "";
          shared.PushEvent(std::move(forward));
          break;
        }
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
          UiEvent forward;
          forward.event = event;
          if (event.type == SDL_EVENT_KEY_DOWN && (((event.key.mod & SDL_KMOD_CTRL) && event.key.key == SDLK_V) || ((event.key.mod & SDL_KMOD_SHIFT) && event.key.key == SDLK_INSERT))) {
            char* held = SDL_GetClipboardText();
            forward.clipboard = held ? held : "";
            SDL_free(held);
          }
          shared.PushEvent(std::move(forward));
          break;
        }
        case SDL_EVENT_MOUSE_MOTION: {
          const int ww = shared.width.load();
          if (g_pointerLocked.load()) {
            UiEvent forward;
            forward.event = event;
            shared.PushEvent(std::move(forward));
            break;
          }
          if (barDrag) {
            Bar bar;
            if (barOf(bar) && bar.track > bar.height) {
              const double ny = std::clamp((event.motion.y - barGrab) / (bar.track - bar.height), 0.0, 1.0) * bar.maxY;
              scrollTo(x, ny);
              scrollActive = Clock::now();
              shared.scrollInputNs = NowNs();
            }
            break;
          }
          const bool over = event.motion.x >= ww - 16;
          if (over != barHover) {
            barHover = over;
            scrollActive = Clock::now();
            dirty = true;
          }
          UiEvent forward;
          forward.event = event;
          shared.PushEvent(std::move(forward));
          break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
          const int ww = shared.width.load(), wh = shared.height.load();
          if (g_pointerLocked.load()) {
            UiEvent forward;
            forward.event = event;
            shared.PushEvent(std::move(forward));
            break;
          }
          Bar bar;
          if (event.button.button == SDL_BUTTON_LEFT && event.button.x >= ww - 16 && barOf(bar)) {
            if (event.button.y >= bar.top && event.button.y <= bar.top + bar.height) {
              barDrag = true;
              barGrab = event.button.y - bar.top;
            } else {
              scrollBy(0, event.button.y < bar.top ? -wh * 0.9 : wh * 0.9, false);
            }
            dirty = true;
            break;
          }
          UiEvent forward;
          forward.event = event;
          shared.PushEvent(std::move(forward));
          break;
        }
        case SDL_EVENT_MOUSE_BUTTON_UP: {
          if (g_pointerLocked.load()) {
            UiEvent forward;
            forward.event = event;
            shared.PushEvent(std::move(forward));
            break;
          }
          if (barDrag) {
            barDrag = false;
            dirty = true;
            break;
          }
          UiEvent forward;
          forward.event = event;
          shared.PushEvent(std::move(forward));
          break;
        }
        default: break;
      }
      any = SDL_PollEvent(&event);
    }
    for (UiCommand& command : shared.TakeCommands()) {
      switch (command.kind) {
        case UiCommand::Kind::Title: SDL_SetWindowTitle(window, command.text.c_str()); break;
        case UiCommand::Kind::ScrollBy: scrollBy(command.x, command.y, command.instant); break;
        case UiCommand::Kind::ScrollTo: scrollTo(command.x, command.y); break;
        case UiCommand::Kind::Forget: painter.Forget(); break;
        case UiCommand::Kind::Shot:
          shotPath = command.text;
          // (SOLAR_GUI_SHOTAT=y: the list the page made is drawn at that scroll, as the window does while the page is busy.)
          if (const char* at = std::getenv("SOLAR_GUI_SHOTAT")) y = targetY = std::atof(at);
          dirty = true;
          break;
        case UiCommand::Kind::Quit: running = false; break;
        case UiCommand::Kind::Clipboard: SDL_SetClipboardText(command.text.c_str()); break;
        case UiCommand::Kind::Fullscreen: SDL_SetWindowFullscreen(window, command.instant); break;
        case UiCommand::Kind::PointerLock:
          if (command.instant) {
            SDL_GetMouseState(&lockedAtX, &lockedAtY);
            SDL_SetWindowRelativeMouseMode(window, true);
          } else {
            SDL_SetWindowRelativeMouseMode(window, false);
            SDL_WarpMouseInWindow(window, lockedAtX, lockedAtY);
          }
          break;
      }
    }
    if (Frame next; shared.TakeFrame(next)) {
      frame = std::move(next);
      // (What can be scrolled to is what the page says it is now.)
      clampTargets();
      if (y > std::max(0.0, frame.maxY) || x > std::max(0.0, frame.maxX)) scrollTo(x, y);
      dirty = true;
    }
    if (gliding) {
      const auto now = Clock::now();
      const double dt = std::chrono::duration<double>(now - lastGlide).count();
      lastGlide = now;
      const double k = 1 - std::exp(-dt / 0.07);
      x += (targetX - x) * k;
      y += (targetY - y) * k;
      if (std::abs(targetX - x) < 0.5 && std::abs(targetY - y) < 0.5) {
        x = targetX;
        y = targetY;
        gliding = false;
      }
      publish();
      dirty = true;
    }
    if (!dirty && !animating) continue;
    dirty = false;

    const int pixelW = shared.pixelWidth.load(), pixelH = shared.pixelHeight.load();
    if (pixelW <= 0 || pixelH <= 0) continue;
    if (!gpu && (!surface || surfaceW != pixelW || surfaceH != pixelH)) {
      surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(pixelW, pixelH));
      if (texture) SDL_DestroyTexture(texture);
      texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, pixelW, pixelH);
      surfaceW = pixelW;
      surfaceH = pixelH;
    }
    const auto d0 = Clock::now();
    SkCanvas* canvas = gpu ? gpu->BeginFrame(pixelW, pixelH) : surface->getCanvas();
    if (!canvas) continue;
    canvas->resetMatrix();
    canvas->clear(SK_ColorWHITE);
    const float density = shared.density.load();
    canvas->scale(density, density);
    const auto rasterStart = Clock::now();
    if (frame.list) painter.Draw(*frame.list, *canvas, x, y);
    const auto rasterEnd = Clock::now();
    {
      Bar bar;
      const double idle = std::chrono::duration<double>(Clock::now() - scrollActive).count();
      const double alpha = barHover || barDrag ? 1.0 : std::clamp(1.0 - (idle - 1.0) / 0.4, 0.0, 1.0);
      const int ww = shared.width.load(), wh = shared.height.load();
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
    }
    const auto uploadStart = Clock::now();
    SkPixmap pixmap;
    SkBitmap shot;  // (the picture of the GPU's frame, for SOLAR_GUI_SHOT)
    if (gpu) {
      if (!shotPath.empty() && gpu->ReadPixels(shot)) shot.peekPixels(&pixmap);
    } else {
      surface->peekPixels(&pixmap);
      SDL_UpdateTexture(texture, nullptr, pixmap.addr(), static_cast<int>(pixmap.rowBytes()));
    }
    const auto uploadEnd = Clock::now();
    if (gpu) {
      gpu->Present();
    } else {
      SDL_RenderClear(renderer);
      SDL_RenderTexture(renderer, texture, nullptr, nullptr);
      SDL_RenderPresent(renderer);
    }
    if (std::getenv("SOLAR_GUI_TIMES")) {
      // (Where a frame goes, in ms: the page painted onto the surface by the CPU, the surface sent to the texture, and the present, which waits for vsync.)
      const auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
      static int count = 0;
      static double rasterSum = 0, rasterMax = 0, uploadSum = 0, presentSum = 0;
      const double raster = ms(rasterStart, rasterEnd);
      rasterSum += raster;
      rasterMax = std::max(rasterMax, raster);
      uploadSum += ms(uploadStart, uploadEnd);
      presentSum += ms(uploadEnd, Clock::now());
      if (++count % 30 == 0 || !shotPath.empty()) {
        const int n = count % 30 == 0 ? 30 : count % 30;
        std::fprintf(stderr, "window: draw %.1f ms (scroll %.0f); over %d frames, per frame: raster %.2f ms (worst %.2f), upload %.2f ms, present %.2f ms; %dx%d px\n", ms(d0, Clock::now()), y, n, rasterSum / n, rasterMax, uploadSum / n, presentSum / n, pixelW, pixelH);
        rasterSum = rasterMax = uploadSum = presentSum = 0;
      }
    }
    // SOLAR_GUI_SHOT=file.png: the frame is written to a file, and the program ends (for looking at what it paints without a screen).
    if (!shotPath.empty()) {
      SkDynamicMemoryWStream out;
      if (pixmap.addr() && SkPngEncoder::Encode(&out, pixmap, SkPngEncoder::Options())) {
        sk_sp<SkData> png = out.detachAsData();
        std::ofstream file(shotPath, std::ios::binary);
        file.write(static_cast<const char*>(png->data()), static_cast<std::streamsize>(png->size()));
      }
      running = false;
    }
  }
  shared.quit = true;
  shared.WakePage();
  if (texture) SDL_DestroyTexture(texture);
}

int PageMain(Shared& shared, int argc, char** argv);
}  // namespace

int main(int argc, char** argv) {
  std::setbuf(stdout, nullptr);
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  solar::gui::InstallSdlAudio();
  // (SOLAR_GUI_SIZE=2560x1600: the window made this size, for timing what a big one costs.)
  int windowW = 1280, windowH = 800;
  if (const char* given = std::getenv("SOLAR_GUI_SIZE")) std::sscanf(given, "%dx%d", &windowW, &windowH);
  // The window is drawn on by the GPU where there is one to be had (SOLAR_GUI_CPU=1: not); where there is none, by the CPU, into a texture SDL shows.
  SDL_Window* window = nullptr;
  SDL_Renderer* renderer = nullptr;
  std::unique_ptr<solar::gui::GpuSurface> gpu;
  if (!std::getenv("SOLAR_GUI_CPU")) {
    for (const solar::gui::GpuBackend backend : solar::gui::GpuSurface::Backends()) {
      solar::gui::GpuSurface::Prepare(backend);
      window = SDL_CreateWindow("Solar", windowW, windowH, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | solar::gui::GpuSurface::WindowFlags(backend));
      gpu = solar::gui::GpuSurface::Create(backend, window);
      if (gpu) break;
      if (std::getenv("SOLAR_GUI_TIMES")) std::fprintf(stderr, "gpu: %s not used (%s)\n", backend == solar::gui::GpuBackend::Vulkan ? "Vulkan" : "OpenGL", SDL_GetError());
      if (window) {
        SDL_DestroyWindow(window);
        window = nullptr;
      }
    }
  }
  if (gpu) {
    if (std::getenv("SOLAR_GUI_TIMES")) std::fprintf(stderr, "gpu: %s through %s\n", gpu->Renderer(), gpu->Interface());
  } else {
    window = SDL_CreateWindow("Solar", windowW, windowH, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
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
  }
  SDL_StartTextInput(window);
  Shared shared;
  {
    int w = 0, h = 0, pw = 0, ph = 0;
    SDL_GetWindowSize(window, &w, &h);
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    shared.width = w;
    shared.height = h;
    shared.pixelWidth = pw;
    shared.pixelHeight = ph;
    shared.density = SDL_GetWindowPixelDensity(window);
    // (SOLAR_GUI_DENSITY=2: as on a high-density screen, for timing what that costs without one.)
    if (const char* forced = gpu ? nullptr : std::getenv("SOLAR_GUI_DENSITY")) {
      const float d = static_cast<float>(std::atof(forced));
      if (d >= 1) {
        shared.density = d;
        shared.pixelWidth = static_cast<int>(w * d);
        shared.pixelHeight = static_cast<int>(h * d);
      }
    }
  }
  int result = 0;
  // The page's thread has a large stack of its own (only what is used is ever given memory): scripts of a few hundred kilobytes nest deeper than the
  // 8 MB a thread gets by default, and the parser keeps to half of the stack it finds.
  pthread_t page;
  {
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, static_cast<size_t>(256) << 20);
    auto* start = new std::function<void()>([&] {
      solar::css::SetLoadThrottle(true);
      result = PageMain(shared, argc, argv);
      shared.pageDone = true;
      shared.quit = true;
      shared.WakeWindow();
    });
    if (pthread_create(&page, &attributes, [](void* arg) -> void* {
          auto* body = static_cast<std::function<void()>*>(arg);
          (*body)();
          delete body;
          return nullptr;
        }, start) != 0) {
      std::fprintf(stderr, "cannot start the page thread\n");
      return 1;
    }
    pthread_attr_destroy(&attributes);
  }
  RunWindow(shared, window, renderer, gpu.get());
  // The page is asked to stop; one that is in the middle of a long script is not waited for for long.
  for (int i = 0; i < 100 && !shared.pageDone.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if (!shared.pageDone.load()) {
    std::_Exit(0);
  }
  pthread_join(page, nullptr);
  gpu.reset();
  if (renderer) SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return result;
}

namespace {

int PageMain(Shared& shared, int argc, char** argv) {
  const auto programStart = Clock::now();
  const std::string given = argc > 1 ? argv[1] : "https://example.com/";
  std::string address = AddressOf(given);
  // The pages this window went through, and where in them it is (back and forward move along them); a page left by going back or forward is not added again.
  std::vector<std::string> session;
  size_t sessionAt = 0;
  bool historyMove = false;

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
  // The network: a service on a thread of its own, which tells this loop (the loop of the page) what happens; SOLAR_NET_INLINE=1: the client on this loop, as it was.
  std::unique_ptr<solar::net::HttpClient> inlineClient;
  std::unique_ptr<solar::net::ThreadedNetworkService> threadedNetwork;
  if (std::getenv("SOLAR_NET_INLINE")) {
    inlineClient = std::make_unique<solar::net::HttpClient>(*loop);
  } else {
    threadedNetwork = std::make_unique<solar::net::ThreadedNetworkService>(*loop);
  }
  solar::net::BrowserNetwork& client = inlineClient ? static_cast<solar::net::BrowserNetwork&>(*inlineClient) : static_cast<solar::net::BrowserNetwork&>(*threadedNetwork);
  solar::web::TimerHost timers;
  isolate->SetTimerProvider(timers.Provider());
  solar::web::JsEventLoop events(*loop, {[&] { isolate->PerformMicrotaskCheckpoint(); }, [&] { timers.RunDue(); }, [&] { return timers.NextDelayMs(); }});
  Browser browser(*isolate, *loop, client, events);
  browser.timing_ = std::getenv("SOLAR_GUI_TIMES") != nullptr;
  browser.dropTimers_ = [&](qe::Realm* realm) { timers.DropRealm(realm); };
  solar::layout::SetImageRequestHandler([&](const std::string& absolute) { return browser.RequestImage(absolute); });
  solar::html::SetFrameEnvironment(&browser);

  // The size the page is laid out for is the window's, in CSS pixels.
  const auto setViewport = [&] {
    const int w = shared.width.load(), h = shared.height.load();
    solar::css::MediaEnvironment environment = solar::css::CurrentMediaEnvironment();
    environment.width = environment.deviceWidth = w;
    environment.height = environment.deviceHeight = h;
    environment.resolution = shared.density.load();
    solar::css::SetCurrentMediaEnvironment(environment);
  };
  setViewport();
  solar::css::SetOverlayScrollbars(true);

  // Puts the document that came for `where` in a new realm, and runs its scripts (what it fetches is waited for, with the window kept alive).
  qe::Realm* realm = nullptr;
  solar::dom::Document* document = nullptr;
  bool dirty = true;
  bool loadingPage = false, firstEarly = true;  // (see paintEarly)
  Clock::time_point nextEarly = Clock::now();
  double builtX = 0, builtY = 0;  // the scroll the list the window has was made at
  solar::dom::Element* pressTarget = nullptr;
  int pressButton = 0;
  const bool timing_log = std::getenv("SOLAR_GUI_TIMES") != nullptr;
  // `stream`: the page is parsed as it comes from this fetch (`markup` is what came first), not once all of it is here.
  const auto commit = [&](const std::string& where, std::string markup, std::string type, const Clock::time_point& started, Collector* stream = nullptr) {
    // (SOLAR_INJECT_HEAD=file: the text of the file is put right after <head> of each page that comes, for looking into what a page's scripts do.)
    if (const char* inject = std::getenv("SOLAR_INJECT_HEAD")) {
      std::ifstream in(inject, std::ios::binary);
      const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      const size_t at = markup.find("<head");
      const size_t close = at == std::string::npos ? at : markup.find('>', at);
      if (close != std::string::npos) markup.insert(close + 1, text);
    }
    // (SOLAR_DUMP_DOC=dir: the text of each page that comes is written to dir/N.html, for looking at what a site sent.)
    if (const char* dump = std::getenv("SOLAR_DUMP_DOC")) {
      static int dumped = 0;
      std::ofstream out(std::string(dump) + "/" + std::to_string(dumped++) + ".html");
      out << "<!-- " << where << " -->\n" << markup;
    }
    qe::Realm* left = realm;
    solar::dom::Document* leftDocument = document;
    pressTarget = nullptr;
    address = where;
    if (!historyMove && (session.empty() || session[sessionAt] != where)) {
      if (!session.empty()) session.resize(sessionAt + 1);
      session.push_back(where);
      sessionAt = session.size() - 1;
    }
    historyMove = false;
    // (The drop-down of the page that was left is gone with it.)
    solar::paint::SetPopup(solar::paint::Popup{});
    browser.SetPageUrl(address);
    browser.SetContentType(address, type);
    {
      const auto parsedAddress = solar::url::Parse(address);
      const bool hasWorker = parsedAddress && solar::webapi::HasServiceWorkerFor(solar::url::SerializeOrigin(*parsedAddress), address);
      if (stream) {
        browser.BeginStreamScan();
        browser.StreamScan(markup, address, false);
      } else if (!hasWorker) {
        browser.Prefetch(markup, address);
      }
    }
    realm = browser.CreateRealm();
    solar::html::RunInRealm(*realm, [&] {
      document = solar::dom::NewDocument(realm->GetContext(), true);
      document->url = address;
      solar::html::InstallWindow(*realm, document);
    });
    const auto loadStart = Clock::now();
    if (timing_log) std::fprintf(stderr, "[%.0f ms] LoadPage begins\n", std::chrono::duration<double, std::milli>(loadStart.time_since_epoch()).count());
    loadingPage = true;
    firstEarly = true;
    nextEarly = Clock::now() + std::chrono::milliseconds(150);
    if (stream) {
      // (What came is given to the parser a piece at a time, as it asks for more; the scanner looks ahead of it at what is in each piece.)
      size_t taken = 0;  // (the first piece is in the body already: the parser gets it with the rest, from the start)
      solar::html::LoadPageStreaming(*realm, document, [&, taken](std::string& out) mutable -> bool {
        browser.WaitForDocument(*stream, taken);
        out.assign(stream->body, taken, std::string::npos);
        taken = stream->body.size();
        browser.StreamScan(stream->body, address, stream->done);
        if (stream->failed) std::fprintf(stderr, "fetch failed: %s\n", stream->error.c_str());
        return !stream->done;
      });
    } else {
      solar::html::LoadPage(*realm, document, markup, type);
    }
    if (std::getenv("SOLAR_DOC_SIZE")) {
      solar::html::RunInRealm(*realm, [&] { realm->Evaluate("console.log('doc elements ' + document.querySelectorAll('*').length + ' html ' + document.documentElement.outerHTML.length + ' links ' + document.querySelectorAll('link[rel=stylesheet]').length + ' hash ' + (document.documentElement.outerHTML.length < 200000 ? Array.from(document.documentElement.outerHTML).reduce((h, c) => (h * 31 + c.codePointAt(0)) | 0, 7) : 0) + ' title ' + document.title + ' bodychildren ' + document.body.children.length + ' head ' + document.head.children.length + ' htmlkids ' + Array.from(document.documentElement.children).map(e => e.localName).join(',') + ' firstbody ' + Array.from(document.body.children).slice(0, 12).map(e => e.localName + (e.id ? '#' + e.id : '')).join(','))", "size"); });
    }
    if (timing_log) std::fprintf(stderr, "[%.0f ms] LoadPage ends\n", std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count());
    if (timing_log) std::fprintf(stderr, "  LoadPage took %.0f ms (%.0f ms of it waiting; scripts %.0f ms, style sheets parsed %.0f ms, layout %.0f ms)\n", std::chrono::duration<double, std::milli>(Clock::now() - loadStart).count(), browser.waited_, solar::html::ScriptMillis(), solar::css::SheetParseMillis(), solar::layout::LayoutMillis());
    const auto stepStart = Clock::now();
    auto lap = [&, last = stepStart](const char* what) mutable {
      const auto now = Clock::now();
      if (timing_log) std::fprintf(stderr, "    after load: %-28s %7.0f ms\n", what, std::chrono::duration<double, std::milli>(now - last).count());
      last = now;
    };
    isolate->PerformMicrotaskCheckpoint();
    lap("microtasks");
    const auto settle = Clock::now() + std::chrono::milliseconds(30);
    while (Clock::now() < settle) events.Step(std::chrono::milliseconds(8));
    lap("settle");
    browser.TakeArrived();
    browser.TakeSizesArrived();
    if (left) {
      // The page that was left: its timers stop, what hangs from its document is let go of, its realm goes, and what is no more used is collected.
      timers.DropRealm(left);
      lap("timers dropped");
      if (leftDocument) solar::dom::DisposeDocument(leftDocument);
      lap("document disposed");
      if (leftDocument) solar::css::ForgetDocument(leftDocument);
      lap("style forgotten");
      browser.ReleaseRealm(left);
      lap("realm released");
      solar::layout::ForgetImages();
      shared.Post({UiCommand::Kind::Forget});
      browser.DropWorkerResources();
      browser.TrimResources(48u << 20);
      lap("resources trimmed");
      isolate->CollectGarbage();
      lap("garbage collected");
    }
    std::fprintf(stderr, "loaded in %.0f ms\n", std::chrono::duration<double, std::milli>(Clock::now() - started).count());
    loadingPage = false;
    {
      UiCommand title;
      title.kind = UiCommand::Kind::Title;
      title.text = "Solar - " + address;
      shared.Post(std::move(title));
      // (A page that came is at the top; the window's scroll is put there.)
      UiCommand top;
      top.kind = UiCommand::Kind::ScrollTo;
      shared.Post(std::move(top));
    }
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
    if (!browser.TryWorkerNavigation(*parsed, where, options, navigation->collector)) client.Fetch(*parsed, navigation->collector, options);
    current = navigation.get();
    navigations.push_back(std::move(navigation));
    {
      UiCommand title;
      title.kind = UiCommand::Kind::Title;
      title.text = "Loading " + where;
      shared.Post(std::move(title));
    }
    return true;
  };
  // (The window is on a thread of its own: it keeps answering while the page waits.)
  browser.pointerLock_ = [&](bool on) {
    g_pointerLocked = on;
    UiCommand command;
    command.kind = UiCommand::Kind::PointerLock;
    command.instant = on;
    shared.Post(std::move(command));
  };
  browser.fullscreen_ = [&](bool on) {
    UiCommand command;
    command.kind = UiCommand::Kind::Fullscreen;
    command.instant = on;
    shared.Post(std::move(command));
  };
  browser.pump_ = [&] {
    if (shared.quit.load()) std::_Exit(0);
  };
  // The document that came: it is put in a realm, unless a newer address was asked for since.
  // A page of HTML in UTF-8 that is coming is parsed as it comes (not for a page that is changed on its way, or whose encoding is not known from its headers).
  const auto streamable = [&](const Collector& c) {
    static const bool off = std::getenv("SOLAR_NO_STREAM") != nullptr || std::getenv("SOLAR_INJECT_HEAD") != nullptr || std::getenv("SOLAR_DUMP_DOC") != nullptr;
    if (off || c.done || c.failed || c.status != 200 || c.body.empty()) return false;
    std::string type = c.contentType;
    for (char& ch : type) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (type.find("text/html") == std::string::npos) return false;
    return type.find("charset=utf-8") != std::string::npos || type.find("charset=\"utf-8\"") != std::string::npos;
  };
  const auto settleNavigation = [&] {
    if (!current) return;
    if (!current->collector.done && !streamable(current->collector)) return;
    Navigation* arrived = current;
    current = nullptr;
    Collector& collector = arrived->collector;
    if (collector.failed) {
      std::fprintf(stderr, "fetch failed: %s\n", collector.error.c_str());
      UiCommand title;
      title.kind = UiCommand::Kind::Title;
      title.text = "Solar - " + address;
      shared.Post(std::move(title));
      return;
    }
    if (!collector.done) {
      // It is not all here: the page is parsed as the rest comes.
      collector.streaming = true;
      if (std::getenv("SOLAR_GUI_TIMES")) std::fprintf(stderr, "  document begins at %.0f ms (%zu bytes so far)\n", std::chrono::duration<double, std::milli>(Clock::now() - arrived->started).count(), collector.body.size());
      const std::string first = collector.body;
      commit(collector.address.empty() ? arrived->where : collector.address, first, "text/html", arrived->started, &collector);
      return;
    }
    if (std::getenv("SOLAR_GUI_TIMES")) std::fprintf(stderr, "  document fetched at %.0f ms (%zu bytes)\n", std::chrono::duration<double, std::milli>(Clock::now() - arrived->started).count(), collector.body.size());
    const std::string type = collector.contentType.find("xml") != std::string::npos ? "application/xhtml+xml" : "text/html";
    commit(collector.address.empty() ? arrived->where : collector.address, std::move(collector.body), type, arrived->started);
  };
  // (What the form controls show is asked of the page's script before the frame, on the thread the script is on; the frame reads it as it was committed.)
  const auto currentTree = [&]() -> solar::layout::Tree* {
    solar::html::RunInRealm(*realm, [&] { solar::html::RefreshControls(document); });
    solar::html::RefreshFrameControls(document);
    return solar::paint::UpdateLayoutWithFrames(realm->GetContext(), document);
  };
  // The frames of a page that have more to scroll than their room, where they are and where they are scrolled to.
  // How far the user can scroll an element with the wheel: what its overflow lets be scrolled (hidden is for script), 0 for what is not a scroller.
  const auto userScrollRange = [&](solar::layout::Tree& tree, solar::dom::Element* element, double& maxX, double& maxY) {
    maxX = maxY = 0;
    const solar::layout::Box* scroller = nullptr;
    for (const solar::layout::Box* b : solar::layout::BoxesOf(tree, element)) {
      if (b->kind == solar::layout::Box::Kind::Block) { scroller = b; break; }
    }
    if (!scroller || scroller->formControl || !scroller->style) return false;
    const solar::layout::BoxStyle& p = scroller->style->Physical();
    const auto wheels = [](solar::layout::Overflow o) { return o == solar::layout::Overflow::Auto || o == solar::layout::Overflow::Scroll; };
    if (!wheels(p.overflowX) && !wheels(p.overflowY)) return false;
    solar::layout::ScrollRange(tree, element, maxX, maxY);
    if (!wheels(p.overflowX)) maxX = 0;
    if (!wheels(p.overflowY)) maxY = 0;
    return maxX > 0 || maxY > 0;
  };
  const auto scrollRegions = [&](solar::layout::Tree& top) {
    std::vector<ScrollRegion> out;
    std::function<void(solar::layout::Tree&, const solar::layout::Box&, double, double, bool, int)> walk;
    walk = [&](solar::layout::Tree& tree, const solar::layout::Box& box, double originX, double originY, bool fixedAbove, int depth) {
      if (box.replaced && box.kind == solar::layout::Box::Kind::Block && box.element() && box.element()->IsHtml("iframe")) {
        solar::dom::Document* inner = box.element()->contentDocument;
        if (inner && inner->frameElement == box.element() && inner->layoutTree && inner->layoutTree->root && depth < 3) {
          solar::layout::Rect rect = solar::layout::AbsoluteBorderBox(box);
          bool fixed = fixedAbove || box.style->position == solar::layout::Position::Fixed;
          for (const solar::layout::Box* q = box.parent; q; q = q->parent) {
            if (q->style && q->style->position == solar::layout::Position::Fixed) fixed = true;
            if (!q->node) continue;
            const auto found = tree.scroll.find(q->node);
            if (found != tree.scroll.end()) { rect.x -= found->second.first; rect.y -= found->second.second; }
          }
          ScrollRegion region;
          region.x = originX + rect.x + box.border.left + box.padding.left;
          region.y = originY + rect.y + box.border.top + box.padding.top;
          region.width = rect.width - box.border.Horizontal() - box.padding.Horizontal();
          region.height = rect.height - box.border.Vertical() - box.padding.Vertical();
          region.fixed = fixed;
          solar::layout::ScrollRange(*inner->layoutTree, nullptr, region.maxX, region.maxY);
          const auto at = inner->layoutTree->scroll.find(inner);
          if (at != inner->layoutTree->scroll.end()) { region.curX = at->second.first; region.curY = at->second.second; }
          if (region.maxX > 0 || region.maxY > 0) out.push_back(region);
          walk(*inner->layoutTree, *inner->layoutTree->root, region.x - region.curX, region.y - region.curY, fixed, depth + 1);
          return;
        }
      }
      // A box with an overflow that scrolls and more than its room.
      if (box.kind == solar::layout::Box::Kind::Block && box.element() && box.parent) {
        solar::dom::Element* element = box.element();
        double maxX = 0, maxY = 0;
        const bool viewport = element == element->nodeDocument->DocumentElement() || element->IsHtml("body");
        if (!viewport && userScrollRange(tree, element, maxX, maxY)) {
          solar::layout::Rect rect = solar::layout::AbsoluteBorderBox(box);
          bool fixed = fixedAbove || box.style->position == solar::layout::Position::Fixed;
          for (const solar::layout::Box* q = box.parent; q; q = q->parent) {
            if (q->style && q->style->position == solar::layout::Position::Fixed) fixed = true;
            if (!q->node) continue;
            const auto found = tree.scroll.find(q->node);
            if (found != tree.scroll.end()) { rect.x -= found->second.first; rect.y -= found->second.second; }
          }
          ScrollRegion region;
          region.x = originX + rect.x + box.border.left;
          region.y = originY + rect.y + box.border.top;
          region.width = rect.width - box.border.Horizontal();
          region.height = rect.height - box.border.Vertical();
          region.fixed = fixed;
          region.maxX = maxX;
          region.maxY = maxY;
          const auto at = tree.scroll.find(element);
          if (at != tree.scroll.end()) { region.curX = at->second.first; region.curY = at->second.second; }
          out.push_back(region);
        }
      }
      for (const auto& child : box.children) walk(tree, *child, originX, originY, fixedAbove, depth);
    };
    if (top.root) walk(top, *top.root, 0, 0, false, 0);
    return out;
  };
  // The page is shown while it is still being loaded (what the parser has made, once a script or a picture is waited for): a page that takes seconds to be
  // loaded has its first part drawn in the first one. How often is held to what a frame costs, so that showing it does not slow the loading.
  const auto paintEarly = [&] {
    static const bool never = std::getenv("SOLAR_NO_EARLY") != nullptr;
    if (never || !loadingPage || !document || !realm || Clock::now() < nextEarly) return;
    // Nothing is drawn before the body has something in it.
    solar::dom::Element* root = document->DocumentElement();
    solar::dom::Element* body = nullptr;
    if (root) {
      for (solar::dom::Node* c = root->firstChild; c; c = c->nextSibling) {
        if (c->IsElement() && static_cast<solar::dom::Element*>(c)->IsHtml("body")) body = static_cast<solar::dom::Element*>(c);
      }
    }
    if (!body || !body->firstChild) return;
    const auto t0 = Clock::now();
    // The first look at the page is of what is first in it: the rest is laid out for the frames that come after (a page of thousands of elements is not waited for).
    if (firstEarly && !std::getenv("SOLAR_NO_PREVIEW")) solar::layout::SetPreviewBudget(1200);
    solar::layout::Tree* tree = currentTree();
    solar::layout::SetPreviewBudget(0);
    if (!tree) return;
    browser.painting_ = true;
    auto list = std::make_shared<const solar::paint::DisplayList>(solar::paint::BuildDisplayList(realm->GetContext(), *tree, shared.height.load()));
    browser.painting_ = false;
    builtX = list->scrollX;
    builtY = list->scrollY;
    Frame frame;
    frame.list = list;
    solar::layout::ScrollRange(*tree, nullptr, frame.maxX, frame.maxY);
    frame.regions = scrollRegions(*tree);
    shared.PostFrame(std::move(frame));
    if (firstEarly) {
      firstEarly = false;
      UiCommand top;
      top.kind = UiCommand::Kind::ScrollTo;
      shared.Post(std::move(top));
      UiCommand title;
      title.kind = UiCommand::Kind::Title;
      title.text = "Solar - " + address;
      shared.Post(std::move(title));
    }
    const double cost = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    if (timing_log) std::fprintf(stderr, "  early frame while loading: %.0f ms (at %.0f ms of the program)\n", cost, std::chrono::duration<double, std::milli>(Clock::now() - programStart).count());
    nextEarly = Clock::now() + std::chrono::milliseconds(static_cast<long>(100 + 4 * cost));
  };
  browser.idle_ = paintEarly;
  timers.afterTask = paintEarly;
  if (!navigate(address)) return 1;
  // (The first page is waited for, as a program that starts with one.)
  while (current && !current->collector.done && !streamable(current->collector)) {
    events.Step(std::chrono::milliseconds(8));
    browser.pump_();
  }
  settleNavigation();
  if (!document) return 1;

  Clock::time_point lastRelayout = Clock::now();
  uint64_t lastVersion = 0;
  const bool timing = std::getenv("SOLAR_GUI_TIMES") != nullptr;

  // A link: the address it has, against the page's (the nearest one at or above `from`).
  const auto followLink = [&](solar::dom::Element* from) {
    for (solar::dom::Node* n = from; n; n = n->parentNode) {
      if (!n->IsElement()) continue;
      solar::dom::Element* element = static_cast<solar::dom::Element*>(n);
      if (!element->IsHtml("a") && !element->IsHtml("area")) continue;
      if (const solar::dom::Attr* href = element->FindAttribute("", "href")) {
        std::optional<solar::url::Url> base = solar::url::Parse(address);
        std::optional<solar::url::Url> target = base ? solar::url::Parse(href->value, &*base) : solar::url::Parse(href->value);
        if (target && (target->scheme == "http" || target->scheme == "https" || target->scheme == "file")) navigate(solar::url::Serialize(*target));
      }
      break;
    }
  };
  // Back (-1) and forward (1): along the entries a script made with pushState if there are any that way, else along the pages of the window.
  const auto goInHistory = [&](int delta) {
    bool handled = false;
    solar::html::RunInRealm(*realm, [&] { handled = solar::html::HistoryStep(document, delta); });
    if (handled) {
      dirty = true;
      return;
    }
    const long target = static_cast<long>(sessionAt) + delta;
    if (target < 0 || target >= static_cast<long>(session.size())) return;
    sessionAt = static_cast<size_t>(target);
    historyMove = true;
    if (!navigate(session[sessionAt])) historyMove = false;
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
    static const char* const kText[] = {"", "text", "search", "tel", "url", "email", "password", "number", "date", "time", "month", "week", "datetime-local"};
    for (const char* k : kText) if (t == k) return focused;
    return nullptr;
  };

  // ---- Editing, lists and the drop-down ----
  constexpr size_t kNone = static_cast<size_t>(-1);
  solar::dom::Element* popupSelect = nullptr;  // the select whose drop-down is open
  const void* popupDocument = nullptr;
  size_t popupHover = kNone, popupTop = 0;
  solar::dom::Element* dragField = nullptr;  // the text control a drag of the pointer is selecting in, and where the drag began (UTF-16 units)
  size_t dragAnchor = 0;
  solar::dom::Element* anchorField = nullptr;  // the control the selection of the keyboard grows from, and where that is
  size_t keyAnchor = 0;
  solar::dom::Element* dragRange = nullptr;  // the range input the thumb of which is being dragged
  bool swallowRelease = false;
  int pressCount = 0;
  Clock::time_point lastPress = Clock::now() - std::chrono::seconds(10);
  double lastPressX = 0, lastPressY = 0;
  bool caretOn = true;
  Clock::time_point caretSince = Clock::now();
  const auto caretReset = [&] {
    caretOn = true;
    caretSince = Clock::now();
    solar::paint::SetCaretVisible(true);
  };
  // What a control shows now (the state script keeps, asked for first if it moved).
  const auto readControl = [&](solar::dom::Element* e, solar::html::ControlView& v) {
    currentTree();
    return solar::html::ReadControl(e, v);
  };
  const auto showPopup = [&] {
    solar::paint::Popup popup;
    popup.select = popupSelect;
    popup.hover = popupHover;
    popup.top = popupTop;
    solar::paint::SetPopup(popup);
    dirty = true;
  };
  const auto closePopup = [&] {
    if (!popupSelect) return;
    popupSelect = nullptr;
    solar::paint::SetPopup(solar::paint::Popup{});
    dirty = true;
  };
  // The option a row of a list is (headings of groups are rows, not options).
  const auto optionOfRow = [&](const solar::html::ControlView& v, size_t row) -> size_t {
    if (row >= v.options.size() || v.options[row].group) return kNone;
    size_t n = 0;
    for (size_t i = 0; i < row; ++i) {
      if (!v.options[i].group) ++n;
    }
    return n;
  };
  const auto selectedRow = [&](const solar::html::ControlView& v) -> size_t {
    for (size_t i = 0; i < v.options.size(); ++i) {
      if (v.options[i].selected) return i;
    }
    return kNone;
  };
  const auto pickable = [&](const solar::html::ControlView& v, size_t row) { return row < v.options.size() && !v.options[row].group && !v.options[row].disabled; };
  const auto openPopup = [&](solar::dom::Element* select) {
    solar::html::ControlView v;
    if (!readControl(select, v) || v.disabled || v.options.empty()) return;
    popupSelect = select;
    popupDocument = document;
    popupHover = selectedRow(v);
    popupTop = popupHover != kNone && popupHover >= 20 ? popupHover - 19 : 0;
    showPopup();
  };
  const auto chooseRow = [&](size_t row) {
    solar::html::ControlView v;
    if (!popupSelect || !readControl(popupSelect, v) || !pickable(v, row)) return;
    solar::dom::Element* select = popupSelect;
    const size_t option = optionOfRow(v, row);
    closePopup();
    solar::html::RunInRealm(*realm, [&] { solar::html::ChooseOption(document, select, option, 0); });
    dirty = true;
  };
  // The row of the open drop-down under a point of the window (kNone outside it); `inside` says whether the point is on the list.
  const auto popupRowAt = [&](double x, double y, bool& inside) -> size_t {
    inside = false;
    solar::layout::Tree* tree = currentTree();
    solar::paint::PopupGeometry g;
    if (!tree || !popupSelect || !solar::paint::PopupGeometryOf(realm->GetContext(), *tree, popupSelect, popupTop, g)) return kNone;
    const auto& scroll = tree->scroll[document];
    const double dx = x + scroll.first, dy = y + scroll.second;
    if (dx < g.x || dx >= g.x + g.width || dy < g.y || dy >= g.y + g.height) return kNone;
    inside = true;
    return popupTop + static_cast<size_t>((dy - g.y) / g.rowHeight);
  };
  // Moves what the drop-down's keys are on to the next row that can be chosen (`toEnd`: the last one in that direction).
  const auto movePopup = [&](int direction, bool toEnd) {
    solar::html::ControlView v;
    if (!popupSelect || !readControl(popupSelect, v)) return;
    long row = popupHover == kNone ? (direction > 0 ? -1 : static_cast<long>(v.options.size())) : static_cast<long>(popupHover);
    if (toEnd) row = direction > 0 ? static_cast<long>(v.options.size()) : -1;
    for (;;) {
      row += direction;
      if (row < 0 || row >= static_cast<long>(v.options.size())) break;
      if (pickable(v, static_cast<size_t>(row))) {
        popupHover = static_cast<size_t>(row);
        if (popupHover < popupTop) popupTop = popupHover;
        if (popupHover >= popupTop + 20) popupTop = popupHover - 19;
        showPopup();
        return;
      }
    }
  };
  // The row after `from` (round the list) whose label begins with what was typed; kNone if none does.
  const auto typeAhead = [&](const solar::html::ControlView& v, size_t from, const std::string& typed) -> size_t {
    if (typed.empty() || v.options.empty()) return kNone;
    std::string lowered = typed;
    for (char& c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (size_t step = 1; step <= v.options.size(); ++step) {
      const size_t row = ((from == kNone ? v.options.size() - 1 : from) + step) % v.options.size();
      if (!pickable(v, row)) continue;
      std::string label = v.options[row].label;
      for (char& c : label) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (label.rfind(lowered, 0) == 0) return row;
    }
    return kNone;
  };
  // The key of a select that is not open (a drop-down changes with the arrows, and opens with space; a list moves its choice).
  const auto selectKey = [&](solar::dom::Element* select, const SDL_KeyboardEvent& k) -> bool {
    solar::html::ControlView v;
    if (!readControl(select, v) || v.disabled || v.options.empty()) return false;
    const bool list = v.rows > 1 || v.multiple;
    const bool shift = (k.mod & SDL_KMOD_SHIFT) != 0, ctrl = (k.mod & SDL_KMOD_CTRL) != 0;
    const size_t current = selectedRow(v);
    if (!list && (k.key == SDLK_SPACE || k.key == SDLK_RETURN || k.key == SDLK_KP_ENTER || k.key == SDLK_F4 || ((k.mod & SDL_KMOD_ALT) && (k.key == SDLK_DOWN || k.key == SDLK_UP)))) {
      openPopup(select);
      return true;
    }
    if (list && v.multiple && k.key == SDLK_SPACE && current != kNone) {
      solar::html::RunInRealm(*realm, [&] { solar::html::ChooseOption(document, select, optionOfRow(v, current), 1); });
      dirty = true;
      return true;
    }
    int direction = 0;
    bool toEnd = false;
    size_t jump = 1;
    switch (k.key) {
      case SDLK_UP: case SDLK_LEFT: direction = -1; break;
      case SDLK_DOWN: case SDLK_RIGHT: direction = 1; break;
      case SDLK_HOME: direction = 1; toEnd = true; break;
      case SDLK_END: direction = -1; toEnd = true; break;
      case SDLK_PAGEUP: direction = -1; jump = std::max<size_t>(1, v.rows - 1); break;
      case SDLK_PAGEDOWN: direction = 1; jump = std::max<size_t>(1, v.rows - 1); break;
      default: return false;
    }
    long row = current == kNone ? (direction > 0 ? -1 : static_cast<long>(v.options.size())) : static_cast<long>(current);
    long found = -1;
    if (toEnd) {
      // (Home is the first row that can be chosen, End the last.)
      for (long r = direction > 0 ? 0 : static_cast<long>(v.options.size()) - 1; r >= 0 && r < static_cast<long>(v.options.size()); r += direction) {
        if (pickable(v, static_cast<size_t>(r))) { found = r; break; }
      }
    } else {
      for (size_t n = 0; n < jump; ++n) {
        for (long r = row + direction; r >= 0 && r < static_cast<long>(v.options.size()); r += direction) {
          if (pickable(v, static_cast<size_t>(r))) { found = r; row = r; break; }
        }
      }
    }
    if (found < 0) return true;
    solar::html::RunInRealm(*realm, [&] {
      solar::html::ChooseOption(document, select, optionOfRow(v, static_cast<size_t>(found)), list && v.multiple && shift && !ctrl ? 2 : 0);
      if (list) {
        size_t top = v.top;
        if (static_cast<size_t>(found) < top) top = static_cast<size_t>(found);
        if (static_cast<size_t>(found) >= top + v.rows) top = static_cast<size_t>(found) - v.rows + 1;
        if (top != v.top) solar::html::SetListTop(document, select, top);
      }
    });
    dirty = true;
    return true;
  };
  // The keys while the drop-down is open.
  const auto popupKey = [&](SDL_Keycode key) {
    switch (key) {
      case SDLK_ESCAPE: closePopup(); break;
      case SDLK_UP: movePopup(-1, false); break;
      case SDLK_DOWN: movePopup(1, false); break;
      case SDLK_HOME: movePopup(1, true); break;
      case SDLK_END: movePopup(-1, true); break;
      case SDLK_PAGEUP: for (int i = 0; i < 10; ++i) movePopup(-1, false); break;
      case SDLK_PAGEDOWN: for (int i = 0; i < 10; ++i) movePopup(1, false); break;
      case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
        if (popupHover != kNone) chooseRow(popupHover);
        else closePopup();
        break;
      case SDLK_TAB: closePopup(); break;
      default: break;
    }
  };

  // Where in the text of a control (UTF-16 units) a point of the window is; for a text area, in the lines it shows.
  struct AreaView {
    std::vector<solar::layout::TextLine> lines;
    double rowHeight = 1, contentLeft = 0, contentTop = 0, contentWidth = 0, size = 0;
    size_t first = 0, visible = 1;
    std::shared_ptr<solar::font::Face> face;
  };
  const auto viewOf = [&](solar::dom::Element* el, const solar::html::ControlView& v, AreaView& out) -> bool {
    solar::layout::Tree* tree = currentTree();
    if (!tree) return false;
    const auto boxes = tree->boxesOf.find(el);
    if (boxes == tree->boxesOf.end() || boxes->second.empty() || !boxes->second.front()->style) return false;
    const solar::layout::Box& box = *boxes->second.front();
    out.face = solar::layout::FaceOf(realm->GetContext(), *tree, box);
    if (!out.face) return false;
    const solar::layout::Rect where = solar::layout::AbsoluteBorderBox(box);
    const auto& scroll = tree->scroll[document];
    out.size = box.style->fontSize;
    out.contentLeft = where.x - scroll.first + box.ContentLeft();
    out.contentTop = where.y - scroll.second + box.ContentTop();
    out.contentWidth = std::max(0.0, where.width - box.border.Horizontal() - box.padding.Horizontal());
    const double contentHeight = std::max(0.0, where.height - box.border.Vertical() - box.padding.Vertical());
    out.rowHeight = std::max(1.0, std::round(out.face->metrics().ascent * out.size) + std::round(out.face->metrics().descent * out.size));
    if (v.kind == "textarea") {
      out.lines = solar::layout::WrapText(*out.face, out.size, v.value, std::max(1.0, out.contentWidth - 15.0));
      out.visible = std::max<size_t>(1, static_cast<size_t>(contentHeight / out.rowHeight));
      const size_t caretByte = solar::layout::BytesBeforeUnit(v.value, v.end);
      size_t caretLine = 0;
      for (size_t i = 0; i < out.lines.size(); ++i) {
        if (caretByte >= out.lines[i].begin) caretLine = i;
      }
      out.first = document->focusedElement == el && caretLine >= out.visible ? caretLine - out.visible + 1 : 0;
    } else {
      out.lines = {solar::layout::TextLine{0, v.value.size()}};
    }
    return true;
  };
  // The byte of a line whose left edge is nearest to `x` (measured from the line's own start).
  const auto nearestByte = [&](const AreaView& a, const std::string& text, const solar::layout::TextLine& line, double x) -> size_t {
    size_t best = line.begin, at = line.begin;
    double bestDistance = std::fabs(x);
    while (at < line.end) {
      at = CharAfter(text, at);
      const double pen = a.face->Measure(text.substr(line.begin, at - line.begin)) * a.size;
      if (std::fabs(x - pen) < bestDistance) {
        bestDistance = std::fabs(x - pen);
        best = at;
      }
    }
    return best;
  };
  const auto textPositionAt = [&](solar::dom::Element* el, double mx, double my) -> size_t {
    solar::html::ControlView v;
    AreaView a;
    if (!readControl(el, v) || !viewOf(el, v, a)) return 0;
    if (v.kind == "textarea") {
      const double rows = std::floor((my - a.contentTop) / a.rowHeight);
      const size_t row = std::min(a.lines.size() - 1, static_cast<size_t>(std::max(0.0, static_cast<double>(a.first) + rows)));
      return solar::layout::UnitsBeforeByte(v.value, nearestByte(a, v.value, a.lines[row], mx - a.contentLeft));
    }
    double shift = 0;
    if (document->focusedElement == el) {
      const double caretX = a.face->Measure(v.value.substr(0, solar::layout::BytesBeforeUnit(v.value, v.end))) * a.size;
      shift = caretX > a.contentWidth - 1 ? caretX - (a.contentWidth - 1) : 0;
    }
    return solar::layout::UnitsBeforeByte(v.value, nearestByte(a, v.value, a.lines[0], mx - a.contentLeft + shift));
  };
  const auto setSelection = [&](solar::dom::Element* field, size_t from, size_t to) {
    solar::html::RunInRealm(*realm, [&] { solar::html::SetCaret(document, field, std::min(from, to), std::max(from, to)); });
    dirty = true;
  };
  // The selection of the page's text a drag of the pointer makes.
  bool pageSelecting = false;
  solar::dom::Node* pageAnchor = nullptr;
  uint32_t pageAnchorOffset = 0;
  double pressStartX = 0, pressStartY = 0;
  // Whether a style keeps the text under an element from being selected (user-select: none, which what is below it inherits unless it says otherwise).
  const auto userSelectNone = [&](solar::dom::Element* element) {
    for (solar::dom::Node* n = element; n; n = n->parentNode) {
      if (!n->IsElement()) continue;
      solar::css::StyleReader reader(realm->GetContext(), static_cast<solar::dom::Element*>(n));
      const std::string value = reader.Get("user-select");
      if (value == "none") return true;
      if (value == "text" || value == "all" || value == "contain") return false;
    }
    return false;
  };
  const auto pageTextPointAt = [&](double wx, double wy, solar::dom::Node*& node, uint32_t& offset, solar::dom::Element** block) -> bool {
    solar::layout::Tree* tree = currentTree();
    if (!tree) return false;
    const auto& scroll = tree->scroll[document];
    return solar::paint::TextPointAt(*tree, wx + scroll.first, wy + scroll.second, node, offset, block);
  };
  // Where on a range input's track a point of the window is, from 0 to 1.
  const auto rangeFraction = [&](solar::dom::Element* el, double mx) -> double {
    solar::html::ControlView v;
    AreaView a;
    if (!readControl(el, v) || !viewOf(el, v, a)) return 0;
    return std::clamp((mx - (a.contentLeft + 7)) / std::max(1.0, a.contentWidth - 14), 0.0, 1.0);
  };
  // What the keys do in a text control; true when they were its.
  const auto textKey = [&](solar::dom::Element* field, const SDL_KeyboardEvent& k, const std::string& pasted) -> bool {
    solar::html::ControlView v;
    if (!readControl(field, v)) return false;
    const bool ctrl = (k.mod & SDL_KMOD_CTRL) != 0, shift = (k.mod & SDL_KMOD_SHIFT) != 0;
    const bool area = v.kind == "textarea";
    const std::string& text = v.value;
    const size_t a = solar::layout::BytesBeforeUnit(text, v.start), b = solar::layout::BytesBeforeUnit(text, v.end);
    // Which end of the selection is the caret: the one the keys last grew it from is the other.
    size_t caret = b, anchor = a;
    if (v.start == v.end) anchor = b;
    else if (anchorField == field && keyAnchor == v.end) { caret = a; anchor = b; }
    const auto go = [&](size_t to, bool extend) {
      const size_t from = extend ? anchor : to;
      anchorField = field;
      keyAnchor = solar::layout::UnitsBeforeByte(text, from);
      setSelection(field, keyAnchor, solar::layout::UnitsBeforeByte(text, to));
    };
    const auto typeIn = [&](const std::string& what, const char* kind) {
      solar::html::RunInRealm(*realm, [&] { solar::html::TypeInto(document, field, what, kind); });
      dirty = true;
    };
    const auto lineMove = [&](int rows) {
      AreaView view;
      if (!viewOf(field, v, view)) return;
      size_t line = 0;
      for (size_t i = 0; i < view.lines.size(); ++i) {
        if (caret >= view.lines[i].begin) line = i;
      }
      const double x = view.face->Measure(text.substr(view.lines[line].begin, std::min(caret, view.lines[line].end) - view.lines[line].begin)) * view.size;
      const long target = static_cast<long>(line) + rows;
      if (target < 0) go(0, shift);
      else if (target >= static_cast<long>(view.lines.size())) go(text.size(), shift);
      else go(nearestByte(view, text, view.lines[static_cast<size_t>(target)], x), shift);
    };
    switch (k.key) {
      case SDLK_A:
        if (!ctrl) return false;
        anchor = 0;
        go(text.size(), true);
        return true;
      case SDLK_C: case SDLK_X:
        if (!ctrl) return false;
        if (a != b && v.type != "password") {
          UiCommand command;
          command.kind = UiCommand::Kind::Clipboard;
          command.text = text.substr(std::min(a, b), std::max(a, b) - std::min(a, b));
          shared.Post(std::move(command));
          if (k.key == SDLK_X && !v.readOnly) typeIn("", "backspace");
        }
        return true;
      case SDLK_V: case SDLK_INSERT:
        if (k.key == SDLK_V ? !ctrl : !shift) return false;
        if (!pasted.empty() && !v.readOnly) typeIn(pasted, "text");
        return true;
      case SDLK_LEFT:
        if (ctrl) go(WordStartBefore(text, caret), shift);
        else if (!shift && a != b) go(std::min(a, b), false);
        else go(CharBefore(text, caret), shift);
        return true;
      case SDLK_RIGHT:
        if (ctrl) go(WordEndAfter(text, caret), shift);
        else if (!shift && a != b) go(std::max(a, b), false);
        else go(CharAfter(text, caret), shift);
        return true;
      case SDLK_HOME: go(ctrl || !area ? 0 : LineStartAt(text, caret), shift); return true;
      case SDLK_END: go(ctrl || !area ? text.size() : LineEndAt(text, caret), shift); return true;
      case SDLK_UP:
        if (area) lineMove(-1);
        else go(0, shift);
        return true;
      case SDLK_DOWN:
        if (area) lineMove(1);
        else go(text.size(), shift);
        return true;
      case SDLK_PAGEUP: case SDLK_PAGEDOWN:
        if (!area) return false;
        {
          AreaView view;
          lineMove(viewOf(field, v, view) ? static_cast<int>(view.visible) * (k.key == SDLK_PAGEUP ? -1 : 1) : 0);
        }
        return true;
      case SDLK_BACKSPACE:
        if (ctrl && a == b && caret > 0) setSelection(field, solar::layout::UnitsBeforeByte(text, WordStartBefore(text, caret)), v.end);
        typeIn("", "backspace");
        return true;
      case SDLK_DELETE:
        if (ctrl && a == b && caret < text.size()) setSelection(field, v.end, solar::layout::UnitsBeforeByte(text, WordEndAfter(text, caret)));
        typeIn("", "delete");
        return true;
      case SDLK_RETURN: case SDLK_KP_ENTER: typeIn("", "enter"); return true;
      default: return false;
    }
  };
  // What is under the pointer is what is hovered.
  double pointerX = 0, pointerY = 0;
  int lockButtons = 0;  // (the buttons held while the pointer is locked)
  bool pointerKnown = false, hoverStale = false;
  bool suppressText = false;  // a script cancelled the key that is about to give a character
  Clock::time_point lastScrollInput = Clock::now() - std::chrono::seconds(10);
  const auto updateHover = [&] {
    hoverStale = false;
    if (!pointerKnown) return;
    solar::layout::Tree* tree = currentTree();
    if (!tree) return;
    const std::vector<solar::dom::Element*> elements = solar::layout::ElementsAtPoint(*tree, pointerX, pointerY);
    solar::dom::Element* before = document->hoverElement;
    solar::dom::Element* now = elements.empty() ? nullptr : elements.front();
    solar::dom::SetHoverElement(document, now);
    if (now != before) {
      solar::html::RunInRealm(*realm, [&] { solar::html::DispatchHover(document, before, now, pointerX, pointerY); });
      dirty = true;
    }
  };
  // ---- Frames ----
  // A page in an iframe is a document of its own with its own tree: what is under the pointer, which is a frame, is asked of that tree at the place in it.
  // The chain is the frames the point is in, outermost first, each with the point as the frame's own viewport has it.
  struct FrameHit {
    solar::dom::Document* doc = nullptr;
    solar::layout::Tree* tree = nullptr;
    qe::Realm* realm = nullptr;
    double x = 0, y = 0;
  };
  const auto frameChainAt = [&](double px, double py) -> std::vector<FrameHit> {
    std::vector<FrameHit> chain;
    solar::dom::Document* doc = document;
    solar::layout::Tree* tree = currentTree();
    double x = px, y = py;
    for (int depth = 0; depth < 4 && tree && doc; ++depth) {
      const std::vector<solar::dom::Element*> elements = solar::layout::ElementsAtPoint(*tree, x, y);
      if (elements.empty()) break;
      solar::dom::Element* frame = elements.front();
      if (!frame->IsHtml("iframe") || !frame->contentDocument || !frame->contentDocument->realm || frame->contentDocument->frameElement != frame) break;
      solar::dom::Document* inner = frame->contentDocument;
      solar::layout::Tree* innerTree = inner->layoutTree.get();
      if (!innerTree || !innerTree->root) break;
      const solar::layout::Box* box = nullptr;
      for (const solar::layout::Box* candidate : solar::layout::BoxesOf(*tree, frame)) {
        if (candidate->kind == solar::layout::Box::Kind::Block) { box = candidate; break; }
      }
      if (!box) break;
      solar::layout::Rect rect = solar::layout::AbsoluteBorderBox(*box);
      for (const solar::layout::Box* q = box->parent; q; q = q->parent) {
        if (!q->node) continue;
        const auto found = tree->scroll.find(q->node);
        if (found != tree->scroll.end()) { rect.x -= found->second.first; rect.y -= found->second.second; }
      }
      x -= rect.x + box->border.left + box->padding.left;
      y -= rect.y + box->border.top + box->padding.top;
      chain.push_back(FrameHit{inner, innerTree, inner->realm, x, y});
      doc = inner;
      tree = innerTree;
    }
    return chain;
  };
  // A link clicked in the page of a frame goes where its target says: the frame itself (no target, _self, or the name of a frame, which is taken to be it),
  // the page above it (_parent) or the window (_top). A link that opens a window of its own (_blank) goes nowhere: there are no other windows.
  const auto followFrameLink = [&](const std::vector<FrameHit>& chain, size_t at, solar::dom::Element* from) {
    for (solar::dom::Node* n = from; n; n = n->parentNode) {
      if (!n->IsElement()) continue;
      solar::dom::Element* element = static_cast<solar::dom::Element*>(n);
      if (!element->IsHtml("a") && !element->IsHtml("area")) continue;
      const solar::dom::Attr* href = element->FindAttribute("", "href");
      if (!href) return;
      const std::string baseText = solar::html::DocumentBaseUrl(chain[at].doc);
      std::optional<solar::url::Url> base = solar::url::Parse(baseText);
      std::optional<solar::url::Url> target = base ? solar::url::Parse(href->value, &*base) : solar::url::Parse(href->value);
      if (!target || (target->scheme != "http" && target->scheme != "https" && target->scheme != "file")) return;
      const std::string where = solar::url::Serialize(*target);
      std::string name;
      if (const solar::dom::Attr* attr = element->FindAttribute("", "target")) name = attr->value;
      for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (name == "_blank") return;
      if (name == "_top" || (name == "_parent" && at == 0)) {
        navigate(where);
        return;
      }
      qe::Realm* realm = name == "_parent" ? chain[at - 1].realm : chain[at].realm;
      std::string quoted = "location.assign('";
      for (char c : where) {
        if (c == '\\' || c == '\'') quoted += '\\';
        quoted += c;
      }
      quoted += "');";
      solar::html::RunInRealm(*realm, [&] { realm->Evaluate(quoted, "link"); });
      dirty = true;
      return;
    }
  };
  std::vector<solar::dom::Document*> hoveredFrames;  // the frames that have an element under the pointer
  // What is under the pointer in each frame it is over is what is hovered there; the frames it left have nothing hovered.
  const auto updateFrameHover = [&](bool moved) {
    const std::vector<FrameHit> chain = frameChainAt(pointerX, pointerY);
    std::vector<solar::dom::Document*> now;
    for (const FrameHit& hit : chain) {
      const std::vector<solar::dom::Element*> elements = solar::layout::ElementsAtPoint(*hit.tree, hit.x, hit.y);
      solar::dom::Element* over = elements.empty() ? nullptr : elements.front();
      solar::dom::Element* before = hit.doc->hoverElement;
      solar::dom::SetHoverElement(hit.doc, over);
      if (over != before) solar::html::RunInRealm(*hit.realm, [&] { solar::html::DispatchHover(hit.doc, before, over, hit.x, hit.y); });
      if (moved && over && &hit == &chain.back()) {
        solar::html::RunInRealm(*hit.realm, [&] { solar::html::DispatchPointer(hit.doc, over, "mousemove", hit.x, hit.y, 0, pressTarget ? 1 : 0); });
      }
      now.push_back(hit.doc);
      dirty = true;
    }
    for (solar::dom::Document* old : hoveredFrames) {
      if (std::find(now.begin(), now.end(), old) != now.end() || !old->realm) continue;
      solar::dom::Element* before = old->hoverElement;
      if (!before) continue;
      solar::dom::SetHoverElement(old, nullptr);
      solar::html::RunInRealm(*old->realm, [&] { solar::html::DispatchHover(old, before, nullptr, 0, 0); });
      dirty = true;
    }
    hoveredFrames = std::move(now);
  };
  // The frame a press began in (the release and the click go to it).
  FrameHit framePress;
  solar::dom::Element* framePressElement = nullptr;
  // The page that has the keyboard: the one whose focused element is a frame, down the chain; the document itself when its focus is not in a frame.
  const auto keyboardDocument = [&]() -> solar::dom::Document* {
    solar::dom::Document* doc = document;
    for (int depth = 0; depth < 4 && doc; ++depth) {
      solar::dom::Element* focused = doc->focusedElement;
      if (!focused || !focused->IsHtml("iframe") || !focused->contentDocument || !focused->contentDocument->realm) break;
      doc = focused->contentDocument;
    }
    return doc;
  };

  // The scroll is the window's (it glides, it is dragged by the scrollbar): the page asks it to move, and is told where it is.
  const bool scrollTestMode = std::getenv("SOLAR_GUI_SCROLL") != nullptr;
  const auto scrollBy = [&](double dx, double dy) {
    UiCommand command;
    command.kind = UiCommand::Kind::ScrollBy;
    command.x = dx;
    command.y = dy;
    command.instant = scrollTestMode;
    shared.Post(std::move(command));
  };
  // What the window scrolled to, put in the tree; what script scrolled to, told to the window. (Which of the two moved since the last time decides.)
  uint64_t appliedSerial = 0;
  double appliedX = 0, appliedY = 0;
  const auto syncScroll = [&] {
    solar::layout::Tree* tree = document ? document->layoutTree.get() : nullptr;
    if (!tree) return;
    auto& at = tree->scroll[document];
    if (at.first != appliedX || at.second != appliedY) {
      // Script scrolled the page (or a link to a fragment did): the window goes there.
      appliedX = at.first;
      appliedY = at.second;
      UiCommand command;
      command.kind = UiCommand::Kind::ScrollTo;
      command.x = appliedX;
      command.y = appliedY;
      shared.Post(std::move(command));
      appliedSerial = shared.scrollSerial.load();
      dirty = true;
      return;
    }
    const uint64_t serial = shared.scrollSerial.load();
    if (serial == appliedSerial) return;
    appliedSerial = serial;
    const double sx = shared.scrollX.load(), sy = shared.scrollY.load();
    if (sx == appliedX && sy == appliedY) return;
    appliedX = at.first = sx;
    appliedY = at.second = sy;
    solar::layout::ApplySticky(*tree);
    lastScrollInput = Clock::now();
    hoverStale = pointerKnown;
    // The list that was made has items for some way above and below where it was made; past a part of that it is made again.
    // (SOLAR_GUI_NOREBUILD: it is not made again for the scroll, to see what the window does by itself.)
    if (!std::getenv("SOLAR_GUI_NOREBUILD") && (std::abs(sy - builtY) > 0.25 * shared.height.load() || std::abs(sx - builtX) > 1)) dirty = true;
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
  while (running && !shared.quit.load()) {
    const auto iterStart = Clock::now();
    barMs = stepMs = glideMs = 0;
    UiEvent incoming;
    bool any = shared.WaitEvent(incoming, std::chrono::milliseconds(dirty ? 0 : 8));
    while (any) {
      SDL_Event& event = incoming.event;
      if (event.type == SDL_EVENT_TEXT_INPUT) event.text.text = incoming.text.c_str();
      switch (event.type) {
        case SDL_EVENT_QUIT: running = false; break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
          setViewport();
          dirty = true;
          break;
        case SDL_EVENT_KEY_UP: {
          const KeyInfo info = DescribeKey(event.key);
          if (!info.key.empty() && document && document->DocumentElement()) {
            solar::dom::Element* target = document->focusedElement ? document->focusedElement : document->DocumentElement();
            solar::html::RunInRealm(*realm, [&] { solar::html::DispatchKey(document, target, "keyup", info.key, info.code, info.keyCode, false, info.modifiers); });
            dirty = true;
          }
          break;
        }
        case SDL_EVENT_TEXT_INPUT: {
          if (suppressText) {
            suppressText = false;
            break;
          }
          caretReset();
          if (solar::dom::Document* typing = keyboardDocument(); typing && typing != document && typing->realm) {
            // The keyboard is in a frame: the text goes to the control that has the focus there.
            solar::dom::Element* field = typing->focusedElement;
            if (field && (field->IsHtml("input") || field->IsHtml("textarea"))) {
              solar::html::RunInRealm(*typing->realm, [&] { solar::html::TypeInto(typing, field, event.text.text, "text"); });
              dirty = true;
            }
            break;
          }
          solar::dom::Element* focused = document ? document->focusedElement : nullptr;
          if (popupSelect) {
            // Typing in the open drop-down goes to the next row that begins with it.
            solar::html::ControlView v;
            if (readControl(popupSelect, v)) {
              const size_t row = typeAhead(v, popupHover, event.text.text);
              if (row != kNone) {
                popupHover = row;
                if (popupHover < popupTop) popupTop = popupHover;
                if (popupHover >= popupTop + 20) popupTop = popupHover - 19;
                showPopup();
              }
            }
            break;
          }
          if (solar::dom::Element* field = focusedTextControl()) {
            solar::html::RunInRealm(*realm, [&] { solar::html::TypeInto(document, field, event.text.text, "text"); });
            dirty = true;
          } else if (focused && focused->IsHtml("select")) {
            solar::html::ControlView v;
            if (readControl(focused, v) && !v.disabled) {
              const size_t row = typeAhead(v, selectedRow(v), event.text.text);
              if (row != kNone) solar::html::RunInRealm(*realm, [&] { solar::html::ChooseOption(document, focused, optionOfRow(v, row), 0); });
              dirty = true;
            }
          }
          break;
        }
        case SDL_EVENT_KEY_DOWN: {
          const int h = shared.height.load();
          suppressText = false;
          caretReset();
          if (solar::dom::Document* typing = keyboardDocument(); typing && typing != document && typing->realm && typing->DocumentElement()) {
            // The keyboard is in a frame: its page sees the key, and its control with the focus takes what edits it.
            const KeyInfo info = DescribeKey(event.key);
            bool proceed = true;
            if (!info.key.empty()) {
              solar::dom::Element* target = typing->focusedElement ? typing->focusedElement : typing->DocumentElement();
              solar::html::RunInRealm(*typing->realm, [&] {
                proceed = solar::html::DispatchKey(typing, target, "keydown", info.key, info.code, info.keyCode, event.key.repeat, info.modifiers);
                if (proceed && !(info.modifiers & (1 | 4 | 8)) && (info.key == "Enter" || (info.key.size() == 1 && info.key[0] >= 0x20))) {
                  if (typing->focusedElement) target = typing->focusedElement;
                  proceed = solar::html::DispatchKey(typing, target, "keypress", info.key, info.code, info.key == "Enter" ? 13 : static_cast<unsigned char>(info.key[0]), event.key.repeat, info.modifiers);
                }
              });
              dirty = true;
            }
            if (!proceed) {
              suppressText = info.key.size() == 1;
              break;
            }
            solar::dom::Element* field = typing->focusedElement;
            const SDL_Keycode frameKey = event.key.key;
            if (frameKey == SDLK_TAB) {
              typing->focusByKeyboard = true;
              solar::html::RunInRealm(*typing->realm, [&] { solar::html::FocusNext(typing, (event.key.mod & SDL_KMOD_SHIFT) != 0); });
              dirty = true;
            } else if (field && (field->IsHtml("input") || field->IsHtml("textarea"))) {
              const char* kind = frameKey == SDLK_BACKSPACE ? "backspace" : frameKey == SDLK_DELETE ? "delete" : (frameKey == SDLK_RETURN || frameKey == SDLK_KP_ENTER) ? "enter" : frameKey == SDLK_LEFT ? "left" : frameKey == SDLK_RIGHT ? "right" : frameKey == SDLK_HOME ? "home" : frameKey == SDLK_END ? "end" : nullptr;
              if (kind) {
                solar::html::RunInRealm(*typing->realm, [&] { solar::html::TypeInto(typing, field, "", kind); });
                dirty = true;
              }
            } else if ((frameKey == SDLK_RETURN || frameKey == SDLK_SPACE) && field) {
              solar::html::RunInRealm(*typing->realm, [&] { solar::html::DispatchClick(typing, field, 0, 0, 0); });
              dirty = true;
            }
            break;
          }
          if (document && document->DocumentElement()) {
            // The page sees the key first; what the program does with it (scrolling, typing, pressing) is for when no script cancelled it.
            const KeyInfo info = DescribeKey(event.key);
            if (!info.key.empty()) {
              solar::dom::Element* target = document->focusedElement ? document->focusedElement : document->DocumentElement();
              bool proceed = true;
              solar::html::RunInRealm(*realm, [&] {
                proceed = solar::html::DispatchKey(document, target, "keydown", info.key, info.code, info.keyCode, event.key.repeat, info.modifiers);
                // (A key that makes a character, or Enter, also fires keypress.)
                if (proceed && !(info.modifiers & (1 | 4 | 8)) && (info.key == "Enter" || (info.key.size() == 1 && info.key[0] >= 0x20))) {
                  if (document->focusedElement) target = document->focusedElement;
                  proceed = solar::html::DispatchKey(document, target, "keypress", info.key, info.code, info.key == "Enter" ? 13 : static_cast<unsigned char>(info.key[0]), event.key.repeat, info.modifiers);
                }
              });
              dirty = true;
              if (!proceed) {
                suppressText = info.key.size() == 1;
                break;
              }
            }
          }
          const SDL_Keycode key = event.key.key;
          const bool shiftHeld = (event.key.mod & SDL_KMOD_SHIFT) != 0, altHeld = (event.key.mod & SDL_KMOD_ALT) != 0;
          solar::dom::Element* focused = document ? document->focusedElement : nullptr;
          if (popupSelect) {
            popupKey(key);
            break;
          }
          if (key == SDLK_TAB) {
            document->focusByKeyboard = true;
            solar::html::RunInRealm(*realm, [&] { solar::html::FocusNext(document, shiftHeld); });
            dirty = true;
            break;
          }
          // Back and forward (alt and the arrows).
          if (altHeld && (key == SDLK_LEFT || key == SDLK_RIGHT)) {
            goInHistory(key == SDLK_LEFT ? -1 : 1);
            break;
          }
          if (!focusedTextControl() && (event.key.mod & SDL_KMOD_CTRL) && (key == SDLK_A || key == SDLK_C)) {
            // What the keys do to the text of the page: select all of it, or copy what is selected.
            if (key == SDLK_A) {
              if (solar::dom::Element* root = document->DocumentElement()) solar::html::RunInRealm(*realm, [&] { solar::html::SelectContents(document, root); });
            } else {
              std::string chosen;
              solar::html::RunInRealm(*realm, [&] { chosen = solar::html::SelectedText(document); });
              if (!chosen.empty()) {
                UiCommand command;
                command.kind = UiCommand::Kind::Clipboard;
                command.text = std::move(chosen);
                shared.Post(std::move(command));
              }
            }
            dirty = true;
            break;
          }
          if (solar::dom::Element* field = focusedTextControl()) {
            solar::html::ControlView v;
            if (readControl(field, v) && v.type == "number" && (key == SDLK_UP || key == SDLK_DOWN)) {
              solar::html::RunInRealm(*realm, [&] { solar::html::StepControl(document, field, key == SDLK_UP ? 1 : -1); });
              dirty = true;
              break;
            }
            if (textKey(field, event.key, incoming.clipboard)) break;
            if (key != SDLK_ESCAPE && key != SDLK_PAGEDOWN && key != SDLK_PAGEUP) break;
          } else if (focused && focused->IsHtml("select")) {
            if (selectKey(focused, event.key)) break;
          } else if (focused && focused->IsHtml("input")) {
            solar::html::ControlView v;
            if (readControl(focused, v) && v.type == "range") {
              bool handled = true;
              if (key == SDLK_LEFT || key == SDLK_DOWN) solar::html::RunInRealm(*realm, [&] { solar::html::StepControl(document, focused, -1); });
              else if (key == SDLK_RIGHT || key == SDLK_UP) solar::html::RunInRealm(*realm, [&] { solar::html::StepControl(document, focused, 1); });
              else if (key == SDLK_HOME) solar::html::RunInRealm(*realm, [&] { solar::html::SetRange(document, focused, 0, true); });
              else if (key == SDLK_END) solar::html::RunInRealm(*realm, [&] { solar::html::SetRange(document, focused, 1, true); });
              else handled = false;
              if (handled) {
                dirty = true;
                break;
              }
            } else if (v.type == "radio" && (key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_UP || key == SDLK_DOWN)) {
              solar::html::RunInRealm(*realm, [&] { solar::html::MoveInRadioGroup(document, focused, key == SDLK_LEFT || key == SDLK_UP ? -1 : 1); });
              dirty = true;
              break;
            }
          }
          if (focused && (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE) &&
              (focused->IsHtml("button") || focused->IsHtml("input") || focused->IsHtml("a") || focused->IsHtml("summary"))) {
            // Enter presses a link, a button or a box; space the last two.
            if (focused->IsHtml("a") && key == SDLK_SPACE) {}
            else {
              bool follow = true;
              solar::html::RunInRealm(*realm, [&] { follow = solar::html::DispatchClick(document, focused, 0, 0, 0); });
              if (follow) followLink(focused);
              dirty = true;
              break;
            }
          }
          switch (key) {
            case SDLK_ESCAPE: if (!focused) running = false; break;
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
        case SDL_EVENT_MOUSE_WHEEL: {
          if (document && pointerKnown && document->hoverElement) {
            solar::dom::Element* over = document->hoverElement;
            solar::html::RunInRealm(*realm, [&] { solar::html::DispatchWheel(document, over, pointerX, pointerY, -event.wheel.x * 100.0, -event.wheel.y * 100.0); });
            dirty = true;
          }
          if (document && pointerKnown) {
            const std::vector<FrameHit> chain = frameChainAt(pointerX, pointerY);
            if (!chain.empty()) {
              const FrameHit& hit = chain.back();
              const std::vector<solar::dom::Element*> elements = solar::layout::ElementsAtPoint(*hit.tree, hit.x, hit.y);
              if (!elements.empty()) {
                solar::dom::Element* over = elements.front();
                solar::html::RunInRealm(*hit.realm, [&] { solar::html::DispatchWheel(hit.doc, over, hit.x, hit.y, -event.wheel.x * 100.0, -event.wheel.y * 100.0); });
                dirty = true;
              }
            }
            // What is scrolled is the innermost scroller under the pointer that can go that way: an element with an overflow that scrolls, then the
            // page of its frame, then what is around the frame (the window did not scroll, see the wheel of the window; at the end of them all it does).
            const double stepX = -event.wheel.x * 60, stepY = -event.wheel.y * 60;
            struct Level { solar::layout::Tree* tree; solar::dom::Document* doc; double x, y; };
            std::vector<Level> levels;
            if (solar::layout::Tree* top = currentTree()) levels.push_back({top, document, pointerX, pointerY});
            for (const FrameHit& hit : chain) levels.push_back({hit.tree, hit.doc, hit.x, hit.y});
            const auto scrollAt = [&](solar::layout::Tree& tree, const solar::dom::Node* key, double maxX, double maxY) {
              auto& position = tree.scroll[key];
              const double nx = std::clamp(position.first + stepX, 0.0, std::max(0.0, maxX)), ny = std::clamp(position.second + stepY, 0.0, std::max(0.0, maxY));
              if (nx == position.first && ny == position.second) return false;
              position = {nx, ny};
              solar::layout::ApplySticky(tree);
              dirty = true;
              return true;
            };
            for (auto level = levels.rbegin(); level != levels.rend(); ++level) {
              const std::vector<solar::dom::Element*> under = solar::layout::ElementsAtPoint(*level->tree, level->x, level->y);
              bool done = false;
              for (solar::dom::Node* n = under.empty() ? nullptr : under.front(); n && n->IsElement() && !done; n = n->parentNode) {
                double maxX = 0, maxY = 0;
                if (userScrollRange(*level->tree, static_cast<solar::dom::Element*>(n), maxX, maxY)) done = scrollAt(*level->tree, n, maxX, maxY);
              }
              if (done) break;
              if (level->doc != document) {
                double maxX = 0, maxY = 0;
                solar::layout::ScrollRange(*level->tree, nullptr, maxX, maxY);
                if (scrollAt(*level->tree, level->doc, maxX, maxY)) break;
              }
            }
          }
          break;
        }
        case SDL_EVENT_MOUSE_MOTION: {
          if (g_pointerLocked.load()) {
            // Locked: the page hears how far the pointer went, and the pointer stays where it was.
            solar::html::RunInRealm(*realm, [&] { solar::html::DispatchLocked(document, "mousemove", pointerX, pointerY, 0, lockButtons, event.motion.xrel, event.motion.yrel); });
            dirty = true;
            break;
          }
          pointerX = event.motion.x;
          pointerY = event.motion.y;
          pointerKnown = true;
          if (popupSelect) {
            bool inside = false;
            const size_t row = popupRowAt(pointerX, pointerY, inside);
            solar::html::ControlView v;
            if (inside && readControl(popupSelect, v) && pickable(v, row) && row != popupHover) {
              popupHover = row;
              showPopup();
            }
            break;
          }
          if (dragField) {
            const size_t at = textPositionAt(dragField, pointerX, pointerY);
            setSelection(dragField, dragAnchor, at);
            caretReset();
            break;
          }
          if (pageSelecting) {
            solar::dom::Node* node = nullptr;
            uint32_t offset = 0;
            if (pageTextPointAt(pointerX, pointerY, node, offset, nullptr)) {
              solar::html::RunInRealm(*realm, [&] { solar::html::SelectText(document, pageAnchor, pageAnchorOffset, node, offset); });
              dirty = true;
            }
            break;
          }
          if (dragRange) {
            const double fraction = rangeFraction(dragRange, pointerX);
            solar::html::RunInRealm(*realm, [&] { solar::html::SetRange(document, dragRange, fraction, false); });
            dirty = true;
            break;
          }
          // (While the page is scrolling, what is under the pointer is looked at when it stops: a change of hover is a change of style, and a frame is not the time for one.)
          if (shared.gliding.load() || Clock::now() - lastScrollInput < std::chrono::milliseconds(150)) hoverStale = true;
          else {
            updateHover();
            if (document->hoverElement) {
              solar::dom::Element* over = document->hoverElement;
              solar::html::RunInRealm(*realm, [&] { solar::html::DispatchPointer(document, over, "mousemove", event.motion.x, event.motion.y, 0, pressTarget ? 1 : 0); });
              dirty = true;
            }
            updateFrameHover(true);
          }
          break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
          if (g_pointerLocked.load()) {
            const int b = event.button.button == SDL_BUTTON_LEFT ? 0 : event.button.button == SDL_BUTTON_MIDDLE ? 1 : 2;
            lockButtons |= 1 << b;
            solar::html::RunInRealm(*realm, [&] { solar::html::DispatchLocked(document, "mousedown", pointerX, pointerY, b, lockButtons, 0, 0); });
            dirty = true;
            break;
          }
          if (hoverStale) updateHover();
          pointerX = event.button.x;
          pointerY = event.button.y;
          pointerKnown = true;
          caretReset();
          if (popupSelect) {
            // A press with the drop-down open chooses what is under it, or closes it; the page does not hear of it.
            bool inside = false;
            const size_t row = popupRowAt(pointerX, pointerY, inside);
            if (inside) chooseRow(row);
            else closePopup();
            swallowRelease = true;
            break;
          }
          updateHover();
          pressTarget = document->hoverElement;
          framePressElement = nullptr;
          framePress = FrameHit{};
          if (pressTarget && pressTarget->IsHtml("iframe")) {
            // A press on a frame is a press in the page it holds, at the place it is.
            const std::vector<FrameHit> chain = frameChainAt(event.button.x, event.button.y);
            if (!chain.empty()) {
              const FrameHit& hit = chain.back();
              const std::vector<solar::dom::Element*> elements = solar::layout::ElementsAtPoint(*hit.tree, hit.x, hit.y);
              if (!elements.empty()) {
                const int button = event.button.button == SDL_BUTTON_LEFT ? 0 : event.button.button == SDL_BUTTON_MIDDLE ? 1 : 2;
                framePress = hit;
                framePressElement = elements.front();
                // The focus is in the frame now: each page that holds it has the frame as what it has the focus in.
                for (solar::dom::Document* holder = hit.doc; holder && holder->frameElement; holder = holder->frameElement->nodeDocument) {
                  solar::dom::Document* parent = holder->frameElement->nodeDocument;
                  if (!parent) break;
                  if (parent->focusedElement != holder->frameElement) {
                    parent->focusedElement = holder->frameElement;
                    parent->NoteWrite();
                  }
                }
                solar::dom::Element* target = framePressElement;
                solar::html::RunInRealm(*hit.realm, [&] { solar::html::DispatchPointer(hit.doc, target, "mousedown", hit.x, hit.y, button, 1 << button); });
                if (event.button.button == SDL_BUTTON_LEFT) solar::dom::SetPressedElement(hit.doc, target);
                dirty = true;
              }
            }
          }
          if (timing) std::fprintf(stderr, "press at %.0f,%.0f on %s%s%s\n", event.button.x, event.button.y, pressTarget ? pressTarget->localName.c_str() : "nothing", pressTarget && pressTarget->FindAttribute("", "class") ? "." : "", pressTarget && pressTarget->FindAttribute("", "class") ? pressTarget->FindAttribute("", "class")->value.c_str() : "");
          pressButton = event.button.button == SDL_BUTTON_LEFT ? 0 : event.button.button == SDL_BUTTON_MIDDLE ? 1 : 2;
          if (event.button.button == SDL_BUTTON_X1 || event.button.button == SDL_BUTTON_X2) {
            goInHistory(event.button.button == SDL_BUTTON_X1 ? -1 : 1);
            pressTarget = nullptr;
            swallowRelease = true;
            break;
          }
          if (pressTarget) {
            bool proceed = true;
            document->focusByKeyboard = false;
            solar::html::RunInRealm(*realm, [&] { proceed = solar::html::DispatchPointer(document, pressTarget, "mousedown", event.button.x, event.button.y, pressButton, 1 << pressButton); });
            // (A script may have taken the element away.)
            if (proceed && event.button.button == SDL_BUTTON_LEFT && document->hoverElement) solar::dom::SetPressedElement(document, document->hoverElement);
            if (proceed && event.button.button == SDL_BUTTON_LEFT && pressTarget) {
              // Presses close together on one place are a double and a triple click.
              const auto now = Clock::now();
              if (now - lastPress < std::chrono::milliseconds(500) && std::fabs(pointerX - lastPressX) < 5 && std::fabs(pointerY - lastPressY) < 5) ++pressCount;
              else pressCount = 1;
              lastPress = now;
              pressStartX = pointerX;
              pressStartY = pointerY;
              lastPressX = pointerX;
              lastPressY = pointerY;
              if (pressTarget == focusedTextControl()) {
                // A press in a text field puts the caret between the letters nearest to it; a double click takes the word, a triple one the line (or all); a drag grows it.
                solar::html::ControlView v;
                if (readControl(pressTarget, v)) {
                  const size_t at = textPositionAt(pressTarget, pointerX, pointerY);
                  const size_t atByte = solar::layout::BytesBeforeUnit(v.value, at);
                  const bool shiftHeld = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
                  if (shiftHeld && pressCount == 1) {
                    if (anchorField != pressTarget) { anchorField = pressTarget; keyAnchor = v.start == v.end ? v.end : v.start; }
                    dragAnchor = keyAnchor;
                  } else if (pressCount == 2) {
                    size_t from = 0, to = 0;
                    WordAround(v.value, atByte, from, to);
                    dragAnchor = solar::layout::UnitsBeforeByte(v.value, from);
                    setSelection(pressTarget, dragAnchor, solar::layout::UnitsBeforeByte(v.value, to));
                    anchorField = pressTarget;
                    keyAnchor = dragAnchor;
                    dragField = nullptr;
                    goto pressed;
                  } else if (pressCount >= 3) {
                    const bool area = v.kind == "textarea";
                    dragAnchor = solar::layout::UnitsBeforeByte(v.value, area ? LineStartAt(v.value, atByte) : 0);
                    setSelection(pressTarget, dragAnchor, solar::layout::UnitsBeforeByte(v.value, area ? LineEndAt(v.value, atByte) : v.value.size()));
                    anchorField = pressTarget;
                    keyAnchor = dragAnchor;
                    dragField = nullptr;
                    goto pressed;
                  } else {
                    dragAnchor = at;
                  }
                  anchorField = pressTarget;
                  keyAnchor = dragAnchor;
                  dragField = pressTarget;
                  setSelection(pressTarget, dragAnchor, at);
                }
              } else if (pressTarget->IsHtml("select")) {
                solar::html::ControlView v;
                if (readControl(pressTarget, v) && !v.disabled) {
                  if (v.rows > 1 || v.multiple) {
                    // A list: the row under the pointer is chosen (with control it is toggled, with shift the choice grows to it).
                    AreaView view;
                    if (viewOf(pressTarget, v, view)) {
                      const size_t row = v.top + static_cast<size_t>(std::max(0.0, std::floor((pointerY - view.contentTop) / view.rowHeight)));
                      if (pickable(v, row)) {
                        const SDL_Keymod mods = SDL_GetModState();
                        const int mode = !v.multiple ? 0 : (mods & SDL_KMOD_CTRL) ? 1 : (mods & SDL_KMOD_SHIFT) ? 2 : 0;
                        solar::html::RunInRealm(*realm, [&] { solar::html::ChooseOption(document, pressTarget, optionOfRow(v, row), mode); });
                      }
                    }
                  } else {
                    openPopup(pressTarget);
                  }
                }
              } else if (pressTarget->IsHtml("input") || pressTarget->IsHtml("textarea") || pressTarget->IsHtml("button")) {
                solar::html::ControlView v;
                if (pressTarget->IsHtml("input") && readControl(pressTarget, v) && v.type == "range" && !v.disabled) {
                  dragRange = pressTarget;
                  const double fraction = rangeFraction(pressTarget, pointerX);
                  solar::html::RunInRealm(*realm, [&] { solar::html::SetRange(document, dragRange, fraction, false); });
                }
              } else if (!userSelectNone(pressTarget)) {
                // A press on the text of the page begins a selection (a double click takes the word, a triple one the block).
                solar::dom::Node* node = nullptr;
                uint32_t offset = 0;
                solar::dom::Element* block = nullptr;
                if (pageTextPointAt(pointerX, pointerY, node, offset, &block)) {
                  const std::string& data = static_cast<solar::dom::CharacterData*>(node)->Data();
                  if (pressCount == 2) {
                    size_t from = 0, to = 0;
                    WordAround(data, solar::layout::BytesBeforeUnit(data, offset), from, to);
                    solar::html::RunInRealm(*realm, [&] { solar::html::SelectText(document, node, static_cast<uint32_t>(solar::layout::UnitsBeforeByte(data, from)), node, static_cast<uint32_t>(solar::layout::UnitsBeforeByte(data, to))); });
                  } else if (pressCount >= 3 && block) {
                    solar::html::RunInRealm(*realm, [&] { solar::html::SelectContents(document, block); });
                  } else {
                    pageSelecting = true;
                    pageAnchor = node;
                    pageAnchorOffset = offset;
                    solar::html::RunInRealm(*realm, [&] { solar::html::SelectText(document, node, offset, node, offset); });
                  }
                } else {
                  solar::html::RunInRealm(*realm, [&] { solar::html::ClearSelection(document); });
                }
              }
            }
            pressed:
            dirty = true;
          }
          break;
        }
        case SDL_EVENT_MOUSE_BUTTON_UP: {
          if (g_pointerLocked.load()) {
            const int b = event.button.button == SDL_BUTTON_LEFT ? 0 : event.button.button == SDL_BUTTON_MIDDLE ? 1 : 2;
            lockButtons &= ~(1 << b);
            solar::html::RunInRealm(*realm, [&] { solar::html::DispatchLocked(document, "mouseup", pointerX, pointerY, b, lockButtons, 0, 0); });
            dirty = true;
            break;
          }
          if (dragField) dragField = nullptr;
          const bool draggedFar = pageSelecting && (std::fabs(event.button.x - pressStartX) > 4 || std::fabs(event.button.y - pressStartY) > 4);
          pageSelecting = false;
          if (dragRange) {
            const double fraction = rangeFraction(dragRange, event.button.x);
            solar::dom::Element* range = dragRange;
            dragRange = nullptr;
            solar::html::RunInRealm(*realm, [&] { solar::html::SetRange(document, range, fraction, true); });
            dirty = true;
          }
          if (swallowRelease) {
            swallowRelease = false;
            break;
          }
          solar::dom::Element* pressed = document->pressedElement;
          solar::dom::SetPressedElement(document, nullptr);
          if (framePress.doc && framePressElement) {
            // The release of a press that began in a frame goes to the page there, which has the click if it was released on what it was pressed on.
            const std::vector<FrameHit> chain = frameChainAt(event.button.x, event.button.y);
            solar::dom::Document* pressedDoc = framePress.doc;
            solar::dom::SetPressedElement(pressedDoc, nullptr);
            for (const FrameHit& hit : chain) {
              if (hit.doc != pressedDoc) continue;
              const std::vector<solar::dom::Element*> elements = solar::layout::ElementsAtPoint(*hit.tree, hit.x, hit.y);
              solar::dom::Element* released = elements.empty() ? nullptr : elements.front();
              if (!released) break;
              solar::dom::Element* clicked = nullptr;
              if (framePressElement == released) clicked = released;
              else {
                for (solar::dom::Node* n = released; n && !clicked; n = n->parentNode) if (n == framePressElement) clicked = framePressElement;
                for (solar::dom::Node* n = framePressElement; n && !clicked; n = n->parentNode) if (n == released) clicked = released;
              }
              bool follow = true;
              solar::html::RunInRealm(*hit.realm, [&] {
                solar::html::DispatchPointer(hit.doc, released, "mouseup", hit.x, hit.y, pressButton, 0);
                if (clicked) follow = solar::html::DispatchClick(hit.doc, clicked, hit.x, hit.y, pressButton);
              });
              dirty = true;
              if (clicked && follow && event.button.button == SDL_BUTTON_LEFT) followFrameLink(chain, static_cast<size_t>(&hit - chain.data()), clicked);
              break;
            }
            pressTarget = nullptr;  // (the page above does not hear of a click that was in the frame)
            framePressElement = nullptr;
            framePress = FrameHit{};
          }
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
          if (event.button.button == SDL_BUTTON_LEFT && clicked && follow && !draggedFar) followLink(clicked);
          (void)pressed;
          break;
        }
        default: break;
      }
      any = shared.WaitEvent(incoming, std::chrono::milliseconds(0));
      if (any) {}
    }
    // The drop-down belongs to the page it was opened in.
    if (popupSelect && popupDocument != static_cast<const void*>(document)) {
      popupSelect = nullptr;
      dragField = nullptr;
      dragRange = nullptr;
      solar::paint::SetPopup(solar::paint::Popup{});
    }
    // The caret of the field with the focus blinks.
    if (focusedTextControl()) {
      if (Clock::now() - caretSince > std::chrono::milliseconds(530)) {
        caretOn = !caretOn;
        caretSince = Clock::now();
        solar::paint::SetCaretVisible(caretOn);
        dirty = true;
      }
    } else if (!caretOn) {
      caretReset();
      dirty = true;
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
      if (step.rfind("r:", 0) == 0) {
        // (r:dx,dy: the mouse moves by that much, as with the pointer locked.)
        float dx = 0, dy = 0;
        std::sscanf(step.c_str() + 2, "%f,%f", &dx, &dy);
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.xrel = dx;
        e.motion.yrel = dy;
        SDL_PushEvent(&e);
      } else if (step.rfind("c:", 0) == 0 || step.rfind("m:", 0) == 0 || step.rfind("p:", 0) == 0 || step.rfind("u:", 0) == 0) {
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
      } else if (step.rfind("w:", 0) == 0) {
        e.type = SDL_EVENT_MOUSE_WHEEL;
        e.wheel.y = -static_cast<float>(std::atof(step.c_str() + 2));
        e.wheel.mouse_x = static_cast<float>(pointerX);
        e.wheel.mouse_y = static_cast<float>(pointerY);
        SDL_PushEvent(&e);
      } else if (step.rfind("t:", 0) == 0) {
        e.type = SDL_EVENT_TEXT_INPUT;
        static std::string typed;
        typed = step.substr(2);
        e.text.text = typed.c_str();
        SDL_PushEvent(&e);
      } else if (step.rfind("k:", 0) == 0) {
        e.type = SDL_EVENT_KEY_DOWN;
        std::string name = step.substr(2);
        // (ctrl+, shift+ and alt+ in front of the key.)
        for (;;) {
          if (name.rfind("ctrl+", 0) == 0) { e.key.mod |= SDL_KMOD_CTRL; name = name.substr(5); }
          else if (name.rfind("shift+", 0) == 0) { e.key.mod |= SDL_KMOD_SHIFT; name = name.substr(6); }
          else if (name.rfind("alt+", 0) == 0) { e.key.mod |= SDL_KMOD_ALT; name = name.substr(4); }
          else break;
        }
        e.key.key = name == "tab" ? SDLK_TAB : name == "pageup" ? SDLK_PAGEUP : name == "pagedown" ? SDLK_PAGEDOWN : name == "escape" ? SDLK_ESCAPE : name == "backspace" ? SDLK_BACKSPACE : name == "enter" ? SDLK_RETURN : name == "left" ? SDLK_LEFT : name == "right" ? SDLK_RIGHT : name == "delete" ? SDLK_DELETE : name == "home" ? SDLK_HOME : name == "end" ? SDLK_END : name == "space" ? SDLK_SPACE : name == "up" ? SDLK_UP : name == "down" ? SDLK_DOWN : name.size() == 1 ? static_cast<SDL_Keycode>(name[0]) : SDLK_UNKNOWN;
        SDL_PushEvent(&e);
        e.type = SDL_EVENT_KEY_UP;
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
    if (wheelDone != Clock::time_point() && !shared.gliding.load() && Clock::now() - wheelDone > std::chrono::milliseconds(300)) running = false;
    syncScroll();
    if (hoverStale && !shared.gliding.load() && Clock::now() - lastScrollInput > std::chrono::milliseconds(150)) updateHover();
    const auto g0 = Clock::now();
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

    // (An animation that changed how something is drawn, or what a layout is made of, is a new frame.)
    const uint64_t version = (((solar::dom::DocumentVersion() * 1000003 + solar::css::StyleVersion()) * 1000003 + solar::dom::StateVersion()) * 1000003 + solar::css::AnimationVersion()) * 1000003 + solar::css::DrawAnimationVersion();
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
    // (The list has items for some way above and below the viewport too, so that the window can scroll without waiting for the page.)
    auto list = std::make_shared<const solar::paint::DisplayList>(solar::paint::BuildDisplayList(realm->GetContext(), *tree, shared.height.load()));
    browser.painting_ = false;
    const auto t2 = Clock::now();
    builtX = list->scrollX;
    builtY = list->scrollY;
    Frame frame;
    frame.list = list;
    solar::layout::ScrollRange(*tree, nullptr, frame.maxX, frame.maxY);
    frame.regions = scrollRegions(*tree);
    const size_t itemCount = list->items.size();
    shared.PostFrame(std::move(frame));
    // SOLAR_GUI_SHOT=file.png: the frame is written to a file by the window, and the program ends (for looking at what it paints without a screen).
    // (SOLAR_GUI_SETTLE=ms: not before the pictures had that long to come.)
    const char* settle = std::getenv("SOLAR_GUI_SETTLE");
    if (autoScroll > 0 || (std::getenv("SOLAR_GUI_SCRIPT") && (!script.empty() || scriptQuiet < 6))) {
    } else if (settle && std::getenv("SOLAR_GUI_SHOT") && Clock::now() - programStart < std::chrono::milliseconds(std::atoi(settle))) {
      dirty = true;  // (again, until the time is up)
    } else if (const char* shot = settle && Clock::now() - programStart < std::chrono::milliseconds(std::atoi(settle)) ? nullptr : std::getenv("SOLAR_GUI_SHOT")) {
      UiCommand command;
      command.kind = UiCommand::Kind::Shot;
      command.text = shot;
      shared.Post(std::move(command));
      // (The window ends the program once it has written the file.)
      for (int i = 0; i < 1000 && !shared.quit.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
      running = false;
    }
    if (timing) {
      const auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
      std::fprintf(stderr, "frame: layout %.1f ms, display list %.1f ms (%zu items), step %.1f, iteration %.1f\n", ms(t0, t1), ms(t1, t2), itemCount, stepMs, ms(iterStart, Clock::now()));
    }
    if (scrollTest && autoScroll == 0) running = false;
  }

  shared.quit = true;
  solar::html::SetFrameEnvironment(nullptr);
  return 0;
}

}  // namespace
