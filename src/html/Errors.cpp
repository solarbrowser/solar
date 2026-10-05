#include "solar/html/Errors.h"

#include <cstdio>
#include <memory>

#include "solar/dom/NodeBindingsInternal.h"
#include "solar/html/Frames.h"
#include "solar/html/Modules.h"
#include "solar/web/Console.h"
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

namespace {

// A rejection nobody has handled yet, held until the microtask checkpoint is over: a handler attached before then
// (promise.catch right after the rejection) makes it no rejection at all.
struct PendingRejection {
  qe::Realm* realm;
  std::shared_ptr<qe::Persistent> promise, reason;
  bool reported = false;
};
std::vector<PendingRejection>& Pending() {
  static std::vector<PendingRejection> pending;
  return pending;
}
bool g_flushQueued = false;

void DeliverRejection(qe::Realm* realm, const Value& promise, const Value& reason, qe::RejectionEvent event) {
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

void FlushRejections() {
  g_flushQueued = false;
  // What is still pending is told, once; a handler that comes later is a rejectionhandled.
  for (size_t i = 0; i < Pending().size(); ++i) {
    PendingRejection& entry = Pending()[i];
    if (entry.reported) continue;
    entry.reported = true;
    qe::Realm* realm = entry.realm;
    const Value promise = entry.promise->Get(), reason = entry.reason->Get();
    DeliverRejection(realm, promise, reason, qe::RejectionEvent::Unhandled);
  }
}

}  // namespace

qe::PromiseRejectionHandler MakeRejectionHandler() {
  return [](qe::Realm* realm, const Value& promise, const Value& reason, qe::RejectionEvent event) {
    if (!realm) return;
    Context& ctx = realm->GetContext();
    if (event == qe::RejectionEvent::Unhandled) {
      Pending().push_back({realm, std::make_shared<qe::Persistent>(ctx, promise), std::make_shared<qe::Persistent>(ctx, reason)});
      if (!g_flushQueued) {
        g_flushQueued = true;
        realm->EnqueueTask("unhandled rejections", [] { FlushRejections(); });
      }
      return;
    }
    // Handled: before it was told, nothing happened; after, it is rejectionhandled.
    for (auto it = Pending().begin(); it != Pending().end(); ++it) {
      if (!(it->promise->Get() == promise)) continue;
      const bool reported = it->reported;
      Pending().erase(it);
      if (reported) DeliverRejection(realm, promise, reason, qe::RejectionEvent::Handled);
      return;
    }
  };
}

namespace {

// A node as a developer console shows it: an element as its start tag, the rest as what they are.
std::optional<std::string> FormatNode(Context&, const Value& value) {
  const dom::Node* node = Quanta::DOMObject::Cast<dom::Node>(value);
  if (!node) return std::nullopt;
  switch (node->nodeType) {
    case dom::NodeType::Element: {
      const auto* element = static_cast<const dom::Element*>(node);
      std::string text = "<" + element->QualifiedName();
      for (const dom::Attr* attribute : element->attributes) text += " " + attribute->QualifiedName() + "=\"" + attribute->value + "\"";
      text += ">";
      if (element->firstChild) text += "…</" + element->QualifiedName() + ">";
      return text;
    }
    case dom::NodeType::Text: return "#text \"" + static_cast<const dom::CharacterData*>(node)->data + "\"";
    case dom::NodeType::CdataSection: return "#cdata-section";
    case dom::NodeType::Comment: return "<!--" + static_cast<const dom::CharacterData*>(node)->data + "-->";
    case dom::NodeType::ProcessingInstruction: return "<?" + static_cast<const dom::CharacterData*>(node)->target + " " + static_cast<const dom::CharacterData*>(node)->data + "?>";
    case dom::NodeType::Document: return "#document";
    case dom::NodeType::DocumentFragment: return static_cast<const dom::DocumentFragment*>(node)->isShadowRoot ? "#shadow-root" : "#document-fragment";
    case dom::NodeType::DocumentType: return "<!DOCTYPE " + static_cast<const dom::DocumentType*>(node)->name + ">";
    case dom::NodeType::Attribute: {
      const auto* attribute = static_cast<const dom::Attr*>(node);
      return attribute->QualifiedName() + "=\"" + attribute->value + "\"";
    }
  }
  return std::nullopt;
}

}  // namespace

void DefineErrorNatives(Context&) {
  web::SetHostObjectFormatter(FormatNode);
  // What an exception no script caught is given to: a window's error event, if the realm is a window.
  web::SetExceptionReporter([](Context& c, const Value& exception) {
    const qe::ErrorInfo info = qe::InspectError(c, exception);
    ReportError(c, exception, info);
  });
}

}  // namespace solar::html
