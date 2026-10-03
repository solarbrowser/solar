#include <algorithm>
#include <string>

#include "solar/web/FetchBindingsInternal.h"
#include "solar/web/FetchHost.h"

namespace solar::web {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Object;
using Quanta::Value;

// A body is read by the streams script, which owns the ReadableStream and everything the standard says
// of it. These are the two halves of the hand-over: what the script asks of the body (how much of its
// bytes there are, a wake-up when there are more) and what it asks of the script (a stream over them, a
// tee, the whole of one read).

namespace {

char g_helpersKey;

// A function the script registered, looked up on the hidden prototype that holds them. That prototype
// belongs to a class that is never made visible, so a page has no way to reach, or to replace, any of them.
Value Helper(Context& ctx, const char* name) {
  Object* holder = static_cast<Object*>(qe::GetRealmData(ctx, &g_helpersKey));
  return holder ? qe::Get(ctx, qe::FromObject(holder), name) : qe::Undefined();
}

Value CallHelper(Context& ctx, const char* name, std::initializer_list<Value> arguments) {
  Value function = Helper(ctx, name);
  if (!qe::IsCallable(function)) {
    qe::ThrowTypeError(ctx, "The streams of this realm are not set up");
    return qe::Undefined();
  }
  std::vector<Value> argv(arguments);
  return qe::Call(ctx, function, qe::Undefined(), qe::Args(argv.data(), argv.size()));
}

JsBodyOwner* OwnerOf(Context& ctx, const Value& value) {
  JsBodyOwner* owner = DOMObject::Cast<JsBodyOwner>(value);
  if (!owner) qe::ThrowTypeError(ctx, "Illegal invocation");
  return owner;
}

// __solarRegisterBody({ ... }): the script gives the functions it will be asked for.
Value Register(Context& ctx, Value, qe::Args args, Value) {
  Object* holder = static_cast<Object*>(qe::GetRealmData(ctx, &g_helpersKey));
  if (!holder || args.empty() || !qe::IsObject(args[0])) return qe::Undefined();
  for (const char* name : {"make", "readAll", "tee", "proxy", "isStream", "isUnusable", "isDisturbed", "blobStream"}) {
    Value function = qe::Get(ctx, args[0], name);
    qe::Set(ctx, qe::FromObject(holder), name, function);
  }
  return qe::Undefined();
}

enum BodyState { kWait = 0, kData = 1, kDone = 2, kFailed = 3 };

// __solarBodyState(owner, offset): what a stream that has read `offset` bytes of the body can do next.
Value State(Context& ctx, Value, qe::Args args, Value) {
  JsBodyOwner* owner = args.size() > 1 ? OwnerOf(ctx, args[0]) : nullptr;
  if (!owner || !owner->body) return qe::FromUint32(kDone);
  const size_t offset = qe::ToUint32(ctx, args[1]);
  const BodyBuffer& buffer = *owner->body;
  if (offset < buffer.bytes.size()) return qe::FromUint32(kData);
  if (buffer.failure) return qe::FromUint32(kFailed);
  return qe::FromUint32(buffer.complete ? kDone : kWait);
}

// __solarBodyTake(owner, offset): the bytes there are from `offset` on, as a Uint8Array.
Value Take(Context& ctx, Value, qe::Args args, Value) {
  JsBodyOwner* owner = args.size() > 1 ? OwnerOf(ctx, args[0]) : nullptr;
  if (!owner || !owner->body) return qe::Undefined();
  const size_t offset = std::min<size_t>(qe::ToUint32(ctx, args[1]), owner->body->bytes.size());
  const std::string& bytes = owner->body->bytes;
  // A piece at a time, so that a long body does not become one allocation in the stream's queue.
  const size_t count = std::min<size_t>(bytes.size() - offset, 64 * 1024);
  return qe::NewUint8Array(ctx, {reinterpret_cast<const uint8_t*>(bytes.data()) + offset, count});
}

// __solarBodyError(owner): why the body failed.
Value Error(Context& ctx, Value, qe::Args args, Value) {
  JsBodyOwner* owner = !args.empty() ? OwnerOf(ctx, args[0]) : nullptr;
  if (!owner || !owner->body || !owner->body->failure) return qe::Undefined();
  const BodyBuffer& buffer = *owner->body;
  return buffer.failureValue ? buffer.failureValue->Get() : MakeTypeError(ctx, *buffer.failure);
}

// __solarBodyWatch(owner, wake): calls wake() once the body has more, or has ended.
Value Watch(Context& ctx, Value, qe::Args args, Value) {
  JsBodyOwner* owner = args.size() > 1 ? OwnerOf(ctx, args[0]) : nullptr;
  FetchHost* host = HostOf(ctx);
  if (!owner || !owner->body || !host) return qe::Undefined();
  auto wake = std::make_shared<qe::Persistent>(ctx, args[1]);
  owner->body->watchers.push_back([host, wake] {
    Context& context = host->context();
    qe::Call(context, wake->Get(), qe::Undefined());
    if (qe::HasException(context)) context.clear_exception();
  });
  return qe::Undefined();
}

// __solarBodyFilled(owner, bytes) and __solarBodyFailed(owner, reason): the end of a ReadBodyStream.
Value Filled(Context& ctx, Value, qe::Args args, Value) {
  JsBodyOwner* owner = args.size() > 1 ? OwnerOf(ctx, args[0]) : nullptr;
  if (!owner || !owner->pendingRead) return qe::Undefined();
  auto pending = std::move(*owner->pendingRead);
  owner->pendingRead.reset();
  std::string bytes;
  if (const auto view = qe::BytesOf(args[1])) bytes.assign(reinterpret_cast<const char*>(view->data()), view->size());
  pending.ok(std::move(bytes));
  return qe::Undefined();
}

Value Failed(Context& ctx, Value, qe::Args args, Value) {
  JsBodyOwner* owner = args.size() > 1 ? OwnerOf(ctx, args[0]) : nullptr;
  if (!owner || !owner->pendingRead) return qe::Undefined();
  auto pending = std::move(*owner->pendingRead);
  owner->pendingRead.reset();
  pending.fail(args[1]);
  return qe::Undefined();
}

// __solarBlobChunk(blob, offset): the next piece of a Blob's bytes, or null at its end.
Value BlobChunk(Context& ctx, Value, qe::Args args, Value) {
  JsBlob* blob = args.size() > 1 ? DOMObject::Cast<JsBlob>(args[0]) : nullptr;
  if (!blob) {
    qe::ThrowTypeError(ctx, "Illegal invocation");
    return qe::Undefined();
  }
  const std::string_view bytes = blob->Bytes();
  const size_t offset = qe::ToUint32(ctx, args[1]);
  if (offset >= bytes.size()) return qe::Null();
  const size_t count = std::min<size_t>(bytes.size() - offset, 64 * 1024);
  return qe::NewUint8Array(ctx, {reinterpret_cast<const uint8_t*>(bytes.data()) + offset, count});
}

Value HolderConstructor(Context& ctx, Value, qe::Args, Value) {
  qe::ThrowTypeError(ctx, "Illegal constructor");
  return qe::Undefined();
}

}  // namespace

void DefineBodyStreamFunctions(Context& ctx) {
  // A class no one is given: its prototype is where the script's functions are kept.
  qe::ClassRef holder = qe::DefineClass(ctx, "SolarBodyHelpers", HolderConstructor, 0);
  qe::SetRealmData(ctx, &g_helpersKey, holder.prototype);
  qe::DefineGlobalFunction(ctx, "__solarRegisterBody", Register, 1);
  qe::DefineGlobalFunction(ctx, "__solarBodyState", State, 2);
  qe::DefineGlobalFunction(ctx, "__solarBodyTake", Take, 2);
  qe::DefineGlobalFunction(ctx, "__solarBodyError", Error, 1);
  qe::DefineGlobalFunction(ctx, "__solarBodyWatch", Watch, 2);
  qe::DefineGlobalFunction(ctx, "__solarBodyFilled", Filled, 2);
  qe::DefineGlobalFunction(ctx, "__solarBodyFailed", Failed, 2);
  qe::DefineGlobalFunction(ctx, "__solarBlobChunk", BlobChunk, 2);
}

bool IsReadableStream(Context& ctx, const Value& value) {
  Value answer = CallHelper(ctx, "isStream", {value});
  if (qe::HasException(ctx)) {
    ctx.clear_exception();
    return false;
  }
  return answer.to_boolean();
}

bool StreamIsUnusable(Context& ctx, Object* stream) {
  Value answer = CallHelper(ctx, "isUnusable", {qe::FromObject(stream)});
  return !qe::HasException(ctx) && answer.to_boolean();
}

Object* ProxyBodyStream(Context& ctx, Object* stream) {
  Value proxy = CallHelper(ctx, "proxy", {qe::FromObject(stream)});
  return !qe::HasException(ctx) && qe::IsObject(proxy) ? proxy.as_object() : nullptr;
}

Object* BodyStream(Context& ctx, JsBodyOwner* owner) {
  if (owner->stream) return owner->stream;
  if (!owner->body) return nullptr;
  Value stream = CallHelper(ctx, "make", {qe::FromObject(owner), qe::FromBool(owner->used)});
  if (qe::HasException(ctx) || !qe::IsObject(stream)) return nullptr;
  owner->stream = stream.as_object();
  owner->NoteWrite(stream);
  return owner->stream;
}

bool BodyIsUsed(Context& ctx, JsBodyOwner* owner) {
  if (owner->used) return true;
  if (!owner->stream) return false;
  Value disturbed = CallHelper(ctx, "isDisturbed", {qe::FromObject(owner->stream)});
  if (qe::HasException(ctx)) {
    ctx.clear_exception();
    return false;
  }
  return disturbed.to_boolean();
}

bool ReadBodyStream(Context& ctx, JsBodyOwner* owner, std::function<void(std::string)> ok, std::function<void(const Value&)> fail) {
  Object* stream = owner->stream ? owner->stream : BodyStream(ctx, owner);
  if (!stream) {
    if (!qe::HasException(ctx)) qe::ThrowTypeError(ctx, "There is no body to read");
    return false;
  }
  const bool unusable = StreamIsUnusable(ctx, stream);
  if (qe::HasException(ctx)) return false;
  if (unusable || owner->pendingRead) {
    qe::ThrowTypeError(ctx, "Body is unusable: Body has already been read");
    return false;
  }
  owner->pendingRead = JsBodyOwner::PendingRead{std::move(ok), std::move(fail)};
  CallHelper(ctx, "readAll", {qe::FromObject(owner), qe::FromObject(stream)});
  if (qe::HasException(ctx)) {
    owner->pendingRead.reset();
    return false;
  }
  return true;
}

bool CloneBody(Context& ctx, JsBodyOwner* from, JsBodyOwner* to) {
  if (!from->stream) {
    to->body = from->body;  // the bytes are the same; each reads them for itself
    return true;
  }
  Value pair = CallHelper(ctx, "tee", {qe::FromObject(from->stream)});
  if (qe::HasException(ctx)) return false;
  Value first = qe::Get(ctx, pair, "0"), second = qe::Get(ctx, pair, "1");
  if (qe::HasException(ctx) || !qe::IsObject(first) || !qe::IsObject(second)) return false;
  from->stream = first.as_object();
  from->NoteWrite(first);
  to->stream = second.as_object();
  to->NoteWrite(second);
  to->body = from->body;
  return true;
}

Value BlobStream(Context& ctx, JsBlob* blob) { return CallHelper(ctx, "blobStream", {qe::FromObject(blob)}); }

}  // namespace solar::web
