#include <algorithm>
#include <string>

#include "solar/web/FetchBindingsInternal.h"
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

char g_prototypeKey;
char g_iteratorPrototypeKey;

Object* Prototype(Context& ctx) { return static_cast<Object*>(qe::GetRealmData(ctx, &g_prototypeKey)); }
Object* IteratorPrototype(Context& ctx) { return static_cast<Object*>(qe::GetRealmData(ctx, &g_iteratorPrototypeKey)); }

enum class IterationKind { Entries, Keys, Values };

struct JsHeadersIterator : DOMObject {
  JsHeaders* target = nullptr;
  IterationKind kind = IterationKind::Entries;
  size_t index = 0;
  bool done = false;

  void Visit(Quanta::Visitor& visitor) { visitor.Mark(target); }
};

JsHeaders* This(Context& ctx, const Value& thisValue) {
  JsHeaders* self = DOMObject::Cast<JsHeaders>(thisValue);
  if (!self) qe::ThrowTypeError(ctx, "Illegal invocation");
  return self;
}

// A script's string as a ByteString, which a header name or value is. False, with a TypeError pending,
// if it is not one.
bool ToByteString(Context& ctx, const Value& value, std::string& out, const char* what) {
  const std::string text = qe::ToUsvUtf8(ctx, value);
  if (qe::HasException(ctx)) return false;
  std::optional<std::string> bytes = net::ToByteString(text);
  if (!bytes) {
    qe::ThrowTypeError(ctx, std::string("Headers: ") + what + " is not a ByteString (it has a character above U+00FF)");
    return false;
  }
  out = std::move(*bytes);
  return true;
}

bool ReadName(Context& ctx, const Value& value, std::string& name, const char* method) {
  if (!ToByteString(ctx, value, name, "the header name")) return false;
  if (!net::IsValidHeaderName(name)) {
    qe::ThrowTypeError(ctx, std::string("Failed to execute '") + method + "' on 'Headers': Invalid name");
    return false;
  }
  return true;
}

// Says what a refused change was, as a TypeError. False if it was.
bool Report(Context& ctx, net::FetchHeaders::Status status, const char* method) {
  switch (status) {
    case net::FetchHeaders::Status::Ok:
      return true;
    case net::FetchHeaders::Status::InvalidValue:
      qe::ThrowTypeError(ctx, std::string("Failed to execute '") + method + "' on 'Headers': Invalid value");
      return false;
    case net::FetchHeaders::Status::Immutable:
      qe::ThrowTypeError(ctx, std::string("Failed to execute '") + method + "' on 'Headers': Headers are immutable");
      return false;
  }
  return false;
}

// Adds one name and value from an init, which stops the whole init on the first one that will not do.
bool AppendPair(Context& ctx, JsHeaders* target, const Value& nameValue, const Value& valueValue) {
  std::string name, value;
  if (!ReadName(ctx, nameValue, name, "constructor")) return false;
  if (!ToByteString(ctx, valueValue, value, "the header value")) return false;
  return Report(ctx, target->headers.Append(name, value), "constructor");
}

// sequence<sequence<ByteString>>, each inner sequence being a name and a value.
bool FillFromSequence(Context& ctx, JsHeaders* target, const Value& init, const Value& method) {
  return Iterate(ctx, init, method, [&](const Value& pair) {
    if (!qe::IsObject(pair)) {
      qe::ThrowTypeError(ctx, "Failed to construct 'Headers': The provided value cannot be converted to a sequence.");
      return false;
    }
    Value innerMethod = qe::GetIteratorMethod(ctx, pair);
    if (qe::HasException(ctx)) return false;
    if (qe::IsUndefined(innerMethod)) {
      qe::ThrowTypeError(ctx, "Failed to construct 'Headers': The provided value cannot be converted to a sequence.");
      return false;
    }
    std::vector<Value> items;
    bool ok = Iterate(ctx, pair, innerMethod, [&](const Value& item) {
      items.push_back(item);
      return true;
    });
    if (!ok) return false;
    if (items.size() != 2) {
      qe::ThrowTypeError(ctx, "Failed to construct 'Headers': Each header pair must be an iterable [name, value] tuple");
      return false;
    }
    return AppendPair(ctx, target, items[0], items[1]);
  });
}

// record<ByteString, ByteString>.
bool FillFromRecord(Context& ctx, JsHeaders* target, const Value& init) {
  const std::vector<std::string> keys = qe::OwnKeys(ctx, init);
  if (qe::HasException(ctx)) return false;
  for (const std::string& rawKey : keys) {
    Value value = qe::Get(ctx, init, rawKey);
    if (qe::HasException(ctx)) return false;
    if (!AppendPair(ctx, target, qe::FromUtf8(ctx, ScrubSurrogates(rawKey)), value)) return false;
  }
  return true;
}

