// Cache API (https://w3c.github.io/ServiceWorker/#cache-objects): caches, the CacheStorage of a realm. A response is kept as its bytes and head, so that match() makes
// a new Response each time. (Kept in memory, for the life of the page; shared by the pages and workers of one realm only.)
(() => {
  "use strict";
  if (typeof Request !== "function" || typeof Response !== "function") return;
  const hide = (name, value) => Object.defineProperty(globalThis, name, { value, writable: true, configurable: true, enumerable: false });
  const stores = new Map();  // cache name -> Cache (insertion order is the order keys() gives)

  const requestOf = (input) => (input instanceof Request ? input : new Request(input));
  const keyOf = (request) => request.url;
  const matchesOptions = (stored, request, options, requestIsGet) => {
    let a = new URL(stored.url), b = new URL(request.url);
    if (options && options.ignoreSearch) { a.search = ""; b.search = ""; }
    a.hash = ""; b.hash = "";
    if (a.href !== b.href) return false;
    if (!(options && options.ignoreMethod) && stored.method !== request.method && !(request.method === "GET" || request.method === "HEAD")) return false;
    if (!(options && options.ignoreMethod) && stored.method !== "GET" && stored.method !== "HEAD") return false;
    if (!(options && options.ignoreVary) && stored.vary) {
      for (const name of stored.vary.split(",").map((s) => s.trim().toLowerCase())) {
        if (name === "*") return false;
        if (stored.requestHeaders.get(name) !== request.headers.get(name)) return false;
      }
    }
    void requestIsGet;
    return true;
  };
  const toResponse = (stored) => {
    const response = new Response(stored.body.length ? stored.body.slice(0) : null, { status: stored.status, statusText: stored.statusText, headers: stored.headers });
    try { Object.defineProperty(response, "url", { value: stored.url, configurable: true }); } catch (e) { /* (read-only) */ }
    return response;
  };

  class Cache {
    #entries = [];
    constructor(token) { if (token !== stores) throw new TypeError("Illegal constructor"); }
    #find(request, options) { return this.#entries.filter((e) => matchesOptions(e, request, options)); }
    match(request, options) {
      return new Promise((resolve) => {
        if (arguments.length === 0) throw new TypeError("Failed to execute 'match' on 'Cache': 1 argument required, but only 0 present.");
        const r = requestOf(request);
        const found = this.#find(r, options)[0];
        resolve(found ? toResponse(found) : undefined);
      });
    }
    matchAll(request, options) {
      return new Promise((resolve) => {
        if (request === undefined) { resolve(this.#entries.map(toResponse)); return; }
        resolve(this.#find(requestOf(request), options).map(toResponse));
      });
    }
    add(request) { return this.addAll([request]); }
    addAll(requests) {
      return new Promise((resolve, reject) => {
        const list = Array.from(requests, requestOf);
        for (const r of list) {
          if (!/^https?:$/.test(new URL(r.url).protocol)) throw new TypeError("Request scheme '" + new URL(r.url).protocol.slice(0, -1) + "' is unsupported");
          if (r.method !== "GET") throw new TypeError("Request method '" + r.method + "' is unsupported");
        }
        Promise.all(list.map((r) => fetch(r))).then((responses) => {
          for (const response of responses) {
            if (!response.ok) throw new TypeError("Request failed");
            if (response.status === 206) throw new TypeError("Partial response (status code 206) is unsupported");
          }
          return Promise.all(list.map((r, i) => this.put(r, responses[i])));
        }).then(() => resolve(undefined), reject);
      });
    }
    put(request, response) {
      return new Promise((resolve, reject) => {
        if (arguments.length < 2) throw new TypeError("Failed to execute 'put' on 'Cache': 2 arguments required, but only " + arguments.length + " present.");
        const r = requestOf(request);
        if (!/^https?:$/.test(new URL(r.url).protocol)) throw new TypeError("Request scheme '" + new URL(r.url).protocol.slice(0, -1) + "' is unsupported");
        if (r.method !== "GET") throw new TypeError("Request method '" + r.method + "' is unsupported");
        if (!(response instanceof Response)) throw new TypeError("Failed to execute 'put' on 'Cache': parameter 2 is not of type 'Response'.");
        if (response.status === 206) throw new TypeError("Partial response (status code 206) is unsupported");
        const vary = response.headers.get("vary");
        if (vary && vary.split(",").some((s) => s.trim() === "*")) throw new TypeError("Vary header contains *");
        if (response.bodyUsed) throw new TypeError("Response body is already used");
        response.clone().arrayBuffer().then((buffer) => {
          const entry = {
            url: r.url, method: r.method, requestHeaders: new Headers(r.headers), vary: vary || "",
            status: response.status, statusText: response.statusText, headers: new Headers(response.headers), body: new Uint8Array(buffer),
          };
          const at = this.#entries.findIndex((e) => matchesOptions(e, r, { ignoreVary: true }));
          if (at >= 0) this.#entries.splice(at, 1);
          this.#entries.push(entry);
          resolve(undefined);
        }, reject);
      });
    }
    delete(request, options) {
      return new Promise((resolve) => {
        const r = requestOf(request);
        const before = this.#entries.length;
        this.#entries = this.#entries.filter((e) => !matchesOptions(e, r, options));
        resolve(this.#entries.length !== before);
      });
    }
    keys(request, options) {
      return new Promise((resolve) => {
        const list = request === undefined ? this.#entries : this.#find(requestOf(request), options);
        resolve(list.map((e) => new Request(e.url, { method: e.method, headers: e.requestHeaders })));
      });
    }
  }
  Object.defineProperty(Cache.prototype, Symbol.toStringTag, { value: "Cache", configurable: true });

  class CacheStorage {
    constructor() { throw new TypeError("Illegal constructor"); }
    open(name) {
      return new Promise((resolve) => {
        name = String(name);
        if (!stores.has(name)) stores.set(name, new Cache(stores));
        resolve(stores.get(name));
      });
    }
    has(name) { return Promise.resolve(stores.has(String(name))); }
    delete(name) { return Promise.resolve(stores.delete(String(name))); }
    keys() { return Promise.resolve(Array.from(stores.keys())); }
    match(request, options) {
      return new Promise((resolve, reject) => {
        const r = requestOf(request);
        const names = options && options.cacheName !== undefined ? [String(options.cacheName)] : Array.from(stores.keys());
        const next = (i) => {
          if (i >= names.length) { resolve(undefined); return; }
          const cache = stores.get(names[i]);
          if (!cache) { next(i + 1); return; }
          cache.match(r, options).then((hit) => (hit ? resolve(hit) : next(i + 1)), reject);
        };
        next(0);
      });
    }
  }
  Object.defineProperty(CacheStorage.prototype, Symbol.toStringTag, { value: "CacheStorage", configurable: true });
  hide("Cache", Cache);
  hide("CacheStorage", CacheStorage);
  const storage = Object.create(CacheStorage.prototype);
  // (caches exists only in a secure context: https, localhost, or a file.)
  const secure = typeof isSecureContext === "boolean" ? isSecureContext : true;
  if (secure) Object.defineProperty(globalThis, "caches", { get() { return storage; }, enumerable: true, configurable: true });
})();
