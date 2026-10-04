#include <string>
#include <vector>

#include "solar/css/CssBindings.h"
#include "solar/css/Selectors.h"
#include "solar/css/Tokenizer.h"
#include "solar/dom/NodeBindingsInternal.h"

namespace solar::css {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Object;
using Quanta::Value;

namespace {

// The selector in `args[0]`, or nullopt with the exception pending.
std::optional<SelectorList> ParseArgument(Context& ctx, qe::Args args, const char* member) {
  if (args.empty()) {
    qe::ThrowTypeError(ctx, std::string("Failed to execute '") + member + "': 1 argument required, but only 0 present.");
    return std::nullopt;
  }
  const std::string text = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return std::nullopt;
  std::optional<SelectorList> list = ParseSelectorList(text);
  if (!list) dom::Throw(ctx, {"SyntaxError", "Failed to execute '" + std::string(member) + "': '" + text + "' is not a valid selector."});
  return list;
}

MatchContext ContextFor(const dom::Node* node, const dom::Element* scope) {
  MatchContext context;
  context.scope = scope;
  const dom::Document* document = node->IsDocument() ? static_cast<const dom::Document*>(node) : node->nodeDocument;
  context.quirks = document && document->mode == dom::Document::Mode::Quirks;
  return context;
}

Value QuerySelector(Context& ctx, Value t, qe::Args args, Value) {
  dom::Node* self = dom::ThisNode(ctx, t);
  if (!self) return qe::Undefined();
  const auto list = ParseArgument(ctx, args, "querySelector");
  if (!list) return qe::Undefined();
  const MatchContext context = ContextFor(self, dom::AsElement(self));
  for (dom::Node* node = self->NextInTree(self); node; node = node->NextInTree(self)) {
    dom::Element* element = dom::AsElement(node);
    if (element && MatchesAny(*list, element, context)) return qe::FromObject(element);
  }
  return qe::Null();
}

Value QuerySelectorAll(Context& ctx, Value t, qe::Args args, Value) {
  dom::Node* self = dom::ThisNode(ctx, t);
  if (!self) return qe::Undefined();
  const auto list = ParseArgument(ctx, args, "querySelectorAll");
  if (!list) return qe::Undefined();
  const MatchContext context = ContextFor(self, dom::AsElement(self));
  std::vector<dom::Node*> found;
  for (dom::Node* node = self->NextInTree(self); node; node = node->NextInTree(self)) {
    dom::Element* element = dom::AsElement(node);
    if (element && MatchesAny(*list, element, context)) found.push_back(element);
  }
  return qe::FromObject(dom::NewStaticNodeList(ctx, std::move(found)));
}

Value Matches(Context& ctx, Value t, qe::Args args, Value) {
  dom::Element* self = dom::ThisElement(ctx, t);
  if (!self) return qe::Undefined();
  const auto list = ParseArgument(ctx, args, "matches");
  if (!list) return qe::Undefined();
  return qe::FromBool(MatchesAny(*list, self, ContextFor(self, self)));
}

Value Closest(Context& ctx, Value t, qe::Args args, Value) {
  dom::Element* self = dom::ThisElement(ctx, t);
  if (!self) return qe::Undefined();
  const auto list = ParseArgument(ctx, args, "closest");
  if (!list) return qe::Undefined();
  const MatchContext context = ContextFor(self, self);
  for (dom::Node* node = self; node; node = node->parentNode) {
    dom::Element* element = dom::AsElement(node);
    if (element && MatchesAny(*list, element, context)) return qe::FromObject(element);
  }
  return qe::Null();
}

Value Escape(Context& ctx, Value, qe::Args args, Value) {
  if (args.empty()) {
    qe::ThrowTypeError(ctx, "Failed to execute 'escape' on 'CSS': 1 argument required, but only 0 present.");
    return qe::Undefined();
  }
  const std::string text = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  return qe::FromWtf8(ctx, EscapeIdentifier(text));
}

template <typename Host>
void Install(Host& host) {
  Context& ctx = host.GetContext();
  for (dom::Interface interface : {dom::Interface::Element, dom::Interface::Document, dom::Interface::DocumentFragment}) {
    Object* prototype = dom::InterfacePrototype(ctx, interface);
    qe::DefineMethod(prototype, "querySelector", QuerySelector, 1);
    qe::DefineMethod(prototype, "querySelectorAll", QuerySelectorAll, 1);
  }
  Object* element = dom::InterfacePrototype(ctx, dom::Interface::Element);
  qe::DefineMethod(element, "matches", Matches, 1);
  qe::DefineMethod(element, "webkitMatchesSelector", Matches, 1);
  qe::DefineMethod(element, "closest", Closest, 1);
  // The CSS namespace object: an object with static operations and no constructor.
  host.Evaluate("globalThis.CSS = Object.create(Object.prototype); Object.defineProperty(CSS, Symbol.toStringTag, { value: 'CSS', configurable: true });");
  qe::DefineGlobalFunction(ctx, "__solarCssEscape", Escape, 1);
  host.Evaluate("Object.defineProperty(CSS, 'escape', { value: __solarCssEscape, writable: true, enumerable: true, configurable: true }); delete globalThis.__solarCssEscape;");
}

}  // namespace

void InstallSelectorApis(Quanta::Embed::Realm& realm) { Install(realm); }
void InstallSelectorApis(Quanta::Embed::Runtime& runtime) { Install(runtime); }

}  // namespace solar::css
