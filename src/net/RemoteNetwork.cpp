/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/net/RemoteNetwork.h"

#include <atomic>
#include <thread>
#include <unordered_map>

#if defined(__linux__)
#include <pthread.h>
#endif

#include "solar/url/Origin.h"

namespace solar::net {

using ipc::Message;
using ipc::Reader;
using ipc::Writer;

// ===================== the page's end =====================

struct RemoteNetworkService::Impl {
  Loop& pageLoop;
  ipc::Channel channel;
  std::thread reader;
  // Touched from the page's thread only.
  std::unordered_map<uint64_t, PageFetchHandler*> fetches;
  std::unordered_map<uint64_t, WebSocketHandler*> sockets;
  uint64_t nextId = 1;
  std::shared_ptr<bool> alive = std::make_shared<bool>(true);

  Impl(Loop& loop, int fd) : pageLoop(loop), channel(fd) {}

  // What the service said, on the page's thread.
  void Deliver(Message type, const std::string& bytes) {
    Reader in(bytes);
    const uint64_t id = in.U64();
    switch (type) {
      case Message::FetchResponse: {
        PageResponse response;
        if (!ipc::Read(in, response)) return;
        const auto found = fetches.find(id);
        if (found != fetches.end()) found->second->OnResponse(response);
        return;
      }
      case Message::FetchBody: {
        const std::string data = in.String(ipc::Channel::kMaxMessage);
        const auto found = fetches.find(id);
        if (in.ok() && found != fetches.end()) found->second->OnBody(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(data.data()), data.size()));
        return;
      }
      case Message::FetchRestart: {
        const auto found = fetches.find(id);
        if (found != fetches.end()) found->second->OnRestart();
        return;
      }
      case Message::FetchEnd: {
        const auto found = fetches.find(id);
        if (found == fetches.end()) return;
        PageFetchHandler* handler = found->second;
        fetches.erase(found);
        pageLoop.EndExternalWork();
        handler->OnEnd();
        return;
      }
      case Message::FetchError: {
        const std::string message = in.String(1 << 16);
        const auto found = fetches.find(id);
        if (found == fetches.end()) return;
        PageFetchHandler* handler = found->second;
        fetches.erase(found);
        pageLoop.EndExternalWork();
        handler->OnError(message);
        return;
      }
      case Message::WebSocketOpen: {
        const std::string protocol = in.String(256), extensions = in.String(1 << 12);
        const auto found = sockets.find(id);
        if (in.ok() && found != sockets.end()) found->second->OnOpen(protocol, extensions);
        return;
      }
      case Message::WebSocketMessage: {
        std::string data = in.String(ipc::Channel::kMaxMessage);
        const bool binary = in.Bool();
        const auto found = sockets.find(id);
        if (in.ok() && found != sockets.end()) found->second->OnMessage(std::move(data), binary);
        return;
      }
      case Message::WebSocketClosed: {
        const bool clean = in.Bool();
        const uint16_t code = static_cast<uint16_t>(in.U32());
        const std::string reason = in.String(1 << 12), error = in.String(1 << 16);
        const auto found = sockets.find(id);
        if (!in.ok() || found == sockets.end()) return;
        WebSocketHandler* handler = found->second;
        sockets.erase(found);
        pageLoop.EndExternalWork();
        handler->OnClosed(clean, code, reason, error);
        return;
      }
      default: return;
    }
  }

  // The connection is gone: what is in flight ends.
  void Lost() {
    const auto pendingFetches = std::move(fetches);
    const auto pendingSockets = std::move(sockets);
    fetches.clear();
    sockets.clear();
    for (const auto& entry : pendingFetches) {
      pageLoop.EndExternalWork();
      entry.second->OnError("Failed to fetch (the network service is gone)");
    }
    for (const auto& entry : pendingSockets) {
      pageLoop.EndExternalWork();
      entry.second->OnClosed(false, 1006, "", "the network service is gone");
    }
  }
};

RemoteNetworkService::RemoteNetworkService(Loop& pageLoop, int fd) : impl_(std::make_unique<Impl>(pageLoop, fd)) {
  Impl* impl = impl_.get();
  impl->reader = std::thread([impl] {
#if defined(__linux__)
    pthread_setname_np(pthread_self(), "solar-net-ipc");
#endif
    Message type;
    std::string bytes;
    while (impl->channel.Receive(type, bytes)) {
      impl->pageLoop.Post([impl, alive = impl->alive, type, bytes = std::move(bytes)] {
        if (*alive) impl->Deliver(type, bytes);
      });
      bytes = std::string();
    }
    impl->pageLoop.Post([impl, alive = impl->alive] {
      if (*alive) impl->Lost();
    });
  });
}

