/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <algorithm>
#include <string>

#include "solar/net/Cors.h"
#include "solar/net/FetchHeaders.h"
#include "solar/url/Origin.h"
#include "solar/url/Parser.h"
#include "solar/url/Serializer.h"
#include "solar/web/BlobUrls.h"
#include "solar/web/FetchBindingsInternal.h"
#include "solar/web/FetchHost.h"
#include "solar/web/MimeType.h"
#include "solar/web/FetchOperation.h"
#include "solar/web/Integrity.h"
#include "solar/web/Orb.h"

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

// Sec-Fetch-Site / -Mode / -Dest (Fetch Metadata): sent to the addresses a browser trusts (https, and the loopback), and to no others.
bool PotentiallyTrustworthy(const url::Url& address) {
  if (address.scheme == "https" || address.scheme == "wss") return true;
  if (address.scheme != "http" || !address.host) return false;
  const std::string& host = *address.host;
  return host == "localhost" || host.ends_with(".localhost") || host == "[::1]" || host.starts_with("127.");
}

void SetSecFetchHeaders(std::vector<std::pair<std::string, std::string>>& headers, const url::Url& target, int site, const std::string& mode) {
  std::erase_if(headers, [](const auto& header) { return header.first.size() > 10 && EqualsIgnoreCase(header.first.substr(0, 10), "sec-fetch-"); });
  if (!PotentiallyTrustworthy(target)) return;
  headers.emplace_back("Sec-Fetch-Dest", "empty");
  headers.emplace_back("Sec-Fetch-Mode", mode);
  headers.emplace_back("Sec-Fetch-Site", site == 0 ? "same-origin" : site == 1 ? "same-site" : "cross-site");
}

int SecFetchSiteOf(const url::Url& page, const url::Url& target) {
  if (url::SerializeOrigin(page) == url::SerializeOrigin(target)) return 0;
  return url::IsSameSite(page, target) ? 1 : 2;
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
    body->Wake(true);
  }
  if (host.config().afterScript) host.config().afterScript();
}

void FetchOperation::Done() {
  if (finished) return;
  finished = true;
  if (signal && abortListener) std::erase(signal->abortAlgorithms, abortListener);
  host.Finish(this);
}

