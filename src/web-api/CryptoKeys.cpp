/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// The public-key side of crypto.subtle (src/web-api/Crypto.cpp has the rest): keys are held as DER (PKCS#8 for a private key, SubjectPublicKeyInfo for a public one),
// and what the script asks of them is signing, verifying, RSA-OAEP, ECDH/X25519, and the conversions to and from the parts a JWK or a raw key is made of.
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "solar/web-api/Crypto.h"

namespace solar::webapi {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::Value;

namespace {

using Bytes = std::vector<uint8_t>;
template <typename T, void (*F)(T*)>
struct Free {
  void operator()(T* p) const { if (p) F(p); }
};
using PKey = std::unique_ptr<EVP_PKEY, Free<EVP_PKEY, EVP_PKEY_free>>;
using PCtx = std::unique_ptr<EVP_PKEY_CTX, Free<EVP_PKEY_CTX, EVP_PKEY_CTX_free>>;
using MdCtx = std::unique_ptr<EVP_MD_CTX, Free<EVP_MD_CTX, EVP_MD_CTX_free>>;
using Bn = std::unique_ptr<BIGNUM, Free<BIGNUM, BN_free>>;

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

const char* CurveOf(const std::string& name) {
  if (name == "P-256") return "prime256v1";
  if (name == "P-384") return "secp384r1";
  if (name == "P-521") return "secp521r1";
  return nullptr;
}

std::string NameOfCurve(EVP_PKEY* key) {
  char buffer[64] = {};
  size_t length = 0;
  if (!EVP_PKEY_get_utf8_string_param(key, OSSL_PKEY_PARAM_GROUP_NAME, buffer, sizeof buffer, &length)) return "";
  const std::string name(buffer, length);
  if (name == "prime256v1" || name == "P-256") return "P-256";
  if (name == "secp384r1" || name == "P-384") return "P-384";
  if (name == "secp521r1" || name == "P-521") return "P-521";
  return name;
}

Bytes PublicDer(EVP_PKEY* key) {
  Bytes out;
  const int n = i2d_PUBKEY(key, nullptr);
  if (n <= 0) return out;
  out.resize(n);
  uint8_t* p = out.data();
  i2d_PUBKEY(key, &p);
  return out;
}

Bytes PrivateDer(EVP_PKEY* key) {
  Bytes out;
  PKCS8_PRIV_KEY_INFO* info = EVP_PKEY2PKCS8(key);
  if (!info) return out;
  const int n = i2d_PKCS8_PRIV_KEY_INFO(info, nullptr);
  if (n > 0) {
    out.resize(n);
    uint8_t* p = out.data();
    i2d_PKCS8_PRIV_KEY_INFO(info, &p);
  }
  PKCS8_PRIV_KEY_INFO_free(info);
  return out;
}

PKey ParsePublic(std::span<const uint8_t> der) {
  const uint8_t* p = der.data();
  return PKey(d2i_PUBKEY(nullptr, &p, static_cast<long>(der.size())));
}

PKey ParsePrivate(std::span<const uint8_t> der) {
  const uint8_t* p = der.data();
  PKCS8_PRIV_KEY_INFO* info = d2i_PKCS8_PRIV_KEY_INFO(nullptr, &p, static_cast<long>(der.size()));
  if (!info) return nullptr;
  PKey key(EVP_PKCS82PKEY(info));
  PKCS8_PRIV_KEY_INFO_free(info);
  return key;
}

// Either DER form of a key (the script says which, by `isPrivate`).
PKey Parse(std::span<const uint8_t> der, bool isPrivate) { return isPrivate ? ParsePrivate(der) : ParsePublic(der); }

std::string B64Url(const Bytes& bytes) {
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  for (size_t i = 0; i < bytes.size(); i += 3) {
    const uint32_t n = (uint32_t{bytes[i]} << 16) | (i + 1 < bytes.size() ? uint32_t{bytes[i + 1]} << 8 : 0) | (i + 2 < bytes.size() ? uint32_t{bytes[i + 2]} : 0);
    out += alphabet[(n >> 18) & 63];
    out += alphabet[(n >> 12) & 63];
    if (i + 1 < bytes.size()) out += alphabet[(n >> 6) & 63];
    if (i + 2 < bytes.size()) out += alphabet[n & 63];
  }
  return out;
}

Bytes FromB64Url(const std::string& text) {
  Bytes out;
  uint32_t bits = 0;
  int count = 0;
  for (char c : text) {
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '-' || c == '+') v = 62;
    else if (c == '_' || c == '/') v = 63;
    else continue;
    bits = (bits << 6) | v;
    count += 6;
    if (count >= 8) {
      count -= 8;
      out.push_back(static_cast<uint8_t>((bits >> count) & 0xFF));
    }
  }
  return out;
}

bool BnParam(EVP_PKEY* key, const char* name, Bytes& out, int pad = 0) {
  BIGNUM* raw = nullptr;
  if (!EVP_PKEY_get_bn_param(key, name, &raw) || !raw) return false;
  Bn number(raw);
  const int size = pad > 0 ? pad : BN_num_bytes(number.get());
  out.assign(size, 0);
  return BN_bn2binpad(number.get(), out.data(), size) >= 0;
}

// __solarCryptoGenerate(kind, parameter, exponentBytes): [pkcs8, spki] for "RSA" (parameter: modulus bits), "EC" (parameter: "P-256"...), "Ed25519", "X25519".
Value Generate(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 2) return qe::Undefined();
  const std::string kind = qe::ToWtf8(ctx, args[0]);
  PKey key;
  EVP_PKEY* raw = nullptr;
  if (kind == "RSA") {
    PCtx pc(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr));
    if (!pc || EVP_PKEY_keygen_init(pc.get()) <= 0) return qe::Undefined();
    if (EVP_PKEY_CTX_set_rsa_keygen_bits(pc.get(), static_cast<int>(qe::ToNumber(ctx, args[1]))) <= 0) return qe::Undefined();
    const auto exponent = BytesAt(args, 2);
    if (!exponent.empty()) {
      BIGNUM* e = BN_bin2bn(exponent.data(), static_cast<int>(exponent.size()), nullptr);
      if (!e || EVP_PKEY_CTX_set1_rsa_keygen_pubexp(pc.get(), e) <= 0) { BN_free(e); return qe::Undefined(); }
      BN_free(e);
    }
    if (EVP_PKEY_keygen(pc.get(), &raw) <= 0) return qe::Undefined();
  } else if (kind == "EC") {
    const char* curve = CurveOf(qe::ToWtf8(ctx, args[1]));
    if (!curve) return qe::Undefined();
    raw = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", curve);
  } else if (kind == "Ed25519" || kind == "X25519") {
    raw = EVP_PKEY_Q_keygen(nullptr, nullptr, kind.c_str());
  }
  key.reset(raw);
  if (!key) return qe::Undefined();
  Value pair = qe::NewArray(ctx);
  qe::ArrayPush(ctx, pair, Result(ctx, PrivateDer(key.get())));
  qe::ArrayPush(ctx, pair, Result(ctx, PublicDer(key.get())));
  return pair;
}

