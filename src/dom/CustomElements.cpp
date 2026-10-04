#include "solar/dom/CustomElements.h"

#include <algorithm>

#include "solar/dom/NodeBindingsInternal.h"

namespace solar::dom {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Object;
using Quanta::Value;

void CustomDefinition::Visit(Quanta::Visitor& visitor) {
  visitor.Mark(constructor);
  for (const Value& callback : callbacks) visitor.Mark(callback);
  for (Element* element : constructionStack) visitor.Mark(element);
}

namespace {

int g_stateKey;
int g_registryPrototypeKey;
bool g_anyDefinition = false;

struct Pending {
  std::string name;
  Value promise;
  Value resolve;
};

// What the registry and the reactions need, on the realm: the registry's definitions, the promises of whenDefined,
// the custom element reactions stack, and the few functions script has to do some of the work.
struct CeState : DOMObject {
  Value construct;        // (constructor) => new constructor()
  Value isConstructor;    // (value) => whether new value is possible
  Value report;           // (exception) => reports it to the window
  Value toStrings;        // (value) => a sequence of DOMString as an array
  Value queueMicrotask;
  Value processBackup;
  Value htmlElementConstructor;
  std::vector<CustomDefinition*> definitions;
  std::vector<Pending> pending;
  std::vector<std::vector<Element*>> stack;
  std::vector<Element*> backup;
  // The queues whose reactions are being run, which nothing else holds.
  std::vector<std::vector<Element*>> processing;
  bool backupQueued = false;
  bool defining = false;

