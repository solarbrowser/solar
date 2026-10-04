#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "quanta/Embed.h"
#include "solar/web/DomBindingsInternal.h"

// The document tree of the DOM Standard (https://dom.spec.whatwg.org/).
//
// A node is a cell of the collector, a host object like any other, with its tree links as plain C++
// pointers: style and layout walk them without calling into script, and script gets the node itself,
// so a node is the same object each time it is reached. The types are not polymorphic (a cell has no
// vtable); a node's kind is its `nodeType`, and a subtype is reached by casting once that is known.
//
// What the standard raises as an exception is returned as a DomError instead, so that the tree
// operations can be used by native code (the HTML parser) as well as by script.
namespace solar::dom {

enum class NodeType : uint16_t {
  Element = 1,
  Attribute = 2,
  Text = 3,
  CdataSection = 4,
  ProcessingInstruction = 7,
  Comment = 8,
  Document = 9,
  DocumentType = 10,
  DocumentFragment = 11,
};

// What a MutationObserver was asked to watch for, and a registration of one on a node.
struct MutationOptions {
  bool childList = false;
  bool attributes = false;
  bool characterData = false;
  bool subtree = false;
  bool attributeOldValue = false;
  bool characterDataOldValue = false;
  bool hasAttributeFilter = false;
  std::vector<std::string> attributeFilter;
};

struct Registration {
  Quanta::Object* observer = nullptr;
  MutationOptions options;
  bool transient = false;  // left on a removed node, for the observers of the subtree it came from
  uint64_t id = 0;
  uint64_t source = 0;     // the registration a transient one stands for
};

struct Node;
struct Document;
struct Element;

// An exception the standard names: a DOMException's name and the message that goes with it.
struct DomError {
  std::string name;
  std::string message;
};

// The namespaces the standard and the HTML parser name.
inline constexpr std::string_view kHtmlNamespace = "http://www.w3.org/1999/xhtml";
inline constexpr std::string_view kSvgNamespace = "http://www.w3.org/2000/svg";
inline constexpr std::string_view kMathMlNamespace = "http://www.w3.org/1998/Math/MathML";
inline constexpr std::string_view kXmlNamespace = "http://www.w3.org/XML/1998/namespace";
inline constexpr std::string_view kXmlnsNamespace = "http://www.w3.org/2000/xmlns/";

struct Node : web::JsEventTarget {
  using Parent = web::JsEventTarget;

  NodeType nodeType = NodeType::Element;
  Node* parentNode = nullptr;
  Node* firstChild = nullptr;
  Node* lastChild = nullptr;
  Node* previousSibling = nullptr;
  Node* nextSibling = nullptr;
  // The document the node belongs to; null only for a Document itself.
  Document* nodeDocument = nullptr;
  // What script gets from childNodes and children, which is the same object each time.
  Quanta::Object* childNodesList = nullptr;
  Quanta::Object* childrenList = nullptr;
  // The mutation observers that watch this node; none for nearly every node.
  std::unique_ptr<std::vector<Registration>> registrations;

  void Visit(Quanta::Visitor& visitor);

  bool IsElement() const { return nodeType == NodeType::Element; }
  bool IsText() const { return nodeType == NodeType::Text || nodeType == NodeType::CdataSection; }
  bool IsCharacterData() const { return IsText() || nodeType == NodeType::ProcessingInstruction || nodeType == NodeType::Comment; }
  bool IsDocument() const { return nodeType == NodeType::Document; }
  bool IsDocumentType() const { return nodeType == NodeType::DocumentType; }
  bool IsFragment() const { return nodeType == NodeType::DocumentFragment; }

  bool HasChildNodes() const { return firstChild != nullptr; }
  size_t ChildCount() const;
  // The root: the last of this node and its ancestors.
  Node* Root();
  // `other` is this node or one of its descendants.
  bool Contains(const Node* other) const;
  // The index of the node among its parent's children.
  size_t IndexInParent() const;

  // The next node after this one in tree order, within `root`; null at the end of it.
  Node* NextInTree(const Node* root);
  // The descendant text, concatenated: the textContent of an element or a fragment.
  std::string DescendantText() const;
};

// A node whose content is a string: Text, CDATASection, Comment and ProcessingInstruction.
// The data is WTF-8 here (UTF-8, with a lone surrogate as the three bytes its code point has); the standard's offsets and lengths count UTF-16 code units, which the
// operations on it convert.
struct CharacterData : Node {
  using Parent = Node;
  std::string data;
  std::string target;  // a ProcessingInstruction's

  void Visit(Quanta::Visitor& visitor) { Node::Visit(visitor); }
};

struct DocumentType : Node {
  using Parent = Node;
  std::string name;
  std::string publicId;
  std::string systemId;

