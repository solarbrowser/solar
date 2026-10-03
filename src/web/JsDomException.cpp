#include <string>

#include "solar/web/DomBindingsInternal.h"

namespace solar::web {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Heap;
using Quanta::Object;
using Quanta::Value;

namespace {

char g_prototypeKey;

Object* Prototype(Context& ctx) { return static_cast<Object*>(qe::GetRealmData(ctx, &g_prototypeKey)); }

struct NameAndCode {
  const char* name;
  uint16_t code;
};

// The names Web IDL defines, with the code each has had since DOM Level 3.
constexpr NameAndCode kNames[] = {
    {"IndexSizeError", 1},       {"HierarchyRequestError", 3},  {"WrongDocumentError", 4},    {"InvalidCharacterError", 5},
    {"NoModificationAllowedError", 7}, {"NotFoundError", 8},    {"NotSupportedError", 9},     {"InUseAttributeError", 10},
    {"InvalidStateError", 11},   {"SyntaxError", 12},           {"InvalidModificationError", 13}, {"NamespaceError", 14},
    {"InvalidAccessError", 15},  {"TypeMismatchError", 17},     {"SecurityError", 18},        {"NetworkError", 19},
    {"AbortError", 20},          {"URLMismatchError", 21},      {"QuotaExceededError", 22},   {"TimeoutError", 23},
    {"InvalidNodeTypeError", 24}, {"DataCloneError", 25},
};

uint16_t CodeOf(const std::string& name) {
  for (const NameAndCode& entry : kNames) {
    if (name == entry.name) return entry.code;
  }
  return 0;
}

JsDomException* This(Context& ctx, const Value& thisValue) {
  JsDomException* self = DOMObject::Cast<JsDomException>(thisValue);
  if (!self) qe::ThrowTypeError(ctx, "Illegal invocation");
  return self;
}

JsDomException* Allocate(Object* prototype, const std::string& message, const std::string& name) {
  JsDomException* exception = Heap::Allocate<JsDomException>();
  exception->message = message;
  exception->name = name;
  exception->code = CodeOf(name);
  exception->initialize_prototype(prototype);
  return exception;
}

Value Construct(Context& ctx, Value, qe::Args args, Value newTarget) {
  if (qe::IsUndefined(newTarget)) {
    qe::ThrowTypeError(ctx, "Failed to construct 'DOMException': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    return qe::Undefined();
  }
  std::string message;
  std::string name = "Error";
  if (!args.empty() && !qe::IsUndefined(args[0])) {
    message = qe::ToUsvUtf8(ctx, args[0]);
    if (qe::HasException(ctx)) return qe::Undefined();
  }
  if (args.size() > 1 && !qe::IsUndefined(args[1])) {
    name = qe::ToUsvUtf8(ctx, args[1]);
    if (qe::HasException(ctx)) return qe::Undefined();
  }
  Object* prototype = qe::PrototypeFromNewTarget(ctx, newTarget);
  if (qe::HasException(ctx)) return qe::Undefined();
  return qe::FromObject(Allocate(prototype ? prototype : Prototype(ctx), message, name));
}

Value GetName(Context& ctx, Value thisValue, qe::Args, Value) {
  JsDomException* self = This(ctx, thisValue);
  return self ? qe::FromUtf8(ctx, self->name) : qe::Undefined();
}

Value GetMessage(Context& ctx, Value thisValue, qe::Args, Value) {
  JsDomException* self = This(ctx, thisValue);
  return self ? qe::FromUtf8(ctx, self->message) : qe::Undefined();
}

Value GetCode(Context& ctx, Value thisValue, qe::Args, Value) {
  JsDomException* self = This(ctx, thisValue);
  return self ? qe::FromUint32(self->code) : qe::Undefined();
}

// Errors have a stack in this engine, and a DOMException is one as far as script can tell.
Value GetStack(Context& ctx, Value thisValue, qe::Args, Value) {
  JsDomException* self = This(ctx, thisValue);
  return self ? qe::FromUtf8(ctx, self->name + ": " + self->message + "\n    at <anonymous>") : qe::Undefined();
}

}  // namespace

Value NewDomException(Context& ctx, const std::string& message, const std::string& name) {
  return qe::FromObject(Allocate(Prototype(ctx), message, name));
}

void ThrowDomException(Context& ctx, const std::string& message, const std::string& name) {
  ctx.throw_exception(NewDomException(ctx, message, name));
}

void DefineDomExceptionClass(Context& ctx) {
  // A DOMException is an Error as far as script can tell: it inherits from Error.prototype.
  Value error = qe::Get(ctx, qe::FromObject(ctx.get_global_object()), "Error");
  Value errorPrototype = qe::Get(ctx, error, "prototype");
  Object* parent = qe::IsObject(errorPrototype) ? errorPrototype.as_object() : nullptr;

  qe::ClassRef exception = qe::DefineClass(ctx, "DOMException", Construct, 0, parent);
  qe::SetRealmData(ctx, &g_prototypeKey, exception.prototype);
  qe::DefineAccessor(exception.prototype, "name", GetName, nullptr);
  qe::DefineAccessor(exception.prototype, "message", GetMessage, nullptr);
  qe::DefineAccessor(exception.prototype, "code", GetCode, nullptr);
  qe::DefineAccessor(exception.prototype, "stack", GetStack, nullptr);
  qe::DefineGlobal(ctx, "DOMException", exception.constructor);
}

}  // namespace solar::web
