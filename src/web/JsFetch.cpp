#include <algorithm>
#include <string>

#include "solar/net/FetchHeaders.h"
#include "solar/url/Origin.h"
#include "solar/url/Serializer.h"
#include "solar/web/FetchBindingsInternal.h"
#include "solar/web/FetchHost.h"
#include "solar/web/FetchOperation.h"

namespace solar::web {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Value;

namespace {

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return (x >= 'A' && x <= 'Z' ? x + 0x20 : x) == (y >= 'A' && y <= 'Z' ? y + 0x20 : y);
         });
}

std::string_view Trimmed(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

bool IsRedirectStatus(int status) { return status == 301 || status == 302 || status == 303 || status == 307 || status == 308; }

size_t CountHeaders(const net::HttpResponseHead& head, std::string_view name) {
  return static_cast<size_t>(std::count_if(head.headers.begin(), head.headers.end(), [&](const auto& h) { return EqualsIgnoreCase(h.first, name); }));
}

// The CORS check of the Fetch Standard: the response names this origin, or anyone if the request
// carried no credentials, and says so about credentials when it did.
bool CorsCheck(const net::HttpResponseHead& head, const std::string& origin, bool includeCredentials) {
  if (CountHeaders(head, "access-control-allow-origin") != 1) return false;
  const std::string_view allowed = *head.Header("access-control-allow-origin");
  if (!includeCredentials && allowed == "*") return true;
  if (allowed != origin) return false;
  if (!includeCredentials) return true;
  const auto credentials = head.Header("access-control-allow-credentials");
  return credentials && *credentials == "true";
}

// Which of a cors response's headers a script may see: the safelisted ones, and what the server exposes.
bool IsCorsExposed(const std::string& name, const net::HttpResponseHead& head, bool includeCredentials) {
  for (const char* safe : {"cache-control", "content-language", "content-length", "content-type", "expires", "last-modified", "pragma"}) {
    if (EqualsIgnoreCase(name, safe)) return true;
  }
  std::string exposed;
  for (const auto& [key, value] : head.headers) {
    if (!EqualsIgnoreCase(key, "access-control-expose-headers")) continue;
    if (!exposed.empty()) exposed += ",";
    exposed += value;
  }
  size_t start = 0;
  while (start <= exposed.size()) {
    size_t end = exposed.find(',', start);
    if (end == std::string::npos) end = exposed.size();
    const std::string_view token = Trimmed(std::string_view(exposed).substr(start, end - start));
    if (token == "*" && !includeCredentials) return true;
    if (!token.empty() && EqualsIgnoreCase(token, name)) return true;
    start = end + 1;
  }
  return false;
}

// What a Referer header carries under the request's referrer policy (Referrer Policy, "determine the
// request's referrer"), or nothing.
std::optional<std::string> RefererFor(const std::string& policy, const url::Url& page, const url::Url& target) {
  if (page.scheme != "http" && page.scheme != "https") return std::nullopt;
  url::Url full = page;
  full.fragment.reset();
  full.username.clear();
  full.password.clear();
  const std::string fullText = url::Serialize(full);
  const std::string originText = url::SerializeOrigin(page) + "/";
  const bool sameOrigin = url::SerializeOrigin(page) == url::SerializeOrigin(target);
  const bool downgrade = page.scheme == "https" && target.scheme != "https";

  const std::string effective = policy.empty() ? "strict-origin-when-cross-origin" : policy;
  if (effective == "no-referrer") return std::nullopt;
  if (effective == "origin") return originText;
  if (effective == "unsafe-url") return fullText;
  if (effective == "same-origin") return sameOrigin ? std::optional<std::string>(fullText) : std::nullopt;
  if (effective == "origin-when-cross-origin") return sameOrigin ? fullText : originText;
  if (effective == "no-referrer-when-downgrade") return downgrade ? std::nullopt : std::optional<std::string>(fullText);
  if (effective == "strict-origin") return downgrade ? std::nullopt : std::optional<std::string>(originText);
  // strict-origin-when-cross-origin
  if (sameOrigin) return fullText;
  return downgrade ? std::nullopt : std::optional<std::string>(originText);
}

bool NeedsPreflight(const std::string& method, const net::FetchHeaders& headers) {
  if (method != "GET" && method != "HEAD" && method != "POST") return true;
  for (const auto& [name, value] : headers.list()) {
    if (!net::IsNoCorsSafelistedRequestHeader(name, value)) return true;
  }
  return false;
}

}  // namespace

void FetchOperation::FailWith(const Value& reason, const std::string& message) {
  Context& ctx = host.context();
  if (!settled) {
    settled = true;
    qe::Call(ctx, reject.Get(), qe::Undefined(), qe::Args(&reason, 1));
  } else if (body && !body->complete && !body->failure) {
    body->failure = message;
    body->failureValue = std::make_shared<qe::Persistent>(ctx, reason);
    std::vector<std::function<void()>> waiting = std::move(body->waiting);
    body->waiting.clear();
    for (auto& wake : waiting) wake();
  }
  if (host.config().afterScript) host.config().afterScript();
}

