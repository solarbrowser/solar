#include <algorithm>
#include <string>

#include "solar/dom/CustomElements.h"
#include "solar/dom/NodeBindingsInternal.h"

namespace solar::dom {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Heap;
using Quanta::Object;
using Quanta::Value;

namespace {

char g_namedNodeMapKey;
char g_tokenListKey;

Value IllegalConstructor(Context& ctx, Value, qe::Args, Value) {
  qe::ThrowTypeError(ctx, "Illegal constructor");
  return qe::Undefined();
}

bool Missing(Context& ctx, qe::Args args, size_t count, const char* interface, const char* member) {
  if (args.size() >= count) return false;
  qe::ThrowTypeError(ctx, std::string("Failed to execute '") + member + "' on '" + interface + "': " + std::to_string(count) + " argument" + (count == 1 ? "" : "s") +
                              " required, but only " + std::to_string(args.size()) + " present.");
  return true;
}

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }

bool HasUpper(const std::string& text) {
  return std::any_of(text.begin(), text.end(), [](char c) { return c >= 'A' && c <= 'Z'; });
}

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 0x20);
  }
  return out;
}

}  // namespace

// ---- NamedNodeMap ----

struct JsNamedNodeMap : DOMObject {
  Element* element = nullptr;

  static bool IndexedGetter(Context&, JsNamedNodeMap& self, uint32_t index, Value& out) {
    if (index >= self.element->attributes.size()) return false;
    out = qe::FromObject(self.element->attributes[index]);
    return true;
  }
  static uint32_t IndexedLength(Context&, JsNamedNodeMap& self) { return static_cast<uint32_t>(self.element->attributes.size()); }

  // A name is supported if it names an attribute, and on an HTML element in an HTML document only if it has no upper case.
  static bool Lowers(const Element* element) { return element->IsHtml() && element->nodeDocument && element->nodeDocument->isHtml; }
  static bool NamedGetter(Context&, JsNamedNodeMap& self, const std::string& name, Value& out) {
    if (Lowers(self.element) && HasUpper(name)) return false;
    Attr* attribute = self.element->FindAttribute(name);
    if (!attribute) return false;
    out = qe::FromObject(attribute);
    return true;
  }
  static std::vector<std::string> NamedKeys(Context&, JsNamedNodeMap& self) {
    std::vector<std::string> names;
    for (const Attr* attribute : self.element->attributes) {
      const std::string name = attribute->QualifiedName();
      if (Lowers(self.element) && HasUpper(name)) continue;
      if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    }
    return names;
  }
  static constexpr bool LegacyUnenumerableNamedProperties = true;

  void Visit(Quanta::Visitor& visitor) { visitor.Mark(element); }
};

Object* NewNamedNodeMap(Context& ctx, Element* element) {
  JsNamedNodeMap* map = Heap::Allocate<JsNamedNodeMap>();
  map->element = element;
  map->initialize_prototype(static_cast<Object*>(qe::GetRealmData(ctx, &g_namedNodeMapKey)));
  return map;
}

