#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "quanta/Embed.h"

namespace solar::web {

struct JsEventTarget;
struct JsAbortSignal;

// A DOMException: the name and message of what went wrong, and the legacy code its name has.
struct JsDomException : Quanta::DOMObject {
  std::string name;
  std::string message;
  uint16_t code = 0;
  void Visit(Quanta::Visitor&) {}
};

struct JsEvent : Quanta::DOMObject {
  std::string type;
  bool bubbles = false;
  bool cancelable = false;
  bool composed = false;
  bool defaultPrevented = false;
  bool stopPropagation = false;
  bool stopImmediatePropagation = false;
  bool inPassiveListener = false;
  bool dispatching = false;
  bool initialized = true;
  bool isTrusted = false;
  uint16_t phase = 0;  // 0 none, 1 capturing, 2 at target, 3 bubbling
  double timeStamp = 0;
  JsEventTarget* target = nullptr;
  JsEventTarget* currentTarget = nullptr;
  // The event's relatedTarget, which MouseEvent, FocusEvent and the like give it, and dispatch retargets.
  JsEventTarget* relatedTarget = nullptr;
  // One step of the event path: where the event is invoked, and what it is said to be at, with the related
  // target as seen from there.
  struct PathItem {
    JsEventTarget* invocationTarget = nullptr;
    JsEventTarget* shadowAdjustedTarget = nullptr;
    JsEventTarget* relatedTarget = nullptr;
    bool rootOfClosedTree = false;
    bool slotInClosedTree = false;
  };
  std::vector<PathItem> eventPath;

  void Visit(Quanta::Visitor& visitor);
};

struct JsCustomEvent : JsEvent {
  using Parent = JsEvent;
  Quanta::Value detail;  // null unless the init said otherwise
  void Visit(Quanta::Visitor& visitor) {
    JsEvent::Visit(visitor);
    visitor.Mark(detail);
  }
};

// One registration with addEventListener, or the slot an on<event> attribute stands in.
struct Listener {
  std::string type;
  Quanta::Value callback;          // a function, or an object with handleEvent; empty for the others
  std::function<void(Quanta::Context&, JsEvent*)> native;  // run instead of a callback, by the host
  Quanta::Object* owner = nullptr;  // what `native` needs to stay alive
  bool capture = false;
  bool passive = false;
  bool once = false;
  bool removed = false;
  bool isEventHandler = false;  // reads the target's on<type> attribute when it runs
};

// What an event target that sits in a tree (a node) tells dispatch about the tree, which a cell with no virtual
// functions cannot be asked: the standard's "get the parent" and "retarget" and the questions about shadow
// trees that the event path asks.
struct EventTargetOps {
  JsEventTarget* (*getParent)(JsEventTarget* self, JsEvent* event);
  JsEventTarget* (*retarget)(JsEventTarget* a, JsEventTarget* against);
  // A's root is a shadow-including inclusive ancestor of B.
  bool (*rootIncludes)(JsEventTarget* a, JsEventTarget* b);
  bool (*rootIsShadowRoot)(JsEventTarget* a);
  bool (*rootIsClosedShadowRoot)(JsEventTarget* a);
  bool (*isClosedShadowRoot)(JsEventTarget* a);
  bool (*isAssigned)(JsEventTarget* a);
};

struct JsEventTarget : Quanta::DOMObject {
  // Set by a type that sits in a tree; an event target without it is a path of one.
  const EventTargetOps* ops = nullptr;
  std::vector<std::shared_ptr<Listener>> listeners;
  // The on<type> attributes' values, which are callables or null.
  std::map<std::string, Quanta::Value> eventHandlers;

  void Visit(Quanta::Visitor& visitor);
};

struct JsAbortSignal : JsEventTarget {
  using Parent = JsEventTarget;

  bool aborted = false;
  Quanta::Value reason;
  // Signals made by AbortSignal.any from this one abort with it.
  std::vector<JsAbortSignal*> dependents;
  // For a signal made by AbortSignal.any: the signals it follows, none of which follows another.
  bool isDependent = false;
  std::vector<JsAbortSignal*> sources;
  // Listeners added with { signal: this }, to be removed when it aborts.
  struct Removal {
    JsEventTarget* target;
    std::shared_ptr<Listener> listener;
  };
  std::vector<Removal> removals;
  // Work the host wants done on abort: a fetch cancelling itself.
  std::vector<std::shared_ptr<Listener>> abortAlgorithms;

  void Visit(Quanta::Visitor& visitor);
};

// Delivers `event` to `target`'s listeners. False if a listener cancelled it.
bool DispatchOn(Quanta::Context& ctx, JsEventTarget* target, JsEvent* event);
JsEvent* NewEvent(Quanta::Context& ctx, const std::string& type, bool bubbles, bool cancelable);
// The on<type> attribute: the value of its slot, and setting it.
void SetEventHandler(JsEventTarget* target, const std::string& type, const Quanta::Value& handler);
Quanta::Value GetEventHandler(JsEventTarget* target, const std::string& type);
Quanta::Object* EventTargetPrototype(Quanta::Context& ctx);

void DefineDomExceptionClass(Quanta::Context& ctx);
void DefineEventClasses(Quanta::Context& ctx);
void DefineAbortClasses(Quanta::Context& ctx);

// A DOMException in the realm of `ctx`. Name decides the code, as the legacy table has it.
Quanta::Value NewDomException(Quanta::Context& ctx, const std::string& message, const std::string& name);
// Throws one: the pending exception is a DOMException.
void ThrowDomException(Quanta::Context& ctx, const std::string& message, const std::string& name);

// A signal that is not aborted, and one that is, in the realm of `ctx`.
JsAbortSignal* NewAbortSignal(Quanta::Context& ctx);
// Makes `follower` abort when `source` does, with its reason, and aborted at once if it already has.
void FollowSignal(Quanta::Context& ctx, JsAbortSignal* follower, JsAbortSignal* source);
// Aborts it as the standard's "signal abort" does: sets the reason (a DOMException "AbortError" when
// it is undefined), runs its algorithms, fires abort and aborts the signals that follow it.
void SignalAbort(Quanta::Context& ctx, JsAbortSignal* signal, Quanta::Value reason);

}  // namespace solar::web