void FetchOperation::Done() {
  if (finished) return;
  finished = true;
  if (signal && abortListener) std::erase(signal->abortAlgorithms, abortListener);
  host.Finish(this);
}

bool FetchOperation::OnRedirect(const net::HttpResponseHead& redirect, const url::Url& next, std::vector<std::pair<std::string, std::string>>& headers) {
  if (settled && !body) return false;
  ++redirects;
  const bool nextCrossOrigin = url::SerializeOrigin(next) != pageOrigin;
  // A cors request that has gone cross-origin must have its redirects allowed by each server.
  if (mode == RequestMode::Cors && url::SerializeOrigin(currentUrl) != pageOrigin &&
      !CorsCheck(redirect, taintedOrigin ? "null" : pageOrigin, credentials == RequestCredentials::Include)) {
    FailWith(MakeTypeError(host.context(), "Failed to fetch: a redirect was not allowed by CORS"), "a redirect was not allowed by CORS");
    return false;
  }
  if (next.scheme != "http" && next.scheme != "https") return false;
  if (mode == RequestMode::Cors && nextCrossOrigin && next.IncludesCredentials()) return false;

  // Having gone from one origin to another, and then on to a third, the request's origin is unknown.
  if (url::SerializeOrigin(next) != url::SerializeOrigin(currentUrl) && url::SerializeOrigin(currentUrl) != pageOrigin) taintedOrigin = true;
  if (mode == RequestMode::Cors && nextCrossOrigin) corsTainted = true;
  currentUrl = next;

  // The Origin header follows: null once tainted, and dropped again on a same-origin GET.
  std::erase_if(headers, [](const auto& header) { return EqualsIgnoreCase(header.first, "origin"); });
  if (corsTainted || taintedOrigin) headers.emplace_back("Origin", taintedOrigin ? "null" : pageOrigin);
  return true;
}

void FetchOperation::OnResponseHead(const net::HttpResponseHead& head, const url::Url& finalUrl) {
  if (settled) return;  // aborted, or failed already
  Context& ctx = host.context();
  currentUrl = finalUrl;
  const bool includeCredentials = credentials == RequestCredentials::Include;

  if (mode == RequestMode::Cors && (corsTainted || crossOrigin) && !CorsCheck(head, taintedOrigin ? "null" : pageOrigin, includeCredentials)) {
    FailWith(MakeTypeError(ctx, "Failed to fetch: the response was not allowed by CORS"), "the response was not allowed by CORS");
    handle.Cancel();
    return;
  }

  JsResponse* r = NewResponse(ctx);
  url::Url withoutFragment = finalUrl;
  withoutFragment.fragment.reset();
  if (redirectMode == net::RedirectMode::Manual && IsRedirectStatus(head.status)) {
    r->type = ResponseType::OpaqueRedirect;
    r->status = 0;
    r->url = url::Serialize(withoutFragment);
    r->headers = NewResponseHeaders(ctx, {});
  } else if (mode == RequestMode::NoCors && crossOrigin) {
    r->type = ResponseType::Opaque;
    r->status = 0;
    r->headers = NewResponseHeaders(ctx, {});
  } else {
    r->type = corsTainted ? ResponseType::Cors : ResponseType::Basic;
    r->status = head.status;
    r->statusText = head.reason;
    r->url = url::Serialize(withoutFragment);
    r->redirected = redirects > 0;
    std::vector<std::pair<std::string, std::string>> list;
    for (const auto& header : head.headers) {
      if (r->type == ResponseType::Cors && !IsCorsExposed(header.first, head, includeCredentials)) continue;
      list.push_back(header);
    }
    r->headers = NewResponseHeaders(ctx, list);
    const bool noBody = head.status == 101 || head.status == 204 || head.status == 205 || head.status == 304;
    if (!noBody) {
      body = std::make_shared<BodyBuffer>();
      body->complete = false;
      r->body = body;
    }
  }
  r->NoteWrite(qe::FromObject(r->headers));
  response = r;
  responseValue = qe::Persistent(ctx, qe::FromObject(r));

  settled = true;
  Value value = qe::FromObject(r);
  qe::Call(ctx, resolve.Get(), qe::Undefined(), qe::Args(&value, 1));
  if (host.config().afterScript) host.config().afterScript();
}

void FetchOperation::OnBody(std::span<const uint8_t> data) {
  if (body && !body->failure) body->bytes.append(reinterpret_cast<const char*>(data.data()), data.size());
}

void FetchOperation::OnEnd() {
  if (body && !body->failure) {
    body->complete = true;
    qe::ReportExternalAllocation(body->bytes.size());
    std::vector<std::function<void()>> waiting = std::move(body->waiting);
    body->waiting.clear();
    for (auto& wake : waiting) wake();
  }
  if (host.config().afterScript) host.config().afterScript();
  Done();
}

