#include <algorithm>
#include <string>

#include "solar/dom/NodeBindingsInternal.h"

namespace solar::dom {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Object;
using Quanta::Value;

namespace {

bool Missing(Context& ctx, qe::Args args, size_t count, const char* member) {
  if (args.size() >= count) return false;
  qe::ThrowTypeError(ctx, std::string("Failed to execute '") + member + "' on 'Element': " + std::to_string(count) + " argument" + (count == 1 ? "" : "s") + " required, but only " +
                              std::to_string(args.size()) + " present.");
  return true;
}

Value IllegalConstructor(Context& ctx, Value, qe::Args, Value) {
  qe::ThrowTypeError(ctx, "Illegal constructor");
  return qe::Undefined();
}

Value NullableUtf8(Context& ctx, const std::string& text) { return text.empty() ? qe::Null() : qe::FromWtf8(ctx, text); }

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 0x20);
  }
  return out;
}

Value GetNamespaceUri(Context& ctx, Value t, qe::Args, Value) {
  Element* self = ThisElement(ctx, t);
  return self ? NullableUtf8(ctx, self->namespaceUri) : qe::Undefined();
}
Value GetPrefix(Context& ctx, Value t, qe::Args, Value) {
  Element* self = ThisElement(ctx, t);
  return self ? NullableUtf8(ctx, self->prefix) : qe::Undefined();
}
Value GetLocalName(Context& ctx, Value t, qe::Args, Value) {
  Element* self = ThisElement(ctx, t);
  return self ? qe::FromWtf8(ctx, self->localName) : qe::Undefined();
}
Value GetTagName(Context& ctx, Value t, qe::Args, Value) {
  Element* self = ThisElement(ctx, t);
  return self ? qe::FromWtf8(ctx, TagNameOf(self)) : qe::Undefined();
}

// The attributes that reflect: id, class and slot are strings in no namespace.
template <const char* Name>
Value GetReflected(Context& ctx, Value t, qe::Args, Value) {
  Element* self = ThisElement(ctx, t);
  if (!self) return qe::Undefined();
  const Attr* attribute = self->FindAttribute("", Name);
  return qe::FromWtf8(ctx, attribute ? attribute->value : "");
}

template <const char* Name>
Value SetReflected(Context& ctx, Value t, qe::Args args, Value) {
  Element* self = ThisElement(ctx, t);
  if (!self || Missing(ctx, args, 1, Name)) return qe::Undefined();
  std::string text = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  if (auto error = SetAttribute(ctx, self, Name, std::move(text))) Throw(ctx, *error);
  return qe::Undefined();
}

constexpr char kId[] = "id";
constexpr char kClass[] = "class";
constexpr char kSlot[] = "slot";

Value GetAttributes(Context& ctx, Value t, qe::Args, Value) {
  Element* self = ThisElement(ctx, t);
  if (!self) return qe::Undefined();
  if (!self->attributeMap) {
    self->attributeMap = NewNamedNodeMap(ctx, self);
    self->NoteWrite();
  }
  return qe::FromObject(self->attributeMap);
}

Value GetClassList(Context& ctx, Value t, qe::Args, Value) {
  Element* self = ThisElement(ctx, t);
  if (!self) return qe::Undefined();
  if (!self->tokenList) {
    self->tokenList = NewTokenList(ctx, self, "class");
    self->NoteWrite();
  }
  return qe::FromObject(self->tokenList);
}

// [PutForwards=value]: assigning to classList assigns to its value.
Value SetClassList(Context& ctx, Value t, qe::Args args, Value) {
  Element* self = ThisElement(ctx, t);
  if (!self || Missing(ctx, args, 1, "classList")) return qe::Undefined();
  std::string text = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  if (auto error = SetAttribute(ctx, self, "class", std::move(text))) Throw(ctx, *error);
  return qe::Undefined();
}

Value HasAttributes(Context& ctx, Value t, qe::Args, Value) {
  Element* self = ThisElement(ctx, t);
  return self ? qe::FromBool(!self->attributes.empty()) : qe::Undefined();
}

