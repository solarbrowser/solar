/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <cstdio>
#include <type_traits>
#include <string>

#include "solar/dom/CustomElements.h"
#include "solar/dom/NodeBindingsInternal.h"
#include "solar/html/HtmlBindings.h"
#include "solar/dom/Range.h"
#include "solar/html/Errors.h"
#include "solar/html/Frames.h"
#include "solar/html/Modules.h"
#include <algorithm>
#include <fstream>
#include "solar/html/Parser.h"
#include "solar/html/Serializer.h"
#include "solar/html/Xml.h"
#include "solar/web/Scripts.h"

namespace solar::layout {
void InstallLayoutNatives(Quanta::Context& ctx);
}

namespace solar::html {

namespace qe = Quanta::Embed;

const char* FormScript();  // JsForms.cpp
const char* ReflectScript();  // JsReflect.cpp
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Heap;
using Quanta::Object;
using Quanta::Value;

namespace {

char g_domParserKey;
char g_xmlSerializerKey;

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 0x20);
  }
  return out;
}

bool Missing(Context& ctx, qe::Args args, size_t count, const char* interface, const char* member) {
  if (args.size() >= count) return false;
  qe::ThrowTypeError(ctx, std::string("Failed to execute '") + member + "' on '" + interface + "': " + std::to_string(count) + " argument" + (count == 1 ? "" : "s") +
                              " required, but only " + std::to_string(args.size()) + " present.");
  return true;
}

// A DOMString that treats null as empty ([LegacyNullToEmptyString]).
std::optional<std::string> ReadMarkup(Context& ctx, qe::Args args) {
  if (args.empty()) {
    qe::ThrowTypeError(ctx, "1 argument required, but only 0 present.");
    return std::nullopt;
  }
  if (qe::IsNull(args[0])) return std::string();
  std::string text = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return std::nullopt;
  return text;
}

// The element a fragment is parsed for, and the node its result goes into: a shadow root stands for its host, and a
// template for its contents.
dom::Element* FragmentContext(dom::Node* node) {
  if (dom::Element* element = dom::AsElement(node)) return element;
  if (node && node->IsFragment() && static_cast<dom::DocumentFragment*>(node)->isShadowRoot) return static_cast<dom::ShadowRoot*>(node)->host;
  return nullptr;
}

dom::Node* FragmentTarget(dom::Node* node) {
  dom::Element* element = dom::AsElement(node);
  if (element && element->namespaceUri == dom::kHtmlNamespace && element->localName == "template" && element->templateContents) return element->templateContents;
  return node;
}

bool IsXmlNode(const dom::Node* node) {
  const dom::Document* document = node->IsDocument() ? static_cast<const dom::Document*>(node) : node->nodeDocument;
  return document && !document->isHtml;
}

Value GetInnerHtml(Context& ctx, Value t, qe::Args, Value) {
  dom::Node* self = dom::ThisNode(ctx, t);
  if (!self) return qe::Undefined();
  if (IsXmlNode(self)) {
    const std::optional<std::string> markup = SerializeXmlChildren(FragmentTarget(self), true);
    if (!markup) {
      dom::Throw(ctx, {"InvalidStateError", "The element could not be serialized as well-formed XML."});
      return qe::Undefined();
    }
    return qe::FromWtf8(ctx, *markup);
  }
  return qe::FromWtf8(ctx, SerializeChildren(FragmentTarget(self)));
}

// The fragment of `markup` in the context, by the parser of the document's kind: nothing, with a SyntaxError raised,
// if XML is not well-formed.
dom::DocumentFragment* ParseFragmentFor(Context& ctx, dom::Element* context, const std::string& markup, bool unsafe = false) {
  if (context && context->nodeDocument && !context->nodeDocument->isHtml) {
    dom::DocumentFragment* fragment = dom::NewDocumentFragment(ctx, context->nodeDocument);
    const XmlResult result = ParseXmlFragment(ctx, context, markup, fragment);
    if (!result.ok) {
      dom::Throw(ctx, {"SyntaxError", "The markup is not well-formed XML: " + result.error});
      return nullptr;
    }
    return fragment;
  }
  return ParseFragment(ctx, context, markup, ScriptingMode::Inert, nullptr, unsafe);
}

Value SetInnerHtml(Context& ctx, Value t, qe::Args args, Value) {
  dom::Node* self = dom::ThisNode(ctx, t);
  if (!self) return qe::Undefined();
  const std::optional<std::string> markup = ReadMarkup(ctx, args);
  if (!markup) return qe::Undefined();
  dom::DocumentFragment* fragment = ParseFragmentFor(ctx, FragmentContext(self), *markup);
  if (!fragment) return qe::Undefined();
  dom::ReplaceAll(FragmentTarget(self), fragment);
  return qe::Undefined();
}

Value SetHtmlUnsafe(Context& ctx, Value t, qe::Args args, Value) {
  dom::Node* self = dom::ThisNode(ctx, t);
  if (!self) return qe::Undefined();
  if (Missing(ctx, args, 1, "Element", "setHTMLUnsafe")) return qe::Undefined();
  const std::string markup = qe::IsNull(args[0]) ? std::string() : qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  dom::DocumentFragment* fragment = ParseFragment(ctx, FragmentContext(self), markup, ScriptingMode::Inert, nullptr, true);
  dom::ReplaceAll(FragmentTarget(self), fragment);
  return qe::Undefined();
}

Value GetHtml(Context& ctx, Value t, qe::Args args, Value) {
  dom::Node* self = dom::ThisNode(ctx, t);
  if (!self) return qe::Undefined();
  bool serializable = false;
  std::vector<const dom::ShadowRoot*> roots;
  if (!args.empty() && qe::IsObject(args[0])) {
    serializable = qe::Get(ctx, args[0], "serializableShadowRoots").to_boolean();
    if (qe::HasException(ctx)) return qe::Undefined();
    Value list = qe::Get(ctx, args[0], "shadowRoots");
    if (qe::HasException(ctx)) return qe::Undefined();
    if (!qe::IsUndefined(list)) {
      const uint32_t length = qe::ToUint32(ctx, qe::Get(ctx, list, "length"));
      for (uint32_t i = 0; i < length && !qe::HasException(ctx); ++i) {
        dom::Node* node = DOMObject::Cast<dom::Node>(qe::Get(ctx, list, qe::FromWtf8(ctx, std::to_string(i))));
        if (qe::HasException(ctx)) return qe::Undefined();
        const dom::ShadowRoot* root = node && node->IsFragment() && static_cast<dom::DocumentFragment*>(node)->isShadowRoot ? static_cast<dom::ShadowRoot*>(node) : nullptr;
        if (!root) {
          qe::ThrowTypeError(ctx, "Failed to execute 'getHTML': member shadowRoots is not of type ShadowRoot.");
          return qe::Undefined();
        }
        roots.push_back(root);
      }
      if (qe::HasException(ctx)) return qe::Undefined();
    }
  }
  return qe::FromWtf8(ctx, SerializeChildrenWithShadowRoots(FragmentTarget(self), serializable, roots));
}

// Range.createContextualFragment(markup): the markup parsed as it would be in the element the range starts in, with
// its scripts not yet started, so that they run when the fragment is put in a document.
Value CreateContextualFragment(Context& ctx, Value t, qe::Args args, Value) {
  dom::Range* range = Quanta::DOMObject::Cast<dom::Range>(t);
  if (!range) {
    qe::ThrowTypeError(ctx, "Illegal invocation");
    return qe::Undefined();
  }
  if (Missing(ctx, args, 1, "Range", "createContextualFragment")) return qe::Undefined();
  const std::string markup = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  dom::Node* node = range->startNode;
  dom::Element* context = nullptr;
  if (dom::Element* element = dom::AsElement(node)) context = element;
  else if (node && !node->IsDocument() && !node->IsFragment()) context = dom::AsElement(node->parentNode);
  if (!context || (context->IsHtml("html") && context->nodeDocument && context->nodeDocument->isHtml)) {
    dom::Document* owner = node && node->IsDocument() ? static_cast<dom::Document*>(node) : node ? node->nodeDocument : nullptr;
    context = dom::NewElement(ctx, owner, "body", dom::kHtmlNamespace);
  }
  dom::DocumentFragment* fragment = ParseFragment(ctx, context, markup, ScriptingMode::Fragment);
  for (dom::Node* n = fragment->firstChild; n; n = n->NextInTree(fragment)) {
    if (dom::Element* element = dom::AsElement(n); element && element->IsHtml("script")) element->scriptStarted = false;
  }
  return qe::FromObject(fragment);
}

Value ParseHtmlUnsafe(Context& ctx, Value, qe::Args args, Value) {
  if (Missing(ctx, args, 1, "Document", "parseHTMLUnsafe")) return qe::Undefined();
  const std::string markup = qe::IsNull(args[0]) ? std::string() : qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  dom::Document* document = dom::NewDocument(ctx, true);
  document->contentType = "text/html";
  ParseDocument(ctx, document, markup, ScriptingMode::Disabled, nullptr, nullptr, true);
  return qe::FromObject(document);
}

Value GetOuterHtml(Context& ctx, Value t, qe::Args, Value) {
  dom::Element* self = dom::ThisElement(ctx, t);
  if (!self) return qe::Undefined();
  if (IsXmlNode(self)) {
    const std::optional<std::string> markup = SerializeXml(self, true);
    if (!markup) {
      dom::Throw(ctx, {"InvalidStateError", "The element could not be serialized as well-formed XML."});
      return qe::Undefined();
    }
    return qe::FromWtf8(ctx, *markup);
  }
  return qe::FromWtf8(ctx, SerializeNode(self));
}

Value SetOuterHtml(Context& ctx, Value t, qe::Args args, Value) {
  dom::Element* self = dom::ThisElement(ctx, t);
  if (!self) return qe::Undefined();
  const std::optional<std::string> markup = ReadMarkup(ctx, args);
  if (!markup) return qe::Undefined();
  dom::Node* parent = self->parentNode;
  if (!parent) return qe::Undefined();
  if (parent->IsDocument()) {
    dom::Throw(ctx, {"NoModificationAllowedError", "Failed to set the 'outerHTML' property on 'Element': This element's parent is of type '#document', which is not an element node."});
    return qe::Undefined();
  }
  dom::Element* context = dom::AsElement(parent);
  if (!context) {
    // A fragment is no context: the body element stands for it.
    context = dom::NewElement(ctx, self->nodeDocument, "body", dom::kHtmlNamespace);
  }
  dom::DocumentFragment* fragment = ParseFragmentFor(ctx, context, *markup);
  if (!fragment) return qe::Undefined();
  if (auto error = dom::ReplaceChild(parent, fragment, self)) dom::Throw(ctx, *error);
  return qe::Undefined();
}

