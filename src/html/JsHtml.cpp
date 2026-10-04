#include <string>

#include "solar/dom/CustomElements.h"
#include "solar/dom/NodeBindingsInternal.h"
#include "solar/html/HtmlBindings.h"
#include "solar/dom/Range.h"
#include "solar/html/Frames.h"
#include "solar/html/Parser.h"
#include "solar/html/Serializer.h"
#include "solar/html/Xml.h"

namespace solar::html {

namespace qe = Quanta::Embed;
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

// __solarNamedLookup(name): what window[name] is: the element with that name, or a collection of them, or undefined.
Value NamedLookup(Context& ctx, Value, qe::Args args, Value) {
  dom::Document* document = dom::AssociatedDocument(ctx);
  if (!document || args.empty()) return qe::Undefined();
  const std::string name = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  dom::Element* first = nullptr;
  size_t count = 0;
  for (dom::Node* node = document->NextInTree(document); node; node = node->NextInTree(document)) {
    dom::Element* element = dom::AsElement(node);
    if (element && dom::HasWindowName(element, name)) {
      if (!first) first = element;
      ++count;
    }
  }
  if (count == 0) return qe::Undefined();
  if (count == 1) return qe::FromObject(first);
  return dom::NewWindowNamedCollection(ctx, document, name);
}

// __solarRegisterNamed(update): the window script's function that follows the names.
Value RegisterNamed(Context& ctx, Value, qe::Args args, Value) {
  if (Object* holder = dom::RealmHolder(ctx); holder && !args.empty()) qe::Set(ctx, qe::FromObject(holder), "windowNamedUpdate", args[0]);
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
  InstallFrameHooks();
  Object* document = dom::InterfacePrototype(ctx, dom::Interface::Document);
  qe::DefineAccessor(document, "head", GetHead, nullptr);
  qe::DefineAccessor(document, "body", GetBody, SetBody);
  qe::DefineAccessor(document, "title", GetTitle, SetTitle);
  qe::DefineAccessor(document, "readyState", GetReadyState, nullptr);
  qe::DefineAccessor(document, "currentScript", GetCurrentScript, nullptr);
  qe::DefineAccessor(document, "defaultView", GetDefaultView, nullptr);
  qe::DefineGlobalFunction(ctx, "__solarDocument", GlobalDocument, 0);
  qe::DefineGlobalFunction(ctx, "__solarSetWindow", SetWindowTarget, 1);
  qe::DefineGlobalFunction(ctx, "__solarSetDocumentUrl", SetDocumentUrl, 1);
  qe::DefineGlobalFunction(ctx, "__solarNamedLookup", NamedLookup, 1);
  qe::DefineGlobalFunction(ctx, "__solarRegisterNamed", RegisterNamed, 1);

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
const char* const kWindowScript = R"JS(
(function () {
  const target = new EventTarget();
  __solarSetWindow(target);
  const getDocument = __solarDocument;
  const setDocumentUrl = __solarSetDocumentUrl;
  const lookup = __solarNamedLookup;
  const registerNamed = __solarRegisterNamed;
  const parentWindow = __solarParent;
  const frameElement = __solarFrameElement;
  const childCount = __solarChildCount;
  const childWindow = __solarChildWindow;
  __solarRegisterFire((target, type) => target.dispatchEvent(new Event(type)));
  __solarRegisterFocusFire((target, type, related, bubbles) => target.dispatchEvent(new FocusEvent(type, { bubbles, composed: true, relatedTarget: related, view: globalThis })));
  delete globalThis.__solarRegisterFocusFire;
  // window.focus() and blur(): the window has the focus already, and has no other window to give it to.
  for (const name of ["focus", "blur"]) Object.defineProperty(globalThis, name, { value: function () {}, writable: true, enumerable: true, configurable: true });
  for (const name of ["__solarParent", "__solarFrameElement", "__solarChildCount", "__solarChildWindow", "__solarFrameTask", "__solarScriptTask", "__solarRegisterFire"]) delete globalThis[name];
  // window.postMessage: a message event, as a task, at this window. What sent it is not known to a window that is
  // only a function of its own, so the origin is its own and the source is not given.
  Object.defineProperty(globalThis, "postMessage", { value: function postMessage(message, targetOrigin, transfer) {
    if (arguments.length < 1) throw new TypeError("Failed to execute 'postMessage' on 'Window': 1 argument required, but only 0 present.");
    setTimeout(() => { target.dispatchEvent(new MessageEvent("message", { data: message, origin: location.origin })); }, 0);
  }, writable: true, enumerable: true, configurable: true });
  delete globalThis.__solarNamedLookup;
  delete globalThis.__solarRegisterNamed;
  delete globalThis.__solarSetWindow;
  delete globalThis.__solarDocument;
  delete globalThis.__solarSetDocumentUrl;
  // window.location: the address of the document, as the parts of a URL. Going to another document is not
  // something a page can do yet, so only the fragment can change.
  const url = () => new URL(getDocument().URL);
  const fragmentOnly = (next) => {
    const current = url();
    const target = new URL(next, current);
    const withoutHash = (u) => { const copy = new URL(u.href); copy.hash = ""; return copy.href; };
    if (withoutHash(current) === withoutHash(target)) setDocumentUrl(target.href);
  };
  // window[name], for the elements of the document that have the name: a property that is there while one does,
  // unless the page has made one of its own.
  const named = new Set();
  const hasOwn = Object.prototype.hasOwnProperty;
  registerNamed((name) => {
    const present = lookup(name) !== undefined;
    if (present && !named.has(name)) {
      if (hasOwn.call(globalThis, name)) return;
      named.add(name);
      Object.defineProperty(globalThis, name, {
        get() { return lookup(name); },
        set(value) { named.delete(name); Object.defineProperty(globalThis, name, { value, writable: true, enumerable: true, configurable: true }); },
        enumerable: false,
        configurable: true,
      });
    } else if (!present && named.has(name)) {
      named.delete(name);
      delete globalThis[name];
    }
  });
  const location = {};
  for (const name of ["origin", "protocol", "host", "hostname", "port", "pathname", "search"]) {
    Object.defineProperty(location, name, { get() { return url()[name]; }, set(value) {}, enumerable: true, configurable: false });
  }
  Object.defineProperty(location, "href", { get() { return url().href; }, set(value) { fragmentOnly(String(value)); }, enumerable: true, configurable: false });
  Object.defineProperty(location, "hash", { get() { return url().hash; }, set(value) { const next = url(); next.hash = value; setDocumentUrl(next.href); }, enumerable: true, configurable: false });
  for (const name of ["assign", "replace"]) {
    Object.defineProperty(location, name, { value: function (next) { fragmentOnly(String(next)); }, writable: false, enumerable: true, configurable: false });
  }
  Object.defineProperty(location, "reload", { value: function () {}, writable: false, enumerable: true, configurable: false });
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
  Object.defineProperty(globalThis, "opener", { value: null, writable: true, enumerable: true, configurable: true });
  Object.defineProperty(globalThis, "getSelection", { value: function getSelection() { return getDocument().getSelection(); }, writable: true, enumerable: true, configurable: true });
  // performance.now(), from the clock the page started on, and animation frames: there is no display to wait for,
  // so a frame is a 16 ms timer.
  const timeOrigin = Date.now();
  Object.defineProperty(globalThis, "performance", { value: { now() { return Date.now() - timeOrigin; }, timeOrigin, toJSON() { return { timeOrigin }; } }, writable: true, enumerable: true, configurable: true });
  const frames = new Map();
  let nextFrame = 1;
  Object.defineProperty(globalThis, "requestAnimationFrame", { value: function requestAnimationFrame(callback) {
    if (typeof callback !== "function") throw new TypeError("Failed to execute 'requestAnimationFrame' on 'Window': The callback provided as parameter 1 is not a function.");
    const handle = nextFrame++;
    frames.set(handle, setTimeout(() => { frames.delete(handle); callback(Date.now() - timeOrigin); }, 16));
    return handle;
  }, writable: true, enumerable: true, configurable: true });
  Object.defineProperty(globalThis, "cancelAnimationFrame", { value: function cancelAnimationFrame(handle) {
    if (frames.has(handle)) { clearTimeout(frames.get(handle)); frames.delete(handle); }
  }, writable: true, enumerable: true, configurable: true });
  for (const name of ["addEventListener", "removeEventListener", "dispatchEvent"]) {
    const method = EventTarget.prototype[name];
    Object.defineProperty(globalThis, name, { value: function (...args) { return method.apply(target, args); }, writable: true, enumerable: true, configurable: true });
  }
  // onload and the like: the handler is a listener on the window.
  for (const type of ["load", "error", "message", "hashchange", "popstate", "pageshow", "pagehide", "unload", "beforeunload", "resize", "scroll", "focus", "blur"]) {
    let handler = null;
    let listener = null;
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
  const handlers = new WeakMap();
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
  __solarRegisterContentHandler((element, name, value) => {
    const type = name.slice(2);
    if (!types.includes(type) && !windowTypes.has(type)) return;
    const onWindow = (element instanceof HTMLBodyElement || element instanceof HTMLFrameSetElement) && windowTypes.has(type);
    const target = onWindow ? globalThis : element;
    if (value === null) { target[name] = null; return; }
    let handler;
    try { handler = new Function("event", value); } catch (e) { return; }
    target[name] = handler;
  });
  delete globalThis.__solarRegisterContentHandler;
})();
)JS";

}  // namespace

void InstallHtmlApis(Quanta::Embed::Realm& realm) { Install(realm); }
void InstallHtmlApis(Quanta::Embed::Runtime& runtime) { Install(runtime); }

template <typename Host>
void InstallWindowOn(Host& host, dom::Document* document, Quanta::Embed::Realm* realm) {
  Context& ctx = host.GetContext();
  if (document) {
    dom::SetAssociatedDocument(ctx, document);
    document->globalObject = ctx.get_global_object();
    document->realm = realm;
    document->NoteWrite();
  }
  host.Evaluate(kWindowScript, "window.js");
  host.Evaluate(kEventHandlerScript, "handlers.js");
}

void InstallWindow(Quanta::Embed::Runtime& runtime, dom::Document* document) { InstallWindowOn(runtime, document, nullptr); }
void InstallWindow(Quanta::Embed::Realm& realm, dom::Document* document) { InstallWindowOn(realm, document, &realm); }

}  // namespace solar::html
