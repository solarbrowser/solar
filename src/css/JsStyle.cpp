#include <algorithm>
#include <string>
#include <vector>

#include "solar/dom/CustomElements.h"
#include "solar/dom/NodeBindingsInternal.h"

namespace solar::css {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Heap;
using Quanta::Object;
using Quanta::Value;

// element.style, for now as little as it takes for script to set and read the properties of the style
// attribute. A style sheet's rules and the computed style come with the cascade; the property table is
// short until then, and what is not in it is not a property of the object.

namespace {

char g_styleKey;

const char* const kProperties[] = {
    "align-content", "align-items", "align-self", "all", "animation", "background", "background-attachment", "background-clip", "background-color", "background-image",
    "background-origin", "background-position", "background-repeat", "background-size", "border", "border-bottom", "border-bottom-color", "border-bottom-style",
    "border-bottom-width", "border-collapse", "border-color", "border-left", "border-left-color", "border-left-style", "border-left-width", "border-radius", "border-right",
    "border-right-color", "border-right-style", "border-right-width", "border-spacing", "border-style", "border-top", "border-top-color", "border-top-style", "border-top-width",
    "border-width", "bottom", "box-shadow", "box-sizing", "clear", "clip", "color", "content", "cursor", "direction", "display", "flex", "flex-basis", "flex-direction",
    "flex-flow", "flex-grow", "flex-shrink", "flex-wrap", "float", "font", "font-family", "font-size", "font-style", "font-variant", "font-weight", "gap", "grid",
    "height", "justify-content", "left", "letter-spacing", "line-height", "list-style", "list-style-type", "margin", "margin-bottom", "margin-left", "margin-right",
    "margin-top", "max-height", "max-width", "min-height", "min-width", "opacity", "order", "outline", "overflow", "overflow-x", "overflow-y", "padding", "padding-bottom",
    "padding-left", "padding-right", "padding-top", "pointer-events", "position", "right", "table-layout", "text-align", "text-decoration", "text-indent", "text-overflow",
    "text-transform", "top", "transform", "transition", "unicode-bidi", "vertical-align", "visibility", "white-space", "width", "word-break", "word-spacing", "z-index",
};

// "backgroundColor" and "background-color" are the same property: the dashed name, or empty if it is none.
std::string DashedName(const std::string& name) {
  std::string dashed;
  for (char c : name) {
    if (c >= 'A' && c <= 'Z') {
      dashed += '-';
      dashed += static_cast<char>(c + 0x20);
    } else {
      dashed += c;
    }
  }
  // cssFloat is float.
  if (dashed == "css-float") dashed = "float";
  return std::find(std::begin(kProperties), std::end(kProperties), dashed) != std::end(kProperties) ? dashed : std::string();
}

struct Declaration {
  std::string name;
  std::string value;
  bool important = false;
};

std::string Trim(std::string text) {
  const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; };
  while (!text.empty() && space(text.back())) text.pop_back();
  size_t start = 0;
  while (start < text.size() && space(text[start])) ++start;
  return text.substr(start);
}

std::vector<Declaration> ParseDeclarations(const std::string& text) {
  std::vector<Declaration> list;
  std::string current;
  int depth = 0;
  char quote = 0;
  std::vector<std::string> parts;
  for (char c : text) {
    if (quote) {
      if (c == quote) quote = 0;
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '(') {
      ++depth;
    } else if (c == ')' && depth > 0) {
      --depth;
    } else if (c == ';' && depth == 0) {
      parts.push_back(current);
      current.clear();
      continue;
    }
    current += c;
  }
  parts.push_back(current);
  for (const std::string& part : parts) {
    const size_t colon = part.find(':');
    if (colon == std::string::npos) continue;
    Declaration declaration;
    declaration.name = Trim(part.substr(0, colon));
    for (char& c : declaration.name) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 0x20);
    }
    declaration.value = Trim(part.substr(colon + 1));
    const std::string bang = "!important";
    if (declaration.value.size() >= bang.size() && declaration.value.compare(declaration.value.size() - bang.size(), bang.size(), bang) == 0) {
      declaration.important = true;
      declaration.value = Trim(declaration.value.substr(0, declaration.value.size() - bang.size()));
    }
    if (declaration.name.empty() || declaration.value.empty()) continue;
    // A later declaration of a property replaces an earlier one.
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Declaration& d) { return d.name == declaration.name; }), list.end());
    list.push_back(std::move(declaration));
  }
  return list;
}