Value Construct(Context& ctx, Value, qe::Args args, Value newTarget) {
  if (qe::IsUndefined(newTarget)) {
    qe::ThrowTypeError(ctx, "Failed to construct 'Headers': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    return qe::Undefined();
  }
  Object* prototype = qe::PrototypeFromNewTarget(ctx, newTarget);
  if (qe::HasException(ctx)) return qe::Undefined();

  JsHeaders* headers = AllocateHeaders(ctx, net::HeadersGuard::None);
  if (prototype) headers->initialize_prototype(prototype);

  if (!args.empty() && !qe::IsUndefined(args[0])) {
    if (!qe::IsObject(args[0])) {
      qe::ThrowTypeError(ctx, "Failed to construct 'Headers': The provided value is not of type '(sequence<sequence<ByteString>> or record<ByteString, ByteString>)'.");
      return qe::Undefined();
    }
    Value method = qe::GetIteratorMethod(ctx, args[0]);
    if (qe::HasException(ctx)) return qe::Undefined();
    const bool ok = qe::IsUndefined(method) ? FillFromRecord(ctx, headers, args[0]) : FillFromSequence(ctx, headers, args[0], method);
    if (!ok) return qe::Undefined();
  }
  return qe::FromObject(headers);
}

Value Append(Context& ctx, Value thisValue, qe::Args args, Value) {
  JsHeaders* self = This(ctx, thisValue);
  if (!self) return qe::Undefined();
  if (!RequireArguments(ctx, args, 2, "Failed to execute 'append' on 'Headers'")) return qe::Undefined();
  std::string name, value;
  if (!ReadName(ctx, args[0], name, "append") || !ToByteString(ctx, args[1], value, "the header value")) return qe::Undefined();
  Report(ctx, self->headers.Append(name, value), "append");
  return qe::Undefined();
}

Value Delete(Context& ctx, Value thisValue, qe::Args args, Value) {
  JsHeaders* self = This(ctx, thisValue);
  if (!self) return qe::Undefined();
  if (!RequireArguments(ctx, args, 1, "Failed to execute 'delete' on 'Headers'")) return qe::Undefined();
  std::string name;
  if (!ReadName(ctx, args[0], name, "delete")) return qe::Undefined();
  Report(ctx, self->headers.Delete(name), "delete");
  return qe::Undefined();
}

Value Get(Context& ctx, Value thisValue, qe::Args args, Value) {
  JsHeaders* self = This(ctx, thisValue);
  if (!self) return qe::Undefined();
  if (!RequireArguments(ctx, args, 1, "Failed to execute 'get' on 'Headers'")) return qe::Undefined();
  std::string name;
  if (!ReadName(ctx, args[0], name, "get")) return qe::Undefined();
  const std::optional<std::string> value = self->headers.Get(name);
  return value ? qe::FromUtf8(ctx, net::IsomorphicDecode(*value)) : qe::Null();
}

Value GetSetCookie(Context& ctx, Value thisValue, qe::Args, Value) {
  JsHeaders* self = This(ctx, thisValue);
  if (!self) return qe::Undefined();
  Value array = qe::NewArray(ctx);
  for (const std::string& value : self->headers.GetSetCookie()) qe::ArrayPush(ctx, array, qe::FromUtf8(ctx, net::IsomorphicDecode(value)));
  return array;
}

Value Has(Context& ctx, Value thisValue, qe::Args args, Value) {
  JsHeaders* self = This(ctx, thisValue);
  if (!self) return qe::Undefined();
  if (!RequireArguments(ctx, args, 1, "Failed to execute 'has' on 'Headers'")) return qe::Undefined();
  std::string name;
  if (!ReadName(ctx, args[0], name, "has")) return qe::Undefined();
  return qe::FromBool(self->headers.Has(name));
}

Value Set(Context& ctx, Value thisValue, qe::Args args, Value) {
  JsHeaders* self = This(ctx, thisValue);
  if (!self) return qe::Undefined();
  if (!RequireArguments(ctx, args, 2, "Failed to execute 'set' on 'Headers'")) return qe::Undefined();
  std::string name, value;
  if (!ReadName(ctx, args[0], name, "set") || !ToByteString(ctx, args[1], value, "the header value")) return qe::Undefined();
  Report(ctx, self->headers.Set(name, value), "set");
  return qe::Undefined();
}

Value ForEach(Context& ctx, Value thisValue, qe::Args args, Value) {
  JsHeaders* self = This(ctx, thisValue);
  if (!self) return qe::Undefined();
  if (!RequireArguments(ctx, args, 1, "Failed to execute 'forEach' on 'Headers'")) return qe::Undefined();
  if (!qe::IsCallable(args[0])) {
    qe::ThrowTypeError(ctx, "Failed to execute 'forEach' on 'Headers': parameter 1 is not of type 'Function'.");
    return qe::Undefined();
  }
  Value thisArg = args.size() > 1 ? args[1] : qe::Undefined();

  // The callback may change the list, so each step takes the list as it is then.
  for (size_t i = 0;; ++i) {
    const std::vector<net::FetchHeaders::Entry> entries = self->headers.SortedAndCombined();
    if (i >= entries.size()) break;
    Value callbackArgs[3] = {qe::FromUtf8(ctx, net::IsomorphicDecode(entries[i].second)), qe::FromUtf8(ctx, entries[i].first), thisValue};
    qe::Call(ctx, args[0], thisArg, qe::Args(callbackArgs, 3));
    if (qe::HasException(ctx)) return qe::Undefined();
  }
  return qe::Undefined();
}

template <IterationKind Kind>
Value MakeIterator(Context& ctx, Value thisValue, qe::Args, Value) {
  JsHeaders* self = This(ctx, thisValue);
  if (!self) return qe::Undefined();
  JsHeadersIterator* iterator = Heap::Allocate<JsHeadersIterator>();
  iterator->target = self;
  iterator->kind = Kind;
  iterator->initialize_prototype(self->iteratorPrototype);
  return qe::FromObject(iterator);
}

Value IteratorNext(Context& ctx, Value thisValue, qe::Args, Value) {
  JsHeadersIterator* iterator = DOMObject::Cast<JsHeadersIterator>(thisValue);
  if (!iterator) {
    qe::ThrowTypeError(ctx, "Illegal invocation");
    return qe::Undefined();
  }
  // Taken afresh at every step, as the standard has it: a header added meanwhile is seen.
  const std::vector<net::FetchHeaders::Entry> entries = iterator->target->headers.SortedAndCombined();
  if (iterator->done || iterator->index >= entries.size()) {
    iterator->done = true;
    return qe::MakeIterResult(ctx, qe::Undefined(), true);
  }
  const net::FetchHeaders::Entry& entry = entries[iterator->index++];
  const Value name = qe::FromUtf8(ctx, entry.first);
  const Value value = qe::FromUtf8(ctx, net::IsomorphicDecode(entry.second));
  switch (iterator->kind) {
    case IterationKind::Keys:
      return qe::MakeIterResult(ctx, name, false);
    case IterationKind::Values:
      return qe::MakeIterResult(ctx, value, false);
    case IterationKind::Entries: {
      Value pair = qe::NewArray(ctx);
      qe::ArrayPush(ctx, pair, name);
      qe::ArrayPush(ctx, pair, value);
      return qe::MakeIterResult(ctx, pair, false);
    }
  }
  return qe::Undefined();
}

Value IllegalConstructor(Context& ctx, Value, qe::Args, Value) {
  qe::ThrowTypeError(ctx, "Illegal constructor");
  return qe::Undefined();
}

}  // namespace

