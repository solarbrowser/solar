/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <functional>
#include <memory>
#include <string>

#include "solar/net/HttpClient.h"
#include "solar/net/Ipc.h"
#include "solar/net/Loop.h"
#include "solar/net/PageFetch.h"

namespace solar::net {

// The network service as a page (a renderer) has it when the network is in another process: what the page asks is sent as messages (see Ipc.h) and what comes back is told from `pageLoop`,
// as with the other services. It is all a renderer has of the network: a descriptor of a connection to the service, and nothing else.
class RemoteNetworkService final : public NetworkService {
 public:
  // Takes the descriptor of the connection. A thread of its own reads what the service says and puts it on `pageLoop`, which the caller runs.
  RemoteNetworkService(Loop& pageLoop, int fd);
  // Ends the connection. What is in flight is let go without being told of.
  ~RemoteNetworkService() override;

  RemoteNetworkService(const RemoteNetworkService&) = delete;
  RemoteNetworkService& operator=(const RemoteNetworkService&) = delete;

  FetchHandle StartFetch(PageRequest request, PageFetchHandler& handler) override;
  void Cancel(uint64_t id) override;
  WebSocketHandle ConnectWebSocket(const url::Url& url, WebSocketHandler& handler, WebSocketOptions options = {}) override;
  void SendWebSocket(uint64_t id, std::string data, bool binary) override;
  void CloseWebSocket(uint64_t id, uint16_t code, std::string reason) override;
  void AbortWebSocket(uint64_t id) override;

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

// The service's end of the connection of a page: it reads what the page asks, checks it, has `network` do it, and says what happens. The page is one that is tied to an origin (a
// renderer is locked to the site it was made for): what it asks for another origin's account is refused. Its handlers hear from `hostLoop`, which the caller runs.
// Asked for each request a page makes, before it is fetched: true refuses it (the content blocker). Set once by the browser process; none by default. Called on the network host's thread.
using RequestBlocker = std::function<bool(const PageRequest&)>;
void SetRequestBlocker(RequestBlocker blocker);

class NetworkServiceHost {
 public:
  // Takes the descriptor of the connection. `lockedOrigin` is the origin the page is allowed to ask for (serialized); it can be changed by SetOrigin when the page goes somewhere.
  NetworkServiceHost(BrowserNetwork& network, Loop& hostLoop, int fd, std::string lockedOrigin);
  // Ends the connection and what the page had in flight (the fetches are cancelled, the sockets dropped): that is done on the host's thread, by a task put on `hostLoop`, which is
  // to be running, with `network` there, until a task put after this has run.
  ~NetworkServiceHost();

  NetworkServiceHost(const NetworkServiceHost&) = delete;
  NetworkServiceHost& operator=(const NetworkServiceHost&) = delete;

  // The origins the page may ask for: the one it was made for (empty: none yet) and the ones the browser lets it have as it navigates. SetOrigin is the only one now;
  // AllowOrigin adds one (the page is on its way to it and may still be on the old one); KeepOnlyOrigin drops the others, where the one given is allowed (a page can only ask for less).
  void SetOrigin(std::string origin);
  void AllowOrigin(std::string origin);
  void KeepOnlyOrigin(std::string origin);
  // How many of the page's requests were refused for naming an origin that is not its (for looking at).
  size_t refused() const;

  struct Impl;

 private:
  std::shared_ptr<Impl> impl_;
};

}  // namespace solar::net