bool FetchOperation::OnRedirectIn(const net::HttpResponseHead& redirect, const url::Url& next, std::vector<std::pair<std::string, std::string>>& headers) {
  if (settled && !body) return false;
  ++redirects;
  const bool nextCrossOrigin = url::SerializeOrigin(next) != pageOrigin;
  // A cors request that has gone cross-origin must have its redirects allowed by each server.
  if (mode == RequestMode::Cors && url::SerializeOrigin(currentUrl) != pageOrigin &&
      !net::CorsAllowsResponse(redirect, taintedOrigin ? "null" : pageOrigin, credentials == RequestCredentials::Include)) {
    FailWith(MakeTypeError(host.context(), "Failed to fetch: a redirect was not allowed by CORS"), "a redirect was not allowed by CORS");
    return false;
  }
  if (next.scheme != "http" && next.scheme != "https") return false;
  if (mode == RequestMode::Cors && (corsTainted || nextCrossOrigin) && next.IncludesCredentials()) {
    FailWith(MakeTypeError(host.context(), "Failed to fetch: a redirect to an address with credentials"), "a redirect to an address with credentials");
    return false;
  }
  // Having gone from one origin to another, and then on to a third, the request's origin is unknown.
  if (url::SerializeOrigin(next) != url::SerializeOrigin(currentUrl) && url::SerializeOrigin(currentUrl) != pageOrigin) taintedOrigin = true;
  if (mode == RequestMode::Cors && nextCrossOrigin) corsTainted = true;
  currentUrl = next;

  // The Origin header follows: null once tainted, and dropped again on a same-origin GET.
  std::erase_if(headers, [](const auto& header) { return EqualsIgnoreCase(header.first, "origin"); });
  // (301 and 302 turn a POST into a GET, 303 turns anything but a HEAD into one; 307 and 308 keep the method.)
  std::string next_method = method;
  if (((redirect.status == 301 || redirect.status == 302) && method == "POST") || (redirect.status == 303 && method != "GET" && method != "HEAD")) next_method = "GET";
  secFetchSite = std::max(secFetchSite, SecFetchSiteOf(secFetchPage, next));
  SetSecFetchHeaders(headers, next, secFetchSite, secFetchMode);
  // The Referrer-Policy of the redirect response is the policy from here on (the last of its tokens that is one), and the Referer is made again.
  if (const auto policyHeader = redirect.Header("referrer-policy")) {
    std::string value(*policyHeader), token;
    value.push_back(',');
    for (char c : value) {
      if (c != ',') { if (c != ' ' && c != '\t') token.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c)))); continue; }
      for (const char* known : {"no-referrer", "no-referrer-when-downgrade", "same-origin", "origin", "strict-origin", "origin-when-cross-origin", "strict-origin-when-cross-origin", "unsafe-url"}) {
        if (token == known) referrerPolicy = token;
      }
      token.clear();
    }
  }
  std::erase_if(headers, [](const auto& header) { return EqualsIgnoreCase(header.first, "referer"); });
  if (sendsReferrer) {
    // (What is sent is what the request's referrer is from then on: a policy cannot give back what an earlier one took away.)
    if (const auto referer = RefererFor(referrerPolicy, referrerSource, next)) {
      headers.emplace_back("Referer", *referer);
      if (const auto parsed = url::Parse(*referer)) referrerSource = *parsed;
    } else {
      sendsReferrer = false;
    }
  }
  if (corsTainted || taintedOrigin || (next_method != "GET" && next_method != "HEAD")) headers.emplace_back("Origin", taintedOrigin ? "null" : pageOrigin);
  method = next_method;
  if (nonSimple && corsTainted && mode == RequestMode::Cors) {
    // The target is asked first, as the request is: this hop ends here, and the next begins with the preflight.
    redirectForPreflight = true;
    redirectTarget = next;
    redirectOptions = mainOptions;
    redirectOptions.method = next_method;
    redirectOptions.headers = headers;
    if (next_method == "GET" || next_method == "HEAD") redirectOptions.body.reset();
    unsafeHeaders = net::CorsUnsafeRequestHeaderNames(requestHeaders);
    return false;
  }
  return true;
}