Value InsertAdjacentHtml(Context& ctx, Value t, qe::Args args, Value) {
  dom::Element* self = dom::ThisElement(ctx, t);
  if (!self || Missing(ctx, args, 2, "Element", "insertAdjacentHTML")) return qe::Undefined();
  const std::string position = Lower(qe::ToWtf8(ctx, args[0]));
  const std::string markup = qe::HasException(ctx) ? "" : qe::ToWtf8(ctx, args[1]);
  if (qe::HasException(ctx)) return qe::Undefined();
  dom::Node* contextNode = nullptr;
  if (position == "beforebegin" || position == "afterend") {
    contextNode = self->parentNode;
    if (!contextNode || contextNode->IsDocument()) {
      dom::Throw(ctx, {"NoModificationAllowedError", "The element has no parent that can be modified."});
      return qe::Undefined();
    }
  } else if (position == "afterbegin" || position == "beforeend") {
    contextNode = self;
  } else {
    dom::Throw(ctx, {"SyntaxError", "The value provided ('" + position + "') is not one of 'beforebegin', 'afterbegin', 'beforeend', or 'afterend'."});
    return qe::Undefined();
  }
  dom::Element* context = dom::AsElement(contextNode);
  if (!context || (context->namespaceUri == dom::kHtmlNamespace && context->localName == "html" && context->nodeDocument && context->nodeDocument->isHtml)) {
    context = dom::NewElement(ctx, self->nodeDocument, "body", dom::kHtmlNamespace);
  }
  dom::DocumentFragment* fragment = ParseFragmentFor(ctx, context, markup);
  if (!fragment) return qe::Undefined();
  std::optional<dom::DomError> error;
  if (position == "beforebegin") error = dom::PreInsert(fragment, self->parentNode, self);
  else if (position == "afterbegin") error = dom::PreInsert(fragment, self, self->firstChild);
  else if (position == "beforeend") error = dom::PreInsert(fragment, self, nullptr);
  else error = dom::PreInsert(fragment, self->parentNode, self->nextSibling);
  if (error) dom::Throw(ctx, *error);
  return qe::Undefined();
}

// ---- DOMParser ----

struct JsDomParser : DOMObject {
  void Visit(Quanta::Visitor&) {}
};

Value ConstructDomParser(Context& ctx, Value, qe::Args, Value newTarget) {
  if (qe::IsUndefined(newTarget)) {
    qe::ThrowTypeError(ctx, "Failed to construct 'DOMParser': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    return qe::Undefined();
  }
  Object* prototype = qe::PrototypeFromNewTarget(ctx, newTarget);
  if (qe::HasException(ctx)) return qe::Undefined();
  JsDomParser* parser = Heap::Allocate<JsDomParser>();
  parser->initialize_prototype(prototype ? prototype : static_cast<Object*>(qe::GetRealmData(ctx, &g_domParserKey)));
  return qe::FromObject(parser);
}

Value ParseFromString(Context& ctx, Value t, qe::Args args, Value) {
  if (!DOMObject::Cast<JsDomParser>(t)) {
    qe::ThrowTypeError(ctx, "Illegal invocation");
    return qe::Undefined();
  }
  if (Missing(ctx, args, 2, "DOMParser", "parseFromString")) return qe::Undefined();
  const std::string markup = qe::ToWtf8(ctx, args[0]);
  const std::string type = qe::HasException(ctx) ? "" : qe::ToWtf8(ctx, args[1]);
  if (qe::HasException(ctx)) return qe::Undefined();
  if (type == "text/html") {
    dom::Document* document = dom::NewDocument(ctx, true);
    document->contentType = "text/html";
    if (dom::Document* current = dom::AssociatedDocument(ctx)) document->url = current->url;
    ParseDocument(ctx, document, markup, ScriptingMode::Disabled, nullptr, nullptr, false);
    return qe::FromObject(document);
  }
  if (type == "text/xml" || type == "application/xml" || type == "application/xhtml+xml" || type == "image/svg+xml") {
    dom::Document* document = dom::NewDocument(ctx, false, true);
    document->contentType = type;
    if (dom::Document* current = dom::AssociatedDocument(ctx)) document->url = current->url;
    const XmlResult result = ParseXmlDocument(ctx, document, markup);
    if (!result.ok) {
      // A document that says what was wrong with the markup, in the element the browsers make for it.
      while (document->firstChild) dom::RemoveUnchecked(document->firstChild);
      dom::Element* error = dom::NewElement(ctx, document, "parsererror", "http://www.mozilla.org/newlayout/xml/parsererror.xml");
      dom::AppendChild(error, dom::NewText(ctx, document, result.error));
      dom::AppendChild(document, error);
    }
    return qe::FromObject(document);
  }
  qe::ThrowTypeError(ctx, "Failed to execute 'parseFromString' on 'DOMParser': The provided value '" + type + "' is not a valid enum value of type SupportedType.");
  return qe::Undefined();
}

// document.forms and the like: the same collection each time, as the standard has it.
template <const char* Which, int Index>
Value GetDocumentCollection(Context& ctx, Value t, qe::Args, Value) {
  dom::Document* self = dom::ThisDocument(ctx, t);
  if (!self) return qe::Undefined();
  if (!self->specialCollections[Index]) {
    Value collection = dom::GetSpecialCollection(ctx, self, Which);
    self->specialCollections[Index] = collection.as_object();
    self->NoteWrite();
  }
  return qe::FromObject(self->specialCollections[Index]);
}

constexpr char kForms[] = "forms";
constexpr char kImages[] = "images";
constexpr char kLinks[] = "links";
constexpr char kAnchors[] = "anchors";
constexpr char kEmbeds[] = "embeds";
constexpr char kScripts[] = "scripts";

// ---- XMLSerializer ----

struct JsXmlSerializer : DOMObject {
  void Visit(Quanta::Visitor&) {}
};

Value ConstructXmlSerializer(Context& ctx, Value, qe::Args, Value newTarget) {
  if (qe::IsUndefined(newTarget)) {
    qe::ThrowTypeError(ctx, "Failed to construct 'XMLSerializer': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    return qe::Undefined();
  }
  Object* prototype = qe::PrototypeFromNewTarget(ctx, newTarget);
  if (qe::HasException(ctx)) return qe::Undefined();
  JsXmlSerializer* serializer = Heap::Allocate<JsXmlSerializer>();
  serializer->initialize_prototype(prototype ? prototype : static_cast<Object*>(qe::GetRealmData(ctx, &g_xmlSerializerKey)));
  return qe::FromObject(serializer);
}

Value SerializeToString(Context& ctx, Value t, qe::Args args, Value) {
  if (!DOMObject::Cast<JsXmlSerializer>(t)) {
    qe::ThrowTypeError(ctx, "Illegal invocation");
    return qe::Undefined();
  }
  if (Missing(ctx, args, 1, "XMLSerializer", "serializeToString")) return qe::Undefined();
  dom::Node* node = DOMObject::Cast<dom::Node>(args[0]);
  if (!node) {
    qe::ThrowTypeError(ctx, "Failed to execute 'serializeToString' on 'XMLSerializer': parameter 1 is not of type 'Node'.");
    return qe::Undefined();
  }
  // An HTML document is serialized as the HTML syntax would not do: as XML, with the HTML namespace.
  const std::optional<std::string> markup = SerializeXml(node, false);
  if (!markup) {
    dom::Throw(ctx, {"InvalidStateError", "The node could not be serialized."});
    return qe::Undefined();
  }
  return qe::FromWtf8(ctx, *markup);
}

// ---- What the HTML Standard adds to Document ----

bool IsHtml(const dom::Node* node, std::string_view name) {
  const dom::Element* element = dom::AsElement(node);
  return element && element->namespaceUri == dom::kHtmlNamespace && element->localName == name;
}

dom::Element* HeadOf(dom::Document* document) {
  dom::Element* root = document->DocumentElement();
  if (!root) return nullptr;
  for (dom::Node* child = root->firstChild; child; child = child->nextSibling) {
    if (IsHtml(child, "head")) return static_cast<dom::Element*>(child);
  }
  return nullptr;
}

dom::Element* BodyOf(dom::Document* document) {
  dom::Element* root = document->DocumentElement();
  if (!root || !IsHtml(root, "html")) return nullptr;
  for (dom::Node* child = root->firstChild; child; child = child->nextSibling) {
    if (IsHtml(child, "body") || IsHtml(child, "frameset")) return static_cast<dom::Element*>(child);
  }
  return nullptr;
}

dom::Element* TitleElementOf(dom::Document* document) {
  for (dom::Node* node = document->NextInTree(document); node; node = node->NextInTree(document)) {
    if (IsHtml(node, "title")) return static_cast<dom::Element*>(node);
  }
  return nullptr;
}

Value GetHead(Context& ctx, Value t, qe::Args, Value) {
  dom::Document* self = dom::ThisDocument(ctx, t);
  return self ? dom::NodeValue(HeadOf(self)) : qe::Undefined();
}

Value GetBody(Context& ctx, Value t, qe::Args, Value) {
  dom::Document* self = dom::ThisDocument(ctx, t);
  return self ? dom::NodeValue(BodyOf(self)) : qe::Undefined();
}

Value SetBody(Context& ctx, Value t, qe::Args args, Value) {
  dom::Document* self = dom::ThisDocument(ctx, t);
  if (!self) return qe::Undefined();
  dom::Node* value = args.empty() || qe::IsNull(args[0]) ? nullptr : DOMObject::Cast<dom::Node>(args[0]);
  if (!value || !(IsHtml(value, "body") || IsHtml(value, "frameset"))) {
    dom::Throw(ctx, {"HierarchyRequestError", "The new body must be a body or frameset element."});
    return qe::Undefined();
  }
  dom::Element* current = BodyOf(self);
  if (current == value) return qe::Undefined();
  std::optional<dom::DomError> error;
  if (current) error = dom::ReplaceChild(current->parentNode, value, current);
  else if (!self->DocumentElement()) error = dom::DomError{"HierarchyRequestError", "There is no document element to put the body in."};
  else error = dom::AppendChild(self->DocumentElement(), value);
  if (error) dom::Throw(ctx, *error);
  return qe::Undefined();
}

// Strips and collapses ASCII whitespace, as the title does.
std::string CollapseWhitespace(const std::string& text) {
  std::string out;
  bool pending = false;
  for (char c : text) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r') {
      pending = !out.empty();
    } else {
      if (pending) out += ' ';
      pending = false;
      out += c;
    }
  }
  return out;
}

Value GetTitle(Context& ctx, Value t, qe::Args, Value) {
  dom::Document* self = dom::ThisDocument(ctx, t);
  if (!self) return qe::Undefined();
  const dom::Element* title = TitleElementOf(self);
  return qe::FromWtf8(ctx, title ? CollapseWhitespace(title->DescendantText()) : "");
}

