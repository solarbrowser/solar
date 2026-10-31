// Cache API (https://w3c.github.io/ServiceWorker/#cache-objects): caches, the CacheStorage of an origin. What is cached is kept by the program (src/web-api/CacheStore.cpp),
// not by the realm, so that the page and a service worker of one origin see the same caches, and it is written to disk a moment after it changes.
(() => {
  "use strict";
  if (typeof Request !== "function" || typeof Response !== "function") return;
  const N = {};
  for (const n of ["Names", "Open", "Has", "Delete", "Metas", "Body", "Put", "Remove", "Flush"]) { N[n] = globalThis["__solarCache" + n]; delete globalThis["__solarCache" + n]; }
  if (typeof N.Names !== "function") return;
  const hide = (name, value) => Object.defineProperty(globalThis, name, { value, writable: true, configurable: true, enumerable: false });
  const origin = () => location.origin;
  const objects = new Map();  // cache name -> the Cache object of this realm
  let timer = 0;
  const changed = () => {
    if (timer) return;
    timer = setTimeout(() => { timer = 0; N.Flush(); }, 200);
  };
  if (typeof addEventListener === "function") {
    try { addEventListener("pagehide", () => N.Flush()); } catch (e) { /* (a worker's scope has none) */ }
  }
  const metasOf = (name) => { const t = N.Metas(origin(), name); return t === undefined ? [] : JSON.parse(t); };
  const pairs = (headers) => Array.from(headers.entries());
  const stored = (meta) => ({ ...meta, requestHeaders: new Headers(meta.requestHeaders), headers: new Headers(meta.headers) });

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
    #name;
    constructor(token, name) { if (token !== objects) throw new TypeError("Illegal constructor"); this.#name = name; }
    #all() { return metasOf(this.#name).map((m, i) => ({ ...stored(m), index: i })); }
    #find(request, options) { return this.#all().filter((e) => matchesOptions(e, request, options)); }
    #response(entry) {
      const bytes = N.Body(origin(), this.#name, entry.index);
      return toResponse({ ...entry, body: new Uint8Array(bytes || new ArrayBuffer(0)) });
    }
    match(request, options) {
      return new Promise((resolve) => {
        if (arguments.length === 0) throw new TypeError("Failed to execute 'match' on 'Cache': 1 argument required, but only 0 present.");
        const found = this.#find(requestOf(request), options)[0];
        resolve(found ? this.#response(found) : undefined);
      });
    }
    matchAll(request, options) {
      return new Promise((resolve) => {
        const list = request === undefined ? this.#all() : this.#find(requestOf(request), options);
        resolve(list.map((e) => this.#response(e)));
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
          const meta = { url: r.url, method: r.method, requestHeaders: pairs(r.headers), vary: vary || "", status: response.status, statusText: response.statusText, headers: pairs(response.headers) };
          const at = this.#all().find((e) => matchesOptions(e, r, { ignoreVary: true }));
          if (!N.Put(origin(), this.#name, JSON.stringify(meta), buffer, at ? at.index : -1)) throw new TypeError("The cache was deleted.");
          changed();
          resolve(undefined);
        }).catch(reject);
      });
    }
    delete(request, options) {
      return new Promise((resolve) => {
        const r = requestOf(request);
        const hits = this.#find(r, options).map((e) => e.index).sort((a, b) => b - a);
        for (const i of hits) N.Remove(origin(), this.#name, i);
        if (hits.length) changed();
        resolve(hits.length > 0);
      });
    }
    keys(request, options) {
      return new Promise((resolve) => {
        const list = request === undefined ? this.#all() : this.#find(requestOf(request), options);
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
        N.Open(origin(), name);
        if (!objects.has(name)) objects.set(name, new Cache(objects, name));
        changed();
        resolve(objects.get(name));
      });
    }
    has(name) { return Promise.resolve(N.Has(origin(), String(name))); }
    delete(name) { name = String(name); objects.delete(name); const gone = N.Delete(origin(), name); if (gone) changed(); return Promise.resolve(gone); }
    keys() { return Promise.resolve(JSON.parse(N.Names(origin()))); }
    match(request, options) {
      return new Promise((resolve, reject) => {
        const r = requestOf(request);
        const names = options && options.cacheName !== undefined ? [String(options.cacheName)] : JSON.parse(N.Names(origin()));
        const next = (i) => {
          if (i >= names.length) { resolve(undefined); return; }
          if (!N.Has(origin(), names[i])) { next(i + 1); return; }
          this.open(names[i]).then((cache) => cache.match(r, options)).then((hit) => (hit ? resolve(hit) : next(i + 1)), reject);
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