Value GetAttributeNames(Context& ctx, Value t, qe::Args, Value) {
  Element* self = ThisElement(ctx, t);
  if (!self) return qe::Undefined();
  Value array = qe::NewArray(ctx);
  for (const Attr* attribute : self->attributes) qe::ArrayPush(ctx, array, qe::FromWtf8(ctx, attribute->QualifiedName()));
  return array;
}

Value GetAttributeMethod(Context& ctx, Value t, qe::Args args, Value) {
  Element* self = ThisElement(ctx, t);
  if (!self || Missing(ctx, args, 1, "getAttribute")) return qe::Undefined();
  const std::string name = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  const std::optional<std::string> value = GetAttribute(self, name);
  return value ? qe::FromWtf8(ctx, *value) : qe::Null();
}

Value GetAttributeNs(Context& ctx, Value t, qe::Args args, Value) {
  Element* self = ThisElement(ctx, t);
  if (!self || Missing(ctx, args, 2, "getAttributeNS")) return qe::Undefined();
  const std::optional<std::string> ns = NullableString(ctx, args[0]);
  const std::string local = qe::ToWtf8(ctx, args[1]);
  if (qe::HasException(ctx)) return qe::Undefined();
  const Attr* attribute = self->FindAttribute(ns.value_or(""), local);
  return attribute ? qe::FromWtf8(ctx, attribute->value) : qe::Null();
}

Value SetAttributeMethod(Context& ctx, Value t, qe::Args args, Value) {
  Element* self = ThisElement(ctx, t);
  if (!self || Missing(ctx, args, 2, "setAttribute")) return qe::Undefined();
  const std::string name = qe::ToWtf8(ctx, args[0]);
  std::string value = qe::HasException(ctx) ? "" : qe::ToWtf8(ctx, args[1]);
  if (qe::HasException(ctx)) return qe::Undefined();
  if (auto error = SetAttribute(ctx, self, name, std::move(value))) Throw(ctx, *error);
  return qe::Undefined();
}

Value SetAttributeNs(Context& ctx, Value t, qe::Args args, Value) {
  Element* self = ThisElement(ctx, t);
  if (!self || Missing(ctx, args, 3, "setAttributeNS")) return qe::Undefined();
  const std::optional<std::string> ns = NullableString(ctx, args[0]);
  const std::string qualified = qe::HasException(ctx) ? "" : qe::ToWtf8(ctx, args[1]);
  std::string value = qe::HasException(ctx) ? "" : qe::ToWtf8(ctx, args[2]);
  if (qe::HasException(ctx)) return qe::Undefined();
  QualifiedParts parts;
  if (auto error = ValidateAndExtract(ns.value_or(""), qualified, parts)) {
    Throw(ctx, *error);
    return qe::Undefined();
  }
  if (Attr* existing = self->FindAttribute(parts.namespaceUri, parts.localName)) {
    existing->value = std::move(value);
    existing->prefix = parts.prefix;
    NoteTreeChange();
    return qe::Undefined();
  }
  Attr* attribute = NewAttr(ctx, self->nodeDocument, parts.namespaceUri, parts.prefix, parts.localName, std::move(value));
  attribute->ownerElement = self;
  self->attributes.push_back(attribute);
  self->NoteWrite();
  NoteTreeChange();
  return qe::Undefined();
}

Value RemoveAttributeMethod(Context& ctx, Value t, qe::Args args, Value) {
  Element* self = ThisElement(ctx, t);
  if (!self || Missing(ctx, args, 1, "removeAttribute")) return qe::Undefined();
  const std::string name = qe::ToWtf8(ctx, args[0]);
  if (!qe::HasException(ctx)) RemoveAttribute(self, name);
  return qe::Undefined();
}

Value RemoveAttributeNs(Context& ctx, Value t, qe::Args args, Value) {
  Element* self = ThisElement(ctx, t);
  if (!self || Missing(ctx, args, 2, "removeAttributeNS")) return qe::Undefined();
  const std::optional<std::string> ns = NullableString(ctx, args[0]);
  const std::string local = qe::HasException(ctx) ? "" : qe::ToWtf8(ctx, args[1]);
  if (qe::HasException(ctx)) return qe::Undefined();
  Attr* attribute = self->FindAttribute(ns.value_or(""), local);
  if (attribute) RemoveAttributeNode(self, attribute);
  return qe::Undefined();
}

