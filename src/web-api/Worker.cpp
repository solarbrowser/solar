/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/web-api/Worker.h"

#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "solar/html/Frames.h"
#include "solar/web-api/CacheStore.h"
#include "solar/web-api/WebApis.h"
#include "solar/web/BlobUrls.h"

namespace solar::webapi {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::Value;

namespace {

WorkerHost* g_host = nullptr;

// The workers there are: the realm of each, and the realm of the page that made it (what is told of an error goes there).
struct Entry {
  qe::Realm* realm = nullptr;
  qe::Realm* owner = nullptr;
  bool closing = false;
  std::shared_ptr<qe::Persistent> onError;  // the page's: told of what the worker threw (its message, where, and whether the page took it)
  // A service worker is kept when its page is left, to be found by the next page (and by a navigation) while it lives.
  bool service = false;
  std::string origin, scope;
  std::vector<std::pair<qe::Realm*, uint32_t>> attached;  // the pages that gave it a port, and the number the worker knows each port by
};
uint32_t g_attachment = 1;
std::map<uint32_t, Entry>& Workers() {
  static std::map<uint32_t, Entry> workers;
  return workers;
}
uint32_t g_next = 1;
char g_workerKey;  // (in a worker's realm: the number of the worker)

// A worker is let go in a task of its page, not in the one it asked in (it may be in the middle of its own).
void Release(uint32_t id) {
  const auto found = Workers().find(id);
  if (found == Workers().end() || found->second.closing) return;
  found->second.closing = true;
  qe::Realm* owner = found->second.owner;
  auto release = [id] {
    const auto at = Workers().find(id);
    if (at == Workers().end()) return;
    qe::Realm* realm = at->second.realm;
    Workers().erase(at);
    if (g_host && realm) g_host->ReleaseWorkerRealm(realm);
  };
  if (owner) owner->EnqueueTask("worker", release);
  else if (g_host) g_host->RunLater(release);
}

// The start of a worker, or of a service worker that no page asked for: the global scope is made, the port (if there is one) is given, the script is run. A failure is
// told as text; empty when it went well.
std::string Boot(qe::Realm* realm, qe::SerializedData* port, const std::string& info, const std::string& source, const std::string& url, bool isModule) {
  std::string failure;
  html::RunInRealm(*realm, [&] {
    Context& wctx = realm->GetContext();
    Value global = qe::FromObject(wctx.get_global_object());
    if (port) {
      Value value = qe::Deserialize(wctx, *port);
      if (qe::HasException(wctx)) {
        failure = "the port could not be given to the worker";
        wctx.clear_exception();
        return;
      }
      qe::Set(wctx, global, "__solarWorkerPort", value);
    }
    qe::Set(wctx, global, "__solarWorkerInfoText", qe::FromUtf8(wctx, info));
    {
      // (How long a service worker with nothing to do is kept; SOLAR_SW_IDLE_MS changes it, for tests.)
      const char* idle = std::getenv("SOLAR_SW_IDLE_MS");
      qe::Set(wctx, global, "__solarSwIdleMs", qe::FromNumber(idle ? std::atof(idle) : 30000.0));
    }
    const qe::EvaluateResult scope = realm->Evaluate(WorkerScopeScript(), "worker-scope.js");
    if (!scope.ok) {
      failure = "the global scope of the worker could not be made: " + scope.error;
      return;
    }
    if (isModule) {
      // A module is evaluated asynchronously: what is wrong with it (an import that is not there, a throw) is a rejection, told as any error of the worker.
      Value promise = realm->EvaluateModule(source, url);
      if (qe::HasException(wctx)) {
        failure = qe::InspectError(wctx, wctx.get_exception()).message;
        wctx.clear_exception();
      } else if (qe::IsObject(promise)) {
        Value rejected = qe::NewFunction(wctx, "onrejected", 1, [](Context& c, Value, qe::Args reason, Value) {
          const Value what = reason.empty() ? qe::Undefined() : reason[0];
          ReportWorkerError(c, what, qe::InspectError(c, what));
          return qe::Undefined();
        });
        Value then = qe::Get(wctx, promise, "then");
        Value arguments[] = {qe::Undefined(), rejected};
        qe::Call(wctx, then, promise, qe::Args(arguments, 2));
        if (qe::HasException(wctx)) wctx.clear_exception();
      }
    } else {
      const qe::EvaluateResult ran = realm->Evaluate(source, url);
      if (!ran.ok) failure = ran.error;
    }
  });
  return failure;
}

// __solarWorkerCreate(url): the number of a new worker, or -1 where there is no program to make it.
Value Create(Context& ctx, Value, qe::Args args, Value) {
  if (!g_host || args.empty()) return qe::FromNumber(-1);
  const std::string url = qe::ToWtf8(ctx, args[0]);
  qe::Realm* realm = g_host->CreateWorkerRealm(url);
  if (!realm) return qe::FromNumber(-1);
  const uint32_t id = g_next++;
  Entry entry;
  entry.realm = realm;
  entry.owner = qe::Realm::FromContext(ctx);
  Workers()[id] = entry;
  // (The worker knows which it is.)
  qe::SetRealmData(realm->GetContext(), &g_workerKey, reinterpret_cast<void*>(static_cast<uintptr_t>(id)));
  return qe::FromNumber(id);
}

// __solarWorkerStart(id, port, source, url, info, onError, isModule): the port is given to the worker, and its script is run (a failure is told to onError).
Value Start(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 7) return qe::Undefined();
  const uint32_t id = qe::ToUint32(ctx, args[0]);
  const auto found = Workers().find(id);
  if (found == Workers().end() || found->second.closing) return qe::Undefined();
  qe::Realm* realm = found->second.realm;
  const std::string source = qe::ToWtf8(ctx, args[2]);
  const std::string url = qe::ToWtf8(ctx, args[3]);
  const std::string info = qe::ToWtf8(ctx, args[4]);
  const Value onError = args[5];
  const bool isModule = qe::ToBoolean(args[6]);
  if (qe::IsCallable(onError)) found->second.onError = std::make_shared<qe::Persistent>(ctx, onError);
  // The port goes over to the realm of the worker: the one end of the channel the page keeps is the other.
  qe::SerializeOptions options;
  options.transfer.push_back(args[1]);
  qe::SerializedData data;
  if (!qe::Serialize(ctx, args[1], options, data)) return qe::Undefined();
  const std::string failure = Boot(realm, &data, info, source, url, isModule);
  if (!failure.empty() && qe::IsCallable(onError)) {
    Value message = qe::FromUtf8(ctx, failure);
    qe::Call(ctx, onError, qe::Undefined(), qe::Args(&message, 1));
    if (qe::HasException(ctx)) ctx.clear_exception();
  }
  return qe::Undefined();
}

Value Terminate(Context& ctx, Value, qe::Args args, Value) {
  if (!args.empty()) Release(qe::ToUint32(ctx, args[0]));
  return qe::Undefined();
}

// In a worker's realm. __solarWorkerClose(): the worker stops.
Value Close(Context& ctx, Value, qe::Args, Value) {
  void* id = qe::GetRealmData(ctx, &g_workerKey);
  if (id) Release(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(id)));
  return qe::Undefined();
}

