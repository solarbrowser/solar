/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/web-api/Worker.h"

#include <map>
#include <memory>

#include "solar/html/Frames.h"
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
};
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
  if (!owner) return;
  owner->EnqueueTask("worker", [id] {
    const auto at = Workers().find(id);
    if (at == Workers().end()) return;
    qe::Realm* realm = at->second.realm;
    Workers().erase(at);
    if (g_host && realm) g_host->ReleaseWorkerRealm(realm);
  });
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

// __solarWorkerStart(id, port, source, url, info, onError): the port is given to the worker, and its script is run (a failure is told to onError).
Value Start(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 6) return qe::Undefined();
  const uint32_t id = qe::ToUint32(ctx, args[0]);
  const auto found = Workers().find(id);
  if (found == Workers().end() || found->second.closing) return qe::Undefined();
  qe::Realm* realm = found->second.realm;
  const std::string source = qe::ToWtf8(ctx, args[2]);
  const std::string url = qe::ToWtf8(ctx, args[3]);
  const std::string info = qe::ToWtf8(ctx, args[4]);
  const Value onError = args[5];
  // The port goes over to the realm of the worker: the one end of the channel the page keeps is the other.
  qe::SerializeOptions options;
  options.transfer.push_back(args[1]);
  qe::SerializedData data;
  if (!qe::Serialize(ctx, args[1], options, data)) return qe::Undefined();
  std::string failure;
  html::RunInRealm(*realm, [&] {
    Context& wctx = realm->GetContext();
    Value port = qe::Deserialize(wctx, data);
    if (qe::HasException(wctx)) {
      failure = "the port could not be given to the worker";
      wctx.clear_exception();
      return;
    }
    Value global = qe::FromObject(wctx.get_global_object());
    qe::Set(wctx, global, "__solarWorkerPort", port);
    qe::Set(wctx, global, "__solarWorkerInfoText", qe::FromUtf8(wctx, info));
    const qe::EvaluateResult scope = realm->Evaluate(WorkerScopeScript(), "worker-scope.js");
    if (!scope.ok) {
      failure = "the global scope of the worker could not be made: " + scope.error;
      return;
    }
    const qe::EvaluateResult ran = realm->Evaluate(source, url);
    if (!ran.ok) failure = ran.error;
  });
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

}  // namespace

void SetWorkerHost(WorkerHost* host) { g_host = host; }

void InstallWorkerApis(qe::Realm& realm) {
  Context& ctx = realm.GetContext();
  qe::DefineGlobalFunction(ctx, "__solarWorkerCreate", Create, 1);
  qe::DefineGlobalFunction(ctx, "__solarWorkerStart", Start, 6);
  qe::DefineGlobalFunction(ctx, "__solarWorkerTerminate", Terminate, 1);
  qe::DefineGlobalFunction(ctx, "__solarWorkerSource", Load, 1);
}

void InstallWorkerScope(qe::Realm& realm) {
  Context& ctx = realm.GetContext();
  qe::DefineGlobalFunction(ctx, "__solarWorkerClose", Close, 0);
  qe::DefineGlobalFunction(ctx, "__solarWorkerLoad", Load, 1);
}

}  // namespace solar::webapi
