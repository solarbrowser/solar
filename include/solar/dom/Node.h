#pragma once

#include <cstdint>
#include <functional>
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
struct CustomDefinition;

// An element's custom element state (https://html.spec.whatwg.org/#custom-element-state): Uncustomized is the one of
// every element that cannot be a custom one.
enum class CustomState : uint8_t { Uncustomized, Undefined, Failed, Precustomized, Custom };

// A reaction in an element's queue: calling one of the callbacks of its definition, or upgrading it.
struct CustomReaction {
  bool upgrade = false;
  CustomDefinition* definition = nullptr;  // for an upgrade
  Quanta::Value callback;
  std::vector<Quanta::Value> args;
};

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
  // Slottables (elements and text) have a slot they are assigned to, and one a script assigned them to by hand.
  Element* assignedSlot = nullptr;
  Element* manualSlot = nullptr;
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
  Element* host = nullptr;  // for the content of a <template>, or the shadow host of a shadow root
  bool isShadowRoot = false;

  void Visit(Quanta::Visitor& visitor);
};

enum class ShadowMode { Open, Closed };
enum class SlotAssignment { Named, Manual };

// A shadow root: the root of a tree of its own that an element, its host, carries beside its children.
struct ShadowRoot : DocumentFragment {
  using Parent = DocumentFragment;
  ShadowMode mode = ShadowMode::Open;
  SlotAssignment slotAssignment = SlotAssignment::Named;
  bool delegatesFocus = false;
  bool clonable = false;
  bool serializable = false;
  bool declarative = false;  // made by the parser, from a template
  bool availableToElementInternals = false;
  void Visit(Quanta::Visitor& visitor) { DocumentFragment::Visit(visitor); }
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
  ShadowRoot* shadowRoot = nullptr;
  // A slot's: the slottables it has been given, and those a script asked it to take in (slot assignment "manual").
  std::vector<Node*> assignedNodes;
  std::vector<Node*> manuallyAssignedNodes;
  // What makes it a custom element, if it is: its definition, the value of is that went with it, and the callbacks
  // that are waiting to be called.
  CustomState customState = CustomState::Uncustomized;
  CustomDefinition* customDefinition = nullptr;
  std::optional<std::string> isValue;
  std::vector<CustomReaction> reactions;
  // An iframe's nested browsing context: the document in it, and the window (global object) of that document.
  Document* contentDocument = nullptr;
  Quanta::Object* contentWindow = nullptr;
  uint64_t frameLoad = 0;  // which load of the frame is the one that counts
  // A script element's "already started" and "parser document" flags.
  bool scriptStarted = false;
  bool scriptParserInserted = false;

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
  bool isXmlDocument = false;  // one that implements XMLDocument
  Quanta::Object* implementation = nullptr;  // document.implementation, the same object each time
  // The inert document the contents of this one's template elements belong to.
  Document* templateContentsOwner = nullptr;
  // document.readyState, which the page loader moves along; a document made by script is complete.
  std::string readyState = "complete";
  // The script element that is running, for document.currentScript.
  Element* currentScript = nullptr;
  // The focused area of the document, if an element is: what document.activeElement is, once it is retargeted.
  Element* focusedElement = nullptr;
  // document.forms, images, links, anchors, embeds and scripts: each the same object every time.
  Quanta::Object* specialCollections[6] = {};
  // The context the document was made in, which the algorithms that must queue a microtask need.
  Quanta::Context* context = nullptr;
  // document.getSelection(): the same object each time.
  Quanta::Object* selection = nullptr;
  // The window this document is the document of, which an event on the document goes on to; null for any other.
  web::JsEventTarget* window = nullptr;
  // The global object of that window, and the iframe this document is the content of, if it is.
  Quanta::Object* globalObject = nullptr;
  Quanta::Embed::Realm* realm = nullptr;  // the realm of the window, which scripts of the document run in
  // A document the fragment parser makes for a window's elements: it has no window, but its elements are made as the
  // window's are, which is as custom elements if they are defined.
  bool customElementsEnabled = false;
  Element* frameElement = nullptr;
  // How many of the frames in it are still loading, and what to do when that has come to none after the
  // document is done being parsed: the load event waits for them.
  uint32_t pendingFrameLoads = 0;
  std::function<void()> whenFramesLoaded;
  // document.write while a script the parser runs is running: puts the markup in the parser's input.
  std::function<void(const std::string&)> parserInsert;
  // The parser document.open() made, which document.write feeds and document.close() ends.
  std::shared_ptr<void> scriptParser;
  // The throw-on-dynamic-markup-insertion counter: above zero, document.open, write and close throw.
  uint32_t throwOnDynamicMarkup = 0;

