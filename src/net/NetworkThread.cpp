/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/net/NetworkThread.h"

#include <condition_variable>
#include <future>
#include <thread>
#include <unordered_map>

#if defined(__linux__)
#include <pthread.h>
#endif

namespace solar::net {

namespace {

// What the service makes of what it was asked for, on its own thread.
struct NetFetch;

}  // namespace

struct ThreadedNetworkService::Impl {
  Loop& pageLoop;
  // The page's side: only touched from the thread the pages are on.
  struct PageFetch {
    FetchHandler* handler = nullptr;
    uint32_t epoch = 0;  // what came of an older one (before a restart) is not told
  };
  uint64_t nextId = 1;
  std::unordered_map<uint64_t, PageFetch> fetches;
  std::shared_ptr<bool> alive = std::make_shared<bool>(true);  // read on the page's thread only

  // The service's side: made on its thread and touched from it only.
  std::thread thread;
  std::unique_ptr<Loop> netLoop;
  std::unique_ptr<HttpClient> client;
  std::unordered_map<uint64_t, std::unique_ptr<NetFetch>> netFetches;

  explicit Impl(Loop& loop) : pageLoop(loop) {}

  // Tells the page of something, in the order it happened: `tell` is called, on the page's thread, with the handler of the fetch, unless the fetch is gone or the telling is of a
  // response asked for again since.
  void Tell(uint64_t id, uint32_t epoch, std::function<void(PageFetch&)> tell) {
    pageLoop.Post([this, alive = alive, id, epoch, tell = std::move(tell)] {
      if (!*alive) return;
      auto it = fetches.find(id);
      if (it == fetches.end() || epoch < it->second.epoch) return;
      tell(it->second);
    });
  }

  // The end of a fetch, on the page's side: it is let go and the loop is no longer kept waiting for it.
  void Ended(uint64_t id) {
    if (fetches.erase(id)) pageLoop.EndExternalWork();
  }
};

namespace {

struct NetFetch final : FetchHandler {
  ThreadedNetworkService::Impl& impl;
  const uint64_t id;
  FetchHandle handle;
  uint32_t epoch = 0;

  NetFetch(ThreadedNetworkService::Impl& i, uint64_t number) : impl(i), id(number) {}

  bool DefersRedirects() const override { return true; }
  void OnReceiveRedirect(const HttpResponseHead& redirect, const url::Url& next, const std::vector<std::pair<std::string, std::string>>& headers) override {
    impl.Tell(id, epoch, [this_id = id, &impl = impl, redirect, next, headers](ThreadedNetworkService::Impl::PageFetch& page) mutable {
      FetchHandler* handler = page.handler;
      if (handler->DefersRedirects()) {
        handler->OnReceiveRedirect(redirect, next, headers);
        return;
      }
      // (A handler that answers at once is asked here, in the middle of its answers; what it does to the headers goes back with the answer. It may cancel the fetch itself, or
      // end it: nothing here touches it after.)
      std::vector<std::pair<std::string, std::string>> next_headers = headers;
      const bool follow = handler->OnRedirect(redirect, next, next_headers);
      // (The answer goes to the service of the thread the fetch is made on.)
      impl.netLoop->Post([&impl, this_id, follow, next_headers = std::move(next_headers)]() mutable {
        auto found = impl.netFetches.find(this_id);
        if (found != impl.netFetches.end()) impl.client->FollowRedirect(found->second->handle.id(), follow, std::move(next_headers));
      });
    });
  }

  bool DefersRestarts() const override { return true; }
  void OnRestartRequested() override {
    impl.Tell(id, epoch, [this_id = id, &impl = impl](ThreadedNetworkService::Impl::PageFetch& page) {
      FetchHandler* handler = page.handler;
      if (handler->DefersRestarts()) {
        handler->OnRestartRequested();
        return;
      }
      const bool allow = handler->OnRestart();
      if (allow) ++page.epoch;
      impl.netLoop->Post([&impl, this_id, allow] {
        auto found = impl.netFetches.find(this_id);
        if (found == impl.netFetches.end()) return;
        if (allow) ++found->second->epoch;  // (what comes from here on is of the response asked for again)
        impl.client->AnswerRestart(found->second->handle.id(), allow);
      });
    });
  }

  void OnResponseHead(const HttpResponseHead& head, const url::Url& finalUrl) override {
    impl.Tell(id, epoch, [head, finalUrl](ThreadedNetworkService::Impl::PageFetch& page) { page.handler->OnResponseHead(head, finalUrl); });
  }
  void OnBody(std::span<const uint8_t> data) override {
    impl.Tell(id, epoch, [bytes = std::vector<uint8_t>(data.begin(), data.end())](ThreadedNetworkService::Impl::PageFetch& page) { page.handler->OnBody(bytes); });
  }
  void OnEnd() override {
    const uint64_t number = id;
    impl.Tell(id, epoch, [&impl = impl, number](ThreadedNetworkService::Impl::PageFetch& page) {
      FetchHandler* handler = page.handler;
      impl.Ended(number);
      handler->OnEnd();
    });
    Finished();
  }
  void OnError(std::string_view message) override {
    const uint64_t number = id;
    impl.Tell(id, epoch, [&impl = impl, number, message = std::string(message)](ThreadedNetworkService::Impl::PageFetch& page) {
      FetchHandler* handler = page.handler;
      impl.Ended(number);
      handler->OnError(message);
    });
    Finished();
  }

