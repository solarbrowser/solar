#include "solar/dom/Node.h"

#include "solar/dom/Mutation.h"
#include "solar/dom/Range.h"

#include <algorithm>

namespace solar::dom {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::Heap;
using Quanta::Object;

// ---- Collector ----

void Node::Visit(Quanta::Visitor& visitor) {
  JsEventTarget::Visit(visitor);
  visitor.Mark(parentNode);
  visitor.Mark(firstChild);
  visitor.Mark(lastChild);
  visitor.Mark(previousSibling);
  visitor.Mark(nextSibling);
  visitor.Mark(nodeDocument);
  visitor.Mark(childNodesList);
  visitor.Mark(childrenList);
  if (registrations) {
    for (const Registration& registration : *registrations) visitor.Mark(registration.observer);
  }
}

void Document::Visit(Quanta::Visitor& visitor) {
  Node::Visit(visitor);
  visitor.Mark(implementation);
  visitor.Mark(templateContentsOwner);
  visitor.Mark(currentScript);
  visitor.Mark(selection);
  visitor.Mark(window);
}

void DocumentFragment::Visit(Quanta::Visitor& visitor) {
  Node::Visit(visitor);
  visitor.Mark(host);
}

void Attr::Visit(Quanta::Visitor& visitor) {
  Node::Visit(visitor);
  visitor.Mark(ownerElement);
}

void Element::Visit(Quanta::Visitor& visitor) {
  Node::Visit(visitor);
  for (Attr* attribute : attributes) visitor.Mark(attribute);
  visitor.Mark(attributeMap);
  visitor.Mark(tokenList);
  visitor.Mark(templateContents);
}

// ---- Making nodes ----

namespace {
uint64_t g_treeVersion = 1;
}

uint64_t TreeVersion() { return g_treeVersion; }
void NoteTreeChange() { ++g_treeVersion; }

namespace {

char g_prototypeKeys[10];

char* PrototypeKey(Interface interface) { return &g_prototypeKeys[static_cast<int>(interface)]; }

template <typename T>
T* Make(Context& ctx, Interface interface, NodeType type, Document* document) {
  T* node = Heap::Allocate<T>();
  node->nodeType = type;
  node->nodeDocument = document;
  // An event goes on from a node to the node it is in.
  node->eventParent = type == NodeType::Document ? [](web::JsEventTarget* target) -> web::JsEventTarget* { return static_cast<Document*>(target)->window; }
                                                  : [](web::JsEventTarget* target) -> web::JsEventTarget* { return static_cast<Node*>(target)->parentNode; };
  node->initialize_prototype(InterfacePrototype(ctx, interface));
  return node;
}

}  // namespace

void SetInterfacePrototype(Context& ctx, Interface interface, Object* prototype) { qe::SetRealmData(ctx, PrototypeKey(interface), prototype); }

Object* InterfacePrototype(Context& ctx, Interface interface) { return static_cast<Object*>(qe::GetRealmData(ctx, PrototypeKey(interface))); }

Document* NewDocument(Context& ctx, bool isHtml) {
  Document* document = Make<Document>(ctx, Interface::Document, NodeType::Document, nullptr);
  document->isHtml = isHtml;
  document->contentType = isHtml ? "text/html" : "application/xml";
  return document;
}

Document* TemplateContentsOwner(Context& ctx, Document* document) {
  if (!document->templateContentsOwner) {
    document->templateContentsOwner = NewDocument(ctx, document->isHtml);
    document->NoteWrite();
    // It is its own owner, so that a template inside a template's contents has contents of the same kind.
    document->templateContentsOwner->templateContentsOwner = document->templateContentsOwner;
  }
  return document->templateContentsOwner;
}

Element* NewElement(Context& ctx, Document* document, std::string_view localName, std::string_view namespaceUri, std::string_view prefix) {
  Element* element = Make<Element>(ctx, namespaceUri == kHtmlNamespace && InterfacePrototype(ctx, Interface::HtmlElement) ? Interface::HtmlElement : Interface::Element, NodeType::Element, document);
  if (namespaceUri == kHtmlNamespace) {
    if (Object* prototype = HtmlElementPrototype(ctx, localName)) element->initialize_prototype(prototype);
  }
  element->localName = localName;
  element->namespaceUri = namespaceUri;
  element->prefix = prefix;
  if (namespaceUri == kHtmlNamespace && localName == "template" && document) {
    DocumentFragment* contents = NewDocumentFragment(ctx, TemplateContentsOwner(ctx, document));
    contents->host = element;
    element->templateContents = contents;
    element->NoteWrite();
  }
  return element;
}

CharacterData* NewText(Context& ctx, Document* document, std::string data) {
  CharacterData* text = Make<CharacterData>(ctx, Interface::Text, NodeType::Text, document);
  text->data = std::move(data);
  return text;
}

CharacterData* NewComment(Context& ctx, Document* document, std::string data) {
  CharacterData* comment = Make<CharacterData>(ctx, Interface::Comment, NodeType::Comment, document);
  comment->data = std::move(data);
  return comment;
}

CharacterData* NewCdataSection(Context& ctx, Document* document, std::string data) {
  CharacterData* section = Make<CharacterData>(ctx, Interface::CdataSection, NodeType::CdataSection, document);
  section->data = std::move(data);
  return section;
}

CharacterData* NewProcessingInstruction(Context& ctx, Document* document, std::string target, std::string data) {
  CharacterData* instruction = Make<CharacterData>(ctx, Interface::ProcessingInstruction, NodeType::ProcessingInstruction, document);
  instruction->target = std::move(target);
  instruction->data = std::move(data);
  return instruction;
}

DocumentType* NewDocumentType(Context& ctx, Document* document, std::string name, std::string publicId, std::string systemId) {
  DocumentType* type = Make<DocumentType>(ctx, Interface::DocumentType, NodeType::DocumentType, document);
  type->name = std::move(name);
  type->publicId = std::move(publicId);
  type->systemId = std::move(systemId);
  return type;
}

DocumentFragment* NewDocumentFragment(Context& ctx, Document* document) {
  return Make<DocumentFragment>(ctx, Interface::DocumentFragment, NodeType::DocumentFragment, document);
}

Attr* NewAttr(Context& ctx, Document* document, std::string_view namespaceUri, std::string_view prefix, std::string_view localName, std::string value) {
  Attr* attribute = Make<Attr>(ctx, Interface::Attr, NodeType::Attribute, document);
  attribute->namespaceUri = namespaceUri;
  attribute->prefix = prefix;
  attribute->localName = localName;
  attribute->value = std::move(value);
  return attribute;
}

// ---- Queries ----

size_t Node::ChildCount() const {
  size_t count = 0;
  for (const Node* child = firstChild; child; child = child->nextSibling) ++count;
  return count;
}

Node* Node::Root() {
  Node* node = this;
  while (node->parentNode) node = node->parentNode;
  return node;
}

bool Node::Contains(const Node* other) const {
  for (; other; other = other->parentNode) {
    if (other == this) return true;
  }
  return false;
}

size_t Node::IndexInParent() const {
  size_t index = 0;
  for (const Node* node = previousSibling; node; node = node->previousSibling) ++index;
  return index;
}

Node* Node::NextInTree(const Node* root) {
  if (firstChild) return firstChild;
  for (Node* node = this; node && node != root; node = node->parentNode) {
    if (node->nextSibling) return node->nextSibling;
  }
  return nullptr;
}

std::string Node::DescendantText() const {
  std::string text;
  for (Node* node = firstChild; node;) {
    if (node->IsText()) text += static_cast<CharacterData*>(node)->data;
    Node* next = node->NextInTree(this);
    node = next;
  }
  return text;
}

Element* Document::DocumentElement() const {
  for (Node* child = firstChild; child; child = child->nextSibling) {
    if (child->IsElement()) return static_cast<Element*>(child);
  }
  return nullptr;
}

DocumentType* Document::Doctype() const {
  for (Node* child = firstChild; child; child = child->nextSibling) {
    if (child->IsDocumentType()) return static_cast<DocumentType*>(child);
  }
  return nullptr;
}

// ---- Insertion and removal ----

namespace {

DomError HierarchyRequest(const char* message) { return {"HierarchyRequestError", message}; }

Document* DocumentOf(Node* node) { return node->IsDocument() ? static_cast<Document*>(node) : node->nodeDocument; }

bool HasElementChild(const Node* parent, const Node* except = nullptr) {
  for (const Node* child = parent->firstChild; child; child = child->nextSibling) {
    if (child->IsElement() && child != except) return true;
  }
  return false;
}

bool DoctypeFollows(const Node* child) {
  for (const Node* node = child->nextSibling; node; node = node->nextSibling) {
    if (node->IsDocumentType()) return true;
  }
  return false;
}

bool ElementPrecedes(const Node* child) {
  for (const Node* node = child->previousSibling; node; node = node->previousSibling) {
    if (node->IsElement()) return true;
  }
  return false;
}

// What the standard checks of an insertion and of a replacement alike. `replacing` names the child
// that is going away, which does not count against the node that takes its place.
std::optional<DomError> Validate(Node* node, Node* parent, Node* child, bool replacing) {
  if (!parent->IsDocument() && !parent->IsFragment() && !parent->IsElement()) return HierarchyRequest("The parent is not a Document, DocumentFragment or Element");
  if (node->Contains(parent)) return HierarchyRequest("The new child contains the parent");
  if (child && child->parentNode != parent) return DomError{"NotFoundError", "The child is not a child of this node"};
  if (!node->IsFragment() && !node->IsDocumentType() && !node->IsElement() && !node->IsCharacterData()) return HierarchyRequest("This node type cannot be inserted");
  if (node->IsText() && parent->IsDocument()) return HierarchyRequest("A Text node cannot be a child of a Document");
  if (node->IsDocumentType() && !parent->IsDocument()) return HierarchyRequest("A doctype can only be a child of a Document");
  if (!parent->IsDocument()) return std::nullopt;

  const Node* going = replacing ? child : nullptr;
  if (node->IsFragment()) {
    size_t elements = 0;
    for (const Node* n = node->firstChild; n; n = n->nextSibling) {
      if (n->IsElement()) ++elements;
      if (n->IsText()) return HierarchyRequest("A Document cannot have a Text child");
    }
    if (elements > 1) return HierarchyRequest("A Document can have only one element child");
    if (elements == 1) {
      if (HasElementChild(parent, going) || (!replacing && child && child->IsDocumentType()) || (child && DoctypeFollows(child))) {
        return HierarchyRequest("A Document can have only one element child");
      }
    }
  } else if (node->IsElement()) {
    if (HasElementChild(parent, going) || (!replacing && child && child->IsDocumentType()) || (child && DoctypeFollows(child))) {
      return HierarchyRequest("A Document can have only one element child");
    }
  } else if (node->IsDocumentType()) {
    bool hasOther = false;
    for (const Node* n = parent->firstChild; n; n = n->nextSibling) {
      if (n->IsDocumentType() && n != going) hasOther = true;
    }
    if (hasOther || (child && ElementPrecedes(child)) || (!child && HasElementChild(parent))) return HierarchyRequest("A Document can have only one doctype, before its element");
  }
  return std::nullopt;
}

void Unlink(Node* node) {
  NoteTreeChange();
  Node* parent = node->parentNode;
  if (node->previousSibling) {
    node->previousSibling->nextSibling = node->nextSibling;
    node->previousSibling->NoteWrite();
  } else {
    parent->firstChild = node->nextSibling;
  }
  if (node->nextSibling) {
    node->nextSibling->previousSibling = node->previousSibling;
    node->nextSibling->NoteWrite();
  } else {
    parent->lastChild = node->previousSibling;
  }
  parent->NoteWrite();
  node->parentNode = nullptr;
  node->previousSibling = nullptr;
  node->nextSibling = nullptr;
  node->NoteWrite();
}

void Link(Node* node, Node* parent, Node* child) {
  NoteTreeChange();
  node->parentNode = parent;
  node->previousSibling = child ? child->previousSibling : parent->lastChild;
  node->nextSibling = child;
  if (node->previousSibling) {
    node->previousSibling->nextSibling = node;
    node->previousSibling->NoteWrite();
  } else {
    parent->firstChild = node;
  }
  if (child) {
    child->previousSibling = node;
    child->NoteWrite();
  } else {
    parent->lastChild = node;
  }
  parent->NoteWrite();
  node->NoteWrite();
}

void SetDocumentOfTree(Node* node, Document* document) {
  for (Node* n = node; n; n = n->NextInTree(node)) {
    if (n->nodeDocument != document) {
      n->nodeDocument = document;
      n->NoteWrite();
    }
    if (Element* element = AsElement(n)) {
      for (Attr* attribute : element->attributes) {
        attribute->nodeDocument = document;
        attribute->NoteWrite();
      }
    }
  }
}

}  // namespace

std::optional<DomError> EnsurePreInsertionValidity(Node* node, Node* parent, Node* child) { return Validate(node, parent, child, false); }

namespace {

// "remove" of the standard: the observers of a subtree are left with the node, and (unless suppressed) told.
void RemoveImpl(Node* node, bool suppress) {
  Node* parent = node->parentNode;
  if (!parent) return;
  Node* oldPrevious = node->previousSibling;
  Node* oldNext = node->nextSibling;
  const bool observed = HasMutationObservers();
  if (observed) RegisterTransientObservers(node, parent);
  if (HasLiveRanges()) RangesBeforeRemove(node, parent, static_cast<uint32_t>(node->IndexInParent()));
  Unlink(node);
  if (observed && !suppress) QueueChildListRecord(parent, {}, {node}, oldPrevious, oldNext);
}

std::vector<Node*> NodesOf(Node* node) {
  std::vector<Node*> nodes;
  if (node->IsFragment()) {
    for (Node* n = node->firstChild; n; n = n->nextSibling) nodes.push_back(n);
  } else {
    nodes.push_back(node);
  }
  return nodes;
}

// "insert" of the standard.
void InsertImpl(Node* node, Node* parent, Node* child, bool suppress) {
  Document* document = DocumentOf(parent);
  const std::vector<Node*> nodes = NodesOf(node);
  if (nodes.empty()) return;
  Node* previous = child ? child->previousSibling : parent->lastChild;
  if (child && HasLiveRanges()) RangesBeforeInsert(parent, static_cast<uint32_t>(child->IndexInParent()), static_cast<uint32_t>(nodes.size()));
  if (node->IsFragment()) {
    for (Node* n : nodes) RemoveImpl(n, true);
  }
  for (Node* n : nodes) {
    // Adopting takes it out of where it was, which the observers there hear of.
    if (n->parentNode) RemoveImpl(n, false);
    if (n->nodeDocument != document) SetDocumentOfTree(n, document);
    Link(n, parent, child);
  }
  if (!suppress && HasMutationObservers()) QueueChildListRecord(parent, nodes, {}, previous, child);
}

}  // namespace

void RemoveUnchecked(Node* node) { RemoveImpl(node, false); }

void Adopt(Node* node, Document* document) {
  RemoveImpl(node, false);
  if (node->nodeDocument != document) SetDocumentOfTree(node, document);
}

void InsertUnchecked(Node* node, Node* parent, Node* child) { InsertImpl(node, parent, child, false); }

std::optional<DomError> PreInsert(Node* node, Node* parent, Node* child) {
  if (auto error = EnsurePreInsertionValidity(node, parent, child)) return error;
  Node* reference = child == node ? node->nextSibling : child;
  InsertImpl(node, parent, reference, false);
  return std::nullopt;
}

std::optional<DomError> AppendChild(Node* parent, Node* node) { return PreInsert(node, parent, nullptr); }

std::optional<DomError> ReplaceChild(Node* parent, Node* node, Node* child) {
  if (!child) return DomError{"NotFoundError", "The child is not a child of this node"};
  if (auto error = Validate(node, parent, child, true)) return error;
  Node* reference = child->nextSibling;
  if (reference == node) reference = node->nextSibling;
  Node* previous = child->previousSibling;
  std::vector<Node*> removed;
  if (child->parentNode) {
    removed.push_back(child);
    RemoveImpl(child, true);
  }
  const std::vector<Node*> added = NodesOf(node);
  InsertImpl(node, parent, reference, true);
  if (HasMutationObservers()) QueueChildListRecord(parent, added, removed, previous, reference);
  return std::nullopt;
}

std::optional<DomError> RemoveChild(Node* parent, Node* child) {
  if (child->parentNode != parent) return DomError{"NotFoundError", "The node to be removed is not a child of this node"};
  RemoveImpl(child, false);
  return std::nullopt;
}

void ReplaceAll(Node* parent, Node* node) {
  std::vector<Node*> removed;
  for (Node* child = parent->firstChild; child; child = child->nextSibling) removed.push_back(child);
  const std::vector<Node*> added = node ? NodesOf(node) : std::vector<Node*>();
  for (Node* child : removed) RemoveImpl(child, true);
  if (node) InsertImpl(node, parent, nullptr, true);
  if (HasMutationObservers() && (!added.empty() || !removed.empty())) QueueChildListRecord(parent, added, removed, nullptr, nullptr);
}

void SetTextContent(Context& ctx, Node* node, std::string text) {
  ReplaceAll(node, text.empty() ? nullptr : NewText(ctx, DocumentOf(node), std::move(text)));
}

// ---- Character data, in the UTF-16 units the standard counts it in ----
// The data is kept as WTF-8: a lone surrogate is the three bytes its code point has, as the engine keeps it.

std::u16string ToUtf16(std::string_view text) {
  std::u16string out;
  for (size_t at = 0; at < text.size();) {
    const unsigned char lead = static_cast<unsigned char>(text[at]);
    uint32_t point;
    int length;
    if (lead < 0x80) { point = lead; length = 1; }
    else if (lead >= 0xF0) { point = lead & 0x07; length = 4; }
    else if (lead >= 0xE0) { point = lead & 0x0F; length = 3; }
    else { point = lead & 0x1F; length = 2; }
    for (int i = 1; i < length && at + i < text.size(); ++i) point = (point << 6) | (static_cast<unsigned char>(text[at + i]) & 0x3F);
    at += length;
    if (point >= 0x10000) {
      point -= 0x10000;
      out.push_back(static_cast<char16_t>(0xD800 + (point >> 10)));
      out.push_back(static_cast<char16_t>(0xDC00 + (point & 0x3FF)));
    } else {
      out.push_back(static_cast<char16_t>(point));
    }
  }
  return out;
}

std::string FromUtf16(std::u16string_view text) {
  std::string out;
  const auto append = [&out](uint32_t point) {
    if (point < 0x80) {
      out.push_back(static_cast<char>(point));
    } else if (point < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (point >> 6)));
      out.push_back(static_cast<char>(0x80 | (point & 0x3F)));
    } else if (point < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (point >> 12)));
      out.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (point & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (point >> 18)));
      out.push_back(static_cast<char>(0x80 | ((point >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (point & 0x3F)));
    }
  };
  for (size_t i = 0; i < text.size(); ++i) {
    const char16_t unit = text[i];
    if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < text.size() && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
      append(0x10000 + ((unit - 0xD800) << 10) + (text[i + 1] - 0xDC00));
      ++i;
    } else {
      append(unit);
    }
  }
  return out;
}