  void Visit(Quanta::Visitor& visitor) { Node::Visit(visitor); }
};

struct DocumentFragment : Node {
  using Parent = Node;
  Element* host = nullptr;  // for the content of a <template>

  void Visit(Quanta::Visitor& visitor);
};

// An attribute: a node of its own, so that script can hold one and see it change.
struct Attr : Node {
  using Parent = Node;
  std::string namespaceUri;  // empty: none
  std::string prefix;        // empty: none
  std::string localName;
  std::string value;
  Element* ownerElement = nullptr;

  // The qualified name: prefix:localName, or the local name alone.
  std::string QualifiedName() const { return prefix.empty() ? localName : prefix + ":" + localName; }
  void Visit(Quanta::Visitor& visitor);
};

struct Element : Node {
  using Parent = Node;
  std::string namespaceUri;  // empty: none
  std::string prefix;        // empty: none
  std::string localName;
  std::vector<Attr*> attributes;
  // attributes and classList, which are the same object each time.
  Quanta::Object* attributeMap = nullptr;
  Quanta::Object* tokenList = nullptr;
  // A template element's contents, which are not its children.
  DocumentFragment* templateContents = nullptr;

  std::string QualifiedName() const { return prefix.empty() ? localName : prefix + ":" + localName; }
  // `name` is compared as the standard does for a name on an HTML element in an HTML document: after
  // lowering its case.
  Attr* FindAttribute(std::string_view name) const;
  Attr* FindAttribute(std::string_view namespaceUri, std::string_view localName) const;
  bool IsHtml() const { return namespaceUri == kHtmlNamespace; }
  bool IsHtml(std::string_view local) const { return IsHtml() && localName == local; }
  void Visit(Quanta::Visitor& visitor);
};

struct Document : Node {
  using Parent = Node;
  std::string url = "about:blank";
  std::string contentType = "application/xml";
  std::string characterSet = "UTF-8";
  enum class Mode { NoQuirks, Quirks, LimitedQuirks };
  Mode mode = Mode::NoQuirks;
  bool isHtml = false;  // an HTML document and not an XML one
  Quanta::Object* implementation = nullptr;  // document.implementation, the same object each time
  // The inert document the contents of this one's template elements belong to.
  Document* templateContentsOwner = nullptr;
  // document.readyState, which the page loader moves along; a document made by script is complete.
  std::string readyState = "complete";
  // The script element that is running, for document.currentScript.
  Element* currentScript = nullptr;
  // The window this document is the document of, which an event on the document goes on to; null for any other.
  web::JsEventTarget* window = nullptr;