namespace {

JsNamedNodeMap* ThisMap(Context& ctx, const Value& t) {
  JsNamedNodeMap* map = DOMObject::Cast<JsNamedNodeMap>(t);
  if (!map) qe::ThrowTypeError(ctx, "Illegal invocation");
  return map;
}

Value MapLength(Context& ctx, Value t, qe::Args, Value) {
  JsNamedNodeMap* self = ThisMap(ctx, t);
  return self ? qe::FromUint32(static_cast<uint32_t>(self->element->attributes.size())) : qe::Undefined();
}

Value MapItem(Context& ctx, Value t, qe::Args args, Value) {
  JsNamedNodeMap* self = ThisMap(ctx, t);
  if (!self || Missing(ctx, args, 1, "NamedNodeMap", "item")) return qe::Undefined();
  const uint32_t index = qe::ToUint32(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  return index < self->element->attributes.size() ? qe::FromObject(self->element->attributes[index]) : qe::Null();
}

Value GetNamedItem(Context& ctx, Value t, qe::Args args, Value) {
  JsNamedNodeMap* self = ThisMap(ctx, t);
  if (!self || Missing(ctx, args, 1, "NamedNodeMap", "getNamedItem")) return qe::Undefined();
  std::string name = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  if (JsNamedNodeMap::Lowers(self->element)) name = Lower(name);
  return NodeValue(self->element->FindAttribute(name));
}

Value GetNamedItemNs(Context& ctx, Value t, qe::Args args, Value) {
  JsNamedNodeMap* self = ThisMap(ctx, t);
  if (!self || Missing(ctx, args, 2, "NamedNodeMap", "getNamedItemNS")) return qe::Undefined();
  const std::optional<std::string> ns = NullableString(ctx, args[0]);
  const std::string local = qe::HasException(ctx) ? "" : qe::ToWtf8(ctx, args[1]);
  if (qe::HasException(ctx)) return qe::Undefined();
  return NodeValue(self->element->FindAttribute(ns.value_or(""), local));
}

Value SetNamedItem(Context& ctx, Value t, qe::Args args, Value) {
  JsNamedNodeMap* self = ThisMap(ctx, t);
  if (!self || Missing(ctx, args, 1, "NamedNodeMap", "setNamedItem")) return qe::Undefined();
  Node* node = DOMObject::Cast<Node>(args[0]);
  if (!node || node->nodeType != NodeType::Attribute) {
    qe::ThrowTypeError(ctx, "Failed to execute 'setNamedItem' on 'NamedNodeMap': parameter 1 is not of type 'Attr'.");
    return qe::Undefined();
  }
  Attr* replaced = nullptr;
  if (auto error = SetAttributeNode(self->element, static_cast<Attr*>(node), replaced)) {
    Throw(ctx, *error);
    return qe::Undefined();
  }
  return NodeValue(replaced);
}

Value RemoveNamedItem(Context& ctx, Value t, qe::Args args, Value) {
  JsNamedNodeMap* self = ThisMap(ctx, t);
  if (!self || Missing(ctx, args, 1, "NamedNodeMap", "removeNamedItem")) return qe::Undefined();
  std::string name = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  if (JsNamedNodeMap::Lowers(self->element)) name = Lower(name);
  Attr* attribute = self->element->FindAttribute(name);
  if (!attribute) {
    Throw(ctx, {"NotFoundError", "The attribute is not an attribute of this element"});
    return qe::Undefined();
  }
  RemoveAttributeNode(self->element, attribute);
  return qe::FromObject(attribute);
}

Value RemoveNamedItemNs(Context& ctx, Value t, qe::Args args, Value) {
  JsNamedNodeMap* self = ThisMap(ctx, t);
  if (!self || Missing(ctx, args, 2, "NamedNodeMap", "removeNamedItemNS")) return qe::Undefined();
  const std::optional<std::string> ns = NullableString(ctx, args[0]);
  const std::string local = qe::HasException(ctx) ? "" : qe::ToWtf8(ctx, args[1]);
  if (qe::HasException(ctx)) return qe::Undefined();
  Attr* attribute = self->element->FindAttribute(ns.value_or(""), local);
  if (!attribute) {
    Throw(ctx, {"NotFoundError", "The attribute is not an attribute of this element"});
    return qe::Undefined();
  }
  RemoveAttributeNode(self->element, attribute);
  return qe::FromObject(attribute);
}

}  // namespace

// ---- DOMTokenList ----

struct JsTokenList : DOMObject {
  Element* element = nullptr;
  std::string attribute;

  // The ordered set of tokens of the attribute's value.
  std::vector<std::string> Tokens() const {
    std::vector<std::string> tokens;
    const Attr* value = element->FindAttribute("", attribute);
    if (!value) return tokens;
    const std::string& text = value->value;
    for (size_t i = 0; i < text.size();) {
      while (i < text.size() && IsSpace(text[i])) ++i;
      const size_t start = i;
      while (i < text.size() && !IsSpace(text[i])) ++i;
      if (i > start) {
        std::string token = text.substr(start, i - start);
        if (std::find(tokens.begin(), tokens.end(), token) == tokens.end()) tokens.push_back(std::move(token));
      }
    }
    return tokens;
  }

  static bool IndexedGetter(Context& ctx, JsTokenList& self, uint32_t index, Value& out) {
    const std::vector<std::string> tokens = self.Tokens();
    if (index >= tokens.size()) return false;
    out = qe::FromWtf8(ctx, tokens[index]);
    return true;
  }
  static uint32_t IndexedLength(Context&, JsTokenList& self) { return static_cast<uint32_t>(self.Tokens().size()); }

