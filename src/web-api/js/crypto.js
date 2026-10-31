// Web Crypto (https://w3c.github.io/webcrypto/): crypto.getRandomValues, randomUUID and crypto.subtle. The algorithms are OpenSSL's, reached by the natives of
// src/web-api/Crypto.cpp; what is here is the interface: names and usages checked, keys held, promises settled, JWK read and written.
// Done: SHA digests, HMAC, AES-CBC/CTR/GCM/KW, PBKDF2, HKDF, RSASSA-PKCS1-v1_5, RSA-PSS, RSA-OAEP, ECDSA, ECDH (P-256/384/521), Ed25519, X25519; key import/export (raw, spki, pkcs8, jwk), generateKey, deriveKey, wrapKey/unwrapKey.
(() => {
  "use strict";
  const digestN = globalThis.__solarCryptoDigest, hmacN = globalThis.__solarCryptoHmac, aesN = globalThis.__solarCryptoAes;
  const pbkdf2N = globalThis.__solarCryptoPbkdf2, hkdfN = globalThis.__solarCryptoHkdf, randomN = globalThis.__solarCryptoRandom;
  const N = {};
  for (const n of ["Generate", "Info", "PublicOf", "Parts", "FromParts", "EcRaw", "EcFromRaw", "Sign", "Verify", "Oaep", "Ecdh"]) { N[n] = globalThis["__solarCrypto" + n]; delete globalThis["__solarCrypto" + n]; }
  for (const n of ["Digest", "Hmac", "Aes", "Pbkdf2", "Hkdf", "Random"]) delete globalThis["__solarCrypto" + n];
  if (typeof randomN !== "function") return;

  const define = (target, name, value) => Object.defineProperty(target, name, { value, writable: true, configurable: true, enumerable: false });
  const fail = (name, message) => new DOMException(message, name);

  // ---- Bytes ----
  const copy = (data, what = "data") => {
    if (data instanceof ArrayBuffer || (typeof SharedArrayBuffer === "function" && data instanceof SharedArrayBuffer)) return new Uint8Array(data.slice(0));
    if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength));
    throw new TypeError(`The ${what} provided is not an ArrayBuffer or ArrayBufferView.`);
  };
  const b64 = (bytes) => { let s = ""; for (let i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]); return btoa(s); };
  const b64url = (bytes) => b64(bytes).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
  const unb64url = (text) => {
    let s = String(text).replace(/-/g, "+").replace(/_/g, "/");
    while (s.length % 4) s += "=";
    let bin;
    try { bin = atob(s); } catch (e) { throw fail("DataError", "The JWK member is not valid base64url."); }
    const out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
  };

  // ---- Random ----
  const fill = (array) => {
    if (!ArrayBuffer.isView(array) || array instanceof Float32Array || array instanceof Float64Array || array instanceof DataView) {
      throw fail("TypeMismatchError", "The data argument must be an integer-type TypedArray.");
    }
    if (array.byteLength > 65536) throw fail("QuotaExceededError", `The ArrayBufferView's byte length (${array.byteLength}) exceeds the number of bytes of entropy available via this API (65536).`);
    if (array.byteLength) new Uint8Array(array.buffer, array.byteOffset, array.byteLength).set(new Uint8Array(randomN(array.byteLength)));
    return array;
  };
  const uuid = () => {
    const b = new Uint8Array(randomN(16));
    b[6] = (b[6] & 15) | 64;
    b[8] = (b[8] & 63) | 128;
    const h = Array.from(b, (x) => x.toString(16).padStart(2, "0")).join("");
    return `${h.slice(0, 8)}-${h.slice(8, 12)}-${h.slice(12, 16)}-${h.slice(16, 20)}-${h.slice(20)}`;
  };

  // ---- Algorithms ----
  const HASHES = { "SHA-1": "SHA-1", "SHA-256": "SHA-256", "SHA-384": "SHA-384", "SHA-512": "SHA-512" };
  const NAMES = {
    "SHA-1": "digest", "SHA-256": "digest", "SHA-384": "digest", "SHA-512": "digest",
    "HMAC": "hmac", "AES-CBC": "aes", "AES-CTR": "aes", "AES-GCM": "aes", "AES-KW": "aes", "PBKDF2": "kdf", "HKDF": "kdf",
    "RSASSA-PKCS1-v1_5": "rsa", "RSA-PSS": "rsa", "RSA-OAEP": "rsa", "ECDSA": "ec", "ECDH": "ec", "Ed25519": "okp", "X25519": "okp",
  };
  const asymmetric = (kind) => kind === "rsa" || kind === "ec" || kind === "okp";
  const canonical = (name) => {
    const upper = String(name).toUpperCase();
    for (const known of Object.keys(NAMES)) if (known.toUpperCase() === upper) return known;
    return null;
  };
  const normalize = (algorithm, operation) => {
    let given = algorithm;
    if (typeof algorithm === "string") given = { name: algorithm };
    else if (algorithm === null || typeof algorithm !== "object") throw new TypeError("The algorithm is not an object or a string.");
    if (given.name === undefined) throw new TypeError("The algorithm is not a dictionary with a name.");
    const name = canonical(given.name);
    if (!name) throw fail("NotSupportedError", "Unrecognized name.");
    const out = { name };
    for (const key of Object.keys(given)) out[key] = given[key];
    out.name = name;
    if (out.hash !== undefined) {
      const h = typeof out.hash === "string" ? out.hash : out.hash && out.hash.name;
      const hn = h && HASHES[String(h).toUpperCase()];
      if (!hn) throw fail("NotSupportedError", "Unrecognized hash name.");
      out.hash = { name: hn };
    }
    void operation;
    return out;
  };
  const hashName = (alg) => alg.hash.name;
  const hashBits = { "SHA-1": 160, "SHA-256": 256, "SHA-384": 384, "SHA-512": 512 };

  // ---- Keys ----
  const secret = new WeakMap();
  const token = Symbol("CryptoKey");
  class CryptoKey {
    #type; #extractable; #algorithm; #usages;
    constructor(t, type, extractable, algorithm, usages, bytes) {
      if (t !== token) throw new TypeError("Illegal constructor");
      this.#type = type; this.#extractable = extractable; this.#algorithm = algorithm; this.#usages = Object.freeze(usages.slice());
      secret.set(this, bytes);
    }
    get type() { return this.#type; }
    get extractable() { return this.#extractable; }
    get algorithm() { return this.#algorithm; }
    get usages() { return this.#usages.slice(); }
  }
  Object.defineProperty(CryptoKey.prototype, Symbol.toStringTag, { value: "CryptoKey", configurable: true });
  const ALL_USAGES = ["encrypt", "decrypt", "sign", "verify", "deriveKey", "deriveBits", "wrapKey", "unwrapKey"];
  const allowed = { aes: ["encrypt", "decrypt", "wrapKey", "unwrapKey"], hmac: ["sign", "verify"], kdf: ["deriveKey", "deriveBits"] };
  const checkUsages = (kind, name, usages, mine) => {
    const list = Array.from(usages || [], String);
    for (const u of list) {
      if (!ALL_USAGES.includes(u)) throw new TypeError(`The provided value '${u}' is not a valid enum value of type KeyUsage.`);
      const permitted = name === "AES-KW" ? ["wrapKey", "unwrapKey"] : allowed[kind];
      if (!permitted.includes(u)) throw fail("SyntaxError", "Unsupported key usage for " + name + " key.");
    }
    return Array.from(new Set(list));
  };
  const keyOf = (key, usage, name) => {
    if (!(key instanceof CryptoKey)) throw new TypeError("Parameter 1 is not of type 'CryptoKey'.");
    if (name && key.algorithm.name !== name) throw fail("InvalidAccessError", "The key is not for this algorithm.");
    if (usage && !key.usages.includes(usage)) throw fail("InvalidAccessError", `The key does not support the '${usage}' operation.`);
    return key;
  };
  const makeKey = (type, extractable, algorithm, usages, bytes) => new CryptoKey(token, type, !!extractable, algorithm, usages, bytes);

  // ---- The operations ----
  const need = (result) => { if (result === undefined) throw fail("OperationError", "The operation failed for an operation-specific reason"); return result; };

  const aesParams = (alg, op) => {
    if (alg.name === "AES-CBC") {
      const iv = copy(alg.iv, "iv");
      if (iv.length !== 16) throw fail("OperationError", "The iv must be 16 bytes long.");
      return { iv, aad: new Uint8Array(0), tag: 16 };
    }
    if (alg.name === "AES-CTR") {
      const counter = copy(alg.counter, "counter");
      if (counter.length !== 16) throw fail("OperationError", "The counter must be 16 bytes long.");
      const length = alg.length;
      if (!(length >= 1 && length <= 128)) throw fail("OperationError", "The length must be between 1 and 128.");
      return { iv: counter, aad: new Uint8Array(0), tag: 16 };
    }
    if (alg.name === "AES-GCM") {
      const iv = copy(alg.iv, "iv");
      const tag = alg.tagLength === undefined ? 128 : alg.tagLength;
      if (![32, 64, 96, 104, 112, 120, 128].includes(tag)) throw fail("OperationError", "Invalid tag length.");
      return { iv, aad: alg.additionalData === undefined ? new Uint8Array(0) : copy(alg.additionalData, "additionalData"), tag: tag / 8 };
    }
    void op;
    return { iv: new Uint8Array(0), aad: new Uint8Array(0), tag: 16 };
  };
  // AES-CTR counts in the low `length` bits of the counter block only, and wraps there without carrying into the rest; OpenSSL counts in all 128. So the data is cut where
  // the low bits wrap, and each piece is run with the counter as it is by then.
  const aesCtr = (alg, key, data) => {
    const counter = copy(alg.counter, "counter");
    const bits = alg.length;
    if (bits >= 128) return need(aesN(alg.name, 1, secret.get(key), counter, data, new Uint8Array(0), 16));
    const low = (c) => { let v = 0n; for (let i = 0; i < 16; i++) v = (v << 8n) | BigInt(c[i]); return v & ((1n << BigInt(bits)) - 1n); };
    const clear = (c) => {
      const out = new Uint8Array(c);
      for (let i = 0; i < bits; i++) out[15 - (i >> 3)] &= ~(1 << (i & 7));
      return out;
    };
    const room = 1n << BigInt(bits);
    const parts = [];
    let rest = data, current = counter;
    while (rest.length) {
      const blocks = BigInt(Math.ceil(rest.length / 16));
      const until = room - low(current);
      const take = blocks <= until ? rest.length : Number(until) * 16;
      parts.push(new Uint8Array(need(aesN(alg.name, 1, secret.get(key), current, rest.subarray(0, take), new Uint8Array(0), 16))));
      rest = rest.subarray(take);
      current = clear(current);
    }
    const out = new Uint8Array(data.length);
    let at = 0;
    for (const part of parts) { out.set(part, at); at += part.length; }
    return out.buffer;
  };
  const aesRun = (alg, key, data, encrypt) => {
    if (alg.name === "AES-CTR") {
      const length = alg.length;
      if (!(length >= 1 && length <= 128)) throw fail("OperationError", "The length must be between 1 and 128.");
      if (copy(alg.counter, "counter").length !== 16) throw fail("OperationError", "The counter must be 16 bytes long.");
      return aesCtr(alg, key, data);
    }
    const p = aesParams(alg, encrypt);
    return need(aesN(alg.name, encrypt ? 1 : 0, secret.get(key), p.iv, data, p.aad, p.tag));
  };
  const kdfBits = (alg, key, length) => {
    if (length === null || length === undefined || length % 8 !== 0) throw fail("OperationError", "The length provided for deriveBits is not a multiple of 8.");
    const bytes = length / 8;
    if (alg.name === "PBKDF2") {
      if (!alg.hash) throw new TypeError("PBKDF2 needs a hash.");
      if (!(alg.iterations >= 1)) throw fail("OperationError", "Iterations must be at least 1.");
      return need(pbkdf2N(hashName(alg), secret.get(key), copy(alg.salt, "salt"), alg.iterations, bytes));
    }
    if (!alg.hash) throw new TypeError("HKDF needs a hash.");
    if (bytes > 255 * hashBits[hashName(alg)] / 8) throw fail("OperationError", "The length is too long for HKDF.");
    return need(hkdfN(hashName(alg), secret.get(key), copy(alg.salt, "salt"), copy(alg.info, "info"), bytes));
  };

  const keyLength = (alg) => {
    if (alg.name === "HMAC") return alg.length !== undefined ? alg.length : hashBits[hashName(alg)] === 160 ? 512 : hashBits[hashName(alg)] <= 256 ? 512 : 1024;
    if (alg.length !== 128 && alg.length !== 192 && alg.length !== 256) throw fail("OperationError", "AES key length must be 128, 192 or 256 bits.");
    return alg.length;
  };

  const importKeyData = (format, keyData, alg, extractable, usages) => {
    const kind = NAMES[alg.name];
    if (asymmetric(kind)) return importAsym(format, keyData, alg, extractable, usages);
    if (kind === "digest") throw fail("NotSupportedError", "Unsupported algorithm.");
    let bytes;
    if (format === "raw") {
      bytes = copy(keyData, "keyData");
    } else if (format === "jwk") {
      if (keyData === null || typeof keyData !== "object") throw new TypeError("The JWK is not an object.");
      if (keyData.kty !== "oct") throw fail("DataError", "The JWK's kty must be 'oct'.");
      if (typeof keyData.k !== "string") throw fail("DataError", "The JWK has no k.");
      if (kind === "kdf") throw fail("NotSupportedError", "Unsupported import key format for this algorithm.");
      if (keyData.use !== undefined && keyData.use !== "enc" && alg.name !== "HMAC") throw fail("DataError", "The JWK's use does not fit.");
      if (keyData.key_ops !== undefined && usages.some((u) => !keyData.key_ops.includes(u))) throw fail("DataError", "The JWK's key_ops do not allow the usages.");
      if (keyData.ext === false && extractable) throw fail("DataError", "The JWK is not extractable.");
      bytes = unb64url(keyData.k);
      if (alg.name === "HMAC") {
        const expected = { "SHA-1": "HS1", "SHA-256": "HS256", "SHA-384": "HS384", "SHA-512": "HS512" }[hashName(alg)];
        if (keyData.alg !== undefined && keyData.alg !== expected) throw fail("DataError", "The JWK's alg does not fit.");
      } else if (kind === "aes") {
        const expected = { "AES-CBC": "CBC", "AES-CTR": "CTR", "AES-GCM": "GCM", "AES-KW": "KW" }[alg.name] ;
        if (keyData.alg !== undefined && keyData.alg !== `A${bytes.length * 8}${expected}`) throw fail("DataError", "The JWK's alg does not fit.");
      }
    } else {
      throw fail("NotSupportedError", `Unsupported import key format '${format}' for ${alg.name}.`);
    }
    if (kind === "kdf") {
      if (extractable) throw new SyntaxError("KDF keys must not be extractable.");
      return makeKey("secret", false, { name: alg.name }, checkUsages(kind, alg.name, usages), bytes);
    }
    const list = checkUsages(kind, alg.name, usages);
    if (list.length === 0) throw new SyntaxError("Usages cannot be empty when importing a secret key.");
    if (kind === "aes") {
      if (![16, 24, 32].includes(bytes.length)) throw fail("DataError", "Invalid AES key length.");
      return makeKey("secret", extractable, { name: alg.name, length: bytes.length * 8 }, list, bytes);
    }
    if (bytes.length === 0) throw fail("DataError", "Zero-length key is not supported.");
    let length = bytes.length * 8;
    if (alg.length !== undefined) {
      if (alg.length > length || alg.length <= length - 8) throw fail("DataError", "The key length does not match the data.");
      length = alg.length;
    }
    return makeKey("secret", extractable, { name: "HMAC", hash: { name: hashName(alg) }, length }, list, bytes);
  };

  const exportKeyData = (format, key) => {
    if (asymmetric(NAMES[key.algorithm.name])) return exportAsym(format, key);
    if (!key.extractable) throw fail("InvalidAccessError", "key is not extractable");
    const bytes = secret.get(key);
    const raw = bytes.slice().buffer;
    if (format === "raw") return raw;
    if (format === "jwk") {
      const jwk = { kty: "oct", k: b64url(bytes), ext: true, key_ops: key.usages };
      if (key.algorithm.name === "HMAC") jwk.alg = { "SHA-1": "HS1", "SHA-256": "HS256", "SHA-384": "HS384", "SHA-512": "HS512" }[key.algorithm.hash.name];
      else jwk.alg = `A${bytes.length * 8}${{ "AES-CBC": "CBC", "AES-CTR": "CTR", "AES-GCM": "GCM", "AES-KW": "KW" }[key.algorithm.name]}`;
      return jwk;
    }
    throw fail("NotSupportedError", `Unsupported export key format '${format}'.`);
  };


  // ---- Public-key algorithms (the keys are held as DER: PKCS#8 for a private one, SPKI for a public one) ----
  const USAGES = {
    "RSASSA-PKCS1-v1_5": [["verify"], ["sign"]], "RSA-PSS": [["verify"], ["sign"]], "RSA-OAEP": [["encrypt", "wrapKey"], ["decrypt", "unwrapKey"]],
    "ECDSA": [["verify"], ["sign"]], "Ed25519": [["verify"], ["sign"]], "ECDH": [[], ["deriveKey", "deriveBits"]], "X25519": [[], ["deriveKey", "deriveBits"]],
  };
  const CURVES = ["P-256", "P-384", "P-521"];
  const jwkHash = { "SHA-1": "", "SHA-256": "256", "SHA-384": "384", "SHA-512": "512" };
  const asymList = (name, usages, isPrivate) => {
    const list = Array.from(new Set(Array.from(usages || [], String)));
    for (const u of list) {
      if (!ALL_USAGES.includes(u)) throw new TypeError(`The provided value '${u}' is not a valid enum value of type KeyUsage.`);
      if (!USAGES[name][isPrivate ? 1 : 0].includes(u)) throw fail("SyntaxError", "Unsupported key usage for " + name + " key.");
    }
    return list;
  };
  const describe = (alg, info) => {
    if (info.kind === "RSA") return { name: alg.name, modulusLength: info.bits, publicExponent: unb64url(info.e), hash: { name: alg.hash.name } };
    if (info.kind === "EC") return { name: alg.name, namedCurve: info.curve };
    return { name: alg.name };
  };
  const wantKind = (kind) => (kind === "rsa" ? "RSA" : kind === "ec" ? "EC" : null);
  const checkRsaHash = (alg) => { if (!alg.hash) throw new TypeError(`${alg.name} needs a hash.`); };
  const infoOf = (der, isPrivate) => { const t = N.Info(der, isPrivate ? 1 : 0); if (t === undefined) throw fail("DataError", "The key data is not a key this supports."); return JSON.parse(t); };
  const matches = (alg, info) => {
    const kind = NAMES[alg.name];
    if (kind === "rsa") return info.kind === "RSA";
    if (kind === "ec") return info.kind === "EC" && info.curve === alg.namedCurve;
    return info.kind === alg.name;
  };

  const importAsym = (format, keyData, alg, extractable, usages) => {
    const kind = NAMES[alg.name];
    if (kind === "rsa") checkRsaHash(alg);
    if (kind === "ec" && !CURVES.includes(alg.namedCurve)) throw fail("NotSupportedError", "Unsupported named curve.");
    let der, isPrivate, info;
    if (format === "spki" || format === "pkcs8") {
      isPrivate = format === "pkcs8";
      der = copy(keyData, "keyData");
      info = infoOf(der, isPrivate);
    } else if (format === "raw") {
      if (kind === "rsa") throw fail("NotSupportedError", "Unsupported import key format 'raw'.");
      isPrivate = false;
      const point = copy(keyData, "keyData");
      der = N.EcFromRaw(kind === "ec" ? "EC" : alg.name, alg.namedCurve, point);
      if (der === undefined) throw fail("DataError", "The raw key is not valid.");
      der = new Uint8Array(der);
      info = infoOf(der, false);
    } else if (format === "jwk") {
      if (keyData === null || typeof keyData !== "object") throw new TypeError("The JWK is not an object.");
      isPrivate = keyData.d !== undefined;
      const ktyWanted = kind === "rsa" ? "RSA" : kind === "ec" ? "EC" : "OKP";
      if (keyData.kty !== ktyWanted) throw fail("DataError", `The JWK's kty must be '${ktyWanted}'.`);
      if (keyData.ext === false && extractable) throw fail("DataError", "The JWK is not extractable.");
      if (keyData.key_ops !== undefined && usages.some((u) => !keyData.key_ops.includes(u))) throw fail("DataError", "The JWK's key_ops do not allow the usages.");
      if (kind === "ec" && keyData.crv !== alg.namedCurve) throw fail("DataError", "The JWK's crv does not fit.");
      if (kind === "okp" && keyData.crv !== alg.name) throw fail("DataError", "The JWK's crv does not fit.");
      for (const part of kind === "rsa" ? ["n", "e"] : kind === "ec" ? ["x", "y"] : ["x"]) if (typeof keyData[part] !== "string") throw fail("DataError", `The JWK has no ${part}.`);
      if (kind === "rsa" && isPrivate) for (const part of ["d", "p", "q", "dp", "dq", "qi"]) if (typeof keyData[part] !== "string") throw fail("DataError", `The JWK has no ${part}.`);
      der = N.FromParts(wantKind(kind) || alg.name, alg.namedCurve, keyData, isPrivate ? 1 : 0);
      if (der === undefined) throw fail("DataError", "The JWK is not a valid key.");
      der = new Uint8Array(der);
      info = infoOf(der, isPrivate);
    } else {
      throw fail("NotSupportedError", `Unsupported import key format '${format}'.`);
    }
    if (!matches(alg, info)) throw fail("DataError", "The key does not fit the algorithm.");
    const list = asymList(alg.name, usages, isPrivate);
    if (isPrivate && list.length === 0) throw new SyntaxError("Usages cannot be empty when importing a private key.");
    return makeKey(isPrivate ? "private" : "public", extractable, describe(alg, info), list, der);
  };

  const exportAsym = (format, key) => {
    if (!key.extractable) throw fail("InvalidAccessError", "key is not extractable");
    const der = secret.get(key);
    const isPrivate = key.type === "private";
    if (format === "spki") { if (isPrivate) throw fail("InvalidAccessError", "Cannot export a private key as spki."); return der.slice().buffer; }
    if (format === "pkcs8") { if (!isPrivate) throw fail("InvalidAccessError", "Cannot export a public key as pkcs8."); return der.slice().buffer; }
    if (format === "raw") {
      if (isPrivate || NAMES[key.algorithm.name] === "rsa") throw fail("InvalidAccessError", "Cannot export this key as raw.");
      return need(N.EcRaw(der));
    }
    if (format === "jwk") {
      const parts = JSON.parse(need(N.Parts(der, isPrivate ? 1 : 0)));
      const a = key.algorithm, kind = NAMES[a.name];
      const jwk = { kty: kind === "rsa" ? "RSA" : kind === "ec" ? "EC" : "OKP" };
      if (kind === "rsa") {
        const n = jwkHash[a.hash.name];
        jwk.alg = a.name === "RSASSA-PKCS1-v1_5" ? "RS" + (n || "1") : a.name === "RSA-PSS" ? "PS" + (n || "1") : "RSA-OAEP" + (n ? "-" + n : "");
      } else {
        jwk.crv = kind === "ec" ? a.namedCurve : a.name;
      }
      Object.assign(jwk, parts);
      jwk.ext = true;
      jwk.key_ops = key.usages;
      return jwk;
    }
    throw fail("NotSupportedError", `Unsupported export key format '${format}'.`);
  };

  const generateAsym = (alg, extractable, keyUsages) => {
    const kind = NAMES[alg.name];
    let made, info;
    if (kind === "rsa") {
      checkRsaHash(alg);
      if (!(alg.modulusLength >= 256 && alg.modulusLength <= 16384)) throw fail("OperationError", "The modulus length is out of range.");
      const exponent = alg.publicExponent === undefined ? new Uint8Array([1, 0, 1]) : copy(alg.publicExponent, "publicExponent");
      made = N.Generate("RSA", alg.modulusLength, exponent);
    } else if (kind === "ec") {
      if (!CURVES.includes(alg.namedCurve)) throw fail("NotSupportedError", "Unsupported named curve.");
      made = N.Generate("EC", alg.namedCurve);
    } else {
      made = N.Generate(alg.name, "");
    }
    if (made === undefined) throw fail("OperationError", "Key generation failed.");
    const priv = new Uint8Array(made[0]), pub = new Uint8Array(made[1]);
    info = infoOf(pub, false);
    const usages = Array.from(new Set(Array.from(keyUsages || [], String)));
    for (const u of usages) {
      if (!ALL_USAGES.includes(u)) throw new TypeError(`The provided value '${u}' is not a valid enum value of type KeyUsage.`);
      if (!USAGES[alg.name][0].includes(u) && !USAGES[alg.name][1].includes(u)) throw fail("SyntaxError", "Unsupported key usage for " + alg.name + " key.");
    }
    const privateUsages = usages.filter((u) => USAGES[alg.name][1].includes(u));
    if (privateUsages.length === 0) throw new SyntaxError("Usages cannot be empty when creating a key.");
    const description = describe(alg, info);
    return {
      publicKey: makeKey("public", true, description, usages.filter((u) => USAGES[alg.name][0].includes(u)), pub),
      privateKey: makeKey("private", !!extractable, description, privateUsages, priv),
    };
  };

  const signAsym = (alg, key, data) => {
    const kind = NAMES[alg.name];
    keyOf(key, "sign", alg.name);
    if (key.type !== "private") throw fail("InvalidAccessError", "The key is not a private key.");
    let hash = "";
    if (kind === "rsa") hash = key.algorithm.hash.name;
    else if (alg.name === "ECDSA") { if (!alg.hash) throw new TypeError("ECDSA needs a hash."); hash = alg.hash.name; }
    return need(N.Sign(alg.name, hash, secret.get(key), data, alg.saltLength === undefined ? 32 : alg.saltLength));
  };
  const verifyAsym = (alg, key, signature, data) => {
    const kind = NAMES[alg.name];
    keyOf(key, "verify", alg.name);
    if (key.type !== "public") throw fail("InvalidAccessError", "The key is not a public key.");
    let hash = "";
    if (kind === "rsa") hash = key.algorithm.hash.name;
    else if (alg.name === "ECDSA") { if (!alg.hash) throw new TypeError("ECDSA needs a hash."); hash = alg.hash.name; }
    const result = N.Verify(alg.name, hash, secret.get(key), signature, data, alg.saltLength === undefined ? 32 : alg.saltLength);
    if (result === undefined) throw fail("OperationError", "The operation failed for an operation-specific reason");
    return result;
  };
  const oaep = (alg, key, data, encrypt) => {
    keyOf(key, null, "RSA-OAEP");
    if (key.type !== (encrypt ? "public" : "private")) throw fail("InvalidAccessError", `The key is not a ${encrypt ? "public" : "private"} key.`);
    return need(N.Oaep(encrypt ? 1 : 0, key.algorithm.hash.name, secret.get(key), data, alg.label === undefined ? new Uint8Array(0) : copy(alg.label, "label")));
  };
  const ecdhBits = (alg, key, length) => {
    keyOf(key, "deriveBits", alg.name);
    if (key.type !== "private") throw fail("InvalidAccessError", "The key is not a private key.");
    const peer = alg.public;
    if (!(peer instanceof CryptoKey) || peer.type !== "public" || peer.algorithm.name !== alg.name) throw fail("InvalidAccessError", "The public key is not a public " + alg.name + " key.");
    if (alg.name === "ECDH" && peer.algorithm.namedCurve !== key.algorithm.namedCurve) throw fail("InvalidAccessError", "The curves differ.");
    const shared = new Uint8Array(need(N.Ecdh(secret.get(key), secret.get(peer))));
    if (length === null || length === undefined) return shared.buffer;
    if (length > shared.length * 8) throw fail("OperationError", "The length is more than the secret has.");
    return shared.slice(0, Math.ceil(length / 8)).buffer;
  };

  const oaepUnwrap = (alg, key, data) => { keyOf(key, "unwrapKey", alg.name); return need(N.Oaep(0, key.algorithm.hash.name, secret.get(key), data, alg.label === undefined ? new Uint8Array(0) : copy(alg.label, "label"))); };
  const promised = (f) => new Promise((resolve) => resolve(f()));
  const need1 = (args, n, what) => { if (args.length < n) throw new TypeError(`Failed to execute '${what}' on 'SubtleCrypto': ${n} arguments required, but only ${args.length} present.`); };

  class SubtleCrypto {
    constructor() { throw new TypeError("Illegal constructor"); }
    digest(algorithm, data) {
      need1(arguments, 2, "digest");
      return promised(() => {
        const alg = normalize(algorithm);
        if (NAMES[alg.name] !== "digest") throw fail("NotSupportedError", "Unrecognized name.");
        return need(digestN(alg.name, copy(data)));
      });
    }
    importKey(format, keyData, algorithm, extractable, keyUsages) {
      need1(arguments, 5, "importKey");
      return promised(() => importKeyData(String(format), keyData, normalize(algorithm), !!extractable, Array.from(keyUsages || [], String)));
    }
    exportKey(format, key) {
      need1(arguments, 2, "exportKey");
      return promised(() => exportKeyData(String(format), keyOf(key)));
    }
    generateKey(algorithm, extractable, keyUsages) {
      need1(arguments, 3, "generateKey");
      return promised(() => {
        const alg = normalize(algorithm);
        const kind = NAMES[alg.name];
        if (asymmetric(kind)) return generateAsym(alg, extractable, keyUsages);
        if (kind !== "aes" && kind !== "hmac") throw fail("NotSupportedError", "Unsupported algorithm.");
        if (kind === "hmac" && !alg.hash) throw new TypeError("HMAC needs a hash.");
        const usages = checkUsages(kind, alg.name, keyUsages);
        if (usages.length === 0) throw new SyntaxError("Usages cannot be empty when creating a key.");
        const length = keyLength(alg);
        const bytes = new Uint8Array(randomN(Math.ceil(length / 8)));
        const description = kind === "hmac" ? { name: "HMAC", hash: { name: hashName(alg) }, length } : { name: alg.name, length };
        return makeKey("secret", !!extractable, description, usages, bytes);
      });
    }
    encrypt(algorithm, key, data) { need1(arguments, 3, "encrypt"); return this.#cipher(algorithm, key, data, true); }
    decrypt(algorithm, key, data) { need1(arguments, 3, "decrypt"); return this.#cipher(algorithm, key, data, false); }
    #cipher(algorithm, key, data, encrypt) {
      return promised(() => {
        const alg = normalize(algorithm);
        if (alg.name === "RSA-OAEP") { keyOf(key, encrypt ? "encrypt" : "decrypt", "RSA-OAEP"); return oaep(alg, key, copy(data), encrypt); }
        if (NAMES[alg.name] !== "aes" || alg.name === "AES-KW") throw fail("NotSupportedError", "Unsupported algorithm.");
        keyOf(key, encrypt ? "encrypt" : "decrypt", alg.name);
        return aesRun(alg, key, copy(data), encrypt);
      });
    }
    sign(algorithm, key, data) {
      need1(arguments, 3, "sign");
      return promised(() => {
        const alg = normalize(algorithm);
        if (asymmetric(NAMES[alg.name]) && alg.name !== "RSA-OAEP" && alg.name !== "ECDH" && alg.name !== "X25519") return signAsym(alg, key, copy(data));
        if (alg.name !== "HMAC") throw fail("NotSupportedError", "Unsupported algorithm.");
        keyOf(key, "sign", "HMAC");
        return need(hmacN(key.algorithm.hash.name, secret.get(key), copy(data)));
      });
    }
    verify(algorithm, key, signature, data) {
      need1(arguments, 4, "verify");
      return promised(() => {
        const alg = normalize(algorithm);
        if (asymmetric(NAMES[alg.name]) && alg.name !== "RSA-OAEP" && alg.name !== "ECDH" && alg.name !== "X25519") return verifyAsym(alg, key, copy(signature, "signature"), copy(data));
        if (alg.name !== "HMAC") throw fail("NotSupportedError", "Unsupported algorithm.");
        keyOf(key, "verify", "HMAC");
        const expected = new Uint8Array(need(hmacN(key.algorithm.hash.name, secret.get(key), copy(data))));
        const given = copy(signature, "signature");
        if (expected.length !== given.length) return false;
        let difference = 0;
        for (let i = 0; i < expected.length; i++) difference |= expected[i] ^ given[i];
        return difference === 0;
      });
    }
    deriveBits(algorithm, baseKey, length) {
      need1(arguments, 3, "deriveBits");
      return promised(() => {
        const alg = normalize(algorithm);
        if (alg.name === "ECDH" || alg.name === "X25519") return ecdhBits(alg, baseKey, length);
        if (NAMES[alg.name] !== "kdf") throw fail("NotSupportedError", "Unsupported algorithm.");
        keyOf(baseKey, "deriveBits", alg.name);
        return kdfBits(alg, baseKey, length);
      });
    }
    deriveKey(algorithm, baseKey, derivedKeyType, extractable, keyUsages) {
      need1(arguments, 5, "deriveKey");
      return promised(() => {
        const alg = normalize(algorithm);
        const agreement = alg.name === "ECDH" || alg.name === "X25519";
        if (!agreement && NAMES[alg.name] !== "kdf") throw fail("NotSupportedError", "Unsupported algorithm.");
        if (!agreement) keyOf(baseKey, "deriveKey", alg.name);
        const target = normalize(derivedKeyType);
        const kind = NAMES[target.name];
        if (kind !== "aes" && kind !== "hmac") throw fail("NotSupportedError", "Unsupported derived key algorithm.");
        const bits = kind === "aes" ? keyLength(target) : target.length !== undefined ? target.length : keyLength({ ...target, length: undefined });
        const derived = new Uint8Array(agreement ? ecdhBits(alg, baseKey, Math.ceil(bits / 8) * 8) : kdfBits(alg, baseKey, Math.ceil(bits / 8) * 8));
        return importKeyData("raw", derived.slice(0, Math.ceil(bits / 8)), target, !!extractable, Array.from(keyUsages || [], String));
      });
    }
    wrapKey(format, key, wrappingKey, wrapAlgorithm) {
      need1(arguments, 4, "wrapKey");
      return promised(() => {
        const alg = normalize(wrapAlgorithm);
        if (NAMES[alg.name] !== "aes" && alg.name !== "RSA-OAEP") throw fail("NotSupportedError", "Unsupported algorithm.");
        keyOf(wrappingKey, "wrapKey", alg.name);
        let exported = exportKeyData(String(format), keyOf(key));
        if (String(format) === "jwk") exported = new TextEncoder().encode(JSON.stringify(exported));
        else exported = new Uint8Array(exported);
        if (alg.name === "RSA-OAEP") { keyOf(wrappingKey, "wrapKey", alg.name); return oaep(alg, wrappingKey, exported, true); }
        return aesRun(alg, wrappingKey, exported, true);
      });
    }
    unwrapKey(format, wrappedKey, unwrappingKey, unwrapAlgorithm, unwrappedKeyAlgorithm, extractable, keyUsages) {
      need1(arguments, 7, "unwrapKey");
      return promised(() => {
        const alg = normalize(unwrapAlgorithm);
        if (NAMES[alg.name] !== "aes" && alg.name !== "RSA-OAEP") throw fail("NotSupportedError", "Unsupported algorithm.");
        keyOf(unwrappingKey, "unwrapKey", alg.name);
        const plain = new Uint8Array(alg.name === "RSA-OAEP" ? oaepUnwrap(alg, unwrappingKey, copy(wrappedKey, "wrappedKey")) : aesRun(alg, unwrappingKey, copy(wrappedKey, "wrappedKey"), false));
        let data = plain;
        if (String(format) === "jwk") {
          try { data = JSON.parse(new TextDecoder().decode(plain)); } catch (e) { throw fail("DataError", "The unwrapped key is not JSON."); }
        }
        return importKeyData(String(format), data, normalize(unwrappedKeyAlgorithm), !!extractable, Array.from(keyUsages || [], String));
      });
    }
  }
  Object.defineProperty(SubtleCrypto.prototype, Symbol.toStringTag, { value: "SubtleCrypto", configurable: true });
  const subtle = Object.create(SubtleCrypto.prototype);

  // ---- Crypto ----
  if (typeof Crypto === "function" && typeof crypto === "object" && crypto) {
    define(Crypto.prototype, "getRandomValues", function getRandomValues(array) { return fill(array); });
    define(Crypto.prototype, "randomUUID", function randomUUID() { return uuid(); });
    Object.defineProperty(Crypto.prototype, "subtle", { get() { return subtle; }, enumerable: true, configurable: true });
  } else {
    class Crypto {
      constructor() { throw new TypeError("Illegal constructor"); }
      getRandomValues(array) { return fill(array); }
      randomUUID() { return uuid(); }
      get subtle() { return subtle; }
    }
    Object.defineProperty(Crypto.prototype, Symbol.toStringTag, { value: "Crypto", configurable: true });
    define(globalThis, "Crypto", Crypto);
    Object.defineProperty(globalThis, "crypto", { value: Object.create(Crypto.prototype), writable: true, enumerable: true, configurable: true });
  }
  define(globalThis, "CryptoKey", CryptoKey);
  define(globalThis, "SubtleCrypto", SubtleCrypto);
})();
