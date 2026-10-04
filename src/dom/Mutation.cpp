#include "solar/dom/Mutation.h"

#include <algorithm>

#include "solar/dom/NodeBindingsInternal.h"

namespace solar::dom {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Heap;
using Quanta::Object;
using Quanta::Value;

namespace {

size_t g_registrations = 0;
uint64_t g_nextId = 1;

char g_observerPrototypeKey;
char g_recordPrototypeKey;
char g_stateKey;

// A record as it was queued, before script is given one.
struct PendingRecord {
  std::string type;  // "childList", "attributes" or "characterData"
  Node* target = nullptr;
  std::vector<Node*> added;
  std::vector<Node*> removed;
  Node* previous = nullptr;
  Node* next = nullptr;
  std::string attributeName;
  std::string attributeNamespace;
  std::optional<std::string> oldValue;
};

struct JsMutationObserver : DOMObject {
  Context* ctx = nullptr;
  Value callback;
  std::vector<PendingRecord> queue;
  std::vector<Node*> nodes;  // those it is registered on, which it removes itself from
  bool pending = false;      // in the list of observers with records to give out

  void Visit(Quanta::Visitor& visitor) {
    visitor.Mark(callback);
    for (Node* node : nodes) visitor.Mark(node);
    for (const PendingRecord& record : queue) {
      visitor.Mark(record.target);
      visitor.Mark(record.previous);
      visitor.Mark(record.next);
      for (Node* node : record.added) visitor.Mark(node);
      for (Node* node : record.removed) visitor.Mark(node);
    }
  }
};

struct JsMutationRecord : DOMObject {
  PendingRecord record;
  Object* added = nullptr;    // the lists script gets, which are the same each time
  Object* removed = nullptr;

  void Visit(Quanta::Visitor& visitor) {
    visitor.Mark(record.target);
    visitor.Mark(record.previous);
    visitor.Mark(record.next);
    for (Node* node : record.added) visitor.Mark(node);
    for (Node* node : record.removed) visitor.Mark(node);
    visitor.Mark(added);
    visitor.Mark(removed);
  }
};

// What a realm needs to give records out: queueMicrotask and the function it is to queue, as they were when
// the realm was made, and the observers that have records.
struct MutationState : DOMObject {
  Value queueMicrotask;
  Value notify;
  std::vector<JsMutationObserver*> pending;
  bool queued = false;