Value SetTitle(Context& ctx, Value t, qe::Args args, Value) {
  dom::Document* self = dom::ThisDocument(ctx, t);
  if (!self || Missing(ctx, args, 1, "Document", "title")) return qe::Undefined();
  const std::string text = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  dom::Element* title = TitleElementOf(self);
  if (!title) {
    dom::Element* head = HeadOf(self);
    if (!head || !IsHtml(self->DocumentElement(), "html")) return qe::Undefined();
    title = dom::NewElement(ctx, self, "title", dom::kHtmlNamespace);
    dom::InsertUnchecked(title, head, nullptr);
  }
  dom::SetTextContent(ctx, title, text);
  return qe::Undefined();
}

Value GetReadyState(Context& ctx, Value t, qe::Args, Value) {
  dom::Document* self = dom::ThisDocument(ctx, t);
  return self ? qe::FromWtf8(ctx, self->readyState) : qe::Undefined();
}

Value GetCurrentScript(Context& ctx, Value t, qe::Args, Value) {
  dom::Document* self = dom::ThisDocument(ctx, t);
  return self ? dom::NodeValue(self->currentScript) : qe::Undefined();
}

// The window of a document that has one: the global object of this realm.
Value GetDefaultView(Context& ctx, Value t, qe::Args, Value) {
  dom::Document* self = dom::ThisDocument(ctx, t);
  if (!self) return qe::Undefined();
  return self->window && self->globalObject ? qe::FromObject(self->globalObject) : qe::Null();
}

// __solarDocument() and __solarSetWindow(target): what the window's script is made of.
Value GlobalDocument(Context& ctx, Value, qe::Args, Value) { return qe::FromObject(dom::AssociatedDocument(ctx)); }

// __solarSetHelper(name, fn): a function of the window's script that the program calls into, kept by name on the
// realm's holder (windowNamedUpdate, fireEvent, fireFocus, reportError, reportRejection, contentHandler).
Value SetHelper(Context& ctx, Value, qe::Args args, Value) {
  if (Object* holder = dom::RealmHolder(ctx); holder && args.size() >= 2) {
    const std::string name = qe::ToWtf8(ctx, args[0]);
    if (!qe::HasException(ctx)) qe::Set(ctx, qe::FromObject(holder), name, args[1]);
  }
  return qe::Undefined();
}

// __solarFormChanged(element): what a form control shows changed (its value, whether it is checked): the page has to be drawn again.
Value FormChanged(Context& ctx, Value, qe::Args args, Value) {
  if (!args.empty()) {
    if (dom::Element* element = dom::ThisElement(ctx, args[0]); element && element->nodeDocument) {
      element->nodeDocument->NoteWrite();
      dom::MarkControlDirty(element);
      dom::NoteTreeChange();
    }
  }
  return qe::Undefined();
}

// __solarNavigate(url, replace) and __solarSubmitPost(url, body, type): the window goes to another document.
Value NavigateTo(Context& ctx, Value, qe::Args args, Value) {
  dom::Document* document = dom::AssociatedDocument(ctx);
  if (document && !args.empty()) RequestNavigation(document, qe::ToWtf8(ctx, args[0]), args.size() > 1 && qe::ToBoolean(args[1]));
  return qe::Undefined();
}
Value SubmitPost(Context& ctx, Value, qe::Args args, Value) {
  dom::Document* document = dom::AssociatedDocument(ctx);
  if (document && args.size() >= 3) RequestNavigation(document, qe::ToWtf8(ctx, args[0]), false, "POST", qe::ToWtf8(ctx, args[1]), qe::ToWtf8(ctx, args[2]));
  return qe::Undefined();
}

// __solarSetDocumentUrl(url): where a fragment navigation moves the page's address.
Value SetDocumentUrl(Context& ctx, Value, qe::Args args, Value) {
  dom::Document* document = dom::AssociatedDocument(ctx);
  if (document && !args.empty()) {
    document->url = qe::ToWtf8(ctx, args[0]);
    document->NoteWrite();
  }
  return qe::Undefined();
}

Value SetWindowTarget(Context& ctx, Value, qe::Args args, Value) {
  dom::Document* document = dom::AssociatedDocument(ctx);
  web::JsEventTarget* target = args.empty() ? nullptr : DOMObject::Cast<web::JsEventTarget>(args[0]);
  if (document && target) {
    document->window = target;
    document->NoteWrite();
    web::SetWindowEventTarget(ctx, target);
  }
  return qe::Undefined();
}

template <typename Host>
void Install(Host& host) {
  Context& ctx = host.GetContext();
  Object* element = dom::InterfacePrototype(ctx, dom::Interface::Element);
  qe::DefineAccessor(element, "innerHTML", GetInnerHtml, dom::Reactions<SetInnerHtml>);
  qe::DefineAccessor(element, "outerHTML", GetOuterHtml, dom::Reactions<SetOuterHtml>);
  qe::DefineMethod(element, "insertAdjacentHTML", dom::Reactions<InsertAdjacentHtml>, 2);
  qe::DefineMethod(element, "setHTMLUnsafe", dom::Reactions<SetHtmlUnsafe>, 1);
  qe::DefineMethod(element, "getHTML", GetHtml, 0);
  Object* shadowRoot = dom::InterfacePrototype(ctx, dom::Interface::ShadowRoot);
  qe::DefineAccessor(shadowRoot, "innerHTML", GetInnerHtml, dom::Reactions<SetInnerHtml>);
  qe::DefineMethod(shadowRoot, "setHTMLUnsafe", dom::Reactions<SetHtmlUnsafe>, 1);
  qe::DefineMethod(shadowRoot, "getHTML", GetHtml, 0);

  DefineHtmlElementInterfaces(ctx);
  DefineFrameNatives(ctx);
  DefineFocusMembers(ctx);
  DefineErrorNatives(ctx);
  DefineDocumentWriting(ctx);
  InstallFrameHooks();
  Object* document = dom::InterfacePrototype(ctx, dom::Interface::Document);
  qe::DefineAccessor(document, "head", GetHead, nullptr);
  qe::DefineAccessor(document, "body", GetBody, SetBody);
  qe::DefineAccessor(document, "title", GetTitle, SetTitle);
  qe::DefineAccessor(document, "readyState", GetReadyState, nullptr);
  qe::DefineAccessor(document, "forms", GetDocumentCollection<kForms, 0>, nullptr);
  qe::DefineAccessor(document, "images", GetDocumentCollection<kImages, 1>, nullptr);
  qe::DefineAccessor(document, "links", GetDocumentCollection<kLinks, 2>, nullptr);
  qe::DefineAccessor(document, "anchors", GetDocumentCollection<kAnchors, 3>, nullptr);
  qe::DefineAccessor(document, "embeds", GetDocumentCollection<kEmbeds, 4>, nullptr);
  qe::DefineAccessor(document, "plugins", GetDocumentCollection<kEmbeds, 4>, nullptr);
  qe::DefineAccessor(document, "scripts", GetDocumentCollection<kScripts, 5>, nullptr);
  qe::DefineAccessor(document, "currentScript", GetCurrentScript, nullptr);
  qe::DefineAccessor(document, "defaultView", GetDefaultView, nullptr);
  qe::DefineGlobalFunction(ctx, "__solarDocument", GlobalDocument, 0);
  qe::DefineGlobalFunction(ctx, "__solarSetWindow", SetWindowTarget, 1);
  qe::DefineGlobalFunction(ctx, "__solarSetDocumentUrl", SetDocumentUrl, 1);
  qe::DefineGlobalFunction(ctx, "__solarSetHelper", SetHelper, 2);
  qe::DefineGlobalFunction(ctx, "__solarFormChanged", FormChanged, 1);
  qe::DefineGlobalFunction(ctx, "__solarNavigate", NavigateTo, 2);
  qe::DefineGlobalFunction(ctx, "__solarSubmitPost", SubmitPost, 3);

  qe::DefineStaticMethod(qe::Get(ctx, qe::FromObject(dom::InterfacePrototype(ctx, dom::Interface::Document)), "constructor").as_object(), "parseHTMLUnsafe", ParseHtmlUnsafe, 1);
  Value rangePrototype = qe::Get(ctx, qe::Get(ctx, qe::FromObject(ctx.get_global_object()), "Range"), "prototype");
  if (qe::IsObject(rangePrototype)) qe::DefineMethod(rangePrototype.as_object(), "createContextualFragment", dom::Reactions<CreateContextualFragment>, 1);

  qe::ClassRef serializer = qe::DefineClass(ctx, "XMLSerializer", ConstructXmlSerializer, 0);
  qe::SetRealmData(ctx, &g_xmlSerializerKey, serializer.prototype);
  qe::DefineMethod(serializer.prototype, "serializeToString", SerializeToString, 1);
  qe::DefineGlobal(ctx, "XMLSerializer", serializer.constructor);

  qe::ClassRef parser = qe::DefineClass(ctx, "DOMParser", ConstructDomParser, 0);
  qe::SetRealmData(ctx, &g_domParserKey, parser.prototype);
  qe::DefineMethod(parser.prototype, "parseFromString", ParseFromString, 2);
  qe::DefineGlobal(ctx, "DOMParser", parser.constructor);
}

}  // namespace