// __solarCryptoInfo(der, isPrivate): JSON text {kind, curve, bits, exponent} of a key, or undefined if it is not one this knows.
Value Info(Context& ctx, Value, qe::Args args, Value) {
  PKey key = Parse(BytesAt(args, 0), args.size() > 1 && qe::ToNumber(ctx, args[1]) != 0);
  if (!key) return qe::Undefined();
  std::string json;
  const int type = EVP_PKEY_base_id(key.get());
  if (type == EVP_PKEY_RSA) {
    Bytes e;
    BnParam(key.get(), OSSL_PKEY_PARAM_RSA_E, e);
    json = "{\"kind\":\"RSA\",\"bits\":" + std::to_string(EVP_PKEY_bits(key.get())) + ",\"e\":\"" + B64Url(e) + "\"}";
  } else if (type == EVP_PKEY_EC) {
    json = "{\"kind\":\"EC\",\"curve\":\"" + NameOfCurve(key.get()) + "\"}";
  } else if (type == EVP_PKEY_ED25519) {
    json = "{\"kind\":\"Ed25519\"}";
  } else if (type == EVP_PKEY_X25519) {
    json = "{\"kind\":\"X25519\"}";
  } else {
    return qe::Undefined();
  }
  return qe::FromWtf8(ctx, json);
}