  void Visit(Quanta::Visitor& visitor) {
    visitor.Mark(construct);
    visitor.Mark(isConstructor);
    visitor.Mark(report);
    visitor.Mark(toStrings);
    visitor.Mark(queueMicrotask);
    visitor.Mark(processBackup);
    visitor.Mark(htmlElementConstructor);
    for (CustomDefinition* definition : definitions) visitor.Mark(definition);
    for (const Pending& entry : pending) {
      visitor.Mark(entry.promise);
      visitor.Mark(entry.resolve);
    }
    for (const std::vector<Element*>& queue : stack) {
      for (Element* element : queue) visitor.Mark(element);
    }
    for (Element* element : backup) visitor.Mark(element);
    for (const std::vector<Element*>& queue : processing) {
      for (Element* element : queue) visitor.Mark(element);
    }
  }
};

CeState* StateOf(Context& ctx) { return static_cast<CeState*>(qe::GetRealmData(ctx, &g_stateKey)); }

struct JsRegistry : DOMObject {
  void Visit(Quanta::Visitor&) {}
};

Context* ContextOf(const Node* node) { return node && node->nodeDocument ? node->nodeDocument->context : nullptr; }

// The nodes of a tree in shadow-including tree order: a shadow root's tree comes before the children of its host.
std::vector<Node*> ShadowIncludingInclusiveDescendants(Node* root) {
  std::vector<Node*> out;
  std::vector<Node*> stack{root};
  while (!stack.empty()) {
    Node* node = stack.back();
    stack.pop_back();
    out.push_back(node);
    std::vector<Node*> children;
    for (Node* child = node->firstChild; child; child = child->nextSibling) children.push_back(child);
    for (auto it = children.rbegin(); it != children.rend(); ++it) stack.push_back(*it);
    if (Element* element = AsElement(node); element && element->shadowRoot) stack.push_back(element->shadowRoot);
  }
  return out;
}

bool IsConnected(Node* node) {
  Node* root = ShadowIncludingRoot(node);
  return root && root->IsDocument();
}

void EnqueueElement(Context& ctx, Element* element) {
  CeState* state = StateOf(ctx);
  if (!state) return;
  if (!state->stack.empty()) {
    state->stack.back().push_back(element);
    return;
  }
  state->backup.push_back(element);
  if (state->backupQueued) return;
  state->backupQueued = true;
  Value process = state->processBackup;
  qe::Call(ctx, state->queueMicrotask, qe::Undefined(), qe::Args(&process, 1));
  if (qe::HasException(ctx)) ctx.clear_exception();
}

void EnqueueUpgrade(Context& ctx, Element* element, CustomDefinition* definition) {
  CustomReaction reaction;
  reaction.upgrade = true;
  reaction.definition = definition;
  element->reactions.push_back(std::move(reaction));
  element->NoteWrite();
  EnqueueElement(ctx, element);
}

void RunReactions(Context& ctx, Element* element) {
  while (!element->reactions.empty()) {
    CustomReaction reaction = std::move(element->reactions.front());
    element->reactions.erase(element->reactions.begin());
    if (reaction.upgrade) {
      Upgrade(ctx, element, reaction.definition);
      continue;
    }
    qe::Call(ctx, reaction.callback, qe::FromObject(element), qe::Args(reaction.args.data(), reaction.args.size()));
    if (qe::HasException(ctx)) ReportException(ctx);
  }
}

// "invoke custom element reactions" in a queue.
void InvokeReactions(Context& ctx, std::vector<Element*> queue) {
  CeState* state = StateOf(ctx);
  const size_t level = state->processing.size();
  state->processing.push_back(std::move(queue));
  state->NoteWrite();
  for (size_t i = 0; i < state->processing[level].size(); ++i) RunReactions(ctx, state->processing[level][i]);
  state->processing.pop_back();
}

// The function of `callback` for an element's definition, and whether the element is in the state to have one called.
bool DefinitionHas(const CustomDefinition* definition, Callback callback) { return !definition->callbacks[static_cast<int>(callback)].is_undefined(); }

Value StringValue(Context& ctx, const std::string& text) { return qe::FromWtf8(ctx, text); }

Value OptionalStringValue(Context& ctx, const std::optional<std::string>& text) { return text ? qe::FromWtf8(ctx, *text) : qe::Null(); }

// An element with the name of a custom element or that of a built-in one it is to be, not yet one.
Element* NewUndefinedElement(Context& ctx, Document* document, std::string_view localName, std::string_view prefix, Object* prototype) {
  Element* element = NewElement(ctx, document, localName, kHtmlNamespace, prefix);
  element->customState = CustomState::Undefined;
  if (prototype) element->set_prototype(prototype);
  element->NoteWrite();
  return element;
}

}  // namespace

bool HasCustomDefinitions() { return g_anyDefinition; }

void ReportException(Context& ctx) {
  if (!qe::HasException(ctx)) return;
  const Value exception = ctx.get_exception();
  ctx.clear_exception();
  CeState* state = StateOf(ctx);
  if (state && qe::IsCallable(state->report)) {
    Value argument = exception;
    qe::Call(ctx, state->report, qe::Undefined(), qe::Args(&argument, 1));
    if (qe::HasException(ctx)) ctx.clear_exception();
  } else {
    std::fprintf(stderr, "Uncaught %s\n", exception.to_string().c_str());
  }
}

CustomDefinition* LookupDefinition(Context& ctx, const Document* document, std::string_view ns, std::string_view localName, const std::optional<std::string>& is) {
  if (!g_anyDefinition || ns != kHtmlNamespace || !document || (!document->window && !document->customElementsEnabled)) return nullptr;
  CeState* state = StateOf(ctx);
  if (!state) return nullptr;
  for (CustomDefinition* definition : state->definitions) {
    if (definition->name == localName && definition->localName == localName) return definition;
  }
  if (is) {
    for (CustomDefinition* definition : state->definitions) {
      if (definition->name == *is && definition->localName == localName) return definition;
    }
  }
  return nullptr;
}

void EnqueueCallback(Context& ctx, Element* element, Callback callback, std::vector<Value> args) {
  CustomDefinition* definition = element->customDefinition;
  if (!definition || !DefinitionHas(definition, callback)) return;
  if (callback == Callback::AttributeChanged) {
    const std::string name = args.empty() ? "" : args[0].to_string();
    if (std::find(definition->observedAttributes.begin(), definition->observedAttributes.end(), name) == definition->observedAttributes.end()) return;
  }
  CustomReaction reaction;
  reaction.callback = definition->callbacks[static_cast<int>(callback)];
  reaction.args = std::move(args);
  element->reactions.push_back(std::move(reaction));
  element->NoteWrite();
  EnqueueElement(ctx, element);
}

void TryUpgrade(Context& ctx, Element* element) {
  CustomDefinition* definition = LookupDefinition(ctx, element->nodeDocument, element->namespaceUri, element->localName, element->isValue);
  if (definition) EnqueueUpgrade(ctx, element, definition);
}

void Upgrade(Context& ctx, Element* element, CustomDefinition* definition) {
  if (element->customState != CustomState::Undefined && element->customState != CustomState::Uncustomized) return;
  element->customDefinition = definition;
  element->customState = CustomState::Failed;
  element->NoteWrite();
  for (Attr* attribute : std::vector<Attr*>(element->attributes)) {
    EnqueueCallback(ctx, element, Callback::AttributeChanged,
                    {StringValue(ctx, attribute->localName), qe::Null(), StringValue(ctx, attribute->value), attribute->namespaceUri.empty() ? qe::Null() : StringValue(ctx, attribute->namespaceUri)});
  }
  if (IsConnected(element)) EnqueueCallback(ctx, element, Callback::Connected);
  definition->constructionStack.push_back(element);
  definition->NoteWrite();
  CeState* state = StateOf(ctx);
  bool succeeded = false;
  if (definition->disableShadow && element->shadowRoot) {
    Throw(ctx, {"NotSupportedError", "The element has a shadow root, which this definition does not allow."});
  } else {
    element->customState = CustomState::Precustomized;
    Value constructor = definition->constructor;
    ++element->nodeDocument->throwOnDynamicMarkup;
    Value result = qe::Call(ctx, state->construct, qe::Undefined(), qe::Args(&constructor, 1));
    --element->nodeDocument->throwOnDynamicMarkup;
    if (!qe::HasException(ctx)) {
      if (DOMObject::Cast<Node>(result) != element) qe::ThrowTypeError(ctx, "The custom element constructor did not produce the element being upgraded.");
      else succeeded = true;
    }
  }
  if (!definition->constructionStack.empty()) definition->constructionStack.pop_back();
  if (!succeeded) {
    element->customDefinition = nullptr;
    element->customState = CustomState::Failed;
    element->reactions.clear();
    element->NoteWrite();
    ReportException(ctx);
    return;
  }
  element->customState = CustomState::Custom;
  element->NoteWrite();
}

Element* CreateElement(Context& ctx, Document* document, std::string_view localName, std::string_view ns, std::string_view prefix, const std::optional<std::string>& is, bool synchronous) {
  CustomDefinition* definition = LookupDefinition(ctx, document, ns, localName, is);
  if (definition && definition->name != definition->localName) {
    // A customized built-in element: the element of the built-in interface, made a custom one by the upgrade.
    Element* result = NewElement(ctx, document, localName, ns, prefix);
    result->customState = CustomState::Undefined;
    result->isValue = is;
    result->NoteWrite();
    if (synchronous) Upgrade(ctx, result, definition);
    else EnqueueUpgrade(ctx, result, definition);
    return result;
  }
  if (definition) {
    if (!synchronous) {
      Element* result = NewUndefinedElement(ctx, document, localName, prefix, nullptr);
      EnqueueUpgrade(ctx, result, definition);
      return result;
    }
    CeState* state = StateOf(ctx);
    Value constructor = definition->constructor;
    ++document->throwOnDynamicMarkup;
    Value constructed = qe::Call(ctx, state->construct, qe::Undefined(), qe::Args(&constructor, 1));
    --document->throwOnDynamicMarkup;
    Element* result = nullptr;
    if (!qe::HasException(ctx)) {
      Node* node = DOMObject::Cast<Node>(constructed);
      result = AsElement(node);
      if (!result || !result->IsHtml()) {
        qe::ThrowTypeError(ctx, "The custom element constructor did not produce an HTML element.");
      } else if (!result->attributes.empty()) {
        Throw(ctx, {"NotSupportedError", "The result must not have attributes"});
      } else if (result->firstChild) {
        Throw(ctx, {"NotSupportedError", "The result must not have children"});
      } else if (result->parentNode) {
        Throw(ctx, {"NotSupportedError", "The result must not have a parent"});
      } else if (result->nodeDocument != document) {
        Throw(ctx, {"NotSupportedError", "The result must be in the document it was made for"});
      } else if (result->localName != localName) {
        Throw(ctx, {"NotSupportedError", "The result must have the local name it was made for"});
      }
    }
    if (qe::HasException(ctx)) {
      ReportException(ctx);
      result = NewElement(ctx, document, localName, kHtmlNamespace, prefix);
      if (Object* unknown = UnknownHtmlElementPrototype(ctx)) result->set_prototype(unknown);
      result->customState = CustomState::Failed;
      result->NoteWrite();
      return result;
    }
    result->prefix = prefix;
    result->isValue = std::nullopt;
    result->customState = CustomState::Custom;
    result->customDefinition = definition;
    result->NoteWrite();
    return result;
  }
  Element* result = NewElement(ctx, document, localName, ns, prefix);
  if (ns == kHtmlNamespace && (IsValidCustomElementName(localName) || is)) result->customState = CustomState::Undefined;
  result->isValue = is;
  result->NoteWrite();
  return result;
}

void CustomAfterInsert(Node* node) {
  if (!g_anyDefinition) return;
  Context* ctx = ContextOf(node);
  if (!ctx || !IsConnected(node)) return;
  for (Node* descendant : ShadowIncludingInclusiveDescendants(node)) {
    Element* element = AsElement(descendant);
    if (!element) continue;
    if (element->customState == CustomState::Custom) EnqueueCallback(*ctx, element, Callback::Connected);
    else TryUpgrade(*ctx, element);
  }
}

void CustomAfterRemove(Node* node, bool parentWasConnected) {
  if (!g_anyDefinition || !parentWasConnected) return;
  Context* ctx = ContextOf(node);
  if (!ctx) return;
  for (Node* descendant : ShadowIncludingInclusiveDescendants(node)) {
    Element* element = AsElement(descendant);
    if (element && element->customState == CustomState::Custom) EnqueueCallback(*ctx, element, Callback::Disconnected);
  }
}

void CustomAfterAdopt(Node* node, Document* oldDocument) {
  if (!g_anyDefinition) return;
  Context* ctx = ContextOf(node);
  if (!ctx) return;
  for (Node* descendant : ShadowIncludingInclusiveDescendants(node)) {
    Element* element = AsElement(descendant);
    if (element && element->customState == CustomState::Custom) EnqueueCallback(*ctx, element, Callback::Adopted, {NodeValue(oldDocument), NodeValue(node->nodeDocument)});
  }
}

void CustomAfterMove(Node* node) {
  if (!g_anyDefinition) return;
  Context* ctx = ContextOf(node);
  if (!ctx) return;
  const bool connected = IsConnected(node);
  for (Node* descendant : ShadowIncludingInclusiveDescendants(node)) {
    Element* element = AsElement(descendant);
    if (!element || element->customState != CustomState::Custom || !connected) continue;
    if (element->customDefinition && DefinitionHas(element->customDefinition, Callback::ConnectedMove)) {
      EnqueueCallback(*ctx, element, Callback::ConnectedMove);
    } else {
      EnqueueCallback(*ctx, element, Callback::Disconnected);
      EnqueueCallback(*ctx, element, Callback::Connected);
    }
  }
}

void CustomAttributeChanged(Element* element, const std::string& name, const std::string& ns, const std::optional<std::string>& oldValue, const std::optional<std::string>& newValue) {
  if (!g_anyDefinition || element->customState != CustomState::Custom) return;
  Context* ctx = ContextOf(element);
  if (!ctx) return;
  EnqueueCallback(*ctx, element, Callback::AttributeChanged, {StringValue(*ctx, name), OptionalStringValue(*ctx, oldValue), OptionalStringValue(*ctx, newValue), ns.empty() ? qe::Null() : StringValue(*ctx, ns)});
}

ReactionsScope::ReactionsScope(Context& ctx) : ctx_(ctx) {
  if (CeState* state = StateOf(ctx_)) state->stack.emplace_back();
}

ReactionsScope::~ReactionsScope() {
  CeState* state = StateOf(ctx_);
  if (!state || state->stack.empty()) return;
  std::vector<Element*> queue = std::move(state->stack.back());
  state->stack.pop_back();
  if (queue.empty()) return;
  // The reactions are script, which has to run with no exception pending: the one the call is ending with waits.
  const bool had = qe::HasException(ctx_);
  const Value saved = had ? ctx_.get_exception() : Value();
  if (had) ctx_.clear_exception();
  InvokeReactions(ctx_, std::move(queue));
  if (had) ctx_.throw_exception(saved);
}

// ---- The HTMLElement constructor ----

Value ConstructHtmlElement(Context& ctx, const Value& newTarget, Object* active, const std::vector<std::string_view>& localNames) {
  CeState* state = StateOf(ctx);
  if (!state || !qe::IsObject(newTarget) || newTarget.as_object() == active) {
    qe::ThrowTypeError(ctx, "Illegal constructor");
    return qe::Undefined();
  }
  CustomDefinition* definition = nullptr;
  for (CustomDefinition* candidate : state->definitions) {
    if (candidate->constructor.is_object_like() && candidate->constructor.as_object() == newTarget.as_object()) definition = candidate;
  }
  if (!definition) {
    qe::ThrowTypeError(ctx, "Illegal constructor: the new.target is not a defined custom element.");
    return qe::Undefined();
  }
  if (definition->localName == definition->name) {
    if (!state->htmlElementConstructor.is_object_like() || active != state->htmlElementConstructor.as_object()) {
      qe::ThrowTypeError(ctx, "Illegal constructor: an autonomous custom element must extend HTMLElement.");
      return qe::Undefined();
    }
  } else if (std::find(localNames.begin(), localNames.end(), definition->localName) == localNames.end() &&
             // Many built-in elements have no interface of their own and are HTMLElements.
             !(state->htmlElementConstructor.is_object_like() && active == state->htmlElementConstructor.as_object() && HtmlElementPrototype(ctx, definition->localName) == InterfacePrototype(ctx, Interface::HtmlElement))) {
    qe::ThrowTypeError(ctx, "Illegal constructor: the customized built-in element does not extend the interface of the element it is defined for.");
    return qe::Undefined();
  }
  Value prototypeValue = qe::Get(ctx, newTarget, "prototype");
  if (qe::HasException(ctx)) return qe::Undefined();
  Object* prototype = qe::IsObject(prototypeValue) ? prototypeValue.as_object() : nullptr;
  if (!prototype) {
    prototype = HtmlElementPrototype(ctx, definition->localName);
    if (!prototype) prototype = InterfacePrototype(ctx, Interface::HtmlElement);
  }
  if (definition->constructionStack.empty()) {
    Document* document = AssociatedDocument(ctx);
    Element* element = NewUndefinedElement(ctx, document, definition->localName, "", prototype);
    if (definition->name != definition->localName) element->isValue = definition->name;
    return qe::FromObject(element);
  }
  Element* element = definition->constructionStack.back();
  if (!element) {
    qe::ThrowTypeError(ctx, "The custom element constructor was run more than once for the same element.");
    return qe::Undefined();
  }
  element->set_prototype(prototype);
  definition->constructionStack.back() = nullptr;
  return qe::FromObject(element);
}

Value HtmlElementConstructor(Context& ctx, Value, qe::Args, Value newTarget) {
  CeState* state = StateOf(ctx);
  Object* active = state && state->htmlElementConstructor.is_object_like() ? state->htmlElementConstructor.as_object() : nullptr;
  return ConstructHtmlElement(ctx, newTarget, active, {});
}

// ---- customElements ----

namespace {

JsRegistry* ThisRegistry(Context& ctx, const Value& t) {
  JsRegistry* registry = DOMObject::Cast<JsRegistry>(t);
  if (!registry) qe::ThrowTypeError(ctx, "Illegal invocation");
  return registry;
}

template <qe::NativeFn Fn>
Value WithReactions(Context& ctx, Value t, qe::Args args, Value newTarget) {
  ReactionsScope scope(ctx);
  return Fn(ctx, t, args, newTarget);
}

bool Missing(Context& ctx, qe::Args args, size_t count, const char* member) {
  if (args.size() >= count) return false;
  qe::ThrowTypeError(ctx, std::string("Failed to execute '") + member + "' on 'CustomElementRegistry': " + std::to_string(count) + " argument" + (count == 1 ? "" : "s") + " required, but only " +
                              std::to_string(args.size()) + " present.");
  return true;
}

CustomDefinition* FindByName(CeState* state, std::string_view name) {
  for (CustomDefinition* definition : state->definitions) {
    if (definition->name == name) return definition;
  }
  return nullptr;
}

Value RejectedSyntaxError(Context& ctx, const std::string& message) {
  qe::PromiseCapability capability = qe::NewPromiseCapability(ctx);
  Throw(ctx, {"SyntaxError", message});
  Value reason = ctx.get_exception();
  ctx.clear_exception();
  qe::Call(ctx, capability.reject, qe::Undefined(), qe::Args(&reason, 1));
  return capability.promise;
}

Value Define(Context& ctx, Value t, qe::Args args, Value) {
  if (!ThisRegistry(ctx, t) || Missing(ctx, args, 2, "define")) return qe::Undefined();
  CeState* state = StateOf(ctx);
  const std::string name = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  Value constructor = args[1];
  Value isConstructor = qe::Call(ctx, state->isConstructor, qe::Undefined(), qe::Args(&constructor, 1));
  if (qe::HasException(ctx)) return qe::Undefined();
  if (!isConstructor.to_boolean()) {
    qe::ThrowTypeError(ctx, "Failed to execute 'define' on 'CustomElementRegistry': parameter 2 is not of type 'Function'.");
    return qe::Undefined();
  }
  if (!IsValidCustomElementName(name)) {
    Throw(ctx, {"SyntaxError", "Failed to execute 'define' on 'CustomElementRegistry': \"" + name + "\" is not a valid custom element name"});
    return qe::Undefined();
  }
  if (FindByName(state, name)) {
    Throw(ctx, {"NotSupportedError", "Failed to execute 'define' on 'CustomElementRegistry': the name \"" + name + "\" has already been used with this registry"});
    return qe::Undefined();
  }
  for (CustomDefinition* definition : state->definitions) {
    if (definition->constructor.is_object_like() && definition->constructor.as_object() == constructor.as_object()) {
      Throw(ctx, {"NotSupportedError", "Failed to execute 'define' on 'CustomElementRegistry': this constructor has already been used with this registry"});
      return qe::Undefined();
    }
  }
  // The options: extends and disabledFeatures, read in this order.
  std::optional<std::string> extends;
  std::vector<std::string> disabled;
  if (args.size() > 2 && !qe::IsUndefined(args[2]) && !qe::IsNull(args[2])) {
    if (!qe::IsObject(args[2])) {
      qe::ThrowTypeError(ctx, "Failed to execute 'define' on 'CustomElementRegistry': parameter 3 is not of type 'ElementDefinitionOptions'.");
      return qe::Undefined();
    }
    Value extendsValue = qe::Get(ctx, args[2], "extends");
    if (qe::HasException(ctx)) return qe::Undefined();
    if (!qe::IsUndefined(extendsValue)) {
      extends = qe::ToWtf8(ctx, extendsValue);
      if (qe::HasException(ctx)) return qe::Undefined();
    }
  }
  std::string localName = name;
  if (extends) {
    if (IsValidCustomElementName(*extends)) {
      Throw(ctx, {"NotSupportedError", "Failed to execute 'define' on 'CustomElementRegistry': \"" + *extends + "\" is a valid custom element name"});
      return qe::Undefined();
    }
    if (IsUnknownHtmlElementName(ctx, *extends)) {
      Throw(ctx, {"NotSupportedError", "Failed to execute 'define' on 'CustomElementRegistry': \"" + *extends + "\" is not a valid built-in element name"});
      return qe::Undefined();
    }
    localName = *extends;
  }
  if (state->defining) {
    Throw(ctx, {"NotSupportedError", "Failed to execute 'define' on 'CustomElementRegistry': this name is already being defined"});
    return qe::Undefined();
  }
  // The flag is down again however this ends, which includes the engine leaving by way of a C++ exception.
  struct RunningFlag {
    CeState* state;
    explicit RunningFlag(CeState* s) : state(s) { state->defining = true; }
    ~RunningFlag() { state->defining = false; }
  };
  std::optional<RunningFlag> running(std::in_place, state);
  CustomDefinition* definition = Quanta::Heap::Allocate<CustomDefinition>();
  definition->initialize_prototype(nullptr);
  definition->name = name;
  definition->localName = localName;
  definition->constructor = constructor;
  // The callbacks of the prototype and the observed attributes of the constructor, which are read once, now.
  bool ok = false;
  do {
    Value prototype = qe::Get(ctx, constructor, "prototype");
    if (qe::HasException(ctx)) break;
    if (!qe::IsObject(prototype)) {
      qe::ThrowTypeError(ctx, "Failed to execute 'define' on 'CustomElementRegistry': The 'prototype' property of the constructor is not an object.");
      break;
    }
    static const char* const kByCallback[] = {"connectedCallback", "disconnectedCallback", "adoptedCallback", "attributeChangedCallback", "connectedMoveCallback"};
    bool failed = false;
    // The order the standard reads them in, which is not that of Callback.
    static const int kReadOrder[] = {0, 1, 4, 2, 3};
    for (int step = 0; step < 5 && !failed; ++step) {
      const int slot = kReadOrder[step];
      Value callback = qe::Get(ctx, prototype, kByCallback[slot]);
      if (qe::HasException(ctx)) {
        failed = true;
        break;
      }
      if (qe::IsUndefined(callback)) continue;
      if (!qe::IsCallable(callback)) {
        qe::ThrowTypeError(ctx, std::string("Failed to execute 'define' on 'CustomElementRegistry': The '") + kByCallback[slot] + "' property is not a function.");
        failed = true;
        break;
      }
      definition->callbacks[slot] = callback;
    }
    if (failed) break;
    if (DefinitionHas(definition, Callback::AttributeChanged)) {
      Value observed = qe::Get(ctx, constructor, "observedAttributes");
      if (qe::HasException(ctx)) break;
      if (!qe::IsUndefined(observed)) {
        Value list = qe::Call(ctx, state->toStrings, qe::Undefined(), qe::Args(&observed, 1));
        if (qe::HasException(ctx)) break;
        const uint32_t length = qe::ToUint32(ctx, qe::Get(ctx, list, "length"));
        for (uint32_t i = 0; i < length; ++i) {
          definition->observedAttributes.push_back(qe::ToWtf8(ctx, qe::Get(ctx, list, qe::FromWtf8(ctx, std::to_string(i)))));
          if (qe::HasException(ctx)) break;
        }
        if (qe::HasException(ctx)) break;
      }
    }
    Value features = qe::Get(ctx, constructor, "disabledFeatures");
    if (qe::HasException(ctx)) break;
    if (!qe::IsUndefined(features)) {
      Value list = qe::Call(ctx, state->toStrings, qe::Undefined(), qe::Args(&features, 1));
      if (qe::HasException(ctx)) break;
      const uint32_t length = qe::ToUint32(ctx, qe::Get(ctx, list, "length"));
      for (uint32_t i = 0; i < length; ++i) {
        const std::string feature = qe::ToWtf8(ctx, qe::Get(ctx, list, qe::FromWtf8(ctx, std::to_string(i))));
        if (qe::HasException(ctx)) break;
        if (feature == "shadow") definition->disableShadow = true;
        if (feature == "internals") definition->disableInternals = true;
      }
      if (qe::HasException(ctx)) break;
    }
    // formAssociated is read, and the callbacks that go with it, though form-associated custom elements are not made yet.
    const bool formAssociated = qe::Get(ctx, constructor, "formAssociated").to_boolean();
    if (qe::HasException(ctx)) break;
    if (formAssociated) {
      static const char* const kFormCallbacks[] = {"formAssociatedCallback", "formResetCallback", "formDisabledCallback", "formStateRestoreCallback"};
      bool bad = false;
      for (const char* name : kFormCallbacks) {
        Value callback = qe::Get(ctx, prototype, name);
        if (qe::HasException(ctx)) {
          bad = true;
          break;
        }
        if (!qe::IsUndefined(callback) && !qe::IsCallable(callback)) {
          qe::ThrowTypeError(ctx, std::string("Failed to execute 'define' on 'CustomElementRegistry': The '") + name + "' property is not a function.");
          bad = true;
          break;
        }
      }
      if (bad) break;
    }
    ok = true;
  } while (false);
  running.reset();
  if (!ok) return qe::Undefined();
  definition->NoteWrite();
  state->definitions.push_back(definition);
  g_anyDefinition = true;
  // The elements of the document that were waiting for this definition.
  if (Document* document = AssociatedDocument(ctx)) {
    for (Node* node : ShadowIncludingInclusiveDescendants(document)) {
      Element* element = AsElement(node);
      if (!element || !element->IsHtml() || element->localName != definition->localName) continue;
      if (definition->name != definition->localName && element->isValue != std::optional<std::string>(definition->name)) continue;
      EnqueueUpgrade(ctx, element, definition);
    }
  }
  for (auto it = state->pending.begin(); it != state->pending.end();) {
    if (it->name == name) {
      Value resolve = it->resolve;
      it = state->pending.erase(it);
      qe::Call(ctx, resolve, qe::Undefined(), qe::Args(&constructor, 1));
      if (qe::HasException(ctx)) ctx.clear_exception();
    } else {
      ++it;
    }
  }
  return qe::Undefined();
}

Value Get(Context& ctx, Value t, qe::Args args, Value) {
  if (!ThisRegistry(ctx, t) || Missing(ctx, args, 1, "get")) return qe::Undefined();
  const std::string name = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  CustomDefinition* definition = FindByName(StateOf(ctx), name);
  return definition ? definition->constructor : qe::Undefined();
}

Value GetName(Context& ctx, Value t, qe::Args args, Value) {
  if (!ThisRegistry(ctx, t) || Missing(ctx, args, 1, "getName")) return qe::Undefined();
  Value constructor = args[0];
  Value isConstructor = qe::Call(ctx, StateOf(ctx)->isConstructor, qe::Undefined(), qe::Args(&constructor, 1));
  if (qe::HasException(ctx)) return qe::Undefined();
  if (!isConstructor.to_boolean()) {
    qe::ThrowTypeError(ctx, "Failed to execute 'getName' on 'CustomElementRegistry': parameter 1 is not of type 'Function'.");
    return qe::Undefined();
  }
  for (CustomDefinition* definition : StateOf(ctx)->definitions) {
    if (definition->constructor.is_object_like() && definition->constructor.as_object() == constructor.as_object()) return StringValue(ctx, definition->name);
  }
  return qe::Null();
}

Value WhenDefined(Context& ctx, Value t, qe::Args args, Value) {
  if (!ThisRegistry(ctx, t)) return qe::Undefined();
  qe::PromiseCapability failure = qe::NewPromiseCapability(ctx);
  if (args.empty()) {
    qe::ThrowTypeError(ctx, "Failed to execute 'whenDefined' on 'CustomElementRegistry': 1 argument required, but only 0 present.");
    Value reason = ctx.get_exception();
    ctx.clear_exception();
    qe::Call(ctx, failure.reject, qe::Undefined(), qe::Args(&reason, 1));
    return failure.promise;
  }
  const std::string name = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) {
    Value reason = ctx.get_exception();
    ctx.clear_exception();
    qe::Call(ctx, failure.reject, qe::Undefined(), qe::Args(&reason, 1));
    return failure.promise;
  }
  if (!IsValidCustomElementName(name)) return RejectedSyntaxError(ctx, "Failed to execute 'whenDefined' on 'CustomElementRegistry': \"" + name + "\" is not a valid custom element name");
  CeState* state = StateOf(ctx);
  if (CustomDefinition* definition = FindByName(state, name)) {
    Value constructor = definition->constructor;
    qe::Call(ctx, failure.resolve, qe::Undefined(), qe::Args(&constructor, 1));
    return failure.promise;
  }
  for (const Pending& entry : state->pending) {
    if (entry.name == name) return entry.promise;
  }
  state->pending.push_back({name, failure.promise, failure.resolve});
  return failure.promise;
}

Value UpgradeMethod(Context& ctx, Value t, qe::Args args, Value) {
  if (!ThisRegistry(ctx, t) || Missing(ctx, args, 1, "upgrade")) return qe::Undefined();
  Node* root = NodeArgument(ctx, args, 0, "Failed to execute 'upgrade' on 'CustomElementRegistry'");
  if (!root) return qe::Undefined();
  for (Node* node : ShadowIncludingInclusiveDescendants(root)) {
    if (Element* element = AsElement(node)) TryUpgrade(ctx, element);
  }
  return qe::Undefined();
}

Value ConstructRegistry(Context& ctx, Value, qe::Args, Value) {
  qe::ThrowTypeError(ctx, "Illegal constructor");
  return qe::Undefined();
}

// What script has to give: the functions the algorithms are written in terms of. The registry is what it returns.
Value Init(Context& ctx, Value, qe::Args args, Value) {
  CeState* state = StateOf(ctx);
  if (!state || args.empty() || !qe::IsObject(args[0])) return qe::Undefined();
  state->construct = qe::Get(ctx, args[0], "construct");
  state->isConstructor = qe::Get(ctx, args[0], "isConstructor");
  state->report = qe::Get(ctx, args[0], "report");
  state->toStrings = qe::Get(ctx, args[0], "toStrings");
  state->NoteWrite();
  return qe::Undefined();
}

Value ProcessBackup(Context& ctx, Value, qe::Args, Value) {
  CeState* state = StateOf(ctx);
  if (!state) return qe::Undefined();
  std::vector<Element*> queue = std::move(state->backup);
  state->backup.clear();
  state->backupQueued = false;
  InvokeReactions(ctx, std::move(queue));
  return qe::Undefined();
}

Value NewRegistry(Context& ctx, Value, qe::Args, Value) {
  Object* prototype = static_cast<Object*>(qe::GetRealmData(ctx, &g_registryPrototypeKey));
  JsRegistry* registry = Quanta::Heap::Allocate<JsRegistry>();
  registry->initialize_prototype(prototype);
  return qe::FromObject(registry);
}

}  // namespace