  // document[name]: the embeds, forms, iframes, images and objects the document has under that name.
  static bool NamedGetter(Quanta::Context& ctx, Document& self, const std::string& name, Quanta::Value& out);
  static std::vector<std::string> NamedKeys(Quanta::Context& ctx, Document& self);
  static constexpr bool LegacyOverrideBuiltIns = true;
  static constexpr bool LegacyUnenumerableNamedProperties = true;

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
enum class Interface { ShadowRoot, Document, Element, HtmlElement, Text, CdataSection, ProcessingInstruction, Comment, DocumentType, DocumentFragment, Attr, XmlDocument };
// What DefineClass made for `interface`; the DOM bindings record it, and a node made before they have
// has none.
void SetInterfacePrototype(Quanta::Context& ctx, Interface interface, Quanta::Object* prototype);
Quanta::Object* InterfacePrototype(Quanta::Context& ctx, Interface interface);

// Nodes are made in the realm of `ctx`, with the prototype that realm's DOM bindings gave the kind
// (none, if it has not any).
// A document; with `xmlInterface` one that is an XMLDocument, which is what the ones made as XML are.
Document* NewDocument(Quanta::Context& ctx, bool isHtml, bool xmlInterface = false);
// What the HTML module wants to know of the nodes that go into and come out of a tree (the iframes, mostly).
struct TreeHooks {
  void (*afterInsert)(Node* node) = nullptr;
  void (*afterRemove)(Node* node, bool parentWasConnected) = nullptr;
  void (*attributeChanged)(Element* element, const std::string& name) = nullptr;
};
void SetTreeHooks(const TreeHooks& hooks);
// Tells the hooks of an attribute an element was made with, as the parser makes them without going through the changes.
void NotifyParsedAttribute(Element* element, const std::string& name);

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
// HTMLUnknownElement's prototype, and whether `localName` is a name no built-in element has (and is no custom element's).
Quanta::Object* UnknownHtmlElementPrototype(Quanta::Context& ctx);
bool IsUnknownHtmlElementName(Quanta::Context& ctx, std::string_view localName);

// ---- Shadow trees ----
// "attach a shadow root" to `element`, which has to be one that can host one: DomError if it is not.
std::optional<DomError> AttachShadow(Quanta::Context& ctx, Element* element, ShadowMode mode, SlotAssignment slotAssignment, bool delegatesFocus, bool clonable, bool serializable,
                                     ShadowRoot*& out);
bool IsValidCustomElementName(std::string_view name);
// Whether `a` is, or has, `b` in its shadow-including tree: a shadow-including inclusive ancestor of b (the
// same as host-including).
bool IsShadowIncludingInclusiveAncestor(const Node* a, const Node* b);
// A node's root, but through shadow hosts: the root of the tree that hosts the shadow tree it is in.
Node* ShadowIncludingRoot(Node* node);
// "retarget": `a` as seen from `against`.
Node* Retarget(Node* a, Node* against);
// The shadow root a node is in the tree of, if it is in one.
ShadowRoot* ContainingShadowRoot(Node* node);
// Slots (https://dom.spec.whatwg.org/#finding-slots-and-slottables).
bool IsSlot(const Node* node);
bool IsSlottable(const Node* node);
std::string SlotName(const Element* slot);
Element* FindSlot(Node* slottable, bool open);
std::vector<Node*> FindFlattenedSlottables(Element* slot);
void AssignSlottablesForSlot(Element* slot);
void AssignSlottablesForTree(Node* root);
// What the tree algorithms tell the shadow trees (when there are any).
bool HasShadowTrees();
void ShadowAfterInsert(Node* node, Node* parent);
void ShadowAfterRemove(Node* node, Node* parent, Element* wasAssignedTo);
void ShadowAttributeChanged(Element* element, const std::string& name, const std::string& namespaceUri, const std::optional<std::string>& oldValue,
                            const std::optional<std::string>& newValue);
// The functions dispatch asks a node about its tree.
const web::EventTargetOps* NodeEventOps();

// The names a page's window gains and loses as its elements come and go, which the window's script follows.
void WindowNamesOf(const Node* subtree, std::vector<std::string>& names);
// Whether `element` is named `name` for window[name]: its id does, or its name does if it is one of the few that may be.
bool HasWindowName(const Element* element, const std::string& name);
// "named access on the Window object": a name that elements gain or lose, which the window's script follows.
void WindowNamesChanged(Quanta::Context& ctx, const std::string& name);
bool HasWindowNames();

// ---- The tree ----
// Each is the algorithm of that name in the standard. All but the last return the error the standard
// raises, if it does; none has changed the tree when it does.

// "ensure pre-insertion validity" for inserting `node` into `parent` before `child` (null: at the end).
std::optional<DomError> EnsurePreInsertionValidity(Node* node, Node* parent, Node* child);
// appendChild and insertBefore.
std::optional<DomError> PreInsert(Node* node, Node* parent, Node* child);
// "move a node" into `parent` before `child`: PreInsert for a node that is kept, and so one that already has a parent.
std::optional<DomError> MoveBefore(Node* node, Node* parent, Node* child);
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
// Character data counts and cuts in UTF-16 units, which these convert from and to the WTF-8 it is kept in.
std::u16string ToUtf16(std::string_view text);
std::string FromUtf16(std::u16string_view units);
uint32_t Utf16Length(std::string_view text);
// A node's length as the standard has it: its characters for character data, its children for the rest.
uint32_t NodeLength(const Node* node);
// "replace data": IndexSizeError if the offset is past the end. Live ranges and observers hear of it.
std::optional<DomError> ReplaceData(CharacterData* node, uint32_t offset, uint32_t count, std::string_view replacement);
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
