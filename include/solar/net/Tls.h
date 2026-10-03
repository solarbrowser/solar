#pragma once

#include <memory>
#include <string>

#include "solar/net/Loop.h"

struct ssl_ctx_st;

namespace solar::net {

struct TlsOptions {
  // PEM certificates to trust in addition to the system's roots, for a private CA.
  std::string extraRootsPem;
};

// The settings shared by the connections made with it. TLS 1.2 is the oldest version spoken and
// the server's certificate and name are always verified; there is no switch to turn that off.
class TlsContext {
 public:
  static std::shared_ptr<TlsContext> Create(const TlsOptions& options = {});
  ~TlsContext();

  TlsContext(const TlsContext&) = delete;
  TlsContext& operator=(const TlsContext&) = delete;

  ssl_ctx_st* native() const { return context_; }

 private:
  explicit TlsContext(ssl_ctx_st* context) : context_(context) {}
  ssl_ctx_st* context_;
};

// The process's shared context, which trusts the system's roots only.
std::shared_ptr<TlsContext> DefaultTlsContext();

struct TlsState;

// TLS over a connection. It is the handler of the connection below it and the transport of the
// handler above it, which sees the plaintext: OnConnected comes when the handshake has finished
// and the certificate has been checked. Ciphertext is read straight out of the buffer the
// connection delivered, not copied into one of OpenSSL's first. It deletes itself after the
// handler above has been told the connection is over.
class TlsLayer : public ConnectionHandler, public Transport {
 public:
  // `serverName` is what the certificate must name: a DNS name, or an IP address literal.
  TlsLayer(std::shared_ptr<TlsContext> context, std::string serverName, ConnectionHandler& upper);
  ~TlsLayer() override;

  // Must be called with the connection this layer was given to, before the loop runs.
  void Attach(Connection* connection) { lower_ = connection; }

  void Send(std::string data) override;
  void Close() override;

  // Why the connection failed, if the failure was TLS's; empty otherwise. Valid until the
  // handler above has returned from OnClosed.
  const std::string& failure() const { return failure_; }

  void OnConnected() override;
  void OnData(std::span<const uint8_t> data) override;
  void OnClosed(int error) override;

 private:
  void Pump();
  void Flush();
  void Fail(std::string message);

  std::shared_ptr<TlsContext> context_;
  std::string serverName_;
  ConnectionHandler& upper_;
  Connection* lower_ = nullptr;
  std::unique_ptr<TlsState> state_;
  bool handshaken_ = false;
  bool closing_ = false;
  bool peerClosedCleanly_ = false;
  std::string failure_;
};

}  // namespace solar::net
