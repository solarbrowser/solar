#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "Pki.h"

struct ssl_st;
struct ssl_ctx_st;

namespace solar::test {

// Like TestServer, with TLS: the script is called with the connection once the handshake is done.
class TlsTestServer {
 public:
  using Script = std::function<void(ssl_st*)>;

  // `sendCloseNotify`: end each connection with a close_notify, as a well-behaved server does.
  // `protocols`: what ALPN may settle on, most preferred first, of those the client offers.
  TlsTestServer(const Identity& identity, Script script, bool sendCloseNotify = true, std::vector<std::string> protocols = {"http/1.1"});
  ~TlsTestServer();

  TlsTestServer(const TlsTestServer&) = delete;
  TlsTestServer& operator=(const TlsTestServer&) = delete;

  bool ok() const { return listener_ >= 0; }
  uint16_t port() const { return port_; }

  // What the client sent in its hello, as seen by the most recent connection.
  std::string serverName() const;
  std::string alpn() const;
  std::string version() const;

  static std::string ReadHead(ssl_st* connection);
  static std::string ReadBytes(ssl_st* connection, size_t length);
  static void SendAll(ssl_st* connection, std::string_view data);
  static void WaitForClose(ssl_st* connection);

 private:
  void AcceptLoop();

  Script script_;
  bool sendCloseNotify_;
  std::string protocolList_;  // in ALPN's wire format
  ssl_ctx_st* context_ = nullptr;
  int listener_ = -1;
  uint16_t port_ = 0;
  std::atomic<bool> stopping_{false};
  std::thread acceptor_;

  mutable std::mutex mutex_;
  std::string serverName_;
  std::string alpn_;
  std::string version_;
  std::vector<std::thread> workers_;
  std::vector<int> clients_;

  friend int ServerNameCallback(ssl_st*, int*, void*);
  friend int AlpnCallback(ssl_st*, const unsigned char**, unsigned char*, const unsigned char*, unsigned int, void*);
};

}  // namespace solar::test
