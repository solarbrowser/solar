#include <algorithm>
#include <string>

#include "solar/net/FetchHeaders.h"
#include "solar/url/Origin.h"
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

JsRequest* This(Context& ctx, const Value& thisValue) {
  JsRequest* self = DOMObject::Cast<JsRequest>(thisValue);
  if (!self) qe::ThrowTypeError(ctx, "Illegal invocation");
  return self;
}

JsRequest* Allocate(Context& ctx, Object* prototype) {
  JsRequest* request = Heap::Allocate<JsRequest>();
  request->initialize_prototype(prototype ? prototype : Prototype(ctx));
  return request;
}

bool EqualsIgnoreCase(const std::string& a, const char* b) {
  return a.size() == std::char_traits<char>::length(b) && std::equal(a.begin(), a.end(), b, [](char x, char y) {
           return (x >= 'A' && x <= 'Z' ? x + 0x20 : x) == (y >= 'A' && y <= 'Z' ? y + 0x20 : y);
         });
}

// The members of a RequestInit, converted (but not yet weighed) in the order Web IDL reads them.
struct RequestInit {
  Value body;
  bool hasBody = false;
  std::optional<std::string> cache, credentials, method, mode, redirect, referrer, referrerPolicy, integrity;
  std::optional<bool> keepalive;
  Value headers;
  bool hasHeaders = false;
  Value signal;
  bool hasSignal = false;
  bool any = false;  // whether any member was given
};

bool ReadEnumMember(Context& ctx, const Value& init, const char* name, std::initializer_list<const char*> allowed, const char* typeName, std::optional<std::string>& out,
                    bool& any) {
  Value value = qe::Get(ctx, init, name);
  if (qe::HasException(ctx)) return false;
  if (qe::IsUndefined(value)) return true;
  std::string text = qe::ToUsvUtf8(ctx, value);
  if (qe::HasException(ctx)) return false;
  if (std::find_if(allowed.begin(), allowed.end(), [&](const char* name) { return text == name; }) == allowed.end()) {
    qe::ThrowTypeError(ctx, "Failed to construct 'Request': The provided value '" + text + "' is not a valid enum value of type " + typeName + ".");
    return false;
  }
  out = std::move(text);
  any = true;
  return true;
}

