/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <chrono>
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
// so that none of it is collected while the network still has something to say. The rules of fetching (CORS,
// redirects, the headers a browser writes, the filtering of the response) are the network service's: this is
// told what the page may be told, and makes the Response of it.
struct FetchOperation : net::PageFetchHandler {
  FetchOperation(FetchHost& host) : host(host) {}

  FetchHost& host;
  Quanta::Embed::Persistent resolve;
  Quanta::Embed::Persistent reject;
  Quanta::Embed::Persistent signalValue;
  JsAbortSignal* signal = nullptr;
  std::shared_ptr<Listener> abortListener;
  net::FetchHandle handle;

  net::RequestMode mode = net::RequestMode::Cors;
  // The integrity metadata of the request: its response is held back until the body is whole and has been checked.
  // What Resource Timing says of it once it is done (performance.getEntriesByType("resource")).
  std::string address;
  int responseStatus = 0;
  std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
  std::string integrity;
  bool integrityBuffering = false, integrityReleased = false;
  net::PageResponse integrityHead;
  std::string integrityBytes;

  // What has happened.
  bool settled = false;   // the promise has been resolved or rejected
  bool finished = false;  // the network has had its last word
  bool abortedByScript = false;
  std::shared_ptr<BodyBuffer> body;
  Quanta::Embed::Persistent responseValue;
  JsResponse* response = nullptr;

  // What the network tells arrives outside any script, where the realm that natives answer for is the last one entered, not this one's: each of these goes
  // into the realm of the host before it does anything with classes (a Response made here must be of this realm's).
  void OnResponse(const net::PageResponse& response) override;
  void OnBody(std::span<const uint8_t> data) override;
  void OnEnd() override;
  void OnError(std::string_view message) override;
  void OnResponseIn(const net::PageResponse& response);
  void OnBodyIn(std::span<const uint8_t> data);
  void OnEndIn();
  void OnErrorIn(std::string_view message);

  // Ends the promise (if it is not) and the body (if it is not) with a failure.
  void FailWith(const Quanta::Value& reason, const std::string& message);
  void Done();
};

}  // namespace solar::web