namespace {

// A page's global object, in script: window and its other names for itself, the document, and the
// EventTarget methods the window has (an event on the document goes on to it).
// What pages take for granted of a browser, and a page that is not shown to anyone has little to say of: the history, storage (kept for the page only), cookies,
// the visibility of the document, random values and idle callbacks.
const char* const kPlatformScript = R"JS(
(function () {
  const define = (target, name, value) => Object.defineProperty(target, name, { value, writable: true, enumerable: false, configurable: true });
  const moveUrl = globalThis.__solarMoveUrl;
  delete globalThis.__solarMoveUrl;

  // ---- History ----
  class History {
    constructor(access) { access.history = this; }
  }
  const entries = [{ state: null, url: document.URL }];
  let index = 0;
  let scrollRestoration = "auto";
  const urlFor = (url) => {
    const target = new URL(url === undefined || url === null ? document.URL : String(url), document.URL);
    const here = new URL(document.URL);
    if (target.origin !== here.origin && !(here.protocol === "file:" && target.protocol === "file:")) throw new DOMException("A history state object with URL '" + target.href + "' cannot be created in a document with origin '" + here.origin + "'.", "SecurityError");
    return target.href;
  };
  const history = Object.create(History.prototype);
  Object.defineProperties(History.prototype, {
    length: { get() { return entries.length; }, enumerable: true, configurable: true },
    state: { get() { return entries[index].state; }, enumerable: true, configurable: true },
    scrollRestoration: { get() { return scrollRestoration; }, set(value) { if (value === "auto" || value === "manual") scrollRestoration = value; }, enumerable: true, configurable: true },
    pushState: { value(state, title, url) { const href = url === undefined ? entries[index].url : urlFor(url); entries.length = index + 1; entries.push({ state: state === undefined ? null : state, url: href }); index = entries.length - 1; moveUrl(href); }, writable: true, enumerable: true, configurable: true },
    replaceState: { value(state, title, url) { const href = url === undefined ? entries[index].url : urlFor(url); entries[index] = { state: state === undefined ? null : state, url: href }; moveUrl(href); }, writable: true, enumerable: true, configurable: true },
    go: { value(delta = 0) {
      const next = index + Math.trunc(Number(delta) || 0);
      if (delta === 0 || next < 0 || next >= entries.length) return;
      index = next;
      const entry = entries[index];
      setTimeout(() => { moveUrl(entry.url); window.dispatchEvent(new PopStateEvent("popstate", { state: entry.state })); }, 0);
    }, writable: true, enumerable: true, configurable: true },
    back: { value() { this.go(-1); }, writable: true, enumerable: true, configurable: true },
    forward: { value() { this.go(1); }, writable: true, enumerable: true, configurable: true },
  });
  // What the window's back and forward keys ask first: whether this page has an entry that way (then it moves there, and nothing else does).
  Object.defineProperty(globalThis, "__solarHistoryStep", { value: (delta) => { const next = index + delta; if (next < 0 || next >= entries.length) return false; history.go(delta); return true; }, writable: true, enumerable: false, configurable: true });
  define(globalThis, "History", History);
  Object.defineProperty(globalThis, "history", { value: history, writable: true, enumerable: true, configurable: true });
  if (typeof PopStateEvent === "undefined") {
    class PopStateEvent extends Event { constructor(type, init = {}) { super(type, init); this.state = init.state === undefined ? null : init.state; } }
    define(globalThis, "PopStateEvent", PopStateEvent);
  }

  // ---- Storage ----
  class Storage {
    constructor() { throw new TypeError("Illegal constructor"); }
  }
  const makeStorage = () => {
    const data = new Map();
    const target = Object.create(Storage.prototype);
    const api = {
      getItem: (key) => (data.has(String(key)) ? data.get(String(key)) : null),
      setItem: (key, value) => { data.set(String(key), String(value)); },
      removeItem: (key) => { data.delete(String(key)); },
      clear: () => { data.clear(); },
      key: (i) => { const keys = [...data.keys()]; return i >= 0 && i < keys.length ? keys[i] : null; },
    };
    return new Proxy(target, {
      get(object, key, receiver) {
        if (key === "length") return data.size;
        if (typeof key === "string" && key in api) return api[key];
        if (typeof key === "string" && data.has(key)) return data.get(key);
        return Reflect.get(object, key, receiver);
      },
      set(object, key, value) { if (typeof key === "string") data.set(key, String(value)); return true; },
      deleteProperty(object, key) { if (typeof key === "string") data.delete(key); return true; },
      has(object, key) { return (typeof key === "string" && (data.has(key) || key in api || key === "length")) || key in object; },
      ownKeys() { return [...data.keys()]; },
      getOwnPropertyDescriptor(object, key) { return typeof key === "string" && data.has(key) ? { value: data.get(key), writable: true, enumerable: true, configurable: true } : undefined; },
    });
  };
  for (const name of ["getItem", "setItem", "removeItem", "clear", "key"]) define(Storage.prototype, name, function () { throw new TypeError("Illegal invocation"); });
  define(globalThis, "Storage", Storage);
  Object.defineProperty(globalThis, "localStorage", { value: makeStorage(), writable: true, enumerable: true, configurable: true });
  Object.defineProperty(globalThis, "sessionStorage", { value: makeStorage(), writable: true, enumerable: true, configurable: true });

  // ---- Document state ----
  const cookies = new Map();
  const documentProto = Document.prototype;
  const accessors = {
    visibilityState: { get() { return "visible"; } },
    hidden: { get() { return false; } },
    webkitHidden: { get() { return false; } },
    webkitVisibilityState: { get() { return "visible"; } },
    referrer: { get() { return ""; } },
    fullscreenEnabled: { get() { return false; } },
    fullscreenElement: { get() { return null; } },
    pictureInPictureEnabled: { get() { return false; } },
    cookie: {
      get() { return [...cookies].map(([k, v]) => (v === "" ? k : k + "=" + v)).join("; "); },
      set(text) {
        const parts = String(text).split(";");
        const first = parts[0];
        const at = first.indexOf("=");
        const name = (at < 0 ? "" : first.slice(0, at)).trim(), value = (at < 0 ? first : first.slice(at + 1)).trim();
        const attributes = parts.slice(1).map((p) => p.trim().toLowerCase());
        const expired = attributes.some((a) => a.startsWith("max-age=") && Number(a.slice(8)) <= 0) || attributes.some((a) => a.startsWith("expires=") && Date.parse(a.slice(8)) < Date.now());
        if (expired) cookies.delete(name); else cookies.set(name, value);
      },
    },
  };
  for (const [name, accessor] of Object.entries(accessors)) {
    if (!Object.getOwnPropertyDescriptor(documentProto, name)) Object.defineProperty(documentProto, name, { ...accessor, enumerable: true, configurable: true });
  }
  if (!documentProto.exitFullscreen) define(documentProto, "exitFullscreen", function exitFullscreen() { return Promise.resolve(); });
  if (!documentProto.execCommand) define(documentProto, "execCommand", function execCommand() { return false; });
  if (!documentProto.queryCommandSupported) define(documentProto, "queryCommandSupported", function queryCommandSupported() { return false; });

  // ---- Random values, idle callbacks, beacons ----
  if (typeof crypto === "undefined") {
    const fill = (array) => {
      if (!ArrayBuffer.isView(array) || array instanceof Float32Array || array instanceof Float64Array) throw new TypeError("The data argument must be an integer-type TypedArray.");
      if (array.byteLength > 65536) throw new DOMException("The ArrayBufferView's byte length (" + array.byteLength + ") exceeds the number of bytes of entropy available via this API (65536).", "QuotaExceededError");
      const bytes = new Uint8Array(array.buffer, array.byteOffset, array.byteLength);
      for (let i = 0; i < bytes.length; i++) bytes[i] = Math.floor(Math.random() * 256);
      return array;
    };
    class Crypto {
      getRandomValues(array) { return fill(array); }
      randomUUID() {
        const b = new Uint8Array(16);
        fill(b);
        b[6] = (b[6] & 0x0f) | 0x40;
        b[8] = (b[8] & 0x3f) | 0x80;
        const hex = [...b].map((x) => x.toString(16).padStart(2, "0")).join("");
        return hex.slice(0, 8) + "-" + hex.slice(8, 12) + "-" + hex.slice(12, 16) + "-" + hex.slice(16, 20) + "-" + hex.slice(20);
      }
      get subtle() { return undefined; }
    }
    define(globalThis, "Crypto", Crypto);
    Object.defineProperty(globalThis, "crypto", { value: new Crypto(), writable: true, enumerable: true, configurable: true });
  }
  if (typeof requestIdleCallback === "undefined") {
    define(globalThis, "requestIdleCallback", function requestIdleCallback(callback) {
      const start = Date.now();
      return setTimeout(() => callback({ didTimeout: false, timeRemaining: () => Math.max(0, 50 - (Date.now() - start)) }), 1);
    });
    define(globalThis, "cancelIdleCallback", function cancelIdleCallback(handle) { clearTimeout(handle); });
  }
  if (typeof navigator !== "undefined") {
    if (!navigator.sendBeacon) define(navigator, "sendBeacon", function sendBeacon() { return true; });
    if (!("connection" in navigator)) Object.defineProperty(navigator, "connection", { value: Object.assign(new EventTarget(), { effectiveType: "4g", downlink: 10, rtt: 50, saveData: false }), configurable: true, enumerable: true });
    if (!("deviceMemory" in navigator)) Object.defineProperty(navigator, "deviceMemory", { value: 8, configurable: true, enumerable: true });
    if (!("clipboard" in navigator)) Object.defineProperty(navigator, "clipboard", { value: { writeText: () => Promise.reject(new DOMException("Write permission denied.", "NotAllowedError")), readText: () => Promise.reject(new DOMException("Read permission denied.", "NotAllowedError")) }, configurable: true, enumerable: true });
    if (!("mediaDevices" in navigator)) Object.defineProperty(navigator, "mediaDevices", { value: Object.assign(new EventTarget(), { enumerateDevices: () => Promise.resolve([]), getUserMedia: () => Promise.reject(new DOMException("Requested device not found", "NotFoundError")), getSupportedConstraints: () => ({}) }), configurable: true, enumerable: true });
    if (!("serviceWorker" in navigator)) Object.defineProperty(navigator, "serviceWorker", { value: Object.assign(new EventTarget(), { controller: null, ready: new Promise(() => {}), register: () => Promise.reject(new DOMException("Service workers are not supported.", "SecurityError")), getRegistrations: () => Promise.resolve([]) }), configurable: true, enumerable: true });
  }
  if (!("visualViewport" in globalThis)) Object.defineProperty(globalThis, "visualViewport", { get() { return Object.assign(new EventTarget(), { width: innerWidth, height: innerHeight, scale: 1, offsetLeft: 0, offsetTop: 0, pageLeft: scrollX, pageTop: scrollY }); }, configurable: true, enumerable: true });
})();
)JS";

