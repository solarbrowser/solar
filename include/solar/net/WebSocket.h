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
#include "solar/net/Tls.h"
#include "solar/url/Url.h"

// WebSocket (RFC 6455): a connection made with an HTTP/1.1 upgrade, over TLS for wss, that carries messages both ways until either end closes it. The
// client of it is made on the HttpClient's loop, lookups and cookies; the messages are whole ones (a fragmented message comes together).
namespace solar::net {

// What happens on a connection. Every call is made from the loop's thread, never from inside the call that asked for something.
class WebSocketHandler {
 public:
  virtual ~WebSocketHandler() = default;
  // The server accepted the upgrade: the sub-protocol it chose ("" for none) and the extensions it agreed to.
  virtual void OnOpen(const std::string& protocol, const std::string& extensions) = 0;
  virtual void OnMessage(std::string data, bool binary) = 0;
  // The last call. `clean` is a close that both ends took part in; otherwise the code is 1006 and `error` says what went wrong (empty if the
  // connection was only dropped).
  virtual void OnClosed(bool clean, uint16_t code, const std::string& reason, const std::string& error) = 0;
};

struct WebSocketOptions {
  std::vector<std::string> protocols;  // the sub-protocols offered, in order
  std::string origin;                  // the page's origin, sent as Origin
  std::shared_ptr<TlsContext> tls;     // null means the shared context
  std::optional<url::Url> initiator;   // the page the connection is made for (cookies are held back from a cross-site one)
  std::vector<std::pair<std::string, std::string>> headers;  // others to send (not the ones the upgrade is made of)
  size_t maxMessageBytes = size_t{256} << 20;
};

class WebSocketClient;

class WebSocketHandle {
 public:
  WebSocketHandle() = default;
  // Sends a message (silently dropped unless the connection is open).
  void Send(std::string data, bool binary);
  // Begins the closing handshake with the code and reason (123 bytes at most), or ends a connection that is not yet open.
  void Close(uint16_t code = 1005, std::string reason = {});
  // Bytes that were sent and have not yet been given to the network.
  size_t BufferedAmount() const;

 private:
  friend class WebSocketClient;
  WebSocketHandle(WebSocketClient* client, uint64_t id) : client_(client), id_(id) {}
  WebSocketClient* client_ = nullptr;
  uint64_t id_ = 0;
};

class WebSocketClient {
 public:
  explicit WebSocketClient(HttpClient& http);
  ~WebSocketClient();
  WebSocketClient(const WebSocketClient&) = delete;
  WebSocketClient& operator=(const WebSocketClient&) = delete;

  // ws and wss addresses; http and https ones are taken for them. The handler must outlive the connection, which ends with its OnClosed.
  WebSocketHandle Connect(const url::Url& url, WebSocketHandler& handler, WebSocketOptions options = {});
  // Ends every connection of the client (each gets OnClosed).
  void CloseAll();

  struct Impl;

 private:
  friend class WebSocketHandle;
  std::unique_ptr<Impl> impl_;
};

}  // namespace solar::net
