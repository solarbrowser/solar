#include "solar/css/Cssom.h"
#include "solar/css/MediaQuery.h"
#include "solar/dom/NodeBindingsInternal.h"
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

// The environment of the document the script runs in.
MediaEnvironment WindowEnvironment(Context& ctx) {
  Value document = qe::Get(ctx, qe::FromObject(ctx.get_global_object()), "document");
  if (!qe::IsObject(document)) return CurrentMediaEnvironment();
  dom::Node* node = dom::ThisNode(ctx, document);
  if (qe::HasException(ctx)) return CurrentMediaEnvironment();
  return node && node->IsDocument() ? EnvironmentFor(static_cast<dom::Document*>(node)) : CurrentMediaEnvironment();
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
  return self ? qe::FromBool(MediaListMatches(Queries(self->media), WindowEnvironment(ctx))) : qe::Undefined();
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

Value NoScroll(Context&, Value, qe::Args, Value) { return qe::Undefined(); }

// The window's geometry (https://drafts.csswg.org/cssom-view/#extensions-to-the-window-interface): the viewport the media
// environment describes, and a screen the size of it, scrolled to the origin.
Value InnerWidth(Context& ctx, Value, qe::Args, Value) { return qe::FromNumber(WindowEnvironment(ctx).width); }
Value InnerHeight(Context& ctx, Value, qe::Args, Value) { return qe::FromNumber(WindowEnvironment(ctx).height); }
Value PixelRatio(Context& ctx, Value, qe::Args, Value) { return qe::FromNumber(WindowEnvironment(ctx).resolution); }
Value IgnoreSet(Context&, Value, qe::Args, Value) { return qe::Undefined(); }

void DefineViewport(Context& ctx) {
  const MediaEnvironment& environment = CurrentMediaEnvironment();
  Object* global = ctx.get_global_object();
  Value globalValue = qe::FromObject(global);
  const auto number = [&](Value target, const char* name, double value) { qe::Set(ctx, target, name, qe::FromNumber(value)); };
  qe::DefineAccessor(global, "innerWidth", InnerWidth, IgnoreSet);
  qe::DefineAccessor(global, "innerHeight", InnerHeight, IgnoreSet);
  qe::DefineAccessor(global, "outerWidth", InnerWidth, IgnoreSet);
  qe::DefineAccessor(global, "outerHeight", InnerHeight, IgnoreSet);
  qe::DefineAccessor(global, "devicePixelRatio", PixelRatio, IgnoreSet);
  for (const char* name : {"screenX", "screenY", "screenLeft", "screenTop", "scrollX", "scrollY", "pageXOffset", "pageYOffset"}) number(globalValue, name, 0);
  Value screen = qe::NewObject(ctx);
  number(screen, "width", environment.deviceWidth);
  number(screen, "height", environment.deviceHeight);
  number(screen, "availWidth", environment.deviceWidth);
  number(screen, "availHeight", environment.deviceHeight);
  number(screen, "availLeft", 0);
  number(screen, "availTop", 0);
  number(screen, "colorDepth", 24);
  number(screen, "pixelDepth", 24);
  qe::Set(ctx, globalValue, "screen", screen);
  qe::DefineGlobalFunction(ctx, "scrollTo", NoScroll, 0);
  qe::DefineGlobalFunction(ctx, "scrollBy", NoScroll, 0);
  qe::DefineGlobalFunction(ctx, "scroll", NoScroll, 0);
}

void DefineMediaQueryList(Context& ctx) {
  DefineViewport(ctx);
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