const char* const kWindowScript = R"JS(
(function () {
  const target = new EventTarget();
  __solarSetWindow(target);
  const getDocument = __solarDocument;
  const setDocumentUrl = __solarSetDocumentUrl;
  const navigateTo = __solarNavigate;
  Object.defineProperty(globalThis, "__solarMoveUrl", { value: setDocumentUrl, configurable: true });
  const setHelper = __solarSetHelper;
  const parentWindow = __solarParent;
  const frameElement = __solarFrameElement;
  const childCount = __solarChildCount;
  const childWindow = __solarChildWindow;
  setHelper("fireEvent", (target, type) => target.dispatchEvent(new Event(type)));
  // The error event of the window for an exception nothing caught, and the rejection events for a promise. An error
  // in a handler of the error event is not reported again, as that would never end.
  let reporting = false;
  setHelper("reportError",
    (error, message, filename, lineno, colno) => {
      if (reporting) return false;
      reporting = true;
      try {
        const event = new ErrorEvent("error", { message, filename, lineno, colno, error, cancelable: true });
        target.dispatchEvent(event);
        return event.defaultPrevented;
      } finally {
        reporting = false;
      }
    });
  setHelper("reportRejection",
    (promise, reason, handled) => {
      const event = new PromiseRejectionEvent(handled ? "rejectionhandled" : "unhandledrejection", { promise, reason, cancelable: !handled });
      target.dispatchEvent(event);
      return event.defaultPrevented;
    });
  setHelper("fireFocus", (target, type, related, bubbles) => target.dispatchEvent(new FocusEvent(type, { bubbles, composed: true, relatedTarget: related, view: globalThis })));
  // The constructors of elements that HTML names as functions: new Image(width, height), new Audio(src), new Option(text, value, defaultSelected, selected).
  const defineGlobal = (name, value) => Object.defineProperty(globalThis, name, { value, writable: true, enumerable: false, configurable: true });
  const Image = function Image(width, height) {
    const element = getDocument().createElement("img");
    if (width !== undefined) element.setAttribute("width", String(width));
    if (height !== undefined) element.setAttribute("height", String(height));
    return element;
  };
  Image.prototype = globalThis.HTMLImageElement.prototype;
  defineGlobal("Image", Image);
  const Audio = function Audio(src) {
    const element = getDocument().createElement("audio");
    element.setAttribute("preload", "auto");
    if (src !== undefined) element.setAttribute("src", String(src));
    return element;
  };
  Audio.prototype = globalThis.HTMLAudioElement.prototype;
  defineGlobal("Audio", Audio);
  const Option = function Option(text, value, defaultSelected, selected) {
    const element = getDocument().createElement("option");
    if (text !== undefined) element.appendChild(getDocument().createTextNode(String(text)));
    if (value !== undefined) element.setAttribute("value", String(value));
    if (defaultSelected) element.setAttribute("selected", "");
    if (selected !== undefined) element.selected = !!selected;
    return element;
  };
  Option.prototype = globalThis.HTMLOptionElement.prototype;
  defineGlobal("Option", Option);
  // btoa and atob (https://infra.spec.whatwg.org/#forgiving-base64): a string of bytes and its base64.
  const base64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  defineGlobal("btoa", function btoa(data) {
    if (arguments.length === 0) throw new TypeError("btoa requires 1 argument");
    const input = String(data);
    let out = "";
    for (let i = 0; i < input.length; i += 3) {
      const a = input.charCodeAt(i), b = input.charCodeAt(i + 1), c = input.charCodeAt(i + 2);
      if (a > 255 || b > 255 || c > 255) throw new DOMException("The string to be encoded contains characters outside of the Latin1 range.", "InvalidCharacterError");
      const n = (a << 16) | ((b || 0) << 8) | (c || 0);
      out += base64[(n >> 18) & 63] + base64[(n >> 12) & 63] + (i + 1 < input.length ? base64[(n >> 6) & 63] : "=") + (i + 2 < input.length ? base64[n & 63] : "=");
    }
    return out;
  });
  defineGlobal("atob", function atob(data) {
    if (arguments.length === 0) throw new TypeError("atob requires 1 argument");
    let input = String(data).replace(/[\t\n\f\r ]/g, "");
    if (input.length % 4 === 0) input = input.replace(/==?$/, "");
    if (input.length % 4 === 1 || /[^A-Za-z0-9+/]/.test(input)) throw new DOMException("The string to be decoded is not correctly encoded.", "InvalidCharacterError");
    let out = "", bits = 0, value = 0;
    for (const ch of input) {
      value = (value << 6) | base64.indexOf(ch);
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        out += String.fromCharCode((value >> bits) & 255);
      }
    }
    return out;
  });
  // window.focus() and blur(): the window has the focus already, and has no other window to give it to.
  for (const name of ["focus", "blur"]) Object.defineProperty(globalThis, name, { value: function () {}, writable: true, enumerable: true, configurable: true });
  for (const name of ["__solarParent", "__solarFrameElement", "__solarChildCount", "__solarChildWindow"]) delete globalThis[name];
  delete globalThis.__solarSetWindow;
  delete globalThis.__solarDocument;
  delete globalThis.__solarSetDocumentUrl;
  delete globalThis.__solarNavigate;
  // window.location: the address of the document, as the parts of a URL. Going to another document is asked of the program.
  const url = () => new URL(getDocument().URL);
  const fragmentOnly = (next, replace) => {
    const current = url();
    let target;
    try { target = new URL(next, current); } catch (e) { throw new DOMException("Failed to parse the URL '" + next + "'", "SyntaxError"); }
    const withoutHash = (u) => { const copy = new URL(u.href); copy.hash = ""; return copy.href; };
    if (withoutHash(current) === withoutHash(target) && target.hash !== "") setDocumentUrl(target.href);
    else if (/^(https?|file):$/.test(target.protocol)) navigateTo(target.href, !!replace);
  };
  const location = {};
  for (const name of ["origin", "protocol", "host", "hostname", "port", "pathname", "search"]) {
    Object.defineProperty(location, name, { get() { return url()[name]; }, set(value) {}, enumerable: true, configurable: false });
  }
  Object.defineProperty(location, "href", { get() { return url().href; }, set(value) { fragmentOnly(String(value)); }, enumerable: true, configurable: false });
  Object.defineProperty(location, "hash", { get() { return url().hash; }, set(value) { const next = url(); next.hash = value; setDocumentUrl(next.href); }, enumerable: true, configurable: false });
  for (const name of ["assign", "replace"]) {
    Object.defineProperty(location, name, { value: function (next) { fragmentOnly(String(next), name === "replace"); }, writable: false, enumerable: true, configurable: false });
  }
  Object.defineProperty(location, "reload", { value: function () { navigateTo(url().href, true); }, writable: false, enumerable: true, configurable: false });
  Object.defineProperty(location, "toString", { value: function () { return url().href; }, writable: false, enumerable: true, configurable: false });
  Object.defineProperty(globalThis, "location", { get() { return location; }, set(value) { fragmentOnly(String(value)); }, enumerable: true, configurable: false });
  Object.defineProperty(Document.prototype, "location", { get() { return this === getDocument() ? location : null; }, set(value) { if (this === getDocument()) fragmentOnly(String(value)); }, enumerable: true, configurable: true });
  Object.defineProperty(globalThis, "document", { get: getDocument, set: undefined, enumerable: true, configurable: true });
  for (const name of ["window", "self", "frames"]) {
    Object.defineProperty(globalThis, name, { value: globalThis, writable: true, enumerable: true, configurable: true });
  }
  // The window this one is in, or itself for the top one; and the iframe it is the content of.
  Object.defineProperty(globalThis, "parent", { get() { return parentWindow() ?? globalThis; }, set: undefined, enumerable: true, configurable: true });
  Object.defineProperty(globalThis, "top", { get() { let w = globalThis; for (;;) { const p = w.parent; if (p === w) return w; w = p; } }, set: undefined, enumerable: true, configurable: true });
  Object.defineProperty(globalThis, "frameElement", { get() { return frameElement(); }, set: undefined, enumerable: true, configurable: true });
  Object.defineProperty(globalThis, "length", { get() { return childCount(); }, set: undefined, enumerable: true, configurable: true });
  // window[0] and the like, kept to the frames that are there.
  let indexedFrames = 0;
  setHelper("syncFrames", () => {
    const count = childCount();
    for (let i = count; i < indexedFrames; ++i) delete globalThis[i];
    for (let i = indexedFrames; i < count; ++i) Object.defineProperty(globalThis, i, { get() { return childWindow(i); }, set: undefined, enumerable: true, configurable: true });
    indexedFrames = count;
  });
  Object.defineProperty(globalThis, "opener", { value: null, writable: true, enumerable: true, configurable: true });
  Object.defineProperty(globalThis, "getSelection", { value: function getSelection() { return getDocument().getSelection(); }, writable: true, enumerable: true, configurable: true });
  // performance.now(), from the clock the page started on, and animation frames: there is no display to wait for,
  // so a frame is a 16 ms timer.
  const timeOrigin = Date.now();
  {
    // The performance timeline: marks and measures the page makes, and the navigation timing of a page that is not timed in more than its start.
    const now = () => Date.now() - timeOrigin;
    class PerformanceEntry {
      constructor(name, entryType, startTime, duration) { this.name = name; this.entryType = entryType; this.startTime = startTime; this.duration = duration; }
      toJSON() { return { name: this.name, entryType: this.entryType, startTime: this.startTime, duration: this.duration }; }
    }
    class PerformanceMark extends PerformanceEntry {
      constructor(name, options = {}) { super(String(name), "mark", options.startTime === undefined ? now() : Number(options.startTime), 0); this.detail = options.detail === undefined ? null : options.detail; }
    }
    class PerformanceMeasure extends PerformanceEntry {
      constructor(name, startTime, duration, detail) { super(name, "measure", startTime, duration); this.detail = detail === undefined ? null : detail; }
    }
    const entries = [];
    const observers = new Set();
    let flushing = false;
    const note = (entry) => {
      entries.push(entry);
      for (const observer of observers) if (observer.types.has(entry.entryType)) observer.queue.push(entry);
      // (Observers are told once the script that made the entry is done.)
      if (!flushing && observers.size) {
        flushing = true;
        queueMicrotask(() => { flushing = false; for (const observer of [...observers]) if (observer.queue.length) observer.flush(); });
      }
    };
    const timing = {};
    for (const name of ["navigationStart", "unloadEventStart", "unloadEventEnd", "redirectStart", "redirectEnd", "fetchStart", "domainLookupStart", "domainLookupEnd", "connectStart", "connectEnd", "secureConnectionStart",
                        "requestStart", "responseStart", "responseEnd", "domLoading", "domInteractive", "domContentLoadedEventStart", "domContentLoadedEventEnd", "domComplete", "loadEventStart", "loadEventEnd"]) {
      timing[name] = name === "unloadEventStart" || name === "unloadEventEnd" || name === "redirectStart" || name === "redirectEnd" || name === "secureConnectionStart" ? 0 : timeOrigin;
    }
    timing.toJSON = () => ({ ...timing });
    const navigationEntry = Object.assign(new PerformanceEntry(getDocument().URL, "navigation", 0, 0), { initiatorType: "navigation", type: "navigate", redirectCount: 0, transferSize: 0, encodedBodySize: 0, decodedBodySize: 0, nextHopProtocol: "h2", responseStart: 0, requestStart: 0, domInteractive: 0, domComplete: 0, loadEventStart: 0, loadEventEnd: 0, domContentLoadedEventStart: 0, domContentLoadedEventEnd: 0 });
    const timeFor = (value) => {
      if (value === undefined) return now();
      if (typeof value === "number") return value;
      const named = entries.slice().reverse().find((e) => e.entryType === "mark" && e.name === String(value));
      if (named) return named.startTime;
      if (String(value) in timing) return timing[String(value)] - timeOrigin;
      throw new DOMException("The mark '" + value + "' does not exist.", "SyntaxError");
    };
    class Performance extends EventTarget {
      get timeOrigin() { return timeOrigin; }
      get timing() { return timing; }
      get navigation() { return { type: 0, redirectCount: 0 }; }
      get memory() { return { jsHeapSizeLimit: 2172649472, totalJSHeapSize: 20000000, usedJSHeapSize: 16000000 }; }
      get eventCounts() { return new Map(); }
      now() { return now(); }
      mark(name, options) { const mark = new PerformanceMark(name, options); note(mark); return mark; }
      measure(name, startOrOptions, end) {
        let start = 0, finish = now(), detail;
        if (startOrOptions && typeof startOrOptions === "object") {
          if (startOrOptions.start !== undefined) start = timeFor(startOrOptions.start);
          if (startOrOptions.end !== undefined) finish = timeFor(startOrOptions.end);
          else if (startOrOptions.duration !== undefined) finish = start + startOrOptions.duration;
          detail = startOrOptions.detail;
        } else {
          if (startOrOptions !== undefined) start = timeFor(startOrOptions);
          if (end !== undefined) finish = timeFor(end);
        }
        const measure = new PerformanceMeasure(String(name), start, finish - start, detail);
        note(measure);
        return measure;
      }
      getEntries() { return entries.slice(); }
      getEntriesByType(type) { return type === "navigation" ? [navigationEntry] : entries.filter((e) => e.entryType === type); }
      getEntriesByName(name, type) { return entries.filter((e) => e.name === name && (type === undefined || e.entryType === type)); }
      clearMarks(name) { for (let i = entries.length - 1; i >= 0; i--) if (entries[i].entryType === "mark" && (name === undefined || entries[i].name === name)) entries.splice(i, 1); }
      clearMeasures(name) { for (let i = entries.length - 1; i >= 0; i--) if (entries[i].entryType === "measure" && (name === undefined || entries[i].name === name)) entries.splice(i, 1); }
      clearResourceTimings() {}
      setResourceTimingBufferSize() {}
      toJSON() { return { timeOrigin, timing: timing.toJSON() }; }
    }
    class PerformanceObserver {
      constructor(callback) { this.callback = callback; this.types = new Set(); this.queue = []; }
      observe(options = {}) {
        const types = options.entryTypes || (options.type ? [options.type] : []);
        for (const type of types) this.types.add(type);
        observers.add(this);
        if (options.buffered) for (const entry of entries) if (this.types.has(entry.entryType)) this.queue.push(entry);
        if (this.queue.length) queueMicrotask(() => this.flush());
      }
      flush() { const batch = this.queue.splice(0); if (batch.length && observers.has(this)) this.callback({ getEntries: () => batch, getEntriesByType: (t) => batch.filter((e) => e.entryType === t), getEntriesByName: (n) => batch.filter((e) => e.name === n) }, this); }
      disconnect() { observers.delete(this); this.queue = []; }
      takeRecords() { return this.queue.splice(0); }
      static get supportedEntryTypes() { return ["mark", "measure", "navigation", "resource"]; }
    }
    for (const [name, value] of [["PerformanceEntry", PerformanceEntry], ["PerformanceMark", PerformanceMark], ["PerformanceMeasure", PerformanceMeasure], ["Performance", Performance], ["PerformanceObserver", PerformanceObserver]]) {
      Object.defineProperty(globalThis, name, { value, writable: true, enumerable: false, configurable: true });
    }
    Object.defineProperty(globalThis, "performance", { value: new Performance(), writable: true, enumerable: true, configurable: true });
  }
  if (typeof navigator === "undefined") {
    const navigator = { userAgent: "Mozilla/5.0 (X11; Linux x86_64) Solar/1.0", appName: "Netscape", appVersion: "5.0 (X11)", appCodeName: "Mozilla", platform: "Linux x86_64", product: "Gecko", productSub: "20030107", vendor: "", vendorSub: "", language: "en-US", languages: Object.freeze(["en-US"]), onLine: true, cookieEnabled: true, hardwareConcurrency: 4, webdriver: false, maxTouchPoints: 0, doNotTrack: null, pdfViewerEnabled: false };
    Object.defineProperty(navigator, Symbol.toStringTag, { value: "Navigator", configurable: true });
    Object.defineProperty(globalThis, "navigator", { value: navigator, writable: true, enumerable: true, configurable: true });
  }
  const frames = new Map();
  let nextFrame = 1;
  let frameTimer = null;
  // What the animations do on a frame, before the callbacks run: update, finish the frame, and whether another is wanted.
  const animationFrame = { run: null, end: null, wants: null };
  const scheduleFrame = () => {
    if (frameTimer === null && !frameBusy) frameTimer = setTimeout(runFrame, 16);
  };
  const reportLater = (error) => setTimeout(() => { throw error; }, 0);
  let frameBusy = false;
  // Other steps of "update the rendering" (layout's: the events of content-visibility: auto), which run before the animations.
  const frameSteps = [];
  const frameEndSteps = [];
  function runFrame() {
    frameTimer = null;
    frameBusy = true;
    const now = performance.now();
    for (const step of frameSteps) { try { step(now); } catch (error) { reportLater(error); } }
    if (animationFrame.run) { try { animationFrame.run(now); } catch (error) { reportLater(error); } }
    // The promise jobs and the events of the animations have their turn before the callbacks do.
    setTimeout(() => runCallbacks(now), 0);
  }
  function runCallbacks(now) {
    const callbacks = [...frames.values()];
    frames.clear();
    for (const callback of callbacks) {
      try { callback(now); } catch (error) { reportLater(error); }
    }
    // The rest of "update the rendering", after the callbacks: observers of size and intersection.
    for (const step of frameEndSteps) { try { step(now); } catch (error) { reportLater(error); } }
    // Whether another frame is wanted is seen at the time of this one: the animations are running or not as of it.
    const more = frames.size > 0 || (animationFrame.wants && animationFrame.wants());
    if (animationFrame.end) animationFrame.end();
    frameBusy = false;
    if (more) scheduleFrame();
  }
  Object.defineProperty(globalThis, "__solarAddFrameEndStep", { value: function (step) { frameEndSteps.push(step); }, configurable: true });
  Object.defineProperty(globalThis, "__solarAddFrameStep", { value: function (step) { frameSteps.push(step); }, configurable: true });
  Object.defineProperty(globalThis, "__solarFrameHook", { value: function (run, end, wants) {
    animationFrame.run = run; animationFrame.end = end; animationFrame.wants = wants;
    return scheduleFrame;
  }, configurable: true });
  Object.defineProperty(globalThis, "requestAnimationFrame", { value: function requestAnimationFrame(callback) {
    if (typeof callback !== "function") throw new TypeError("Failed to execute 'requestAnimationFrame' on 'Window': The callback provided as parameter 1 is not a function.");
    const handle = nextFrame++;
    frames.set(handle, callback);
    scheduleFrame();
    return handle;
  }, writable: true, enumerable: true, configurable: true });
  Object.defineProperty(globalThis, "cancelAnimationFrame", { value: function cancelAnimationFrame(handle) {
    frames.delete(handle);
  }, writable: true, enumerable: true, configurable: true });
  for (const name of ["addEventListener", "removeEventListener", "dispatchEvent"]) {
    const method = EventTarget.prototype[name];
    Object.defineProperty(globalThis, name, { value: function (...args) { return method.apply(target, args); }, writable: true, enumerable: true, configurable: true });
  }
  // onload and the like: the handler is a listener on the window.
  const windowHandlerResets = [];
  setHelper("eraseWindowHandlers", () => { for (const reset of windowHandlerResets) reset(); });
  for (const type of ["load", "error", "message", "hashchange", "popstate", "pageshow", "pagehide", "unload", "beforeunload", "resize", "scroll", "focus", "blur"]) {
    let handler = null;
    let listener = null;
    windowHandlerResets.push(() => { handler = null; listener = null; });
    Object.defineProperty(globalThis, "on" + type, {
      get() { return handler; },
      set(value) {
        handler = typeof value === "function" ? value : null;
        if (!listener) {
          listener = (event) => {
            if (!handler) return;
            if (type === "error" && typeof ErrorEvent === "function" && event instanceof ErrorEvent) {
              // window.onerror is called with the parts of the error, and cancels the report by returning true.
              if (handler.call(globalThis, event.message, event.filename, event.lineno, event.colno, event.error) === true) event.preventDefault();
            } else {
              handler.call(globalThis, event);
            }
          };
          target.addEventListener(type, listener);
        }
      },
      enumerable: true,
      configurable: true,
    });
  }
})();
)JS";