// __solarCryptoPublicOf(privateDer): the SPKI of a private key.
Value PublicOf(Context& ctx, Value, qe::Args args, Value) {
  PKey key = ParsePrivate(BytesAt(args, 0));
  if (!key) return qe::Undefined();
  return Result(ctx, PublicDer(key.get()));
}

// __solarCryptoParts(der, isPrivate): JSON text of the base64url parts of a key: RSA n e [d p q dp dq qi], EC x y [d], OKP x [d].
Value Parts(Context& ctx, Value, qe::Args args, Value) {
  const bool isPrivate = args.size() > 1 && qe::ToNumber(ctx, args[1]) != 0;
  PKey key = Parse(BytesAt(args, 0), isPrivate);
  if (!key) return qe::Undefined();
  std::string json = "{";
  auto add = [&](const char* name, const Bytes& bytes) { if (json.size() > 1) json += ","; json += std::string("\"") + name + "\":\"" + B64Url(bytes) + "\""; };
  Bytes b;
  const int type = EVP_PKEY_base_id(key.get());
  if (type == EVP_PKEY_RSA) {
    if (BnParam(key.get(), OSSL_PKEY_PARAM_RSA_N, b)) add("n", b);
    if (BnParam(key.get(), OSSL_PKEY_PARAM_RSA_E, b)) add("e", b);
    if (isPrivate) {
      if (BnParam(key.get(), OSSL_PKEY_PARAM_RSA_D, b)) add("d", b);
      if (BnParam(key.get(), OSSL_PKEY_PARAM_RSA_FACTOR1, b)) add("p", b);
      if (BnParam(key.get(), OSSL_PKEY_PARAM_RSA_FACTOR2, b)) add("q", b);
      if (BnParam(key.get(), OSSL_PKEY_PARAM_RSA_EXPONENT1, b)) add("dp", b);
      if (BnParam(key.get(), OSSL_PKEY_PARAM_RSA_EXPONENT2, b)) add("dq", b);
      if (BnParam(key.get(), OSSL_PKEY_PARAM_RSA_COEFFICIENT1, b)) add("qi", b);
    }
  } else if (type == EVP_PKEY_EC) {
    const int size = (EVP_PKEY_bits(key.get()) + 7) / 8;
    BnParam(key.get(), OSSL_PKEY_PARAM_EC_PUB_X, b, size);
    if (!b.empty()) add("x", b);
    if (BnParam(key.get(), OSSL_PKEY_PARAM_EC_PUB_Y, b, size)) add("y", b);
    if (isPrivate && BnParam(key.get(), OSSL_PKEY_PARAM_PRIV_KEY, b, size)) add("d", b);
  } else if (type == EVP_PKEY_ED25519 || type == EVP_PKEY_X25519) {
    uint8_t buffer[64];
    size_t length = sizeof buffer;
    if (EVP_PKEY_get_raw_public_key(key.get(), buffer, &length)) add("x", Bytes(buffer, buffer + length));
    length = sizeof buffer;
    if (isPrivate && EVP_PKEY_get_raw_private_key(key.get(), buffer, &length)) add("d", Bytes(buffer, buffer + length));
  } else {
    return qe::Undefined();
  }
  return qe::FromWtf8(ctx, json + "}");
}