  void Visit(Quanta::Visitor& visitor) { visitor.Mark(element); }
};

Object* NewTokenList(Context& ctx, Element* element, const char* attribute) {
  JsTokenList* list = Heap::Allocate<JsTokenList>();
  list->element = element;
  list->attribute = attribute;
  list->initialize_prototype(static_cast<Object*>(qe::GetRealmData(ctx, &g_tokenListKey)));
  return list;
}

namespace {

JsTokenList* ThisList(Context& ctx, const Value& t) {
  JsTokenList* list = DOMObject::Cast<JsTokenList>(t);
  if (!list) qe::ThrowTypeError(ctx, "Illegal invocation");
  return list;
}

// The "update steps": the attribute is written from the set, unless it is empty and has no attribute.
void Update(Context& ctx, JsTokenList* list, const std::vector<std::string>& tokens) {
  if (tokens.empty() && !list->element->FindAttribute("", list->attribute)) return;
  std::string text;
  for (const std::string& token : tokens) text += (text.empty() ? "" : " ") + token;
  SetAttribute(ctx, list->element, list->attribute, std::move(text));
}

// A token has to be something: SyntaxError for the empty one, InvalidCharacterError for one with space.
bool ValidateToken(Context& ctx, const std::string& token) {
  if (token.empty()) {
    Throw(ctx, {"SyntaxError", "The token provided must not be empty."});
    return false;
  }
  if (std::any_of(token.begin(), token.end(), IsSpace)) {
    Throw(ctx, {"InvalidCharacterError", "The token provided ('" + token + "') contains HTML space characters, which are not valid in tokens."});
    return false;
  }
  return true;
}

// Converts the arguments to strings and validates them all before any is used.
bool ReadTokens(Context& ctx, qe::Args args, std::vector<std::string>& out) {
  for (const Value& arg : args) {
    out.push_back(qe::ToWtf8(ctx, arg));
    if (qe::HasException(ctx)) return false;
  }
  for (const std::string& token : out) {
    if (!ValidateToken(ctx, token)) return false;
  }
  return true;
}

Value ListLength(Context& ctx, Value t, qe::Args, Value) {
  JsTokenList* self = ThisList(ctx, t);
  return self ? qe::FromUint32(static_cast<uint32_t>(self->Tokens().size())) : qe::Undefined();
}

Value ListItem(Context& ctx, Value t, qe::Args args, Value) {
  JsTokenList* self = ThisList(ctx, t);
  if (!self || Missing(ctx, args, 1, "DOMTokenList", "item")) return qe::Undefined();
  const uint32_t index = qe::ToUint32(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  const std::vector<std::string> tokens = self->Tokens();
  return index < tokens.size() ? qe::FromWtf8(ctx, tokens[index]) : qe::Null();
}

Value ListContains(Context& ctx, Value t, qe::Args args, Value) {
  JsTokenList* self = ThisList(ctx, t);
  if (!self || Missing(ctx, args, 1, "DOMTokenList", "contains")) return qe::Undefined();
  const std::string token = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  const std::vector<std::string> tokens = self->Tokens();
  return qe::FromBool(std::find(tokens.begin(), tokens.end(), token) != tokens.end());
}

Value ListAdd(Context& ctx, Value t, qe::Args args, Value) {
  JsTokenList* self = ThisList(ctx, t);
  if (!self) return qe::Undefined();
  std::vector<std::string> add;
  if (!ReadTokens(ctx, args, add)) return qe::Undefined();
  std::vector<std::string> tokens = self->Tokens();
  for (std::string& token : add) {
    if (std::find(tokens.begin(), tokens.end(), token) == tokens.end()) tokens.push_back(std::move(token));
  }
  Update(ctx, self, tokens);
  return qe::Undefined();
}

Value ListRemove(Context& ctx, Value t, qe::Args args, Value) {
  JsTokenList* self = ThisList(ctx, t);
  if (!self) return qe::Undefined();
  std::vector<std::string> remove;
  if (!ReadTokens(ctx, args, remove)) return qe::Undefined();
  std::vector<std::string> tokens = self->Tokens();
  std::erase_if(tokens, [&](const std::string& token) { return std::find(remove.begin(), remove.end(), token) != remove.end(); });
  Update(ctx, self, tokens);
  return qe::Undefined();
}

Value ListToggle(Context& ctx, Value t, qe::Args args, Value) {
  JsTokenList* self = ThisList(ctx, t);
  if (!self || Missing(ctx, args, 1, "DOMTokenList", "toggle")) return qe::Undefined();
  const std::string token = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx) || !ValidateToken(ctx, token)) return qe::Undefined();
  const bool hasForce = args.size() > 1 && !qe::IsUndefined(args[1]);
  const bool force = hasForce && args[1].to_boolean();
  std::vector<std::string> tokens = self->Tokens();
  const auto found = std::find(tokens.begin(), tokens.end(), token);
  if (found != tokens.end()) {
    if (!hasForce || !force) {
      tokens.erase(found);
      Update(ctx, self, tokens);
      return qe::FromBool(false);
    }
    return qe::FromBool(true);
  }
  if (!hasForce || force) {
    tokens.push_back(token);
    Update(ctx, self, tokens);
    return qe::FromBool(true);
  }
  return qe::FromBool(false);
}

Value ListReplace(Context& ctx, Value t, qe::Args args, Value) {
  JsTokenList* self = ThisList(ctx, t);
  if (!self || Missing(ctx, args, 2, "DOMTokenList", "replace")) return qe::Undefined();
  const std::string token = qe::ToWtf8(ctx, args[0]);
  const std::string replacement = qe::HasException(ctx) ? "" : qe::ToWtf8(ctx, args[1]);
  if (qe::HasException(ctx) || !ValidateToken(ctx, token) || !ValidateToken(ctx, replacement)) return qe::Undefined();
  std::vector<std::string> tokens = self->Tokens();
  if (std::find(tokens.begin(), tokens.end(), token) == tokens.end()) return qe::FromBool(false);
  // In the place of the first of the two that is there; the other goes.
  std::vector<std::string> result;
  bool placed = false;
  for (const std::string& existing : tokens) {
    if (existing == token || existing == replacement) {
      if (!placed) result.push_back(replacement);
      placed = true;
    } else {
      result.push_back(existing);
    }
  }
  Update(ctx, self, result);
  return qe::FromBool(true);
}

Value ListSupports(Context& ctx, Value t, qe::Args args, Value) {
  JsTokenList* self = ThisList(ctx, t);
  if (!self || Missing(ctx, args, 1, "DOMTokenList", "supports")) return qe::Undefined();
  // An attribute with no defined supported tokens (class, say) gives a TypeError.
  qe::ThrowTypeError(ctx, "Failed to execute 'supports' on 'DOMTokenList': The token list has no supported tokens defined.");
  return qe::Undefined();
}

Value GetListValue(Context& ctx, Value t, qe::Args, Value) {
  JsTokenList* self = ThisList(ctx, t);
  if (!self) return qe::Undefined();
  const Attr* attribute = self->element->FindAttribute("", self->attribute);
  return qe::FromWtf8(ctx, attribute ? attribute->value : "");
}

Value SetListValue(Context& ctx, Value t, qe::Args args, Value) {
  JsTokenList* self = ThisList(ctx, t);
  if (!self || Missing(ctx, args, 1, "DOMTokenList", "value")) return qe::Undefined();
  std::string text = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  SetAttribute(ctx, self->element, self->attribute, std::move(text));
  return qe::Undefined();
}

}  // namespace

