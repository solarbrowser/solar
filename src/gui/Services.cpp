/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "Services.h"

#include <atomic>
#include <cctype>
#include <fstream>
#include <future>
#include <map>
#include <sstream>
#include <thread>

#include "solar/net/HttpClient.h"
#include "solar/net/Loop.h"
#include "solar/net/NetworkThread.h"
#include "solar/net/RemoteNetwork.h"
#include "solar/url/Origin.h"
#include "solar/url/Parser.h"
#include "solar/url/Serializer.h"

namespace solar::gui {

std::string FileOfAddress(const std::string& address) {
  std::string path = address.size() > 7 ? address.substr(7) : "";  // file://
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

struct BrowserServices::Impl {
  std::unique_ptr<net::Loop> loop = net::Loop::Create();
  std::thread thread;
  std::unique_ptr<net::ThreadedNetworkService> network;
};

namespace {

constexpr size_t kMostNavigations = 64;
constexpr size_t kLargestFile = size_t{256} << 20;

}  // namespace

struct BrowserServices::Tab::Impl {
  BrowserServices::Impl& services;
  net::ipc::Channel& link;
  const bool allowFiles;
  std::unique_ptr<net::NetworkServiceHost> host;
  std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
  std::weak_ptr<Impl> self;

  // One navigation the page asked for (on the thread of the services' loop).
  struct Navigation : net::FetchHandler {
    Impl* tab = nullptr;
    uint32_t id = 0;
    net::FetchHandle handle;
    bool bodySent = false, ended = false;

    void Send(wire::ToRenderer type, const std::string& bytes) { tab->link.SendRaw(static_cast<uint8_t>(type), bytes); }
    void Head(int status, const std::string& contentType, const std::string& address) {
      net::ipc::Writer out;
      wire::Write(out, wire::NavHead{id, status, contentType, address});
      Send(wire::ToRenderer::NavHead, out.bytes());
    }
    void Body(std::string_view data) {
      bodySent = true;
      net::ipc::Writer out;
      out.U32(id);
      out.String(data);
      Send(wire::ToRenderer::NavBody, out.bytes());
    }
    void End() {
      ended = true;
      net::ipc::Writer out;
      out.U32(id);
      Send(wire::ToRenderer::NavEnd, out.bytes());
      tab->Done(id);
    }
    void Error(std::string_view message) {
      ended = true;
      net::ipc::Writer out;
      out.U32(id);
      out.String(message);
      Send(wire::ToRenderer::NavError, out.bytes());
      tab->Done(id);
    }

    void OnResponseHead(const net::HttpResponseHead& head, const url::Url& finalUrl) override {
      // (The page may ask for the origin it is going to before it has the document: the first thing it does with it.)
      tab->host->AllowOrigin(url::SerializeOrigin(finalUrl));
      std::string type;
      if (auto value = head.Header("content-type")) type = std::string(*value);
      Head(head.status, type, url::Serialize(finalUrl));
    }
    void OnBody(std::span<const uint8_t> data) override { Body(std::string_view(reinterpret_cast<const char*>(data.data()), data.size())); }
    void OnEnd() override { End(); }
    void OnError(std::string_view message) override { Error(message); }
    bool OnRestart() override {
      // (Asked for again from the start, as long as the page has none of the body to take back.)
      if (bodySent) return false;
      net::ipc::Writer out;
      out.U32(id);
      Send(wire::ToRenderer::NavRestart, out.bytes());
      return true;
    }
  };
  std::map<uint32_t, std::unique_ptr<Navigation>> navigations;  // (the services' loop only)

  Impl(BrowserServices::Impl& s, net::ipc::Channel& l, bool files) : services(s), link(l), allowFiles(files) {}

  // The navigation is over: it is let go of (not from inside its own call).
  void Done(uint32_t id) {
    services.loop->Post([keep = self.lock(), alive = alive, id] {
      if (keep && alive->load()) keep->navigations.erase(id);
    });
  }

  void Start(wire::Navigate request) {
    if (!alive->load()) return;
    const auto refuse = [&](const char* why) {
      net::ipc::Writer out;
      out.U32(request.id);
      out.String(why);
      link.SendRaw(static_cast<uint8_t>(wire::ToRenderer::NavError), out.bytes());
    };
    if (navigations.size() >= kMostNavigations || navigations.count(request.id)) return refuse("too many navigations");
    std::optional<url::Url> parsed = url::Parse(request.url);
    if (!parsed) return refuse("invalid address");
    auto navigation = std::make_unique<Navigation>();
    navigation->tab = this;
    navigation->id = request.id;
    Navigation* raw = navigation.get();
    if (parsed->scheme == "file") {
      if (!allowFiles) return refuse("this page may not open files");
      navigations[request.id] = std::move(navigation);
      std::ifstream file(FileOfAddress(request.url), std::ios::binary);
      std::stringstream contents;
      if (!file) return raw->Error("cannot read the file");
      contents << file.rdbuf();
      const std::string text = contents.str();
      if (text.size() > kLargestFile) return raw->Error("the file is too large");
      host->AllowOrigin(url::SerializeOrigin(*parsed));
      raw->Head(200, "text/html", request.url);
      if (!text.empty()) raw->Body(text);
      raw->End();
      return;
    }
    if (parsed->scheme != "http" && parsed->scheme != "https") return refuse("unsupported scheme");
    net::FetchOptions options;
    if (request.method == "POST") {
      options.method = "POST";
      options.body = std::make_shared<const std::string>(std::move(request.body));
      if (!request.bodyType.empty()) options.headers.emplace_back("Content-Type", request.bodyType);
    }
    navigations[request.id] = std::move(navigation);
    raw->handle = services.network->Fetch(*parsed, *raw, std::move(options));
  }

  void Close() {
    for (auto& [id, navigation] : navigations) {
      if (!navigation->ended) navigation->handle.Cancel();
    }
    navigations.clear();
  }
};

BrowserServices::BrowserServices() : impl_(std::make_unique<Impl>()) {
  Impl* impl = impl_.get();
  impl->network = std::make_unique<net::ThreadedNetworkService>(*impl->loop);
  std::promise<void> ready;
  impl->thread = std::thread([impl, &ready] {
    impl->loop->BeginExternalWork();
    ready.set_value();
    impl->loop->Run();
  });
  ready.get_future().wait();
}

BrowserServices::~BrowserServices() {
  // (The network is let go of on the loop's thread, where what it tells is run: it is that thread's.)
  std::promise<void> flushed;
  impl_->loop->Post([&flushed, impl = impl_.get()] {
    impl->network.reset();
    flushed.set_value();
  });
  flushed.get_future().wait();
  impl_->loop->Post([impl = impl_.get()] {
    impl->loop->EndExternalWork();
    impl->loop->Stop();
  });
  impl_->thread.join();
}

std::unique_ptr<BrowserServices::Tab> BrowserServices::OpenTab(int networkFd, net::ipc::Channel& link, bool allowFiles) {
  auto tab = std::unique_ptr<Tab>(new Tab());
  tab->impl_ = std::make_shared<Tab::Impl>(*impl_, link, allowFiles);
  tab->impl_->self = tab->impl_;
  // (No origin is allowed until the first document comes.)
  tab->impl_->host = std::make_unique<net::NetworkServiceHost>(*impl_->network, *impl_->loop, networkFd, "");
  return tab;
}

BrowserServices::Tab::~Tab() {
  // What the tab had is ended on the loop's thread (where it is kept), which holds the state until it has.
  impl_->alive->store(false);
  std::promise<void> closed;
  impl_->services.loop->Post([keep = impl_, &closed] {
    keep->Close();
    keep->host.reset();
    closed.set_value();
  });
  closed.get_future().wait();
}

void BrowserServices::Tab::Navigate(wire::Navigate request) {
  impl_->services.loop->Post([keep = impl_, request = std::move(request)]() mutable { keep->Start(std::move(request)); });
}

void BrowserServices::Tab::Committed(const std::string& address) {
  std::optional<url::Url> parsed = url::Parse(address);
  if (!parsed) return;
  const std::string origin = url::SerializeOrigin(*parsed);
  impl_->services.loop->Post([keep = impl_, origin] {
    if (keep->alive->load() && keep->host) keep->host->KeepOnlyOrigin(origin);
  });
}

}  // namespace solar::gui
