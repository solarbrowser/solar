#include "solar/net/Tls.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <cstring>

namespace solar::net {

// What the BIO and the layer share: the ciphertext being consumed, and the ciphertext to send.
struct TlsState {
  SSL* ssl = nullptr;
  std::span<const uint8_t> input;
  std::string outgoing;
  uint8_t plaintext[16 * 1024];
};

namespace {

int BioRead(BIO* bio, char* out, size_t length, size_t* read) {
  auto* state = static_cast<TlsState*>(BIO_get_data(bio));
  BIO_clear_retry_flags(bio);
  if (state->input.empty()) {
    BIO_set_retry_read(bio);
    *read = 0;
    return 0;
  }
  const size_t count = std::min(length, state->input.size());
  std::memcpy(out, state->input.data(), count);
  state->input = state->input.subspan(count);
  *read = count;
  return 1;
}

int BioWrite(BIO* bio, const char* data, size_t length, size_t* written) {
  auto* state = static_cast<TlsState*>(BIO_get_data(bio));
  state->outgoing.append(data, length);
  *written = length;
  return 1;
}

long BioControl(BIO*, int command, long, void*) { return command == BIO_CTRL_FLUSH ? 1 : 0; }

int BioCreate(BIO* bio) {
  BIO_set_init(bio, 1);
  return 1;
}

BIO_METHOD* BioMethod() {
  static BIO_METHOD* method = [] {
    BIO_METHOD* m = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "solar");
    BIO_meth_set_read_ex(m, BioRead);
    BIO_meth_set_write_ex(m, BioWrite);
    BIO_meth_set_ctrl(m, BioControl);
    BIO_meth_set_create(m, BioCreate);
    return m;
  }();
  return method;
}

std::string DescribeFailure(SSL* ssl) {
  const long verify = SSL_get_verify_result(ssl);
  if (verify != X509_V_OK) return std::string("certificate verify failed: ") + X509_verify_cert_error_string(verify);

  char buffer[256];
  const unsigned long code = ERR_get_error();
  ERR_clear_error();
  if (code == 0) return "TLS handshake failed";
  ERR_error_string_n(code, buffer, sizeof(buffer));
  return std::string("TLS failure: ") + buffer;
}

bool IsIpLiteral(const std::string& name) {
  ASN1_OCTET_STRING* parsed = a2i_IPADDRESS(name.c_str());
  if (!parsed) return false;
  ASN1_OCTET_STRING_free(parsed);
  return true;
}

}  // namespace

TlsContext::~TlsContext() { SSL_CTX_free(context_); }

std::shared_ptr<TlsContext> TlsContext::Create(const TlsOptions& options) {
  SSL_CTX* context = SSL_CTX_new(TLS_client_method());
  if (!context) return nullptr;

  SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION);
  SSL_CTX_set_verify(context, SSL_VERIFY_PEER, nullptr);
  SSL_CTX_set_default_verify_paths(context);

  if (!options.extraRootsPem.empty()) {
    BIO* pem = BIO_new_mem_buf(options.extraRootsPem.data(), static_cast<int>(options.extraRootsPem.size()));
    X509_STORE* store = SSL_CTX_get_cert_store(context);
    while (X509* certificate = PEM_read_bio_X509(pem, nullptr, nullptr, nullptr)) {
      X509_STORE_add_cert(store, certificate);
      X509_free(certificate);
    }
    ERR_clear_error();  // the loop ends on the "no more certificates" error
    BIO_free(pem);
  }
  return std::shared_ptr<TlsContext>(new TlsContext(context));
}

std::shared_ptr<TlsContext> DefaultTlsContext() {
  static std::shared_ptr<TlsContext> context = TlsContext::Create();
  return context;
}

TlsLayer::TlsLayer(std::shared_ptr<TlsContext> context, std::string serverName, ConnectionHandler& upper)
    : context_(std::move(context)), serverName_(std::move(serverName)), upper_(upper), state_(std::make_unique<TlsState>()) {}

