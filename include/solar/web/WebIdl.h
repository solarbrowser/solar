#pragma once

#include <string>

#include "quanta/Embed.h"

// Conversions and iteration that Web IDL defines once and every binding needs.
namespace solar::web {

// The engine keeps a lone surrogate as a 3-byte sequence, which USVString turns into one
// U+FFFD; the keys OwnKeys returns have not been through that yet.
inline std::string ScrubSurrogates(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    bool surrogate = static_cast<unsigned char>(s[i]) == 0xED && i + 2 < s.size() &&
                     static_cast<unsigned char>(s[i + 1]) >= 0xA0;
    if (surrogate) {
      out += "\xEF\xBF\xBD";
      i += 2;
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

// Calls `visit` with each value `iterable`'s iterator produces. False, with an exception
// pending, when anything fails or `visit` returns false.
template <typename Visit>
inline bool Iterate(Quanta::Context& ctx, const Quanta::Value& iterable, const Quanta::Value& method, Visit visit) {
  Quanta::Value iterator = Quanta::Embed::Call(ctx, method, iterable);
  if (Quanta::Embed::HasException(ctx)) return false;
  if (!Quanta::Embed::IsObject(iterator)) {
    Quanta::Embed::ThrowTypeError(ctx, "The iterator method did not return an object");
    return false;
  }
  Quanta::Value next = Quanta::Embed::Get(ctx, iterator, "next");
  if (Quanta::Embed::HasException(ctx)) return false;

  while (true) {
    Quanta::Value result = Quanta::Embed::Call(ctx, next, iterator);
    if (Quanta::Embed::HasException(ctx)) return false;
    if (!Quanta::Embed::IsObject(result)) {
      Quanta::Embed::ThrowTypeError(ctx, "The iterator result is not an object");
      return false;
    }
    Quanta::Value done = Quanta::Embed::Get(ctx, result, "done");
    if (Quanta::Embed::HasException(ctx)) return false;
    if (done.to_boolean()) return true;
    Quanta::Value item = Quanta::Embed::Get(ctx, result, "value");
    if (Quanta::Embed::HasException(ctx)) return false;
    if (!visit(item)) return false;
  }
}


// The conversion of an object to a record, in the steps and the order Web IDL gives, which a Proxy can
// see: [[OwnPropertyKeys]], then for each key a [[GetOwnProperty]] and, if the property is enumerable,
// the key's conversion to the record's key type, then a [[Get]] of the value. `convertKey` is that
// conversion (a symbol cannot be converted to a string, so it throws there); `visit` gets the value.
// False from either stops. False, with an exception pending, when anything fails.
template <typename ConvertKey, typename Visit>
inline bool IterateRecord(Quanta::Context& ctx, const Quanta::Value& object, ConvertKey convertKey, Visit visit) {
  Quanta::Embed::ValueList keys = Quanta::Embed::OwnPropertyKeys(ctx, object);
  if (Quanta::Embed::HasException(ctx)) return false;
  for (const Quanta::Value& key : keys) {
    const bool enumerable = Quanta::Embed::GetOwnEnumerable(ctx, object, key);
    if (Quanta::Embed::HasException(ctx)) return false;
    if (!enumerable) continue;
    if (!convertKey(key)) return false;
    Quanta::Value value = Quanta::Embed::Get(ctx, object, key);
    if (Quanta::Embed::HasException(ctx)) return false;
    if (!visit(value)) return false;
  }
  return true;
}

}  // namespace solar::web
