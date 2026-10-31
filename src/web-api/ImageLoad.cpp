/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/web-api/ImageLoad.h"

#include <cstring>
#include <map>
#include <memory>

#include "solar/net/HttpClient.h"
#include "solar/url/Parser.h"
#include "solar/web/FetchHost.h"

namespace solar::webapi {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::Value;

namespace {

char g_key;

struct State;

struct Transfer final : net::FetchHandler {
  State* owner = nullptr;
  uint32_t id = 0;
  int status = 0;
  std::string contentType, body;
  bool cancelled = false;
  void OnResponseHead(const net::HttpResponseHead& head, const url::Url&) override {
    status = head.status;
    if (auto type = head.Header("content-type")) contentType = std::string(*type);
  }
  void OnBody(std::span<const uint8_t> data) override { body.append(reinterpret_cast<const char*>(data.data()), data.size()); }
  void OnEnd() override;
  void OnError(std::string_view) override;
};

struct State {
  State(Context& c, web::FetchHost& h) : ctx(&c), host(&h) {}
  Context* ctx;
  web::FetchHost* host;
  struct Item {
    std::unique_ptr<Transfer> transfer;
    net::FetchHandle handle;
    qe::Persistent callback;
  };
  std::map<uint32_t, Item> items;
  uint32_t next = 1;
  std::shared_ptr<bool> alive = std::make_shared<bool>(true);

  void Finish(uint32_t id, bool ok) {
    const auto found = items.find(id);
    if (found == items.end() || !*alive) return;
    Transfer& t = *found->second.transfer;
    Context& c = *ctx;
    Value arguments[3];
    arguments[0] = qe::FromNumber(ok ? t.status : 0);
    arguments[1] = qe::FromWtf8(c, t.contentType);
    if (ok) {
      Value buffer = qe::NewArrayBuffer(c, t.body.size());
      if (!t.body.empty()) {
        if (auto out = qe::MutableBytesOf(buffer)) std::memcpy(out->data(), t.body.data(), t.body.size());
      }
      arguments[2] = buffer;
    } else {
      arguments[2] = qe::Undefined();
    }
    qe::Persistent callback = std::move(found->second.callback);
    // (The item goes later: this is called from inside the fetch's own call.)
    host->config().loop->Post([this, id, alive = alive] {
      if (*alive) items.erase(id);
    });
    qe::Call(c, callback.Get(), qe::Undefined(), qe::Args(arguments, 3));
    if (qe::HasException(c)) c.clear_exception();
    host->config().afterScript();
  }
};

void Transfer::OnEnd() { owner->Finish(id, true); }
void Transfer::OnError(std::string_view) { owner->Finish(id, false); }

struct Guard {
  explicit Guard(std::shared_ptr<State> s) : state(std::move(s)) {}
  ~Guard() {
    *state->alive = false;
    for (auto& [id, item] : state->items) item.handle.Cancel();
  }
  std::shared_ptr<State> state;
};

State* StateOf(Context& ctx) {
  web::FetchHost* host = web::HostOf(ctx);
  if (!host) return nullptr;
  if (auto found = host->Extension(&g_key)) return static_cast<Guard*>(found.get())->state.get();
  auto guard = std::make_shared<Guard>(std::make_shared<State>(ctx, *host));
  host->SetExtension(&g_key, guard);
  return guard->state.get();
}

Value Fetch(Context& ctx, Value, qe::Args args, Value) {
  State* state = StateOf(ctx);
  if (!state || args.size() < 2 || !qe::IsCallable(args[1])) return qe::FromNumber(-1);
  const auto parsed = url::Parse(qe::ToWtf8(ctx, args[0]));
  if (!parsed || (parsed->scheme != "http" && parsed->scheme != "https")) return qe::FromNumber(-1);
  State::Item item;
  item.transfer = std::make_unique<Transfer>();
  item.transfer->owner = state;
  item.callback = qe::Persistent(ctx, args[1]);
  const uint32_t id = state->next++;
  item.transfer->id = id;
  net::FetchOptions options;
  options.timeout = std::chrono::minutes(2);
  options.initiator = state->host->config().pageUrl;
  options.headers.emplace_back("Accept", "image/avif,image/webp,image/png,image/svg+xml,image/*,*/*;q=0.8");
  Transfer* transfer = item.transfer.get();
  state->items[id] = std::move(item);
  state->items[id].handle = state->host->config().client->Fetch(*parsed, *transfer, std::move(options));
  return qe::FromNumber(id);
}

}  // namespace

void InstallImageLoadNatives(qe::Realm& realm) { qe::DefineGlobalFunction(realm.GetContext(), "__solarImageFetch", Fetch, 2); }

}  // namespace solar::webapi