Value ToggleAttribute(Context& ctx, Value t, qe::Args args, Value) {
  Element* self = ThisElement(ctx, t);
  if (!self || Missing(ctx, args, 1, "toggleAttribute")) return qe::Undefined();
  const std::string name = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  const bool hasForce = args.size() > 1 && !qe::IsUndefined(args[1]);
  const bool force = hasForce && args[1].to_boolean();
  if (!IsXmlName(name)) {
    Throw(ctx, {"InvalidCharacterError", "The string contains invalid characters"});
    return qe::Undefined();
  }
  if (!GetAttribute(self, name)) {
    if (!hasForce || force) {
      SetAttribute(ctx, self, name, "");
      return qe::FromBool(true);
    }
    return qe::FromBool(false);
  }
  if (!hasForce || !force) {
    RemoveAttribute(self, name);
    return qe::FromBool(false);
  }
  return qe::FromBool(true);
}

Value HasAttributeMethod(Context& ctx, Value t, qe::Args args, Value) {
  Element* self = ThisElement(ctx, t);
  if (!self || Missing(ctx, args, 1, "hasAttribute")) return qe::Undefined();
  const std::string name = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  return qe::FromBool(GetAttribute(self, name).has_value());
}

Value HasAttributeNs(Context& ctx, Value t, qe::Args args, Value) {
  Element* self = ThisElement(ctx, t);
  if (!self || Missing(ctx, args, 2, "hasAttributeNS")) return qe::Undefined();
  const std::optional<std::string> ns = NullableString(ctx, args[0]);
  const std::string local = qe::HasException(ctx) ? "" : qe::ToWtf8(ctx, args[1]);
  if (qe::HasException(ctx)) return qe::Undefined();
  return qe::FromBool(self->FindAttribute(ns.value_or(""), local) != nullptr);
}

Value GetAttributeNode(Context& ctx, Value t, qe::Args args, Value) {
  Element* self = ThisElement(ctx, t);
  if (!self || Missing(ctx, args, 1, "getAttributeNode")) return qe::Undefined();
  std::string name = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  if (self->IsHtml() && self->nodeDocument && self->nodeDocument->isHtml) name = Lower(name);
  return NodeValue(self->FindAttribute(name));
}

Value GetAttributeNodeNs(Context& ctx, Value t, qe::Args args, Value) {
  Element* self = ThisElement(ctx, t);
  if (!self || Missing(ctx, args, 2, "getAttributeNodeNS")) return qe::Undefined();
  const std::optional<std::string> ns = NullableString(ctx, args[0]);
  const std::string local = qe::HasException(ctx) ? "" : qe::ToWtf8(ctx, args[1]);
  if (qe::HasException(ctx)) return qe::Undefined();
  return NodeValue(self->FindAttribute(ns.value_or(""), local));
}

Value SetAttributeNodeMethod(Context& ctx, Value t, qe::Args args, Value) {
  Element* self = ThisElement(ctx, t);
  if (!self || Missing(ctx, args, 1, "setAttributeNode")) return qe::Undefined();
  Node* node = DOMObject::Cast<Node>(args[0]);
  if (!node || node->nodeType != NodeType::Attribute) {
    qe::ThrowTypeError(ctx, "Failed to execute 'setAttributeNode' on 'Element': parameter 1 is not of type 'Attr'.");
    return qe::Undefined();
  }
  Attr* replaced = nullptr;
  if (auto error = SetAttributeNode(self, static_cast<Attr*>(node), replaced)) {
    Throw(ctx, *error);
    return qe::Undefined();
  }
  return NodeValue(replaced);
}