uint32_t Utf16Length(std::string_view text) { return static_cast<uint32_t>(ToUtf16(text).size()); }

uint32_t NodeLength(const Node* node) {
  switch (node->nodeType) {
    case NodeType::DocumentType:
    case NodeType::Attribute:
      return 0;
    case NodeType::Text:
    case NodeType::CdataSection:
    case NodeType::ProcessingInstruction:
    case NodeType::Comment:
      return Utf16Length(static_cast<const CharacterData*>(node)->data);
    default:
      return static_cast<uint32_t>(node->ChildCount());
  }
}

std::optional<DomError> ReplaceData(CharacterData* node, uint32_t offset, uint32_t count, std::string_view replacement) {
  std::u16string units = ToUtf16(node->data);
  if (offset > units.size()) {
    return DomError{"IndexSizeError", "The offset " + std::to_string(offset) + " is larger than the node's length (" + std::to_string(units.size()) + ")."};
  }
  if (count > units.size() - offset) count = static_cast<uint32_t>(units.size() - offset);
  const std::u16string inserted = ToUtf16(replacement);
  if (HasMutationObservers()) QueueCharacterDataRecord(node, node->data);
  units.replace(offset, count, inserted);
  node->data = FromUtf16(units);
  if (HasLiveRanges()) RangesReplaceData(node, offset, count, static_cast<uint32_t>(inserted.size()));
  return std::nullopt;
}

