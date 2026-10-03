#include <string>

#include "solar/url/Parser.h"
#include "solar/url/Serializer.h"
#include "solar/web/FetchBindingsInternal.h"
#include "solar/web/FetchHost.h"
#include "solar/web/UrlBindingsInternal.h"

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

JsResponse* This(Context& ctx, const Value& thisValue) {
  JsResponse* self = DOMObject::Cast<JsResponse>(thisValue);
  if (!self) qe::ThrowTypeError(ctx, "Illegal invocation");
  return self;
}

JsResponse* Allocate(Context& ctx, Object* prototype) {
  JsResponse* response = Heap::Allocate<JsResponse>();
  response->initialize_prototype(prototype ? prototype : Prototype(ctx));
  return response;
}

bool IsNullBodyStatus(int status) { return status == 101 || status == 204 || status == 205 || status == 304; }

// A reason-phrase: tab, space, visible ASCII and the bytes above it, and no more.
bool IsReasonPhrase(const std::string& text) {
  for (unsigned char c : text) {
    if (!(c == '\t' || (c >= 0x20 && c <= 0x7E) || c >= 0x80)) return false;
  }
  return true;
}

// The ResponseInit dictionary, and the body, for `new Response` and Response.json.
bool Initialize(Context& ctx, JsResponse* response, const ExtractedBody* body, const Value& init) {
  int status = 200;
  std::string statusText;
  Value headersInit;

  if (!qe::IsUndefined(init) && !qe::IsNull(init)) {
    if (!qe::IsObject(init)) {
      qe::ThrowTypeError(ctx, "Failed to construct 'Response': The provided value is not of type 'ResponseInit'.");
      return false;
    }
    Value statusValue = qe::Get(ctx, init, "status");
    if (qe::HasException(ctx)) return false;
    if (!qe::IsUndefined(statusValue)) status = static_cast<int>(qe::ToUint32(ctx, statusValue) & 0xFFFF);
    if (qe::HasException(ctx)) return false;
    Value textValue = qe::Get(ctx, init, "statusText");
    if (qe::HasException(ctx)) return false;
    if (!qe::IsUndefined(textValue)) {
      // A ByteString: one byte for each character, none of them above U+00FF.
      const std::string text = qe::ToUsvUtf8(ctx, textValue);
      if (qe::HasException(ctx)) return false;
      std::optional<std::string> bytes = net::ToByteString(text);
      if (!bytes) {
        qe::ThrowTypeError(ctx, "Failed to construct 'Response': Invalid statusText");
        return false;
      }
      statusText = std::move(*bytes);
    }
    headersInit = qe::Get(ctx, init, "headers");
    if (qe::HasException(ctx)) return false;
  }
  if (status < 200 || status > 599) {
    ctx.throw_range_error("Failed to construct 'Response': The status provided (" + std::to_string(status) + ") is outside the range [200, 599].");
    return false;
  }
  if (!IsReasonPhrase(statusText)) {
    qe::ThrowTypeError(ctx, "Failed to construct 'Response': Invalid statusText");
    return false;
  }
  response->status = status;
  response->statusText = statusText;
  response->headers = AllocateHeaders(ctx, net::HeadersGuard::Response);
  response->NoteWrite(qe::FromObject(response->headers));
  if (!qe::IsUndefined(headersInit) && !FillHeaders(ctx, response->headers, headersInit, "Failed to construct 'Response'")) return false;

  if (body && body->buffer) {
    if (IsNullBodyStatus(status)) {
      qe::ThrowTypeError(ctx, "Failed to construct 'Response': Response with null body status cannot have body");
      return false;
    }
    response->body = body->buffer;
    if (!body->contentType.empty() && !response->headers->headers.Has("content-type")) {
      response->headers->headers.Append("Content-Type", body->contentType);
    }
  }
  return true;
}

