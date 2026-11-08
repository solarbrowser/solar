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
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include "solar/net/HttpClient.h"
#include "solar/net/Loop.h"
#include "solar/net/NetworkThread.h"
#include "solar/net/RemoteNetwork.h"
#include "solar/url/Origin.h"
#include "solar/web-api/Storage.h"
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
  std::atomic<std::thread::id> loopThread{};  // (what runs on the loop's thread need not wait its turn)
  std::unique_ptr<net::ThreadedNetworkService> network;
  // What the pages store is read and written on a thread of its own (the disk is waited for there and not on the loop of the network).
  webapi::DiskStorage disk{webapi::StorageDirectory()};
  std::thread storageThread;
  std::mutex storageMutex;
  std::condition_variable storageWake;
  std::deque<std::function<void()>> storageJobs;
  bool storageStop = false;
  void StorageLoop() {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock<std::mutex> lock(storageMutex);
        storageWake.wait(lock, [&] { return storageStop || !storageJobs.empty(); });
        if (storageJobs.empty()) return;
        job = std::move(storageJobs.front());
        storageJobs.pop_front();
      }
      job();
    }
  }
  void DoStorage(std::function<void()> job) {
    {
      std::lock_guard<std::mutex> lock(storageMutex);
      storageJobs.push_back(std::move(job));
    }
    storageWake.notify_one();
  }
};

struct BrowserServices::Tab::Impl {
  BrowserServices::Impl& services;
  net::ipc::Channel& link;
  std::unique_ptr<net::NetworkServiceHost> host;
  std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
  std::weak_ptr<Impl> self;

  // The origins the page may ask for, here as well as in the network's host (the storage is asked of the browser with an origin, and the same set says which it may be).
  std::mutex originsMutex;
  std::set<std::string> origins;
  void AllowOrigin(const std::string& origin) {
    {
      std::lock_guard<std::mutex> lock(originsMutex);
      origins.insert(origin);
    }
    host->AllowOrigin(origin);
  }
  void KeepOnlyOrigin(const std::string& origin) {
    {
      std::lock_guard<std::mutex> lock(originsMutex);
      if (!origins.count(origin)) return;
      origins = {origin};
    }
    host->KeepOnlyOrigin(origin);
  }
  bool MayHave(const std::string& origin) {
    std::lock_guard<std::mutex> lock(originsMutex);
    return origins.count(origin) > 0;
  }

  Impl(BrowserServices::Impl& s, net::ipc::Channel& l) : services(s), link(l) {}
};

BrowserServices::BrowserServices() : impl_(std::make_unique<Impl>()) {
  Impl* impl = impl_.get();
  impl->network = std::make_unique<net::ThreadedNetworkService>(*impl->loop);
  std::promise<void> ready;
  impl->storageThread = std::thread([impl] { impl->StorageLoop(); });
  impl->thread = std::thread([impl, &ready] {
    impl->loopThread = std::this_thread::get_id();
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
  {
    std::lock_guard<std::mutex> lock(impl_->storageMutex);
    impl_->storageStop = true;
  }
  impl_->storageWake.notify_all();
  impl_->storageThread.join();
}

std::unique_ptr<BrowserServices::Tab> BrowserServices::OpenTab(int networkFd, net::ipc::Channel& link) {
  auto tab = std::unique_ptr<Tab>(new Tab());
  tab->impl_ = std::make_shared<Tab::Impl>(*impl_, link);
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
    keep->host.reset();
    closed.set_value();
  });
  closed.get_future().wait();
}

// (From the loop's thread the origin is allowed at once, so that it is the page's before the page is told it has the document: its first request is not behind it.)
void BrowserServices::Tab::AllowOrigin(const std::string& origin) {
  if (impl_->services.loopThread.load() == std::this_thread::get_id()) {
    if (impl_->alive->load() && impl_->host) impl_->AllowOrigin(origin);
    return;
  }
  impl_->services.loop->Post([keep = impl_, origin] {
    if (keep->alive->load() && keep->host) keep->AllowOrigin(origin);
  });
}

void BrowserServices::Tab::KeepOnlyOrigin(const std::string& origin) {
  if (impl_->services.loopThread.load() == std::this_thread::get_id()) {
    if (impl_->alive->load() && impl_->host) impl_->KeepOnlyOrigin(origin);
    return;
  }
  impl_->services.loop->Post([keep = impl_, origin] {
    if (keep->alive->load() && keep->host) keep->KeepOnlyOrigin(origin);
  });
}

void BrowserServices::Tab::Storage(wire::StorageRequest request) {
  std::shared_ptr<Impl> keep = impl_;
  const auto area = static_cast<webapi::StorageArea>(request.area);
  // (The origin is the page's own, or the registrations of a worker, which are read to see whether a navigation is for one.)
  const bool registrations = area == webapi::StorageArea::Kv && request.op == wire::StorageRequest::Op::Read && request.file == webapi::StorageId(area, "sw-registrations");
  const bool allowed = keep->MayHave(request.origin) || registrations;
  keep->services.DoStorage([keep, request = std::move(request), area, allowed] {
    if (!keep->alive->load()) return;
    bool ok = false;
    std::string data;
    if (allowed) {
      webapi::DiskStorage& disk = keep->services.disk;
      switch (request.op) {
        case wire::StorageRequest::Op::Read:
          if (auto bytes = disk.Read(area, request.origin, request.file, request.maxBytes)) {
            ok = true;
            data = std::move(*bytes);
          }
          break;
        case wire::StorageRequest::Op::Write: ok = disk.Write(area, request.origin, request.file, request.data); break;
        case wire::StorageRequest::Op::Remove: ok = disk.Remove(area, request.origin, request.file); break;
        case wire::StorageRequest::Op::List:
          ok = true;
          for (const std::string& id : disk.List(area, request.origin)) data += id + "\n";
          break;
      }
    }
    net::ipc::Writer out;
    out.U32(request.id);
    out.Bool(ok);
    out.String(data);
    keep->link.SendRaw(static_cast<uint8_t>(wire::ToRenderer::StorageReply), out.bytes());
  });
}

void BrowserServices::Post(std::function<void()> task) { impl_->loop->Post(std::move(task)); }

net::FetchHandle BrowserServices::Fetch(const url::Url& address, net::FetchHandler& handler, net::FetchOptions options) { return impl_->network->Fetch(address, handler, std::move(options)); }

}  // namespace solar::gui