RemoteNetworkService::~RemoteNetworkService() {
  Impl* impl = impl_.get();
  *impl->alive = false;
  impl->channel.Shutdown();
  impl->reader.join();
  for (size_t i = 0; i < impl->fetches.size() + impl->sockets.size(); ++i) impl->pageLoop.EndExternalWork();
}

FetchHandle RemoteNetworkService::StartFetch(PageRequest request, PageFetchHandler& handler) {
  const uint64_t id = impl_->nextId++;
  impl_->fetches[id] = &handler;
  impl_->pageLoop.BeginExternalWork();
  Writer out;
  out.U64(id);
  ipc::Write(out, request);
  if (!impl_->channel.Send(Message::StartFetch, out.bytes())) {
    // (The connection is gone: it is told from the loop, not from inside this call.)
    Impl* impl = impl_.get();
    impl->pageLoop.Post([impl, alive = impl->alive, id] {
      if (!*alive) return;
      const auto found = impl->fetches.find(id);
      if (found == impl->fetches.end()) return;
      PageFetchHandler* handler = found->second;
      impl->fetches.erase(found);
      impl->pageLoop.EndExternalWork();
      handler->OnError("Failed to fetch (the network service is gone)");
    });
  }
  return MakeHandle(this, id);
}

void RemoteNetworkService::Cancel(uint64_t id) {
  const auto found = impl_->fetches.find(id);
  if (found == impl_->fetches.end()) return;
  PageFetchHandler* handler = found->second;
  impl_->fetches.erase(found);
  impl_->pageLoop.EndExternalWork();
  Writer out;
  out.U64(id);
  impl_->channel.Send(Message::Cancel, out.bytes());
  handler->OnError("aborted");
}

WebSocketHandle RemoteNetworkService::ConnectWebSocket(const url::Url& url, WebSocketHandler& handler, WebSocketOptions options) {
  const uint64_t id = impl_->nextId++;
  impl_->sockets[id] = &handler;
  impl_->pageLoop.BeginExternalWork();
  Writer out;
  out.U64(id);
  out.Url(url);
  ipc::Write(out, options);
  if (!impl_->channel.Send(Message::WebSocketConnect, out.bytes())) {
    Impl* impl = impl_.get();
    impl->pageLoop.Post([impl, alive = impl->alive, id] {
      if (!*alive) return;
      const auto found = impl->sockets.find(id);
      if (found == impl->sockets.end()) return;
      WebSocketHandler* handler = found->second;
      impl->sockets.erase(found);
      impl->pageLoop.EndExternalWork();
      handler->OnClosed(false, 1006, "", "the network service is gone");
    });
  }
  return MakeWebSocketHandle(this, id);
}

void RemoteNetworkService::SendWebSocket(uint64_t id, std::string data, bool binary) {
  if (!impl_->sockets.count(id)) return;
  Writer out;
  out.U64(id);
  out.String(data);
  out.Bool(binary);
  impl_->channel.Send(Message::WebSocketSend, out.bytes());
}

void RemoteNetworkService::CloseWebSocket(uint64_t id, uint16_t code, std::string reason) {
  if (!impl_->sockets.count(id)) return;
  Writer out;
  out.U64(id);
  out.U32(code);
  out.String(reason);
  impl_->channel.Send(Message::WebSocketClose, out.bytes());
}

void RemoteNetworkService::AbortWebSocket(uint64_t id) {
  if (!impl_->sockets.count(id)) return;
  Writer out;
  out.U64(id);
  impl_->channel.Send(Message::WebSocketAbort, out.bytes());
}

// ===================== the service's end =====================

namespace {
struct HostFetch;
struct HostSocket;
}  // namespace