void SetCharacterData(CharacterData* node, std::string data) {
  if (HasLiveRanges()) {
    ReplaceData(node, 0, Utf16Length(node->data), data);
    return;
  }
  if (HasMutationObservers()) QueueCharacterDataRecord(node, node->data);
  node->data = std::move(data);
}

void AppendCharacterData(CharacterData* node, std::string_view data) {
  if (HasLiveRanges()) {
    ReplaceData(node, Utf16Length(node->data), 0, data);
    return;
  }
  if (HasMutationObservers()) QueueCharacterDataRecord(node, node->data);
  node->data += data;
}

void SetAttrValue(Attr* attribute, std::string value) {
  if (attribute->ownerElement && HasMutationObservers()) QueueAttributeRecord(attribute->ownerElement, attribute->localName, attribute->namespaceUri, attribute->value);
  attribute->value = std::move(value);
  NoteTreeChange();
}

void AppendAttr(Element* element, Attr* attribute) {
  attribute->ownerElement = element;
  attribute->nodeDocument = element->nodeDocument;
  element->attributes.push_back(attribute);
  element->NoteWrite();
  attribute->NoteWrite();
  NoteTreeChange();
  if (HasMutationObservers()) QueueAttributeRecord(element, attribute->localName, attribute->namespaceUri, std::nullopt);
}