Value Construct(Context& ctx, Value, qe::Args args, Value newTarget) {
  if (qe::IsUndefined(newTarget)) {
    qe::ThrowTypeError(ctx, "Failed to construct 'Response': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    return qe::Undefined();
  }
  ExtractedBody body;
  const bool hasBody = !args.empty() && !qe::IsUndefined(args[0]) && !qe::IsNull(args[0]);
  if (hasBody && !ExtractBody(ctx, args[0], body)) return qe::Undefined();
  Object* prototype = qe::PrototypeFromNewTarget(ctx, newTarget);
  if (qe::HasException(ctx)) return qe::Undefined();

  JsResponse* response = Allocate(ctx, prototype);
  if (!Initialize(ctx, response, hasBody ? &body : nullptr, args.size() > 1 ? args[1] : qe::Undefined())) return qe::Undefined();
  return qe::FromObject(response);
}

Value Error(Context& ctx, Value, qe::Args, Value) {
  JsResponse* response = Allocate(ctx, nullptr);
  response->type = ResponseType::Error;
  response->status = 0;
  response->headers = AllocateHeaders(ctx, net::HeadersGuard::Immutable);
  response->NoteWrite(qe::FromObject(response->headers));
  return qe::FromObject(response);
}

Value Redirect(Context& ctx, Value, qe::Args args, Value) {
  FetchHost* host = HostOf(ctx);
  if (args.empty()) {
    qe::ThrowTypeError(ctx, "Failed to execute 'redirect' on 'Response': 1 argument required, but only 0 present.");
    return qe::Undefined();
  }
  const std::string text = qe::ToUsvUtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  int status = 302;
  if (args.size() > 1 && !qe::IsUndefined(args[1])) status = static_cast<int>(qe::ToUint32(ctx, args[1]) & 0xFFFF);
  if (qe::HasException(ctx)) return qe::Undefined();

  std::optional<url::Url> parsed = url::Parse(text, host ? &host->config().pageUrl : nullptr);
  if (!parsed) {
    qe::ThrowTypeError(ctx, "Failed to execute 'redirect' on 'Response': Invalid URL");
    return qe::Undefined();
  }
  if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308) {
    ctx.throw_range_error("Failed to execute 'redirect' on 'Response': Invalid status code");
    return qe::Undefined();
  }
  JsResponse* response = Allocate(ctx, nullptr);
  response->status = status;
  response->headers = AllocateHeaders(ctx, net::HeadersGuard::Response);
  response->NoteWrite(qe::FromObject(response->headers));
  response->headers->headers.Append("Location", net::ToByteString(url::Serialize(*parsed)).value_or(""));
  response->headers->headers.SetGuard(net::HeadersGuard::Immutable);
  return qe::FromObject(response);
}

Value Json(Context& ctx, Value, qe::Args args, Value) {
  FetchHost* host = HostOf(ctx);
  if (args.empty()) {
    qe::ThrowTypeError(ctx, "Failed to execute 'json' on 'Response': 1 argument required, but only 0 present.");
    return qe::Undefined();
  }
  if (!host) {
    qe::ThrowTypeError(ctx, "There is no fetch host in this realm");
    return qe::Undefined();
  }
  Value text = qe::Call(ctx, host->JsonStringify(), qe::Undefined(), qe::Args(args.data(), 1));
  if (qe::HasException(ctx)) return qe::Undefined();
  if (!text.is_string()) {
    qe::ThrowTypeError(ctx, "Failed to execute 'json' on 'Response': The data is not JSON serializable");
    return qe::Undefined();
  }
  ExtractedBody body;
  body.buffer = std::make_shared<BodyBuffer>();
  body.buffer->bytes = qe::ToUsvUtf8(ctx, text);
  body.contentType = "application/json";

  JsResponse* response = Allocate(ctx, nullptr);
  if (!Initialize(ctx, response, &body, args.size() > 1 ? args[1] : qe::Undefined())) return qe::Undefined();
  return qe::FromObject(response);
}

Value GetType(Context& ctx, Value t, qe::Args, Value) {
  JsResponse* self = This(ctx, t);
  if (!self) return qe::Undefined();
  static const char* const kNames[] = {"basic", "cors", "default", "error", "opaque", "opaqueredirect"};
  return qe::FromUtf8(ctx, kNames[static_cast<int>(self->type)]);
}

Value GetUrl(Context& ctx, Value t, qe::Args, Value) {
  JsResponse* self = This(ctx, t);
  return self ? qe::FromUtf8(ctx, self->url) : qe::Undefined();
}

Value GetRedirected(Context& ctx, Value t, qe::Args, Value) {
  JsResponse* self = This(ctx, t);
  return self ? qe::FromBool(self->redirected) : qe::Undefined();
}

Value GetStatus(Context& ctx, Value t, qe::Args, Value) {
  JsResponse* self = This(ctx, t);
  return self ? qe::FromUint32(static_cast<uint32_t>(self->status)) : qe::Undefined();
}

Value GetOk(Context& ctx, Value t, qe::Args, Value) {
  JsResponse* self = This(ctx, t);
  return self ? qe::FromBool(self->status >= 200 && self->status <= 299) : qe::Undefined();
}