struct NetworkServiceHost::Impl {
  std::weak_ptr<Impl> self;  // (what a task of the host's loop holds, so that the state is there while it runs)
  BrowserNetwork& network;
  Loop& hostLoop;
  ipc::Channel channel;
  std::thread reader;
  std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);  // (read on the host's thread, written by the destructor)
  std::string origin;                          // the origin the page may ask for (read on the host's thread)
  std::atomic<size_t> refusedCount{0};
  // Touched from the host's thread only.
  std::unordered_map<uint64_t, std::unique_ptr<HostFetch>> fetches;
  std::unordered_map<uint64_t, std::unique_ptr<HostSocket>> sockets;
  static constexpr size_t kMaxInFlight = 4096;

  Impl(BrowserNetwork& n, Loop& loop, int fd, std::string o) : network(n), hostLoop(loop), channel(fd), origin(std::move(o)) {}

  void Handle(Message type, const std::string& bytes);
  // What the page had in flight ends (on the host's thread).
  void CloseAll();
  void RefuseFetch(uint64_t id, const std::string& why) {
    ++refusedCount;
    Writer out;
    out.U64(id);
    out.String(why);
    channel.Send(Message::FetchError, out.bytes());
  }
};

namespace {

// What the service hears of a fetch, said to the page.
struct HostFetch final : PageFetchHandler {
  NetworkServiceHost::Impl& impl;
  const uint64_t id;
  FetchHandle handle;
  HostFetch(NetworkServiceHost::Impl& i, uint64_t number) : impl(i), id(number) {}

  void OnResponse(const PageResponse& response) override {
    Writer out;
    out.U64(id);
    ipc::Write(out, response);
    impl.channel.Send(Message::FetchResponse, out.bytes());
  }
  void OnBody(std::span<const uint8_t> data) override {
    // (In pieces a message can hold.)
    constexpr size_t kPiece = size_t{4} << 20;
    for (size_t at = 0; at < data.size(); at += kPiece) {
      Writer out;
      out.U64(id);
      out.String(std::string_view(reinterpret_cast<const char*>(data.data()) + at, std::min(kPiece, data.size() - at)));
      impl.channel.Send(Message::FetchBody, out.bytes());
    }
  }
  void OnRestart() override {
    Writer out;
    out.U64(id);
    impl.channel.Send(Message::FetchRestart, out.bytes());
  }
  void OnEnd() override {
    Writer out;
    out.U64(id);
    impl.channel.Send(Message::FetchEnd, out.bytes());
    Finished();
  }
  void OnError(std::string_view message) override {
    Writer out;
    out.U64(id);
    out.String(message);
    impl.channel.Send(Message::FetchError, out.bytes());
    Finished();
  }
  void Finished() {
    NetworkServiceHost::Impl* owner = &impl;
    const uint64_t number = id;
    impl.hostLoop.Post([owner, alive = impl.alive, number] {
      if (alive->load()) owner->fetches.erase(number);
    });
  }
};

struct HostSocket final : WebSocketHandler {
  NetworkServiceHost::Impl& impl;
  const uint64_t id;
  WebSocketHandle handle;
  HostSocket(NetworkServiceHost::Impl& i, uint64_t number) : impl(i), id(number) {}

  void OnOpen(const std::string& protocol, const std::string& extensions) override {
    Writer out;
    out.U64(id);
    out.String(protocol);
    out.String(extensions);
    impl.channel.Send(Message::WebSocketOpen, out.bytes());
  }
  void OnMessage(std::string data, bool binary) override {
    Writer out;
    out.U64(id);
    out.String(data);
    out.Bool(binary);
    impl.channel.Send(Message::WebSocketMessage, out.bytes());
  }
  void OnClosed(bool clean, uint16_t code, const std::string& reason, const std::string& error) override {
    Writer out;
    out.U64(id);
    out.Bool(clean);
    out.U32(code);
    out.String(reason);
    out.String(error);
    impl.channel.Send(Message::WebSocketClosed, out.bytes());
    NetworkServiceHost::Impl* owner = &impl;
    const uint64_t number = id;
    impl.hostLoop.Post([owner, alive = impl.alive, number] {
      if (alive->load()) owner->sockets.erase(number);
    });
  }
};

}  // namespace

void NetworkServiceHost::Impl::CloseAll() {
  for (auto& [id, fetch] : fetches) fetch->handle.Cancel();
  for (auto& [id, socket] : sockets) socket->handle.Abort();
}

