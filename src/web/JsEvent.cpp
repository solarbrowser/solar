#include <chrono>
#include <cstdio>
#include <string>

#include "solar/web/DomBindingsInternal.h"
#include "solar/web/ErrorReporting.h"
#include "solar/web/UrlBindingsInternal.h"

namespace solar::web {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Heap;
using Quanta::Object;
using Quanta::Value;

void JsEvent::Visit(Quanta::Visitor& visitor) {
  visitor.Mark(target);
  visitor.Mark(currentTarget);
  visitor.Mark(relatedTarget);
  for (const PathItem& item : eventPath) {
    visitor.Mark(item.invocationTarget);
    visitor.Mark(item.shadowAdjustedTarget);
    visitor.Mark(item.relatedTarget);
  }
}

void JsEventTarget::Visit(Quanta::Visitor& visitor) {
  for (const std::shared_ptr<Listener>& listener : listeners) {
    visitor.Mark(listener->callback);
    visitor.Mark(listener->owner);
  }
  for (auto& [type, handler] : eventHandlers) visitor.Mark(handler);
}

namespace {

char g_eventPrototypeKey;
char g_targetPrototypeKey;
char g_customEventPrototypeKey;

Object* EventPrototype(Context& ctx) { return static_cast<Object*>(qe::GetRealmData(ctx, &g_eventPrototypeKey)); }
Object* CustomEventPrototype(Context& ctx) { return static_cast<Object*>(qe::GetRealmData(ctx, &g_customEventPrototypeKey)); }
Object* TargetPrototype(Context& ctx) { return static_cast<Object*>(qe::GetRealmData(ctx, &g_targetPrototypeKey)); }

// Milliseconds since the process started, which is as good a time origin as any until there is a
// document to measure from. The origin is taken when the program is loaded, so that the first event
// does not have a time stamp of nothing.
const auto g_timeOrigin = std::chrono::steady_clock::now();

double Now() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_timeOrigin).count() + 1; }

JsEvent* ThisEvent(Context& ctx, const Value& thisValue) {
  JsEvent* self = DOMObject::Cast<JsEvent>(thisValue);
  if (!self) qe::ThrowTypeError(ctx, "Illegal invocation");
  return self;
}

JsEventTarget* ThisTarget(Context& ctx, const Value& thisValue) {
  JsEventTarget* self = DOMObject::Cast<JsEventTarget>(thisValue);
  if (!self) qe::ThrowTypeError(ctx, "Illegal invocation");
  return self;
}

// ---- Event ----

// The type and the EventInit dictionary of a constructor. `detail` is read too when it is asked for.
bool ReadEventArguments(Context& ctx, qe::Args args, const char* what, std::string& type, bool& bubbles, bool& cancelable, bool& composed, Value* detail) {
  if (!RequireArguments(ctx, args, 1, (std::string("Failed to construct '") + what + "'").c_str())) return false;
  type = qe::ToUsvUtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return false;
  if (args.size() > 1 && !qe::IsUndefined(args[1]) && !qe::IsNull(args[1])) {
    if (!qe::IsObject(args[1])) {
      qe::ThrowTypeError(ctx, std::string("Failed to construct '") + what + "': The provided value is not of type '" + what + "Init'.");
      return false;
    }
    for (auto [name, flag] : {std::pair<const char*, bool*>{"bubbles", &bubbles}, {"cancelable", &cancelable}, {"composed", &composed}}) {
      Value value = qe::Get(ctx, args[1], name);
      if (qe::HasException(ctx)) return false;
      *flag = value.to_boolean();
    }
    if (detail) {
      *detail = qe::Get(ctx, args[1], "detail");
      if (qe::HasException(ctx)) return false;
    }
  }
  return true;
}