JsHeaders* AllocateHeaders(Context& ctx, net::HeadersGuard guard) {
  JsHeaders* headers = Heap::Allocate<JsHeaders>();
  headers->headers.SetGuard(guard);
  headers->iteratorPrototype = IteratorPrototype(ctx);
  headers->initialize_prototype(Prototype(ctx));
  return headers;
}

void DefineHeadersClass(Context& ctx) {
  qe::ClassRef iterator = qe::DefineClass(ctx, "Headers Iterator", IllegalConstructor, 0, qe::GetIteratorPrototype(ctx));
  qe::SetRealmData(ctx, &g_iteratorPrototypeKey, iterator.prototype);
  qe::DefineMethod(iterator.prototype, "next", IteratorNext, 0);
  qe::DefineToStringTag(iterator.prototype, "Headers Iterator");

  qe::ClassRef headers = qe::DefineClass(ctx, "Headers", Construct, 0);
  qe::SetRealmData(ctx, &g_prototypeKey, headers.prototype);
  qe::DefineMethod(headers.prototype, "append", Append, 2);
  qe::DefineMethod(headers.prototype, "delete", Delete, 1);
  qe::DefineMethod(headers.prototype, "get", Get, 1);
  qe::DefineMethod(headers.prototype, "getSetCookie", GetSetCookie, 0);
  qe::DefineMethod(headers.prototype, "has", Has, 1);
  qe::DefineMethod(headers.prototype, "set", Set, 2);
  qe::DefineMethod(headers.prototype, "forEach", ForEach, 1);
  qe::DefineMethod(headers.prototype, "entries", MakeIterator<IterationKind::Entries>, 0);
  qe::DefineMethod(headers.prototype, "keys", MakeIterator<IterationKind::Keys>, 0);
  qe::DefineMethod(headers.prototype, "values", MakeIterator<IterationKind::Values>, 0);
  qe::DefineGlobal(ctx, "Headers", headers.constructor);
}

}  // namespace solar::web