// Interfaces and members a page tests for or calls and that this program has no behaviour behind: they exist so that
// the check passes the way it does in a browser that lacks the feature (a canvas that has no context, a media element
// that cannot play, a permission that is denied), and a page that feature-detects takes its other path.
const char* const kStubScript = R"JS(
(function () {
  const define = (target, name, value) => Object.defineProperty(target, name, { value, writable: true, enumerable: false, configurable: true });
  const defineGlobal = (name, value) => { if (typeof globalThis[name] === "undefined") define(globalThis, name, value); };
  const ensure = (target, name, value) => { if (target && !(name in target)) define(target, name, value); };
  const illegal = () => { throw new TypeError("Illegal constructor"); };
  const rect = () => (typeof DOMRect === "function" ? new DOMRect(0, 0, 0, 0) : { x: 0, y: 0, width: 0, height: 0, top: 0, left: 0, right: 0, bottom: 0 });

  if (typeof MouseEvent === "function" && typeof PointerEvent === "undefined") {
    const defaults = { pointerId: 0, width: 1, height: 1, pressure: 0, tangentialPressure: 0, tiltX: 0, tiltY: 0, twist: 0, pointerType: "", isPrimary: false };
    class PointerEvent extends MouseEvent {
      constructor(type, init = {}) {
        super(type, init);
        for (const key of Object.keys(defaults)) Object.defineProperty(this, key, { value: init[key] === undefined ? defaults[key] : init[key], enumerable: true, configurable: true });
      }
      getCoalescedEvents() { return []; }
      getPredictedEvents() { return []; }
    }
    define(globalThis, "PointerEvent", PointerEvent);
  }
  if (typeof ClipboardEvent === "undefined") {
    class ClipboardEvent extends Event {
      constructor(type, init = {}) { super(type, init); Object.defineProperty(this, "clipboardData", { value: init.clipboardData === undefined ? null : init.clipboardData, enumerable: true, configurable: true }); }
    }
    define(globalThis, "ClipboardEvent", ClipboardEvent);
  }

  if (typeof PerformanceEntry === "function") {
    for (const name of ["PerformanceResourceTiming", "PerformanceNavigationTiming"]) {
      defineGlobal(name, class extends PerformanceEntry { constructor() { super(); illegal(); } });
      Object.defineProperty(globalThis[name], "name", { value: name, configurable: true });
    }
    // The second extends the first, as the specification has it.
    if (!(PerformanceNavigationTiming.prototype instanceof PerformanceResourceTiming)) Object.setPrototypeOf(PerformanceNavigationTiming.prototype, PerformanceResourceTiming.prototype);
  }
  defineGlobal("PerformanceTiming", class PerformanceTiming { constructor() { illegal(); } });

  defineGlobal("SVGAnimatedString", class SVGAnimatedString { constructor() { illegal(); } get baseVal() { return ""; } set baseVal(v) {} get animVal() { return ""; } });
  // ElementCSSInlineStyle is on SVG elements too (and MathML's): the accessor of HTMLElement serves any element.
  for (const name of ["SVGElement", "MathMLElement"]) {
    const target = typeof globalThis[name] === "function" ? globalThis[name].prototype : null;
    const style = typeof HTMLElement === "function" ? Object.getOwnPropertyDescriptor(HTMLElement.prototype, "style") : null;
    if (target && style && !Object.getOwnPropertyDescriptor(target, "style")) Object.defineProperty(target, "style", style);
  }
  if (typeof SVGElement === "function") {
    ensure(SVGElement.prototype, "getBBox", function getBBox() { return rect(); });
    ensure(SVGElement.prototype, "getComputedTextLength", function getComputedTextLength() { return 0; });
  }

  // Canvas: no context is made (getContext returns null, as for a type the browser does not support).
  defineGlobal("ImageData", class ImageData {
    constructor(a, b, c) {
      const data = a instanceof Uint8ClampedArray ? a : null;
      const width = data ? b : a, height = data ? (c === undefined ? data.length / 4 / width : c) : b;
      if (!(width > 0) || !(height > 0)) throw new DOMException("The source width is zero or not a number.", "IndexSizeError");
      define(this, "width", width); define(this, "height", height); define(this, "data", data || new Uint8ClampedArray(width * height * 4)); define(this, "colorSpace", "srgb");
    }
  });
  defineGlobal("Path2D", class Path2D {
    addPath() {} closePath() {} moveTo() {} lineTo() {} bezierCurveTo() {} quadraticCurveTo() {} arc() {} arcTo() {} ellipse() {} rect() {} roundRect() {}
  });
  defineGlobal("CanvasRenderingContext2D", class CanvasRenderingContext2D { constructor() { illegal(); } });
  if (typeof HTMLCanvasElement === "function") {
    ensure(HTMLCanvasElement.prototype, "getContext", function getContext() { return null; });
    ensure(HTMLCanvasElement.prototype, "toDataURL", function toDataURL() { return "data:,"; });
    ensure(HTMLCanvasElement.prototype, "toBlob", function toBlob(callback) { setTimeout(() => callback(null), 0); });
  }

  // Media: a player that cannot play anything. The elements sit under HTMLMediaElement, which they lacked.
  if (typeof HTMLMediaElement === "undefined" && typeof HTMLElement === "function") {
    function HTMLMediaElement() { illegal(); }
    HTMLMediaElement.prototype = Object.create(HTMLElement.prototype, { constructor: { value: HTMLMediaElement, writable: true, configurable: true } });
    const members = {
      load() {},
      pause() {},
      play() { return Promise.reject(new DOMException("The element has no supported sources.", "NotSupportedError")); },
      canPlayType() { return ""; },
      addTextTrack() { return null; },
      paused: { get() { return true; } },
      ended: { get() { return false; } },
      seeking: { get() { return false; } },
      readyState: { get() { return 0; } },
      networkState: { get() { return 0; } },
      duration: { get() { return NaN; } },
      error: { get() { return null; } },
      buffered: { get() { return { length: 0, start() { throw new DOMException("Index out of range.", "IndexSizeError"); }, end() { throw new DOMException("Index out of range.", "IndexSizeError"); } }; } },
      currentTime: { get() { return 0; }, set(v) {} },
      volume: { get() { return 1; }, set(v) {} },
      muted: { get() { return false; }, set(v) {} },
      playbackRate: { get() { return 1; }, set(v) {} },
    };
    for (const [name, member] of Object.entries(members)) {
      Object.defineProperty(HTMLMediaElement.prototype, name, typeof member === "function" ? { value: member, writable: true, enumerable: true, configurable: true } : { ...member, enumerable: true, configurable: true });
    }
    for (const [name, value] of Object.entries({ NETWORK_EMPTY: 0, NETWORK_IDLE: 1, NETWORK_LOADING: 2, NETWORK_NO_SOURCE: 3, HAVE_NOTHING: 0, HAVE_METADATA: 1, HAVE_CURRENT_DATA: 2, HAVE_FUTURE_DATA: 3, HAVE_ENOUGH_DATA: 4 })) {
      Object.defineProperty(HTMLMediaElement, name, { value, enumerable: true });
      Object.defineProperty(HTMLMediaElement.prototype, name, { value, enumerable: true });
    }
    define(globalThis, "HTMLMediaElement", HTMLMediaElement);
    // The elements sit under it by their prototypes; their constructors are left as they are.
    for (const name of ["HTMLVideoElement", "HTMLAudioElement"]) {
      if (typeof globalThis[name] === "function") Object.setPrototypeOf(globalThis[name].prototype, HTMLMediaElement.prototype);
    }
  }

  defineGlobal("Notification", class Notification extends EventTarget {
    constructor(title, options = {}) { super(); define(this, "title", String(title)); define(this, "body", options.body === undefined ? "" : String(options.body)); }
    close() {}
    static get permission() { return "denied"; }
    static requestPermission(callback) { if (typeof callback === "function") callback("denied"); return Promise.resolve("denied"); }
  });

  if (typeof navigator !== "undefined") {
    ensure(navigator, "javaEnabled", function javaEnabled() { return false; });
    ensure(navigator, "vibrate", function vibrate() { return false; });
    ensure(navigator, "share", function share() { return Promise.reject(new DOMException("Share canceled", "AbortError")); });
    ensure(navigator, "canShare", function canShare() { return false; });
    if (!("plugins" in navigator)) Object.defineProperty(navigator, "plugins", { value: Object.assign([], { item: () => null, namedItem: () => null, refresh() {} }), enumerable: true, configurable: true });
    if (!("mimeTypes" in navigator)) Object.defineProperty(navigator, "mimeTypes", { value: Object.assign([], { item: () => null, namedItem: () => null }), enumerable: true, configurable: true });
    if (!("permissions" in navigator)) Object.defineProperty(navigator, "permissions", { value: { query: (descriptor) => Promise.resolve(Object.assign(new EventTarget(), { name: descriptor && descriptor.name, state: "prompt", onchange: null })) }, enumerable: true, configurable: true });
    if (!("storage" in navigator)) Object.defineProperty(navigator, "storage", { value: { estimate: () => Promise.resolve({ usage: 0, quota: 0 }), persist: () => Promise.resolve(false), persisted: () => Promise.resolve(false) }, enumerable: true, configurable: true });
    if (!("geolocation" in navigator)) {
      const denied = () => ({ code: 1, message: "User denied Geolocation", PERMISSION_DENIED: 1, POSITION_UNAVAILABLE: 2, TIMEOUT: 3 });
      Object.defineProperty(navigator, "geolocation", { value: {
        getCurrentPosition(success, failure) { if (typeof failure === "function") setTimeout(() => failure(denied()), 0); },
        watchPosition(success, failure) { if (typeof failure === "function") setTimeout(() => failure(denied()), 0); return 1; },
        clearWatch() {},
      }, enumerable: true, configurable: true });
    }
  }

  if (typeof Document === "function" && !Object.getOwnPropertyDescriptor(Document.prototype, "domain")) {
    Object.defineProperty(Document.prototype, "domain", { get() { return location.hostname; }, set(value) {}, enumerable: true, configurable: true });
  }
  if (!("isSecureContext" in globalThis)) {
    Object.defineProperty(globalThis, "isSecureContext", { get() { return location.protocol === "https:" || location.protocol === "file:" || location.hostname === "localhost" || location.hostname === "127.0.0.1"; }, set: undefined, enumerable: true, configurable: true });
  }
  if (!("crossOriginIsolated" in globalThis)) Object.defineProperty(globalThis, "crossOriginIsolated", { value: false, enumerable: true, configurable: true });
  defineGlobal("CryptoKey", class CryptoKey { constructor() { illegal(); } });
  // A window that opens nothing: a pop-up is blocked.
  ensure(globalThis, "open", function open() { return null; });
  ensure(globalThis, "close", function close() {});

  if (typeof HTMLElement === "function") {
    ensure(HTMLElement.prototype, "setPointerCapture", function setPointerCapture() {});
    ensure(HTMLElement.prototype, "releasePointerCapture", function releasePointerCapture() {});
    ensure(HTMLElement.prototype, "hasPointerCapture", function hasPointerCapture() { return false; });
    ensure(HTMLElement.prototype, "requestFullscreen", function requestFullscreen() { return Promise.reject(new TypeError("Fullscreen is not supported.")); });
  }
  if (typeof HTMLInputElement === "function") ensure(HTMLInputElement.prototype, "showPicker", function showPicker() {});
  if (typeof HTMLDialogElement === "function") {
    ensure(HTMLDialogElement.prototype, "showModal", function showModal() { this.setAttribute("open", ""); });
    ensure(HTMLDialogElement.prototype, "show", function show() { this.setAttribute("open", ""); });
    ensure(HTMLDialogElement.prototype, "close", function close(value) { if (value !== undefined) this.returnValue = String(value); if (this.hasAttribute("open")) { this.removeAttribute("open"); setTimeout(() => this.dispatchEvent(new Event("close")), 0); } });
  }
})();
)JS";

