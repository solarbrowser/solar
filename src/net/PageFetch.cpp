/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/net/PageFetchLoader.h"

#include <algorithm>

#include "solar/net/Orb.h"
#include "solar/url/Origin.h"
#include "solar/url/Parser.h"
#include "solar/url/Serializer.h"

namespace solar::net {

namespace {

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return (x >= 'A' && x <= 'Z' ? x + 0x20 : x) == (y >= 'A' && y <= 'Z' ? y + 0x20 : y);
         });
}

bool IsRedirectStatus(int status) { return status == 301 || status == 302 || status == 303 || status == 307 || status == 308; }

// What a Referer header carries under the request's referrer policy (Referrer Policy, "determine the request's referrer"), or nothing.
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

PageFetchLoader::PageFetchLoader(HttpClient& client, PreflightCache& preflights, PageRequest request, PageFetchHandler& handler, std::function<void()> finished)
    : client_(client), preflights_(preflights), request_(std::move(request)), handler_(handler), finished_(std::move(finished)), mode_(request_.mode), credentials_(request_.credentials),
      redirectMode_(static_cast<RedirectMode>(request_.redirect)), requestHeaders_(HeadersGuard::None) {
  for (const auto& [name, value] : request_.headers) requestHeaders_.AppendUnchecked(name, value);
}

void PageFetchLoader::Start() {
  if (finished) return;  // (cancelled before it began)
  const url::Url& page = request_.pageUrl;
  currentUrl_ = request_.url;
  pageOrigin_ = url::SerializeOrigin(page);
  crossOrigin_ = url::SerializeOrigin(request_.url) != pageOrigin_;
  method_ = request_.method;

  // (These are told from the loop, not from inside the call that began the fetch.)
  const auto refuse = [this](const char* message) {
    const std::string text = message;
    client_.loop().Post([this, text] { Fail(text); });
  };
  if (crossOrigin_ && mode_ == RequestMode::SameOrigin) return refuse("Failed to fetch: a same-origin request to another origin");
  if (crossOrigin_ && mode_ == RequestMode::Cors) corsTainted_ = true;
  // A no-cors request to another origin has its redirects followed or it is an error.
  if (crossOrigin_ && mode_ == RequestMode::NoCors && redirectMode_ != RedirectMode::Follow) return refuse("Failed to fetch: a no-cors request to another origin must follow redirects");

  FetchOptions options;
  options.method = request_.method;
  options.headers = request_.headers;
  options.cache = request_.cache;
  options.redirect = redirectMode_;
  options.timeout = request_.timeout;
  options.useCookies = credentials_ == RequestCredentials::Include || (credentials_ == RequestCredentials::SameOrigin && !crossOrigin_);
  options.initiator = page;
  options.tls = request_.tls;
  options.body = request_.body;
  secFetchPage_ = page;
  secFetchSite_ = SecFetchSiteOf(page, request_.url);
  secFetchMode_ = mode_ == RequestMode::Cors ? "cors" : mode_ == RequestMode::NoCors ? "no-cors" : "same-origin";
  SetSecFetchHeaders(options.headers, request_.url, secFetchSite_, secFetchMode_);
  referrerPolicy_ = request_.referrerPolicy;
  sendsReferrer_ = request_.sendsReferrer;
  referrerSource_ = page;
  if (const auto referer = RefererFor(request_.sendsReferrer ? request_.referrerPolicy : "no-referrer", page, request_.url); referer && request_.sendsReferrer) {
    options.headers.emplace_back("Referer", *referer);
    if (const auto parsed = url::Parse(*referer)) referrerSource_ = *parsed;
  } else {
    sendsReferrer_ = false;
  }
  // Origin goes with a cross-origin request, and with any request that is not a GET or a HEAD.
  if (corsTainted_ || (request_.method != "GET" && request_.method != "HEAD")) options.headers.emplace_back("Origin", pageOrigin_);

  // A cors request that is not simple asks the server first, unless it has been told lately that it may.
  mainOptions_ = options;
  nonSimple_ = mode_ == RequestMode::Cors && NeedsPreflight(request_.method, requestHeaders_);
  if (corsTainted_ && NeedsPreflight(request_.method, requestHeaders_)) {
    unsafeHeaders_ = CorsUnsafeRequestHeaderNames(requestHeaders_);
    url::Url withoutFragment = request_.url;
    withoutFragment.fragment.reset();
    preflightUrl_ = url::Serialize(withoutFragment);
    const bool includeCredentials = credentials_ == RequestCredentials::Include;
    if (!preflights_.Allows(pageOrigin_, preflightUrl_, includeCredentials, method_, unsafeHeaders_)) {
      FetchOptions preflight;
      preflight.method = "OPTIONS";
      preflight.headers.emplace_back("Origin", pageOrigin_);
      preflight.headers.emplace_back("Access-Control-Request-Method", method_);
      if (!unsafeHeaders_.empty()) {
        std::string names;
        for (const std::string& name : unsafeHeaders_) names += (names.empty() ? "" : ",") + name;
        preflight.headers.emplace_back("Access-Control-Request-Headers", names);
      }
      for (const auto& [name, value] : options.headers) {
        if (EqualsIgnoreCase(name, "referer")) preflight.headers.emplace_back(name, value);
      }
      preflight.redirect = RedirectMode::Error;  // a preflight that is redirected has failed
      preflight.cache = CacheMode::NoStore;
      preflight.useCookies = false;              // it never carries credentials
      preflight.timeout = options.timeout;
      preflight.initiator = page;
      preflight.tls = request_.tls;
      mainOptions_ = std::move(options);
      preflighting_ = true;
      handle_ = client_.Fetch(request_.url, *this, std::move(preflight));
      return;
    }
  }
  handle_ = client_.Fetch(request_.url, *this, std::move(options));
}