TlsLayer::~TlsLayer() {
  if (state_->ssl) SSL_free(state_->ssl);
}

void TlsLayer::OnConnected() {
  SSL* ssl = SSL_new(context_->native());
  state_->ssl = ssl;
  BIO* bio = BIO_new(BioMethod());
  BIO_set_data(bio, state_.get());
  SSL_set_bio(ssl, bio, bio);
  SSL_set_connect_state(ssl);

  if (IsIpLiteral(serverName_)) {
    X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), serverName_.c_str());
  } else {
    SSL_set_tlsext_host_name(ssl, serverName_.c_str());  // an IP literal may not be sent as a name
    SSL_set_hostflags(ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    SSL_set1_host(ssl, serverName_.c_str());
  }
  static const unsigned char kAlpn[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
  SSL_set_alpn_protos(ssl, kAlpn, sizeof(kAlpn));

  Pump();
}

void TlsLayer::OnData(std::span<const uint8_t> data) {
  if (!state_->ssl || !failure_.empty() || closing_) return;
  state_->input = data;
  Pump();
  state_->input = {};
}

void TlsLayer::Flush() {
  if (state_->outgoing.empty() || !lower_) return;
  lower_->Send(std::move(state_->outgoing));
  state_->outgoing.clear();
}

void TlsLayer::Fail(std::string message) {
  if (!failure_.empty()) return;
  failure_ = std::move(message);
  closing_ = true;
  Flush();  // the alert OpenSSL queued
  lower_->Close();
}

// Runs the handshake and then reads whatever is decryptable, until more ciphertext is needed.
void TlsLayer::Pump() {
  SSL* ssl = state_->ssl;
  while (failure_.empty() && !closing_) {
    ERR_clear_error();  // the queue is per thread, and a stale entry would name the wrong failure
    if (!handshaken_) {
      const int result = SSL_do_handshake(ssl);
      if (result == 1) {
        handshaken_ = true;
        Flush();
        upper_.OnConnected();
        continue;
      }
      const int error = SSL_get_error(ssl, result);
      if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
        Flush();
        return;
      }
      Fail(DescribeFailure(ssl));
      return;
    }

    const int count = SSL_read(ssl, state_->plaintext, sizeof(state_->plaintext));
    if (count > 0) {
      upper_.OnData(std::span<const uint8_t>(state_->plaintext, static_cast<size_t>(count)));
      continue;
    }
    const int error = SSL_get_error(ssl, count);
    if (error == SSL_ERROR_WANT_READ) {
      Flush();
      return;
    }
    if (error == SSL_ERROR_ZERO_RETURN) {
      peerClosedCleanly_ = true;
      Close();
      return;
    }
    Fail(DescribeFailure(ssl));
    return;
  }
}

void TlsLayer::Send(std::string data) {
  if (!handshaken_ || closing_ || !failure_.empty() || data.empty()) return;
  size_t written = 0;
  if (!SSL_write_ex(state_->ssl, data.data(), data.size(), &written)) {
    Fail(DescribeFailure(state_->ssl));
    return;
  }
  Flush();
}

void TlsLayer::Close() {
  if (closing_) return;
  closing_ = true;
  if (handshaken_ && failure_.empty()) {
    SSL_shutdown(state_->ssl);  // queues our close_notify
    Flush();
  }
  lower_->Close();
}

void TlsLayer::OnClosed(int error) {
  int reported = error;
  if (failure_.empty() && error == 0 && !closing_) {
    // The peer closed the TCP connection first. Without a close_notify the data may have been cut
    // short, which TLS exists to make detectable.
    if (!handshaken_) {
      failure_ = "connection closed during the TLS handshake";
    } else if (!peerClosedCleanly_) {
      failure_ = "connection closed without a TLS close_notify";
    }
  }
  if (!failure_.empty() && reported == 0) reported = kProtocolError;
  upper_.OnClosed(reported);
}

}  // namespace solar::net