void DefineCustomElementClasses(Context& ctx) {
  CeState* state = Quanta::Heap::Allocate<CeState>();
  state->initialize_prototype(nullptr);
  Value global = qe::FromObject(ctx.get_global_object());
  state->queueMicrotask = qe::Get(ctx, global, "queueMicrotask");
  qe::DefineGlobalFunction(ctx, "__solarProcessCustomElements", ProcessBackup, 0);
  state->processBackup = qe::Get(ctx, global, "__solarProcessCustomElements");
  state->htmlElementConstructor = qe::Get(ctx, global, "HTMLElement");
  state->NoteWrite();
  qe::SetRealmData(ctx, &g_stateKey, state);
  if (Object* holder = RealmHolder(ctx)) qe::Set(ctx, qe::FromObject(holder), "customElementsState", qe::FromObject(state));

  qe::ClassRef registry = qe::DefineClass(ctx, "CustomElementRegistry", ConstructRegistry, 0);
  qe::SetRealmData(ctx, &g_registryPrototypeKey, registry.prototype);
  qe::DefineMethod(registry.prototype, "define", WithReactions<Define>, 2);
  qe::DefineMethod(registry.prototype, "get", Get, 1);
  qe::DefineMethod(registry.prototype, "getName", GetName, 1);
  qe::DefineMethod(registry.prototype, "whenDefined", WhenDefined, 1);
  qe::DefineMethod(registry.prototype, "upgrade", WithReactions<UpgradeMethod>, 1);
  qe::DefineGlobal(ctx, "CustomElementRegistry", registry.constructor);
  qe::DefineGlobalFunction(ctx, "__solarCustomElementsInit", Init, 1);
  qe::DefineGlobalFunction(ctx, "__solarCustomElementsRegistry", NewRegistry, 0);
}

}  // namespace solar::dom
