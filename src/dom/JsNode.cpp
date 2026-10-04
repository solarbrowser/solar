#include <algorithm>
#include <string>

#include "solar/dom/NodeBindingsInternal.h"

namespace solar::dom {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Object;
using Quanta::Value;

// ---- Shared helpers ----

namespace {
char g_nodePrototypeKey;
}

Object* NodePrototype(Context& ctx) { return static_cast<Object*>(qe::GetRealmData(ctx, &g_nodePrototypeKey)); }

Node* ThisNode(Context& ctx, const Value& thisValue) {
  Node* node = DOMObject::Cast<Node>(thisValue);
  if (!node) qe::ThrowTypeError(ctx, "Illegal invocation");
  return node;
}

Element* ThisElement(Context& ctx, const Value& thisValue) {
  Node* node = ThisNode(ctx, thisValue);
  if (!node) return nullptr;
  if (!node->IsElement()) {
    qe::ThrowTypeError(ctx, "Illegal invocation");
    return nullptr;
  }
  return static_cast<Element*>(node);
}

Document* ThisDocument(Context& ctx, const Value& thisValue) {
  Node* node = ThisNode(ctx, thisValue);
  if (!node) return nullptr;
  if (!node->IsDocument()) {
    qe::ThrowTypeError(ctx, "Illegal invocation");
    return nullptr;
  }
  return static_cast<Document*>(node);
}

CharacterData* ThisCharacterData(Context& ctx, const Value& thisValue) {
  Node* node = ThisNode(ctx, thisValue);
  if (!node) return nullptr;
  if (!node->IsCharacterData()) {
    qe::ThrowTypeError(ctx, "Illegal invocation");
    return nullptr;
  }
  return static_cast<CharacterData*>(node);
}

void Throw(Context& ctx, const DomError& error) { web::ThrowDomException(ctx, error.message, error.name); }

Value NodeValue(Node* node) { return node ? qe::FromObject(node) : qe::Null(); }

Node* NodeArgument(Context& ctx, qe::Args args, size_t index, const char* what) {
  Node* node = index < args.size() ? DOMObject::Cast<Node>(args[index]) : nullptr;
  if (!node) qe::ThrowTypeError(ctx, std::string(what) + ": parameter " + std::to_string(index + 1) + " is not of type 'Node'.");
  return node;
}

std::optional<std::string> NullableString(Context& ctx, const Value& value) {
  if (qe::IsNull(value) || qe::IsUndefined(value)) return std::nullopt;
  return qe::ToWtf8(ctx, value);
}

namespace {

Value StringValue(Context& ctx, const std::optional<std::string>& text) { return text ? qe::FromWtf8(ctx, *text) : qe::Null(); }

bool Missing(Context& ctx, qe::Args args, size_t count, const char* interface, const char* member) {
  if (args.size() >= count) return false;
  qe::ThrowTypeError(ctx, std::string("Failed to execute '") + member + "' on '" + interface + "': " + std::to_string(count) + " argument" + (count == 1 ? "" : "s") +
                              " required, but only " + std::to_string(args.size()) + " present.");
  return true;
}

}  // namespace

std::string TagNameOf(const Element* element) {
  std::string name = element->QualifiedName();
  if (element->IsHtml() && element->nodeDocument && element->nodeDocument->isHtml) {
    for (char& c : name) {
      if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 0x20);
    }
  }
  return name;
}

Node* ConvertNodesIntoNode(Context& ctx, qe::Args args, Document* document) {
  // Held where the collector can see them: the Text nodes made here are reachable from nothing else.
  qe::ValueList nodes;
  for (const Value& arg : args) {
    if (Node* node = DOMObject::Cast<Node>(arg)) {
      nodes.Append(qe::FromObject(node));
    } else {
      std::string text = qe::ToWtf8(ctx, arg);
      if (qe::HasException(ctx)) return nullptr;
      nodes.Append(qe::FromObject(NewText(ctx, document, std::move(text))));
    }
  }
  if (nodes.size() == 1) return DOMObject::Cast<Node>(nodes[0]);
  DocumentFragment* fragment = NewDocumentFragment(ctx, document);
  for (const Value& value : nodes) {
    if (auto error = AppendChild(fragment, DOMObject::Cast<Node>(value))) {
      Throw(ctx, *error);
      return nullptr;
    }
  }
  return fragment;
}