Bytes Field(Context& ctx, Value object, const char* name) {
  Value v = qe::Get(ctx, object, name);
  if (v.is_undefined()) return {};
  return FromB64Url(qe::ToWtf8(ctx, v));
}

// __solarCryptoFromParts(kind, curve, parts (object of base64url strings), isPrivate): the DER of a key made of its parts.
Value FromParts(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 4) return qe::Undefined();
  const std::string kind = qe::ToWtf8(ctx, args[0]);
  const bool isPrivate = qe::ToNumber(ctx, args[3]) != 0;
  Value parts = args[2];
  PKey key;
  if (kind == "RSA") {
    OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
    std::vector<Bn> keep;
    auto push = [&](const char* name, const Bytes& bytes) {
      if (bytes.empty()) return;
      keep.emplace_back(BN_bin2bn(bytes.data(), static_cast<int>(bytes.size()), nullptr));
      OSSL_PARAM_BLD_push_BN(bld, name, keep.back().get());
    };
    push(OSSL_PKEY_PARAM_RSA_N, Field(ctx, parts, "n"));
    push(OSSL_PKEY_PARAM_RSA_E, Field(ctx, parts, "e"));
    if (isPrivate) {
      push(OSSL_PKEY_PARAM_RSA_D, Field(ctx, parts, "d"));
      push(OSSL_PKEY_PARAM_RSA_FACTOR1, Field(ctx, parts, "p"));
      push(OSSL_PKEY_PARAM_RSA_FACTOR2, Field(ctx, parts, "q"));
      push(OSSL_PKEY_PARAM_RSA_EXPONENT1, Field(ctx, parts, "dp"));
      push(OSSL_PKEY_PARAM_RSA_EXPONENT2, Field(ctx, parts, "dq"));
      push(OSSL_PKEY_PARAM_RSA_COEFFICIENT1, Field(ctx, parts, "qi"));
    }
    OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(bld);
    PCtx pc(EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr));
    EVP_PKEY* raw = nullptr;
    if (pc && params && EVP_PKEY_fromdata_init(pc.get()) > 0) EVP_PKEY_fromdata(pc.get(), &raw, isPrivate ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY, params);
    key.reset(raw);
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(bld);
  } else if (kind == "EC") {
    const char* curve = CurveOf(qe::ToWtf8(ctx, args[1]));
    if (!curve) return qe::Undefined();
    Bytes x = Field(ctx, parts, "x"), y = Field(ctx, parts, "y"), d = Field(ctx, parts, "d");
    Bytes point;
    point.push_back(4);
    point.insert(point.end(), x.begin(), x.end());
    point.insert(point.end(), y.begin(), y.end());
    OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
    OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, curve, 0);
    OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size());
    Bn priv;
    if (isPrivate && !d.empty()) {
      priv.reset(BN_bin2bn(d.data(), static_cast<int>(d.size()), nullptr));
      OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_PRIV_KEY, priv.get());
    }
    OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(bld);
    PCtx pc(EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr));
    EVP_PKEY* raw = nullptr;
    if (pc && params && EVP_PKEY_fromdata_init(pc.get()) > 0) EVP_PKEY_fromdata(pc.get(), &raw, isPrivate ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY, params);
    key.reset(raw);
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(bld);
  } else if (kind == "Ed25519" || kind == "X25519") {
    const int type = kind == "Ed25519" ? EVP_PKEY_ED25519 : EVP_PKEY_X25519;
    Bytes x = Field(ctx, parts, "x"), d = Field(ctx, parts, "d");
    if (isPrivate) key.reset(EVP_PKEY_new_raw_private_key(type, nullptr, d.data(), d.size()));
    else key.reset(EVP_PKEY_new_raw_public_key(type, nullptr, x.data(), x.size()));
  }
  if (!key) return qe::Undefined();
  return Result(ctx, isPrivate ? PrivateDer(key.get()) : PublicDer(key.get()));
}

