/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/web-api/Media.h"

#include <cmath>
#include <map>
#include <memory>

#include "solar/dom/Damage.h"
#include "solar/dom/NodeBindingsInternal.h"
#include "solar/media/Player.h"
#include "solar/net/HttpClient.h"
#include "solar/url/Parser.h"
#include "solar/web/BlobUrls.h"
#include "solar/web/FetchHost.h"

namespace solar::webapi {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::Value;

namespace {

char g_key;

std::string Escape(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
    else out += static_cast<char>(c);
  }
  return out + "\"";
}

struct State;

// The resource, fetched into the player's stream (as the loop gets it).
struct Transfer final : net::FetchHandler {
  std::shared_ptr<media::ByteStream> stream;
  bool failed = false;
  void OnResponseHead(const net::HttpResponseHead& head, const url::Url&) override {
    if (head.status < 200 || head.status >= 300) {
      failed = true;
      return;
    }
    if (auto length = head.Header("content-length")) stream->SetTotal(static_cast<uint64_t>(std::strtoull(std::string(*length).c_str(), nullptr, 10)));
  }
  void OnBody(std::span<const uint8_t> data) override {
    if (!failed) stream->Append(data.data(), data.size());
  }
  void OnEnd() override { stream->Finish(failed); }
  void OnError(std::string_view) override { stream->Finish(true); }
};

struct Item {
  std::shared_ptr<media::Player> player;
  std::shared_ptr<media::ByteStream> stream;
  std::unique_ptr<Transfer> transfer;
  net::FetchHandle handle;
  qe::Persistent callback;
  std::shared_ptr<std::atomic<bool>> pending = std::make_shared<std::atomic<bool>>(false);
  std::vector<dom::Element*> elements;
  int lastWidth = 0, lastHeight = 0;  // the size the elements were laid out with
};

// What the realm keeps: its players. When the realm goes they are closed, and nothing more is told to script.
struct State : std::enable_shared_from_this<State> {
  State(Context& c, web::FetchHost& h) : ctx(&c), host(&h) {}
  ~State() { CloseAll(); }
  Context* ctx;
  web::FetchHost* host;
  std::map<uint32_t, std::unique_ptr<Item>> items;
  uint32_t next = 1;
  std::shared_ptr<bool> alive = std::make_shared<bool>(true);