Value ConstructEvent(Context& ctx, Value, qe::Args args, Value newTarget) {
  if (qe::IsUndefined(newTarget)) {
    qe::ThrowTypeError(ctx, "Failed to construct 'Event': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    return qe::Undefined();
  }
  std::string type;
  bool bubbles = false, cancelable = false, composed = false;
  if (!ReadEventArguments(ctx, args, "Event", type, bubbles, cancelable, composed, nullptr)) return qe::Undefined();
  Object* prototype = qe::PrototypeFromNewTarget(ctx, newTarget);
  if (qe::HasException(ctx)) return qe::Undefined();

  JsEvent* event = Heap::Allocate<JsEvent>();
  event->initialize_prototype(prototype ? prototype : EventPrototype(ctx));
  event->type = type;
  event->bubbles = bubbles;
  event->cancelable = cancelable;
  event->composed = composed;
  event->timeStamp = Now();
  return qe::FromObject(event);
}

Value ConstructCustomEvent(Context& ctx, Value, qe::Args args, Value newTarget) {
  if (qe::IsUndefined(newTarget)) {
    qe::ThrowTypeError(ctx, "Failed to construct 'CustomEvent': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    return qe::Undefined();
  }
  std::string type;
  bool bubbles = false, cancelable = false, composed = false;
  Value detail = qe::Null();
  if (!ReadEventArguments(ctx, args, "CustomEvent", type, bubbles, cancelable, composed, &detail)) return qe::Undefined();
  if (qe::IsUndefined(detail)) detail = qe::Null();
  Object* prototype = qe::PrototypeFromNewTarget(ctx, newTarget);
  if (qe::HasException(ctx)) return qe::Undefined();

  JsCustomEvent* event = Heap::Allocate<JsCustomEvent>();
  event->initialize_prototype(prototype ? prototype : CustomEventPrototype(ctx));
  event->type = type;
  event->bubbles = bubbles;
  event->cancelable = cancelable;
  event->composed = composed;
  event->timeStamp = Now();
  event->detail = detail;
  event->NoteWrite(detail);
  return qe::FromObject(event);
}

Value GetDetail(Context& ctx, Value t, qe::Args, Value) {
  JsCustomEvent* self = DOMObject::Cast<JsCustomEvent>(t);
  if (!self) {
    qe::ThrowTypeError(ctx, "Illegal invocation");
    return qe::Undefined();
  }
  return self->detail;
}

// initCustomEvent(type, bubbles, cancelable, detail): the old way to fill an event in.
Value InitCustomEvent(Context& ctx, Value t, qe::Args args, Value) {
  JsCustomEvent* self = DOMObject::Cast<JsCustomEvent>(t);
  if (!self) {
    qe::ThrowTypeError(ctx, "Illegal invocation");
    return qe::Undefined();
  }
  if (!RequireArguments(ctx, args, 1, "Failed to execute 'initCustomEvent' on 'CustomEvent'")) return qe::Undefined();
  const std::string type = qe::ToUsvUtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  if (self->dispatching) return qe::Undefined();
  self->type = type;
  self->bubbles = args.size() > 1 && args[1].to_boolean();
  self->cancelable = args.size() > 2 && args[2].to_boolean();
  self->detail = args.size() > 3 ? args[3] : qe::Null();
  self->defaultPrevented = false;
  self->stopPropagation = self->stopImmediatePropagation = false;
  self->NoteWrite(self->detail);
  return qe::Undefined();
}

Value GetType(Context& ctx, Value t, qe::Args, Value) {
  JsEvent* self = ThisEvent(ctx, t);
  return self ? qe::FromUtf8(ctx, self->type) : qe::Undefined();
}

Value GetTarget(Context& ctx, Value t, qe::Args, Value) {
  JsEvent* self = ThisEvent(ctx, t);
  return self && self->target ? qe::FromObject(self->target) : qe::Null();
}

Value GetCurrentTarget(Context& ctx, Value t, qe::Args, Value) {
  JsEvent* self = ThisEvent(ctx, t);
  return self && self->currentTarget ? qe::FromObject(self->currentTarget) : qe::Null();
}

Value GetEventPhase(Context& ctx, Value t, qe::Args, Value) {
  JsEvent* self = ThisEvent(ctx, t);
  return self ? qe::FromUint32(self->phase) : qe::Undefined();
}

Value GetBubbles(Context& ctx, Value t, qe::Args, Value) {
  JsEvent* self = ThisEvent(ctx, t);
  return self ? qe::FromBool(self->bubbles) : qe::Undefined();
}

Value GetCancelable(Context& ctx, Value t, qe::Args, Value) {
  JsEvent* self = ThisEvent(ctx, t);
  return self ? qe::FromBool(self->cancelable) : qe::Undefined();
}

Value GetComposed(Context& ctx, Value t, qe::Args, Value) {
  JsEvent* self = ThisEvent(ctx, t);
  return self ? qe::FromBool(self->composed) : qe::Undefined();
}

Value GetDefaultPrevented(Context& ctx, Value t, qe::Args, Value) {
  JsEvent* self = ThisEvent(ctx, t);
  return self ? qe::FromBool(self->defaultPrevented) : qe::Undefined();
}

Value GetIsTrusted(Context& ctx, Value t, qe::Args, Value) {
  JsEvent* self = ThisEvent(ctx, t);
  return self ? qe::FromBool(self->isTrusted) : qe::Undefined();
}

Value GetTimeStamp(Context& ctx, Value t, qe::Args, Value) {
  JsEvent* self = ThisEvent(ctx, t);
  return self ? Value(self->timeStamp) : qe::Undefined();
}

void PreventDefaultOf(JsEvent* event) {
  if (event->cancelable && !event->inPassiveListener) event->defaultPrevented = true;
}

Value PreventDefault(Context& ctx, Value t, qe::Args, Value) {
  if (JsEvent* self = ThisEvent(ctx, t)) PreventDefaultOf(self);
  return qe::Undefined();
}

Value StopPropagation(Context& ctx, Value t, qe::Args, Value) {
  if (JsEvent* self = ThisEvent(ctx, t)) self->stopPropagation = true;
  return qe::Undefined();
}

Value StopImmediatePropagation(Context& ctx, Value t, qe::Args, Value) {
  if (JsEvent* self = ThisEvent(ctx, t)) self->stopPropagation = self->stopImmediatePropagation = true;
  return qe::Undefined();
}

Value GetCancelBubble(Context& ctx, Value t, qe::Args, Value) {
  JsEvent* self = ThisEvent(ctx, t);
  return self ? qe::FromBool(self->stopPropagation) : qe::Undefined();
}

Value SetCancelBubble(Context& ctx, Value t, qe::Args args, Value) {
  JsEvent* self = ThisEvent(ctx, t);
  if (self && !args.empty() && args[0].to_boolean()) self->stopPropagation = true;
  return qe::Undefined();
}

Value GetReturnValue(Context& ctx, Value t, qe::Args, Value) {
  JsEvent* self = ThisEvent(ctx, t);
  return self ? qe::FromBool(!self->defaultPrevented) : qe::Undefined();
}

Value SetReturnValue(Context& ctx, Value t, qe::Args args, Value) {
  JsEvent* self = ThisEvent(ctx, t);
  if (self && !args.empty() && !args[0].to_boolean()) PreventDefaultOf(self);
  return qe::Undefined();
}

// initEvent(type, bubbles, cancelable): the old way to fill an event in.
// __solarEventSetRelatedTarget(event, target) and __solarEventRelatedTarget(event): where the events that have a
// relatedTarget keep it, which is where dispatch retargets it.
Value SetRelatedTarget(Context&, Value, qe::Args args, Value) {
  if (args.size() > 1) {
    if (JsEvent* event = DOMObject::Cast<JsEvent>(args[0])) {
      event->relatedTarget = DOMObject::Cast<JsEventTarget>(args[1]);
      event->NoteWrite();
    }
  }
  return qe::Undefined();
}

Value GetRelatedTarget(Context&, Value, qe::Args args, Value) {
  JsEvent* event = args.empty() ? nullptr : DOMObject::Cast<JsEvent>(args[0]);
  return event && event->relatedTarget ? qe::FromObject(event->relatedTarget) : qe::Null();
}

// __solarEventUninitialized(event): what createEvent leaves an event as, until it is initialized.
Value MakeUninitialized(Context&, Value, qe::Args args, Value) {
  if (!args.empty()) {
    if (JsEvent* event = DOMObject::Cast<JsEvent>(args[0])) event->initialized = false;
  }
  return qe::Undefined();
}

Value InitEvent(Context& ctx, Value t, qe::Args args, Value) {
  JsEvent* self = ThisEvent(ctx, t);
  if (!self) return qe::Undefined();
  if (!RequireArguments(ctx, args, 1, "Failed to execute 'initEvent' on 'Event'")) return qe::Undefined();
  const std::string type = qe::ToUsvUtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  if (self->dispatching) return qe::Undefined();
  self->type = type;
  self->bubbles = args.size() > 1 && args[1].to_boolean();
  self->cancelable = args.size() > 2 && args[2].to_boolean();
  self->defaultPrevented = false;
  self->stopPropagation = self->stopImmediatePropagation = false;
  self->target = nullptr;
  return qe::Undefined();
}

// What composedPath() gives: the path as the current target can see it, with what is in a closed shadow
// tree left out when the current target is outside of it.
Value ComposedPath(Context& ctx, Value t, qe::Args, Value) {
  JsEvent* self = ThisEvent(ctx, t);
  if (!self) return qe::Undefined();
  std::vector<JsEventTarget*> composed;
  const auto& path = self->eventPath;
  if (!path.empty() && self->currentTarget) {
    composed.push_back(self->currentTarget);
    long currentTargetIndex = 0;
    long currentTargetHiddenSubtreeLevel = 0;
    for (long index = static_cast<long>(path.size()) - 1; index >= 0; --index) {
      if (path[index].rootOfClosedTree) ++currentTargetHiddenSubtreeLevel;
      if (path[index].invocationTarget == self->currentTarget) {
        currentTargetIndex = index;
        break;
      }
      if (path[index].slotInClosedTree) --currentTargetHiddenSubtreeLevel;
    }
    long currentHiddenLevel = currentTargetHiddenSubtreeLevel;
    long maxHiddenLevel = currentTargetHiddenSubtreeLevel;
    for (long index = currentTargetIndex - 1; index >= 0; --index) {
      if (path[index].rootOfClosedTree) ++currentHiddenLevel;
      if (currentHiddenLevel <= maxHiddenLevel) composed.insert(composed.begin(), path[index].invocationTarget);
      if (path[index].slotInClosedTree) {
        --currentHiddenLevel;
        if (currentHiddenLevel < maxHiddenLevel) maxHiddenLevel = currentHiddenLevel;
      }
    }
    currentHiddenLevel = currentTargetHiddenSubtreeLevel;
    maxHiddenLevel = currentTargetHiddenSubtreeLevel;
    for (size_t index = static_cast<size_t>(currentTargetIndex) + 1; index < path.size(); ++index) {
      if (path[index].slotInClosedTree) ++currentHiddenLevel;
      if (currentHiddenLevel <= maxHiddenLevel) composed.push_back(path[index].invocationTarget);
      if (path[index].rootOfClosedTree) {
        --currentHiddenLevel;
        if (currentHiddenLevel < maxHiddenLevel) maxHiddenLevel = currentHiddenLevel;
      }
    }
  }
  Value array = qe::NewArray(ctx);
  for (JsEventTarget* node : composed) qe::ArrayPush(ctx, array, qe::FromObject(node));
  return array;
}

// ---- EventTarget ----

Value ConstructTarget(Context& ctx, Value, qe::Args, Value newTarget) {
  if (qe::IsUndefined(newTarget)) {
    qe::ThrowTypeError(ctx, "Failed to construct 'EventTarget': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    return qe::Undefined();
  }
  Object* prototype = qe::PrototypeFromNewTarget(ctx, newTarget);
  if (qe::HasException(ctx)) return qe::Undefined();
  JsEventTarget* target = Heap::Allocate<JsEventTarget>();
  target->initialize_prototype(prototype ? prototype : TargetPrototype(ctx));
  return qe::FromObject(target);
}

struct ListenerOptions {
  bool capture = false;
  bool passive = false;
  bool once = false;
  JsAbortSignal* signal = nullptr;
};

// The third argument of addEventListener: a boolean for capture, or a dictionary.
bool ReadAddOptions(Context& ctx, qe::Args args, ListenerOptions& out) {
  if (args.size() < 3 || qe::IsUndefined(args[2])) return true;
  if (!qe::IsObject(args[2])) {
    out.capture = args[2].to_boolean();
    return true;
  }
  for (auto [name, flag] : {std::pair<const char*, bool*>{"capture", &out.capture}, {"passive", &out.passive}, {"once", &out.once}}) {
    Value value = qe::Get(ctx, args[2], name);
    if (qe::HasException(ctx)) return false;
    *flag = value.to_boolean();
  }
  Value signal = qe::Get(ctx, args[2], "signal");
  if (qe::HasException(ctx)) return false;
  if (!qe::IsUndefined(signal)) {
    out.signal = DOMObject::Cast<JsAbortSignal>(signal);
    if (!out.signal) {
      qe::ThrowTypeError(ctx, "Failed to execute 'addEventListener' on 'EventTarget': Failed to read the 'signal' property from 'AddEventListenerOptions': Failed to convert value to 'AbortSignal'.");
      return false;
    }
  }
  return true;
}

bool ReadCallback(Context& ctx, qe::Args args, const char* method, Value& callback, bool& isNull) {
  isNull = args.size() < 2 || qe::IsNull(args[1]) || qe::IsUndefined(args[1]);
  if (isNull) return true;
  if (!qe::IsObject(args[1])) {
    qe::ThrowTypeError(ctx, std::string("Failed to execute '") + method + "' on 'EventTarget': The callback provided as parameter 2 is not an object.");
    return false;
  }
  callback = args[1];
  return true;
}

Value AddEventListener(Context& ctx, Value thisValue, qe::Args args, Value) {
  JsEventTarget* self = ThisTarget(ctx, thisValue);
  if (!self) return qe::Undefined();
  if (!RequireArguments(ctx, args, 2, "Failed to execute 'addEventListener' on 'EventTarget'")) return qe::Undefined();
  const std::string type = qe::ToUsvUtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  Value callback;
  bool isNull = false;
  if (!ReadCallback(ctx, args, "addEventListener", callback, isNull)) return qe::Undefined();
  ListenerOptions options;
  if (!ReadAddOptions(ctx, args, options)) return qe::Undefined();

  if (options.signal && options.signal->aborted) return qe::Undefined();
  if (isNull) return qe::Undefined();
  for (const std::shared_ptr<Listener>& existing : self->listeners) {
    if (!existing->isEventHandler && existing->type == type && existing->capture == options.capture && existing->callback.strict_equals(callback)) {
      return qe::Undefined();  // the same listener again does nothing
    }
  }
  auto listener = std::make_shared<Listener>();
  listener->type = type;
  listener->callback = callback;
  listener->capture = options.capture;
  listener->passive = options.passive;
  listener->once = options.once;
  self->listeners.push_back(listener);
  self->NoteWrite(callback);
  if (options.signal) {
    options.signal->removals.push_back({self, listener});
    options.signal->NoteWrite(qe::FromObject(self));
  }
  return qe::Undefined();
}

Value RemoveEventListener(Context& ctx, Value thisValue, qe::Args args, Value) {
  JsEventTarget* self = ThisTarget(ctx, thisValue);
  if (!self) return qe::Undefined();
  if (!RequireArguments(ctx, args, 2, "Failed to execute 'removeEventListener' on 'EventTarget'")) return qe::Undefined();
  const std::string type = qe::ToUsvUtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  Value callback;
  bool isNull = false;
  if (!ReadCallback(ctx, args, "removeEventListener", callback, isNull)) return qe::Undefined();
  bool capture = false;
  if (args.size() > 2 && !qe::IsUndefined(args[2])) {
    if (qe::IsObject(args[2])) {
      Value value = qe::Get(ctx, args[2], "capture");
      if (qe::HasException(ctx)) return qe::Undefined();
      capture = value.to_boolean();
    } else {
      capture = args[2].to_boolean();
    }
  }
  if (isNull) return qe::Undefined();
  for (auto it = self->listeners.begin(); it != self->listeners.end(); ++it) {
    if (!(*it)->isEventHandler && (*it)->type == type && (*it)->capture == capture && (*it)->callback.strict_equals(callback)) {
      (*it)->removed = true;
      self->listeners.erase(it);
      break;
    }
  }
  return qe::Undefined();
}

// Runs one listener for the event. An exception in it is reported and does not stop the others.
void Invoke(Context& ctx, JsEventTarget* target, JsEvent* event, const std::shared_ptr<Listener>& listener) {
  if (listener->removed) return;
  if (listener->once) {
    listener->removed = true;
    std::erase(target->listeners, listener);
  }
  event->inPassiveListener = listener->passive;

  if (listener->native) {
    listener->native(ctx, event);
  } else {
    Value callback = listener->callback;
    if (listener->isEventHandler) {
      auto it = target->eventHandlers.find(listener->type);
      callback = it == target->eventHandlers.end() ? qe::Null() : it->second;
    }
    Value eventValue = qe::FromObject(event);
    if (qe::IsCallable(callback)) {
      qe::Call(ctx, callback, qe::FromObject(target), qe::Args(&eventValue, 1));
    } else if (qe::IsObject(callback)) {
      Value handle = qe::Get(ctx, callback, "handleEvent");
      if (!qe::HasException(ctx) && qe::IsCallable(handle)) qe::Call(ctx, handle, callback, qe::Args(&eventValue, 1));
    }
  }
  event->inPassiveListener = false;

  // The standard has the exception reported to the global object; it must not reach the code that dispatched the event.
  if (qe::HasException(ctx)) ReportException(ctx);
}

Value DispatchEvent(Context& ctx, Value thisValue, qe::Args args, Value) {
  JsEventTarget* self = ThisTarget(ctx, thisValue);
  if (!self) return qe::Undefined();
  if (!RequireArguments(ctx, args, 1, "Failed to execute 'dispatchEvent' on 'EventTarget'")) return qe::Undefined();
  JsEvent* event = DOMObject::Cast<JsEvent>(args[0]);
  if (!event) {
    qe::ThrowTypeError(ctx, "Failed to execute 'dispatchEvent' on 'EventTarget': parameter 1 is not of type 'Event'.");
    return qe::Undefined();
  }
  if (event->dispatching || !event->initialized) {
    ThrowDomException(ctx, "Failed to execute 'dispatchEvent' on 'EventTarget': The event is already being dispatched.", "InvalidStateError");
    return qe::Undefined();
  }
  return qe::FromBool(DispatchOn(ctx, self, event));
}

}  // namespace

namespace {

// What a target can say about its tree, or the answers of one that sits in none.
JsEventTarget* GetParentOf(JsEventTarget* target, JsEvent* event) { return target->ops ? target->ops->getParent(target, event) : nullptr; }
JsEventTarget* RetargetOf(JsEventTarget* a, JsEventTarget* against) { return a && a->ops ? a->ops->retarget(a, against) : a; }
bool RootIsShadowRoot(JsEventTarget* a) { return a && a->ops && a->ops->rootIsShadowRoot(a); }
bool RootIsClosedShadowRoot(JsEventTarget* a) { return a && a->ops && a->ops->rootIsClosedShadowRoot(a); }
bool IsClosedShadowRoot(JsEventTarget* a) { return a && a->ops && a->ops->isClosedShadowRoot(a); }
bool IsAssigned(JsEventTarget* a) { return a && a->ops && a->ops->isAssigned(a); }

void AppendToPath(JsEvent* event, JsEventTarget* invocationTarget, JsEventTarget* shadowAdjustedTarget, JsEventTarget* relatedTarget, bool slotInClosedTree) {
  JsEvent::PathItem item;
  item.invocationTarget = invocationTarget;
  item.shadowAdjustedTarget = shadowAdjustedTarget;
  item.relatedTarget = relatedTarget;
  item.rootOfClosedTree = IsClosedShadowRoot(invocationTarget);
  item.slotInClosedTree = slotInClosedTree;
  event->eventPath.push_back(item);
}

// "invoke": the listeners of one step of the path, the capturing ones or the others.
void InvokeAt(Context& ctx, JsEvent* event, size_t index, uint16_t phase, bool capturing) {
  // The event is at what the last step up to this one that has a target is said to be.
  for (size_t i = index + 1; i-- > 0;) {
    if (event->eventPath[i].shadowAdjustedTarget) {
      event->target = event->eventPath[i].shadowAdjustedTarget;
      break;
    }
  }
  const JsEvent::PathItem item = event->eventPath[index];
  event->relatedTarget = item.relatedTarget;
  event->phase = phase;
  if (event->stopPropagation) return;
  JsEventTarget* current = item.invocationTarget;
  event->currentTarget = current;
  event->NoteWrite(qe::FromObject(current));
  const std::vector<std::shared_ptr<Listener>> snapshot = current->listeners;
  for (const std::shared_ptr<Listener>& listener : snapshot) {
    if (event->stopImmediatePropagation) break;
    if (listener->type != event->type || listener->capture != capturing) continue;
    Invoke(ctx, current, event, listener);
  }
}

}  // namespace

// The standard's dispatch: the path of the event is made first, from the target out through every parent (a
// shadow root's host, a slot for what is assigned to it), and then the event goes down it with the capturing
// listeners and back up it, if it bubbles, with the others.
bool DispatchOn(Context& ctx, JsEventTarget* target, JsEvent* event) {
  event->dispatching = true;
  event->target = target;
  event->eventPath.clear();
  JsEventTarget* const originalRelated = event->relatedTarget;
  JsEventTarget* relatedTarget = RetargetOf(originalRelated, target);
  bool clearTargets = false;

  if (target != relatedTarget || target == originalRelated) {
    AppendToPath(event, target, target, relatedTarget, false);
    JsEventTarget* slottable = IsAssigned(target) ? target : nullptr;
    bool slotInClosedTree = false;
    JsEventTarget* current = target;
    JsEventTarget* parent = GetParentOf(target, event);
    while (parent) {
      if (slottable) {
        slottable = nullptr;
        if (RootIsClosedShadowRoot(parent)) slotInClosedTree = true;
      }
      if (IsAssigned(parent)) slottable = parent;
      relatedTarget = RetargetOf(originalRelated, parent);
      // A parent that is no node is the window; one in the tree of the target, or a shadow-including ancestor of it, is on the way.
      const bool inTree = !parent->ops || (current->ops && current->ops->rootIncludes(current, parent));
      // The shadow-adjusted target is the parent when it is the first one past the boundary of a shadow tree.
      if (!parent->ops || inTree) {
        // The event stays at the same target: this is an ancestor of it, or the window.
        AppendToPath(event, parent, nullptr, relatedTarget, slotInClosedTree);
      } else if (parent == relatedTarget) {
        parent = nullptr;
      } else {
        current = parent;
        AppendToPath(event, parent, parent, relatedTarget, slotInClosedTree);
      }
      if (parent) parent = GetParentOf(parent, event);
      slotInClosedTree = false;
    }
    for (size_t i = event->eventPath.size(); i-- > 0;) {
      const JsEvent::PathItem& item = event->eventPath[i];
      if (item.shadowAdjustedTarget) {
        clearTargets = RootIsShadowRoot(item.shadowAdjustedTarget) || RootIsShadowRoot(item.relatedTarget);
        break;
      }
    }
    for (size_t i = event->eventPath.size(); i-- > 0;) InvokeAt(ctx, event, i, event->eventPath[i].shadowAdjustedTarget ? 2 : 1, true);
    for (size_t i = 0; i < event->eventPath.size(); ++i) {
      uint16_t phase = 2;
      if (!event->eventPath[i].shadowAdjustedTarget) {
        if (!event->bubbles) continue;
        phase = 3;
      }
      InvokeAt(ctx, event, i, phase, false);
    }
  }

  event->dispatching = false;
  event->phase = 0;
  event->currentTarget = nullptr;
  event->eventPath.clear();
  event->stopPropagation = false;
  event->stopImmediatePropagation = false;
  if (clearTargets) {
    event->target = nullptr;
    event->relatedTarget = nullptr;
  }
  return !event->defaultPrevented;
}

JsEvent* NewEvent(Context& ctx, const std::string& type, bool bubbles, bool cancelable) {
  JsEvent* event = Heap::Allocate<JsEvent>();
  event->initialize_prototype(EventPrototype(ctx));
  event->type = type;
  event->bubbles = bubbles;
  event->cancelable = cancelable;
  event->timeStamp = Now();
  event->isTrusted = true;  // made by the browser, not by a script
  return event;
}

void SetEventHandler(JsEventTarget* target, const std::string& type, const Value& handler) {
  const bool first = target->eventHandlers.find(type) == target->eventHandlers.end();
  target->eventHandlers[type] = handler;
  target->NoteWrite(handler);
  if (first) {
    // The handler stands in the listener list where it was first set, as the standard has it.
    auto listener = std::make_shared<Listener>();
    listener->type = type;
    listener->isEventHandler = true;
    target->listeners.push_back(listener);
  }
}

Value GetEventHandler(JsEventTarget* target, const std::string& type) {
  auto it = target->eventHandlers.find(type);
  return it == target->eventHandlers.end() ? qe::Null() : it->second;
}

Object* EventTargetPrototype(Context& ctx) { return TargetPrototype(ctx); }

void DefineEventClasses(Context& ctx) {
  qe::ClassRef event = qe::DefineClass(ctx, "Event", ConstructEvent, 1);
  qe::SetRealmData(ctx, &g_eventPrototypeKey, event.prototype);
  qe::DefineAccessor(event.prototype, "type", GetType, nullptr);
  qe::DefineAccessor(event.prototype, "target", GetTarget, nullptr);
  qe::DefineAccessor(event.prototype, "srcElement", GetTarget, nullptr);
  qe::DefineAccessor(event.prototype, "currentTarget", GetCurrentTarget, nullptr);
  qe::DefineAccessor(event.prototype, "eventPhase", GetEventPhase, nullptr);
  qe::DefineAccessor(event.prototype, "bubbles", GetBubbles, nullptr);
  qe::DefineAccessor(event.prototype, "cancelable", GetCancelable, nullptr);
  qe::DefineAccessor(event.prototype, "composed", GetComposed, nullptr);
  qe::DefineAccessor(event.prototype, "defaultPrevented", GetDefaultPrevented, nullptr);
  qe::DefineAccessor(event.prototype, "isTrusted", GetIsTrusted, nullptr);
  qe::DefineAccessor(event.prototype, "timeStamp", GetTimeStamp, nullptr);
  qe::DefineAccessor(event.prototype, "cancelBubble", GetCancelBubble, SetCancelBubble);
  qe::DefineAccessor(event.prototype, "returnValue", GetReturnValue, SetReturnValue);
  qe::DefineMethod(event.prototype, "preventDefault", PreventDefault, 0);
  qe::DefineMethod(event.prototype, "stopPropagation", StopPropagation, 0);
  qe::DefineMethod(event.prototype, "stopImmediatePropagation", StopImmediatePropagation, 0);
  qe::DefineMethod(event.prototype, "composedPath", ComposedPath, 0);
  qe::DefineMethod(event.prototype, "initEvent", InitEvent, 1);
  qe::DefineGlobal(ctx, "Event", event.constructor);

  qe::ClassRef custom = qe::DefineClass(ctx, "CustomEvent", ConstructCustomEvent, 1, event.prototype);
  qe::SetRealmData(ctx, &g_customEventPrototypeKey, custom.prototype);
  qe::DefineAccessor(custom.prototype, "detail", GetDetail, nullptr);
  qe::DefineMethod(custom.prototype, "initCustomEvent", InitCustomEvent, 1);
  qe::DefineGlobal(ctx, "CustomEvent", custom.constructor);

  qe::DefineGlobalFunction(ctx, "__solarEventUninitialized", MakeUninitialized, 1);
  qe::DefineGlobalFunction(ctx, "__solarEventSetRelatedTarget", SetRelatedTarget, 2);
  qe::DefineGlobalFunction(ctx, "__solarEventRelatedTarget", GetRelatedTarget, 1);

  qe::ClassRef target = qe::DefineClass(ctx, "EventTarget", ConstructTarget, 0);
  qe::SetRealmData(ctx, &g_targetPrototypeKey, target.prototype);
  qe::DefineMethod(target.prototype, "addEventListener", AddEventListener, 2);
  qe::DefineMethod(target.prototype, "removeEventListener", RemoveEventListener, 2);
  qe::DefineMethod(target.prototype, "dispatchEvent", DispatchEvent, 1);
  qe::DefineGlobal(ctx, "EventTarget", target.constructor);
}

}  // namespace solar::web
