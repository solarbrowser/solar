#include <string>

#include "solar/dom/NodeBindingsInternal.h"

namespace solar::dom {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Object;
using Quanta::Value;

namespace {

Value IllegalConstructor(Context& ctx, Value, qe::Args, Value) {
  qe::ThrowTypeError(ctx, "Illegal constructor");
  return qe::Undefined();
}

ShadowRoot* ThisShadowRoot(Context& ctx, const Value& t) {
  Node* node = DOMObject::Cast<Node>(t);
  if (!node || !node->IsFragment() || !static_cast<DocumentFragment*>(node)->isShadowRoot) {
    qe::ThrowTypeError(ctx, "Illegal invocation");
    return nullptr;
  }
  return static_cast<ShadowRoot*>(node);
}

Value GetMode(Context& ctx, Value t, qe::Args, Value) {
  ShadowRoot* self = ThisShadowRoot(ctx, t);
  return self ? qe::FromWtf8(ctx, self->mode == ShadowMode::Open ? "open" : "closed") : qe::Undefined();
}
Value GetSlotAssignment(Context& ctx, Value t, qe::Args, Value) {
  ShadowRoot* self = ThisShadowRoot(ctx, t);
  return self ? qe::FromWtf8(ctx, self->slotAssignment == SlotAssignment::Manual ? "manual" : "named") : qe::Undefined();
}
Value GetDelegatesFocus(Context& ctx, Value t, qe::Args, Value) {
  ShadowRoot* self = ThisShadowRoot(ctx, t);
  return self ? qe::FromBool(self->delegatesFocus) : qe::Undefined();
}
Value GetClonable(Context& ctx, Value t, qe::Args, Value) {
  ShadowRoot* self = ThisShadowRoot(ctx, t);
  return self ? qe::FromBool(self->clonable) : qe::Undefined();
}
Value GetSerializable(Context& ctx, Value t, qe::Args, Value) {
  ShadowRoot* self = ThisShadowRoot(ctx, t);
  return self ? qe::FromBool(self->serializable) : qe::Undefined();
}
Value GetHost(Context& ctx, Value t, qe::Args, Value) {
  ShadowRoot* self = ThisShadowRoot(ctx, t);
  return self ? NodeValue(self->host) : qe::Undefined();
}
Value GetOnSlotChange(Context& ctx, Value t, qe::Args, Value) {
  ShadowRoot* self = ThisShadowRoot(ctx, t);
  return self ? web::GetEventHandler(self, "slotchange") : qe::Undefined();
}
Value SetOnSlotChange(Context& ctx, Value t, qe::Args args, Value) {
  ShadowRoot* self = ThisShadowRoot(ctx, t);
  if (self) web::SetEventHandler(self, "slotchange", !args.empty() && qe::IsCallable(args[0]) ? args[0] : qe::Null());
  return qe::Undefined();
}

// ---- Element ----

Value GetShadowRoot(Context& ctx, Value t, qe::Args, Value) {
  Element* self = ThisElement(ctx, t);
  if (!self) return qe::Undefined();
  return self->shadowRoot && self->shadowRoot->mode == ShadowMode::Open ? qe::FromObject(self->shadowRoot) : qe::Null();
}

// A member of the ShadowRootInit dictionary that is an enumeration.
bool ReadEnum(Context& ctx, const Value& init, const char* name, std::initializer_list<const char*> allowed, const char* type, std::string& out, bool required) {
  Value value = qe::Get(ctx, init, name);
  if (qe::HasException(ctx)) return false;
  if (qe::IsUndefined(value)) {
    if (required) {
      qe::ThrowTypeError(ctx, std::string("Failed to execute 'attachShadow' on 'Element': Failed to read the '") + name + "' property from 'ShadowRootInit': Required member is undefined.");
      return false;
    }
    return true;
  }
  const std::string text = qe::ToWtf8(ctx, value);
  if (qe::HasException(ctx)) return false;
  for (const char* option : allowed) {
    if (text == option) {
      out = text;
      return true;
    }
  }
  qe::ThrowTypeError(ctx, std::string("Failed to execute 'attachShadow' on 'Element': Failed to read the '") + name + "' property from 'ShadowRootInit': The provided value '" + text +
                              "' is not a valid enum value of type " + type + ".");
  return false;
}

Value AttachShadowMethod(Context& ctx, Value t, qe::Args args, Value) {
  Element* self = ThisElement(ctx, t);
  if (!self) return qe::Undefined();
  if (args.empty() || !qe::IsObject(args[0])) {
    qe::ThrowTypeError(ctx, "Failed to execute 'attachShadow' on 'Element': The provided value is not of type 'ShadowRootInit'.");
    return qe::Undefined();
  }
  const Value init = args[0];
  // The members in alphabetical order, as a dictionary's are read.
  Value clonable = qe::Get(ctx, init, "clonable");
  Value delegatesFocus = qe::HasException(ctx) ? Value() : qe::Get(ctx, init, "delegatesFocus");
  if (qe::HasException(ctx)) return qe::Undefined();
  std::string mode, slotAssignment = "named";
  if (!ReadEnum(ctx, init, "mode", {"open", "closed"}, "ShadowRootMode", mode, true)) return qe::Undefined();
  Value serializable = qe::Get(ctx, init, "serializable");
  if (qe::HasException(ctx) || !ReadEnum(ctx, init, "slotAssignment", {"named", "manual"}, "SlotAssignmentMode", slotAssignment, false)) return qe::Undefined();
  ShadowRoot* shadow = nullptr;
  if (auto error = AttachShadow(ctx, self, mode == "open" ? ShadowMode::Open : ShadowMode::Closed, slotAssignment == "manual" ? SlotAssignment::Manual : SlotAssignment::Named,
                                delegatesFocus.to_boolean(), clonable.to_boolean(), serializable.to_boolean(), shadow)) {
    Throw(ctx, *error);
    return qe::Undefined();
  }
  return qe::FromObject(shadow);
}

// ---- Slottable ----

Value GetAssignedSlot(Context& ctx, Value t, qe::Args, Value) {
  Node* self = ThisNode(ctx, t);
  if (!self) return qe::Undefined();
  return NodeValue(FindSlot(self, true));
}

}  // namespace

void DefineShadowClasses(Context& ctx) {
  Object* fragment = InterfacePrototype(ctx, Interface::DocumentFragment);
  qe::ClassRef shadow = qe::DefineClass(ctx, "ShadowRoot", IllegalConstructor, 0, fragment);
  SetInterfacePrototype(ctx, Interface::ShadowRoot, shadow.prototype);
  qe::DefineAccessor(shadow.prototype, "mode", GetMode, nullptr);
  qe::DefineAccessor(shadow.prototype, "delegatesFocus", GetDelegatesFocus, nullptr);
  qe::DefineAccessor(shadow.prototype, "slotAssignment", GetSlotAssignment, nullptr);
  qe::DefineAccessor(shadow.prototype, "clonable", GetClonable, nullptr);
  qe::DefineAccessor(shadow.prototype, "serializable", GetSerializable, nullptr);
  qe::DefineAccessor(shadow.prototype, "host", GetHost, nullptr);
  qe::DefineAccessor(shadow.prototype, "onslotchange", GetOnSlotChange, SetOnSlotChange);
  qe::DefineGlobal(ctx, "ShadowRoot", shadow.constructor);

  Object* element = InterfacePrototype(ctx, Interface::Element);
  qe::DefineAccessor(element, "shadowRoot", GetShadowRoot, nullptr);
  qe::DefineMethod(element, "attachShadow", AttachShadowMethod, 1);
  qe::DefineAccessor(element, "assignedSlot", GetAssignedSlot, nullptr);
  qe::DefineAccessor(InterfacePrototype(ctx, Interface::Text), "assignedSlot", GetAssignedSlot, nullptr);
}

}  // namespace solar::dom