// __solarCryptoEcRaw(spkiDer): the uncompressed point of an EC public key; __solarCryptoEcFromRaw(curve, point): the SPKI of one.
Value EcRaw(Context& ctx, Value, qe::Args args, Value) {
  PKey key = ParsePublic(BytesAt(args, 0));
  if (!key) return qe::Undefined();
  uint8_t buffer[200];
  size_t length = 0;
  if (EVP_PKEY_get_octet_string_param(key.get(), OSSL_PKEY_PARAM_PUB_KEY, buffer, sizeof buffer, &length)) return Result(ctx, Bytes(buffer, buffer + length));
  length = sizeof buffer;
  if (EVP_PKEY_get_raw_public_key(key.get(), buffer, &length)) return Result(ctx, Bytes(buffer, buffer + length));
  return qe::Undefined();
}

Value EcFromRaw(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 3) return qe::Undefined();
  const std::string kind = qe::ToWtf8(ctx, args[0]);
  const auto point = BytesAt(args, 2);
  PKey key;
  if (kind == "EC") {
    const char* curve = CurveOf(qe::ToWtf8(ctx, args[1]));
    if (!curve) return qe::Undefined();
    OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
    OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, curve, 0);
    OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size());
    OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(bld);
    PCtx pc(EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr));
    EVP_PKEY* raw = nullptr;
    if (pc && params && EVP_PKEY_fromdata_init(pc.get()) > 0) EVP_PKEY_fromdata(pc.get(), &raw, EVP_PKEY_PUBLIC_KEY, params);
    key.reset(raw);
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(bld);
  } else {
    key.reset(EVP_PKEY_new_raw_public_key(kind == "Ed25519" ? EVP_PKEY_ED25519 : EVP_PKEY_X25519, nullptr, point.data(), point.size()));
  }
  if (!key) return qe::Undefined();
  return Result(ctx, PublicDer(key.get()));
}

// ECDSA gives and takes a DER signature; Web Crypto's is r then s, each as long as the curve's order.
bool DerToRaw(const Bytes& der, int size, Bytes& out) {
  const uint8_t* p = der.data();
  ECDSA_SIG* sig = d2i_ECDSA_SIG(nullptr, &p, static_cast<long>(der.size()));
  if (!sig) return false;
  const BIGNUM *r, *s;
  ECDSA_SIG_get0(sig, &r, &s);
  out.assign(size * 2, 0);
  const bool good = BN_bn2binpad(r, out.data(), size) >= 0 && BN_bn2binpad(s, out.data() + size, size) >= 0;
  ECDSA_SIG_free(sig);
  return good;
}

bool RawToDer(std::span<const uint8_t> raw, int size, Bytes& out) {
  if (raw.size() != static_cast<size_t>(size) * 2) return false;
  ECDSA_SIG* sig = ECDSA_SIG_new();
  BIGNUM* r = BN_bin2bn(raw.data(), size, nullptr);
  BIGNUM* s = BN_bin2bn(raw.data() + size, size, nullptr);
  if (!sig || !r || !s || !ECDSA_SIG_set0(sig, r, s)) { ECDSA_SIG_free(sig); BN_free(r); BN_free(s); return false; }
  const int n = i2d_ECDSA_SIG(sig, nullptr);
  out.resize(n);
  uint8_t* p = out.data();
  i2d_ECDSA_SIG(sig, &p);
  ECDSA_SIG_free(sig);
  return true;
}

