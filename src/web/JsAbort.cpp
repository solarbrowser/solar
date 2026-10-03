#include <algorithm>
#include <string>

#include "solar/web/DomBindingsInternal.h"
#include "solar/web/UrlBindingsInternal.h"
#include "solar/web/WebIdl.h"

namespace solar::web {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Heap;
using Quanta::Object;
using Quanta::Value;

void JsAbortSignal::Visit(Quanta::Visitor& visitor) {
  JsEventTarget::Visit(visitor);
  visitor.Mark(reason);
  for (JsAbortSignal* dependent : dependents) visitor.Mark(dependent);
  for (const Removal& removal : removals) {
    visitor.Mark(removal.target);
    visitor.Mark(removal.listener->callback);
  }
  for (const std::shared_ptr<Listener>& algorithm : abortAlgorithms) visitor.Mark(algorithm->owner);
  for (JsAbortSignal* source : sources) visitor.Mark(source);
}

namespace {

char g_signalPrototypeKey;
char g_controllerPrototypeKey;

Object* SignalPrototype(Context& ctx) { return static_cast<Object*>(qe::GetRealmData(ctx, &g_signalPrototypeKey)); }
Object* ControllerPrototype(Context& ctx) { return static_cast<Object*>(qe::GetRealmData(ctx, &g_controllerPrototypeKey)); }

struct JsAbortController : DOMObject {
  JsAbortSignal* signal = nullptr;
  void Visit(Quanta::Visitor& visitor) { visitor.Mark(signal); }
};

JsAbortSignal* ThisSignal(Context& ctx, const Value& thisValue) {
  JsAbortSignal* self = DOMObject::Cast<JsAbortSignal>(thisValue);
  if (!self) qe::ThrowTypeError(ctx, "Illegal invocation");
  return self;
}

JsAbortController* ThisController(Context& ctx, const Value& thisValue) {
  JsAbortController* self = DOMObject::Cast<JsAbortController>(thisValue);
  if (!self) qe::ThrowTypeError(ctx, "Illegal invocation");
  return self;
}

Value IllegalConstructor(Context& ctx, Value, qe::Args, Value) {
  qe::ThrowTypeError(ctx, "Illegal constructor");
  return qe::Undefined();
}

Value GetAborted(Context& ctx, Value t, qe::Args, Value) {
  JsAbortSignal* self = ThisSignal(ctx, t);
  return self ? qe::FromBool(self->aborted) : qe::Undefined();
}

Value GetReason(Context& ctx, Value t, qe::Args, Value) {
  JsAbortSignal* self = ThisSignal(ctx, t);
  return self ? self->reason : qe::Undefined();
}

Value ThrowIfAborted(Context& ctx, Value t, qe::Args, Value) {
  JsAbortSignal* self = ThisSignal(ctx, t);
  if (self && self->aborted) ctx.throw_exception(self->reason, /*raw=*/true);  // whatever it is, as it is
  return qe::Undefined();
}

Value GetOnAbort(Context& ctx, Value t, qe::Args, Value) {
  JsAbortSignal* self = ThisSignal(ctx, t);
  return self ? GetEventHandler(self, "abort") : qe::Undefined();
}

Value SetOnAbort(Context& ctx, Value t, qe::Args args, Value) {
  JsAbortSignal* self = ThisSignal(ctx, t);
  if (self) SetEventHandler(self, "abort", !args.empty() && qe::IsCallable(args[0]) ? args[0] : qe::Null());
  return qe::Undefined();
}

Value AbortStatic(Context& ctx, Value, qe::Args args, Value) {
  JsAbortSignal* signal = NewAbortSignal(ctx);
  signal->aborted = true;
  signal->reason = args.empty() || qe::IsUndefined(args[0]) ? NewDomException(ctx, "signal is aborted without reason", "AbortError") : args[0];
  signal->NoteWrite(signal->reason);
  return qe::FromObject(signal);
}

}  // namespace

void FollowSignal(Context& ctx, JsAbortSignal* follower, JsAbortSignal* source) {
  (void)ctx;
  if (source->aborted) {
    follower->aborted = true;
    follower->reason = source->reason;
    follower->NoteWrite(follower->reason);
    return;
  }
  const std::vector<JsAbortSignal*> sources = source->isDependent ? source->sources : std::vector<JsAbortSignal*>{source};
  for (JsAbortSignal* each : sources) {
    follower->sources.push_back(each);
    each->dependents.push_back(follower);
    each->NoteWrite(qe::FromObject(follower));
  }
  follower->isDependent = true;
}

namespace {

Value AnyStatic(Context& ctx, Value, qe::Args args, Value) {
  if (!RequireArguments(ctx, args, 1, "Failed to execute 'any' on 'AbortSignal'")) return qe::Undefined();
  std::vector<JsAbortSignal*> given;
  Value method = qe::IsObject(args[0]) ? qe::GetIteratorMethod(ctx, args[0]) : qe::Undefined();
  if (qe::HasException(ctx)) return qe::Undefined();
  if (qe::IsUndefined(method)) {
    qe::ThrowTypeError(ctx, "Failed to execute 'any' on 'AbortSignal': The provided value cannot be converted to a sequence.");
    return qe::Undefined();
  }
  bool ok = Iterate(ctx, args[0], method, [&](const Value& item) {
    JsAbortSignal* signal = DOMObject::Cast<JsAbortSignal>(item);
    if (!signal) {
      qe::ThrowTypeError(ctx, "Failed to execute 'any' on 'AbortSignal': Failed to convert value to 'AbortSignal'.");
      return false;
    }
    given.push_back(signal);
    return true;
  });
  if (!ok) return qe::Undefined();

  JsAbortSignal* result = NewAbortSignal(ctx);
  for (JsAbortSignal* signal : given) {
    if (signal->aborted) {
      result->aborted = true;
      result->reason = signal->reason;
      result->NoteWrite(result->reason);
      return qe::FromObject(result);
    }
  }
  // The result follows the signals that are not themselves followers; those that are bring their sources.
  for (JsAbortSignal* signal : given) {
    const std::vector<JsAbortSignal*> sources = signal->isDependent ? signal->sources : std::vector<JsAbortSignal*>{signal};
    for (JsAbortSignal* source : sources) {
      if (std::find(result->sources.begin(), result->sources.end(), source) != result->sources.end()) continue;
      result->sources.push_back(source);
      source->dependents.push_back(result);
      source->NoteWrite(qe::FromObject(result));
    }
  }
  result->isDependent = true;
  result->NoteWrite(qe::Undefined());
  return qe::FromObject(result);
}

Value ConstructController(Context& ctx, Value, qe::Args, Value newTarget) {
  if (qe::IsUndefined(newTarget)) {
    qe::ThrowTypeError(ctx, "Failed to construct 'AbortController': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    return qe::Undefined();
  }
  Object* prototype = qe::PrototypeFromNewTarget(ctx, newTarget);
  if (qe::HasException(ctx)) return qe::Undefined();
  JsAbortController* controller = Heap::Allocate<JsAbortController>();
  controller->initialize_prototype(prototype ? prototype : ControllerPrototype(ctx));
  controller->signal = NewAbortSignal(ctx);
  return qe::FromObject(controller);
}

Value GetSignal(Context& ctx, Value t, qe::Args, Value) {
  JsAbortController* self = ThisController(ctx, t);
  return self ? qe::FromObject(self->signal) : qe::Undefined();
}

Value Abort(Context& ctx, Value t, qe::Args args, Value) {
  JsAbortController* self = ThisController(ctx, t);
  if (self) SignalAbort(ctx, self->signal, args.empty() ? qe::Undefined() : args[0]);
  return qe::Undefined();
}

}  // namespace

JsAbortSignal* NewAbortSignal(Context& ctx) {
  JsAbortSignal* signal = Heap::Allocate<JsAbortSignal>();
  signal->initialize_prototype(SignalPrototype(ctx));
  return signal;
}

namespace {

// What happens to a signal that has been marked aborted: the things waiting on it are done with, the
// host's work is told, and the abort event is fired.
void RunAbortSteps(Context& ctx, JsAbortSignal* signal) {
  // Listeners added with { signal } come out, and the host's work is told.
  std::vector<JsAbortSignal::Removal> removals = std::move(signal->removals);
  signal->removals.clear();
  for (JsAbortSignal::Removal& removal : removals) {
    removal.listener->removed = true;
    std::erase(removal.target->listeners, removal.listener);
  }
  std::vector<std::shared_ptr<Listener>> algorithms = std::move(signal->abortAlgorithms);
  signal->abortAlgorithms.clear();
  for (const std::shared_ptr<Listener>& algorithm : algorithms) algorithm->native(ctx, nullptr);

  DispatchOn(ctx, signal, NewEvent(ctx, "abort", false, false));
}

}  // namespace

void SignalAbort(Context& ctx, JsAbortSignal* signal, Value reason) {
  if (signal->aborted) return;
  signal->aborted = true;
  signal->reason = qe::IsUndefined(reason) ? NewDomException(ctx, "signal is aborted without reason", "AbortError") : reason;
  signal->NoteWrite(signal->reason);

  // The signals that follow this one are marked aborted before anything runs, so that a listener
  // that looks at any of them sees them aborted.
  std::vector<JsAbortSignal*> toRun = {signal};
  for (JsAbortSignal* dependent : signal->dependents) {
    if (dependent->aborted) continue;
    dependent->aborted = true;
    dependent->reason = signal->reason;
    dependent->NoteWrite(dependent->reason);
    toRun.push_back(dependent);
  }
  for (JsAbortSignal* aborted : toRun) RunAbortSteps(ctx, aborted);
}

void DefineAbortClasses(Context& ctx) {
  qe::ClassRef signal = qe::DefineClass(ctx, "AbortSignal", IllegalConstructor, 0, EventTargetPrototype(ctx));
  qe::SetRealmData(ctx, &g_signalPrototypeKey, signal.prototype);
  qe::DefineAccessor(signal.prototype, "aborted", GetAborted, nullptr);
  qe::DefineAccessor(signal.prototype, "reason", GetReason, nullptr);
  qe::DefineAccessor(signal.prototype, "onabort", GetOnAbort, SetOnAbort);
  qe::DefineMethod(signal.prototype, "throwIfAborted", ThrowIfAborted, 0);
  qe::DefineStaticMethod(signal.constructor, "abort", AbortStatic, 0);
  qe::DefineStaticMethod(signal.constructor, "any", AnyStatic, 1);
  qe::DefineGlobal(ctx, "AbortSignal", signal.constructor);

  qe::ClassRef controller = qe::DefineClass(ctx, "AbortController", ConstructController, 0);
  qe::SetRealmData(ctx, &g_controllerPrototypeKey, controller.prototype);
  qe::DefineAccessor(controller.prototype, "signal", GetSignal, nullptr);
  qe::DefineMethod(controller.prototype, "abort", Abort, 0);
  qe::DefineGlobal(ctx, "AbortController", controller.constructor);
}

}  // namespace solar::web
