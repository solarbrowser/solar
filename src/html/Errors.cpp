#include "solar/html/Errors.h"

#include <cstdio>

#include "solar/dom/NodeBindingsInternal.h"
#include "solar/html/Frames.h"
#include "solar/html/Modules.h"
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

}  // namespace

bool ReportError(Context& ctx, const Value& exception, const qe::ErrorInfo& info) {
  Value report = HolderFunction(ctx, "reportError");
  if (!qe::IsCallable(report)) {
    Print("Uncaught ", info);
    return false;
  }
  std::string message = "Uncaught ";
  message += info.is_error ? (info.name.empty() ? "" : info.name + ": ") + info.message : info.message;
  Value arguments[] = {exception, qe::FromWtf8(ctx, message), qe::FromWtf8(ctx, VisibleModuleUrl(info.filename)), qe::FromUint32(info.line), qe::FromUint32(info.column)};
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

qe::PromiseRejectionHandler MakeRejectionHandler() {
  return [](qe::Realm* realm, const Value& promise, const Value& reason, qe::RejectionEvent event) {
    if (!realm) return;
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
  };
}

void DefineErrorNatives(Context&) {
  // What an exception no script caught is given to: a window's error event, if the realm is a window.
  web::SetExceptionReporter([](Context& c, const Value& exception) {
    const qe::ErrorInfo info = qe::InspectError(c, exception);
    ReportError(c, exception, info);
  });
}

}  // namespace solar::html