void NetworkServiceHost::Impl::Handle(Message type, const std::string& bytes) {
  Reader in(bytes);
  const uint64_t id = in.U64();
  if (!in.ok()) return;
  switch (type) {
    case Message::StartFetch: {
      PageRequest request;
      if (!ipc::Read(in, request) || !in.AtEnd()) { RefuseFetch(id, "Failed to fetch (the request was not understood)"); return; }
      if (fetches.count(id) || fetches.size() >= kMaxInFlight) { RefuseFetch(id, "Failed to fetch (too many requests)"); return; }
      // A page asks for the account of its own origin and no other's: what it names as its address is checked against what it was made for.
      if (url::SerializeOrigin(request.pageUrl) != origin) { RefuseFetch(id, "Failed to fetch (the page is not the one this was made for)"); return; }
      if (request.url.scheme != "http" && request.url.scheme != "https") { RefuseFetch(id, "Failed to fetch (not an address of the network)"); return; }
      auto fetch = std::make_unique<HostFetch>(*this, id);
      HostFetch* raw = fetch.get();
      fetches[id] = std::move(fetch);
      raw->handle = network.StartFetch(std::move(request), *raw);
      return;
    }
    case Message::Cancel: {
      const auto found = fetches.find(id);
      if (found != fetches.end()) found->second->handle.Cancel();
      return;
    }
    case Message::WebSocketConnect: {
      auto address = in.ReadUrl();
      WebSocketOptions options;
      if (!address || !ipc::Read(in, options) || !in.AtEnd() || sockets.count(id) || sockets.size() >= kMaxInFlight ||
          !options.initiator || url::SerializeOrigin(*options.initiator) != origin) {
        ++refusedCount;
        Writer out;
        out.U64(id);
        out.Bool(false);
        out.U32(1006);
        out.String("");
        out.String("the connection was refused");
        channel.Send(Message::WebSocketClosed, out.bytes());
        return;
      }
      auto socket = std::make_unique<HostSocket>(*this, id);
      HostSocket* raw = socket.get();
      sockets[id] = std::move(socket);
      raw->handle = network.ConnectWebSocket(*address, *raw, std::move(options));
      return;
    }
    case Message::WebSocketSend: {
      std::string data = in.String(ipc::Channel::kMaxMessage);
      const bool binary = in.Bool();
      const auto found = sockets.find(id);
      if (in.ok() && found != sockets.end()) found->second->handle.Send(std::move(data), binary);
      return;
    }
    case Message::WebSocketClose: {
      const uint16_t code = static_cast<uint16_t>(in.U32());
      const std::string reason = in.String(256);
      const auto found = sockets.find(id);
      if (in.ok() && found != sockets.end()) found->second->handle.Close(code, reason);
      return;
    }
    case Message::WebSocketAbort: {
      const auto found = sockets.find(id);
      if (found != sockets.end()) found->second->handle.Abort();
      return;
    }
    default: return;  // (what a page may not say: nothing is done)
  }
}

NetworkServiceHost::NetworkServiceHost(BrowserNetwork& network, Loop& hostLoop, int fd, std::string lockedOrigin)
    : impl_(std::make_shared<Impl>(network, hostLoop, fd, std::move(lockedOrigin))) {
  impl_->self = impl_;
  Impl* impl = impl_.get();
  impl->reader = std::thread([impl] {
#if defined(__linux__)
    pthread_setname_np(pthread_self(), "solar-host-ipc");
#endif
    Message type;
    std::string bytes;
    while (impl->channel.Receive(type, bytes)) {
      impl->hostLoop.Post([keep = impl->self.lock(), alive = impl->alive, type, bytes = std::move(bytes)] {
        if (keep && alive->load()) keep->Handle(type, bytes);
      });
      bytes = std::string();
    }
  });
}

NetworkServiceHost::~NetworkServiceHost() {
  Impl* impl = impl_.get();
  impl->alive->store(false);
  impl->channel.Shutdown();
  impl->reader.join();
  // What the page had in flight ends, on the host's thread (where it is kept): the state is held by the task until it has run. The loop is to be running until it has, and the
  // network is to be there (a caller that stops them waits for the loop to have gone through what was posted before).
  std::shared_ptr<Impl> keep = std::move(impl_);
  keep->hostLoop.Post([keep] { keep->CloseAll(); });
}

void NetworkServiceHost::SetOrigin(std::string origin) {
  Impl* impl = impl_.get();
  impl->hostLoop.Post([keep = impl_, origin = std::move(origin)]() mutable {
    if (keep->alive->load()) keep->origin = std::move(origin);
  });
}

size_t NetworkServiceHost::refused() const { return impl_->refusedCount.load(); }

}  // namespace solar::net