namespace {

Document* DocumentFor(Node* node) { return node->IsDocument() ? static_cast<Document*>(node) : node->nodeDocument; }

// ---- Namespace lookup ----

std::optional<std::string> LocateNamespace(const Node* node, const std::optional<std::string>& prefix) {
  switch (node->nodeType) {
    case NodeType::Element: {
      const Element* element = static_cast<const Element*>(node);
      if (prefix == "xml") return std::string(kXmlNamespace);
      if (prefix == "xmlns") return std::string(kXmlnsNamespace);
      if (!element->namespaceUri.empty() && (prefix ? *prefix == element->prefix && !element->prefix.empty() : element->prefix.empty())) return element->namespaceUri;
      for (const Attr* attribute : element->attributes) {
        if (attribute->namespaceUri != kXmlnsNamespace) continue;
        if ((attribute->prefix == "xmlns" && prefix && attribute->localName == *prefix) || (!prefix && attribute->prefix.empty() && attribute->localName == "xmlns")) {
          if (attribute->value.empty()) return std::nullopt;
          return attribute->value;
        }
      }
      const Node* parent = element->parentNode;
      if (!parent || !parent->IsElement()) return std::nullopt;
      return LocateNamespace(parent, prefix);
    }
    case NodeType::Document: {
      const Element* root = static_cast<const Document*>(node)->DocumentElement();
      return root ? LocateNamespace(root, prefix) : std::nullopt;
    }
    case NodeType::DocumentType:
    case NodeType::DocumentFragment:
      return std::nullopt;
    case NodeType::Attribute: {
      const Element* owner = static_cast<const Attr*>(node)->ownerElement;
      return owner ? LocateNamespace(owner, prefix) : std::nullopt;
    }
    default: {
      const Node* parent = node->parentNode;
      if (!parent || !parent->IsElement()) return std::nullopt;
      return LocateNamespace(parent, prefix);
    }
  }
}

std::optional<std::string> LocatePrefix(const Element* element, const std::string& ns) {
  if (element->namespaceUri == ns && !element->prefix.empty()) return element->prefix;
  for (const Attr* attribute : element->attributes) {
    if (attribute->prefix == "xmlns" && attribute->namespaceUri == kXmlnsNamespace && attribute->value == ns) return attribute->localName;
  }
  const Node* parent = element->parentNode;
  if (parent && parent->IsElement()) return LocatePrefix(static_cast<const Element*>(parent), ns);
  return std::nullopt;
}

// ---- Node ----

Value IllegalConstructor(Context& ctx, Value, qe::Args, Value) {
  qe::ThrowTypeError(ctx, "Illegal constructor");
  return qe::Undefined();
}

Value GetNodeType(Context& ctx, Value t, qe::Args, Value) {
  Node* self = ThisNode(ctx, t);
  return self ? qe::FromUint32(static_cast<uint32_t>(self->nodeType)) : qe::Undefined();
}

std::string NodeNameOf(const Node* node) {
  switch (node->nodeType) {
    case NodeType::Element: return TagNameOf(static_cast<const Element*>(node));
    case NodeType::Attribute: return static_cast<const Attr*>(node)->QualifiedName();
    case NodeType::Text: return "#text";
    case NodeType::CdataSection: return "#cdata-section";
    case NodeType::ProcessingInstruction: return static_cast<const CharacterData*>(node)->target;
    case NodeType::Comment: return "#comment";
    case NodeType::Document: return "#document";
    case NodeType::DocumentType: return static_cast<const DocumentType*>(node)->name;
    case NodeType::DocumentFragment: return "#document-fragment";
  }
  return "";
}

Value GetNodeName(Context& ctx, Value t, qe::Args, Value) {
  Node* self = ThisNode(ctx, t);
  return self ? qe::FromWtf8(ctx, NodeNameOf(self)) : qe::Undefined();
}

Value GetBaseUri(Context& ctx, Value t, qe::Args, Value) {
  Node* self = ThisNode(ctx, t);
  if (!self) return qe::Undefined();
  Document* document = DocumentFor(self);
  return qe::FromWtf8(ctx, document ? document->url : "about:blank");
}

Value GetIsConnected(Context& ctx, Value t, qe::Args, Value) {
  Node* self = ThisNode(ctx, t);
  return self ? qe::FromBool(self->Root()->IsDocument()) : qe::Undefined();
}

Value GetOwnerDocument(Context& ctx, Value t, qe::Args, Value) {
  Node* self = ThisNode(ctx, t);
  if (!self) return qe::Undefined();
  return self->IsDocument() ? qe::Null() : NodeValue(self->nodeDocument);
}

Value GetParentNode(Context& ctx, Value t, qe::Args, Value) {
  Node* self = ThisNode(ctx, t);
  return self ? NodeValue(self->parentNode) : qe::Undefined();
}

Value GetParentElement(Context& ctx, Value t, qe::Args, Value) {
  Node* self = ThisNode(ctx, t);
  if (!self) return qe::Undefined();
  return self->parentNode && self->parentNode->IsElement() ? qe::FromObject(self->parentNode) : qe::Null();
}

Value HasChildNodes(Context& ctx, Value t, qe::Args, Value) {
  Node* self = ThisNode(ctx, t);
  return self ? qe::FromBool(self->HasChildNodes()) : qe::Undefined();
}

Value GetChildNodes(Context& ctx, Value t, qe::Args, Value) {
  Node* self = ThisNode(ctx, t);
  if (!self) return qe::Undefined();
  if (!self->childNodesList) {
    self->childNodesList = NewCollection(ctx, JsCollection::Kind::ChildNodes, self, true);
    self->NoteWrite(qe::FromObject(self->childNodesList));
  }
  return qe::FromObject(self->childNodesList);
}

Value GetFirstChild(Context& ctx, Value t, qe::Args, Value) {
  Node* self = ThisNode(ctx, t);
  return self ? NodeValue(self->firstChild) : qe::Undefined();
}

Value GetLastChild(Context& ctx, Value t, qe::Args, Value) {
  Node* self = ThisNode(ctx, t);
  return self ? NodeValue(self->lastChild) : qe::Undefined();
}

Value GetPreviousSibling(Context& ctx, Value t, qe::Args, Value) {
  Node* self = ThisNode(ctx, t);
  return self ? NodeValue(self->previousSibling) : qe::Undefined();
}

Value GetNextSibling(Context& ctx, Value t, qe::Args, Value) {
  Node* self = ThisNode(ctx, t);
  return self ? NodeValue(self->nextSibling) : qe::Undefined();
}

// replaceData on a character data node, as nodeValue and textContent set it.
void SetData(CharacterData* node, std::string data) { node->data = std::move(data); }

Value GetNodeValue(Context& ctx, Value t, qe::Args, Value) {
  Node* self = ThisNode(ctx, t);
  if (!self) return qe::Undefined();
  if (self->nodeType == NodeType::Attribute) return qe::FromWtf8(ctx, static_cast<Attr*>(self)->value);
  if (self->IsCharacterData()) return qe::FromWtf8(ctx, static_cast<CharacterData*>(self)->data);
  return qe::Null();
}

Value SetNodeValue(Context& ctx, Value t, qe::Args args, Value) {
  Node* self = ThisNode(ctx, t);
  if (!self) return qe::Undefined();
  std::string text;
  if (!args.empty() && !qe::IsNull(args[0])) {
    text = qe::ToWtf8(ctx, args[0]);
    if (qe::HasException(ctx)) return qe::Undefined();
  }
  if (self->nodeType == NodeType::Attribute) {
    static_cast<Attr*>(self)->value = std::move(text);
    NoteTreeChange();
  } else if (self->IsCharacterData()) {
    SetData(static_cast<CharacterData*>(self), std::move(text));
  }
  return qe::Undefined();
}

Value GetTextContent(Context& ctx, Value t, qe::Args, Value) {
  Node* self = ThisNode(ctx, t);
  if (!self) return qe::Undefined();
  switch (self->nodeType) {
    case NodeType::Document:
    case NodeType::DocumentType:
      return qe::Null();
    case NodeType::DocumentFragment:
    case NodeType::Element:
      return qe::FromWtf8(ctx, self->DescendantText());
    case NodeType::Attribute:
      return qe::FromWtf8(ctx, static_cast<Attr*>(self)->value);
    default:
      return qe::FromWtf8(ctx, static_cast<CharacterData*>(self)->data);
  }
}

Value SetTextContentValue(Context& ctx, Value t, qe::Args args, Value) {
  Node* self = ThisNode(ctx, t);
  if (!self) return qe::Undefined();
  std::string text;
  if (!args.empty() && !qe::IsNull(args[0])) {
    text = qe::ToWtf8(ctx, args[0]);
    if (qe::HasException(ctx)) return qe::Undefined();
  }
  switch (self->nodeType) {
    case NodeType::DocumentFragment:
    case NodeType::Element:
      SetTextContent(ctx, self, std::move(text));
      break;
    case NodeType::Attribute:
      static_cast<Attr*>(self)->value = std::move(text);
      NoteTreeChange();
      break;
    case NodeType::Document:
    case NodeType::DocumentType:
      break;
    default:
      SetData(static_cast<CharacterData*>(self), std::move(text));
      break;
  }
  return qe::Undefined();
}

// Joins the runs of Text nodes under `node` and drops the empty ones.
void Normalize(Node* node) {
  Node* child = node->firstChild;
  while (child) {
    Node* next = child->nextSibling;
    if (child->nodeType == NodeType::Text) {
      CharacterData* text = static_cast<CharacterData*>(child);
      if (text->data.empty()) {
        RemoveUnchecked(text);
      } else {
        while (next && next->nodeType == NodeType::Text) {
          text->data += static_cast<CharacterData*>(next)->data;
          Node* after = next->nextSibling;
          RemoveUnchecked(next);
          next = after;
        }
      }
    } else {
      Normalize(child);
    }
    child = next;
  }
}

Value NormalizeMethod(Context& ctx, Value t, qe::Args, Value) {
  Node* self = ThisNode(ctx, t);
  if (self) Normalize(self);
  return qe::Undefined();
}

Value CloneNodeMethod(Context& ctx, Value t, qe::Args args, Value) {
  Node* self = ThisNode(ctx, t);
  if (!self) return qe::Undefined();
  return qe::FromObject(CloneNode(ctx, self, !args.empty() && args[0].to_boolean()));
}

Value IsEqualNodeMethod(Context& ctx, Value t, qe::Args args, Value) {
  Node* self = ThisNode(ctx, t);
  if (!self) return qe::Undefined();
  if (args.empty() || qe::IsNull(args[0]) || qe::IsUndefined(args[0])) return qe::FromBool(false);
  Node* other = NodeArgument(ctx, args, 0, "Failed to execute 'isEqualNode' on 'Node'");
  return other ? qe::FromBool(IsEqualNode(self, other)) : qe::Undefined();
}

Value IsSameNodeMethod(Context& ctx, Value t, qe::Args args, Value) {
  Node* self = ThisNode(ctx, t);
  if (!self) return qe::Undefined();
  Node* other = !args.empty() ? DOMObject::Cast<Node>(args[0]) : nullptr;
  return qe::FromBool(other == self);
}

Value CompareDocumentPositionMethod(Context& ctx, Value t, qe::Args args, Value) {
  Node* self = ThisNode(ctx, t);
  if (!self || Missing(ctx, args, 1, "Node", "compareDocumentPosition")) return qe::Undefined();
  Node* other = NodeArgument(ctx, args, 0, "Failed to execute 'compareDocumentPosition' on 'Node'");
  return other ? qe::FromUint32(CompareDocumentPosition(self, other)) : qe::Undefined();
}

Value ContainsMethod(Context& ctx, Value t, qe::Args args, Value) {
  Node* self = ThisNode(ctx, t);
  if (!self || Missing(ctx, args, 1, "Node", "contains")) return qe::Undefined();
  if (qe::IsNull(args[0])) return qe::FromBool(false);
  Node* other = NodeArgument(ctx, args, 0, "Failed to execute 'contains' on 'Node'");
  return other ? qe::FromBool(self->Contains(other)) : qe::Undefined();
}

Value GetRootNodeMethod(Context& ctx, Value t, qe::Args, Value) {
  Node* self = ThisNode(ctx, t);
  return self ? qe::FromObject(self->Root()) : qe::Undefined();
}

Value LookupPrefix(Context& ctx, Value t, qe::Args args, Value) {
  Node* self = ThisNode(ctx, t);
  if (!self || Missing(ctx, args, 1, "Node", "lookupPrefix")) return qe::Undefined();
  std::optional<std::string> ns = NullableString(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  if (!ns || ns->empty()) return qe::Null();
  const Element* element = nullptr;
  switch (self->nodeType) {
    case NodeType::Element: element = static_cast<Element*>(self); break;
    case NodeType::Document: element = static_cast<Document*>(self)->DocumentElement(); break;
    case NodeType::DocumentType:
    case NodeType::DocumentFragment: return qe::Null();
    case NodeType::Attribute: element = static_cast<Attr*>(self)->ownerElement; break;
    default: element = self->parentNode && self->parentNode->IsElement() ? static_cast<Element*>(self->parentNode) : nullptr; break;
  }
  return element ? StringValue(ctx, LocatePrefix(element, *ns)) : qe::Null();
}

Value LookupNamespaceUri(Context& ctx, Value t, qe::Args args, Value) {
  Node* self = ThisNode(ctx, t);
  if (!self || Missing(ctx, args, 1, "Node", "lookupNamespaceURI")) return qe::Undefined();
  std::optional<std::string> prefix = NullableString(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  if (prefix && prefix->empty()) prefix.reset();
  return StringValue(ctx, LocateNamespace(self, prefix));
}

Value IsDefaultNamespace(Context& ctx, Value t, qe::Args args, Value) {
  Node* self = ThisNode(ctx, t);
  if (!self || Missing(ctx, args, 1, "Node", "isDefaultNamespace")) return qe::Undefined();
  std::optional<std::string> ns = NullableString(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  if (ns && ns->empty()) ns.reset();
  return qe::FromBool(LocateNamespace(self, std::nullopt) == ns);
}

Value InsertBefore(Context& ctx, Value t, qe::Args args, Value) {
  Node* self = ThisNode(ctx, t);
  if (!self || Missing(ctx, args, 2, "Node", "insertBefore")) return qe::Undefined();
  Node* node = NodeArgument(ctx, args, 0, "Failed to execute 'insertBefore' on 'Node'");
  if (!node) return qe::Undefined();
  Node* child = nullptr;
  if (!qe::IsNull(args[1])) {
    child = NodeArgument(ctx, args, 1, "Failed to execute 'insertBefore' on 'Node'");
    if (!child) return qe::Undefined();
  }
  if (auto error = PreInsert(node, self, child)) {
    Throw(ctx, *error);
    return qe::Undefined();
  }
  return qe::FromObject(node);
}

Value AppendChildMethod(Context& ctx, Value t, qe::Args args, Value) {
  Node* self = ThisNode(ctx, t);
  if (!self || Missing(ctx, args, 1, "Node", "appendChild")) return qe::Undefined();
  Node* node = NodeArgument(ctx, args, 0, "Failed to execute 'appendChild' on 'Node'");
  if (!node) return qe::Undefined();
  if (auto error = AppendChild(self, node)) {
    Throw(ctx, *error);
    return qe::Undefined();
  }
  return qe::FromObject(node);
}

Value ReplaceChildMethod(Context& ctx, Value t, qe::Args args, Value) {
  Node* self = ThisNode(ctx, t);
  if (!self || Missing(ctx, args, 2, "Node", "replaceChild")) return qe::Undefined();
  Node* node = NodeArgument(ctx, args, 0, "Failed to execute 'replaceChild' on 'Node'");
  Node* child = node ? NodeArgument(ctx, args, 1, "Failed to execute 'replaceChild' on 'Node'") : nullptr;
  if (!node || !child) return qe::Undefined();
  if (auto error = ReplaceChild(self, node, child)) {
    Throw(ctx, *error);
    return qe::Undefined();
  }
  return qe::FromObject(child);
}

Value RemoveChildMethod(Context& ctx, Value t, qe::Args args, Value) {
  Node* self = ThisNode(ctx, t);
  if (!self || Missing(ctx, args, 1, "Node", "removeChild")) return qe::Undefined();
  Node* child = NodeArgument(ctx, args, 0, "Failed to execute 'removeChild' on 'Node'");
  if (!child) return qe::Undefined();
  if (auto error = RemoveChild(self, child)) {
    Throw(ctx, *error);
    return qe::Undefined();
  }
  return qe::FromObject(child);
}

// ---- ParentNode, ChildNode, NonDocumentTypeChildNode ----

Element* NextElement(Node* node, bool forward) {
  for (; node; node = forward ? node->nextSibling : node->previousSibling) {
    if (node->IsElement()) return static_cast<Element*>(node);
  }
  return nullptr;
}

Value GetChildren(Context& ctx, Value t, qe::Args, Value) {
  Node* self = ThisNode(ctx, t);
  if (!self) return qe::Undefined();
  if (!self->childrenList) {
    self->childrenList = NewCollection(ctx, JsCollection::Kind::Children, self, false);
    self->NoteWrite(qe::FromObject(self->childrenList));
  }
  return qe::FromObject(self->childrenList);
}

Value GetFirstElementChild(Context& ctx, Value t, qe::Args, Value) {
  Node* self = ThisNode(ctx, t);
  return self ? NodeValue(NextElement(self->firstChild, true)) : qe::Undefined();
}

Value GetLastElementChild(Context& ctx, Value t, qe::Args, Value) {
  Node* self = ThisNode(ctx, t);
  return self ? NodeValue(NextElement(self->lastChild, false)) : qe::Undefined();
}

Value GetChildElementCount(Context& ctx, Value t, qe::Args, Value) {
  Node* self = ThisNode(ctx, t);
  if (!self) return qe::Undefined();
  uint32_t count = 0;
  for (Node* child = self->firstChild; child; child = child->nextSibling) count += child->IsElement();
  return qe::FromUint32(count);
}

Value PrependMethod(Context& ctx, Value t, qe::Args args, Value) {
  Node* self = ThisNode(ctx, t);
  if (!self) return qe::Undefined();
  Node* node = ConvertNodesIntoNode(ctx, args, DocumentFor(self));
  if (!node) return qe::Undefined();
  if (auto error = PreInsert(node, self, self->firstChild)) Throw(ctx, *error);
  return qe::Undefined();
}

Value AppendMethod(Context& ctx, Value t, qe::Args args, Value) {
  Node* self = ThisNode(ctx, t);
  if (!self) return qe::Undefined();
  Node* node = ConvertNodesIntoNode(ctx, args, DocumentFor(self));
  if (!node) return qe::Undefined();
  if (auto error = PreInsert(node, self, nullptr)) Throw(ctx, *error);
  return qe::Undefined();
}

Value ReplaceChildrenMethod(Context& ctx, Value t, qe::Args args, Value) {
  Node* self = ThisNode(ctx, t);
  if (!self) return qe::Undefined();
  Node* node = ConvertNodesIntoNode(ctx, args, DocumentFor(self));
  if (!node) return qe::Undefined();
  if (auto error = EnsurePreInsertionValidity(node, self, nullptr)) {
    Throw(ctx, *error);
    return qe::Undefined();
  }
  while (self->firstChild) RemoveUnchecked(self->firstChild);
  InsertUnchecked(node, self, nullptr);
  return qe::Undefined();
}

bool IsAmong(const Node* node, qe::Args args) {
  for (const Value& arg : args) {
    if (DOMObject::Cast<Node>(arg) == node) return true;
  }
  return false;
}

Value BeforeMethod(Context& ctx, Value t, qe::Args args, Value) {
  Node* self = ThisNode(ctx, t);
  if (!self || !self->parentNode) return qe::Undefined();
  Node* parent = self->parentNode;
  Node* viablePrevious = self->previousSibling;
  while (viablePrevious && IsAmong(viablePrevious, args)) viablePrevious = viablePrevious->previousSibling;
  Node* node = ConvertNodesIntoNode(ctx, args, DocumentFor(self));
  if (!node) return qe::Undefined();
  Node* reference = viablePrevious ? viablePrevious->nextSibling : parent->firstChild;
  if (auto error = PreInsert(node, parent, reference)) Throw(ctx, *error);
  return qe::Undefined();
}

Value AfterMethod(Context& ctx, Value t, qe::Args args, Value) {
  Node* self = ThisNode(ctx, t);
  if (!self || !self->parentNode) return qe::Undefined();
  Node* parent = self->parentNode;
  Node* viableNext = self->nextSibling;
  while (viableNext && IsAmong(viableNext, args)) viableNext = viableNext->nextSibling;
  Node* node = ConvertNodesIntoNode(ctx, args, DocumentFor(self));
  if (!node) return qe::Undefined();
  if (auto error = PreInsert(node, parent, viableNext)) Throw(ctx, *error);
  return qe::Undefined();
}

Value ReplaceWithMethod(Context& ctx, Value t, qe::Args args, Value) {
  Node* self = ThisNode(ctx, t);
  if (!self || !self->parentNode) return qe::Undefined();
  Node* parent = self->parentNode;
  Node* viableNext = self->nextSibling;
  while (viableNext && IsAmong(viableNext, args)) viableNext = viableNext->nextSibling;
  Node* node = ConvertNodesIntoNode(ctx, args, DocumentFor(self));
  if (!node) return qe::Undefined();
  const auto error = self->parentNode == parent ? ReplaceChild(parent, node, self) : PreInsert(node, parent, viableNext);
  if (error) Throw(ctx, *error);
  return qe::Undefined();
}

Value RemoveMethod(Context& ctx, Value t, qe::Args, Value) {
  Node* self = ThisNode(ctx, t);
  if (self && self->parentNode) RemoveUnchecked(self);
  return qe::Undefined();
}

Value GetPreviousElementSibling(Context& ctx, Value t, qe::Args, Value) {
  Node* self = ThisNode(ctx, t);
  return self ? NodeValue(NextElement(self->previousSibling, false)) : qe::Undefined();
}

Value GetNextElementSibling(Context& ctx, Value t, qe::Args, Value) {
  Node* self = ThisNode(ctx, t);
  return self ? NodeValue(NextElement(self->nextSibling, true)) : qe::Undefined();
}

}  // namespace

void DefineParentNode(Object* prototype) {
  qe::DefineAccessor(prototype, "children", GetChildren, nullptr);
  qe::DefineAccessor(prototype, "firstElementChild", GetFirstElementChild, nullptr);
  qe::DefineAccessor(prototype, "lastElementChild", GetLastElementChild, nullptr);
  qe::DefineAccessor(prototype, "childElementCount", GetChildElementCount, nullptr);
  qe::DefineMethod(prototype, "prepend", PrependMethod, 0);
  qe::DefineMethod(prototype, "append", AppendMethod, 0);
  qe::DefineMethod(prototype, "replaceChildren", ReplaceChildrenMethod, 0);
}

void DefineChildNode(Object* prototype) {
  qe::DefineMethod(prototype, "before", BeforeMethod, 0);
  qe::DefineMethod(prototype, "after", AfterMethod, 0);
  qe::DefineMethod(prototype, "replaceWith", ReplaceWithMethod, 0);
  qe::DefineMethod(prototype, "remove", RemoveMethod, 0);
}

void DefineNonDocumentTypeChildNode(Object* prototype) {
  qe::DefineAccessor(prototype, "previousElementSibling", GetPreviousElementSibling, nullptr);
  qe::DefineAccessor(prototype, "nextElementSibling", GetNextElementSibling, nullptr);
}

void DefineNodeClass(Context& ctx) {
  qe::ClassRef node = qe::DefineClass(ctx, "Node", IllegalConstructor, 0, web::EventTargetPrototype(ctx));
  Object* p = node.prototype;
  qe::DefineAccessor(p, "nodeType", GetNodeType, nullptr);
  qe::DefineAccessor(p, "nodeName", GetNodeName, nullptr);
  qe::DefineAccessor(p, "baseURI", GetBaseUri, nullptr);
  qe::DefineAccessor(p, "isConnected", GetIsConnected, nullptr);
  qe::DefineAccessor(p, "ownerDocument", GetOwnerDocument, nullptr);
  qe::DefineMethod(p, "getRootNode", GetRootNodeMethod, 0);
  qe::DefineAccessor(p, "parentNode", GetParentNode, nullptr);
  qe::DefineAccessor(p, "parentElement", GetParentElement, nullptr);
  qe::DefineMethod(p, "hasChildNodes", HasChildNodes, 0);
  qe::DefineAccessor(p, "childNodes", GetChildNodes, nullptr);
  qe::DefineAccessor(p, "firstChild", GetFirstChild, nullptr);
  qe::DefineAccessor(p, "lastChild", GetLastChild, nullptr);
  qe::DefineAccessor(p, "previousSibling", GetPreviousSibling, nullptr);
  qe::DefineAccessor(p, "nextSibling", GetNextSibling, nullptr);
  qe::DefineAccessor(p, "nodeValue", GetNodeValue, SetNodeValue);
  qe::DefineAccessor(p, "textContent", GetTextContent, SetTextContentValue);
  qe::DefineMethod(p, "normalize", NormalizeMethod, 0);
  qe::DefineMethod(p, "cloneNode", CloneNodeMethod, 0);
  qe::DefineMethod(p, "isEqualNode", IsEqualNodeMethod, 1);
  qe::DefineMethod(p, "isSameNode", IsSameNodeMethod, 1);
  qe::DefineMethod(p, "compareDocumentPosition", CompareDocumentPositionMethod, 1);
  qe::DefineMethod(p, "contains", ContainsMethod, 1);
  qe::DefineMethod(p, "lookupPrefix", LookupPrefix, 1);
  qe::DefineMethod(p, "lookupNamespaceURI", LookupNamespaceUri, 1);
  qe::DefineMethod(p, "isDefaultNamespace", IsDefaultNamespace, 1);
  qe::DefineMethod(p, "insertBefore", InsertBefore, 2);
  qe::DefineMethod(p, "appendChild", AppendChildMethod, 1);
  qe::DefineMethod(p, "replaceChild", ReplaceChildMethod, 2);
  qe::DefineMethod(p, "removeChild", RemoveChildMethod, 1);
  qe::DefineGlobal(ctx, "Node", node.constructor);
  qe::SetRealmData(ctx, &g_nodePrototypeKey, node.prototype);
}

}  // namespace solar::dom
