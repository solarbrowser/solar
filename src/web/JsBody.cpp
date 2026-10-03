#include <string>

#include "solar/web/FetchBindingsInternal.h"
#include "solar/web/FetchHost.h"
#include "solar/web/UrlBindingsInternal.h"

namespace solar::web {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Object;
using Quanta::Value;

Value MakeTypeError(Context& ctx, const std::string& message) {
  ctx.throw_type_error(message);
  Value error = ctx.get_exception();
  ctx.clear_exception();
  return error;
}

Value RejectedPromise(Context& ctx, const Value& reason) {
  qe::PromiseCapability capability = qe::NewPromiseCapability(ctx);
  qe::Call(ctx, capability.reject, qe::Undefined(), qe::Args(&reason, 1));
  return capability.promise;
}

Value ResolvedPromise(Context& ctx, const Value& value) {
  qe::PromiseCapability capability = qe::NewPromiseCapability(ctx);
  qe::Call(ctx, capability.resolve, qe::Undefined(), qe::Args(&value, 1));
  return capability.promise;
}

// Which the engine's FromUtf8 does with U+FFFD for what is malformed.
Value DecodeUtf8(Context& ctx, std::string_view bytes) {
  if (bytes.starts_with("\xEF\xBB\xBF")) bytes.remove_prefix(3);
  return qe::FromUtf8(ctx, bytes);
}

std::string BlobTypeFromContentType(std::string_view contentType) {
  // type "/" subtype, each a token, before any parameters; the rest is kept as it is.
  size_t start = 0;
  while (start < contentType.size() && (contentType[start] == ' ' || contentType[start] == '\t')) ++start;
  size_t end = contentType.size();
  while (end > start && (contentType[end - 1] == ' ' || contentType[end - 1] == '\t')) --end;
  const std::string_view value = contentType.substr(start, end - start);
  const size_t slash = value.find('/');
  const size_t semicolon = value.find(';');
  if (slash == std::string_view::npos || (semicolon != std::string_view::npos && semicolon < slash)) return "";
  const std::string_view essence = value.substr(0, semicolon);
  const std::string_view type = essence.substr(0, slash), subtype = essence.substr(slash + 1);
  const auto isToken = [](std::string_view part) {
    return !part.empty() && part.find_first_not_of("!#$%&'*+-.^_`|~0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") == std::string_view::npos;
  };
  if (!isToken(type) || !isToken(subtype)) return "";
  std::string out(value);
  for (size_t i = 0; i < essence.size(); ++i) out[i] = out[i] >= 'A' && out[i] <= 'Z' ? static_cast<char>(out[i] + 0x20) : out[i];
  return out;
}

bool ExtractBody(Context& ctx, const Value& init, ExtractedBody& out) {
  auto buffer = std::make_shared<BodyBuffer>();
  if (JsBlob* blob = DOMObject::Cast<JsBlob>(init)) {
    buffer->bytes.assign(blob->Bytes());
    out.contentType = blob->type;
  } else if (JsUrlSearchParams* params = DOMObject::Cast<JsUrlSearchParams>(init)) {
    buffer->bytes = params->Params().ToString();
    out.contentType = "application/x-www-form-urlencoded;charset=UTF-8";
  } else if (std::optional<std::span<const uint8_t>> bytes = qe::BytesOf(init)) {
    buffer->bytes.assign(reinterpret_cast<const char*>(bytes->data()), bytes->size());
  } else if (qe::IsObject(init) && qe::IsCallable(qe::Get(ctx, init, "getReader"))) {
    qe::ThrowTypeError(ctx, "A ReadableStream body is not supported yet");
    return false;
  } else if (qe::IsObject(init) && DOMObject::Cast<DOMObject>(init)) {
    // Some other host object: no class of this build is a body.
    qe::ThrowTypeError(ctx, "This kind of body is not supported yet");
    return false;
  } else {
    buffer->bytes = qe::ToUsvUtf8(ctx, init);
    if (qe::HasException(ctx)) return false;
    out.contentType = "text/plain;charset=UTF-8";
  }
  out.buffer = std::move(buffer);
  return true;
}

namespace {

// Settles `capability`'s promise with the body read as `kind`, or rejects it if the body failed.
void Settle(FetchHost* host, Context& ctx, const qe::Persistent& resolve, const qe::Persistent& reject, const BodyBuffer& buffer, BodyKind kind, const std::string& mimeType) {
  if (buffer.failure) {
    Value error = buffer.failureValue ? buffer.failureValue->Get() : MakeTypeError(ctx, *buffer.failure);
    qe::Call(ctx, reject.Get(), qe::Undefined(), qe::Args(&error, 1));
    return;
  }
  Value result;
  switch (kind) {
    case BodyKind::Text:
      result = DecodeUtf8(ctx, buffer.bytes);
      break;
    case BodyKind::Json: {
      Value text = DecodeUtf8(ctx, buffer.bytes);
      result = qe::Call(ctx, host->JsonParse(), qe::Undefined(), qe::Args(&text, 1));
      break;
    }
    case BodyKind::Blob:
      result = qe::FromObject(NewBlob(ctx, buffer.bytes, BlobTypeFromContentType(mimeType)));
      break;
    case BodyKind::ArrayBuffer:
    case BodyKind::Bytes: {
      Value array = qe::NewUint8Array(ctx, {reinterpret_cast<const uint8_t*>(buffer.bytes.data()), buffer.bytes.size()});
      result = !qe::HasException(ctx) && kind == BodyKind::ArrayBuffer ? qe::Get(ctx, array, "buffer") : array;
      break;
    }
  }
  if (qe::HasException(ctx)) {
    // JSON.parse refusing the text, say: that is the promise's rejection.
    Value error = ctx.get_exception();
    ctx.clear_exception();
    qe::Call(ctx, reject.Get(), qe::Undefined(), qe::Args(&error, 1));
    return;
  }
  qe::Call(ctx, resolve.Get(), qe::Undefined(), qe::Args(&result, 1));
}

}  // namespace

Value ConsumeBody(Context& ctx, JsBodyOwner* owner, BodyKind kind, const std::string& mimeType) {
  FetchHost* host = HostOf(ctx);
  qe::PromiseCapability capability = qe::NewPromiseCapability(ctx);
  if (!host) {
    Value error = MakeTypeError(ctx, "There is no fetch host in this realm");
    qe::Call(ctx, capability.reject, qe::Undefined(), qe::Args(&error, 1));
    return capability.promise;
  }
  if (owner->used) {
    Value error = MakeTypeError(ctx, "Body is unusable: Body has already been read");
    qe::Call(ctx, capability.reject, qe::Undefined(), qe::Args(&error, 1));
    return capability.promise;
  }

  qe::Persistent resolve(ctx, capability.resolve);
  qe::Persistent reject(ctx, capability.reject);
  if (!owner->body) {
    BodyBuffer empty;
    Settle(host, ctx, resolve, reject, empty, kind, mimeType);
    return capability.promise;
  }

  owner->used = true;
  std::shared_ptr<BodyBuffer> buffer = owner->body;
  if (buffer->complete || buffer->failure) {
    Settle(host, ctx, resolve, reject, *buffer, kind, mimeType);
    return capability.promise;
  }
  // Still arriving: the promise settles when it has, from the network's callback.
  auto resolveShared = std::make_shared<qe::Persistent>(std::move(resolve));
  auto rejectShared = std::make_shared<qe::Persistent>(std::move(reject));
  buffer->waiting.push_back([host, buffer, resolveShared, rejectShared, kind, mimeType] { Settle(host, host->context(), *resolveShared, *rejectShared, *buffer, kind, mimeType); });
  return capability.promise;
}

}  // namespace solar::web