  void Visit(Quanta::Visitor& visitor) {
    visitor.Mark(queueMicrotask);
    visitor.Mark(notify);
    for (JsMutationObserver* observer : pending) visitor.Mark(observer);
  }
};

MutationState* StateOf(Context& ctx) { return static_cast<MutationState*>(qe::GetRealmData(ctx, &g_stateKey)); }

void Schedule(JsMutationObserver* observer) {
  Context& ctx = *observer->ctx;
  MutationState* state = StateOf(ctx);
  if (!state) return;
  if (!observer->pending) {
    observer->pending = true;
    state->pending.push_back(observer);
    state->NoteWrite();
  }
  if (state->queued) return;
  state->queued = true;
  Value notify = state->notify;
  qe::Call(ctx, state->queueMicrotask, qe::Undefined(), qe::Args(&notify, 1));
}

bool Contains(const std::vector<std::string>& list, const std::string& name) { return std::find(list.begin(), list.end(), name) != list.end(); }

void Queue(PendingRecord base) {
  struct Interest {
    JsMutationObserver* observer;
    std::optional<std::string> oldValue;
  };
  std::vector<Interest> interested;
  for (Node* node = base.target; node; node = node->parentNode) {
    if (!node->registrations) continue;
    for (const Registration& registration : *node->registrations) {
      const MutationOptions& options = registration.options;
      if (node != base.target && !options.subtree) continue;
      if (base.type == "attributes") {
        if (!options.attributes) continue;
        if (options.hasAttributeFilter && (!base.attributeNamespace.empty() || !Contains(options.attributeFilter, base.attributeName))) continue;
      } else if (base.type == "characterData") {
        if (!options.characterData) continue;
      } else if (!options.childList) {
        continue;
      }
      auto* observer = static_cast<JsMutationObserver*>(registration.observer);
      auto found = std::find_if(interested.begin(), interested.end(), [&](const Interest& i) { return i.observer == observer; });
      if (found == interested.end()) {
        interested.push_back({observer, std::nullopt});
        found = interested.end() - 1;
      }
      if ((base.type == "attributes" && options.attributeOldValue) || (base.type == "characterData" && options.characterDataOldValue)) found->oldValue = base.oldValue;
    }
  }
  for (Interest& interest : interested) {
    PendingRecord record = base;
    record.oldValue = interest.oldValue;
    interest.observer->queue.push_back(std::move(record));
    interest.observer->NoteWrite();
    Schedule(interest.observer);
  }
}

JsMutationObserver* ThisObserver(Context& ctx, const Value& t) {
  JsMutationObserver* observer = DOMObject::Cast<JsMutationObserver>(t);
  if (!observer) qe::ThrowTypeError(ctx, "Illegal invocation");
  return observer;
}

void RemoveRegistrations(Node* node, const std::function<bool(const Registration&)>& predicate) {
  if (!node->registrations) return;
  auto& list = *node->registrations;
  const size_t before = list.size();
  std::erase_if(list, predicate);
  g_registrations -= before - list.size();
}

Value MakeRecord(Context& ctx, const PendingRecord& record) {
  JsMutationRecord* object = Heap::Allocate<JsMutationRecord>();
  object->initialize_prototype(static_cast<Object*>(qe::GetRealmData(ctx, &g_recordPrototypeKey)));
  object->record = record;
  return qe::FromObject(object);
}

Value RecordsArray(Context& ctx, std::vector<PendingRecord>& records) {
  Value array = qe::NewArray(ctx);
  for (const PendingRecord& record : records) qe::ArrayPush(ctx, array, MakeRecord(ctx, record));
  return array;
}

// "notify mutation observers", run as a microtask.
Value Notify(Context& ctx, Value, qe::Args, Value) {
  MutationState* state = StateOf(ctx);
  if (!state) return qe::Undefined();
  state->queued = false;
  std::vector<JsMutationObserver*> observers = std::move(state->pending);
  state->pending.clear();
  for (JsMutationObserver* observer : observers) {
    observer->pending = false;
    std::vector<PendingRecord> records = std::move(observer->queue);
    observer->queue.clear();
    // The transient registrations were for the records that have been taken.
    for (Node* node : observer->nodes) RemoveRegistrations(node, [&](const Registration& r) { return r.transient && r.observer == observer; });
    if (records.empty()) continue;
    Value array = RecordsArray(ctx, records);
    if (qe::HasException(ctx)) {
      ctx.clear_exception();
      continue;
    }
    Value self = qe::FromObject(observer);
    Value args[2] = {array, self};
    qe::Call(ctx, observer->callback, self, qe::Args(args, 2));
    // An exception in a callback is reported, and the other observers still get theirs.
    if (qe::HasException(ctx)) ctx.clear_exception();
  }
  return qe::Undefined();
}

Value ConstructObserver(Context& ctx, Value, qe::Args args, Value newTarget) {
  if (qe::IsUndefined(newTarget)) {
    qe::ThrowTypeError(ctx, "Failed to construct 'MutationObserver': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    return qe::Undefined();
  }
  Object* prototype = qe::PrototypeFromNewTarget(ctx, newTarget);
  if (qe::HasException(ctx)) return qe::Undefined();
  if (args.empty()) {
    qe::ThrowTypeError(ctx, "Failed to construct 'MutationObserver': 1 argument required, but only 0 present.");
    return qe::Undefined();
  }
  if (!qe::IsCallable(args[0])) {
    qe::ThrowTypeError(ctx, "Failed to construct 'MutationObserver': The callback provided as parameter 1 is not a function.");
    return qe::Undefined();
  }
  JsMutationObserver* observer = Heap::Allocate<JsMutationObserver>();
  observer->initialize_prototype(prototype ? prototype : static_cast<Object*>(qe::GetRealmData(ctx, &g_observerPrototypeKey)));
  observer->ctx = &ctx;
  observer->callback = args[0];
  observer->NoteWrite(args[0]);
  return qe::FromObject(observer);
}

// A member of the options that was given, as a boolean; `present` says whether it was.
bool OptionFlag(Context& ctx, const Value& options, const char* name, bool& present) {
  Value value = qe::Get(ctx, options, name);
  present = !qe::IsUndefined(value);
  return present && value.to_boolean();
}

Value Observe(Context& ctx, Value t, qe::Args args, Value) {
  JsMutationObserver* self = ThisObserver(ctx, t);
  if (!self) return qe::Undefined();
  if (args.empty()) {
    qe::ThrowTypeError(ctx, "Failed to execute 'observe' on 'MutationObserver': 1 argument required, but only 0 present.");
    return qe::Undefined();
  }
  Node* target = DOMObject::Cast<Node>(args[0]);
  if (!target) {
    qe::ThrowTypeError(ctx, "Failed to execute 'observe' on 'MutationObserver': parameter 1 is not of type 'Node'.");
    return qe::Undefined();
  }
  MutationOptions options;
  const Value init = args.size() > 1 ? args[1] : qe::Undefined();
  if (!qe::IsUndefined(init) && !qe::IsNull(init) && !qe::IsObject(init)) {
    qe::ThrowTypeError(ctx, "Failed to execute 'observe' on 'MutationObserver': The provided value is not of type 'MutationObserverInit'.");
    return qe::Undefined();
  }
  bool hasChildList = false, hasAttributes = false, hasCharacterData = false, hasSubtree = false, hasAttributeOldValue = false, hasCharacterDataOldValue = false;
  if (qe::IsObject(init)) {
    // In the alphabetical order of a dictionary's members.
    Value filter = qe::Get(ctx, init, "attributeFilter");
    if (qe::HasException(ctx)) return qe::Undefined();
    options.attributeOldValue = OptionFlag(ctx, init, "attributeOldValue", hasAttributeOldValue);
    options.attributes = OptionFlag(ctx, init, "attributes", hasAttributes);
    options.characterData = OptionFlag(ctx, init, "characterData", hasCharacterData);
    options.characterDataOldValue = OptionFlag(ctx, init, "characterDataOldValue", hasCharacterDataOldValue);
    options.childList = OptionFlag(ctx, init, "childList", hasChildList);
    options.subtree = OptionFlag(ctx, init, "subtree", hasSubtree);
    if (qe::HasException(ctx)) return qe::Undefined();
    if (!qe::IsUndefined(filter)) {
      options.hasAttributeFilter = true;
      Value lengthValue = qe::Get(ctx, filter, "length");
      const uint32_t length = qe::HasException(ctx) ? 0 : qe::ToUint32(ctx, lengthValue);
      for (uint32_t i = 0; i < length && !qe::HasException(ctx); ++i) {
        Value item = qe::Get(ctx, filter, qe::FromWtf8(ctx, std::to_string(i)));
        options.attributeFilter.push_back(qe::HasException(ctx) ? "" : qe::ToWtf8(ctx, item));
      }
      if (qe::HasException(ctx)) return qe::Undefined();
    }
  }
  if ((hasAttributeOldValue || options.hasAttributeFilter) && !hasAttributes) options.attributes = true;
  if (hasCharacterDataOldValue && !hasCharacterData) options.characterData = true;
  const char* problem = nullptr;
  if (!options.childList && !options.attributes && !options.characterData) problem = "The options object must set at least one of 'attributes', 'characterData', or 'childList' to true.";
  else if (options.attributeOldValue && hasAttributes && !options.attributes) problem = "The options object may only set 'attributeOldValue' to true when 'attributes' is true or not present.";
  else if (options.hasAttributeFilter && hasAttributes && !options.attributes) problem = "The options object may only set 'attributeFilter' when 'attributes' is true or not present.";
  else if (options.characterDataOldValue && hasCharacterData && !options.characterData) problem = "The options object may only set 'characterDataOldValue' to true when 'characterData' is true or not present.";
  if (problem) {
    qe::ThrowTypeError(ctx, std::string("Failed to execute 'observe' on 'MutationObserver': ") + problem);
    return qe::Undefined();
  }

  if (!target->registrations) target->registrations = std::make_unique<std::vector<Registration>>();
  for (Registration& registration : *target->registrations) {
    if (registration.observer != self || registration.transient) continue;
    for (Node* node : self->nodes) RemoveRegistrations(node, [&](const Registration& r) { return r.transient && r.source == registration.id; });
    registration.options = options;
    return qe::Undefined();
  }
  Registration registration;
  registration.observer = self;
  registration.options = options;
  registration.id = g_nextId++;
  target->registrations->push_back(std::move(registration));
  ++g_registrations;
  self->nodes.push_back(target);
  self->NoteWrite();
  target->NoteWrite();
  return qe::Undefined();
}

Value Disconnect(Context& ctx, Value t, qe::Args, Value) {
  JsMutationObserver* self = ThisObserver(ctx, t);
  if (!self) return qe::Undefined();
  for (Node* node : self->nodes) RemoveRegistrations(node, [&](const Registration& r) { return r.observer == self; });
  self->nodes.clear();
  self->queue.clear();
  return qe::Undefined();
}

Value TakeRecords(Context& ctx, Value t, qe::Args, Value) {
  JsMutationObserver* self = ThisObserver(ctx, t);
  if (!self) return qe::Undefined();
  std::vector<PendingRecord> records = std::move(self->queue);
  self->queue.clear();
  return RecordsArray(ctx, records);
}

// ---- MutationRecord ----

JsMutationRecord* ThisRecord(Context& ctx, const Value& t) {
  JsMutationRecord* record = DOMObject::Cast<JsMutationRecord>(t);
  if (!record) qe::ThrowTypeError(ctx, "Illegal invocation");
  return record;
}

Value IllegalConstructor(Context& ctx, Value, qe::Args, Value) {
  qe::ThrowTypeError(ctx, "Illegal constructor");
  return qe::Undefined();
}

Value GetRecordType(Context& ctx, Value t, qe::Args, Value) {
  JsMutationRecord* self = ThisRecord(ctx, t);
  return self ? qe::FromWtf8(ctx, self->record.type) : qe::Undefined();
}
Value GetRecordTarget(Context& ctx, Value t, qe::Args, Value) {
  JsMutationRecord* self = ThisRecord(ctx, t);
  return self ? NodeValue(self->record.target) : qe::Undefined();
}
Value GetAddedNodes(Context& ctx, Value t, qe::Args, Value) {
  JsMutationRecord* self = ThisRecord(ctx, t);
  if (!self) return qe::Undefined();
  if (!self->added) {
    self->added = NewStaticNodeList(ctx, self->record.added);
    self->NoteWrite();
  }
  return qe::FromObject(self->added);
}
Value GetRemovedNodes(Context& ctx, Value t, qe::Args, Value) {
  JsMutationRecord* self = ThisRecord(ctx, t);
  if (!self) return qe::Undefined();
  if (!self->removed) {
    self->removed = NewStaticNodeList(ctx, self->record.removed);
    self->NoteWrite();
  }
  return qe::FromObject(self->removed);
}
Value GetRecordPrevious(Context& ctx, Value t, qe::Args, Value) {
  JsMutationRecord* self = ThisRecord(ctx, t);
  return self ? NodeValue(self->record.previous) : qe::Undefined();
}
Value GetRecordNext(Context& ctx, Value t, qe::Args, Value) {
  JsMutationRecord* self = ThisRecord(ctx, t);
  return self ? NodeValue(self->record.next) : qe::Undefined();
}
Value GetAttributeName(Context& ctx, Value t, qe::Args, Value) {
  JsMutationRecord* self = ThisRecord(ctx, t);
  if (!self) return qe::Undefined();
  return self->record.type == "attributes" ? qe::FromWtf8(ctx, self->record.attributeName) : qe::Null();
}
Value GetAttributeNamespace(Context& ctx, Value t, qe::Args, Value) {
  JsMutationRecord* self = ThisRecord(ctx, t);
  if (!self) return qe::Undefined();
  return self->record.type == "attributes" && !self->record.attributeNamespace.empty() ? qe::FromWtf8(ctx, self->record.attributeNamespace) : qe::Null();
}
Value GetOldValue(Context& ctx, Value t, qe::Args, Value) {
  JsMutationRecord* self = ThisRecord(ctx, t);
  if (!self) return qe::Undefined();
  return self->record.oldValue ? qe::FromWtf8(ctx, *self->record.oldValue) : qe::Null();
}

}  // namespace

bool HasMutationObservers() { return g_registrations > 0; }

void QueueChildListRecord(Node* target, const std::vector<Node*>& added, const std::vector<Node*>& removed, Node* previousSibling, Node* nextSibling) {
  PendingRecord record;
  record.type = "childList";
  record.target = target;
  record.added = added;
  record.removed = removed;
  record.previous = previousSibling;
  record.next = nextSibling;
  Queue(std::move(record));
}

void QueueAttributeRecord(Element* element, const std::string& name, const std::string& namespaceUri, const std::optional<std::string>& oldValue) {
  PendingRecord record;
  record.type = "attributes";
  record.target = element;
  record.attributeName = name;
  record.attributeNamespace = namespaceUri;
  record.oldValue = oldValue;
  Queue(std::move(record));
}

void QueueCharacterDataRecord(Node* node, const std::string& oldValue) {
  PendingRecord record;
  record.type = "characterData";
  record.target = node;
  record.oldValue = oldValue;
  Queue(std::move(record));
}

void RegisterTransientObservers(Node* node, Node* oldParent) {
  for (Node* ancestor = oldParent; ancestor; ancestor = ancestor->parentNode) {
    if (!ancestor->registrations) continue;
    // Copied: the node's own list may be the one being added to.
    const std::vector<Registration> registrations = *ancestor->registrations;
    for (const Registration& registration : registrations) {
      if (!registration.options.subtree || registration.transient) continue;
      if (!node->registrations) node->registrations = std::make_unique<std::vector<Registration>>();
      Registration transient;
      transient.observer = registration.observer;
      transient.options = registration.options;
      transient.transient = true;
      transient.source = registration.id;
      node->registrations->push_back(std::move(transient));
      ++g_registrations;
      auto* observer = static_cast<JsMutationObserver*>(registration.observer);
      observer->nodes.push_back(node);
      observer->NoteWrite();
    }
  }
}

void DefineMutationClasses(Context& ctx) {
  qe::ClassRef observer = qe::DefineClass(ctx, "MutationObserver", ConstructObserver, 1);
  qe::SetRealmData(ctx, &g_observerPrototypeKey, observer.prototype);
  qe::DefineMethod(observer.prototype, "observe", Observe, 1);
  qe::DefineMethod(observer.prototype, "disconnect", Disconnect, 0);
  qe::DefineMethod(observer.prototype, "takeRecords", TakeRecords, 0);
  qe::DefineGlobal(ctx, "MutationObserver", observer.constructor);

  qe::ClassRef record = qe::DefineClass(ctx, "MutationRecord", IllegalConstructor, 0);
  qe::SetRealmData(ctx, &g_recordPrototypeKey, record.prototype);
  qe::DefineAccessor(record.prototype, "type", GetRecordType, nullptr);
  qe::DefineAccessor(record.prototype, "target", GetRecordTarget, nullptr);
  qe::DefineAccessor(record.prototype, "addedNodes", GetAddedNodes, nullptr);
  qe::DefineAccessor(record.prototype, "removedNodes", GetRemovedNodes, nullptr);
  qe::DefineAccessor(record.prototype, "previousSibling", GetRecordPrevious, nullptr);
  qe::DefineAccessor(record.prototype, "nextSibling", GetRecordNext, nullptr);
  qe::DefineAccessor(record.prototype, "attributeName", GetAttributeName, nullptr);
  qe::DefineAccessor(record.prototype, "attributeNamespace", GetAttributeNamespace, nullptr);
  qe::DefineAccessor(record.prototype, "oldValue", GetOldValue, nullptr);
  qe::DefineGlobal(ctx, "MutationRecord", record.constructor);

  // What notifying needs, kept on the realm's holder: queueMicrotask and the function it is given.
  qe::DefineGlobalFunction(ctx, "__solarNotifyObservers", Notify, 0);
  Object* holder = RealmHolder(ctx);
  MutationState* state = Heap::Allocate<MutationState>();
  state->initialize_prototype(nullptr);
  Value global = qe::FromObject(ctx.get_global_object());
  state->queueMicrotask = qe::Get(ctx, global, "queueMicrotask");
  state->notify = qe::Get(ctx, global, "__solarNotifyObservers");
  state->NoteWrite();
  if (holder) qe::Set(ctx, qe::FromObject(holder), "mutationState", qe::FromObject(state));
  qe::SetRealmData(ctx, &g_stateKey, state);
}

}  // namespace solar::dom