std::string Serialize(const std::vector<Declaration>& list) {
  std::string text;
  for (const Declaration& d : list) {
    if (!text.empty()) text += ' ';
    text += d.name + ": " + d.value + (d.important ? " !important" : "") + ";";
  }
  return text;
}

}  // namespace

struct JsStyle : DOMObject {
  dom::Element* element = nullptr;

  std::vector<Declaration> Declarations() const {
    const dom::Attr* style = element->FindAttribute("", "style");
    return style ? ParseDeclarations(style->value) : std::vector<Declaration>();
  }
  void Store(Context& ctx, const std::vector<Declaration>& list) const { dom::SetAttribute(ctx, element, "style", Serialize(list)); }

  static bool IndexedGetter(Context& ctx, JsStyle& self, uint32_t index, Value& out) {
    const std::vector<Declaration> list = self.Declarations();
    if (index >= list.size()) return false;
    out = qe::FromWtf8(ctx, list[index].name);
    return true;
  }
  static uint32_t IndexedLength(Context&, JsStyle& self) { return static_cast<uint32_t>(self.Declarations().size()); }

  static bool NamedGetter(Context& ctx, JsStyle& self, const std::string& name, Value& out) {
    const std::string property = DashedName(name);
    if (property.empty()) return false;
    for (const Declaration& d : self.Declarations()) {
      if (d.name == property) {
        out = qe::FromWtf8(ctx, d.value);
        return true;
      }
    }
    out = qe::FromWtf8(ctx, "");
    return true;
  }
  static void NamedSetter(Context& ctx, JsStyle& self, const std::string& name, const Value& value) {
    dom::ReactionsScope reactions(ctx);
    const std::string property = DashedName(name);
    if (property.empty()) return;
    const std::string text = qe::ToWtf8(ctx, value);
    if (qe::HasException(ctx)) return;
    std::vector<Declaration> list = self.Declarations();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Declaration& d) { return d.name == property; }), list.end());
    if (!Trim(text).empty()) list.push_back({property, Trim(text), false});
    self.Store(ctx, list);
  }
  static std::vector<std::string> NamedKeys(Context&, JsStyle&) { return {}; }
  static constexpr bool LegacyUnenumerableNamedProperties = true;

  void Visit(Quanta::Visitor& visitor) { visitor.Mark(element); }
};

namespace {

JsStyle* ThisStyle(Context& ctx, const Value& t) {
  JsStyle* style = DOMObject::Cast<JsStyle>(t);
  if (!style) qe::ThrowTypeError(ctx, "Illegal invocation");
  return style;
}

Value GetLength(Context& ctx, Value t, qe::Args, Value) {
  JsStyle* self = ThisStyle(ctx, t);
  return self ? qe::FromUint32(static_cast<uint32_t>(self->Declarations().size())) : qe::Undefined();
}

Value GetCssText(Context& ctx, Value t, qe::Args, Value) {
  JsStyle* self = ThisStyle(ctx, t);
  return self ? qe::FromWtf8(ctx, Serialize(self->Declarations())) : qe::Undefined();
}

Value SetCssText(Context& ctx, Value t, qe::Args args, Value) {
  JsStyle* self = ThisStyle(ctx, t);
  if (!self || args.empty()) return qe::Undefined();
  const std::string text = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  self->Store(ctx, ParseDeclarations(text));
  return qe::Undefined();
}

Value Item(Context& ctx, Value t, qe::Args args, Value) {
  JsStyle* self = ThisStyle(ctx, t);
  if (!self || args.empty()) return qe::Undefined();
  const uint32_t index = qe::ToUint32(ctx, args[0]);
  const std::vector<Declaration> list = self->Declarations();
  return index < list.size() ? qe::FromWtf8(ctx, list[index].name) : qe::FromWtf8(ctx, "");
}

std::string LowerName(const std::string& name) {
  std::string out = name;
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 0x20);
  }
  return out;
}

