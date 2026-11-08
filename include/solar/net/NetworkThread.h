/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <functional>
#include <memory>

#include "solar/net/HttpClient.h"
#include "solar/net/Loop.h"

namespace solar::net {

// The network service on a thread of its own. The connections, the lookups, the cookies, the cache and the rules of fetching are the thread's (an HttpClient with a loop of its own);
// the pages are on another thread, whose loop is `pageLoop`, and are told what happens by tasks put on it: every call a handler hears is made from that loop, by whoever runs it, as
// with the HttpClient, and no state is shared between the two threads but the messages (a request in one direction, an answer in the other: what they say is copied).
//
// A fetch keeps `pageLoop` from running out of work until it has ended (Loop::BeginExternalWork), so that a loop that is run to wait for one waits for the thread.
// Its handler may leave the redirects and the stalls to the page (DefersRedirects and DefersRestarts), and the ones that do not are asked on the page's thread, in the middle of the
// answers, as before; for a stall that is allowed what had come before it is dropped, so that the handler sees the response again from its start (as FetchHandler::OnRestart has it).
class ThreadedNetworkService final : public NetworkService {
 public:
  ThreadedNetworkService(Loop& pageLoop, HttpClientOptions options = {});
  // For options that are made with the loop of the thread (a resolver is made on one): `makeOptions` is called on the service's thread with its loop.
  ThreadedNetworkService(Loop& pageLoop, std::function<HttpClientOptions(Loop&)> makeOptions);
  // Stops the thread. What is in flight is let go without being told of.
  ~ThreadedNetworkService() override;

  ThreadedNetworkService(const ThreadedNetworkService&) = delete;
  ThreadedNetworkService& operator=(const ThreadedNetworkService&) = delete;

  FetchHandle Fetch(const url::Url& url, FetchHandler& handler, FetchOptions options = {}) override;
  FetchHandle StartFetch(PageRequest request, PageFetchHandler& handler) override;
  void Cancel(uint64_t id) override;
  void FollowRedirect(uint64_t id, bool follow, std::vector<std::pair<std::string, std::string>> headers = {}) override;
  void AnswerRestart(uint64_t id, bool allow) override;
  void CloseIdleConnections() override;
  WebSocketHandle ConnectWebSocket(const url::Url& url, WebSocketHandler& handler, WebSocketOptions options = {}) override;
  void SendWebSocket(uint64_t id, std::string data, bool binary) override;
  void CloseWebSocket(uint64_t id, uint16_t code, std::string reason) override;
  void AbortWebSocket(uint64_t id) override;

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace solar::net
