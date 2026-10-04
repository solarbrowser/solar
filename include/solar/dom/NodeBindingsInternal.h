#pragma once

#include <string>
#include <vector>

#include "quanta/Embed.h"
#include "solar/dom/Node.h"

namespace solar::dom {

// ---- What the binding files share ----

// The node `thisValue` is, or null with a TypeError pending ("Illegal invocation").
Node* ThisNode(Quanta::Context& ctx, const Quanta::Value& thisValue);
// The same, for a receiver that has to be of one kind.
Element* ThisElement(Quanta::Context& ctx, const Quanta::Value& thisValue);
Document* ThisDocument(Quanta::Context& ctx, const Quanta::Value& thisValue);
CharacterData* ThisCharacterData(Quanta::Context& ctx, const Quanta::Value& thisValue);

// Raises `error` as the DOMException it names.
void Throw(Quanta::Context& ctx, const DomError& error);
// A node as a value; null for none.
Quanta::Value NodeValue(Node* node);
// An argument that has to be a Node: null, with a TypeError naming the call, if it is not.
Node* NodeArgument(Quanta::Context& ctx, Quanta::Embed::Args args, size_t index, const char* what);
// An optional DOMString argument that may also be null: the string, or nothing for null or undefined.
std::optional<std::string> NullableString(Quanta::Context& ctx, const Quanta::Value& value);
// Node.prototype, which the interfaces that inherit from Node are defined on.
Quanta::Object* NodePrototype(Quanta::Context& ctx);

// "converting nodes into a node": the strings among `args` become Text nodes, and several nodes a
// fragment. Null, with an exception pending, if one fails.
Node* ConvertNodesIntoNode(Quanta::Context& ctx, Quanta::Embed::Args args, Document* document);

// Defines on `prototype` the members that the standard's mixins give: ParentNode, ChildNode and
// NonDocumentTypeChildNode.
void DefineParentNode(Quanta::Object* prototype);
void DefineChildNode(Quanta::Object* prototype);
void DefineNonDocumentTypeChildNode(Quanta::Object* prototype);

// A tag name in the case the element reports it in: upper case on an HTML element in an HTML document.
std::string TagNameOf(const Element* element);

// ---- Collections ----
// NodeList and HTMLCollection: the children of a node, or the descendants that match, as the
// standard says they stay current with the tree. What they find is looked up again when the tree has
// changed since the last time.
struct JsCollection : Quanta::DOMObject {
  enum class Kind { ChildNodes, Children, ByTagName, ByTagNameNS, ByClassName, ByName, WindowNamed, Static };
  Kind kind = Kind::Static;
  bool isNodeList = false;  // NodeList and not HTMLCollection
  Node* root = nullptr;
  std::string name;  // the qualified name, or the local name, or the class names
  std::string ns;
  std::vector<Node*> items;
  uint64_t version = 0;

  const std::vector<Node*>& Items();

  // list[0] and, for an HTMLCollection, collection["id"], which the engine asks for by these names.
  static bool IndexedGetter(Quanta::Context& ctx, JsCollection& self, uint32_t index, Quanta::Value& out);
  static uint32_t IndexedLength(Quanta::Context& ctx, JsCollection& self);
  static bool NamedGetter(Quanta::Context& ctx, JsCollection& self, const std::string& name, Quanta::Value& out);
  static std::vector<std::string> NamedKeys(Quanta::Context& ctx, JsCollection& self);
  static constexpr bool LegacyUnenumerableNamedProperties = true;

  void Visit(Quanta::Visitor& visitor) {
    visitor.Mark(root);
    for (Node* item : items) visitor.Mark(item);
  }
};

JsCollection* NewCollection(Quanta::Context& ctx, JsCollection::Kind kind, Node* root, bool isNodeList);
JsCollection* NewStaticNodeList(Quanta::Context& ctx, std::vector<Node*> nodes);
void DefineCollectionClasses(Quanta::Context& ctx);
// getElementsByTagName and friends, on an element or a document.
Quanta::Value GetElementsByTagName(Quanta::Context& ctx, Node* root, std::string_view name);
Quanta::Value GetElementsByTagNameNS(Quanta::Context& ctx, Node* root, std::string_view ns, std::string_view local);
// The elements window[name] is when more than one has the name.
Quanta::Value NewWindowNamedCollection(Quanta::Context& ctx, Node* root, std::string_view name);
Quanta::Value GetElementsByClassName(Quanta::Context& ctx, Node* root, std::string_view classes);
// document.getElementsByName: a live NodeList of the elements whose name attribute is `name`.
Quanta::Value GetElementsByName(Quanta::Context& ctx, Node* root, std::string_view name);

// The Document that nodes made by script constructors (new Text, new DocumentFragment) belong to: the
// realm's window document, which the browser sets, or an empty HTML document made on first use.
Document* AssociatedDocument(Quanta::Context& ctx);
void SetAssociatedDocument(Quanta::Context& ctx, Document* document);

// element.attributes, and element.classList (or another token list over `attribute`).
Quanta::Object* NewNamedNodeMap(Quanta::Context& ctx, Element* element);
Quanta::Object* NewTokenList(Quanta::Context& ctx, Element* element, const char* attribute);
void DefineAttributeClasses(Quanta::Context& ctx);

// The object, out of the page's reach, that keeps a realm's own things where the collector sees them.
Quanta::Object* RealmHolder(Quanta::Context& ctx);

// The interfaces, each defined once per realm.
void DefineNodeClass(Quanta::Context& ctx);
void DefineCharacterDataClasses(Quanta::Context& ctx);
void DefineElementClass(Quanta::Context& ctx);
void DefineDocumentClasses(Quanta::Context& ctx);
void DefineShadowClasses(Quanta::Context& ctx);

}  // namespace solar::dom
