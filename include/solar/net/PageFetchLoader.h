/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "solar/net/Cors.h"
#include "solar/net/FetchHeaders.h"
#include "solar/net/HttpClient.h"
#include "solar/net/PageFetch.h"

namespace solar::net {

// One fetch of a page, as the Fetch Standard has it, run on top of the HTTP client's hops: the rules of fetching are here (see PageFetch.h). It is the handler of every hop of its own
// request, a preflight's included, and tells the page's handler what the page may be told.
class PageFetchLoader final : public FetchHandler {
 public:
  // `finished` is called, from the loop and not from inside a call of this, once it has said its last word (it is then let go).
  PageFetchLoader(HttpClient& client, PreflightCache& preflights, PageRequest request, PageFetchHandler& handler, std::function<void()> finished);

  // Begins it. Nothing is reported from inside this call.
  void Start();
  // The page ends it: the handler hears of it before this returns.
  void Cancel();

  bool OnRedirect(const HttpResponseHead& redirect, const url::Url& next, std::vector<std::pair<std::string, std::string>>& headers) override;
  void OnResponseHead(const HttpResponseHead& head, const url::Url& finalUrl) override;
  void OnBody(std::span<const uint8_t> data) override;
  void OnEnd() override;
  void OnError(std::string_view message) override;

 private:
  void StartMain();
  void StartPreflightedHop(const url::Url& target, FetchOptions options);
  void Respond(const HttpResponseHead& head, const url::Url& finalUrl);
  // Ends it with an error the page hears, and lets the hop that is out go.
  void Fail(const std::string& message);
  void Done();

  HttpClient& client_;
  PreflightCache& preflights_;
  PageRequest request_;
  PageFetchHandler& handler_;
  std::function<void()> finished_;
  FetchHandle handle_;

  std::string pageOrigin_;
  url::Url currentUrl_;
  RequestMode mode_;
  RequestCredentials credentials_;
  RedirectMode redirectMode_;
  bool crossOrigin_ = false;
  bool corsTainted_ = false;
  bool taintedOrigin_ = false;
  int redirects_ = 0;
  bool preflighting_ = false;
  FetchOptions mainOptions_;
  std::string method_;
  std::string referrerPolicy_;
  bool sendsReferrer_ = false;
  url::Url referrerSource_;
  std::vector<std::string> unsafeHeaders_;
  std::string preflightUrl_;
  bool nonSimple_ = false;
  int secFetchSite_ = 0;
  url::Url secFetchPage_;
  std::string secFetchMode_;
  FetchHeaders requestHeaders_;
  bool redirectForPreflight_ = false;
  url::Url redirectTarget_;
  FetchOptions redirectOptions_;
  // Opaque Response Blocking of a response whose head could not decide: its body is held until it is whole.
  bool orbBuffering_ = false, orbReleased_ = false;
  HttpResponseHead heldHead_;
  url::Url heldFinalUrl_;
  std::string heldBytes_;

  bool responded_ = false;   // the page has been told of a response
  bool hasBody_ = false;     // what comes of the body goes to the page
  bool finished = false;     // it has had its last word
};

}  // namespace solar::net