// __solarCryptoSign(scheme, hash, privateDer, data, saltLength): "ECDSA", "RSASSA-PKCS1-v1_5", "RSA-PSS" or "Ed25519".
Value Sign(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 4) return qe::Undefined();
  const std::string scheme = qe::ToWtf8(ctx, args[0]);
  const EVP_MD* md = scheme == "Ed25519" ? nullptr : HashOf(qe::ToWtf8(ctx, args[1]));
  if (scheme != "Ed25519" && !md) return qe::Undefined();
  PKey key = ParsePrivate(BytesAt(args, 2));
  if (!key) return qe::Undefined();
  const auto data = BytesAt(args, 3);
  MdCtx mc(EVP_MD_CTX_new());
  EVP_PKEY_CTX* pc = nullptr;
  if (EVP_DigestSignInit(mc.get(), &pc, md, nullptr, key.get()) <= 0) return qe::Undefined();
  if (scheme == "RSA-PSS") {
    if (EVP_PKEY_CTX_set_rsa_padding(pc, RSA_PKCS1_PSS_PADDING) <= 0 || EVP_PKEY_CTX_set_rsa_pss_saltlen(pc, args.size() > 4 ? static_cast<int>(qe::ToNumber(ctx, args[4])) : 32) <= 0) return qe::Undefined();
  }
  size_t length = 0;
  if (EVP_DigestSign(mc.get(), nullptr, &length, data.data(), data.size()) <= 0) return qe::Undefined();
  Bytes out(length);
  if (EVP_DigestSign(mc.get(), out.data(), &length, data.data(), data.size()) <= 0) return qe::Undefined();
  out.resize(length);
  if (scheme == "ECDSA") {
    Bytes raw;
    if (!DerToRaw(out, (EVP_PKEY_bits(key.get()) + 7) / 8, raw)) return qe::Undefined();
    out = raw;
  }
  return Result(ctx, out);
}

// __solarCryptoVerify(scheme, hash, publicDer, signature, data, saltLength): true or false (undefined where the key or scheme is not usable).
Value Verify(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 5) return qe::Undefined();
  const std::string scheme = qe::ToWtf8(ctx, args[0]);
  const EVP_MD* md = scheme == "Ed25519" ? nullptr : HashOf(qe::ToWtf8(ctx, args[1]));
  if (scheme != "Ed25519" && !md) return qe::Undefined();
  PKey key = ParsePublic(BytesAt(args, 2));
  if (!key) return qe::Undefined();
  Bytes signature(BytesAt(args, 3).begin(), BytesAt(args, 3).end());
  const auto data = BytesAt(args, 4);
  if (scheme == "ECDSA") {
    Bytes der;
    if (!RawToDer(signature, (EVP_PKEY_bits(key.get()) + 7) / 8, der)) return qe::FromBool(false);
    signature = der;
  }
  MdCtx mc(EVP_MD_CTX_new());
  EVP_PKEY_CTX* pc = nullptr;
  if (EVP_DigestVerifyInit(mc.get(), &pc, md, nullptr, key.get()) <= 0) return qe::Undefined();
  if (scheme == "RSA-PSS") {
    if (EVP_PKEY_CTX_set_rsa_padding(pc, RSA_PKCS1_PSS_PADDING) <= 0 || EVP_PKEY_CTX_set_rsa_pss_saltlen(pc, args.size() > 5 ? static_cast<int>(qe::ToNumber(ctx, args[5])) : 32) <= 0) return qe::Undefined();
  }
  return qe::FromBool(EVP_DigestVerify(mc.get(), signature.data(), signature.size(), data.data(), data.size()) == 1);
}

