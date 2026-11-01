/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <memory>
#include <string>

#include "quanta/Embed.h"
#include "solar/net/HttpClient.h"
#include "solar/url/Url.h"
#include "solar/web/FetchBindingsInternal.h"
#include "solar/web/FetchHost.h"

namespace solar::web {

// One fetch() in flight: the network's handler for it and the promise it settles. It lives in its host
// until it has ended, and holds what script made (the promise's functions, the signal) in Persistents
// so that none of it is collected while the network still has something to say.
struct FetchOperation : net::FetchHandler {
  FetchOperation(FetchHost& host) : host(host) {}

  FetchHost& host;
  Quanta::Embed::Persistent resolve;
  Quanta::Embed::Persistent reject;
  Quanta::Embed::Persistent signalValue;
  JsAbortSignal* signal = nullptr;
  std::shared_ptr<Listener> abortListener;
  net::FetchHandle handle;

  // What the request is, as the checks on its response need it.
  url::Url requestUrl;
  url::Url currentUrl;  // where the latest hop of it went
  std::string pageOrigin;
  RequestMode mode = RequestMode::Cors;
  RequestCredentials credentials = RequestCredentials::SameOrigin;
  net::RedirectMode redirectMode = net::RedirectMode::Follow;
  bool crossOrigin = false;       // the request is to another origin than the page's
  bool corsTainted = false;       // a response to it is a cors response: some hop went cross-origin in cors mode
  bool taintedOrigin = false;     // a redirect crossed origins twice over: Origin is null
  int redirects = 0;

  // A cors request that has to ask first: the request waits here while the preflight is out.
  bool preflighting = false;
  bool preflightNeeded = false;  // it asked, or an earlier preflight allowed it: a redirect cannot be followed
  net::FetchOptions mainOptions;
  std::string method;
  // What the Referer of a request is made from, so that a redirect (and the Referrer-Policy it brings) can make it again.
  std::string referrerPolicy;
  bool sendsReferrer = false;
  url::Url referrerSource;
  std::vector<std::string> unsafeHeaders;
  std::string preflightUrl;
  void StartMain();
  // The integrity metadata of the request: its response is held back until the body is whole and has been checked.
  std::string integrity;
  bool integrityBuffering = false, integrityReleased = false;
  bool orbBuffering = false;  // the body is held for the check of Opaque Response Blocking
  net::HttpResponseHead integrityHead;
  url::Url integrityFinalUrl;
  std::string integrityBytes;
  // The request is not a simple one: wherever it goes as a cors request it asks first, a redirect's target included.
  bool nonSimple = false;
  int secFetchSite = 0;  // 0 same-origin, 1 same-site, 2 cross-site: the worst of the hops so far
  url::Url secFetchPage;
  std::string secFetchMode;
  net::FetchHeaders requestHeaders;  // (the request's own, as the preflight of a redirect's target names them)
  // A redirect whose target has to be asked first: the hop ends there, and the next one starts with the preflight.
  bool redirectForPreflight = false;
  url::Url redirectTarget;
  net::FetchOptions redirectOptions;
  void StartPreflightedHop(const url::Url& target, net::FetchOptions options);

  // What has happened.
  bool settled = false;   // the promise has been resolved or rejected
  bool finished = false;  // the network has had its last word
  bool abortedByScript = false;
  std::shared_ptr<BodyBuffer> body;
  Quanta::Embed::Persistent responseValue;
  JsResponse* response = nullptr;

  // What the network tells arrives outside any script, where the realm that natives answer for is the last one entered, not this one's: each of these goes
  // into the realm of the host before it does anything with classes (a Response made here must be of this realm's).
  bool OnRedirect(const net::HttpResponseHead& redirect, const url::Url& next, std::vector<std::pair<std::string, std::string>>& headers) override;
  void OnResponseHead(const net::HttpResponseHead& head, const url::Url& finalUrl) override;
  void OnBody(std::span<const uint8_t> data) override;
  void OnEnd() override;
  void OnError(std::string_view message) override;
  bool OnRedirectIn(const net::HttpResponseHead& redirect, const url::Url& next, std::vector<std::pair<std::string, std::string>>& headers);
  void OnResponseHeadIn(const net::HttpResponseHead& head, const url::Url& finalUrl);
  void OnBodyIn(std::span<const uint8_t> data);
  void OnEndIn();
  void OnErrorIn(std::string_view message);

  // Ends the promise (if it is not) and the body (if it is not) with a failure.
  void FailWith(const Quanta::Value& reason, const std::string& message);
  void Done();
};

}  // namespace solar::web
