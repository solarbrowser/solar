/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/gui/Navigation.h"

#include "solar/css/HideRules.h"
#include "solar/dom/Node.h"
#include "solar/net/ContentFilter.h"
#include "solar/net/Http1Parser.h"
#include "solar/url/UrlApi.h"
#include "solar/url/Parser.h"
#include "solar/web/Scripts.h"

#include <map>
#include "solar/url/Serializer.h"

namespace solar::gui {

namespace {

// The cosmetic rules of the content blocker (read from the lists on the first page that is hidden something from), and what each host of this process's pages has hidden.
const net::CosmeticFilter& Cosmetics() {
  static const net::CosmeticFilter* filter = [] {
    auto* f = new net::CosmeticFilter;
    for (const char* const* piece = web::kScriptFilterLists; *piece; ++piece) f->AddList(*piece);
    f->Finish();
    return f;
  }();
  return *filter;
}

const css::HideRules* HidesOf(dom::Document* document) {
  // (Asked for every element's style: the last answer is kept for the document and its address.)
  static dom::Document* lastDocument = nullptr;
  static std::string lastAddress;
  static const css::HideRules* lastRules = nullptr;
  if (document == lastDocument && document->url == lastAddress) return lastRules;
  static std::map<std::string, std::shared_ptr<const css::HideRules>> kept;  // (the page's thread only; kept as long as the process: a few hosts)
  const css::HideRules* rules = nullptr;
  const std::optional<url::Url> address = url::Parse(document->url);
  if (address && (address->scheme == "http" || address->scheme == "https")) {
    const std::string host = url::GetHostname(*address);
    auto found = kept.find(host);
    if (found == kept.end() && kept.size() < 1000) found = kept.emplace(host, Cosmetics().ForHost(host)).first;
    if (found != kept.end()) rules = found->second.get();
  }
  lastDocument = document;
  lastAddress = document->url;
  lastRules = rules;
  return rules;
}

}  // namespace

uint32_t RemoteNavigation::Fetch(const url::Url& address, net::FetchHandler& handler, const net::FetchOptions& options, wire::Navigate::Kind kind, int entry) {
  const uint32_t id = next_++;
  pending_[id] = &handler;
  wire::Navigate request;
  request.id = id;
  request.kind = kind;
  request.entry = entry;
  request.url = url::Serialize(address);
  request.method = options.method == "POST" ? "POST" : "GET";
  if (options.body) request.body = *options.body;
  for (const auto& header : options.headers) {
    if (header.first == "Content-Type") request.bodyType = header.second;
  }
  net::ipc::Writer out;
  wire::Write(out, request);
  if (!link_.SendRaw(static_cast<uint8_t>(wire::ToBrowser::Navigate), out.bytes())) {
    // (The browser is gone: it ends with an error, as a fetch that fails does, from the loop.)
    loop_.Post([this, id] {
      const auto found = pending_.find(id);
      if (found == pending_.end()) return;
      net::FetchHandler* handler = found->second;
      pending_.erase(found);
      handler->OnError("the browser is gone");
    });
  }
  return id;
}

void RemoteNavigation::HistoryGo(int delta) {
  net::ipc::Writer out;
  out.I32(delta);
  link_.SendRaw(static_cast<uint8_t>(wire::ToBrowser::HistoryGo), out.bytes());
}

void RemoteNavigation::Committed(const std::string& address, uint32_t navigation) {
  net::ipc::Writer out;
  out.String(address);
  out.U32(navigation);
  link_.SendRaw(static_cast<uint8_t>(wire::ToBrowser::NavCommitted), out.bytes());
}

void RemoteNavigation::Deliver(wire::ToRenderer type, std::string bytes) {
  loop_.Post([this, type, bytes = std::move(bytes)] { Handle(type, bytes); });
}

void RemoteNavigation::Handle(wire::ToRenderer type, const std::string& bytes) {
  net::ipc::Reader in(bytes);
  switch (type) {
    case wire::ToRenderer::UiState:
      if (ui_) ui_(bytes);
      return;
    case wire::ToRenderer::Find:
      if (find_) find_(bytes);
      return;
    case wire::ToRenderer::Blocking:
      css::SetHideProvider(HidesOf);
      css::SetContentHiding(!bytes.empty() && bytes[0] == 1);
      return;
    case wire::ToRenderer::NavHead: {
      wire::NavHead head;
      if (!wire::Read(in, head)) return;
      const auto found = pending_.find(head.id);
      std::optional<url::Url> where = url::Parse(head.finalUrl);
      if (found == pending_.end() || !where) return;
      net::HttpResponseHead response;
      response.status = head.status;
      if (!head.contentType.empty()) response.headers.emplace_back("content-type", head.contentType);
      found->second->OnResponseHead(response, *where);
      return;
    }
    case wire::ToRenderer::NavBody: {
      const uint32_t id = in.U32();
      const std::string data = in.String(size_t{64} << 20);
      const auto found = pending_.find(id);
      if (!in.ok() || found == pending_.end()) return;
      found->second->OnBody(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(data.data()), data.size()));
      return;
    }
    case wire::ToRenderer::NavEnd: {
      const uint32_t id = in.U32();
      const auto found = pending_.find(id);
      if (!in.ok() || found == pending_.end()) return;
      net::FetchHandler* handler = found->second;
      pending_.erase(found);
      handler->OnEnd();
      return;
    }
    case wire::ToRenderer::NavError: {
      const uint32_t id = in.U32();
      const std::string message = in.String(1 << 16);
      const auto found = pending_.find(id);
      if (!in.ok() || found == pending_.end()) return;
      net::FetchHandler* handler = found->second;
      pending_.erase(found);
      handler->OnError(message);
      return;
    }
    case wire::ToRenderer::NavTransferred: {
      const uint32_t id = in.U32();
      const auto found = pending_.find(id);
      if (!in.ok() || found == pending_.end()) return;
      net::FetchHandler* handler = found->second;
      pending_.erase(found);
      handler->OnError(kTransferred);
      return;
    }
    case wire::ToRenderer::GoTo: {
      const std::string address = in.String(size_t{1} << 21);
      const int entry = in.I32();
      if (in.ok() && goTo_) goTo_(address, entry);
      return;
    }
    case wire::ToRenderer::NavRestart: {
      const uint32_t id = in.U32();
      const auto found = pending_.find(id);
      if (!in.ok() || found == pending_.end()) return;
      found->second->OnRestart();
      return;
    }
    default: return;
  }
}

}  // namespace solar::gui