bool ReadInit(Context& ctx, const Value& init, RequestInit& out) {
  if (qe::IsUndefined(init) || qe::IsNull(init)) return true;
  if (!qe::IsObject(init)) {
    qe::ThrowTypeError(ctx, "Failed to construct 'Request': The provided value is not of type 'RequestInit'.");
    return false;
  }
  // The dictionary's members, in alphabetical order, which is the order a getter on it would see.
  out.body = qe::Get(ctx, init, "body");
  if (qe::HasException(ctx)) return false;
  out.hasBody = !qe::IsUndefined(out.body);
  out.any = out.any || out.hasBody;

  if (!ReadEnumMember(ctx, init, "cache", {"default", "no-store", "reload", "no-cache", "force-cache", "only-if-cached"}, "RequestCache", out.cache, out.any)) return false;
  if (!ReadEnumMember(ctx, init, "credentials", {"omit", "same-origin", "include"}, "RequestCredentials", out.credentials, out.any)) return false;
  Value duplex = qe::Get(ctx, init, "duplex");
  if (qe::HasException(ctx)) return false;
  if (!qe::IsUndefined(duplex)) {
    const std::string text = qe::ToUsvUtf8(ctx, duplex);
    if (qe::HasException(ctx)) return false;
    if (text != "half") {
      qe::ThrowTypeError(ctx, "Failed to construct 'Request': The provided value '" + text + "' is not a valid enum value of type RequestDuplex.");
      return false;
    }
    out.any = true;
  }
  out.headers = qe::Get(ctx, init, "headers");
  if (qe::HasException(ctx)) return false;
  out.hasHeaders = !qe::IsUndefined(out.headers);
  out.any = out.any || out.hasHeaders;
  Value integrity = qe::Get(ctx, init, "integrity");
  if (qe::HasException(ctx)) return false;
  if (!qe::IsUndefined(integrity)) {
    out.integrity = qe::ToUsvUtf8(ctx, integrity);
    if (qe::HasException(ctx)) return false;
    out.any = true;
  }
  Value keepalive = qe::Get(ctx, init, "keepalive");
  if (qe::HasException(ctx)) return false;
  if (!qe::IsUndefined(keepalive)) {
    out.keepalive = keepalive.to_boolean();
    out.any = true;
  }
  Value method = qe::Get(ctx, init, "method");
  if (qe::HasException(ctx)) return false;
  if (!qe::IsUndefined(method)) {
    out.method = qe::ToUsvUtf8(ctx, method);
    if (qe::HasException(ctx)) return false;
    out.any = true;
  }
  if (!ReadEnumMember(ctx, init, "mode", {"same-origin", "no-cors", "cors", "navigate"}, "RequestMode", out.mode, out.any)) return false;
  if (!ReadEnumMember(ctx, init, "redirect", {"follow", "error", "manual"}, "RequestRedirect", out.redirect, out.any)) return false;
  Value referrer = qe::Get(ctx, init, "referrer");
  if (qe::HasException(ctx)) return false;
  if (!qe::IsUndefined(referrer)) {
    out.referrer = qe::ToUsvUtf8(ctx, referrer);
    if (qe::HasException(ctx)) return false;
    out.any = true;
  }
  if (!ReadEnumMember(ctx, init, "referrerPolicy",
                      {"", "no-referrer", "no-referrer-when-downgrade", "same-origin", "origin", "strict-origin", "origin-when-cross-origin",
                       "strict-origin-when-cross-origin", "unsafe-url"},
                      "ReferrerPolicy", out.referrerPolicy, out.any)) {
    return false;
  }
  out.signal = qe::Get(ctx, init, "signal");
  if (qe::HasException(ctx)) return false;
  out.hasSignal = !qe::IsUndefined(out.signal);
  out.any = out.any || out.hasSignal;
  Value window = qe::Get(ctx, init, "window");
  if (qe::HasException(ctx)) return false;
  if (!qe::IsUndefined(window) && !qe::IsNull(window)) {
    qe::ThrowTypeError(ctx, "Failed to construct 'Request': Window can only be null.");
    return false;
  }
  return true;
}

net::CacheMode CacheModeOf(const std::string& name) {
  if (name == "no-store") return net::CacheMode::NoStore;
  if (name == "reload") return net::CacheMode::Reload;
  if (name == "no-cache") return net::CacheMode::NoCache;
  if (name == "force-cache") return net::CacheMode::ForceCache;
  if (name == "only-if-cached") return net::CacheMode::OnlyIfCached;
  return net::CacheMode::Default;
}

const char* CacheName(net::CacheMode mode) {
  switch (mode) {
    case net::CacheMode::NoStore: return "no-store";
    case net::CacheMode::Reload: return "reload";
    case net::CacheMode::NoCache: return "no-cache";
    case net::CacheMode::ForceCache: return "force-cache";
    case net::CacheMode::OnlyIfCached: return "only-if-cached";
    case net::CacheMode::Default: return "default";
  }
  return "default";
}

}  // namespace