// ---- Clone, equality, position ----

Node* CloneNode(Context& ctx, Node* node, bool deep) {
  Node* copy = nullptr;
  Document* document = DocumentOf(node);
  switch (node->nodeType) {
    case NodeType::Document: {
      Document* original = static_cast<Document*>(node);
      Document* clone = NewDocument(ctx, original->isHtml);
      clone->url = original->url;
      clone->contentType = original->contentType;
      clone->characterSet = original->characterSet;
      clone->mode = original->mode;
      copy = clone;
      document = clone;
      break;
    }
    case NodeType::Element: {
      Element* original = static_cast<Element*>(node);
      Element* clone = NewElement(ctx, document, original->localName, original->namespaceUri, original->prefix);
      for (Attr* attribute : original->attributes) {
        Attr* attributeCopy = NewAttr(ctx, document, attribute->namespaceUri, attribute->prefix, attribute->localName, attribute->value);
        attributeCopy->ownerElement = clone;
        clone->attributes.push_back(attributeCopy);
      }
      clone->NoteWrite();
      if (deep && original->templateContents && clone->templateContents) {
        for (Node* child = original->templateContents->firstChild; child; child = child->nextSibling) {
          InsertUnchecked(CloneNode(ctx, child, true), clone->templateContents, nullptr);
        }
      }
      copy = clone;
      break;
    }
    case NodeType::Text:
      copy = NewText(ctx, document, static_cast<CharacterData*>(node)->data);
      break;
    case NodeType::CdataSection:
      copy = NewCdataSection(ctx, document, static_cast<CharacterData*>(node)->data);
      break;
    case NodeType::Comment:
      copy = NewComment(ctx, document, static_cast<CharacterData*>(node)->data);
      break;
    case NodeType::ProcessingInstruction: {
      CharacterData* original = static_cast<CharacterData*>(node);
      copy = NewProcessingInstruction(ctx, document, original->target, original->data);
      break;
    }
    case NodeType::DocumentType: {
      DocumentType* original = static_cast<DocumentType*>(node);
      copy = NewDocumentType(ctx, document, original->name, original->publicId, original->systemId);
      break;
    }
    case NodeType::DocumentFragment:
      copy = NewDocumentFragment(ctx, document);
      break;
    case NodeType::Attribute: {
      Attr* original = static_cast<Attr*>(node);
      copy = NewAttr(ctx, document, original->namespaceUri, original->prefix, original->localName, original->value);
      break;
    }
  }
  if (deep) {
    for (Node* child = node->firstChild; child; child = child->nextSibling) {
      InsertUnchecked(CloneNode(ctx, child, true), copy, nullptr);
    }
  }
  return copy;
}

