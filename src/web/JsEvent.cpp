#include <chrono>
#include <cstdio>
#include <string>

#include "solar/web/DomBindingsInternal.h"
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

Value ComposedPath(Context& ctx, Value t, qe::Args, Value) {
  JsEvent* self = ThisEvent(ctx, t);
  if (!self) return qe::Undefined();
  Value path = qe::NewArray(ctx);
  if (self->dispatching && self->currentTarget) qe::ArrayPush(ctx, path, qe::FromObject(self->currentTarget));
  return path;
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

  if (qe::HasException(ctx)) {
    // The standard has the exception reported to the global object, where there is no handler
    // yet to hear it; it must not reach the code that dispatched the event.
    std::fprintf(stderr, "Uncaught (in event listener) %s\n", ctx.get_exception().to_string().c_str());
    ctx.clear_exception();
  }
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

// There is no tree yet, so an event goes to its target alone: capturing listeners first, then the rest.
bool DispatchOn(Context& ctx, JsEventTarget* target, JsEvent* event) {
  event->dispatching = true;
  event->target = target;
  event->currentTarget = target;
  event->phase = 2;
  event->NoteWrite(qe::FromObject(target));

  const std::vector<std::shared_ptr<Listener>> snapshot = target->listeners;
  for (bool capturing : {true, false}) {
    for (const std::shared_ptr<Listener>& listener : snapshot) {
      if (event->stopImmediatePropagation) break;
      if (listener->type != event->type || listener->capture != capturing) continue;
      Invoke(ctx, target, event, listener);
    }
  }

  event->dispatching = false;
  event->phase = 0;
  event->currentTarget = nullptr;
  event->stopPropagation = false;
  event->stopImmediatePropagation = false;
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

  qe::ClassRef target = qe::DefineClass(ctx, "EventTarget", ConstructTarget, 0);
  qe::SetRealmData(ctx, &g_targetPrototypeKey, target.prototype);
  qe::DefineMethod(target.prototype, "addEventListener", AddEventListener, 2);
  qe::DefineMethod(target.prototype, "removeEventListener", RemoveEventListener, 2);
  qe::DefineMethod(target.prototype, "dispatchEvent", DispatchEvent, 1);
  qe::DefineGlobal(ctx, "EventTarget", target.constructor);
}

}  // namespace solar::web