void PageFetchLoader::StartMain() { handle_ = client_.Fetch(request_.url, *this, std::move(mainOptions_)); }

void PageFetchLoader::StartPreflightedHop(const url::Url& target, FetchOptions options) {
  request_.url = target;
  url::Url withoutFragment = target;
  withoutFragment.fragment.reset();
  preflightUrl_ = url::Serialize(withoutFragment);
  const bool includeCredentials = credentials_ == RequestCredentials::Include;
  if (preflights_.Allows(pageOrigin_, preflightUrl_, includeCredentials, method_, unsafeHeaders_)) {
    handle_ = client_.Fetch(target, *this, std::move(options));
    return;
  }
  FetchOptions preflight;
  preflight.method = "OPTIONS";
  preflight.headers.emplace_back("Origin", taintedOrigin_ ? "null" : pageOrigin_);
  preflight.headers.emplace_back("Access-Control-Request-Method", method_);
  if (!unsafeHeaders_.empty()) {
    std::string names;
    for (const std::string& name : unsafeHeaders_) names += (names.empty() ? "" : ",") + name;
    preflight.headers.emplace_back("Access-Control-Request-Headers", names);
  }
  for (const auto& [name, value] : options.headers) {
    if (EqualsIgnoreCase(name, "referer")) preflight.headers.emplace_back(name, value);
  }
  preflight.redirect = RedirectMode::Error;
  preflight.cache = CacheMode::NoStore;
  preflight.useCookies = false;
  preflight.timeout = options.timeout;
  preflight.initiator = options.initiator;
  preflight.tls = options.tls;
  mainOptions_ = std::move(options);
  preflighting_ = true;
  handle_ = client_.Fetch(target, *this, std::move(preflight));
}

void PageFetchLoader::Cancel() {
  if (finished) return;
  finished = true;
  handle_.Cancel();  // (its own end is not told: this has ended)
  handler_.OnError("aborted");
  if (finished_) client_.loop().Post(std::move(finished_));
}

void PageFetchLoader::Done() {
  if (finished) return;
  finished = true;
  if (finished_) client_.loop().Post(std::move(finished_));
}

void PageFetchLoader::Fail(const std::string& message) {
  if (finished) return;
  finished = true;
  handle_.Cancel();
  handler_.OnError(message);
  if (finished_) client_.loop().Post(std::move(finished_));
}