// __solarWorkerLoad(url): the text of a script, for importScripts (which waits for it).
Value Load(Context& ctx, Value, qe::Args args, Value) {
  if (args.empty()) return qe::Undefined();
  const std::string url = qe::ToWtf8(ctx, args[0]);
  if (url.rfind("blob:", 0) == 0) {
    if (auto blob = web::LookupBlobUrl(url)) return qe::FromWtf8(ctx, *blob->data);
    return qe::Undefined();
  }
  if (g_host) {
    if (auto text = g_host->LoadScript(url)) return qe::FromWtf8(ctx, *text);
  }
  return qe::Undefined();
}


// ---- Service workers that outlive their page ----

std::string JsonString(const std::string& text) {
  std::string out = "\"";
  for (unsigned char c : text) {
    if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
    else out += static_cast<char>(c);
  }
  return out + "\"";
}

// The string members of the objects of a JSON array (the registrations the page's script kept): a small reader for that shape, which nothing else is read with.
std::vector<std::map<std::string, std::string>> StringMembers(const std::string& text) {
  std::vector<std::map<std::string, std::string>> objects;
  size_t at = 0;
  auto skip = [&] { while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at]))) ++at; };
  auto string = [&](std::string& out) {
    if (at >= text.size() || text[at] != '"') return false;
    ++at;
    while (at < text.size() && text[at] != '"') {
      char c = text[at++];
      if (c != '\\') { out += c; continue; }
      if (at >= text.size()) return false;
      const char e = text[at++];
      switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
          if (at + 4 > text.size()) return false;
          uint32_t code = static_cast<uint32_t>(std::stoul(text.substr(at, 4), nullptr, 16));
          at += 4;
          if (code >= 0xD800 && code < 0xDC00 && at + 6 <= text.size() && text[at] == '\\' && text[at + 1] == 'u') {
            const uint32_t low = static_cast<uint32_t>(std::stoul(text.substr(at + 2, 4), nullptr, 16));
            if (low >= 0xDC00 && low < 0xE000) { code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00); at += 6; }
          }
          if (code < 0x80) out += static_cast<char>(code);
          else if (code < 0x800) { out += static_cast<char>(0xC0 | (code >> 6)); out += static_cast<char>(0x80 | (code & 0x3F)); }
          else if (code < 0x10000) { out += static_cast<char>(0xE0 | (code >> 12)); out += static_cast<char>(0x80 | ((code >> 6) & 0x3F)); out += static_cast<char>(0x80 | (code & 0x3F)); }
          else { out += static_cast<char>(0xF0 | (code >> 18)); out += static_cast<char>(0x80 | ((code >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((code >> 6) & 0x3F)); out += static_cast<char>(0x80 | (code & 0x3F)); }
          break;
        }
        default: out += e;
      }
    }
    if (at >= text.size()) return false;
    ++at;
    return true;
  };
  // Skips a value that is not a string: numbers, literals, and objects and arrays by their brackets.
  auto skipValue = [&] {
    int depth = 0;
    while (at < text.size()) {
      const char c = text[at];
      if (c == '"') { std::string ignore; if (!string(ignore)) return; continue; }
      if (c == '{' || c == '[') ++depth;
      else if (c == '}' || c == ']') { if (depth == 0) return; --depth; }
      else if (c == ',' && depth == 0) return;
      ++at;
    }
  };
  skip();
  if (at >= text.size() || text[at] != '[') return objects;
  ++at;
  for (;;) {
    skip();
    if (at >= text.size() || text[at] == ']') break;
    if (text[at] == ',') { ++at; continue; }
    if (text[at] != '{') break;
    ++at;
    std::map<std::string, std::string> members;
    for (;;) {
      skip();
      if (at >= text.size()) return objects;
      if (text[at] == '}') { ++at; break; }
      if (text[at] == ',') { ++at; continue; }
      std::string key;
      if (!string(key)) return objects;
      skip();
      if (at >= text.size() || text[at] != ':') return objects;
      ++at;
      skip();
      std::string value;
      if (at < text.size() && text[at] == '"') {
        if (!string(value)) return objects;
        members[key] = value;
      } else {
        skipValue();
      }
    }
    objects.push_back(std::move(members));
  }
  return objects;
}