// The on<event> properties of elements and documents (GlobalEventHandlers): the handler is a listener that was added
// when the property was first set, and returning false from it cancels the event.
const char* const kEventHandlerScript = R"JS(
(function () {
  const types = ["abort", "auxclick", "beforeinput", "blur", "cancel", "canplay", "canplaythrough", "change", "click", "close", "contextmenu", "copy", "cut",
    "dblclick", "drag", "dragend", "dragenter", "dragleave", "dragover", "dragstart", "drop", "durationchange", "emptied", "ended", "error", "focus", "formdata",
    "input", "invalid", "keydown", "keypress", "keyup", "load", "loadeddata", "loadedmetadata", "loadstart", "mousedown", "mouseenter", "mouseleave", "mousemove",
    "mouseout", "mouseover", "mouseup", "paste", "pause", "play", "playing", "progress", "ratechange", "reset", "resize", "scroll", "seeked", "seeking", "select",
    "slotchange", "stalled", "submit", "suspend", "timeupdate", "toggle", "volumechange", "waiting", "wheel", "pointerdown", "pointerup", "pointermove",
    "pointerover", "pointerout", "pointerenter", "pointerleave", "pointercancel", "gotpointercapture", "lostpointercapture", "touchstart", "touchend", "touchmove",
    "touchcancel", "transitionend", "animationend", "animationstart", "animationiteration", "readystatechange", "fullscreenchange", "fullscreenerror",
    "selectionchange", "visibilitychange", "securitypolicyviolation"];
  // click(): what a click of the mouse does, which is the event, if the element is not disabled.
  Object.defineProperty(HTMLElement.prototype, "click", { value: function click() {
    if (this.hasAttribute("disabled") && /^(button|input|select|textarea|fieldset|optgroup|option)$/.test(this.localName)) return;
    this.dispatchEvent(new MouseEvent("click", { bubbles: true, cancelable: true, composed: true, view: globalThis, detail: 1 }));
  }, writable: true, enumerable: true, configurable: true });
  let handlers = new WeakMap();
  __solarSetHelper("eraseElementHandlers", () => { handlers = new WeakMap(); });
  const define = (prototype, type) => {
    Object.defineProperty(prototype, "on" + type, {
      get() { const entry = handlers.get(this); return entry && entry[type] ? entry[type].handler : null; },
      set(value) {
        let entry = handlers.get(this);
        if (!entry) { entry = {}; handlers.set(this, entry); }
        let record = entry[type];
        if (!record) {
          record = { handler: null };
          record.listener = (event) => {
            if (!record.handler) return;
            const result = record.handler.call(this, event);
            if (result === false) event.preventDefault();
          };
          entry[type] = record;
          this.addEventListener(type, record.listener);
        }
        record.handler = typeof value === "function" || (typeof value === "object" && value !== null && typeof value.call === "function") ? value : null;
      },
      enumerable: true,
      configurable: true,
    });
  };
  for (const type of types) {
    define(HTMLElement.prototype, type);
    define(Document.prototype, type);
  }
  // The content attributes, onclick="..." and the like: a function made of the text, set as the property is. On body
  // and frameset the handlers of the window events are the window's.
  const windowTypes = new Set(["load", "error", "message", "hashchange", "popstate", "pageshow", "pagehide", "unload", "beforeunload", "resize", "scroll", "focus", "blur"]);
  __solarSetHelper("contentHandler", (element, name, value) => {
    const type = name.slice(2);
    if (!types.includes(type) && !windowTypes.has(type)) return;
    const onWindow = (element instanceof HTMLBodyElement || element instanceof HTMLFrameSetElement) && windowTypes.has(type);
    const target = onWindow ? globalThis : element;
    if (value === null) { target[name] = null; return; }
    let handler;
    try { handler = new Function("event", value); } catch (e) { return; }
    target[name] = handler;
  });
})();
)JS";

}  // namespace