void DefineAttributeClasses(Context& ctx) {
  qe::ClassRef map = qe::DefineClass(ctx, "NamedNodeMap", IllegalConstructor, 0);
  qe::SetRealmData(ctx, &g_namedNodeMapKey, map.prototype);
  qe::DefineAccessor(map.prototype, "length", MapLength, nullptr);
  qe::DefineMethod(map.prototype, "item", MapItem, 1);
  qe::DefineMethod(map.prototype, "getNamedItem", GetNamedItem, 1);
  qe::DefineMethod(map.prototype, "getNamedItemNS", GetNamedItemNs, 2);
  qe::DefineMethod(map.prototype, "setNamedItem", Reactions<SetNamedItem>, 1);
  qe::DefineMethod(map.prototype, "setNamedItemNS", Reactions<SetNamedItem>, 1);
  qe::DefineMethod(map.prototype, "removeNamedItem", Reactions<RemoveNamedItem>, 1);
  qe::DefineMethod(map.prototype, "removeNamedItemNS", Reactions<RemoveNamedItemNs>, 2);
  qe::DefineGlobal(ctx, "NamedNodeMap", map.constructor);

  qe::ClassRef list = qe::DefineClass(ctx, "DOMTokenList", IllegalConstructor, 0);
  qe::SetRealmData(ctx, &g_tokenListKey, list.prototype);
  qe::DefineAccessor(list.prototype, "length", ListLength, nullptr);
  qe::DefineMethod(list.prototype, "item", ListItem, 1);
  qe::DefineMethod(list.prototype, "contains", ListContains, 1);
  qe::DefineMethod(list.prototype, "add", Reactions<ListAdd>, 0);
  qe::DefineMethod(list.prototype, "remove", Reactions<ListRemove>, 0);
  qe::DefineMethod(list.prototype, "toggle", Reactions<ListToggle>, 1);
  qe::DefineMethod(list.prototype, "replace", Reactions<ListReplace>, 2);
  qe::DefineMethod(list.prototype, "supports", ListSupports, 1);
  qe::DefineAccessor(list.prototype, "value", GetListValue, Reactions<SetListValue>);
  qe::DefineGlobal(ctx, "DOMTokenList", list.constructor);
}

}  // namespace solar::dom
