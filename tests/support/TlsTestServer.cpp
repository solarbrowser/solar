#include "TlsTestServer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace solar::test {

int ServerNameCallback(ssl_st* ssl, int*, void* argument) {
  auto* server = static_cast<TlsTestServer*>(argument);
  const char* name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
  std::lock_guard<std::mutex> lock(server->mutex_);
  server->serverName_ = name ? name : "";
  return SSL_TLSEXT_ERR_OK;
}

int AlpnCallback(ssl_st* ssl, const unsigned char** out, unsigned char* outLength, const unsigned char* in,
                 unsigned int inLength, void* argument) {
  static const unsigned char kProtocols[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
  auto* server = static_cast<TlsTestServer*>(argument);
  if (SSL_select_next_proto(const_cast<unsigned char**>(out), outLength, kProtocols, sizeof(kProtocols), in, inLength) !=
      OPENSSL_NPN_NEGOTIATED) {
    return SSL_TLSEXT_ERR_NOACK;
  }
  std::lock_guard<std::mutex> lock(server->mutex_);
  server->alpn_.assign(reinterpret_cast<const char*>(*out), *outLength);
  (void)ssl;
  return SSL_TLSEXT_ERR_OK;
}

TlsTestServer::TlsTestServer(const Identity& identity, Script script, bool sendCloseNotify)
    : script_(std::move(script)), sendCloseNotify_(sendCloseNotify) {
  context_ = SSL_CTX_new(TLS_server_method());
  BIO* certificate = BIO_new_mem_buf(identity.certificatePem.data(), static_cast<int>(identity.certificatePem.size()));
  X509* x509 = PEM_read_bio_X509(certificate, nullptr, nullptr, nullptr);
  BIO_free(certificate);
  BIO* key = BIO_new_mem_buf(identity.keyPem.data(), static_cast<int>(identity.keyPem.size()));
  EVP_PKEY* pkey = PEM_read_bio_PrivateKey(key, nullptr, nullptr, nullptr);
  BIO_free(key);
  SSL_CTX_use_certificate(context_, x509);
  SSL_CTX_use_PrivateKey(context_, pkey);
  X509_free(x509);
  EVP_PKEY_free(pkey);
  SSL_CTX_set_tlsext_servername_callback(context_, ServerNameCallback);
  SSL_CTX_set_tlsext_servername_arg(context_, this);
  SSL_CTX_set_alpn_select_cb(context_, AlpnCallback, this);

  listener_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listener_ < 0) return;
  int one = 1;
  ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t length = sizeof(address);
  if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), length) < 0 || ::listen(listener_, 16) < 0) {
    ::close(listener_);
    listener_ = -1;
    return;
  }
  ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
  port_ = ntohs(address.sin_port);
  acceptor_ = std::thread([this] { AcceptLoop(); });
}

TlsTestServer::~TlsTestServer() {
  stopping_ = true;
  if (acceptor_.joinable()) acceptor_.join();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (int client : clients_) ::shutdown(client, SHUT_RDWR);
  }
  for (std::thread& worker : workers_) worker.join();
  if (listener_ >= 0) ::close(listener_);
  SSL_CTX_free(context_);
}

void TlsTestServer::AcceptLoop() {
  while (!stopping_) {
    pollfd waiting{listener_, POLLIN, 0};
    if (::poll(&waiting, 1, 20) <= 0) continue;
    int client = ::accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);
    if (client < 0) continue;

    std::lock_guard<std::mutex> lock(mutex_);
    clients_.push_back(client);
    workers_.emplace_back([this, client] {
      SSL* connection = SSL_new(context_);
      SSL_set_fd(connection, client);
      if (SSL_accept(connection) == 1) {
        {
          std::lock_guard<std::mutex> state(mutex_);
          version_ = SSL_get_version(connection);
        }
        script_(connection);
        if (sendCloseNotify_) SSL_shutdown(connection);
      }
      SSL_free(connection);
      ::close(client);
    });
  }
}

std::string TlsTestServer::serverName() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return serverName_;
}

std::string TlsTestServer::alpn() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return alpn_;
}

std::string TlsTestServer::version() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return version_;
}

std::string TlsTestServer::ReadHead(ssl_st* connection) {
  std::string head;
  char byte;
  while (head.find("\r\n\r\n") == std::string::npos && SSL_read(connection, &byte, 1) == 1) head.push_back(byte);
  return head;
}

void TlsTestServer::SendAll(ssl_st* connection, std::string_view data) {
  while (!data.empty()) {
    size_t written = 0;
    if (!SSL_write_ex(connection, data.data(), data.size(), &written)) return;
    data.remove_prefix(written);
  }
}

void TlsTestServer::WaitForClose(ssl_st* connection) {
  char byte;
  while (SSL_read(connection, &byte, 1) > 0) {
  }
}

}  // namespace solar::test
