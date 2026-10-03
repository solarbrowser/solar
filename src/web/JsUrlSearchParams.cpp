#include <algorithm>
#include <string>

#include "solar/web/UrlBindingsInternal.h"
#include "solar/web/WebIdl.h"

namespace solar::web {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Heap;
using Quanta::Object;
using Quanta::Value;

namespace {

// The prototypes live in the realm, under these keys, so a second realm does not replace them.
char g_prototypeKey;
char g_iteratorPrototypeKey;

Object* Prototype(Context& ctx) { return static_cast<Object*>(qe::GetRealmData(ctx, &g_prototypeKey)); }
Object* IteratorPrototype(Context& ctx) { return static_cast<Object*>(qe::GetRealmData(ctx, &g_iteratorPrototypeKey)); }

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

using Pair = url::UrlSearchParams::Pair;

// sequence<sequence<USVString>>, each inner sequence being a name and a value.
bool ReadSequence(Context& ctx, const Value& init, const Value& method, std::vector<Pair>& out) {
  return Iterate(ctx, init, method, [&](const Value& pair) {
    if (!qe::IsObject(pair)) {
      qe::ThrowTypeError(ctx, "Failed to construct 'URLSearchParams': The provided value cannot be converted to a sequence.");
      return false;
    }
    Value innerMethod = qe::GetIteratorMethod(ctx, pair);
    if (qe::HasException(ctx)) return false;
    if (qe::IsUndefined(innerMethod)) {
      qe::ThrowTypeError(ctx, "Failed to construct 'URLSearchParams': The provided value cannot be converted to a sequence.");
      return false;
    }

    std::vector<std::string> items;
    bool ok = Iterate(ctx, pair, innerMethod, [&](const Value& item) {
      items.push_back(qe::ToUsvUtf8(ctx, item));
      return !qe::HasException(ctx);
    });
    if (!ok) return false;
    if (items.size() != 2) {
      qe::ThrowTypeError(ctx, "Failed to construct 'URLSearchParams': Each query pair must be an iterable [name, value] tuple");
      return false;
    }
    out.emplace_back(std::move(items[0]), std::move(items[1]));
    return true;
  });
}

// record<USVString, USVString>. A repeated key keeps its first position and takes the last value.
bool ReadRecord(Context& ctx, const Value& init, std::vector<Pair>& out) {
  std::string key;
  return IterateRecord(
      ctx, init,
      [&](const Value& rawKey) {
        key = qe::ToUsvUtf8(ctx, rawKey);
        return !qe::HasException(ctx);
      },
      [&](const Value& value) {
        std::string text = qe::ToUsvUtf8(ctx, value);
        if (qe::HasException(ctx)) return false;
        auto existing = std::find_if(out.begin(), out.end(), [&](const Pair& pair) { return pair.first == key; });
        if (existing != out.end()) {
          existing->second = std::move(text);
        } else {
          out.emplace_back(key, std::move(text));
        }
        return true;
      });
}

Value Construct(Context& ctx, Value, qe::Args args, Value newTarget) {
  if (qe::IsUndefined(newTarget)) {
    qe::ThrowTypeError(ctx, "Failed to construct 'URLSearchParams': Please use the 'new' operator, this DOM "
                            "object constructor cannot be called as a function.");
    return qe::Undefined();
  }

  std::string init;
  std::vector<Pair> pairs;
  bool fromObject = false;
  if (!args.empty() && !qe::IsUndefined(args[0])) {
    if (qe::IsObject(args[0])) {
      fromObject = true;
      Value method = qe::GetIteratorMethod(ctx, args[0]);
      if (qe::HasException(ctx)) return qe::Undefined();
      bool ok = qe::IsUndefined(method) ? ReadRecord(ctx, args[0], pairs) : ReadSequence(ctx, args[0], method, pairs);
      if (!ok) return qe::Undefined();
    } else {
      init = qe::ToUsvUtf8(ctx, args[0]);
      if (qe::HasException(ctx)) return qe::Undefined();
    }
  }

  Object* prototype = qe::PrototypeFromNewTarget(ctx, newTarget);
  if (qe::HasException(ctx)) return qe::Undefined();

  JsUrlSearchParams* params = AllocateSearchParams(prototype ? prototype : Prototype(ctx), IteratorPrototype(ctx));
  if (fromObject) {
    params->own.emplace();
    for (const Pair& pair : pairs) params->own->Append(pair.first, pair.second);
  } else {
    params->own.emplace(init);
  }
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
  iterator->initialize_prototype(self->iteratorPrototype);
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

Object* UrlSearchParamsPrototype(Context& ctx) { return Prototype(ctx); }

Object* UrlSearchParamsIteratorPrototype(Context& ctx) { return IteratorPrototype(ctx); }

JsUrlSearchParams* AllocateSearchParams(Object* prototype, Object* iteratorPrototype) {
  JsUrlSearchParams* params = Heap::Allocate<JsUrlSearchParams>();
  params->iteratorPrototype = iteratorPrototype;
  params->initialize_prototype(prototype);
  return params;
}

void DefineUrlSearchParamsClass(Context& ctx) {
  qe::ClassRef iterator = qe::DefineClass(ctx, "URLSearchParams Iterator", IllegalConstructor, 0,
                                          qe::GetIteratorPrototype(ctx));
  qe::SetRealmData(ctx, &g_iteratorPrototypeKey, iterator.prototype);
  qe::DefineMethod(iterator.prototype, "next", IteratorNext, 0);
  qe::DefineToStringTag(iterator.prototype, "URLSearchParams Iterator");

  qe::ClassRef params = qe::DefineClass(ctx, "URLSearchParams", Construct, 0);
  qe::SetRealmData(ctx, &g_prototypeKey, params.prototype);

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
}

}  // namespace solar::web