Value GetStatusText(Context& ctx, Value t, qe::Args, Value) {
  JsResponse* self = This(ctx, t);
  return self ? qe::FromUtf8(ctx, net::IsomorphicDecode(self->statusText)) : qe::Undefined();
}

Value GetHeaders(Context& ctx, Value t, qe::Args, Value) {
  JsResponse* self = This(ctx, t);
  return self ? qe::FromObject(self->headers) : qe::Undefined();
}

Value GetBodyUsed(Context& ctx, Value t, qe::Args, Value) {
  JsResponse* self = This(ctx, t);
  return self ? qe::FromBool(self->used) : qe::Undefined();
}

// A body is a ReadableStream in the standard, and this build has none: a response without a body says so,
// and one with a body says what it cannot give.
Value GetBody(Context& ctx, Value t, qe::Args, Value) {
  JsResponse* self = This(ctx, t);
  if (!self) return qe::Undefined();
  if (!self->body) return qe::Null();
  qe::ThrowTypeError(ctx, "Response.body is not supported yet; read the body with text(), json(), arrayBuffer() or bytes()");
  return qe::Undefined();
}

Value Clone(Context& ctx, Value t, qe::Args, Value) {
  JsResponse* self = This(ctx, t);
  if (!self) return qe::Undefined();
  if (self->used) {
    qe::ThrowTypeError(ctx, "Failed to execute 'clone' on 'Response': Response body is already used");
    return qe::Undefined();
  }
  JsResponse* copy = Allocate(ctx, nullptr);
  copy->type = self->type;
  copy->status = self->status;
  copy->statusText = self->statusText;
  copy->url = self->url;
  copy->redirected = self->redirected;
  copy->body = self->body;  // the bytes are the same; each reads them for itself
  copy->headers = AllocateHeaders(ctx, self->headers->headers.guard());
  copy->headers->headers = self->headers->headers;
  copy->NoteWrite(qe::FromObject(copy->headers));
  return qe::FromObject(copy);
}

template <BodyKind Kind>
Value Consume(Context& ctx, Value t, qe::Args, Value) {
  JsResponse* self = This(ctx, t);
  if (!self) {
    // A method that returns a promise does not throw: a wrong receiver is a rejection.
    Value error = ctx.get_exception();
    ctx.clear_exception();
    return RejectedPromise(ctx, error);
  }
  return ConsumeBody(ctx, self, Kind, self->headers->headers.Get("content-type").value_or(""));
}

}  // namespace

void DefineResponseClass(Context& ctx) {
  qe::ClassRef response = qe::DefineClass(ctx, "Response", Construct, 0);
  qe::SetRealmData(ctx, &g_prototypeKey, response.prototype);
  qe::DefineStaticMethod(response.constructor, "error", Error, 0);
  qe::DefineStaticMethod(response.constructor, "redirect", Redirect, 1);
  qe::DefineStaticMethod(response.constructor, "json", Json, 1);
  qe::DefineAccessor(response.prototype, "type", GetType, nullptr);
  qe::DefineAccessor(response.prototype, "url", GetUrl, nullptr);
  qe::DefineAccessor(response.prototype, "redirected", GetRedirected, nullptr);
  qe::DefineAccessor(response.prototype, "status", GetStatus, nullptr);
  qe::DefineAccessor(response.prototype, "ok", GetOk, nullptr);
  qe::DefineAccessor(response.prototype, "statusText", GetStatusText, nullptr);
  qe::DefineAccessor(response.prototype, "headers", GetHeaders, nullptr);
  qe::DefineAccessor(response.prototype, "body", GetBody, nullptr);
  qe::DefineAccessor(response.prototype, "bodyUsed", GetBodyUsed, nullptr);
  qe::DefineMethod(response.prototype, "clone", Clone, 0);
  qe::DefineMethod(response.prototype, "text", Consume<BodyKind::Text>, 0);
  qe::DefineMethod(response.prototype, "json", Consume<BodyKind::Json>, 0);
  qe::DefineMethod(response.prototype, "arrayBuffer", Consume<BodyKind::ArrayBuffer>, 0);
  qe::DefineMethod(response.prototype, "bytes", Consume<BodyKind::Bytes>, 0);
  qe::DefineMethod(response.prototype, "blob", Consume<BodyKind::Blob>, 0);
  qe::DefineMethod(response.prototype, "formData", Consume<BodyKind::FormData>, 0);
  qe::DefineGlobal(ctx, "Response", response.constructor);
}

JsResponse* NewResponse(Context& ctx) { return Allocate(ctx, nullptr); }

}  // namespace solar::web
