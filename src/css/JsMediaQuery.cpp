#include "solar/css/Cssom.h"
#include "solar/css/MediaQuery.h"
#include "solar/web/DomBindingsInternal.h"

// window.matchMedia and MediaQueryList (https://drafts.csswg.org/cssom-view/#the-mediaquerylist-interface).
namespace solar::css {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::Heap;
using Quanta::Object;
using Quanta::Value;

struct CssMediaQueryList : web::JsEventTarget {
  using Parent = web::JsEventTarget;
  std::string media;
  void Visit(Quanta::Visitor& visitor) { web::JsEventTarget::Visit(visitor); }
};

namespace {

char g_key;

CssMediaQueryList* This(Context& ctx, const Value& t) {
  CssMediaQueryList* self = Quanta::DOMObject::Cast<CssMediaQueryList>(t);
  if (!self) qe::ThrowTypeError(ctx, "Illegal invocation");
  return self;
}

Value IllegalConstructor(Context& ctx, Value, qe::Args, Value) {
  qe::ThrowTypeError(ctx, "Illegal constructor");
  return qe::Undefined();
}

std::vector<std::string> Queries(const std::string& media) {
  std::vector<std::string> queries;
  size_t start = 0;
  // The text is already a serialized list: queries are separated by ", ".
  while (start <= media.size() && !media.empty()) {
    size_t comma = media.find(", ", start);
    queries.push_back(media.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
    if (comma == std::string::npos) break;
    start = comma + 2;
  }
  return queries;
}

Value MatchMedia(Context& ctx, Value, qe::Args args, Value) {
  if (args.empty()) {
    qe::ThrowTypeError(ctx, "Failed to execute 'matchMedia' on 'Window': 1 argument required, but only 0 present.");
    return qe::Undefined();
  }
  const std::string query = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  MediaList* list = NewMediaList(ctx);
  list->SetText(query);
  CssMediaQueryList* result = Heap::Allocate<CssMediaQueryList>();
  result->initialize_prototype(static_cast<Object*>(qe::GetRealmData(ctx, &g_key)));
  result->media = list->Text();
  return qe::FromObject(result);
}

Value GetMedia(Context& ctx, Value t, qe::Args, Value) {
  CssMediaQueryList* self = This(ctx, t);
  return self ? qe::FromWtf8(ctx, self->media) : qe::Undefined();
}

Value GetMatches(Context& ctx, Value t, qe::Args, Value) {
  CssMediaQueryList* self = This(ctx, t);
  return self ? qe::FromBool(MediaListMatches(Queries(self->media), CurrentMediaEnvironment())) : qe::Undefined();
}

Value AddListener(Context& ctx, Value t, qe::Args args, Value) {
  CssMediaQueryList* self = This(ctx, t);
  if (!self) return qe::Undefined();
  if (args.empty() || args[0].is_null() || args[0].is_undefined()) return qe::Undefined();
  Value type = qe::FromWtf8(ctx, "change");
  Value arguments[] = {type, args[0]};
  qe::Call(ctx, qe::Get(ctx, t, "addEventListener"), t, qe::Args(arguments, 2));
  return qe::Undefined();
}

Value RemoveListener(Context& ctx, Value t, qe::Args args, Value) {
  CssMediaQueryList* self = This(ctx, t);
  if (!self) return qe::Undefined();
  if (args.empty() || args[0].is_null() || args[0].is_undefined()) return qe::Undefined();
  Value type = qe::FromWtf8(ctx, "change");
  Value arguments[] = {type, args[0]};
  qe::Call(ctx, qe::Get(ctx, t, "removeEventListener"), t, qe::Args(arguments, 2));
  return qe::Undefined();
}

Value GetOnChange(Context& ctx, Value t, qe::Args, Value) {
  CssMediaQueryList* self = This(ctx, t);
  return self ? web::GetEventHandler(self, "change") : qe::Undefined();
}

Value SetOnChange(Context& ctx, Value t, qe::Args args, Value) {
  CssMediaQueryList* self = This(ctx, t);
  if (self) web::SetEventHandler(self, "change", !args.empty() && qe::IsCallable(args[0]) ? args[0] : qe::Null());
  return qe::Undefined();
}

}  // namespace

void DefineMediaQueryList(Context& ctx) {
  qe::ClassRef list = qe::DefineClass(ctx, "MediaQueryList", IllegalConstructor, 0, web::EventTargetPrototype(ctx));
  qe::SetRealmData(ctx, &g_key, list.prototype);
  qe::DefineAccessor(list.prototype, "media", GetMedia, nullptr);
  qe::DefineAccessor(list.prototype, "matches", GetMatches, nullptr);
  qe::DefineAccessor(list.prototype, "onchange", GetOnChange, SetOnChange);
  qe::DefineMethod(list.prototype, "addListener", AddListener, 1);
  qe::DefineMethod(list.prototype, "removeListener", RemoveListener, 1);
  qe::DefineGlobal(ctx, "MediaQueryList", list.constructor);
  qe::DefineGlobalFunction(ctx, "matchMedia", MatchMedia, 1);
}

}  // namespace solar::css
