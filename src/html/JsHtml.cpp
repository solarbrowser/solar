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

template <typename Host>
void Install(Host& host) {
  Context& ctx = host.GetContext();
  Object* element = dom::InterfacePrototype(ctx, dom::Interface::Element);
  qe::DefineAccessor(element, "innerHTML", GetInnerHtml, SetInnerHtml);
  qe::DefineAccessor(element, "outerHTML", GetOuterHtml, SetOuterHtml);
  qe::DefineMethod(element, "insertAdjacentHTML", InsertAdjacentHtml, 2);

  qe::ClassRef parser = qe::DefineClass(ctx, "DOMParser", ConstructDomParser, 0);
  qe::SetRealmData(ctx, &g_domParserKey, parser.prototype);
  qe::DefineMethod(parser.prototype, "parseFromString", ParseFromString, 2);
  qe::DefineGlobal(ctx, "DOMParser", parser.constructor);
}

}  // namespace

void InstallHtmlApis(Quanta::Embed::Realm& realm) { Install(realm); }
void InstallHtmlApis(Quanta::Embed::Runtime& runtime) { Install(runtime); }

}  // namespace solar::html
