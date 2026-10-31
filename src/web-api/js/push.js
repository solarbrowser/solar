// Push (https://w3c.github.io/push-api/): PushManager and PushSubscription, for a registration (the page's and the worker's). A subscription is kept for the scope, with the
// keys a push service would be given (an ECDH P-256 pair and an auth secret); its endpoint is made here, since there is no push service in the program to ask for one,
// so nothing is delivered to it from the network. What is real is everything on this side: permission, the subscription, its keys, the push event (the worker's
// __solarSwPush) and the notifications that come of it.
(() => {
  "use strict";
  const read = globalThis.__solarKvRead, write = globalThis.__solarKvWrite;
  if (typeof read !== "function" || typeof crypto !== "object" || !crypto.subtle) return;
  const b64url = (bytes) => { let s = ""; for (const b of new Uint8Array(bytes)) s += String.fromCharCode(b); return btoa(s).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, ""); };
  const unb64url = (text) => { let s = String(text).replace(/-/g, "+").replace(/_/g, "/"); while (s.length % 4) s += "="; const bin = atob(s); const out = new Uint8Array(bin.length); for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i); return out; };
  const key = (scope) => "sw-push-" + scope;
  const load = (scope) => { try { const t = read(location.origin, key(scope)); return t ? JSON.parse(t) : null; } catch (e) { return null; } };
  const token = Symbol("push");

  class PushSubscriptionOptions {
    #o;
    constructor(t, o) { if (t !== token) throw new TypeError("Illegal constructor"); this.#o = o; }
    get userVisibleOnly() { return this.#o.userVisibleOnly; }
    get applicationServerKey() { return this.#o.applicationServerKey ? unb64url(this.#o.applicationServerKey).buffer : null; }
  }
  Object.defineProperty(PushSubscriptionOptions.prototype, Symbol.toStringTag, { value: "PushSubscriptionOptions", configurable: true });
  class PushSubscription {
    #d; #scope;
    constructor(t, d, scope) { if (t !== token) throw new TypeError("Illegal constructor"); this.#d = d; this.#scope = scope; }
    get endpoint() { return this.#d.endpoint; }
    get expirationTime() { return null; }
    get options() { return new PushSubscriptionOptions(token, this.#d.options); }
    getKey(name) {
      if (name === "p256dh") return unb64url(this.#d.p256dh).buffer;
      if (name === "auth") return unb64url(this.#d.auth).buffer;
      return null;
    }
    toJSON() { return { endpoint: this.#d.endpoint, expirationTime: null, keys: { p256dh: this.#d.p256dh, auth: this.#d.auth } }; }
    unsubscribe() {
      const had = !!load(this.#scope);
      write(location.origin, key(this.#scope), "");
      return Promise.resolve(had);
    }
  }
  Object.defineProperty(PushSubscription.prototype, Symbol.toStringTag, { value: "PushSubscription", configurable: true });

  // The PushManager of the registration at `scope`; `ready()` tells whether the registration has a worker that is active.
  const manager = (scope, ready) => {
    class PushManager {
      constructor() { throw new TypeError("Illegal constructor"); }
      static get supportedContentEncodings() { return ["aes128gcm", "aesgcm"]; }
      permissionState(options) {
        return Promise.resolve(globalThis.__solarPermissions ? globalThis.__solarPermissions.get("push") === "prompt" ? globalThis.__solarPermissions.get("notifications") : globalThis.__solarPermissions.get("push") : "prompt");
      }
      getSubscription() { const d = load(scope); return Promise.resolve(d && d.endpoint ? new PushSubscription(token, d, scope) : null); }
      async subscribe(options = {}) {
        if (!ready()) throw new DOMException("Subscription failed - no active Service Worker", "InvalidStateError");
        if (!options.userVisibleOnly) throw new DOMException("Subscription failed - missing userVisibleOnly: true", "NotSupportedError");
        const state = await this.permissionState(options);
        if (state !== "granted") throw new DOMException("Registration failed - permission denied", "NotAllowedError");
        const existing = load(scope);
        let serverKey = null;
        if (options.applicationServerKey !== undefined) {
          const bytes = typeof options.applicationServerKey === "string" ? unb64url(options.applicationServerKey) : new Uint8Array(ArrayBuffer.isView(options.applicationServerKey) ? options.applicationServerKey.buffer.slice(options.applicationServerKey.byteOffset, options.applicationServerKey.byteOffset + options.applicationServerKey.byteLength) : options.applicationServerKey.slice(0));
          if (bytes.length !== 65 || bytes[0] !== 4) throw new DOMException("Subscription failed - invalid applicationServerKey", "InvalidAccessError");
          serverKey = b64url(bytes);
        }
        if (existing && existing.endpoint) {
          if ((existing.options.applicationServerKey || null) !== serverKey) throw new DOMException("Subscription failed - a subscription with a different applicationServerKey already exists", "InvalidStateError");
          return new PushSubscription(token, existing, scope);
        }
        const pair = await crypto.subtle.generateKey({ name: "ECDH", namedCurve: "P-256" }, true, ["deriveBits"]);
        const d = {
          endpoint: "https://push.solar.invalid/send/" + b64url(crypto.getRandomValues(new Uint8Array(32))),
          p256dh: b64url(await crypto.subtle.exportKey("raw", pair.publicKey)),
          auth: b64url(crypto.getRandomValues(new Uint8Array(16))),
          options: { userVisibleOnly: true, applicationServerKey: serverKey },
        };
        write(location.origin, key(scope), JSON.stringify(d));
        return new PushSubscription(token, d, scope);
      }
    }
    Object.defineProperty(PushManager.prototype, Symbol.toStringTag, { value: "PushManager", configurable: true });
    return Object.create(PushManager.prototype);
  };
  Object.defineProperty(globalThis, "__solarPushManager", { value: manager, writable: true, configurable: true, enumerable: false });
  Object.defineProperty(globalThis, "PushSubscription", { value: PushSubscription, writable: true, configurable: true, enumerable: false });
  Object.defineProperty(globalThis, "PushSubscriptionOptions", { value: PushSubscriptionOptions, writable: true, configurable: true, enumerable: false });
})();
