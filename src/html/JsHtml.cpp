#include <string>

#include "solar/dom/NodeBindingsInternal.h"
#include "solar/html/HtmlBindings.h"
#include "solar/html/Parser.h"
#include "solar/html/Serializer.h"

namespace solar::html {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Heap;
using Quanta::Object;
using Quanta::Value;

namespace {

char g_domParserKey;

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

Value GetInnerHtml(Context& ctx, Value t, qe::Args, Value) {
  dom::Element* self = dom::ThisElement(ctx, t);
  return self ? qe::FromWtf8(ctx, SerializeChildren(self)) : qe::Undefined();
}

Value SetInnerHtml(Context& ctx, Value t, qe::Args args, Value) {
  dom::Element* self = dom::ThisElement(ctx, t);
  if (!self) return qe::Undefined();
  const std::optional<std::string> markup = ReadMarkup(ctx, args);
  if (!markup) return qe::Undefined();
  dom::DocumentFragment* fragment = ParseFragment(ctx, self, *markup);
  dom::Node* target = self;
  if (self->namespaceUri == dom::kHtmlNamespace && self->localName == "template" && self->templateContents) target = self->templateContents;
  dom::ReplaceAll(target, fragment);
  return qe::Undefined();
}

Value GetOuterHtml(Context& ctx, Value t, qe::Args, Value) {
  dom::Element* self = dom::ThisElement(ctx, t);
  return self ? qe::FromWtf8(ctx, SerializeNode(self)) : qe::Undefined();
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
  dom::DocumentFragment* fragment = ParseFragment(ctx, context, *markup);
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
  dom::DocumentFragment* fragment = ParseFragment(ctx, context, markup);
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
    ParseDocument(ctx, document, markup, ScriptingMode::Disabled);
    return qe::FromObject(document);
  }
  if (type == "text/xml" || type == "application/xml" || type == "application/xhtml+xml" || type == "image/svg+xml") {
    // Refused rather than parsed as HTML: there is no XML parser yet.
    dom::Throw(ctx, {"NotSupportedError", "Parsing " + type + " is not supported yet"});
    return qe::Undefined();
  }
  qe::ThrowTypeError(ctx, "Failed to execute 'parseFromString' on 'DOMParser': The provided value '" + type + "' is not a valid enum value of type SupportedType.");
  return qe::Undefined();
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
  return self->window ? qe::FromObject(ctx.get_global_object()) : qe::Null();
}

// __solarDocument() and __solarSetWindow(target): what the window's script is made of.
Value GlobalDocument(Context& ctx, Value, qe::Args, Value) { return qe::FromObject(dom::AssociatedDocument(ctx)); }

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
  qe::DefineAccessor(element, "innerHTML", GetInnerHtml, SetInnerHtml);
  qe::DefineAccessor(element, "outerHTML", GetOuterHtml, SetOuterHtml);
  qe::DefineMethod(element, "insertAdjacentHTML", InsertAdjacentHtml, 2);

  Object* document = dom::InterfacePrototype(ctx, dom::Interface::Document);
  qe::DefineAccessor(document, "head", GetHead, nullptr);
  qe::DefineAccessor(document, "body", GetBody, SetBody);
  qe::DefineAccessor(document, "title", GetTitle, SetTitle);
  qe::DefineAccessor(document, "readyState", GetReadyState, nullptr);
  qe::DefineAccessor(document, "currentScript", GetCurrentScript, nullptr);
  qe::DefineAccessor(document, "defaultView", GetDefaultView, nullptr);
  qe::DefineGlobalFunction(ctx, "__solarDocument", GlobalDocument, 0);
  qe::DefineGlobalFunction(ctx, "__solarSetWindow", SetWindowTarget, 1);

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
  delete globalThis.__solarSetWindow;
  delete globalThis.__solarDocument;
  Object.defineProperty(globalThis, "document", { get: getDocument, set: undefined, enumerable: true, configurable: true });
  for (const name of ["window", "self", "top", "parent", "frames"]) {
    Object.defineProperty(globalThis, name, { value: globalThis, writable: true, enumerable: true, configurable: true });
  }
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
          listener = (event) => { if (handler) handler.call(globalThis, event); };
          target.addEventListener(type, listener);
        }
      },
      enumerable: true,
      configurable: true,
    });
  }
})();
)JS";

}  // namespace

void InstallHtmlApis(Quanta::Embed::Realm& realm) { Install(realm); }
void InstallHtmlApis(Quanta::Embed::Runtime& runtime) { Install(runtime); }

void InstallWindow(Quanta::Embed::Runtime& runtime, dom::Document* document) {
  if (document) dom::SetAssociatedDocument(runtime.GetContext(), document);
  runtime.Evaluate(kWindowScript, "window.js");
}

}  // namespace solar::html
