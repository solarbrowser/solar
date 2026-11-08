/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "solar/net/HttpClient.h"
#include "solar/net/WebSocketTypes.h"
#include "solar/net/Tls.h"
#include "solar/url/Url.h"

// WebSocket (RFC 6455): a connection made with an HTTP/1.1 upgrade, over TLS for wss, that carries messages both ways until either end closes it. The
// client of it is made on the HttpClient's loop, lookups and cookies; the messages are whole ones (a fragmented message comes together).
namespace solar::net {

class WebSocketClient;

class WebSocketClient {
 public:
  explicit WebSocketClient(HttpClient& http);
  ~WebSocketClient();
  WebSocketClient(const WebSocketClient&) = delete;
  WebSocketClient& operator=(const WebSocketClient&) = delete;

  // ws and wss addresses; http and https ones are taken for them. The handler must outlive the connection, which ends with its OnClosed.
  uint64_t Connect(const url::Url& url, WebSocketHandler& handler, WebSocketOptions options = {});
  // What the connection with that number is asked to do (nothing for one that has ended).
  void Send(uint64_t id, std::string data, bool binary);
  void Close(uint64_t id, uint16_t code, std::string reason);
  void Abort(uint64_t id);
  // Ends every connection of the client (each gets OnClosed).
  void CloseAll();

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace solar::net