// The navigator a service worker started without a page has: what the pages' navigator says (src/html/JsHtml.cpp).
const char* const kNavigatorInfo =
    "\"userAgent\":\"Mozilla/5.0 (X11; Linux x86_64) Solar/1.0\",\"language\":\"en-US\",\"languages\":[\"en-US\"],\"platform\":\"Linux x86_64\","
    "\"hardwareConcurrency\":4,\"onLine\":true,\"appVersion\":\"5.0 (X11)\"";

// __solarSwMark(id, origin, scope): the worker is a service worker of the scope, to be kept when its page is left.
Value Mark(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 3) return qe::Undefined();
  const auto found = Workers().find(qe::ToUint32(ctx, args[0]));
  if (found == Workers().end()) return qe::Undefined();
  found->second.service = true;
  found->second.origin = qe::ToWtf8(ctx, args[1]);
  found->second.scope = qe::ToWtf8(ctx, args[2]);
  return qe::Undefined();
}

// __solarSwAdopt(origin, url): the number of the service worker that is running for the address (the one with the longest scope), or -1. The page becomes the one
// that its errors go to.
Value Adopt(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 2) return qe::FromNumber(-1);
  uint32_t id = 0;
  if (!FindServiceWorker(qe::ToWtf8(ctx, args[0]), qe::ToWtf8(ctx, args[1]), id)) return qe::FromNumber(-1);
  Workers()[id].owner = qe::Realm::FromContext(ctx);
  return qe::FromNumber(id);
}

