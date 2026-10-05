#include <cstdlib>
#include <string>

#include "solar/dom/CustomElements.h"
#include "solar/dom/NodeBindingsInternal.h"
#include "solar/html/HtmlBindings.h"

// The focus model (https://html.spec.whatwg.org/#focus): which elements can be focused, focus() and blur(), the
// events they fire, and document.activeElement. Without a renderer, "being rendered" is only what the markup says.
namespace solar::html {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::Object;
using Quanta::Value;

namespace {

// tabindex: an integer, with white space around it allowed.
bool ParseTabIndex(const dom::Element* element, long& value) {
  const dom::Attr* attribute = element->FindAttribute("", "tabindex");
  if (!attribute) return false;
  std::string text = attribute->value;
  const size_t first = text.find_first_not_of(" \t\n\f\r");
  if (first == std::string::npos) return false;
  text = text.substr(first, text.find_last_not_of(" \t\n\f\r") - first + 1);
  size_t i = 0;
  if (text[0] == '-' || text[0] == '+') ++i;
  if (i >= text.size()) return false;
  for (size_t j = i; j < text.size(); ++j) {
    if (text[j] < '0' || text[j] > '9') return false;
  }
  value = std::strtol(text.c_str(), nullptr, 10);
  return true;
}

bool IsFormControl(const dom::Element* element) { return element->IsHtml("button") || element->IsHtml("input") || element->IsHtml("select") || element->IsHtml("textarea"); }

bool InsideDisabledFieldset(const dom::Element* element) {
  for (const dom::Node* node = element->parentNode; node; node = node->parentNode) {
    const dom::Element* parent = dom::AsElement(node);
    if (parent && parent->IsHtml("fieldset") && parent->FindAttribute("", "disabled")) return true;
  }
  return false;
}

bool IsDisabled(const dom::Element* element) { return IsFormControl(element) && (element->FindAttribute("", "disabled") || InsideDisabledFieldset(element)); }

// An ancestor, in the flat sense of shadow trees, that has the attribute.
bool AncestorHas(const dom::Element* element, const char* attribute) {
  const dom::Node* node = element;
  while (node) {
    if (const dom::Element* e = dom::AsElement(node); e && e->FindAttribute("", attribute)) return true;
    if (node->parentNode) node = node->parentNode;
    else if (node->IsFragment() && static_cast<const dom::DocumentFragment*>(node)->isShadowRoot) node = static_cast<const dom::ShadowRoot*>(node)->host;
    else break;
  }
  return false;
}

bool IsContentEditable(const dom::Element* element) {
  for (const dom::Element* e = element; e; e = dom::AsElement(e->parentNode)) {
    if (const dom::Attr* attribute = e->FindAttribute("", "contenteditable")) {
      std::string value = attribute->value;
      for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (value == "" || value == "true" || value == "plaintext-only") return true;
      if (value == "false") return false;
    }
  }
  return false;
}

// What the element is focusable by itself, with no tabindex.
bool NativelyFocusable(const dom::Element* element) {
  if (!element->IsHtml()) return false;
  const std::string& name = element->localName;
  if ((name == "a" || name == "area") && element->FindAttribute("", "href")) return true;
  if (name == "button" || name == "select" || name == "textarea" || name == "iframe") return true;
  if (name == "input") {
    const dom::Attr* type = element->FindAttribute("", "type");
    if (!type) return true;
    std::string value = type->value;
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value != "hidden";
  }
  if (name == "summary" && element->parentNode) {
    const dom::Element* parent = dom::AsElement(element->parentNode);
    if (parent && parent->IsHtml("details")) {
      for (const dom::Node* child = parent->firstChild; child; child = child->nextSibling) {
        if (const dom::Element* e = dom::AsElement(child); e && e->IsHtml("summary")) return e == element;
      }
    }
    return false;
  }
  if ((name == "audio" || name == "video") && element->FindAttribute("", "controls")) return true;
  return IsContentEditable(element);
}

bool Connected(dom::Node* node) {
  dom::Node* root = dom::ShadowIncludingRoot(node);
  return root && root->IsDocument();
}

bool IsFocusable(dom::Element* element) {
  if (!element->IsHtml() && element->namespaceUri != dom::kSvgNamespace) return false;
  if (!Connected(element)) return false;
  if (AncestorHas(element, "inert") || AncestorHas(element, "hidden")) return false;
  if (IsDisabled(element)) return false;
  long ignored = 0;
  return ParseTabIndex(element, ignored) || NativelyFocusable(element);
}

// The element that takes the focus given to a shadow host whose shadow root delegates it.
dom::Element* FocusDelegate(dom::Node* where) {
  for (dom::Node* node = where->NextInTree(where); node; node = node->NextInTree(where)) {
    dom::Element* element = dom::AsElement(node);
    if (!element) continue;
    if (element->shadowRoot && element->shadowRoot->delegatesFocus) {
      if (dom::Element* delegate = FocusDelegate(element->shadowRoot)) return delegate;
      continue;
    }
    if (IsFocusable(element)) return element;
  }
  return nullptr;
}

dom::Element* DesignatedFocusableArea(dom::Element* element) {
  if (element->shadowRoot && element->shadowRoot->delegatesFocus) return FocusDelegate(element->shadowRoot);
  return IsFocusable(element) ? element : nullptr;
}

// (target, type, related target, bubbles) => the FocusEvent, dispatched.
void Fire(Context& ctx, dom::Element* target, const char* type, dom::Element* related, bool bubbles) {
  Object* holder = dom::RealmHolder(ctx);
  if (!holder) return;
  Value fire = qe::Get(ctx, qe::FromObject(holder), "fireFocus");
  if (!qe::IsCallable(fire)) return;
  Value arguments[] = {qe::FromObject(target), qe::FromUtf8(ctx, type), related ? qe::FromObject(related) : qe::Null(), qe::FromBool(bubbles)};
  qe::Call(ctx, fire, qe::Undefined(), qe::Args(arguments, 4));
  if (qe::HasException(ctx)) ctx.clear_exception();
}

void MoveFocus(Context& ctx, dom::Document* document, dom::Element* target) {
  dom::Element* old = document->focusedElement;
  if (old == target) return;
  if (old) {
    Fire(ctx, old, "blur", target, false);
    Fire(ctx, old, "focusout", target, true);
    // A handler may have moved the focus itself: then it has been placed already.
    if (document->focusedElement != old) return;
  }
  document->focusedElement = target;
  document->NoteWrite();
  // An iframe holds the focus of its parent while an element in it does.
  if (document->frameElement && document->frameElement->nodeDocument && target) {
    document->frameElement->nodeDocument->focusedElement = document->frameElement;
    document->frameElement->nodeDocument->NoteWrite();
  }
  if (target) {
    Fire(ctx, target, "focus", old, false);
    Fire(ctx, target, "focusin", old, true);
  }
}

// Whether `node` is in the shadow tree of `host`, however deep, and not among its light children.
bool InShadowTreeOf(const dom::Node* node, const dom::Element* host) {
  for (;;) {
    const dom::Node* root = node;
    while (root->parentNode) root = root->parentNode;
    if (!root->IsFragment() || !static_cast<const dom::DocumentFragment*>(root)->isShadowRoot) return false;
    node = static_cast<const dom::ShadowRoot*>(root)->host;
    if (node == host) return true;
  }
}

dom::Element* ThisHtmlOrSvg(Context& ctx, const Value& t) {
  dom::Element* self = dom::ThisElement(ctx, t);
  if (self && !self->IsHtml() && self->namespaceUri != dom::kSvgNamespace) {
    qe::ThrowTypeError(ctx, "Illegal invocation");
    return nullptr;
  }
  return self;
}

Value Focus(Context& ctx, Value t, qe::Args, Value) {
  dom::Element* self = ThisHtmlOrSvg(ctx, t);
  if (!self || !self->nodeDocument) return qe::Undefined();
  // A host that delegates the focus does not take it from what already has it inside.
  dom::Document* document = self->nodeDocument;
  if (self->shadowRoot && self->shadowRoot->delegatesFocus && document->focusedElement && InShadowTreeOf(document->focusedElement, self)) return qe::Undefined();
  dom::Element* target = DesignatedFocusableArea(self);
  if (!target) return qe::Undefined();
  MoveFocus(ctx, target->nodeDocument, target);
  return qe::Undefined();
}

Value Blur(Context& ctx, Value t, qe::Args, Value) {
  dom::Element* self = ThisHtmlOrSvg(ctx, t);
  if (!self || !self->nodeDocument) return qe::Undefined();
  dom::Element* focused = self->nodeDocument->focusedElement;
  const bool delegating = self->shadowRoot && self->shadowRoot->delegatesFocus && focused && InShadowTreeOf(focused, self);
  if (focused == self || delegating) MoveFocus(ctx, self->nodeDocument, nullptr);
  return qe::Undefined();
}

// tabIndex: the attribute if it is one, and otherwise 0 for what can be focused by itself and -1 for the rest.
Value GetTabIndex(Context& ctx, Value t, qe::Args, Value) {
  dom::Element* self = ThisHtmlOrSvg(ctx, t);
  if (!self) return qe::Undefined();
  long value = 0;
  if (ParseTabIndex(self, value)) return Value(static_cast<double>(value));
  return Value(static_cast<double>((NativelyFocusable(self) || (self->shadowRoot && self->shadowRoot->delegatesFocus && false)) ? 0 : -1));
}

Value SetTabIndex(Context& ctx, Value t, qe::Args args, Value) {
  dom::Element* self = ThisHtmlOrSvg(ctx, t);
  if (!self || args.empty()) return qe::Undefined();
  const double number = args[0].to_number();
  if (qe::HasException(ctx)) return qe::Undefined();
  dom::SetAttribute(ctx, self, "tabindex", std::to_string(static_cast<long>(number)));
  return qe::Undefined();
}

// document.activeElement and shadowRoot.activeElement: the focused element, as the scope can see it.
Value ActiveElementOf(Context& ctx, dom::Node* scope) {
  dom::Document* document = scope->IsDocument() ? static_cast<dom::Document*>(scope) : scope->nodeDocument;
  if (!document) return qe::Null();
  dom::Node* candidate = document->focusedElement;
  if (candidate) {
    candidate = dom::Retarget(candidate, scope);
    if (candidate && candidate->Root() == scope && !candidate->IsDocument()) return qe::FromObject(candidate);
  }
  if (scope->IsDocument()) {
    // With none focused, the document's body is.
    if (dom::Element* root = document->DocumentElement()) {
      for (dom::Node* child = root->firstChild; child; child = child->nextSibling) {
        if (dom::Element* e = dom::AsElement(child); e && (e->IsHtml("body") || e->IsHtml("frameset"))) return qe::FromObject(e);
      }
      return qe::FromObject(root);
    }
  }
  return qe::Null();
}

Value GetActiveElementDocument(Context& ctx, Value t, qe::Args, Value) {
  dom::Document* self = dom::ThisDocument(ctx, t);
  return self ? ActiveElementOf(ctx, self) : qe::Undefined();
}

Value GetActiveElementShadow(Context& ctx, Value t, qe::Args, Value) {
  dom::Node* self = dom::ThisNode(ctx, t);
  return self ? ActiveElementOf(ctx, self) : qe::Undefined();
}

Value HasFocus(Context& ctx, Value t, qe::Args, Value) {
  dom::Document* self = dom::ThisDocument(ctx, t);
  return self ? qe::FromBool(self->window != nullptr) : qe::Undefined();
}

}  // namespace

// The focus fixup: an element that leaves the document is not the focused one any more, and nothing is told.
void FocusAfterRemove(dom::Node* node) {
  dom::Document* document = node->nodeDocument;
  if (!document || !document->focusedElement) return;
  if (dom::IsShadowIncludingInclusiveAncestor(node, document->focusedElement)) {
    document->focusedElement = nullptr;
    document->NoteWrite();
  }
}

void DefineFocusMembers(Context& ctx) {
  Object* html = dom::InterfacePrototype(ctx, dom::Interface::HtmlElement);
  qe::DefineMethod(html, "focus", Focus, 0);
  qe::DefineMethod(html, "blur", Blur, 0);
  qe::DefineAccessor(html, "tabIndex", GetTabIndex, dom::Reactions<SetTabIndex>);
  Object* document = dom::InterfacePrototype(ctx, dom::Interface::Document);
  qe::DefineAccessor(document, "activeElement", GetActiveElementDocument, nullptr);
  qe::DefineMethod(document, "hasFocus", HasFocus, 0);
  qe::DefineAccessor(dom::InterfacePrototype(ctx, dom::Interface::ShadowRoot), "activeElement", GetActiveElementShadow, nullptr);
}

}  // namespace solar::html
