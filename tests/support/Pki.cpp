#include "Pki.h"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509v3.h>

#include <cstdlib>

namespace solar::test {

namespace {

std::string ToPem(X509* certificate) {
  BIO* bio = BIO_new(BIO_s_mem());
  PEM_write_bio_X509(bio, certificate);
  char* data = nullptr;
  long size = BIO_get_mem_data(bio, &data);
  std::string out(data, static_cast<size_t>(size));
  BIO_free(bio);
  return out;
}

std::string ToPem(EVP_PKEY* key) {
  BIO* bio = BIO_new(BIO_s_mem());
  PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr);
  char* data = nullptr;
  long size = BIO_get_mem_data(bio, &data);
  std::string out(data, static_cast<size_t>(size));
  BIO_free(bio);
  return out;
}

X509* CertificateFromPem(const std::string& pem) {
  BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
  X509* certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  return certificate;
}

EVP_PKEY* KeyFromPem(const std::string& pem) {
  BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
  EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  return key;
}

void AddExtension(X509* certificate, X509* issuer, int nid, const char* value) {
  X509V3_CTX context;
  X509V3_set_ctx(&context, issuer, certificate, nullptr, nullptr, 0);
  X509_EXTENSION* extension = X509V3_EXT_conf_nid(nullptr, &context, nid, value);
  X509_add_ext(certificate, extension, -1);
  X509_EXTENSION_free(extension);
}

void SetSerial(X509* certificate) {
  unsigned char bytes[8];
  RAND_bytes(bytes, sizeof(bytes));
  bytes[0] &= 0x7F;
  BIGNUM* number = BN_bin2bn(bytes, sizeof(bytes), nullptr);
  BN_to_ASN1_INTEGER(number, X509_get_serialNumber(certificate));
  BN_free(number);
}

bool LooksLikeIp(const std::string& name) {
  return name.find(':') != std::string::npos || name.find_first_not_of("0123456789.") == std::string::npos;
}

}  // namespace

TestPki::TestPki() {
  EVP_PKEY* key = EVP_EC_gen("P-256");
  X509* root = X509_new();
  X509_set_version(root, 2);
  SetSerial(root);
  X509_gmtime_adj(X509_getm_notBefore(root), -86400L);
  X509_gmtime_adj(X509_getm_notAfter(root), 86400L * 365);
  X509_NAME* name = X509_get_subject_name(root);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("solar test root"), -1, -1, 0);
  X509_set_issuer_name(root, name);
  X509_set_pubkey(root, key);
  AddExtension(root, root, NID_basic_constraints, "critical,CA:TRUE");
  AddExtension(root, root, NID_key_usage, "critical,keyCertSign,cRLSign");
  X509_sign(root, key, EVP_sha256());

  rootPem_ = ToPem(root);
  rootKeyPem_ = ToPem(key);
  X509_free(root);
  EVP_PKEY_free(key);
}

Identity TestPki::Issue(const std::vector<std::string>& names, int notBeforeDays, int notAfterDays) const {
  X509* root = CertificateFromPem(rootPem_);
  EVP_PKEY* rootKey = KeyFromPem(rootKeyPem_);
  EVP_PKEY* key = EVP_EC_gen("P-256");

  X509* certificate = X509_new();
  X509_set_version(certificate, 2);
  SetSerial(certificate);
  X509_gmtime_adj(X509_getm_notBefore(certificate), 86400L * notBeforeDays);
  X509_gmtime_adj(X509_getm_notAfter(certificate), 86400L * notAfterDays);
  X509_NAME_add_entry_by_txt(X509_get_subject_name(certificate), "CN", MBSTRING_ASC,
                             reinterpret_cast<const unsigned char*>("solar test server"), -1, -1, 0);
  X509_set_issuer_name(certificate, X509_get_subject_name(root));
  X509_set_pubkey(certificate, key);

  std::string alternativeNames;
  for (const std::string& name : names) {
    if (!alternativeNames.empty()) alternativeNames += ",";
    alternativeNames += (LooksLikeIp(name) ? "IP:" : "DNS:") + name;
  }
  AddExtension(certificate, root, NID_basic_constraints, "critical,CA:FALSE");
  AddExtension(certificate, root, NID_subject_alt_name, alternativeNames.c_str());
  X509_sign(certificate, rootKey, EVP_sha256());

  Identity identity{ToPem(certificate), ToPem(key)};
  X509_free(certificate);
  X509_free(root);
  EVP_PKEY_free(rootKey);
  EVP_PKEY_free(key);
  return identity;
}

}  // namespace solar::test