bool IsEqualNode(const Node* a, const Node* b) {
  if (a->nodeType != b->nodeType) return false;
  switch (a->nodeType) {
    case NodeType::DocumentType: {
      const auto* x = static_cast<const DocumentType*>(a);
      const auto* y = static_cast<const DocumentType*>(b);
      if (x->name != y->name || x->publicId != y->publicId || x->systemId != y->systemId) return false;
      break;
    }
    case NodeType::Element: {
      const auto* x = static_cast<const Element*>(a);
      const auto* y = static_cast<const Element*>(b);
      if (x->namespaceUri != y->namespaceUri || x->prefix != y->prefix || x->localName != y->localName || x->attributes.size() != y->attributes.size()) return false;
      for (const Attr* attribute : x->attributes) {
        const Attr* other = y->FindAttribute(attribute->namespaceUri, attribute->localName);
        if (!other || other->value != attribute->value) return false;
      }
      break;
    }
    case NodeType::Attribute: {
      const auto* x = static_cast<const Attr*>(a);
      const auto* y = static_cast<const Attr*>(b);
      if (x->namespaceUri != y->namespaceUri || x->localName != y->localName || x->value != y->value) return false;
      break;
    }
    case NodeType::ProcessingInstruction:
      if (static_cast<const CharacterData*>(a)->target != static_cast<const CharacterData*>(b)->target) return false;
      [[fallthrough]];
    case NodeType::Text:
    case NodeType::CdataSection:
    case NodeType::Comment:
      if (static_cast<const CharacterData*>(a)->data != static_cast<const CharacterData*>(b)->data) return false;
      break;
    default:
      break;
  }
  const Node* x = a->firstChild;
  const Node* y = b->firstChild;
  for (; x && y; x = x->nextSibling, y = y->nextSibling) {
    if (!IsEqualNode(x, y)) return false;
  }
  return !x && !y;
}