bool PageFetchLoader::OnRedirect(const HttpResponseHead& redirect, const url::Url& next, std::vector<std::pair<std::string, std::string>>& headers) {
  if (finished) return false;
  ++redirects_;
  const bool nextCrossOrigin = url::SerializeOrigin(next) != pageOrigin_;
  // A cors request that has gone cross-origin must have its redirects allowed by each server.
  if (mode_ == RequestMode::Cors && url::SerializeOrigin(currentUrl_) != pageOrigin_ &&
      !CorsAllowsResponse(redirect, taintedOrigin_ ? "null" : pageOrigin_, credentials_ == RequestCredentials::Include)) {
    Fail("Failed to fetch: a redirect was not allowed by CORS");
    return false;
  }
  if (next.scheme != "http" && next.scheme != "https") return false;
  if (mode_ == RequestMode::Cors && (corsTainted_ || nextCrossOrigin) && next.IncludesCredentials()) {
    Fail("Failed to fetch: a redirect to an address with credentials");
    return false;
  }
  // Having gone from one origin to another, and then on to a third, the request's origin is unknown.
  if (url::SerializeOrigin(next) != url::SerializeOrigin(currentUrl_) && url::SerializeOrigin(currentUrl_) != pageOrigin_) taintedOrigin_ = true;
  if (mode_ == RequestMode::Cors && nextCrossOrigin) corsTainted_ = true;
  currentUrl_ = next;

  // The Origin header follows: null once tainted, and dropped again on a same-origin GET.
  std::erase_if(headers, [](const auto& header) { return EqualsIgnoreCase(header.first, "origin"); });
  // (301 and 302 turn a POST into a GET, 303 turns anything but a HEAD into one; 307 and 308 keep the method.)
  std::string next_method = method_;
  if (((redirect.status == 301 || redirect.status == 302) && method_ == "POST") || (redirect.status == 303 && method_ != "GET" && method_ != "HEAD")) next_method = "GET";
  secFetchSite_ = std::max(secFetchSite_, SecFetchSiteOf(secFetchPage_, next));
  SetSecFetchHeaders(headers, next, secFetchSite_, secFetchMode_);
  // The Referrer-Policy of the redirect response is the policy from here on (the last of its tokens that is one), and the Referer is made again.
  if (const auto policyHeader = redirect.Header("referrer-policy")) {
    std::string value(*policyHeader), token;
    value.push_back(',');
    for (char c : value) {
      if (c != ',') {
        if (c != ' ' && c != '\t') token.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        continue;
      }
      for (const char* known : {"no-referrer", "no-referrer-when-downgrade", "same-origin", "origin", "strict-origin", "origin-when-cross-origin", "strict-origin-when-cross-origin", "unsafe-url"}) {
        if (token == known) referrerPolicy_ = token;
      }
      token.clear();
    }
  }
  std::erase_if(headers, [](const auto& header) { return EqualsIgnoreCase(header.first, "referer"); });
  if (sendsReferrer_) {
    // (What is sent is what the request's referrer is from then on: a policy cannot give back what an earlier one took away.)
    if (const auto referer = RefererFor(referrerPolicy_, referrerSource_, next)) {
      headers.emplace_back("Referer", *referer);
      if (const auto parsed = url::Parse(*referer)) referrerSource_ = *parsed;
    } else {
      sendsReferrer_ = false;
    }
  }
  if (corsTainted_ || taintedOrigin_ || (next_method != "GET" && next_method != "HEAD")) headers.emplace_back("Origin", taintedOrigin_ ? "null" : pageOrigin_);
  method_ = next_method;
  if (nonSimple_ && corsTainted_ && mode_ == RequestMode::Cors) {
    // The target is asked first, as the request is: this hop ends there, and the next begins with the preflight.
    redirectForPreflight_ = true;
    redirectTarget_ = next;
    redirectOptions_ = mainOptions_;
    redirectOptions_.method = next_method;
    redirectOptions_.headers = headers;
    if (next_method == "GET" || next_method == "HEAD") redirectOptions_.body.reset();
    unsafeHeaders_ = CorsUnsafeRequestHeaderNames(requestHeaders_);
    return false;
  }
  return true;
}