Value RemoveAttributeNodeMethod(Context& ctx, Value t, qe::Args args, Value) {
  Element* self = ThisElement(ctx, t);
  if (!self || Missing(ctx, args, 1, "removeAttributeNode")) return qe::Undefined();
  Node* node = DOMObject::Cast<Node>(args[0]);
  if (!node || node->nodeType != NodeType::Attribute) {
    qe::ThrowTypeError(ctx, "Failed to execute 'removeAttributeNode' on 'Element': parameter 1 is not of type 'Attr'.");
    return qe::Undefined();
  }
  Attr* attribute = static_cast<Attr*>(node);
  if (attribute->ownerElement != self) {
    Throw(ctx, {"NotFoundError", "The attribute is not an attribute of this element"});
    return qe::Undefined();
  }
  RemoveAttributeNode(self, attribute);
  return qe::FromObject(attribute);
}

Value GetElementsByTagNameMethod(Context& ctx, Value t, qe::Args args, Value) {
  Element* self = ThisElement(ctx, t);
  if (!self || Missing(ctx, args, 1, "getElementsByTagName")) return qe::Undefined();
  const std::string name = qe::ToWtf8(ctx, args[0]);
  return qe::HasException(ctx) ? qe::Undefined() : GetElementsByTagName(ctx, self, name);
}

Value GetElementsByTagNameNsMethod(Context& ctx, Value t, qe::Args args, Value) {
  Element* self = ThisElement(ctx, t);
  if (!self || Missing(ctx, args, 2, "getElementsByTagNameNS")) return qe::Undefined();
  const std::optional<std::string> ns = NullableString(ctx, args[0]);
  const std::string local = qe::HasException(ctx) ? "" : qe::ToWtf8(ctx, args[1]);
  return qe::HasException(ctx) ? qe::Undefined() : GetElementsByTagNameNS(ctx, self, ns.value_or(""), local);
}

Value GetElementsByClassNameMethod(Context& ctx, Value t, qe::Args args, Value) {
  Element* self = ThisElement(ctx, t);
  if (!self || Missing(ctx, args, 1, "getElementsByClassName")) return qe::Undefined();
  const std::string classes = qe::ToWtf8(ctx, args[0]);
  return qe::HasException(ctx) ? qe::Undefined() : GetElementsByClassName(ctx, self, classes);
}

// "insert adjacent": the place a position names, or false with the SyntaxError raised.
bool Adjacent(Context& ctx, Element* self, const std::string& where, Node* node, Value& result) {
  const std::string place = Lower(where);
  std::optional<DomError> error;
  result = qe::FromObject(node);
  if (place == "beforebegin") {
    if (!self->parentNode) { result = qe::Null(); return true; }
    error = PreInsert(node, self->parentNode, self);
  } else if (place == "afterbegin") {
    error = PreInsert(node, self, self->firstChild);
  } else if (place == "beforeend") {
    error = PreInsert(node, self, nullptr);
  } else if (place == "afterend") {
    if (!self->parentNode) { result = qe::Null(); return true; }
    error = PreInsert(node, self->parentNode, self->nextSibling);
  } else {
    Throw(ctx, {"SyntaxError", "The value provided ('" + where + "') is not one of 'beforebegin', 'afterbegin', 'beforeend', or 'afterend'."});
    return false;
  }
  if (error) {
    Throw(ctx, *error);
    return false;
  }
  return true;
}

Value InsertAdjacentElement(Context& ctx, Value t, qe::Args args, Value) {
  Element* self = ThisElement(ctx, t);
  if (!self || Missing(ctx, args, 2, "insertAdjacentElement")) return qe::Undefined();
  const std::string where = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  Node* node = DOMObject::Cast<Node>(args[1]);
  if (!node || !node->IsElement()) {
    qe::ThrowTypeError(ctx, "Failed to execute 'insertAdjacentElement' on 'Element': parameter 2 is not of type 'Element'.");
    return qe::Undefined();
  }
  Value result;
  return Adjacent(ctx, self, where, node, result) ? result : qe::Undefined();
}

