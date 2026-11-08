/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "solar/net/HttpCache.h"
#include "solar/net/Tls.h"
#include "solar/url/Url.h"

// What a page asks of the network as a request of the Fetch Standard, and what it is given back. The rules of fetching (CORS and its preflights, the redirects a cors request may follow,
// the headers a browser writes itself: Origin, Referer, Sec-Fetch-*, the cookies a request may carry, the filtering of a response by its type, Opaque Response Blocking) are the network
// service's, not the page's: the page says what it wants, and hears what it may be told.
namespace solar::net {

enum class RequestMode { SameOrigin, NoCors, Cors, Navigate };
enum class RequestCredentials { Omit, SameOrigin, Include };
enum class ResponseType { Basic, Cors, Default, Error, Opaque, OpaqueRedirect };
// What a request is for. Of a fetch() (Fetch) the page's script is told only what a response of its type allows; of a load the page makes for itself (a picture, a style sheet, a
// script, a font, a media file) the page, which is where it is used, is given the head and the body whole, and Opaque Response Blocking and the CORS check of a cors request are what
// keep the sensitive ones out. Other is the loads of the page that are not yet named: they are asked as no-cors ones and are not blocked (to be made none).
enum class RequestDestination { Fetch, Image, Media, Script, Style, Font, Module, Other };


// What the page asks. Everything in it is data (no pointer to what only the page has).
struct PageRequest {
  url::Url url;  // http or https
  std::string method = "GET";
  std::vector<std::pair<std::string, std::string>> headers;  // the ones the page's script gave
  std::shared_ptr<const std::string> body;
  RequestDestination destination = RequestDestination::Fetch;
  RequestMode mode = RequestMode::Cors;
  RequestCredentials credentials = RequestCredentials::SameOrigin;
  int redirect = 0;  // RedirectMode as a number: Follow 0, Error 1, Manual 2
  CacheMode cache = CacheMode::Default;
  bool sendsReferrer = false;  // the page's address is the request's referrer (under the policy)
  std::string referrerPolicy;
  url::Url pageUrl;  // the page the request is for: its origin is the request's origin, its address the Referer, its site the one Sec-Fetch-Site compares
  std::chrono::milliseconds timeout{std::chrono::hours(24)};
  std::shared_ptr<TlsContext> tls;
  // A response that stalls (no byte for `stall`) is asked for again, up to twice, if the handler says it can have it from the start (OnRestart drops what it heard of it).
  bool restartable = false;
  std::chrono::milliseconds stall{8000};
};

// What the page is told of the response: the head of it as the Fetch Standard's response filtering leaves it (an opaque response has no status and no headers).
struct PageResponse {
  ResponseType type = ResponseType::Basic;
  int status = 0;
  std::string statusText;
  std::vector<std::pair<std::string, std::string>> headers;  // those the page may see
  url::Url url;                                              // the address of the last hop, without its fragment
  bool redirected = false;
  int hiddenStatus = 0;           // an opaque redirect: what it hides
  std::string hiddenLocation;
  bool hasBody = true;            // false for the statuses that have none, and for a response whose body the page is not given
};

// What the page hears of one: the response, then its body, then an end, or an error in place of the rest (the message is the one the page makes its TypeError of).
// Every call is made from the loop the fetch was asked on, never from inside the call that asked.
class PageFetchHandler {
 public:
  virtual ~PageFetchHandler() = default;
  virtual void OnResponse(const PageResponse& response) = 0;
  virtual void OnBody(std::span<const uint8_t> data) = 0;
  virtual void OnEnd() = 0;
  virtual void OnError(std::string_view message) = 0;
  // The response is asked for again from the start (only when the request said it was restartable): what was heard of it is dropped, and the response, the body and the end come again.
  virtual void OnRestart() {}
};

}  // namespace solar::net
