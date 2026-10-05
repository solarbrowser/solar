#include "solar/html/Errors.h"

#include <cstdio>
#include <vector>

#include "solar/dom/NodeBindingsInternal.h"
#include "solar/html/Frames.h"
#include "solar/web/ErrorReporting.h"

namespace solar::html {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::Object;
using Quanta::Value;

namespace {

void Print(const char* prefix, const qe::ErrorInfo& info) {
  std::fprintf(stderr, "%s%s%s%s", prefix, info.name.c_str(), info.name.empty() ? "" : ": ", info.message.c_str());
  if (!info.filename.empty()) std::fprintf(stderr, " (%s:%u:%u)", info.filename.c_str(), info.line, info.column);
  std::fprintf(stderr, "\n");
}

Value HolderFunction(Context& ctx, const char* name) {
  Object* holder = dom::RealmHolder(ctx);
  return holder ? qe::Get(ctx, qe::FromObject(holder), name) : qe::Undefined();
}

// __solarRegisterReporters(reportError, reportRejection): the window script's functions that make and dispatch the events.
Value RegisterReporters(Context& ctx, Value, qe::Args args, Value) {
  Object* holder = dom::RealmHolder(ctx);
  if (!holder || args.size() < 2) return qe::Undefined();
  qe::Set(ctx, qe::FromObject(holder), "reportError", args[0]);
  qe::Set(ctx, qe::FromObject(holder), "reportRejection", args[1]);
  return qe::Undefined();
}

}  // namespace

bool ReportError(Context& ctx, const Value& exception, const qe::ErrorInfo& given) {
  // Where it was thrown is the top frame of its stack.
  qe::ErrorInfo info = given;
  if (!info.frames.empty() && !info.frames.front().filename.empty()) {
    info.filename = info.frames.front().filename;
    info.line = info.frames.front().line;
    info.column = info.frames.front().column;
  }
  Value report = HolderFunction(ctx, "reportError");
  if (!qe::IsCallable(report)) {
    Print("Uncaught ", info);
    return false;
  }
  std::string message = "Uncaught ";
  message += info.is_error ? (info.name.empty() ? "" : info.name + ": ") + info.message : info.message;
  Value arguments[] = {exception, qe::FromWtf8(ctx, message), qe::FromWtf8(ctx, info.filename), qe::FromUint32(info.line), qe::FromUint32(info.column)};
  Value result = qe::Call(ctx, report, qe::Undefined(), qe::Args(arguments, 5));
  if (qe::HasException(ctx)) {
    ctx.clear_exception();
    Print("Uncaught ", info);
    return false;
  }
  const bool handled = result.to_boolean();
  if (!handled) Print("Uncaught ", info);
  return handled;
}

qe::UncaughtExceptionHandler MakeUncaughtExceptionHandler() {
  return [](const qe::UncaughtException& uncaught) {
    if (!uncaught.realm) {
      Print("Uncaught ", uncaught.info);
      return;
    }
    // As the realm the exception happened in, which is where its window is.
    RunInRealm(*uncaught.realm, [&] { ReportError(uncaught.realm->GetContext(), uncaught.exception, uncaught.info); });
  };
}

namespace {

// The rejection events held back while a module script's promise is made, as copies that outlive the call.
struct HeldRejection {
  qe::Realm* realm;
  std::shared_ptr<qe::Persistent> promise, reason;
  qe::RejectionEvent event;
};
std::vector<HeldRejection>* g_held = nullptr;

void DeliverRejection(qe::Realm* realm, const Value& promise, const Value& reason, qe::RejectionEvent event);

}  // namespace

qe::PromiseRejectionHandler MakeRejectionHandler() {
  return [](qe::Realm* realm, const Value& promise, const Value& reason, qe::RejectionEvent event) {
    if (!realm) return;
    if (g_held) {
      g_held->push_back({realm, std::make_shared<qe::Persistent>(realm->GetContext(), promise), std::make_shared<qe::Persistent>(realm->GetContext(), reason), event});
      return;
    }
    DeliverRejection(realm, promise, reason, event);
  };
}

ModulePromiseScope::ModulePromiseScope(Context& ctx) : ctx_(ctx) {
  if (!g_held) g_held = new std::vector<HeldRejection>();
}

void ModulePromiseScope::Finish(const Value& modulePromise) {
  if (finished_) return;
  finished_ = true;
  std::vector<HeldRejection> held = std::move(*g_held);
  delete g_held;
  g_held = nullptr;
  for (const HeldRejection& entry : held) {
    if (qe::IsObject(modulePromise) && entry.promise->Get() == modulePromise) continue;
    DeliverRejection(entry.realm, entry.promise->Get(), entry.reason->Get(), entry.event);
  }
}

ModulePromiseScope::~ModulePromiseScope() { Finish(qe::Undefined()); }

namespace {

void DeliverRejection(qe::Realm* realm, const Value& promise, const Value& reason, qe::RejectionEvent event) {
  {
    RunInRealm(*realm, [&] {
      Context& ctx = realm->GetContext();
      Value report = HolderFunction(ctx, "reportRejection");
      bool prevented = false;
      if (qe::IsCallable(report)) {
        Value arguments[] = {promise, reason, qe::FromBool(event == qe::RejectionEvent::Handled)};
        Value result = qe::Call(ctx, report, qe::Undefined(), qe::Args(arguments, 3));
        if (qe::HasException(ctx)) ctx.clear_exception();
        else prevented = result.to_boolean();
      }
      if (event == qe::RejectionEvent::Unhandled && !prevented) {
        const qe::ErrorInfo info = qe::InspectError(ctx, reason);
        Print("Uncaught (in promise) ", info);
      }
    });
  }
}

}  // namespace

void DefineErrorNatives(Context& ctx) {
  qe::DefineGlobalFunction(ctx, "__solarRegisterReporters", RegisterReporters, 2);
  // What an exception no script caught is given to: a window's error event, if the realm is a window.
  web::SetExceptionReporter([](Context& c, const Value& exception) {
    const qe::ErrorInfo info = qe::InspectError(c, exception);
    ReportError(c, exception, info);
  });
}

}  // namespace solar::html
