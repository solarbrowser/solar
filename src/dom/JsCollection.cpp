#include <string>

#include "solar/dom/NodeBindingsInternal.h"

namespace solar::dom {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Heap;
using Quanta::Object;
using Quanta::Value;

namespace {

char g_nodeListKey;
char g_htmlCollectionKey;

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 0x20);
  }
  return out;
}

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }

std::vector<std::string> SplitTokens(std::string_view text) {
  std::vector<std::string> tokens;
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && IsSpace(text[i])) ++i;
    size_t start = i;
    while (i < text.size() && !IsSpace(text[i])) ++i;
    if (i > start) tokens.emplace_back(text.substr(start, i - start));
  }
  return tokens;
}

bool HasAllClasses(const Element* element, const std::vector<std::string>& wanted, bool caseInsensitive) {
  const Attr* attribute = element->FindAttribute("", "class");
  if (!attribute) return false;
  const std::vector<std::string> have = SplitTokens(attribute->value);
  for (const std::string& want : wanted) {
    bool found = false;
    for (const std::string& token : have) {
      if (caseInsensitive ? Lower(token) == Lower(want) : token == want) found = true;
    }
    if (!found) return false;
  }
  return true;
}

}  // namespace

const std::vector<Node*>& JsCollection::Items() {
  if (kind == Kind::Static || version == TreeVersion()) return items;
  version = TreeVersion();
  items.clear();
  switch (kind) {
    case Kind::ChildNodes:
      for (Node* child = root->firstChild; child; child = child->nextSibling) items.push_back(child);
      break;
    case Kind::Children:
      for (Node* child = root->firstChild; child; child = child->nextSibling) {
        if (child->IsElement()) items.push_back(child);
      }
      break;
    case Kind::ByTagName: {
      const bool htmlDocument = root->nodeDocument && root->nodeDocument->isHtml;
      const std::string lowered = Lower(name);
      for (Node* node = root->NextInTree(root); node; node = node->NextInTree(root)) {
        Element* element = AsElement(node);
        if (!element) continue;
        if (name == "*") items.push_back(element);
        else if (htmlDocument && element->IsHtml() ? element->QualifiedName() == lowered : element->QualifiedName() == name) items.push_back(element);
      }
      break;
    }
    case Kind::ByTagNameNS:
      for (Node* node = root->NextInTree(root); node; node = node->NextInTree(root)) {
        Element* element = AsElement(node);
        if (!element) continue;
        if ((ns == "*" || element->namespaceUri == ns) && (name == "*" || element->localName == name)) items.push_back(element);
      }
      break;
    case Kind::ByClassName: {
      const std::vector<std::string> wanted = SplitTokens(name);
      if (wanted.empty()) break;
      const bool quirks = root->nodeDocument && root->nodeDocument->mode == Document::Mode::Quirks;
      for (Node* node = root->NextInTree(root); node; node = node->NextInTree(root)) {
        Element* element = AsElement(node);
        if (element && HasAllClasses(element, wanted, quirks)) items.push_back(element);
      }
      break;
    }
    case Kind::Static:
      break;
  }
  return items;
}

JsCollection* NewCollection(Context& ctx, JsCollection::Kind kind, Node* root, bool isNodeList) {
  JsCollection* collection = Heap::Allocate<JsCollection>();
  collection->kind = kind;
  collection->root = root;
  collection->isNodeList = isNodeList;
  collection->initialize_prototype(static_cast<Object*>(qe::GetRealmData(ctx, isNodeList ? &g_nodeListKey : &g_htmlCollectionKey)));
  return collection;
}

JsCollection* NewStaticNodeList(Context& ctx, std::vector<Node*> nodes) {
  JsCollection* collection = NewCollection(ctx, JsCollection::Kind::Static, nullptr, true);
  collection->items = std::move(nodes);
  collection->NoteWrite();
  return collection;
}