  // The fetch is over: this is let go, from the loop and not from inside its own call.
  void Finished() {
    const uint64_t number = id;
    ThreadedNetworkService::Impl* owner = &impl;
    impl.netLoop->Post([owner, number] { owner->netFetches.erase(number); });
  }
};

}  // namespace

ThreadedNetworkService::ThreadedNetworkService(Loop& pageLoop, HttpClientOptions options) : impl_(std::make_unique<Impl>(pageLoop)) {
  std::promise<void> ready;
  Impl* impl = impl_.get();
  impl->thread = std::thread([impl, options = std::move(options), &ready]() mutable {
#if defined(__linux__)
    pthread_setname_np(pthread_self(), "solar-net");
#endif
    impl->netLoop = Loop::Create();
    impl->client = std::make_unique<HttpClient>(*impl->netLoop, std::move(options));
    impl->netLoop->BeginExternalWork();  // (the loop runs until it is told to stop)
    ready.set_value();
    impl->netLoop->Run();
    // (The client and the loop are let go by the destructor, once this has ended: a Post that is still on its way in is not left with a loop that is gone.)
  });
  ready.get_future().wait();
}

ThreadedNetworkService::~ThreadedNetworkService() {
  Impl* impl = impl_.get();
  *impl->alive = false;
  impl->netLoop->Post([impl] {
    impl->netLoop->EndExternalWork();
    impl->netLoop->Stop();
  });
  impl->thread.join();
  impl->netFetches.clear();
  impl->client.reset();
  impl->netLoop.reset();
  // (What was in flight is let go: the loop of the page is no longer kept waiting for it.)
  for (size_t i = 0; i < impl->fetches.size(); ++i) impl->pageLoop.EndExternalWork();
  impl->fetches.clear();
}

FetchHandle ThreadedNetworkService::Fetch(const url::Url& url, FetchHandler& handler, FetchOptions options) {
  const uint64_t id = impl_->nextId++;
  impl_->fetches[id].handler = &handler;
  impl_->pageLoop.BeginExternalWork();
  Impl* impl = impl_.get();
  impl->netLoop->Post([impl, id, url, options = std::move(options)]() mutable {
    auto fetch = std::make_unique<NetFetch>(*impl, id);
    NetFetch* raw = fetch.get();
    impl->netFetches[id] = std::move(fetch);
    raw->handle = impl->client->Fetch(url, *raw, std::move(options));
  });
  return MakeHandle(this, id);
}

void ThreadedNetworkService::Cancel(uint64_t id) {
  const auto it = impl_->fetches.find(id);
  if (it == impl_->fetches.end()) return;
  FetchHandler* handler = it->second.handler;
  impl_->Ended(id);
  Impl* impl = impl_.get();
  impl->netLoop->Post([impl, id] {
    auto found = impl->netFetches.find(id);
    if (found != impl->netFetches.end()) found->second->handle.Cancel();
  });
  // (As the client has it: the handler hears of the end before the call returns.)
  handler->OnError("aborted");
}

void ThreadedNetworkService::FollowRedirect(uint64_t id, bool follow, std::vector<std::pair<std::string, std::string>> headers) {
  if (!impl_->fetches.count(id)) return;
  Impl* impl = impl_.get();
  impl->netLoop->Post([impl, id, follow, headers = std::move(headers)]() mutable {
    auto found = impl->netFetches.find(id);
    if (found != impl->netFetches.end()) impl->client->FollowRedirect(found->second->handle.id(), follow, std::move(headers));
  });
}

void ThreadedNetworkService::AnswerRestart(uint64_t id, bool allow) {
  const auto it = impl_->fetches.find(id);
  if (it == impl_->fetches.end()) return;
  if (allow) ++it->second.epoch;  // (what was on its way of the response before is not told)
  Impl* impl = impl_.get();
  impl->netLoop->Post([impl, id, allow] {
    auto found = impl->netFetches.find(id);
    if (found == impl->netFetches.end()) return;
    if (allow) ++found->second->epoch;
    impl->client->AnswerRestart(found->second->handle.id(), allow);
  });
}

void ThreadedNetworkService::CloseIdleConnections() {
  Impl* impl = impl_.get();
  impl->netLoop->Post([impl] { impl->client->CloseIdleConnections(); });
}

}  // namespace solar::net
