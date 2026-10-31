/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/web-api/Crypto.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace solar::webapi {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::Value;

namespace {

using Bytes = std::vector<uint8_t>;

std::span<const uint8_t> BytesAt(qe::Args args, size_t i) {
  if (i >= args.size()) return {};
  auto bytes = qe::BytesOf(args[i]);
  return bytes ? *bytes : std::span<const uint8_t>();
}

Value Result(Context& ctx, const Bytes& bytes) {
  Value buffer = qe::NewArrayBuffer(ctx, bytes.size());
  if (!bytes.empty()) {
    if (auto out = qe::MutableBytesOf(buffer)) std::memcpy(out->data(), bytes.data(), bytes.size());
  }
  return buffer;
}

const EVP_MD* HashOf(const std::string& name) {
  if (name == "SHA-1") return EVP_sha1();
  if (name == "SHA-256") return EVP_sha256();
  if (name == "SHA-384") return EVP_sha384();
  if (name == "SHA-512") return EVP_sha512();
  return nullptr;
}

struct CipherCtx {
  EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
  ~CipherCtx() { EVP_CIPHER_CTX_free(c); }
};

// __solarCryptoDigest(hash, data)
Value Digest(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 2) return qe::Undefined();
  const EVP_MD* md = HashOf(qe::ToWtf8(ctx, args[0]));
  if (!md) return qe::Undefined();
  const auto data = BytesAt(args, 1);
  Bytes out(EVP_MAX_MD_SIZE);
  unsigned int length = 0;
  if (!EVP_Digest(data.data(), data.size(), out.data(), &length, md, nullptr)) return qe::Undefined();
  out.resize(length);
  return Result(ctx, out);
}

// __solarCryptoHmac(hash, key, data)
Value Hmac(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 3) return qe::Undefined();
  const EVP_MD* md = HashOf(qe::ToWtf8(ctx, args[0]));
  if (!md) return qe::Undefined();
  const auto key = BytesAt(args, 1);
  const auto data = BytesAt(args, 2);
  Bytes out(EVP_MAX_MD_SIZE);
  unsigned int length = 0;
  if (!HMAC(md, key.data(), static_cast<int>(key.size()), data.data(), data.size(), out.data(), &length)) return qe::Undefined();
  out.resize(length);
  return Result(ctx, out);
}

const EVP_CIPHER* CipherOf(const std::string& mode, size_t keyBytes) {
  if (mode == "AES-CBC") return keyBytes == 16 ? EVP_aes_128_cbc() : keyBytes == 24 ? EVP_aes_192_cbc() : keyBytes == 32 ? EVP_aes_256_cbc() : nullptr;
  if (mode == "AES-CTR") return keyBytes == 16 ? EVP_aes_128_ctr() : keyBytes == 24 ? EVP_aes_192_ctr() : keyBytes == 32 ? EVP_aes_256_ctr() : nullptr;
  if (mode == "AES-GCM") return keyBytes == 16 ? EVP_aes_128_gcm() : keyBytes == 24 ? EVP_aes_192_gcm() : keyBytes == 32 ? EVP_aes_256_gcm() : nullptr;
  if (mode == "AES-KW") return keyBytes == 16 ? EVP_aes_128_wrap() : keyBytes == 24 ? EVP_aes_192_wrap() : keyBytes == 32 ? EVP_aes_256_wrap() : nullptr;
  return nullptr;
}

// __solarCryptoAes(mode, encrypt, key, iv, data, additionalData, tagBytes)
Value Aes(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 5) return qe::Undefined();
  const std::string mode = qe::ToWtf8(ctx, args[0]);
  const bool encrypt = qe::ToNumber(ctx, args[1]) != 0;
  const auto key = BytesAt(args, 2);
  const auto iv = BytesAt(args, 3);
  const auto data = BytesAt(args, 4);
  const auto aad = BytesAt(args, 5);
  const int tagBytes = args.size() > 6 ? static_cast<int>(qe::ToNumber(ctx, args[6])) : 16;
  const EVP_CIPHER* cipher = CipherOf(mode, key.size());
  if (!cipher) return qe::Undefined();
  CipherCtx cc;
  if (!cc.c) return qe::Undefined();
  int length = 0, total = 0;
  Bytes out(data.size() + 32);
  if (mode == "AES-GCM") {
    if (tagBytes < 1 || tagBytes > 16) return qe::Undefined();
    if (!EVP_CipherInit_ex(cc.c, cipher, nullptr, nullptr, nullptr, encrypt ? 1 : 0)) return qe::Undefined();
    if (!EVP_CIPHER_CTX_ctrl(cc.c, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv.size()), nullptr)) return qe::Undefined();
    if (!EVP_CipherInit_ex(cc.c, nullptr, nullptr, key.data(), iv.data(), encrypt ? 1 : 0)) return qe::Undefined();
    if (!aad.empty() && !EVP_CipherUpdate(cc.c, nullptr, &length, aad.data(), static_cast<int>(aad.size()))) return qe::Undefined();
    if (encrypt) {
      if (!EVP_CipherUpdate(cc.c, out.data(), &length, data.data(), static_cast<int>(data.size()))) return qe::Undefined();
      total = length;
      if (!EVP_CipherFinal_ex(cc.c, out.data() + total, &length)) return qe::Undefined();
      total += length;
      out.resize(total + tagBytes);
      if (!EVP_CIPHER_CTX_ctrl(cc.c, EVP_CTRL_GCM_GET_TAG, tagBytes, out.data() + total)) return qe::Undefined();
      return Result(ctx, out);
    }
    if (data.size() < static_cast<size_t>(tagBytes)) return qe::Undefined();
    const size_t body = data.size() - tagBytes;
    if (!EVP_CipherUpdate(cc.c, out.data(), &length, data.data(), static_cast<int>(body))) return qe::Undefined();
    total = length;
    Bytes tag(data.begin() + body, data.end());
    if (!EVP_CIPHER_CTX_ctrl(cc.c, EVP_CTRL_GCM_SET_TAG, tagBytes, tag.data())) return qe::Undefined();
    if (EVP_CipherFinal_ex(cc.c, out.data() + total, &length) <= 0) return qe::Undefined();
    out.resize(total + length);
    return Result(ctx, out);
  }
  if (mode == "AES-KW") EVP_CIPHER_CTX_set_flags(cc.c, EVP_CIPHER_CTX_FLAG_WRAP_ALLOW);
  if (!EVP_CipherInit_ex(cc.c, cipher, nullptr, key.data(), iv.empty() ? nullptr : iv.data(), encrypt ? 1 : 0)) return qe::Undefined();
  if (!EVP_CipherUpdate(cc.c, out.data(), &length, data.data(), static_cast<int>(data.size()))) return qe::Undefined();
  total = length;
  if (EVP_CipherFinal_ex(cc.c, out.data() + total, &length) <= 0) return qe::Undefined();
  out.resize(total + length);
  return Result(ctx, out);
}