// __solarCryptoOaep(encrypt, hash, keyDer, data, label): RSA-OAEP with the public key (to encrypt) or the private one (to decrypt).
Value Oaep(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 4) return qe::Undefined();
  const bool encrypt = qe::ToNumber(ctx, args[0]) != 0;
  const EVP_MD* md = HashOf(qe::ToWtf8(ctx, args[1]));
  if (!md) return qe::Undefined();
  PKey key = Parse(BytesAt(args, 2), !encrypt);
  if (!key) return qe::Undefined();
  const auto data = BytesAt(args, 3);
  const auto label = BytesAt(args, 4);
  PCtx pc(EVP_PKEY_CTX_new(key.get(), nullptr));
  if (!pc) return qe::Undefined();
  if ((encrypt ? EVP_PKEY_encrypt_init(pc.get()) : EVP_PKEY_decrypt_init(pc.get())) <= 0) return qe::Undefined();
  if (EVP_PKEY_CTX_set_rsa_padding(pc.get(), RSA_PKCS1_OAEP_PADDING) <= 0 || EVP_PKEY_CTX_set_rsa_oaep_md(pc.get(), md) <= 0 || EVP_PKEY_CTX_set_rsa_mgf1_md(pc.get(), md) <= 0) return qe::Undefined();
  if (!label.empty()) {
    uint8_t* copy = static_cast<uint8_t*>(OPENSSL_malloc(label.size()));
    std::memcpy(copy, label.data(), label.size());
    if (EVP_PKEY_CTX_set0_rsa_oaep_label(pc.get(), copy, static_cast<int>(label.size())) <= 0) { OPENSSL_free(copy); return qe::Undefined(); }
  }
  size_t length = 0;
  if ((encrypt ? EVP_PKEY_encrypt(pc.get(), nullptr, &length, data.data(), data.size()) : EVP_PKEY_decrypt(pc.get(), nullptr, &length, data.data(), data.size())) <= 0) return qe::Undefined();
  Bytes out(length);
  if ((encrypt ? EVP_PKEY_encrypt(pc.get(), out.data(), &length, data.data(), data.size()) : EVP_PKEY_decrypt(pc.get(), out.data(), &length, data.data(), data.size())) <= 0) return qe::Undefined();
  out.resize(length);
  return Result(ctx, out);
}

// __solarCryptoEcdh(privateDer, publicDer): the shared secret of ECDH or X25519.
Value Ecdh(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 2) return qe::Undefined();
  PKey mine = ParsePrivate(BytesAt(args, 0));
  PKey theirs = ParsePublic(BytesAt(args, 1));
  if (!mine || !theirs) return qe::Undefined();
  PCtx pc(EVP_PKEY_CTX_new(mine.get(), nullptr));
  if (!pc || EVP_PKEY_derive_init(pc.get()) <= 0 || EVP_PKEY_derive_set_peer(pc.get(), theirs.get()) <= 0) return qe::Undefined();
  size_t length = 0;
  if (EVP_PKEY_derive(pc.get(), nullptr, &length) <= 0) return qe::Undefined();
  Bytes out(length);
  if (EVP_PKEY_derive(pc.get(), out.data(), &length) <= 0) return qe::Undefined();
  out.resize(length);
  return Result(ctx, out);
}

}  // namespace

void InstallCryptoKeyNatives(qe::Realm& realm) {
  Context& ctx = realm.GetContext();
  qe::DefineGlobalFunction(ctx, "__solarCryptoGenerate", Generate, 3);
  qe::DefineGlobalFunction(ctx, "__solarCryptoInfo", Info, 2);
  qe::DefineGlobalFunction(ctx, "__solarCryptoPublicOf", PublicOf, 1);
  qe::DefineGlobalFunction(ctx, "__solarCryptoParts", Parts, 2);
  qe::DefineGlobalFunction(ctx, "__solarCryptoFromParts", FromParts, 4);
  qe::DefineGlobalFunction(ctx, "__solarCryptoEcRaw", EcRaw, 1);
  qe::DefineGlobalFunction(ctx, "__solarCryptoEcFromRaw", EcFromRaw, 3);
  qe::DefineGlobalFunction(ctx, "__solarCryptoSign", Sign, 5);
  qe::DefineGlobalFunction(ctx, "__solarCryptoVerify", Verify, 6);
  qe::DefineGlobalFunction(ctx, "__solarCryptoOaep", Oaep, 5);
  qe::DefineGlobalFunction(ctx, "__solarCryptoEcdh", Ecdh, 2);
}

}  // namespace solar::webapi