void FetchOperation::OnResponseHeadIn(const net::HttpResponseHead& head, const url::Url& finalUrl) {
  if (settled) return;  // aborted, or failed already
  if (preflighting) {
    const bool includeCredentials = credentials == RequestCredentials::Include;
    net::PreflightVerdict verdict = net::CheckPreflightResponse(head, pageOrigin, includeCredentials, method, unsafeHeaders);
    if (!verdict.ok) {
      const std::string text = "Failed to fetch (CORS preflight failed: " + verdict.reason + ")";
      FailWith(MakeTypeError(host.context(), text), text);
      handle.Cancel();
      return;
    }
    host.preflights().Store(pageOrigin, preflightUrl, includeCredentials, std::move(verdict.permission));
    return;  // the request itself goes when the preflight has ended
  }
  Context& ctx = host.context();
  currentUrl = finalUrl;
  const bool includeCredentials = credentials == RequestCredentials::Include;

  if (mode == RequestMode::Cors && (corsTainted || crossOrigin) && !net::CorsAllowsResponse(head, taintedOrigin ? "null" : pageOrigin, includeCredentials)) {
    FailWith(MakeTypeError(ctx, "Failed to fetch: the response was not allowed by CORS"), "the response was not allowed by CORS");
    handle.Cancel();
    return;
  }

  // A response to a no-cors request that is from another origin is checked by Opaque Response Blocking before the page has it.
  if (mode == RequestMode::NoCors && crossOrigin && !integrityReleased && integrity.empty()) {
    const auto header = [&](const char* name) { return head.Header(name).value_or(std::string_view()); };
    std::string nosniffValue(header("x-content-type-options"));
    for (char& c : nosniffValue) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const OrbVerdict verdict = OrbCheckHead(head.status, header("content-type"), nosniffValue.starts_with("nosniff"), header("content-range"));
    if (verdict == OrbVerdict::Block) {
      FailWith(MakeTypeError(ctx, "Failed to fetch: the response was blocked (ORB)"), "the response was blocked (ORB)");
      handle.Cancel();
      return;
    }
    if (verdict == OrbVerdict::NeedBody) {
      integrityBuffering = orbBuffering = true;
      integrityHead = head;
      integrityFinalUrl = finalUrl;
      return;
    }
  }
  if (!integrity.empty() && !integrityReleased) {
    // An opaque response cannot be checked; any other waits for its whole body.
    if (mode == RequestMode::NoCors && crossOrigin) {
      FailWith(MakeTypeError(ctx, "Failed to fetch: the integrity of an opaque response cannot be checked"), "integrity of an opaque response");
      handle.Cancel();
      return;
    }
    integrityBuffering = true;
    integrityHead = head;
    integrityFinalUrl = finalUrl;
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
      if (r->type == ResponseType::Cors && !net::IsCorsExposedHeader(header.first, head, includeCredentials)) continue;
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

void FetchOperation::OnBodyIn(std::span<const uint8_t> data) {
  if (preflighting) return;
  if (integrityBuffering) {
    integrityBytes.append(reinterpret_cast<const char*>(data.data()), data.size());
    return;
  }
  if (body && !body->failure) {
    body->bytes.append(reinterpret_cast<const char*>(data.data()), data.size());
    if (!body->watchers.empty()) {
      body->Wake(false);
      if (host.config().afterScript) host.config().afterScript();
    }
  }
}

void FetchOperation::StartMain() {
  handle = host.config().client->Fetch(requestUrl, *this, std::move(mainOptions));
}

void FetchOperation::OnEndIn() {
  if (preflighting) {
    preflighting = false;
    if (!settled) StartMain();
    return;
  }
  if (integrityBuffering) {
    integrityBuffering = false;
    if (orbBuffering) {
      orbBuffering = false;
      if (OrbBodyIsBlocked(integrityBytes)) {
        FailWith(MakeTypeError(host.context(), "Failed to fetch: the response was blocked (ORB)"), "the response was blocked (ORB)");
        Done();
        return;
      }
    } else if (!IntegrityMatches(integrity, integrityBytes)) {
      FailWith(MakeTypeError(host.context(), "Failed to fetch: the body does not match the integrity metadata"), "the body does not match the integrity metadata");
      Done();
      return;
    }
    integrityReleased = true;
    OnResponseHeadIn(integrityHead, integrityFinalUrl);
    if (body && !body->failure) body->bytes = std::move(integrityBytes);
  }
  if (body && !body->failure) {
    body->complete = true;
    qe::ReportExternalAllocation(body->bytes.size());
    body->Wake(true);
  }
  if (host.config().afterScript) host.config().afterScript();
  Done();
}

void FetchOperation::StartPreflightedHop(const url::Url& target, net::FetchOptions options) {
  requestUrl = target;
  url::Url withoutFragment = target;
  withoutFragment.fragment.reset();
  preflightUrl = url::Serialize(withoutFragment);
  const bool includeCredentials = credentials == RequestCredentials::Include;
  if (host.preflights().Allows(pageOrigin, preflightUrl, includeCredentials, method, unsafeHeaders)) {
    handle = host.config().client->Fetch(target, *this, std::move(options));
    return;
  }
  net::FetchOptions preflight;
  preflight.method = "OPTIONS";
  preflight.headers.emplace_back("Origin", taintedOrigin ? "null" : pageOrigin);
  preflight.headers.emplace_back("Access-Control-Request-Method", method);
  if (!unsafeHeaders.empty()) {
    std::string names;
    for (const std::string& name : unsafeHeaders) names += (names.empty() ? "" : ",") + name;
    preflight.headers.emplace_back("Access-Control-Request-Headers", names);
  }
  for (const auto& [name, value] : options.headers) {
    if (EqualsIgnoreCase(name, "referer")) preflight.headers.emplace_back(name, value);
  }
  preflight.redirect = net::RedirectMode::Error;
  preflight.cache = net::CacheMode::NoStore;
  preflight.useCookies = false;
  preflight.timeout = options.timeout;
  preflight.initiator = options.initiator;
  preflight.tls = options.tls;
  mainOptions = std::move(options);
  preflighting = true;
  handle = host.config().client->Fetch(target, *this, std::move(preflight));
}

void FetchOperation::OnErrorIn(std::string_view message) {
  if (redirectForPreflight && !settled) {
    redirectForPreflight = false;
    StartPreflightedHop(redirectTarget, std::move(redirectOptions));
    return;
  }
  if (!abortedByScript && !finished) {
    const std::string text = std::string("Failed to fetch (") + (preflighting ? "CORS preflight: " : "") + std::string(message) + ")";
    FailWith(MakeTypeError(host.context(), text), text);
  }
  Done();
}

bool FetchOperation::OnRedirect(const net::HttpResponseHead& redirect, const url::Url& next, std::vector<std::pair<std::string, std::string>>& headers) {
  bool follow = false;
  host.InRealm([&] { follow = OnRedirectIn(redirect, next, headers); });
  return follow;
}
void FetchOperation::OnResponseHead(const net::HttpResponseHead& head, const url::Url& finalUrl) {
  host.InRealm([&] { OnResponseHeadIn(head, finalUrl); });
}
void FetchOperation::OnBody(std::span<const uint8_t> data) {
  host.InRealm([&] { OnBodyIn(data); });
}
void FetchOperation::OnEnd() {
  host.InRealm([&] { OnEndIn(); });
}
void FetchOperation::OnError(std::string_view message) {
  host.InRealm([&] { OnErrorIn(message); });
}

void CancelOperation(FetchOperation& operation) {
  if (operation.finished) return;
  operation.abortedByScript = true;  // the host is going away: nobody is left to tell
  operation.handle.Cancel();
  if (operation.body && !operation.body->complete) {
    operation.body->failure = "the page went away";
    operation.body->waiting.clear();
    operation.body->watchers.clear();
  }
  operation.Done();
}

namespace {

// The Range header of a request for a blob: "bytes=" and either or both ends (Fetch Standard, "parse a single range header value"). False where it is none.
bool ParseSingleRange(const std::string& text, std::optional<uint64_t>& start, std::optional<uint64_t>& end) {
  size_t i = 0;
  const auto space = [&] { while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i; };
  const auto number = [&](std::optional<uint64_t>& out) {
    if (i >= text.size() || text[i] < '0' || text[i] > '9') return;
    uint64_t value = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
      const uint64_t digit = static_cast<uint64_t>(text[i] - '0');
      value = value > (UINT64_MAX - digit) / 10 ? UINT64_MAX : value * 10 + digit;
      ++i;
    }
    out = value;
  };
  if (text.compare(0, 5, "bytes") != 0) return false;
  i = 5;
  space();
  if (i >= text.size() || text[i] != '=') return false;
  ++i;
  space();
  number(start);
  space();
  if (i >= text.size() || text[i] != '-') return false;
  ++i;
  space();
  number(end);
  space();
  if (i != text.size() || (!start && !end)) return false;
  return !(start && end && *start > *end);
}

}  // namespace

bool SliceBlobRange(const std::string& range, std::string& bytes, std::string& contentRange) {
  std::optional<uint64_t> start, end;
  if (!ParseSingleRange(range, start, end)) return false;
  const uint64_t size = bytes.size();
  uint64_t from, to;
  if (!start) {
    from = *end >= size ? 0 : size - *end;
    to = size ? size - 1 : 0;
  } else {
    from = *start;
    to = !end || *end >= size ? (size ? size - 1 : 0) : *end;
  }
  if (from >= size) return false;
  contentRange = "bytes " + std::to_string(from) + "-" + std::to_string(to) + "/" + std::to_string(size);
  bytes = bytes.substr(from, to - from + 1);
  return true;
}

namespace {

int HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string PercentDecode(std::string_view text) {
  std::string out;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size() + 0 && HexDigit(text[i + 1]) >= 0 && HexDigit(text[i + 2]) >= 0) {
      out += static_cast<char>(HexDigit(text[i + 1]) * 16 + HexDigit(text[i + 2]));
      i += 2;
    } else {
      out += text[i];
    }
  }
  return out;
}

