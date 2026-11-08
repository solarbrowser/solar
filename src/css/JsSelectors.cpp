/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "solar/css/CssBindings.h"
#include "solar/css/Cssom.h"
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

// The selector in `args[0]` (a selector asked for again is not parsed again: scripts ask the same few over and over), or null with the exception pending.
std::shared_ptr<const SelectorList> ParseArgument(Context& ctx, qe::Args args, const char* member) {
  if (args.empty()) {
    qe::ThrowTypeError(ctx, std::string("Failed to execute '") + member + "': 1 argument required, but only 0 present.");
    return nullptr;
  }
  const std::string text = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return nullptr;
  static thread_local std::unordered_map<std::string, std::shared_ptr<const SelectorList>> parsed;
  const auto found = parsed.find(text);
  if (found != parsed.end()) return found->second;
  std::optional<SelectorList> list = ParseSelectorList(text);
  if (!list) {
    dom::Throw(ctx, {"SyntaxError", "Failed to execute '" + std::string(member) + "': '" + text + "' is not a valid selector."});
    return nullptr;
  }
  if (parsed.size() >= 2000) parsed.clear();
  return parsed[text] = std::make_shared<const SelectorList>(std::move(*list));
}

// The id of a selector that is nothing but `#id`, or null.
const std::string* OnlyId(const SelectorList& list) {
  if (list.size() != 1) return nullptr;
  const ComplexSelector& selector = list.front();
  if (selector.compounds.size() != 1 || !selector.combinators.empty() || selector.relative || selector.implicitNesting || selector.hideNesting) return nullptr;
  const CompoundSelector& compound = selector.compounds.front();
  if (compound.simples.size() != 1 || compound.simples.front().kind != SimpleSelector::Kind::Id) return nullptr;
  return &compound.simples.front().name;
}

// The elements of the tree of `self` that have the id, below it, in the order of the tree, found by the index of the ids of the document: for a node whose tree is the document's, and
// no quirks (an id is matched without case there). False where the tree has to be walked.
bool ElementsWithId(dom::Node* self, const std::string& id, bool quirks, std::vector<dom::Element*>& out) {
  if (quirks) return false;
  dom::Node* top = self;
  while (top->parentNode) top = top->parentNode;
  if (!top->IsDocument()) return false;
  if (const std::vector<dom::Element*>* found = dom::ElementsById(static_cast<dom::Document*>(top), id)) {
    for (dom::Element* element : *found) {
      if (element != self && (self == top || self->Contains(element))) out.push_back(element);
    }
  }
  return true;
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
  if (const std::string* id = OnlyId(*list)) {
    std::vector<dom::Element*> found;
    if (ElementsWithId(self, *id, context.quirks, found)) return found.empty() ? qe::Null() : qe::FromObject(found.front());
  }
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
  if (const std::string* id = OnlyId(*list)) {
    std::vector<dom::Element*> elements;
    if (ElementsWithId(self, *id, context.quirks, elements)) {
      found.assign(elements.begin(), elements.end());
      return qe::FromObject(dom::NewStaticNodeList(ctx, std::move(found)));
    }
  }
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
  DefineCssomClasses(ctx);
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
  InstallCssSupports(ctx);
  host.Evaluate("for (const name of ['CSSRuleList', 'StyleSheetList', 'MediaList', 'CSSStyleDeclaration']) Object.defineProperty(globalThis[name].prototype, Symbol.iterator, { value: Array.prototype[Symbol.iterator], writable: true, configurable: true });");
  host.Evaluate("Object.defineProperty(CSS, 'supports', { value: __solarCssSupports, writable: true, enumerable: true, configurable: true }); delete globalThis.__solarCssSupports; Object.defineProperty(CSS, 'registerProperty', { value: __solarCssRegisterProperty, writable: true, enumerable: true, configurable: true }); delete globalThis.__solarCssRegisterProperty;");
  // adoptedStyleSheets is an observable array: the engine's array behind a proxy that checks what goes in.
  host.Evaluate(R"JS((function () {
    const validate = globalThis.__solarAdoptValidate, changed = globalThis.__solarAdoptChanged;
    delete globalThis.__solarAdoptValidate;
    delete globalThis.__solarAdoptChanged;
    const backings = new WeakMap();
    const isIndex = (key) => typeof key === 'string' && /^(?:0|[1-9][0-9]*)$/.test(key) && Number(key) < 4294967295;
    const hide = (name, value) => Object.defineProperty(globalThis, name, { value, writable: true, configurable: true });
    hide('__solarMakeAdopted', function (owner) {
      const backing = [];
      const proxy = new Proxy(backing, {
        set(target, key, value) {
          if (isIndex(key)) {
            validate(owner, value);
            Reflect.defineProperty(target, key, { value, writable: true, enumerable: true, configurable: true });
          } else {
            Reflect.set(target, key, value, target);
          }
          changed();
          return true;
        },
        defineProperty(target, key, descriptor) {
          if (Object.hasOwn(descriptor, 'get') || Object.hasOwn(descriptor, 'set')) throw new TypeError("Failed to define property on 'adoptedStyleSheets': accessors are not allowed");
          if (isIndex(key) && Object.hasOwn(descriptor, 'value')) validate(owner, descriptor.value);
          const result = Reflect.defineProperty(target, key, descriptor);
          changed();
          return result;
        },
        deleteProperty(target, key) {
          const result = Reflect.deleteProperty(target, key);
          changed();
          return result;
        },
      });
      backings.set(proxy, backing);
      return proxy;
    });
    hide('__solarReplaceAdopted', function (proxy, items) {
      const backing = backings.get(proxy);
      backing.length = 0;
      for (let i = 0; i < items.length; ++i) Reflect.defineProperty(backing, String(i), { value: items[i], writable: true, enumerable: true, configurable: true });
    });
  })();)JS");
  host.Evaluate("Object.defineProperty(CSS, 'escape', { value: __solarCssEscape, writable: true, enumerable: true, configurable: true }); delete globalThis.__solarCssEscape;");
}

}  // namespace

void InstallSelectorApis(Quanta::Embed::Realm& realm) { Install(realm); }
void InstallSelectorApis(Quanta::Embed::Runtime& runtime) { Install(runtime); }

}  // namespace solar::css