JsRequest* MakeRequest(Context& ctx, const Value& input, const Value& init) {
  FetchHost* host = HostOf(ctx);
  const url::Url* base = host ? &host->config().pageUrl : nullptr;

  JsRequest* source = DOMObject::Cast<JsRequest>(input);
  std::string inputText;
  if (!source) {
    inputText = qe::ToUsvUtf8(ctx, input);
    if (qe::HasException(ctx)) return nullptr;
  }
  RequestInit members;
  if (!ReadInit(ctx, init, members)) return nullptr;

  JsRequest* request = Allocate(ctx, nullptr);
  std::optional<RequestMode> fallbackMode;
  if (source) {
    request->method = source->method;
    request->url = source->url;
    request->mode = source->mode;
    request->credentials = source->credentials;
    request->cache = source->cache;
    request->redirect = source->redirect;
    request->referrer = source->referrer;
    request->referrerPolicy = source->referrerPolicy;
    request->integrity = source->integrity;
    request->keepalive = source->keepalive;
  } else {
    std::optional<url::Url> parsed = url::Parse(inputText, base);
    if (!parsed) {
      qe::ThrowTypeError(ctx, "Failed to construct 'Request': Failed to parse URL from " + inputText);
      return nullptr;
    }
    if (parsed->IncludesCredentials()) {
      qe::ThrowTypeError(ctx, "Failed to construct 'Request': Request cannot be constructed from a URL that includes credentials: " + inputText);
      return nullptr;
    }
    request->url = std::move(*parsed);
    fallbackMode = RequestMode::Cors;
  }

  if (members.any) {
    if (request->mode == RequestMode::Navigate) request->mode = RequestMode::SameOrigin;
    request->referrer = "about:client";
    request->referrerPolicy = "";
  }
  if (members.referrer) {
    if (members.referrer->empty()) {
      request->referrer = "";
    } else {
      std::optional<url::Url> parsed = url::Parse(*members.referrer, base);
      if (!parsed) {
        qe::ThrowTypeError(ctx, "Failed to construct 'Request': Referrer '" + *members.referrer + "' is not a valid URL.");
        return nullptr;
      }
      const bool client = parsed->scheme == "about" && parsed->opaquePath && *parsed->opaquePath == "client";
      const bool sameOrigin = base && url::SerializeOrigin(*parsed) == url::SerializeOrigin(*base);
      request->referrer = client || !sameOrigin ? "about:client" : url::Serialize(*parsed);
    }
  }
  if (members.referrerPolicy) request->referrerPolicy = *members.referrerPolicy;

  std::optional<RequestMode> mode = fallbackMode;
  if (members.mode) mode = *members.mode == "same-origin" ? RequestMode::SameOrigin : *members.mode == "no-cors" ? RequestMode::NoCors : *members.mode == "navigate" ? RequestMode::Navigate : RequestMode::Cors;
  if (mode == RequestMode::Navigate) {
    qe::ThrowTypeError(ctx, "Failed to construct 'Request': Cannot construct a Request with a RequestInit whose mode member is set as 'navigate'.");
    return nullptr;
  }
  if (mode) request->mode = *mode;
  if (members.credentials) request->credentials = *members.credentials == "omit" ? RequestCredentials::Omit : *members.credentials == "include" ? RequestCredentials::Include : RequestCredentials::SameOrigin;
  if (members.cache) request->cache = CacheModeOf(*members.cache);
  if (request->cache == net::CacheMode::OnlyIfCached && request->mode != RequestMode::SameOrigin) {
    qe::ThrowTypeError(ctx, "Failed to construct 'Request': 'only-if-cached' can be set only with 'same-origin' mode");
    return nullptr;
  }
  if (members.redirect) request->redirect = *members.redirect == "error" ? net::RedirectMode::Error : *members.redirect == "manual" ? net::RedirectMode::Manual : net::RedirectMode::Follow;
  if (members.integrity) request->integrity = *members.integrity;
  if (members.keepalive) request->keepalive = *members.keepalive;

  if (members.method) {
    std::string method = *members.method;
    if (method.empty() || !std::all_of(method.begin(), method.end(), [](char c) { return net::IsValidHeaderName(std::string(1, c)); })) {
      qe::ThrowTypeError(ctx, "Failed to construct 'Request': '" + method + "' is not a valid HTTP method.");
      return nullptr;
    }
    if (EqualsIgnoreCase(method, "CONNECT") || EqualsIgnoreCase(method, "TRACE") || EqualsIgnoreCase(method, "TRACK")) {
      qe::ThrowTypeError(ctx, "Failed to construct 'Request': '" + method + "' HTTP method is unsupported.");
      return nullptr;
    }
    for (const char* known : {"DELETE", "GET", "HEAD", "OPTIONS", "POST", "PUT"}) {
      if (EqualsIgnoreCase(method, known)) method = known;
    }
    request->method = std::move(method);
  }

  // The signal: this request's own, which follows the one given (or the input request's).
  request->signal = NewAbortSignal(ctx);
  request->NoteWrite(qe::FromObject(request->signal));
  JsAbortSignal* followed = nullptr;
  if (members.hasSignal && !qe::IsNull(members.signal)) {
    followed = DOMObject::Cast<JsAbortSignal>(members.signal);
    if (!followed) {
      qe::ThrowTypeError(ctx, "Failed to construct 'Request': Failed to convert value to 'AbortSignal'.");
      return nullptr;
    }
  } else if (!members.hasSignal && source) {
    followed = source->signal;
  }
  if (followed) FollowSignal(ctx, request->signal, followed);

  // The headers: guarded as the mode says, and from the init if it has any, else the input's.
  const bool noCors = request->mode == RequestMode::NoCors;
  if (noCors && request->method != "GET" && request->method != "HEAD" && request->method != "POST") {
    qe::ThrowTypeError(ctx, "Failed to construct 'Request': '" + request->method + "' is unsupported in no-cors mode.");
    return nullptr;
  }
  request->headers = AllocateHeaders(ctx, net::HeadersGuard::None);
  request->NoteWrite(qe::FromObject(request->headers));
  if (members.hasHeaders) {
    request->headers->headers.SetGuard(noCors ? net::HeadersGuard::RequestNoCors : net::HeadersGuard::Request);
    if (!FillHeaders(ctx, request->headers, members.headers, "Failed to construct 'Request'")) return nullptr;
  } else {
    request->headers->headers.SetGuard(noCors ? net::HeadersGuard::RequestNoCors : net::HeadersGuard::Request);
    if (source) {
      for (const auto& [name, value] : source->headers->headers.list()) request->headers->headers.Append(name, value);
    }
  }

  // The body: the init's, or the input's, which then belongs to the new request alone.
  std::shared_ptr<BodyBuffer> body;
  std::string contentType;
  const bool initBody = members.hasBody && !qe::IsNull(members.body);
  if (initBody) {
    ExtractedBody extracted;
    if (!ExtractBody(ctx, members.body, extracted)) return nullptr;
    body = extracted.buffer;
    contentType = extracted.contentType;
  } else if (source && source->body) {
    if (source->used) {
      qe::ThrowTypeError(ctx, "Failed to construct 'Request': Cannot construct a Request with a Request object that has already been used.");
      return nullptr;
    }
    body = source->body;
    source->used = true;
  }
  if (body && (request->method == "GET" || request->method == "HEAD")) {
    qe::ThrowTypeError(ctx, "Failed to construct 'Request': Request with GET/HEAD method cannot have body.");
    return nullptr;
  }
  request->body = std::move(body);
  if (initBody && !contentType.empty() && !request->headers->headers.Has("content-type")) request->headers->headers.Append("Content-Type", contentType);
  return request;
}