// Forgiving base64 (https://infra.spec.whatwg.org/#forgiving-base64-decode); false where it cannot be decoded.
bool DecodeBase64(std::string_view text, std::string& out) {
  std::string clean;
  for (char c : text) if (c != ' ' && c != '\t' && c != '\n' && c != '\f' && c != '\r') clean += c;
  if (clean.size() % 4 == 0) {
    if (clean.size() >= 2 && clean.compare(clean.size() - 2, 2, "==") == 0) clean.resize(clean.size() - 2);
    else if (!clean.empty() && clean.back() == '=') clean.pop_back();
  }
  if (clean.size() % 4 == 1) return false;
  uint32_t bits = 0;
  int count = 0;
  for (char c : clean) {
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+') v = 62;
    else if (c == '/') v = 63;
    else return false;
    bits = (bits << 6) | static_cast<uint32_t>(v);
    count += 6;
    if (count >= 8) {
      count -= 8;
      out += static_cast<char>((bits >> count) & 0xFF);
    }
  }
  return true;
}

// The bytes of a data: address, and its type (https://fetch.spec.whatwg.org/#data-url-processor); false where it is not one that can be read.
bool DecodeDataUrl(const std::string& address, std::string& bytes, std::string& type) {
  if (address.rfind("data:", 0) != 0) return false;
  std::string rest = address.substr(5);
  if (const size_t hash = rest.find('#'); hash != std::string::npos) rest.resize(hash);
  const size_t comma = rest.find(',');
  if (comma == std::string::npos) return false;
  std::string mime = rest.substr(0, comma);
  const std::string payload = PercentDecode(std::string_view(rest).substr(comma + 1));
  // (white space at the ends of the type goes; a trailing ;base64 is the encoding, with only spaces before the word)
  const auto isWhitespace = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; };
  while (!mime.empty() && isWhitespace(mime.back())) mime.pop_back();
  size_t lead = 0;
  while (lead < mime.size() && isWhitespace(mime[lead])) ++lead;
  mime.erase(0, lead);
  bool base64 = false;
  if (mime.size() >= 6) {
    std::string tail = mime.substr(mime.size() - 6);
    for (char& c : tail) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (tail == "base64") {
      size_t end = mime.size() - 6;
      while (end > 0 && mime[end - 1] == ' ') --end;
      if (end > 0 && mime[end - 1] == ';') {
        base64 = true;
        mime.resize(end - 1);
      }
    }
  }
  if (mime.starts_with(";")) mime = "text/plain" + mime;
  if (const auto record = ParseMimeType(mime)) type = SerializeMimeType(*record);
  else type = "text/plain;charset=US-ASCII";
  bytes.clear();
  if (base64) return DecodeBase64(payload, bytes);
  bytes = payload;
  return true;
}