Value InsertAdjacentText(Context& ctx, Value t, qe::Args args, Value) {
  Element* self = ThisElement(ctx, t);
  if (!self || Missing(ctx, args, 2, "insertAdjacentText")) return qe::Undefined();
  const std::string where = qe::ToWtf8(ctx, args[0]);
  const std::string data = qe::HasException(ctx) ? "" : qe::ToWtf8(ctx, args[1]);
  if (qe::HasException(ctx)) return qe::Undefined();
  Value result;
  Adjacent(ctx, self, where, NewText(ctx, self->nodeDocument, data), result);
  return qe::Undefined();
}

}  // namespace

void DefineElementClass(Context& ctx) {
  qe::ClassRef element = qe::DefineClass(ctx, "Element", IllegalConstructor, 0, NodePrototype(ctx));
  SetInterfacePrototype(ctx, Interface::Element, element.prototype);
  Object* p = element.prototype;
  qe::DefineAccessor(p, "namespaceURI", GetNamespaceUri, nullptr);
  qe::DefineAccessor(p, "prefix", GetPrefix, nullptr);
  qe::DefineAccessor(p, "localName", GetLocalName, nullptr);
  qe::DefineAccessor(p, "tagName", GetTagName, nullptr);
  qe::DefineAccessor(p, "id", GetReflected<kId>, SetReflected<kId>);
  qe::DefineAccessor(p, "className", GetReflected<kClass>, SetReflected<kClass>);
  qe::DefineAccessor(p, "slot", GetReflected<kSlot>, SetReflected<kSlot>);
  qe::DefineAccessor(p, "classList", GetClassList, SetClassList);
  qe::DefineAccessor(p, "attributes", GetAttributes, nullptr);
  qe::DefineMethod(p, "hasAttributes", HasAttributes, 0);
  qe::DefineMethod(p, "getAttributeNames", GetAttributeNames, 0);
  qe::DefineMethod(p, "getAttribute", GetAttributeMethod, 1);
  qe::DefineMethod(p, "getAttributeNS", GetAttributeNs, 2);
  qe::DefineMethod(p, "setAttribute", SetAttributeMethod, 2);
  qe::DefineMethod(p, "setAttributeNS", SetAttributeNs, 3);
  qe::DefineMethod(p, "removeAttribute", RemoveAttributeMethod, 1);
  qe::DefineMethod(p, "removeAttributeNS", RemoveAttributeNs, 2);
  qe::DefineMethod(p, "toggleAttribute", ToggleAttribute, 1);
  qe::DefineMethod(p, "hasAttribute", HasAttributeMethod, 1);
  qe::DefineMethod(p, "hasAttributeNS", HasAttributeNs, 2);
  qe::DefineMethod(p, "getAttributeNode", GetAttributeNode, 1);
  qe::DefineMethod(p, "getAttributeNodeNS", GetAttributeNodeNs, 2);
  qe::DefineMethod(p, "setAttributeNode", SetAttributeNodeMethod, 1);
  qe::DefineMethod(p, "setAttributeNodeNS", SetAttributeNodeMethod, 1);
  qe::DefineMethod(p, "removeAttributeNode", RemoveAttributeNodeMethod, 1);
  qe::DefineMethod(p, "getElementsByTagName", GetElementsByTagNameMethod, 1);
  qe::DefineMethod(p, "getElementsByTagNameNS", GetElementsByTagNameNsMethod, 2);
  qe::DefineMethod(p, "getElementsByClassName", GetElementsByClassNameMethod, 1);
  qe::DefineMethod(p, "insertAdjacentElement", InsertAdjacentElement, 2);
  qe::DefineMethod(p, "insertAdjacentText", InsertAdjacentText, 2);
  DefineParentNode(p);
  DefineChildNode(p);
  DefineNonDocumentTypeChildNode(p);
  qe::DefineGlobal(ctx, "Element", element.constructor);

  // The interface the standard gives HTML elements; the HTML specification adds its members.
  qe::ClassRef html = qe::DefineClass(ctx, "HTMLElement", IllegalConstructor, 0, p);
  SetInterfacePrototype(ctx, Interface::HtmlElement, html.prototype);
  qe::DefineGlobal(ctx, "HTMLElement", html.constructor);
}

}  // namespace solar::dom