namespace {

// `a` comes before `b` in tree order; both are in one tree and neither contains the other.
bool Precedes(const Node* a, const Node* b) {
  std::vector<const Node*> pathA, pathB;
  for (const Node* n = a; n; n = n->parentNode) pathA.push_back(n);
  for (const Node* n = b; n; n = n->parentNode) pathB.push_back(n);
  // From the root down, to where they part.
  size_t i = pathA.size(), j = pathB.size();
  while (i > 0 && j > 0 && pathA[i - 1] == pathB[j - 1]) {
    --i;
    --j;
  }
  if (i == 0) return true;   // a contains b
  if (j == 0) return false;  // b contains a
  for (const Node* n = pathA[i - 1]->nextSibling; n; n = n->nextSibling) {
    if (n == pathB[j - 1]) return true;
  }
  return false;
}

}  // namespace

uint16_t CompareDocumentPosition(Node* thisNode, Node* other) {
  if (thisNode == other) return 0;
  Node* node1 = other;
  Node* node2 = thisNode;
  Attr* attr1 = nullptr;
  Attr* attr2 = nullptr;
  if (node1->nodeType == NodeType::Attribute) {
    attr1 = static_cast<Attr*>(node1);
    node1 = attr1->ownerElement;
  }
  if (node2->nodeType == NodeType::Attribute) {
    attr2 = static_cast<Attr*>(node2);
    node2 = attr2->ownerElement;
    if (attr1 && node1 && node1 == node2) {
      for (const Attr* attribute : static_cast<Element*>(node2)->attributes) {
        if (attribute == attr1) return kImplementationSpecific | kPreceding;
        if (attribute == attr2) return kImplementationSpecific | kFollowing;
      }
    }
  }
  if (!node1 || !node2 || node1->Root() != node2->Root()) {
    return kDisconnected | kImplementationSpecific | (std::less<const Node*>()(node1, node2) ? kPreceding : kFollowing);
  }
  if ((node1->Contains(node2) && !attr1) || (node1 == node2 && attr2)) return kContains | kPreceding;
  if ((node2->Contains(node1) && !attr2) || (node1 == node2 && attr1)) return kContainedBy | kFollowing;
  return Precedes(node1, node2) ? kPreceding : kFollowing;
}