namespace {

Value Construct(Context& ctx, Value, qe::Args args, Value newTarget) {
  if (qe::IsUndefined(newTarget)) {
    qe::ThrowTypeError(ctx, "Failed to construct 'Request': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    return qe::Undefined();
  }
  if (args.empty()) {
    qe::ThrowTypeError(ctx, "Failed to construct 'Request': 1 argument required, but only 0 present.");
    return qe::Undefined();
  }
  Object* prototype = qe::PrototypeFromNewTarget(ctx, newTarget);
  if (qe::HasException(ctx)) return qe::Undefined();
  JsRequest* request = MakeRequest(ctx, args[0], args.size() > 1 ? args[1] : qe::Undefined());
  if (!request) return qe::Undefined();
  if (prototype) request->initialize_prototype(prototype);
  return qe::FromObject(request);
}

Value GetMethod(Context& ctx, Value t, qe::Args, Value) {
  JsRequest* self = This(ctx, t);
  return self ? qe::FromUtf8(ctx, self->method) : qe::Undefined();
}

Value GetUrl(Context& ctx, Value t, qe::Args, Value) {
  JsRequest* self = This(ctx, t);
  return self ? qe::FromUtf8(ctx, url::Serialize(self->url)) : qe::Undefined();
}

Value GetHeaders(Context& ctx, Value t, qe::Args, Value) {
  JsRequest* self = This(ctx, t);
  return self ? qe::FromObject(self->headers) : qe::Undefined();
}

Value GetDestination(Context& ctx, Value t, qe::Args, Value) {
  return This(ctx, t) ? qe::FromUtf8(ctx, "") : qe::Undefined();
}

Value GetReferrer(Context& ctx, Value t, qe::Args, Value) {
  JsRequest* self = This(ctx, t);
  return self ? qe::FromUtf8(ctx, self->referrer) : qe::Undefined();
}

Value GetReferrerPolicy(Context& ctx, Value t, qe::Args, Value) {
  JsRequest* self = This(ctx, t);
  return self ? qe::FromUtf8(ctx, self->referrerPolicy) : qe::Undefined();
}

Value GetMode(Context& ctx, Value t, qe::Args, Value) {
  JsRequest* self = This(ctx, t);
  if (!self) return qe::Undefined();
  static const char* const kNames[] = {"same-origin", "no-cors", "cors", "navigate"};
  return qe::FromUtf8(ctx, kNames[static_cast<int>(self->mode)]);
}

Value GetCredentials(Context& ctx, Value t, qe::Args, Value) {
  JsRequest* self = This(ctx, t);
  if (!self) return qe::Undefined();
  static const char* const kNames[] = {"omit", "same-origin", "include"};
  return qe::FromUtf8(ctx, kNames[static_cast<int>(self->credentials)]);
}

Value GetCache(Context& ctx, Value t, qe::Args, Value) {
  JsRequest* self = This(ctx, t);
  return self ? qe::FromUtf8(ctx, CacheName(self->cache)) : qe::Undefined();
}

Value GetRedirect(Context& ctx, Value t, qe::Args, Value) {
  JsRequest* self = This(ctx, t);
  if (!self) return qe::Undefined();
  return qe::FromUtf8(ctx, self->redirect == net::RedirectMode::Error ? "error" : self->redirect == net::RedirectMode::Manual ? "manual" : "follow");
}

Value GetIntegrity(Context& ctx, Value t, qe::Args, Value) {
  JsRequest* self = This(ctx, t);
  return self ? qe::FromUtf8(ctx, self->integrity) : qe::Undefined();
}

Value GetKeepalive(Context& ctx, Value t, qe::Args, Value) {
  JsRequest* self = This(ctx, t);
  return self ? qe::FromBool(self->keepalive) : qe::Undefined();
}

Value GetFalse(Context& ctx, Value t, qe::Args, Value) {
  return This(ctx, t) ? qe::FromBool(false) : qe::Undefined();
}

Value GetSignal(Context& ctx, Value t, qe::Args, Value) {
  JsRequest* self = This(ctx, t);
  return self ? qe::FromObject(self->signal) : qe::Undefined();
}

Value GetDuplex(Context& ctx, Value t, qe::Args, Value) {
  return This(ctx, t) ? qe::FromUtf8(ctx, "half") : qe::Undefined();
}

Value GetBodyUsed(Context& ctx, Value t, qe::Args, Value) {
  JsRequest* self = This(ctx, t);
  return self ? qe::FromBool(self->used) : qe::Undefined();
}

// A body is a ReadableStream in the standard, and this build has none: a request without a body says so,
// and one with a body says what it cannot give.
Value GetBody(Context& ctx, Value t, qe::Args, Value) {
  JsRequest* self = This(ctx, t);
  if (!self) return qe::Undefined();
  if (!self->body) return qe::Null();
  qe::ThrowTypeError(ctx, "Request.body is not supported yet; read the body with text(), json(), arrayBuffer() or bytes()");
  return qe::Undefined();
}

Value Clone(Context& ctx, Value t, qe::Args, Value) {
  JsRequest* self = This(ctx, t);
  if (!self) return qe::Undefined();
  if (self->used) {
    qe::ThrowTypeError(ctx, "Failed to execute 'clone' on 'Request': Request body is already used");
    return qe::Undefined();
  }
  JsRequest* copy = Allocate(ctx, nullptr);
  copy->method = self->method;
  copy->url = self->url;
  copy->mode = self->mode;
  copy->credentials = self->credentials;
  copy->cache = self->cache;
  copy->redirect = self->redirect;
  copy->referrer = self->referrer;
  copy->referrerPolicy = self->referrerPolicy;
  copy->integrity = self->integrity;
  copy->keepalive = self->keepalive;
  copy->body = self->body;
  copy->headers = AllocateHeaders(ctx, self->headers->headers.guard());
  copy->headers->headers = self->headers->headers;
  copy->NoteWrite(qe::FromObject(copy->headers));
  copy->signal = NewAbortSignal(ctx);
  copy->NoteWrite(qe::FromObject(copy->signal));
  FollowSignal(ctx, copy->signal, self->signal);
  return qe::FromObject(copy);
}

template <BodyKind Kind>
Value Consume(Context& ctx, Value t, qe::Args, Value) {
  JsRequest* self = This(ctx, t);
  if (!self) {
    Value error = ctx.get_exception();
    ctx.clear_exception();
    return RejectedPromise(ctx, error);
  }
  return ConsumeBody(ctx, self, Kind);
}

}  // namespace

void DefineRequestClass(Context& ctx) {
  qe::ClassRef request = qe::DefineClass(ctx, "Request", Construct, 1);
  qe::SetRealmData(ctx, &g_prototypeKey, request.prototype);
  qe::DefineAccessor(request.prototype, "method", GetMethod, nullptr);
  qe::DefineAccessor(request.prototype, "url", GetUrl, nullptr);
  qe::DefineAccessor(request.prototype, "headers", GetHeaders, nullptr);
  qe::DefineAccessor(request.prototype, "destination", GetDestination, nullptr);
  qe::DefineAccessor(request.prototype, "referrer", GetReferrer, nullptr);
  qe::DefineAccessor(request.prototype, "referrerPolicy", GetReferrerPolicy, nullptr);
  qe::DefineAccessor(request.prototype, "mode", GetMode, nullptr);
  qe::DefineAccessor(request.prototype, "credentials", GetCredentials, nullptr);
  qe::DefineAccessor(request.prototype, "cache", GetCache, nullptr);
  qe::DefineAccessor(request.prototype, "redirect", GetRedirect, nullptr);
  qe::DefineAccessor(request.prototype, "integrity", GetIntegrity, nullptr);
  qe::DefineAccessor(request.prototype, "keepalive", GetKeepalive, nullptr);
  qe::DefineAccessor(request.prototype, "isReloadNavigation", GetFalse, nullptr);
  qe::DefineAccessor(request.prototype, "isHistoryNavigation", GetFalse, nullptr);
  qe::DefineAccessor(request.prototype, "signal", GetSignal, nullptr);
  qe::DefineAccessor(request.prototype, "duplex", GetDuplex, nullptr);
  qe::DefineAccessor(request.prototype, "body", GetBody, nullptr);
  qe::DefineAccessor(request.prototype, "bodyUsed", GetBodyUsed, nullptr);
  qe::DefineMethod(request.prototype, "clone", Clone, 0);
  qe::DefineMethod(request.prototype, "text", Consume<BodyKind::Text>, 0);
  qe::DefineMethod(request.prototype, "json", Consume<BodyKind::Json>, 0);
  qe::DefineMethod(request.prototype, "arrayBuffer", Consume<BodyKind::ArrayBuffer>, 0);
  qe::DefineMethod(request.prototype, "bytes", Consume<BodyKind::Bytes>, 0);
  qe::DefineGlobal(ctx, "Request", request.constructor);
}

}  // namespace solar::web
