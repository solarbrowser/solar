#pragma once

#include <string>

#include "solar/dom/CustomElements.h"
#include "solar/dom/NodeBindingsInternal.h"

// Reflected attributes (https://html.spec.whatwg.org/#reflecting-content-attributes-in-idl-attributes): the IDL
// attribute that is a content attribute under another name, each as a getter and a setter in terms of the attribute's
// name, which is a template argument (a constexpr char array).
namespace solar::html::reflect {

namespace qe = Quanta::Embed;

inline dom::Element* ThisHtml(Quanta::Context& ctx, const Quanta::Value& t) {
  dom::Element* self = dom::ThisElement(ctx, t);
  if (self && !self->IsHtml()) {
    qe::ThrowTypeError(ctx, "Illegal invocation");
    return nullptr;
  }
  return self;
}

// A DOMString: the value of the attribute, or empty.
template <const char* Attribute>
Quanta::Value GetString(Quanta::Context& ctx, Quanta::Value t, qe::Args, Quanta::Value) {
  dom::Element* self = ThisHtml(ctx, t);
  return self ? qe::FromWtf8(ctx, dom::GetAttribute(self, Attribute).value_or("")) : qe::Undefined();
}

template <const char* Attribute>
Quanta::Value SetString(Quanta::Context& ctx, Quanta::Value t, qe::Args args, Quanta::Value) {
  dom::Element* self = ThisHtml(ctx, t);
  if (!self || args.empty()) return qe::Undefined();
  std::string text = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  dom::SetAttribute(ctx, self, Attribute, std::move(text));
  return qe::Undefined();
}

// A boolean attribute: there or not.
template <const char* Attribute>
Quanta::Value GetFlag(Quanta::Context& ctx, Quanta::Value t, qe::Args, Quanta::Value) {
  dom::Element* self = ThisHtml(ctx, t);
  return self ? qe::FromBool(dom::GetAttribute(self, Attribute).has_value()) : qe::Undefined();
}

template <const char* Attribute>
Quanta::Value SetFlag(Quanta::Context& ctx, Quanta::Value t, qe::Args args, Quanta::Value) {
  dom::Element* self = ThisHtml(ctx, t);
  if (!self || args.empty()) return qe::Undefined();
  if (args[0].to_boolean()) dom::SetAttribute(ctx, self, Attribute, "");
  else dom::RemoveAttribute(self, Attribute);
  return qe::Undefined();
}

// The address in an attribute, made absolute against the document's.
std::string ResolveAgainst(const dom::Element* element, const std::string& value);

template <const char* Attribute>
Quanta::Value GetUrl(Quanta::Context& ctx, Quanta::Value t, qe::Args, Quanta::Value) {
  dom::Element* self = ThisHtml(ctx, t);
  if (!self) return qe::Undefined();
  const std::optional<std::string> value = dom::GetAttribute(self, Attribute);
  if (!value) return qe::FromUtf8(ctx, "");
  return qe::FromWtf8(ctx, ResolveAgainst(self, *value));
}

// What a setter of the reflected attribute is: the same with the call's reactions run after it.
template <Quanta::Embed::NativeFn Setter>
constexpr Quanta::Embed::NativeFn Reacting = dom::Reactions<Setter>;

}  // namespace solar::html::reflect