void InstallHtmlApis(Quanta::Embed::Realm& realm) { Install(realm); }
void InstallHtmlApis(Quanta::Embed::Runtime& runtime) { Install(runtime); }

namespace {

Value IllegalWindowConstructor(Context& ctx, Value, qe::Args, Value) {
  qe::ThrowTypeError(ctx, "Illegal constructor");
  return qe::Undefined();
}

// The elements of the window's document that have `name`, as window[name] is: the element if one has it, or a
// collection of those that do.
bool NamedWindowProperty(qe::Realm* realm, const std::string& name, Value& out) {
  dom::Document* document = DocumentOfRealm(realm);
  if (!document) return false;
  const std::vector<dom::Element*>* elements = dom::WindowNamedElements(document, name);
  if (!elements || elements->empty()) return false;
  out = elements->size() == 1 ? qe::FromObject(elements->front()) : dom::NewWindowNamedCollection(realm->GetContext(), document, name);
  return true;
}

std::vector<std::string> NamedWindowPropertyNames(qe::Realm* realm) {
  std::vector<std::string> names;
  if (dom::Document* document = DocumentOfRealm(realm)) dom::WindowNamesOf(document, names);
  std::vector<std::string> unique;
  for (const std::string& name : names) {
    if (std::find(unique.begin(), unique.end(), name) == unique.end()) unique.push_back(name);
  }
  return unique;
}

void SetUpWindowInterface(Context& ctx) {
  Value global = qe::FromObject(ctx.get_global_object());
  qe::ClassRef window = qe::DefineClass(ctx, "Window", IllegalWindowConstructor, 0);
  qe::DefineGlobal(ctx, "Window", window.constructor);
  Value eventTarget = qe::Get(ctx, qe::Get(ctx, global, "EventTarget"), "prototype");
  qe::NamedPropertiesHooks hooks;
  hooks.get = NamedWindowProperty;
  hooks.names = NamedWindowPropertyNames;
  Value named = qe::NewNamedPropertiesObject(ctx, global, eventTarget, std::move(hooks));
  qe::SetPrototypeOf(ctx, qe::FromObject(window.prototype), named);
  qe::SetPrototypeOf(ctx, global, qe::FromObject(window.prototype));
}

}  // namespace

// What a window in another origin may reach (CrossOriginProperties of a Window).
qe::CrossOriginHooks WindowCrossOriginHooks() {
  qe::CrossOriginHooks hooks;
  hooks.sameOrigin = [](qe::Realm* caller, qe::Realm* target) {
    if (!caller || !target) return false;
    dom::Document* a = DocumentOfRealm(caller);
    dom::Document* b = DocumentOfRealm(target);
    return a && b && OriginOfDocument(a) == OriginOfDocument(b);
  };
  hooks.properties = {{"window", true, false}, {"self", true, false},   {"location", true, true}, {"close", false, false},
                      {"closed", true, false}, {"focus", false, false}, {"blur", false, false},   {"frames", true, false},
                      {"length", true, false}, {"top", true, false},    {"opener", true, false},  {"parent", true, false},
                      {"postMessage", false, false}};
  hooks.childCount = [](qe::Realm* target) -> uint32_t {
    dom::Document* document = DocumentOfRealm(target);
    return document ? static_cast<uint32_t>(ChildWindowsOf(document).size()) : 0;
  };
  hooks.childAt = [](qe::Realm* target, uint32_t index) -> std::optional<Value> {
    dom::Document* document = DocumentOfRealm(target);
    if (!document) return std::nullopt;
    const std::vector<Object*> windows = ChildWindowsOf(document);
    if (index >= windows.size()) return std::nullopt;
    return qe::FromObject(windows[index]);
  };
  hooks.childNamed = [](qe::Realm* target, const std::string& name) -> std::optional<Value> {
    dom::Document* document = DocumentOfRealm(target);
    if (!document) return std::nullopt;
    if (Object* window = ChildWindowNamed(document, name)) return qe::FromObject(window);
    return std::nullopt;
  };
  return hooks;
}

template <typename Host>
void InstallWindowOn(Host& host, dom::Document* document, Quanta::Embed::Realm* realm, Object* reuseProxy) {
  Context& ctx = host.GetContext();
  if (document) {
    dom::SetAssociatedDocument(ctx, document);
    document->globalObject = ctx.get_global_object();
    document->realm = realm;
    if (realm) RegisterRealmDocument(realm, document);
    document->NoteWrite();
  }
  if constexpr (std::is_same_v<Host, qe::Realm>) {
    // The window script is run through a WindowProxy, which stays the same across navigations of an iframe.
    Value proxy;
    if (reuseProxy && qe::IsWindowProxy(Value(reuseProxy)) && qe::SetWindowProxyTarget(Value(reuseProxy), *realm)) {
      proxy = Value(reuseProxy);
    } else {
      proxy = qe::NewWindowProxy(*realm, WindowCrossOriginHooks());
    }
    if (document && qe::IsObject(proxy)) {
      document->globalObject = proxy.as_object();
      document->NoteWrite();
    }
  }
  // The window is a Window, which has a named properties object in its prototype chain, between Window.prototype and
  // EventTarget.prototype, for window[name].
  SetUpWindowInterface(ctx);
  host.Evaluate(kWindowScript, "window.js");
  host.Evaluate(kEventHandlerScript, "handlers.js");
  { const auto platform = host.Evaluate(kPlatformScript, "platform.js"); if (!platform.ok) std::fprintf(stderr, "platform.js: %s\n", platform.error.c_str()); }
  // SOLAR_PRELUDE=file runs a script of the developer's before anything of the page's, for looking into what a site does.
  if (const char* prelude = std::getenv("SOLAR_PRELUDE")) {
    std::ifstream in(prelude, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    host.Evaluate(text, "prelude.js");
  }
  { const auto stubs = host.Evaluate(kStubScript, "stubs.js"); if (!stubs.ok) std::fprintf(stderr, "stubs.js: %s\n", stubs.error.c_str()); }
  { const auto reflect = host.Evaluate(ReflectScript(), "reflect.js"); if (!reflect.ok) std::fprintf(stderr, "reflect.js: %s\n", reflect.error.c_str()); }
  { const auto forms = host.Evaluate(FormScript(), "forms.js"); if (!forms.ok) std::fprintf(stderr, "forms.js: %s\n", forms.error.c_str()); }
  std::string xhr;
  for (const char* const* piece = web::kScriptXhr; *piece; ++piece) xhr += *piece;
  const auto xhrResult = host.Evaluate(xhr, "xhr.js");
  if (!xhrResult.ok) std::fprintf(stderr, "xhr.js: %s\n", xhrResult.error.c_str());
  // The Typed OM is written in script, over the CSS interfaces.
  std::string typedOm;
  for (const char* const* piece = web::kScriptTypedOm; *piece; ++piece) typedOm += *piece;
  typedOm += "\n__solarTypedOm.install(); delete globalThis.__solarTypedOm;\n";
  const auto typedOmResult = host.Evaluate(typedOm, "typedom.js");
  if (!typedOmResult.ok) std::fprintf(stderr, "typedom.js: %s\n", typedOmResult.error.c_str());
  // The geometry of elements.
  layout::InstallLayoutNatives(ctx);
  std::string geometry;
  for (const char* const* piece = web::kScriptLayout; *piece; ++piece) geometry += *piece;
  const auto geometryResult = host.Evaluate(geometry, "layout.js");
  if (!geometryResult.ok) std::fprintf(stderr, "layout.js: %s\n", geometryResult.error.c_str());
  // And the font loading API.
  std::string fontLoading;
  for (const char* const* piece = web::kScriptFontLoading; *piece; ++piece) fontLoading += *piece;
  const auto fontLoadingResult = host.Evaluate(fontLoading, "fontloading.js");
  if (!fontLoadingResult.ok) std::fprintf(stderr, "fontloading.js: %s\n", fontLoadingResult.error.c_str());
  // And Web Animations.
  std::string animations;
  for (const char* const* piece = web::kScriptAnimations; *piece; ++piece) animations += *piece;
  const auto animationsResult = host.Evaluate(animations, "animations.js");
  if (!animationsResult.ok) std::fprintf(stderr, "animations.js: %s\n", animationsResult.error.c_str());
}

void InstallWindow(Quanta::Embed::Runtime& runtime, dom::Document* document) { InstallWindowOn(runtime, document, nullptr, nullptr); }
void InstallWindow(Quanta::Embed::Realm& realm, dom::Document* document, Quanta::Object* reuseProxy) {
  InstallWindowOn(realm, document, &realm, reuseProxy);
}

}  // namespace solar::html