Value GetPropertyValue(Context& ctx, Value t, qe::Args args, Value) {
  JsStyle* self = ThisStyle(ctx, t);
  if (!self || args.empty()) return qe::Undefined();
  const std::string name = LowerName(qe::ToWtf8(ctx, args[0]));
  for (const Declaration& d : self->Declarations()) {
    if (d.name == name) return qe::FromWtf8(ctx, d.value);
  }
  return qe::FromWtf8(ctx, "");
}

Value GetPropertyPriority(Context& ctx, Value t, qe::Args args, Value) {
  JsStyle* self = ThisStyle(ctx, t);
  if (!self || args.empty()) return qe::Undefined();
  const std::string name = LowerName(qe::ToWtf8(ctx, args[0]));
  for (const Declaration& d : self->Declarations()) {
    if (d.name == name) return qe::FromWtf8(ctx, d.important ? "important" : "");
  }
  return qe::FromWtf8(ctx, "");
}

Value SetProperty(Context& ctx, Value t, qe::Args args, Value) {
  JsStyle* self = ThisStyle(ctx, t);
  if (!self || args.size() < 2) return qe::Undefined();
  const std::string name = LowerName(qe::ToWtf8(ctx, args[0]));
  const std::string value = Trim(qe::ToWtf8(ctx, args[1]));
  const std::string priority = args.size() > 2 ? LowerName(qe::ToWtf8(ctx, args[2])) : "";
  if (qe::HasException(ctx)) return qe::Undefined();
  std::vector<Declaration> list = self->Declarations();
  list.erase(std::remove_if(list.begin(), list.end(), [&](const Declaration& d) { return d.name == name; }), list.end());
  if (!value.empty() && (priority.empty() || priority == "important")) list.push_back({name, value, priority == "important"});
  self->Store(ctx, list);
  return qe::Undefined();
}

Value RemoveProperty(Context& ctx, Value t, qe::Args args, Value) {
  JsStyle* self = ThisStyle(ctx, t);
  if (!self || args.empty()) return qe::Undefined();
  const std::string name = LowerName(qe::ToWtf8(ctx, args[0]));
  std::vector<Declaration> list = self->Declarations();
  std::string old;
  for (const Declaration& d : list) {
    if (d.name == name) old = d.value;
  }
  list.erase(std::remove_if(list.begin(), list.end(), [&](const Declaration& d) { return d.name == name; }), list.end());
  self->Store(ctx, list);
  return qe::FromWtf8(ctx, old);
}

Value IllegalConstructor(Context& ctx, Value, qe::Args, Value) {
  qe::ThrowTypeError(ctx, "Illegal constructor");
  return qe::Undefined();
}

Value GetStyle(Context& ctx, Value t, qe::Args, Value) {
  dom::Element* self = dom::ThisElement(ctx, t);
  if (!self) return qe::Undefined();
  // [SameObject] once there is a slot to keep it in; for now it is as good as new each time, as it holds nothing.
  JsStyle* style = Heap::Allocate<JsStyle>();
  style->initialize_prototype(static_cast<Object*>(qe::GetRealmData(ctx, &g_styleKey)));
  style->element = self;
  return qe::FromObject(style);
}

}  // namespace

void DefineStyleDeclaration(Context& ctx) {
  qe::ClassRef declaration = qe::DefineClass(ctx, "CSSStyleDeclaration", IllegalConstructor, 0);
  qe::SetRealmData(ctx, &g_styleKey, declaration.prototype);
  Object* p = declaration.prototype;
  qe::DefineAccessor(p, "length", GetLength, nullptr);
  qe::DefineAccessor(p, "cssText", GetCssText, dom::Reactions<SetCssText>);
  qe::DefineMethod(p, "item", Item, 1);
  qe::DefineMethod(p, "getPropertyValue", GetPropertyValue, 1);
  qe::DefineMethod(p, "getPropertyPriority", GetPropertyPriority, 1);
  qe::DefineMethod(p, "setProperty", dom::Reactions<SetProperty>, 2);
  qe::DefineMethod(p, "removeProperty", dom::Reactions<RemoveProperty>, 1);
  qe::DefineGlobal(ctx, "CSSStyleDeclaration", declaration.constructor);
  if (Object* element = dom::InterfacePrototype(ctx, dom::Interface::HtmlElement)) qe::DefineAccessor(element, "style", GetStyle, nullptr);
}

}  // namespace solar::css