  Element* DocumentElement() const;
  DocumentType* Doctype() const;
  void Visit(Quanta::Visitor& visitor);
};

inline Element* AsElement(Node* node) { return node && node->IsElement() ? static_cast<Element*>(node) : nullptr; }
inline const Element* AsElement(const Node* node) { return node && node->IsElement() ? static_cast<const Element*>(node) : nullptr; }
inline Document* AsDocument(Node* node) { return node && node->IsDocument() ? static_cast<Document*>(node) : nullptr; }
inline CharacterData* AsCharacterData(Node* node) { return node && node->IsCharacterData() ? static_cast<CharacterData*>(node) : nullptr; }
inline const CharacterData* AsCharacterData(const Node* node) { return node && node->IsCharacterData() ? static_cast<const CharacterData*>(node) : nullptr; }

// ---- Making nodes ----

// The interfaces a node can have, for the prototype the realm's bindings give each.
enum class Interface { Document, Element, HtmlElement, Text, CdataSection, ProcessingInstruction, Comment, DocumentType, DocumentFragment, Attr };
// What DefineClass made for `interface`; the DOM bindings record it, and a node made before they have
// has none.
void SetInterfacePrototype(Quanta::Context& ctx, Interface interface, Quanta::Object* prototype);
Quanta::Object* InterfacePrototype(Quanta::Context& ctx, Interface interface);

// Nodes are made in the realm of `ctx`, with the prototype that realm's DOM bindings gave the kind
// (none, if it has not any).
Document* NewDocument(Quanta::Context& ctx, bool isHtml);
Element* NewElement(Quanta::Context& ctx, Document* document, std::string_view localName, std::string_view namespaceUri = kHtmlNamespace, std::string_view prefix = "");
CharacterData* NewText(Quanta::Context& ctx, Document* document, std::string data);
CharacterData* NewComment(Quanta::Context& ctx, Document* document, std::string data);
CharacterData* NewCdataSection(Quanta::Context& ctx, Document* document, std::string data);
CharacterData* NewProcessingInstruction(Quanta::Context& ctx, Document* document, std::string target, std::string data);
DocumentType* NewDocumentType(Quanta::Context& ctx, Document* document, std::string name, std::string publicId, std::string systemId);
DocumentFragment* NewDocumentFragment(Quanta::Context& ctx, Document* document);
// The inert document that the contents of the template elements of `document` belong to.
Document* TemplateContentsOwner(Quanta::Context& ctx, Document* document);
Attr* NewAttr(Quanta::Context& ctx, Document* document, std::string_view namespaceUri, std::string_view prefix, std::string_view localName, std::string value);

// A number that changes whenever any tree does, which the live collections check to know whether what
// they have found is still so.
uint64_t TreeVersion();
void NoteTreeChange();

// The prototype of the interface an HTML element of this name has: HTMLDivElement for "div", HTMLUnknownElement
// for a name the standard does not know, HTMLElement for a custom element's. The HTML bindings register them.
Quanta::Object* HtmlElementPrototype(Quanta::Context& ctx, std::string_view localName);
void RegisterHtmlElementInterface(Quanta::Context& ctx, std::string_view localName, Quanta::Object* prototype);
void SetUnknownHtmlElementInterface(Quanta::Context& ctx, Quanta::Object* prototype);

// ---- The tree ----
// Each is the algorithm of that name in the standard. All but the last return the error the standard
// raises, if it does; none has changed the tree when it does.

// "ensure pre-insertion validity" for inserting `node` into `parent` before `child` (null: at the end).
std::optional<DomError> EnsurePreInsertionValidity(Node* node, Node* parent, Node* child);
// appendChild and insertBefore.
std::optional<DomError> PreInsert(Node* node, Node* parent, Node* child);
std::optional<DomError> AppendChild(Node* parent, Node* node);
// replaceChild.
std::optional<DomError> ReplaceChild(Node* parent, Node* node, Node* child);
// removeChild.
std::optional<DomError> RemoveChild(Node* parent, Node* child);
// "insert" and "remove" without the checks, for a parser that has made sure of what it does.
void InsertUnchecked(Node* node, Node* parent, Node* child);
void RemoveUnchecked(Node* node);
// "adopt": moves `node` and what is under it into `document`, taking it out of its parent.
void Adopt(Node* node, Document* document);

// "replace all": makes `node` (null for none; a fragment's children) the only child of `parent`.
void ReplaceAll(Node* parent, Node* node);
// "replace data" with the whole of it, and the setting of an attribute's value and its appending to an element:
// the changes the observers hear of. Native code that changes these goes through them.
void SetCharacterData(CharacterData* node, std::string data);
// The same for adding to the end of it, without copying what is there when nothing is watching.
void AppendCharacterData(CharacterData* node, std::string_view data);
void SetAttrValue(Attr* attribute, std::string value);
void AppendAttr(Element* element, Attr* attribute);
// textContent set on a node that can have children: replaces them with one Text node, or none for "".
void SetTextContent(Quanta::Context& ctx, Node* node, std::string text);
// cloneNode.
Node* CloneNode(Quanta::Context& ctx, Node* node, bool deep);
// isEqualNode.
bool IsEqualNode(const Node* a, const Node* b);
// compareDocumentPosition's bit set.
constexpr uint16_t kDisconnected = 1, kPreceding = 2, kFollowing = 4, kContains = 8, kContainedBy = 16, kImplementationSpecific = 32;
uint16_t CompareDocumentPosition(Node* a, Node* b);

// ---- Names ----
// Whether `name` is an XML Name (the Name production), and an NCName (one without a colon).
bool IsXmlName(std::string_view name);
bool IsNcName(std::string_view name);
struct QualifiedParts {
  std::string namespaceUri;  // empty: none
  std::string prefix;        // empty: none
  std::string localName;
};
// "validate" of the standard: whether `qualifiedName` is a QName (a prefix needs no namespace here).
std::optional<DomError> ValidateQualifiedName(std::string_view qualifiedName);
// "validate and extract" of the standard, for createElementNS, setAttributeNS and the like.
std::optional<DomError> ValidateAndExtract(std::string_view namespaceUri, std::string_view qualifiedName, QualifiedParts& out);

// ---- Attributes ----
// "set an attribute": puts `attribute` in the element's list, in the place of the one with its namespace
// and local name if there is one, which is `replaced`. InUseAttributeError if another element has it.
std::optional<DomError> SetAttributeNode(Element* element, Attr* attribute, Attr*& replaced);
// "remove an attribute".
void RemoveAttributeNode(Element* element, Attr* attribute);

// setAttribute-style access on an element: the name is lowered on an HTML element in an HTML document.
std::optional<std::string> GetAttribute(const Element* element, std::string_view name);
// InvalidCharacterError if `name` is not an XML Name.
std::optional<DomError> SetAttribute(Quanta::Context& ctx, Element* element, std::string_view name, std::string value);
bool RemoveAttribute(Element* element, std::string_view name);

}  // namespace solar::dom