// __solarCryptoPbkdf2(hash, password, salt, iterations, bytes)
Value Pbkdf2(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 5) return qe::Undefined();
  const EVP_MD* md = HashOf(qe::ToWtf8(ctx, args[0]));
  if (!md) return qe::Undefined();
  const auto password = BytesAt(args, 1);
  const auto salt = BytesAt(args, 2);
  const int iterations = static_cast<int>(qe::ToNumber(ctx, args[3]));
  const size_t length = static_cast<size_t>(qe::ToNumber(ctx, args[4]));
  Bytes out(length);
  if (length == 0) return Result(ctx, out);
  if (!PKCS5_PBKDF2_HMAC(reinterpret_cast<const char*>(password.data()), static_cast<int>(password.size()), salt.data(), static_cast<int>(salt.size()), iterations, md,
                         static_cast<int>(length), out.data())) {
    return qe::Undefined();
  }
  return Result(ctx, out);
}

// __solarCryptoHkdf(hash, key, salt, info, bytes)
Value Hkdf(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 5) return qe::Undefined();
  const std::string hash = qe::ToWtf8(ctx, args[0]);
  if (!HashOf(hash)) return qe::Undefined();
  const auto key = BytesAt(args, 1);
  const auto salt = BytesAt(args, 2);
  const auto info = BytesAt(args, 3);
  const size_t length = static_cast<size_t>(qe::ToNumber(ctx, args[4]));
  Bytes out(length);
  if (length == 0) return Result(ctx, out);
  EVP_KDF* kdf = EVP_KDF_fetch(nullptr, "HKDF", nullptr);
  if (!kdf) return qe::Undefined();
  EVP_KDF_CTX* kctx = EVP_KDF_CTX_new(kdf);
  EVP_KDF_free(kdf);
  if (!kctx) return qe::Undefined();
  OSSL_PARAM params[] = {
      OSSL_PARAM_construct_utf8_string("digest", const_cast<char*>(hash == "SHA-1" ? "SHA1" : hash == "SHA-256" ? "SHA256" : hash == "SHA-384" ? "SHA384" : "SHA512"), 0),
      OSSL_PARAM_construct_octet_string("key", const_cast<uint8_t*>(key.data()), key.size()),
      OSSL_PARAM_construct_octet_string("salt", const_cast<uint8_t*>(salt.data()), salt.size()),
      OSSL_PARAM_construct_octet_string("info", const_cast<uint8_t*>(info.data()), info.size()),
      OSSL_PARAM_construct_end()};
  const bool good = EVP_KDF_derive(kctx, out.data(), out.size(), params) > 0;
  EVP_KDF_CTX_free(kctx);
  return good ? Result(ctx, out) : qe::Undefined();
}

// __solarCryptoRandom(bytes)
Value Random(Context& ctx, Value, qe::Args args, Value) {
  if (args.empty()) return qe::Undefined();
  const size_t length = static_cast<size_t>(qe::ToNumber(ctx, args[0]));
  Bytes out(length);
  if (length && RAND_bytes(out.data(), static_cast<int>(length)) != 1) return qe::Undefined();
  return Result(ctx, out);
}

}  // namespace

void InstallCryptoNatives(qe::Realm& realm) {
  Context& ctx = realm.GetContext();
  qe::DefineGlobalFunction(ctx, "__solarCryptoDigest", Digest, 2);
  qe::DefineGlobalFunction(ctx, "__solarCryptoHmac", Hmac, 3);
  qe::DefineGlobalFunction(ctx, "__solarCryptoAes", Aes, 7);
  qe::DefineGlobalFunction(ctx, "__solarCryptoPbkdf2", Pbkdf2, 5);
  qe::DefineGlobalFunction(ctx, "__solarCryptoHkdf", Hkdf, 5);
  qe::DefineGlobalFunction(ctx, "__solarCryptoRandom", Random, 1);
}

}  // namespace solar::webapi