void PageFetchLoader::OnResponseHead(const HttpResponseHead& head, const url::Url& finalUrl) {
  if (finished) return;
  if (preflighting_) {
    const bool includeCredentials = credentials_ == RequestCredentials::Include;
    PreflightVerdict verdict = CheckPreflightResponse(head, pageOrigin_, includeCredentials, method_, unsafeHeaders_);
    if (!verdict.ok) {
      Fail("Failed to fetch (CORS preflight failed: " + verdict.reason + ")");
      return;
    }
    preflights_.Store(pageOrigin_, preflightUrl_, includeCredentials, std::move(verdict.permission));
    return;  // the request itself goes when the preflight has ended
  }
  currentUrl_ = finalUrl;
  const bool includeCredentials = credentials_ == RequestCredentials::Include;

  if (mode_ == RequestMode::Cors && (corsTainted_ || crossOrigin_) && !CorsAllowsResponse(head, taintedOrigin_ ? "null" : pageOrigin_, includeCredentials)) {
    Fail("Failed to fetch: the response was not allowed by CORS");
    return;
  }

  // A response to a no-cors request that is from another origin is checked by Opaque Response Blocking before the page has it.
  if (mode_ == RequestMode::NoCors && crossOrigin_ && !orbReleased_) {
    const auto header = [&](const char* name) { return head.Header(name).value_or(std::string_view()); };
    std::string nosniffValue(header("x-content-type-options"));
    for (char& c : nosniffValue) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const OrbVerdict verdict = OrbCheckHead(head.status, header("content-type"), nosniffValue.starts_with("nosniff"), header("content-range"));
    if (verdict == OrbVerdict::Block) {
      Fail("Failed to fetch: the response was blocked (ORB)");
      return;
    }
    if (verdict == OrbVerdict::NeedBody) {
      orbBuffering_ = true;
      heldHead_ = head;
      heldFinalUrl_ = finalUrl;
      return;
    }
  }
  Respond(head, finalUrl);
}

void PageFetchLoader::Respond(const HttpResponseHead& head, const url::Url& finalUrl) {
  const bool includeCredentials = credentials_ == RequestCredentials::Include;
  url::Url withoutFragment = finalUrl;
  withoutFragment.fragment.reset();
  PageResponse response;
  response.url = withoutFragment;
  if (redirectMode_ == RedirectMode::Manual && IsRedirectStatus(head.status)) {
    response.type = ResponseType::OpaqueRedirect;
    response.status = 0;
    response.hiddenStatus = head.status;
    if (const auto location = head.Header("location")) response.hiddenLocation = std::string(*location);
    response.hasBody = false;
  } else if (mode_ == RequestMode::NoCors && crossOrigin_) {
    response.type = ResponseType::Opaque;
    response.status = 0;
    response.hasBody = false;  // (what an opaque response holds does not go to the page)
  } else {
    response.type = corsTainted_ ? ResponseType::Cors : ResponseType::Basic;
    response.status = head.status;
    response.statusText = head.reason;
    response.redirected = redirects_ > 0;
    for (const auto& header : head.headers) {
      if (response.type == ResponseType::Cors && !IsCorsExposedHeader(header.first, head, includeCredentials)) continue;
      response.headers.push_back(header);
    }
    response.hasBody = !(head.status == 101 || head.status == 204 || head.status == 205 || head.status == 304);
  }
  hasBody_ = response.hasBody;
  responded_ = true;
  handler_.OnResponse(response);
}

void PageFetchLoader::OnBody(std::span<const uint8_t> data) {
  if (finished || preflighting_) return;
  if (orbBuffering_) {
    heldBytes_.append(reinterpret_cast<const char*>(data.data()), data.size());
    return;
  }
  if (responded_ && hasBody_) handler_.OnBody(data);
}

void PageFetchLoader::OnEnd() {
  if (finished) return;
  if (preflighting_) {
    preflighting_ = false;
    StartMain();
    return;
  }
  if (orbBuffering_) {
    orbBuffering_ = false;
    if (OrbBodyIsBlocked(heldBytes_)) {
      Fail("Failed to fetch: the response was blocked (ORB)");
      return;
    }
    orbReleased_ = true;
    Respond(heldHead_, heldFinalUrl_);
    if (hasBody_ && !heldBytes_.empty()) handler_.OnBody(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(heldBytes_.data()), heldBytes_.size()));
    heldBytes_.clear();
  }
  handler_.OnEnd();
  Done();
}

void PageFetchLoader::OnError(std::string_view message) {
  if (finished) return;
  if (redirectForPreflight_ && !responded_) {
    redirectForPreflight_ = false;
    StartPreflightedHop(redirectTarget_, std::move(redirectOptions_));
    return;
  }
  Fail(std::string("Failed to fetch (") + (preflighting_ ? "CORS preflight: " : "") + std::string(message) + ")");
}

}  // namespace solar::net
