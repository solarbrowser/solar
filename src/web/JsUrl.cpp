#include <string>

#include "UrlBindingsInternal.h"

namespace solar::web {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Heap;
using Quanta::Object;
using Quanta::Value;

namespace {

Object* g_prototype = nullptr;

JsUrl* This(Context& ctx, const Value& thisValue) {
  JsUrl* self = DOMObject::Cast<JsUrl>(thisValue);
  if (!self) qe::ThrowTypeError(ctx, "Illegal invocation");
  return self;
}

JsUrl* Allocate(std::unique_ptr<url::UrlObject> url, Object* prototype) {
  JsUrl* js = Heap::Allocate<JsUrl>();
  js->url = std::move(url);
  js->initialize_prototype(prototype);
  return js;
}

// Absent and undefined both mean "not given" for an optional Web IDL argument.
std::optional<std::string> OptionalUsv(Context& ctx, qe::Args args, size_t index) {
  if (args.size() <= index || qe::IsUndefined(args[index])) return std::nullopt;
  return qe::ToUsvUtf8(ctx, args[index]);
}

Value Construct(Context& ctx, Value, qe::Args args, Value newTarget) {
  if (qe::IsUndefined(newTarget)) {
    qe::ThrowTypeError(ctx, "Failed to construct 'URL': Please use the 'new' operator, this DOM object "
                            "constructor cannot be called as a function.");
    return qe::Undefined();
  }
  if (!RequireArguments(ctx, args, 1, "Failed to construct 'URL'")) return qe::Undefined();

  std::string input = qe::ToUsvUtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  std::optional<std::string> base = OptionalUsv(ctx, args, 1);
  if (qe::HasException(ctx)) return qe::Undefined();

  std::unique_ptr<url::UrlObject> url = url::UrlObject::Create(input, base);
  if (!url) {
    qe::ThrowTypeError(ctx, "Failed to construct 'URL': Invalid URL");
    return qe::Undefined();
  }

  Object* prototype = qe::PrototypeFromNewTarget(ctx, newTarget);
  if (qe::HasException(ctx)) return qe::Undefined();
  return qe::FromObject(Allocate(std::move(url), prototype ? prototype : g_prototype));
}

Value Parse(Context& ctx, Value, qe::Args args, Value) {
  if (!RequireArguments(ctx, args, 1, "Failed to execute 'parse' on 'URL'")) return qe::Undefined();
  std::string input = qe::ToUsvUtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  std::optional<std::string> base = OptionalUsv(ctx, args, 1);
  if (qe::HasException(ctx)) return qe::Undefined();

  std::unique_ptr<url::UrlObject> url = url::UrlObject::Create(input, base);
  if (!url) return qe::Null();
  return qe::FromObject(Allocate(std::move(url), g_prototype));
}

Value CanParse(Context& ctx, Value, qe::Args args, Value) {
  if (!RequireArguments(ctx, args, 1, "Failed to execute 'canParse' on 'URL'")) return qe::Undefined();
  std::string input = qe::ToUsvUtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  std::optional<std::string> base = OptionalUsv(ctx, args, 1);
  if (qe::HasException(ctx)) return qe::Undefined();
  return qe::FromBool(url::UrlObject::Create(input, base) != nullptr);
}

template <std::string (url::UrlObject::*Get)() const>
Value Getter(Context& ctx, Value thisValue, qe::Args, Value) {
  JsUrl* self = This(ctx, thisValue);
  if (!self) return qe::Undefined();
  return qe::FromUtf8(ctx, ((*self->url).*Get)());
}

template <void (url::UrlObject::*Set)(std::string_view)>
Value Setter(Context& ctx, Value thisValue, qe::Args args, Value) {
  JsUrl* self = This(ctx, thisValue);
  if (!self) return qe::Undefined();
  std::string value = qe::ToUsvUtf8(ctx, args.empty() ? qe::Undefined() : args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  ((*self->url).*Set)(value);
  return qe::Undefined();
}

Value SetHref(Context& ctx, Value thisValue, qe::Args args, Value) {
  JsUrl* self = This(ctx, thisValue);
  if (!self) return qe::Undefined();
  std::string value = qe::ToUsvUtf8(ctx, args.empty() ? qe::Undefined() : args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  if (!self->url->SetHref(value)) qe::ThrowTypeError(ctx, "Failed to set the 'href' property on 'URL': Invalid URL");
  return qe::Undefined();
}

Value GetSearchParams(Context& ctx, Value thisValue, qe::Args, Value) {
  JsUrl* self = This(ctx, thisValue);
  if (!self) return qe::Undefined();

  if (!self->searchParams) {
    JsUrlSearchParams* params = Heap::Allocate<JsUrlSearchParams>();
    params->owner = self;
    params->initialize_prototype(UrlSearchParamsPrototype());
    self->searchParams = params;
    // self may already have survived a collection, which would then not trace it again.
    self->NoteWrite();
  }
  return qe::FromObject(self->searchParams);
}

}  // namespace

void DefineUrlClass(qe::Runtime& runtime) {
  Context& ctx = runtime.GetContext();

  qe::ClassRef urlClass = qe::DefineClass(ctx, "URL", Construct, 1);
  g_prototype = urlClass.prototype;

  qe::DefineStaticMethod(urlClass.constructor, "parse", Parse, 1);
  qe::DefineStaticMethod(urlClass.constructor, "canParse", CanParse, 1);

  qe::DefineAccessor(urlClass.prototype, "href", Getter<&url::UrlObject::Href>, SetHref);
  qe::DefineAccessor(urlClass.prototype, "origin", Getter<&url::UrlObject::Origin>, nullptr);
  qe::DefineAccessor(urlClass.prototype, "protocol", Getter<&url::UrlObject::Protocol>, Setter<&url::UrlObject::SetProtocol>);
  qe::DefineAccessor(urlClass.prototype, "username", Getter<&url::UrlObject::Username>, Setter<&url::UrlObject::SetUsername>);
  qe::DefineAccessor(urlClass.prototype, "password", Getter<&url::UrlObject::Password>, Setter<&url::UrlObject::SetPassword>);
  qe::DefineAccessor(urlClass.prototype, "host", Getter<&url::UrlObject::Host>, Setter<&url::UrlObject::SetHost>);
  qe::DefineAccessor(urlClass.prototype, "hostname", Getter<&url::UrlObject::Hostname>, Setter<&url::UrlObject::SetHostname>);
  qe::DefineAccessor(urlClass.prototype, "port", Getter<&url::UrlObject::Port>, Setter<&url::UrlObject::SetPort>);
  qe::DefineAccessor(urlClass.prototype, "pathname", Getter<&url::UrlObject::Pathname>, Setter<&url::UrlObject::SetPathname>);
  qe::DefineAccessor(urlClass.prototype, "search", Getter<&url::UrlObject::Search>, Setter<&url::UrlObject::SetSearch>);
  qe::DefineAccessor(urlClass.prototype, "searchParams", GetSearchParams, nullptr);
  qe::DefineAccessor(urlClass.prototype, "hash", Getter<&url::UrlObject::Hash>, Setter<&url::UrlObject::SetHash>);

  // The stringifier and toJSON both return href, so they share the getter's body.
  qe::DefineMethod(urlClass.prototype, "toString", Getter<&url::UrlObject::Href>, 0);
  qe::DefineMethod(urlClass.prototype, "toJSON", Getter<&url::UrlObject::Href>, 0);

  qe::DefineGlobal(ctx, "URL", urlClass.constructor);
}

}  // namespace solar::web