// __solarWorkerAttach(id, port): a port to the worker is given to it, for the page that has the other end (a service worker has a port for each page it serves).
Value Attach(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 2) return qe::FromBool(false);
  const auto found = Workers().find(qe::ToUint32(ctx, args[0]));
  if (found == Workers().end() || found->second.closing) return qe::FromBool(false);
  qe::Realm* realm = found->second.realm;
  qe::SerializeOptions options;
  options.transfer.push_back(args[1]);
  qe::SerializedData data;
  if (!qe::Serialize(ctx, args[1], options, data)) return qe::FromBool(false);
  const uint32_t token = g_attachment++;
  bool good = false;
  html::RunInRealm(*realm, [&] {
    Context& wctx = realm->GetContext();
    Value port = qe::Deserialize(wctx, data);
    if (qe::HasException(wctx)) {
      wctx.clear_exception();
      return;
    }
    Value function = qe::Get(wctx, qe::FromObject(wctx.get_global_object()), "__solarSwAttach");
    if (!qe::IsCallable(function)) return;
    Value arguments[] = {port, qe::FromNumber(token)};
    qe::Call(wctx, function, qe::Undefined(), qe::Args(arguments, 2));
    if (qe::HasException(wctx)) wctx.clear_exception();
    else good = true;
  });
  if (good) found->second.attached.emplace_back(qe::Realm::FromContext(ctx), token);
  return qe::FromBool(good);
}

}  // namespace

void SetWorkerHost(WorkerHost* host) { g_host = host; }

bool ReportWorkerError(Context& ctx, const Value& exception, const qe::ErrorInfo& info) {
  (void)exception;
  void* number = qe::GetRealmData(ctx, &g_workerKey);
  if (!number) return false;
  const uint32_t id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(number));
  const auto found = Workers().find(id);
  if (found == Workers().end() || found->second.closing) return true;  // (it is gone: what it threw is not told)
  std::string message = "Uncaught ";
  message += info.is_error ? (info.name.empty() ? "" : info.name + ": ") + info.message : info.message;
  bool handled = false;
  qe::Realm* owner = found->second.owner;
  const std::shared_ptr<qe::Persistent> onError = found->second.onError;
  if (owner && onError) {
    html::RunInRealm(*owner, [&] {
      Context& pageCtx = owner->GetContext();
      Value arguments[] = {qe::FromWtf8(pageCtx, message), qe::FromWtf8(pageCtx, info.filename), qe::FromUint32(info.line), qe::FromUint32(info.column)};
      Value result = qe::Call(pageCtx, onError->Get(), qe::Undefined(), qe::Args(arguments, 4));
      if (qe::HasException(pageCtx)) pageCtx.clear_exception();
      else handled = result.to_boolean();
    });
  }
  if (!handled) {
    std::fprintf(stderr, "%s", message.c_str());
    if (!info.filename.empty()) std::fprintf(stderr, " (%s:%u:%u)", info.filename.c_str(), info.line, info.column);
    std::fprintf(stderr, "\n");
  }
  return true;
}

bool FindServiceWorker(const std::string& origin, const std::string& url, uint32_t& id) {
  size_t best = 0;
  bool found = false;
  for (const auto& [number, entry] : Workers()) {
    if (!entry.service || entry.closing || entry.origin != origin || entry.scope.empty()) continue;
    if (url.compare(0, entry.scope.size(), entry.scope) != 0) continue;
    if (!found || entry.scope.size() > best) {
      best = entry.scope.size();
      id = number;
      found = true;
    }
  }
  return found;
}

qe::Realm* WorkerRealmOf(uint32_t id) {
  const auto found = Workers().find(id);
  return found == Workers().end() || found->second.closing ? nullptr : found->second.realm;
}