  void CloseAll() {
    *alive = false;
    for (auto& [id, item] : items) Release(*item);
    items.clear();
  }
  void Release(Item& item) {
    item.handle.Cancel();
    for (dom::Element* element : item.elements) media::DetachPlayer(element);
    item.elements.clear();
    if (item.stream) item.stream->Finish(true);
    item.player.reset();
  }
};

struct Guard {
  explicit Guard(std::shared_ptr<State> s) : state(std::move(s)) {}
  ~Guard() { state->CloseAll(); }
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

Item* ItemOf(State& state, Context& ctx, const Value& id) {
  const auto found = state.items.find(qe::ToUint32(ctx, id));
  return found == state.items.end() ? nullptr : found->second.get();
}

// __solarMediaCanPlay(type, codecs): 0, 1 or 2.
Value CanPlay(Context& ctx, Value, qe::Args args, Value) {
  if (args.empty()) return qe::FromNumber(0);
  return qe::FromNumber(media::CanPlayType(qe::ToWtf8(ctx, args[0]), args.size() > 1 ? qe::ToWtf8(ctx, args[1]) : ""));
}

// __solarMediaOpen(url, callback): the number of a player that plays the resource, or -1. The callback is called (with no arguments) when its state or events have
// something new.
Value Open(Context& ctx, Value, qe::Args args, Value) {
  State* state = StateOf(ctx);
  if (!state || args.size() < 2 || !qe::IsCallable(args[1])) return qe::FromNumber(-1);
  const std::string address = qe::ToWtf8(ctx, args[0]);
  auto item = std::make_unique<Item>();
  item->stream = std::make_shared<media::ByteStream>();
  item->callback = qe::Persistent(ctx, args[1]);
  const uint32_t id = state->next++;
  web::FetchHost* host = state->host;
  std::shared_ptr<bool> alive = state->alive;
  State* raw = state;
  std::shared_ptr<std::atomic<bool>> pending = item->pending;
  item->player = media::Player::Create(item->stream, [host, id, alive, raw, pending] {
    // (From the player's thread: the script is told on the loop's, once for all the changes there were meanwhile.)
    if (pending->exchange(true)) return;
    host->config().loop->Post([host, id, alive, raw, pending] {
      pending->store(false);
      if (!*alive) return;
      const auto found = raw->items.find(id);
      if (found == raw->items.end()) return;
      Context& c = *raw->ctx;
      // A video that has a size now (or another) is laid out again.
      if (found->second->player && !found->second->elements.empty()) {
        const media::PlayerState state = found->second->player->State();
        if (state.videoWidth != found->second->lastWidth || state.videoHeight != found->second->lastHeight) {
          found->second->lastWidth = state.videoWidth;
          found->second->lastHeight = state.videoHeight;
          for (dom::Element* element : found->second->elements) { dom::NoteDamage(element, dom::kDamageAttributes); dom::NoteTreeChangeAt(element); }
        }
      }
      qe::Call(c, found->second->callback.Get(), qe::Undefined());
      if (qe::HasException(c)) c.clear_exception();
      host->config().afterScript();
    });
  });
  if (address.rfind("blob:", 0) == 0) {
    if (auto blob = web::LookupBlobUrl(address)) {
      item->stream->SetTotal(blob->data->size());
      item->stream->Append(reinterpret_cast<const uint8_t*>(blob->data->data()), blob->data->size());
      item->stream->Finish(false);
    } else {
      item->stream->Finish(true);
    }
  } else if (auto parsed = url::Parse(address); parsed && (parsed->scheme == "http" || parsed->scheme == "https")) {
    item->transfer = std::make_unique<Transfer>();
    item->transfer->stream = item->stream;
    net::FetchOptions options;
    options.timeout = std::chrono::hours(12);
    options.initiator = host->config().pageUrl;
    options.headers.emplace_back("Accept", "*/*");
    item->handle = host->config().client->Fetch(*parsed, *item->transfer, std::move(options));
  } else {
    item->stream->Finish(true);
  }
  state->items[id] = std::move(item);
  return qe::FromNumber(id);
}

// __solarMediaControl(id, what, value)
Value Control(Context& ctx, Value, qe::Args args, Value) {
  State* state = StateOf(ctx);
  if (!state || args.size() < 2) return qe::Undefined();
  Item* item = ItemOf(*state, ctx, args[0]);
  if (!item || !item->player) return qe::Undefined();
  const std::string what = qe::ToWtf8(ctx, args[1]);
  const Value value = args.size() > 2 ? args[2] : qe::Undefined();
  if (what == "play") item->player->Play();
  else if (what == "pause") item->player->Pause();
  else if (what == "seek") item->player->Seek(qe::ToNumber(ctx, value));
  else if (what == "volume") item->player->SetVolume(qe::ToNumber(ctx, value));
  else if (what == "muted") item->player->SetMuted(qe::ToBoolean(value));
  else if (what == "rate") item->player->SetRate(qe::ToNumber(ctx, value));
  else if (what == "loop") item->player->SetLoop(qe::ToBoolean(value));
  else if (what == "close") {
    const uint32_t id = qe::ToUint32(ctx, args[0]);
    state->Release(*item);
    // (The player's thread may be calling back: the item goes in a later task.)
    state->host->config().loop->Post([alive = state->alive, state, id] {
      if (*alive) state->items.erase(id);
    });
  }
  return qe::Undefined();
}

// __solarMediaState(id): the state as JSON text.
Value StateJson(Context& ctx, Value, qe::Args args, Value) {
  State* state = StateOf(ctx);
  if (!state || args.empty()) return qe::Undefined();
  Item* item = ItemOf(*state, ctx, args[0]);
  if (!item || !item->player) return qe::Undefined();
  const media::PlayerState s = item->player->State();
  auto number = [](double v) { return std::isnan(v) ? std::string("null") : (std::isinf(v) ? std::string("1e999") : std::to_string(v)); };
  std::string json = "{\"readyState\":" + std::to_string(s.readyState) + ",\"duration\":" + number(s.duration) + ",\"videoWidth\":" + std::to_string(s.videoWidth) +
                     ",\"videoHeight\":" + std::to_string(s.videoHeight) + ",\"hasVideo\":" + (s.hasVideo ? "true" : "false") + ",\"hasAudio\":" + (s.hasAudio ? "true" : "false") +
                     ",\"currentTime\":" + number(s.currentTime) + ",\"seeking\":" + (s.seeking ? "true" : "false") + ",\"ended\":" + (s.ended ? "true" : "false") +
                     ",\"stalled\":" + (s.stalled ? "true" : "false") + ",\"error\":" + std::to_string(s.error) + ",\"errorMessage\":" + Escape(s.errorMessage) + ",\"buffered\":[";
  for (size_t i = 0; i < s.buffered.size(); ++i) json += (i ? "," : "") + std::string("[") + number(s.buffered[i].start) + "," + number(s.buffered[i].end) + "]";
  return qe::FromWtf8(ctx, json + "]}");
}

// __solarMediaEvents(id): the events since last asked, as a JSON array of names.
Value EventsJson(Context& ctx, Value, qe::Args args, Value) {
  State* state = StateOf(ctx);
  if (!state || args.empty()) return qe::FromWtf8(ctx, "[]");
  Item* item = ItemOf(*state, ctx, args[0]);
  if (!item || !item->player) return qe::FromWtf8(ctx, "[]");
  std::string json = "[";
  bool first = true;
  for (const std::string& name : item->player->TakeEvents()) {
    json += (first ? "" : ",") + Escape(name);
    first = false;
  }
  return qe::FromWtf8(ctx, json + "]");
}

// __solarMediaAttach(id, element): the element is drawn with the player's pictures (and laid out with their size). With no player (id < 0) it is let go.
Value Attach(Context& ctx, Value, qe::Args args, Value) {
  State* state = StateOf(ctx);
  if (!state || args.size() < 2) return qe::Undefined();
  dom::Element* element = dom::ThisElement(ctx, args[1]);
  if (!element) {
    ctx.clear_exception();
    return qe::Undefined();
  }
  const double id = qe::ToNumber(ctx, args[0]);
  if (id < 0) {
    media::DetachPlayer(element);
    return qe::Undefined();
  }
  Item* item = ItemOf(*state, ctx, args[0]);
  if (!item || !item->player) return qe::Undefined();
  media::AttachPlayer(element, item->player);
  item->elements.push_back(element);
  { dom::NoteDamage(element, dom::kDamageAttributes); dom::NoteTreeChangeAt(element); }
  return qe::Undefined();
}

}  // namespace

void InstallMediaNatives(qe::Realm& realm) {
  Context& ctx = realm.GetContext();
  qe::DefineGlobalFunction(ctx, "__solarMediaCanPlay", CanPlay, 2);
  qe::DefineGlobalFunction(ctx, "__solarMediaOpen", Open, 2);
  qe::DefineGlobalFunction(ctx, "__solarMediaControl", Control, 3);
  qe::DefineGlobalFunction(ctx, "__solarMediaState", StateJson, 1);
  qe::DefineGlobalFunction(ctx, "__solarMediaEvents", EventsJson, 1);
  qe::DefineGlobalFunction(ctx, "__solarMediaAttach", Attach, 2);
}

}  // namespace solar::webapi