Value Fetch(Context& ctx, Value, qe::Args args, Value newTarget) {
  if (!qe::IsUndefined(newTarget)) {
    qe::ThrowTypeError(ctx, "fetch is not a constructor");
    return qe::Undefined();
  }
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
  // A blob and a data address are not the network's: what they name is here already.
  if (request->url.scheme == "blob" || request->url.scheme == "data") {
    if (request->url.scheme == "blob" && request->method != "GET") return reject(MakeTypeError(ctx, "Failed to fetch: a blob address answers GET only"));
    url::Url withoutFragment = request->url;
    withoutFragment.fragment.reset();
    const std::string address = url::Serialize(withoutFragment);
    std::string bytes, type;
    bool found = false;
    if (request->url.scheme == "blob") {
      if (const auto entry = web::LookupBlobUrl(address)) {
        bytes = *entry->data;
        type = entry->type;
        found = true;
      }
    } else {
      found = DecodeDataUrl(url::Serialize(request->url), bytes, type);
    }
    if (!found) return reject(MakeTypeError(ctx, "Failed to fetch: the " + request->url.scheme + " address cannot be read"));
    JsResponse* r = NewResponse(ctx);
    r->type = ResponseType::Basic;
    r->status = 200;
    r->statusText = "OK";
    r->url = address;
    std::vector<std::pair<std::string, std::string>> list;
    std::string contentRange;
    // A blob answers a request for a range of it with that part.
    if (request->url.scheme == "blob") {
      if (const auto range = request->headers->headers.Get("Range")) {
        if (!SliceBlobRange(*range, bytes, contentRange)) return reject(MakeTypeError(ctx, "Failed to fetch: the Range of a blob request is not valid"));
        r->status = 206;
        r->statusText = "Partial Content";
      }
    }
    if (!type.empty() || request->url.scheme == "blob") list.emplace_back("Content-Type", type);
    if (request->url.scheme == "blob") list.emplace_back("Content-Length", std::to_string(bytes.size()));
    if (!contentRange.empty()) list.emplace_back("Content-Range", contentRange);
    r->headers = NewResponseHeaders(ctx, list);
    auto buffer = std::make_shared<BodyBuffer>();
    buffer->complete = true;
    buffer->bytes = std::move(bytes);
    r->body = buffer;
    r->NoteWrite(qe::FromObject(r->headers));
    Value value = qe::FromObject(r);
    qe::Call(ctx, capability.resolve, qe::Undefined(), qe::Args(&value, 1));
    return capability.promise;
  }
  if (request->url.scheme != "http" && request->url.scheme != "https") return reject(MakeTypeError(ctx, "Failed to fetch: unsupported scheme " + request->url.scheme));

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
  if (op->crossOrigin && request->mode == RequestMode::Cors) op->corsTainted = true;
  // A no-cors request to another origin has its redirects followed or it is an error.
  if (op->crossOrigin && request->mode == RequestMode::NoCors && request->redirect != net::RedirectMode::Follow) return reject(MakeTypeError(ctx, "Failed to fetch: a no-cors request to another origin must follow redirects"));

  net::FetchOptions options;
  options.method = request->method;
  options.headers = request->headers->headers.list();
  options.cache = request->cache;
  options.redirect = request->redirect;
  options.timeout = std::chrono::hours(24);
  options.useCookies = request->credentials == RequestCredentials::Include || (request->credentials == RequestCredentials::SameOrigin && !op->crossOrigin);
  options.initiator = page;
  options.tls = host->config().tls;
  op->secFetchPage = page;
  op->secFetchSite = SecFetchSiteOf(page, request->url);
  op->secFetchMode = request->mode == RequestMode::Cors ? "cors" : request->mode == RequestMode::NoCors ? "no-cors" : "same-origin";
  SetSecFetchHeaders(options.headers, request->url, op->secFetchSite, op->secFetchMode);
  op->referrerPolicy = request->referrerPolicy;
  op->sendsReferrer = !request->referrer.empty();
  op->referrerSource = page;
  if (const auto referer = RefererFor(request->referrer.empty() ? "no-referrer" : request->referrerPolicy, page, request->url);
      referer && !request->referrer.empty()) {
    options.headers.emplace_back("Referer", *referer);
    if (const auto parsed = url::Parse(*referer)) op->referrerSource = *parsed;
  } else {
    op->sendsReferrer = false;
  }
  // Origin goes with a cross-origin request, and with any request that is not a GET or a HEAD.
  if (op->corsTainted || (request->method != "GET" && request->method != "HEAD")) {
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

  op->method = request->method;
  op->integrity = request->integrity;
  host->Adopt(std::move(operation));

  // From here on the request needs its bytes, which a stream gives only once it has ended.
  auto launch = [host, op, request, page = page, options = std::move(options)](std::shared_ptr<const std::string> bodyBytes) mutable {
    options.body = std::move(bodyBytes);

    // A cors request that is not simple asks the server first, unless it has been told lately that it may.
    op->mainOptions = options;
    op->requestHeaders = request->headers->headers;
    op->nonSimple = request->mode == RequestMode::Cors && net::NeedsPreflight(request->method, request->headers->headers);
    if (op->corsTainted && net::NeedsPreflight(request->method, request->headers->headers)) {
      op->preflightNeeded = true;
      op->unsafeHeaders = net::CorsUnsafeRequestHeaderNames(request->headers->headers);
      url::Url withoutFragment = request->url;
      withoutFragment.fragment.reset();
      op->preflightUrl = url::Serialize(withoutFragment);
      const bool includeCredentials = request->credentials == RequestCredentials::Include;
      if (!host->preflights().Allows(op->pageOrigin, op->preflightUrl, includeCredentials, op->method, op->unsafeHeaders)) {
        net::FetchOptions preflight;
        preflight.method = "OPTIONS";
        preflight.headers.emplace_back("Origin", op->pageOrigin);
        preflight.headers.emplace_back("Access-Control-Request-Method", op->method);
        if (!op->unsafeHeaders.empty()) {
          std::string names;
          for (const std::string& name : op->unsafeHeaders) names += (names.empty() ? "" : ",") + name;
          preflight.headers.emplace_back("Access-Control-Request-Headers", names);
        }
        for (const auto& [name, value] : options.headers) {
          if (EqualsIgnoreCase(name, "referer")) preflight.headers.emplace_back(name, value);
        }
        preflight.redirect = net::RedirectMode::Error;  // a preflight that is redirected has failed
        preflight.cache = net::CacheMode::NoStore;
        preflight.useCookies = false;                   // it never carries credentials
        preflight.timeout = options.timeout;
        preflight.initiator = page;
        preflight.tls = host->config().tls;
        op->mainOptions = std::move(options);
        op->preflighting = true;
        op->handle = host->config().client->Fetch(request->url, *op, std::move(preflight));
        return;
      }
    }
    op->handle = host->config().client->Fetch(request->url, *op, std::move(options));
  };

  if (request->body && request->body->fromStream) {
    auto hold = std::make_shared<qe::Persistent>(ctx, qe::FromObject(request));
    auto run = std::make_shared<decltype(launch)>(std::move(launch));
    const bool started = ReadBodyStream(
        ctx, request,
        [host, op, hold, run](std::string bytes) {
          if (!host->Owns(op) || op->finished) return;
          (*run)(std::make_shared<const std::string>(std::move(bytes)));
        },
        [host, op, hold](const Value& reason) {
          if (!host->Owns(op) || op->finished) return;
          op->FailWith(reason, "the request body failed");
          op->Done();
        });
    if (!started) {
      op->abortedByScript = true;
      op->Done();
      return rejectWithPending();
    }
    return capability.promise;
  }
  std::shared_ptr<const std::string> bodyBytes;
  if (request->body) {
    // The request's bytes go as they are, shared with it, and it is read now.
    bodyBytes = std::shared_ptr<const std::string>(request->body, &request->body->bytes);
    request->used = true;
  }
  launch(std::move(bodyBytes));
  return capability.promise;
}

}  // namespace

void DefineFetchFunction(Context& ctx) { qe::DefineGlobalFunction(ctx, "fetch", Fetch, 1); }

}  // namespace solar::web
