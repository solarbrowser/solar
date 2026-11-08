/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "solar/net/Tls.h"
#include "solar/url/Url.h"

// What a page says to the network about a WebSocket, and hears of it: the handler it gives, the options, and the handle it holds (a number the network service knows the connection by).
namespace solar::net {

// What happens on a connection. Every call is made from the loop of the thread the connection was asked for on, never from inside the call that asked for something.
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

class NetworkService;

class WebSocketHandle {
 public:
  WebSocketHandle() = default;
  // Sends a message (silently dropped unless the connection is open).
  void Send(std::string data, bool binary);
  // Begins the closing handshake with the code and reason (123 bytes at most), or ends a connection that is not yet open.
  void Close(uint16_t code = 1005, std::string reason = {});
  // Drops the connection at once (the page went away): it is told of as not clean, and the page hears no more after.
  void Abort();
  // Bytes that were sent and have not yet been given to the network.
  size_t BufferedAmount() const;
  uint64_t id() const { return id_; }

 private:
  friend class NetworkService;
  friend class WebSocketClient;
  WebSocketHandle(NetworkService* service, uint64_t id) : service_(service), id_(id) {}
  NetworkService* service_ = nullptr;
  uint64_t id_ = 0;
};

}  // namespace solar::net