// ---- Attributes ----

Attr* Element::FindAttribute(std::string_view name) const {
  for (Attr* attribute : attributes) {
    if (attribute->prefix.empty() ? attribute->localName == name : (attribute->prefix.size() + 1 + attribute->localName.size() == name.size() && name.starts_with(attribute->prefix) &&
                                                                     name[attribute->prefix.size()] == ':' && name.substr(attribute->prefix.size() + 1) == attribute->localName)) {
      return attribute;
    }
  }
  return nullptr;
}

Attr* Element::FindAttribute(std::string_view ns, std::string_view local) const {
  for (Attr* attribute : attributes) {
    if (attribute->namespaceUri == ns && attribute->localName == local) return attribute;
  }
  return nullptr;
}

namespace {

std::string LowerAscii(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 0x20);
  }
  return out;
}

// Whether the name is lowered: an element of the HTML namespace in an HTML document.
std::string AttributeNameFor(const Element* element, std::string_view name) {
  const Document* document = element->nodeDocument;
  return element->IsHtml() && document && document->isHtml ? LowerAscii(name) : std::string(name);
}

// Code points of UTF-8, which the data of the tree always is.
uint32_t DecodeOne(std::string_view text, size_t& at) {
  const unsigned char lead = static_cast<unsigned char>(text[at]);
  if (lead < 0x80) return text[at++];
  const int length = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
  uint32_t point = lead & (0x3F >> (length - 1));
  for (int i = 1; i < length && at + i < text.size(); ++i) point = (point << 6) | (static_cast<unsigned char>(text[at + i]) & 0x3F);
  at += length;
  return point;
}

bool IsNameStart(uint32_t c) {
  return c == ':' || (c >= 'A' && c <= 'Z') || c == '_' || (c >= 'a' && c <= 'z') || (c >= 0xC0 && c <= 0xD6) || (c >= 0xD8 && c <= 0xF6) || (c >= 0xF8 && c <= 0x2FF) ||
         (c >= 0x370 && c <= 0x37D) || (c >= 0x37F && c <= 0x1FFF) || (c >= 0x200C && c <= 0x200D) || (c >= 0x2070 && c <= 0x218F) || (c >= 0x2C00 && c <= 0x2FEF) ||
         (c >= 0x3001 && c <= 0xD7FF) || (c >= 0xF900 && c <= 0xFDCF) || (c >= 0xFDF0 && c <= 0xFFFD) || (c >= 0x10000 && c <= 0xEFFFF);
}

bool IsNameChar(uint32_t c) { return IsNameStart(c) || c == '-' || c == '.' || (c >= '0' && c <= '9') || c == 0xB7 || (c >= 0x300 && c <= 0x36F) || (c >= 0x203F && c <= 0x2040); }

}  // namespace

std::optional<std::string> GetAttribute(const Element* element, std::string_view name) {
  const Attr* attribute = element->FindAttribute(AttributeNameFor(element, name));
  if (!attribute) return std::nullopt;
  return attribute->value;
}

bool IsXmlName(std::string_view name) {
  if (name.empty()) return false;
  size_t at = 0;
  if (!IsNameStart(DecodeOne(name, at))) return false;
  while (at < name.size()) {
    if (!IsNameChar(DecodeOne(name, at))) return false;
  }
  return true;
}

