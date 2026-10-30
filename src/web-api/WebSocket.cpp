/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/web-api/WebSocket.h"

#include <map>
#include <memory>

#include "solar/net/WebSocket.h"
#include "solar/url/Origin.h"
#include "solar/url/Parser.h"
#include "solar/web/FetchHost.h"

namespace solar::webapi {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::Value;

namespace {

char g_key;

// What the realm keeps: the client, and a socket for each connection that has not ended. When the realm goes, what is open is dropped and nothing more is told
// to script; the state lives on until the last connection has ended, because the network's calls for it are still to come.
struct State : std::enable_shared_from_this<State> {
  State(Context& c, web::FetchHost& h) : ctx(&c), host(&h), loop_(h.config().loop), client(*h.config().client) {}

  struct Socket final : net::WebSocketHandler {
    Socket(State& s, uint32_t i, qe::Persistent cb) : state(s), id(i), callback(std::move(cb)) {}
    State& state;
    uint32_t id;
    qe::Persistent callback;
    net::WebSocketHandle handle;

    void Tell(const char* what, std::vector<Value> arguments) {
      if (state.dead) return;
      arguments.insert(arguments.begin(), qe::FromWtf8(*state.ctx, what));
      qe::Call(*state.ctx, callback.Get(), qe::Undefined(), qe::Args(arguments));
      state.host->config().afterScript();
    }
    void OnOpen(const std::string& protocol, const std::string& extensions) override { Tell("open", {qe::FromWtf8(*state.ctx, protocol), qe::FromWtf8(*state.ctx, extensions)}); }
    void OnMessage(std::string data, bool binary) override {
      if (state.dead) return;
      Context& ctx = *state.ctx;
      if (!binary) {
        Tell("message", {qe::FromWtf8(ctx, data), qe::FromBool(false)});
        return;
      }
      Value buffer = qe::NewArrayBuffer(ctx, data.size());
      if (auto bytes = qe::MutableBytesOf(buffer)) std::copy(data.begin(), data.end(), reinterpret_cast<char*>(bytes->data()));
      Tell("message", {buffer, qe::FromBool(true)});
    }
    void OnClosed(bool clean, uint16_t code, const std::string& reason, const std::string& error) override {
      State& s = state;
      const uint32_t number = id;
      Tell("close", {qe::FromBool(clean), qe::FromNumber(code), qe::FromWtf8(*s.ctx, reason), qe::FromWtf8(*s.ctx, error)});
      // (This object goes after the network is done with it, not from inside its call.)
      std::shared_ptr<State> keep = s.shared_from_this();
      s.loop_->Post([keep, number] {
        keep->sockets.erase(number);
        if (keep->dead && keep->sockets.empty()) {
          // The network's tasks of the last connection may still be queued behind this one.
          State* raw = keep.get();
          raw->loop_->PostDelayed(std::chrono::milliseconds(100), [keep] { keep->self.reset(); });
        }
      });
    }
  };

  Context* ctx;
  web::FetchHost* host;
  net::Loop* loop_;
  net::WebSocketClient client;
  std::map<uint32_t, std::unique_ptr<Socket>> sockets;
  uint32_t next = 1;
  bool dead = false;
  std::shared_ptr<State> self;  // while the realm is gone and connections are not
};

// Held by the host: when it goes, the state is retired.
struct Guard {
  explicit Guard(std::shared_ptr<State> s) : state(std::move(s)) {}
  ~Guard() {
    state->dead = true;
    if (!state->sockets.empty()) {
      state->self = state;
      state->client.CloseAll();
    }
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

// __solarSocketOpen(url, protocols (JSON array text), origin, callback): the number of the connection, or -1.
Value Open(Context& ctx, Value, qe::Args args, Value) {
  State* state = StateOf(ctx);
  if (!state || args.size() < 4 || !qe::IsCallable(args[3])) return qe::FromNumber(-1);
  auto parsed = url::Parse(qe::ToWtf8(ctx, args[0]));
  if (!parsed) return qe::FromNumber(-1);
  net::WebSocketOptions options;
  const std::string protocols = qe::ToWtf8(ctx, args[1]);
  // (A list of tokens separated by commas: the script has checked them.)
  for (size_t at = 0; at < protocols.size();) {
    size_t end = protocols.find(',', at);
    if (end == std::string::npos) end = protocols.size();
    if (end > at) options.protocols.push_back(protocols.substr(at, end - at));
    at = end + 1;
  }
  options.origin = qe::ToWtf8(ctx, args[2]);
  options.initiator = state->host->config().pageUrl;
  const uint32_t id = state->next++;
  auto socket = std::make_unique<State::Socket>(*state, id, qe::Persistent(ctx, args[3]));
  State::Socket* raw = socket.get();
  state->sockets[id] = std::move(socket);
  raw->handle = state->client.Connect(*parsed, *raw, std::move(options));
  return qe::FromNumber(id);
}

// __solarSocketSend(id, data): a string is a text message, anything with bytes a binary one.
Value Send(Context& ctx, Value, qe::Args args, Value) {
  State* state = StateOf(ctx);
  if (!state || args.size() < 2) return qe::Undefined();
  const auto found = state->sockets.find(static_cast<uint32_t>(qe::ToNumber(ctx, args[0])));
  if (found == state->sockets.end()) return qe::Undefined();
  if (qe::IsString(args[1])) {
    found->second->handle.Send(qe::ToWtf8(ctx, args[1]), false);
  } else if (auto bytes = qe::BytesOf(args[1])) {
    found->second->handle.Send(std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size()), true);
  }
  return qe::Undefined();
}

// __solarSocketClose(id, code, reason): code 1005 for none.
Value Close(Context& ctx, Value, qe::Args args, Value) {
  State* state = StateOf(ctx);
  if (!state || args.size() < 3) return qe::Undefined();
  const auto found = state->sockets.find(static_cast<uint32_t>(qe::ToNumber(ctx, args[0])));
  if (found == state->sockets.end()) return qe::Undefined();
  found->second->handle.Close(static_cast<uint16_t>(qe::ToNumber(ctx, args[1])), qe::ToWtf8(ctx, args[2]));
  return qe::Undefined();
}

}  // namespace

void InstallWebSocketNatives(qe::Realm& realm) {
  Context& ctx = realm.GetContext();
  qe::DefineGlobalFunction(ctx, "__solarSocketOpen", Open, 4);
  qe::DefineGlobalFunction(ctx, "__solarSocketSend", Send, 2);
  qe::DefineGlobalFunction(ctx, "__solarSocketClose", Close, 3);
}

}  // namespace solar::webapi
