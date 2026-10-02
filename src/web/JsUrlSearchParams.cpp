#include <string>

#include "UrlBindingsInternal.h"

namespace solar::web {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Heap;
using Quanta::Object;
using Quanta::Value;

namespace {

Object* g_prototype = nullptr;
Object* g_iteratorPrototype = nullptr;

enum class IterationKind { Entries, Keys, Values };

struct JsUrlSearchParamsIterator : DOMObject {
  JsUrlSearchParams* target = nullptr;
  IterationKind kind = IterationKind::Entries;
  size_t index = 0;
  bool done = false;

  void Visit(Quanta::Visitor& visitor) { visitor.Mark(target); }
};

JsUrlSearchParams* This(Context& ctx, const Value& thisValue) {
  JsUrlSearchParams* self = DOMObject::Cast<JsUrlSearchParams>(thisValue);
  if (!self) qe::ThrowTypeError(ctx, "Illegal invocation");
  return self;
}

Value Construct(Context& ctx, Value, qe::Args args, Value newTarget) {
  if (qe::IsUndefined(newTarget)) {
    qe::ThrowTypeError(ctx, "Failed to construct 'URLSearchParams': Please use the 'new' operator, this DOM "
                            "object constructor cannot be called as a function.");
    return qe::Undefined();
  }

  std::string init;
  if (!args.empty() && !qe::IsUndefined(args[0])) {
    if (qe::IsObject(args[0])) {
      // The sequence and record forms need to tell an iterable from a plain object, which
      // takes a Symbol.iterator lookup the embedding surface does not offer yet.
      qe::ThrowTypeError(ctx, "Failed to construct 'URLSearchParams': object initializers are not supported");
      return qe::Undefined();
    }
    init = qe::ToUsvUtf8(ctx, args[0]);
    if (qe::HasException(ctx)) return qe::Undefined();
  }

  Object* prototype = qe::PrototypeFromNewTarget(ctx, newTarget);
  if (qe::HasException(ctx)) return qe::Undefined();

  JsUrlSearchParams* params = Heap::Allocate<JsUrlSearchParams>();
  params->own.emplace(init);
  params->initialize_prototype(prototype ? prototype : g_prototype);
  return qe::FromObject(params);
}

Value GetSize(Context& ctx, Value thisValue, qe::Args, Value) {
  JsUrlSearchParams* self = This(ctx, thisValue);
  if (!self) return qe::Undefined();
  return qe::FromUint32(static_cast<uint32_t>(self->Params().Size()));
}

Value Append(Context& ctx, Value thisValue, qe::Args args, Value) {
  JsUrlSearchParams* self = This(ctx, thisValue);
  if (!self) return qe::Undefined();
  if (!RequireArguments(ctx, args, 2, "Failed to execute 'append' on 'URLSearchParams'")) return qe::Undefined();
  std::string name = qe::ToUsvUtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  std::string value = qe::ToUsvUtf8(ctx, args[1]);
  if (qe::HasException(ctx)) return qe::Undefined();
  self->Params().Append(name, value);
  return qe::Undefined();
}

Value Delete(Context& ctx, Value thisValue, qe::Args args, Value) {
  JsUrlSearchParams* self = This(ctx, thisValue);
  if (!self) return qe::Undefined();
  if (!RequireArguments(ctx, args, 1, "Failed to execute 'delete' on 'URLSearchParams'")) return qe::Undefined();
  std::string name = qe::ToUsvUtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  std::optional<std::string> value;
  if (args.size() > 1 && !qe::IsUndefined(args[1])) {
    value = qe::ToUsvUtf8(ctx, args[1]);
    if (qe::HasException(ctx)) return qe::Undefined();
  }
  self->Params().Delete(name, value ? std::optional<std::string_view>(*value) : std::nullopt);
  return qe::Undefined();
}

Value Get(Context& ctx, Value thisValue, qe::Args args, Value) {
  JsUrlSearchParams* self = This(ctx, thisValue);
  if (!self) return qe::Undefined();
  if (!RequireArguments(ctx, args, 1, "Failed to execute 'get' on 'URLSearchParams'")) return qe::Undefined();
  std::string name = qe::ToUsvUtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  std::optional<std::string> value = self->Params().Get(name);
  return value ? qe::FromUtf8(ctx, *value) : qe::Null();
}

Value GetAll(Context& ctx, Value thisValue, qe::Args args, Value) {
  JsUrlSearchParams* self = This(ctx, thisValue);
  if (!self) return qe::Undefined();
  if (!RequireArguments(ctx, args, 1, "Failed to execute 'getAll' on 'URLSearchParams'")) return qe::Undefined();
  std::string name = qe::ToUsvUtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();

  Value array = qe::NewArray(ctx);
  for (const std::string& value : self->Params().GetAll(name)) qe::ArrayPush(ctx, array, qe::FromUtf8(ctx, value));
  return array;
}

Value Has(Context& ctx, Value thisValue, qe::Args args, Value) {
  JsUrlSearchParams* self = This(ctx, thisValue);
  if (!self) return qe::Undefined();
  if (!RequireArguments(ctx, args, 1, "Failed to execute 'has' on 'URLSearchParams'")) return qe::Undefined();
  std::string name = qe::ToUsvUtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  std::optional<std::string> value;
  if (args.size() > 1 && !qe::IsUndefined(args[1])) {
    value = qe::ToUsvUtf8(ctx, args[1]);
    if (qe::HasException(ctx)) return qe::Undefined();
  }
  return qe::FromBool(self->Params().Has(name, value ? std::optional<std::string_view>(*value) : std::nullopt));
}

Value Set(Context& ctx, Value thisValue, qe::Args args, Value) {
  JsUrlSearchParams* self = This(ctx, thisValue);
  if (!self) return qe::Undefined();
  if (!RequireArguments(ctx, args, 2, "Failed to execute 'set' on 'URLSearchParams'")) return qe::Undefined();
  std::string name = qe::ToUsvUtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  std::string value = qe::ToUsvUtf8(ctx, args[1]);
  if (qe::HasException(ctx)) return qe::Undefined();
  self->Params().Set(name, value);
  return qe::Undefined();
}

Value Sort(Context& ctx, Value thisValue, qe::Args, Value) {
  JsUrlSearchParams* self = This(ctx, thisValue);
  if (!self) return qe::Undefined();
  self->Params().Sort();
  return qe::Undefined();
}

Value ToString(Context& ctx, Value thisValue, qe::Args, Value) {
  JsUrlSearchParams* self = This(ctx, thisValue);
  if (!self) return qe::Undefined();
  return qe::FromUtf8(ctx, self->Params().ToString());
}

Value ForEach(Context& ctx, Value thisValue, qe::Args args, Value) {
  JsUrlSearchParams* self = This(ctx, thisValue);
  if (!self) return qe::Undefined();
  if (!RequireArguments(ctx, args, 1, "Failed to execute 'forEach' on 'URLSearchParams'")) return qe::Undefined();
  if (!qe::IsCallable(args[0])) {
    qe::ThrowTypeError(ctx, "Failed to execute 'forEach' on 'URLSearchParams': parameter 1 is not of type 'Function'.");
    return qe::Undefined();
  }
  Value thisArg = args.size() > 1 ? args[1] : qe::Undefined();

  // The callback may change the list, so each step looks at the list as it is then.
  for (size_t i = 0; i < self->Params().Size(); ++i) {
    const url::UrlSearchParams::Pair pair = self->Params().List()[i];
    Value callbackArgs[3] = {qe::FromUtf8(ctx, pair.second), qe::FromUtf8(ctx, pair.first), thisValue};
    qe::Call(ctx, args[0], thisArg, qe::Args(callbackArgs, 3));
    if (qe::HasException(ctx)) return qe::Undefined();
  }
  return qe::Undefined();
}

template <IterationKind Kind>
Value MakeIterator(Context& ctx, Value thisValue, qe::Args, Value) {
  JsUrlSearchParams* self = This(ctx, thisValue);
  if (!self) return qe::Undefined();

  JsUrlSearchParamsIterator* iterator = Heap::Allocate<JsUrlSearchParamsIterator>();
  iterator->target = self;
  iterator->kind = Kind;
  iterator->initialize_prototype(g_iteratorPrototype);
  return qe::FromObject(iterator);
}

Value IteratorNext(Context& ctx, Value thisValue, qe::Args, Value) {
  JsUrlSearchParamsIterator* iterator = DOMObject::Cast<JsUrlSearchParamsIterator>(thisValue);
  if (!iterator) {
    qe::ThrowTypeError(ctx, "Illegal invocation");
    return qe::Undefined();
  }

  const std::vector<url::UrlSearchParams::Pair>& list = iterator->target->Params().List();
  if (iterator->done || iterator->index >= list.size()) {
    iterator->done = true;
    return qe::MakeIterResult(ctx, qe::Undefined(), true);
  }

  const url::UrlSearchParams::Pair& pair = list[iterator->index++];
  switch (iterator->kind) {
    case IterationKind::Keys:
      return qe::MakeIterResult(ctx, qe::FromUtf8(ctx, pair.first), false);
    case IterationKind::Values:
      return qe::MakeIterResult(ctx, qe::FromUtf8(ctx, pair.second), false);
    case IterationKind::Entries: {
      Value entry = qe::NewArray(ctx);
      qe::ArrayPush(ctx, entry, qe::FromUtf8(ctx, pair.first));
      qe::ArrayPush(ctx, entry, qe::FromUtf8(ctx, pair.second));
      return qe::MakeIterResult(ctx, entry, false);
    }
  }
  return qe::Undefined();
}

Value IllegalConstructor(Context& ctx, Value, qe::Args, Value) {
  qe::ThrowTypeError(ctx, "Illegal constructor");
  return qe::Undefined();
}

}  // namespace

Object* UrlSearchParamsPrototype() { return g_prototype; }

void DefineUrlSearchParamsClass(qe::Runtime& runtime) {
  Context& ctx = runtime.GetContext();

  qe::ClassRef iterator = qe::DefineClass(ctx, "URLSearchParams Iterator", IllegalConstructor, 0,
                                          qe::GetIteratorPrototype(ctx));
  g_iteratorPrototype = iterator.prototype;
  qe::DefineMethod(iterator.prototype, "next", IteratorNext, 0);
  qe::DefineToStringTag(iterator.prototype, "URLSearchParams Iterator");

  qe::ClassRef params = qe::DefineClass(ctx, "URLSearchParams", Construct, 0);
  g_prototype = params.prototype;

  qe::DefineAccessor(params.prototype, "size", GetSize, nullptr);
  qe::DefineMethod(params.prototype, "append", Append, 2);
  qe::DefineMethod(params.prototype, "delete", Delete, 1);
  qe::DefineMethod(params.prototype, "get", Get, 1);
  qe::DefineMethod(params.prototype, "getAll", GetAll, 1);
  qe::DefineMethod(params.prototype, "has", Has, 1);
  qe::DefineMethod(params.prototype, "set", Set, 2);
  qe::DefineMethod(params.prototype, "sort", Sort, 0);
  qe::DefineMethod(params.prototype, "toString", ToString, 0);
  qe::DefineMethod(params.prototype, "forEach", ForEach, 1);
  qe::DefineMethod(params.prototype, "entries", MakeIterator<IterationKind::Entries>, 0);
  qe::DefineMethod(params.prototype, "keys", MakeIterator<IterationKind::Keys>, 0);
  qe::DefineMethod(params.prototype, "values", MakeIterator<IterationKind::Values>, 0);
  qe::DefineGlobal(ctx, "URLSearchParams", params.constructor);

  // Web IDL makes @@iterator the same function object as entries, and the embedding surface
  // has no symbol-keyed definitions, so it is installed from script.
  runtime.Evaluate("URLSearchParams.prototype[Symbol.iterator] = URLSearchParams.prototype.entries;");
}

}  // namespace solar::web