Value GetElementsByTagName(Context& ctx, Node* root, std::string_view name) {
  JsCollection* collection = NewCollection(ctx, JsCollection::Kind::ByTagName, root, false);
  collection->name = name;
  return qe::FromObject(collection);
}

Value GetElementsByTagNameNS(Context& ctx, Node* root, std::string_view ns, std::string_view local) {
  JsCollection* collection = NewCollection(ctx, JsCollection::Kind::ByTagNameNS, root, false);
  collection->ns = ns;
  collection->name = local;
  return qe::FromObject(collection);
}

Value GetElementsByClassName(Context& ctx, Node* root, std::string_view classes) {
  JsCollection* collection = NewCollection(ctx, JsCollection::Kind::ByClassName, root, false);
  collection->name = classes;
  return qe::FromObject(collection);
}

namespace {

JsCollection* This(Context& ctx, const Value& t) {
  JsCollection* collection = DOMObject::Cast<JsCollection>(t);
  if (!collection) qe::ThrowTypeError(ctx, "Illegal invocation");
  return collection;
}

Value IllegalConstructor(Context& ctx, Value, qe::Args, Value) {
  qe::ThrowTypeError(ctx, "Illegal constructor");
  return qe::Undefined();
}

Value GetLength(Context& ctx, Value t, qe::Args, Value) {
  JsCollection* self = This(ctx, t);
  return self ? qe::FromUint32(static_cast<uint32_t>(self->Items().size())) : qe::Undefined();
}

Value Item(Context& ctx, Value t, qe::Args args, Value) {
  JsCollection* self = This(ctx, t);
  if (!self) return qe::Undefined();
  if (args.empty()) {
    qe::ThrowTypeError(ctx, "Failed to execute 'item': 1 argument required, but only 0 present.");
    return qe::Undefined();
  }
  const uint32_t index = qe::ToUint32(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  const std::vector<Node*>& items = self->Items();
  return index < items.size() ? qe::FromObject(items[index]) : qe::Null();
}

Value NamedItem(Context& ctx, Value t, qe::Args args, Value) {
  JsCollection* self = This(ctx, t);
  if (!self) return qe::Undefined();
  if (args.empty()) {
    qe::ThrowTypeError(ctx, "Failed to execute 'namedItem': 1 argument required, but only 0 present.");
    return qe::Undefined();
  }
  const std::string name = qe::ToUsvUtf8(ctx, args[0]);
  if (qe::HasException(ctx) || name.empty()) return qe::Null();
  for (Node* node : self->Items()) {
    Element* element = AsElement(node);
    if (!element) continue;
    const Attr* id = element->FindAttribute("", "id");
    if (id && id->value == name) return qe::FromObject(element);
    const Attr* nameAttribute = element->FindAttribute("", "name");
    if (element->IsHtml() && nameAttribute && nameAttribute->value == name) return qe::FromObject(element);
  }
  return qe::Null();
}

}  // namespace

void DefineCollectionClasses(Context& ctx) {
  qe::ClassRef list = qe::DefineClass(ctx, "NodeList", IllegalConstructor, 0);
  qe::SetRealmData(ctx, &g_nodeListKey, list.prototype);
  qe::DefineAccessor(list.prototype, "length", GetLength, nullptr);
  qe::DefineMethod(list.prototype, "item", Item, 1);
  qe::DefineGlobal(ctx, "NodeList", list.constructor);

  qe::ClassRef collection = qe::DefineClass(ctx, "HTMLCollection", IllegalConstructor, 0);
  qe::SetRealmData(ctx, &g_htmlCollectionKey, collection.prototype);
  qe::DefineAccessor(collection.prototype, "length", GetLength, nullptr);
  qe::DefineMethod(collection.prototype, "item", Item, 1);
  qe::DefineMethod(collection.prototype, "namedItem", NamedItem, 1);
  qe::DefineGlobal(ctx, "HTMLCollection", collection.constructor);
}

}  // namespace solar::dom