bool IsNcName(std::string_view name) { return IsXmlName(name) && name.find(':') == std::string_view::npos; }

std::optional<DomError> ValidateQualifiedName(std::string_view qualifiedName) {
  const DomError invalid{"InvalidCharacterError", "The string contains invalid characters"};
  const size_t colon = qualifiedName.find(':');
  if (colon == std::string_view::npos) return IsNcName(qualifiedName) ? std::nullopt : std::optional<DomError>(invalid);
  if (!IsNcName(qualifiedName.substr(0, colon)) || !IsNcName(qualifiedName.substr(colon + 1))) return invalid;
  return std::nullopt;
}

std::optional<DomError> ValidateAndExtract(std::string_view ns, std::string_view qualifiedName, QualifiedParts& out) {
  const DomError invalid{"InvalidCharacterError", "The string contains invalid characters"};
  const size_t colon = qualifiedName.find(':');
  if (colon == std::string_view::npos) {
    if (!IsNcName(qualifiedName)) return invalid;
    out.prefix.clear();
    out.localName = qualifiedName;
  } else {
    // A QName: an NCName, a colon and an NCName.
    if (!IsNcName(qualifiedName.substr(0, colon)) || !IsNcName(qualifiedName.substr(colon + 1))) return invalid;
    out.prefix = qualifiedName.substr(0, colon);
    out.localName = qualifiedName.substr(colon + 1);
  }
  out.namespaceUri = ns;
  if (!out.prefix.empty() && ns.empty()) return DomError{"NamespaceError", "A prefix needs a namespace"};
  if (out.prefix == "xml" && ns != kXmlNamespace) return DomError{"NamespaceError", "The xml prefix needs the XML namespace"};
  if ((qualifiedName == "xmlns" || out.prefix == "xmlns") && ns != kXmlnsNamespace) return DomError{"NamespaceError", "The xmlns name needs the XMLNS namespace"};
  if (ns == kXmlnsNamespace && qualifiedName != "xmlns" && out.prefix != "xmlns") return DomError{"NamespaceError", "The XMLNS namespace is for xmlns names"};
  return std::nullopt;
}

std::optional<DomError> SetAttribute(Context& ctx, Element* element, std::string_view name, std::string value) {
  if (!IsXmlName(name)) return DomError{"InvalidCharacterError", "The string contains invalid characters"};

  const std::string lowered = AttributeNameFor(element, name);
  if (Attr* existing = element->FindAttribute(lowered)) {
    SetAttrValue(existing, std::move(value));
    return std::nullopt;
  }
  AppendAttr(element, NewAttr(ctx, element->nodeDocument, "", "", lowered, std::move(value)));
  return std::nullopt;
}

std::optional<DomError> SetAttributeNode(Element* element, Attr* attribute, Attr*& replaced) {
  replaced = nullptr;
  if (attribute->ownerElement && attribute->ownerElement != element) return DomError{"InUseAttributeError", "The attribute is in use by another element"};
  Attr* old = element->FindAttribute(attribute->namespaceUri, attribute->localName);
  if (old == attribute) {
    replaced = attribute;
    return std::nullopt;
  }
  if (old) {
    if (HasMutationObservers()) QueueAttributeRecord(element, old->localName, old->namespaceUri, old->value);
    *std::find(element->attributes.begin(), element->attributes.end(), old) = attribute;
    old->ownerElement = nullptr;
    old->NoteWrite();
    attribute->ownerElement = element;
    attribute->nodeDocument = element->nodeDocument;
    attribute->NoteWrite();
    element->NoteWrite();
    NoteTreeChange();
    if (HasMutationObservers()) QueueAttributeRecord(element, attribute->localName, attribute->namespaceUri, old->value);
  } else {
    AppendAttr(element, attribute);
  }
  replaced = old;
  return std::nullopt;
}

void RemoveAttributeNode(Element* element, Attr* attribute) {
  if (HasMutationObservers()) QueueAttributeRecord(element, attribute->localName, attribute->namespaceUri, attribute->value);
  element->attributes.erase(std::find(element->attributes.begin(), element->attributes.end(), attribute));
  attribute->ownerElement = nullptr;
  attribute->NoteWrite();
  NoteTreeChange();
}

bool RemoveAttribute(Element* element, std::string_view name) {
  Attr* attribute = element->FindAttribute(AttributeNameFor(element, name));
  if (!attribute) return false;
  RemoveAttributeNode(element, attribute);
  return true;
}

}  // namespace solar::dom