void FetchOperation::OnError(std::string_view message) {
  if (!abortedByScript && !finished) {
    const std::string text = "Failed to fetch (" + std::string(message) + ")";
    FailWith(MakeTypeError(host.context(), text), text);
  }
  Done();
}

void CancelOperation(FetchOperation& operation) {
  if (operation.finished) return;
  operation.abortedByScript = true;  // the host is going away: nobody is left to tell
  operation.handle.Cancel();
  if (operation.body && !operation.body->complete) {
    operation.body->failure = "the page went away";
    operation.body->waiting.clear();
  }
  operation.Done();
}

namespace {

Value Fetch(Context& ctx, Value, qe::Args args, Value) {
  qe::PromiseCapability capability = qe::NewPromiseCapability(ctx);
  const auto reject = [&](const Value& reason) {
    qe::Call(ctx, capability.reject, qe::Undefined(), qe::Args(&reason, 1));
    return capability.promise;
  };
  const auto rejectWithPending = [&] {
    Value error = ctx.get_exception();
    ctx.clear_exception();
    return reject(error);
  };

  FetchHost* host = HostOf(ctx);
  if (!host) return reject(MakeTypeError(ctx, "Failed to fetch: this realm has no network"));
  if (args.empty()) return reject(MakeTypeError(ctx, "Failed to execute 'fetch' on 'Window': 1 argument required, but only 0 present."));

  JsRequest* request = MakeRequest(ctx, args[0], args.size() > 1 ? args[1] : qe::Undefined());
  if (!request) return rejectWithPending();
  if (request->signal->aborted) return reject(request->signal->reason);
  if (request->url.scheme != "http" && request->url.scheme != "https") return reject(MakeTypeError(ctx, "Failed to fetch: unsupported scheme " + request->url.scheme));
  if (!request->integrity.empty()) return reject(MakeTypeError(ctx, "Failed to fetch: integrity metadata is not supported yet"));

  const url::Url& page = host->config().pageUrl;
  auto operation = std::make_unique<FetchOperation>(*host);
  FetchOperation* op = operation.get();
  op->requestUrl = request->url;
  op->currentUrl = request->url;
  op->pageOrigin = url::SerializeOrigin(page);
  op->mode = request->mode;
  op->credentials = request->credentials;
  op->redirectMode = request->redirect;
  op->crossOrigin = url::SerializeOrigin(request->url) != op->pageOrigin;

  if (op->crossOrigin && request->mode == RequestMode::SameOrigin) return reject(MakeTypeError(ctx, "Failed to fetch: a same-origin request to another origin"));
  if (op->crossOrigin && request->mode == RequestMode::Cors) {
    op->corsTainted = true;
    if (NeedsPreflight(request->method, request->headers->headers)) return reject(MakeTypeError(ctx, "Failed to fetch: a CORS preflight is not supported yet"));
  }

  net::FetchOptions options;
  options.method = request->method;
  options.headers = request->headers->headers.list();
  if (request->body) {
    // The request's bytes go as they are, shared with it, and it is read now.
    options.body = std::shared_ptr<const std::string>(request->body, &request->body->bytes);
    request->used = true;
  }
  options.cache = request->cache;
  options.redirect = request->redirect;
  options.timeout = std::chrono::hours(24);
  options.useCookies = request->credentials == RequestCredentials::Include || (request->credentials == RequestCredentials::SameOrigin && !op->crossOrigin);
  options.initiator = page;
  if (const auto referer = RefererFor(request->referrer.empty() ? "no-referrer" : request->referrerPolicy, page, request->url);
      referer && !request->referrer.empty()) {
    options.headers.emplace_back("Referer", *referer);
  }
  // Origin goes with a cross-origin request, and with any request that is not a GET or a HEAD.
  if ((op->corsTainted || (request->method != "GET" && request->method != "HEAD")) && request->mode != RequestMode::NoCors) {
    options.headers.emplace_back("Origin", op->pageOrigin);
  }

  op->resolve = qe::Persistent(ctx, capability.resolve);
  op->reject = qe::Persistent(ctx, capability.reject);
  if (request->signal) {
    op->signal = request->signal;
    op->signalValue = qe::Persistent(ctx, qe::FromObject(request->signal));
    op->abortListener = std::make_shared<Listener>();
    op->abortListener->native = [op](Context&, JsEvent*) {
      if (op->finished) return;
      op->abortedByScript = true;
      op->FailWith(op->signal->reason, "aborted");
      op->handle.Cancel();
      op->Done();
    };
    op->signal->abortAlgorithms.push_back(op->abortListener);
  }

  host->Adopt(std::move(operation));
  op->handle = host->config().client->Fetch(request->url, *op, std::move(options));
  return capability.promise;
}

}  // namespace

void DefineFetchFunction(Context& ctx) {
  // fetch is a function on the global object, which the embedding surface can only make as a class's
  // interface object; the script that follows turns it into the plain function it should be.
  qe::ClassRef fetch = qe::DefineClass(ctx, "fetch", Fetch, 1);
  qe::DefineGlobal(ctx, "fetch", fetch.constructor);
}

}  // namespace solar::web