uint32_t StartStoredServiceWorker(const std::string& origin, const std::string& url) {
  if (!g_host) return 0;
  const std::optional<std::string> kept = ReadKv(origin, "sw-registrations");
  if (!kept) return 0;
  std::map<std::string, std::string> chosen;
  for (auto& members : StringMembers(*kept)) {
    const std::string& scope = members["scope"];
    if (scope.empty() || members["scriptURL"].empty() || url.compare(0, scope.size(), scope) != 0) continue;
    if (chosen.empty() || scope.size() > chosen["scope"].size()) chosen = members;
  }
  if (chosen.empty()) return 0;
  const std::string scriptUrl = chosen["scriptURL"];
  qe::Realm* realm = g_host->CreateWorkerRealm(scriptUrl);
  if (!realm) return 0;
  const uint32_t id = g_next++;
  Entry entry;
  entry.realm = realm;
  entry.service = true;
  entry.origin = origin;
  entry.scope = chosen["scope"];
  Workers()[id] = entry;
  qe::SetRealmData(realm->GetContext(), &g_workerKey, reinterpret_cast<void*>(static_cast<uintptr_t>(id)));
  const std::string info = "{\"url\":" + JsonString(scriptUrl) + ",\"name\":\"\",\"type\":\"classic\",\"serviceWorker\":true,\"scope\":" + JsonString(chosen["scope"]) +
                           ",\"state\":\"activated\",\"updateViaCache\":" + JsonString(chosen["updateViaCache"].empty() ? "imports" : chosen["updateViaCache"]) + ",\"origin\":" + JsonString(origin) +
                           "," + kNavigatorInfo + "}";
  const std::string failure = Boot(realm, nullptr, info, chosen["source"], scriptUrl, false);
  if (!failure.empty()) {
    std::fprintf(stderr, "service worker %s: %s\n", scriptUrl.c_str(), failure.c_str());
    Release(id);
    return 0;
  }
  return id;
}

void ReleaseOwnedWorkers(qe::Realm* owner) {
  std::vector<uint32_t> ids;
  for (auto& [id, entry] : Workers()) {
    // A service worker forgets the ports of a page that is gone (its clients are the pages that are there).
    for (auto at = entry.attached.begin(); at != entry.attached.end();) {
      if (at->first != owner) { ++at; continue; }
      const uint32_t token = at->second;
      at = entry.attached.erase(at);
      if (entry.closing || !entry.realm) continue;
      html::RunInRealm(*entry.realm, [&] {
        Context& wctx = entry.realm->GetContext();
        Value function = qe::Get(wctx, qe::FromObject(wctx.get_global_object()), "__solarSwDetach");
        if (qe::IsCallable(function)) {
          Value argument = qe::FromNumber(token);
          qe::Call(wctx, function, qe::Undefined(), qe::Args(&argument, 1));
        }
        if (qe::HasException(wctx)) wctx.clear_exception();
      });
    }
    if (entry.owner != owner || entry.closing) continue;
    if (entry.service) {
      entry.owner = nullptr;
      entry.onError.reset();
    } else {
      ids.push_back(id);
    }
  }
  for (uint32_t id : ids) {
    const auto found = Workers().find(id);
    if (found == Workers().end()) continue;
    found->second.owner = nullptr;
    Release(id);
  }
}

void InstallWorkerApis(qe::Realm& realm) {
  Context& ctx = realm.GetContext();
  qe::DefineGlobalFunction(ctx, "__solarWorkerCreate", Create, 1);
  qe::DefineGlobalFunction(ctx, "__solarWorkerStart", Start, 7);
  qe::DefineGlobalFunction(ctx, "__solarWorkerTerminate", Terminate, 1);
  qe::DefineGlobalFunction(ctx, "__solarWorkerSource", Load, 1);
  qe::DefineGlobalFunction(ctx, "__solarSwMark", Mark, 3);
  qe::DefineGlobalFunction(ctx, "__solarSwAdopt", Adopt, 2);
  qe::DefineGlobalFunction(ctx, "__solarWorkerAttach", Attach, 2);
}

void InstallWorkerScope(qe::Realm& realm) {
  Context& ctx = realm.GetContext();
  qe::DefineGlobalFunction(ctx, "__solarWorkerClose", Close, 0);
  qe::DefineGlobalFunction(ctx, "__solarWorkerLoad", Load, 1);
}

}  // namespace solar::webapi
