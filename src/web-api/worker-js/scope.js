// The global scope of a dedicated worker (https://html.spec.whatwg.org/multipage/workers.html#dedicated-workers-and-the-dedicatedworkerglobalscope-interface), made in
// the realm of the worker before its script runs. What the program has already given the realm: events, URLs, fetch, messaging and the console; and the port to the
// page, in __solarWorkerPort.
(() => {
  "use strict";
  const global = globalThis;
  const port = global.__solarWorkerPort;
  const info = JSON.parse(global.__solarWorkerInfoText);
  const close = global.__solarWorkerClose;
  const load = global.__solarWorkerLoad;
  delete global.__solarWorkerPort;
  delete global.__solarWorkerInfoText;
  delete global.__solarWorkerClose;
  delete global.__solarWorkerLoad;
  const define = (name, value, enumerable = false) => Object.defineProperty(global, name, { value, writable: true, configurable: true, enumerable });
  const origin = info.origin === "null" ? "null" : info.origin;
  const started = Date.now();

  // ---- Events ----
  if (typeof MessageEvent !== "function") {
    class MessageEvent extends Event {
      #data; #origin; #lastEventId; #source; #ports;
      constructor(type, init = {}) {
        super(type, init);
        this.#data = init.data === undefined ? null : init.data;
        this.#origin = init.origin === undefined ? "" : String(init.origin);
        this.#lastEventId = init.lastEventId === undefined ? "" : String(init.lastEventId);
        this.#source = init.source === undefined ? null : init.source;
        this.#ports = Object.freeze(Array.from(init.ports || []));
      }
      get data() { return this.#data; }
      get origin() { return this.#origin; }
      get lastEventId() { return this.#lastEventId; }
      get source() { return this.#source; }
      get ports() { return this.#ports; }
    }
    define("MessageEvent", MessageEvent);
  }
  if (typeof ErrorEvent !== "function") {
    class ErrorEvent extends Event {
      #message; #filename; #lineno; #colno; #error;
      constructor(type, init = {}) {
        super(type, init);
        this.#message = init.message === undefined ? "" : String(init.message);
        this.#filename = init.filename === undefined ? "" : String(init.filename);
        this.#lineno = init.lineno >>> 0;
        this.#colno = init.colno >>> 0;
        this.#error = init.error === undefined ? null : init.error;
      }
      get message() { return this.#message; }
      get filename() { return this.#filename; }
      get lineno() { return this.#lineno; }
      get colno() { return this.#colno; }
      get error() { return this.#error; }
    }
    define("ErrorEvent", ErrorEvent);
  }

  // ---- The scope ----
  class WorkerGlobalScope extends EventTarget {
    constructor() { throw new TypeError("Illegal constructor"); }
  }
  class DedicatedWorkerGlobalScope extends WorkerGlobalScope {}
  define("WorkerGlobalScope", WorkerGlobalScope);
  define("DedicatedWorkerGlobalScope", DedicatedWorkerGlobalScope);
  Object.defineProperty(DedicatedWorkerGlobalScope.prototype, Symbol.toStringTag, { value: "DedicatedWorkerGlobalScope", configurable: true });

  // The global object is not made by the constructor, so what an EventTarget does is done by one that is, for it.
  const target = new EventTarget();
  try { Object.setPrototypeOf(global, DedicatedWorkerGlobalScope.prototype); } catch (e) { /* (the methods below are its own) */ }
  define("addEventListener", function addEventListener(type, listener, options) { return target.addEventListener(type, listener, options); });
  define("removeEventListener", function removeEventListener(type, listener, options) { return target.removeEventListener(type, listener, options); });
  define("dispatchEvent", function dispatchEvent(event) { return target.dispatchEvent(event); });
  const handler = (name) => {
    let current = null;
    let registered = false;
    Object.defineProperty(global, "on" + name, {
      get() { return current; },
      set(f) {
        if (!registered) { registered = true; target.addEventListener(name, (e) => { if (typeof current === "function") current.call(global, e); }); }
        current = typeof f === "function" ? f : null;
      },
      enumerable: true, configurable: true,
    });
  };
  for (const name of ["message", "messageerror", "error", "unhandledrejection", "rejectionhandled", "languagechange", "online", "offline"]) handler(name);
  define("self", global, true);
  // (What the realm has of a window's, which a worker has none of.)
  for (const name of ["window", "top", "parent", "frames", "opener", "document"]) {
    if (Object.prototype.hasOwnProperty.call(global, name)) { try { delete global[name]; } catch (e) { /* (kept) */ } }
  }

  // ---- Talking to the page ----
  port.onmessage = (e) => {
    const event = new MessageEvent("message", { data: e.data, ports: [...e.ports], origin: "", lastEventId: "", source: null });
    Object.defineProperty(event, "target", { value: global, configurable: true });
    target.dispatchEvent(event);
  };
  port.onmessageerror = () => {
    const event = new MessageEvent("messageerror", {});
    Object.defineProperty(event, "target", { value: global, configurable: true });
    target.dispatchEvent(event);
  };
  define("postMessage", function postMessage(message, transfer) {
    if (arguments.length === 0) throw new TypeError("Failed to execute 'postMessage' on 'DedicatedWorkerGlobalScope': 1 argument required, but only 0 present.");
    port.postMessage(message, transfer);
  }, true);
  define("close", function close_() { close(); port.close(); }, true);
  define("name", info.name, true);

  // ---- importScripts ----
  define("importScripts", function importScripts(...urls) {
    if (info.type === "module") throw new TypeError("Module scripts don't support importScripts().");
    for (const given of urls) {
      let href;
      try { href = new URL(String(given), info.url).href; } catch (e) { throw new DOMException(`Failed to execute 'importScripts' on 'WorkerGlobalScope': The URL '${given}' is invalid.`, "SyntaxError"); }
      const text = load(href);
      if (text === undefined) throw new DOMException(`Failed to execute 'importScripts' on 'WorkerGlobalScope': The script at '${href}' failed to load.`, "NetworkError");
      (0, eval)(text + "\n//# sourceURL=" + href);
    }
  }, true);

  // ---- location and navigator ----
  const locationUrl = new URL(info.url);
  class WorkerLocation {
    get href() { return locationUrl.href; }
    get origin() { return locationUrl.origin; }
    get protocol() { return locationUrl.protocol; }
    get host() { return locationUrl.host; }
    get hostname() { return locationUrl.hostname; }
    get port() { return locationUrl.port; }
    get pathname() { return locationUrl.pathname; }
    get search() { return locationUrl.search; }
    get hash() { return locationUrl.hash; }
    toString() { return locationUrl.href; }
  }
  Object.defineProperty(WorkerLocation.prototype, Symbol.toStringTag, { value: "WorkerLocation", configurable: true });
  define("WorkerLocation", WorkerLocation);
  define("location", new WorkerLocation(), true);
  class WorkerNavigator {
    get userAgent() { return info.userAgent; }
    get language() { return info.language; }
    get languages() { return Object.freeze(info.languages.slice()); }
    get platform() { return info.platform; }
    get appVersion() { return info.appVersion; }
    get appName() { return "Netscape"; }
    get appCodeName() { return "Mozilla"; }
    get product() { return "Gecko"; }
    get vendor() { return ""; }
    get hardwareConcurrency() { return info.hardwareConcurrency; }
    get onLine() { return info.onLine; }
    get webdriver() { return false; }
  }
  Object.defineProperty(WorkerNavigator.prototype, Symbol.toStringTag, { value: "WorkerNavigator", configurable: true });
  define("WorkerNavigator", WorkerNavigator);
  define("navigator", new WorkerNavigator(), true);
  define("origin", origin, true);
  define("isSecureContext", location.protocol === "https:" || location.hostname === "localhost" || location.protocol === "blob:", true);
  define("crossOriginIsolated", false, true);
  define("reportError", function reportError(error) {
    const event = new ErrorEvent("error", { error, message: error && error.message ? String(error.message) : String(error), cancelable: true });
    if (target.dispatchEvent(event)) console.error(error);
  }, true);

  // ---- What a window has that a worker has too ----
  if (typeof performance === "undefined") {
    class Performance extends EventTarget {
      now() { return Date.now() - started; }
      get timeOrigin() { return started; }
      mark() { return undefined; }
      measure() { return undefined; }
      clearMarks() {}
      clearMeasures() {}
      getEntries() { return []; }
      getEntriesByName() { return []; }
      getEntriesByType() { return []; }
      toJSON() { return { timeOrigin: started }; }
    }
    define("Performance", Performance);
    define("performance", new Performance(), true);
  }
  if (typeof atob === "undefined") {
    const alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    define("btoa", function btoa(data) {
      if (arguments.length === 0) throw new TypeError("btoa requires 1 argument");
      const s = String(data);
      let out = "";
      for (let i = 0; i < s.length; i += 3) {
        const a = s.charCodeAt(i), b = s.charCodeAt(i + 1), c = s.charCodeAt(i + 2);
        if (a > 255 || b > 255 || c > 255) throw new DOMException("The string to be encoded contains characters outside of the Latin1 range.", "InvalidCharacterError");
        const n = (a << 16) | ((b || 0) << 8) | (c || 0);
        out += alphabet[(n >> 18) & 63] + alphabet[(n >> 12) & 63] + (i + 1 < s.length ? alphabet[(n >> 6) & 63] : "=") + (i + 2 < s.length ? alphabet[n & 63] : "=");
      }
      return out;
    }, true);
    define("atob", function atob(data) {
      if (arguments.length === 0) throw new TypeError("atob requires 1 argument");
      let s = String(data).replace(/[\t\n\f\r ]/g, "");
      if (s.length % 4 === 0) s = s.replace(/==?$/, "");
      if (s.length % 4 === 1 || /[^A-Za-z0-9+/]/.test(s)) throw new DOMException("The string to be decoded is not correctly encoded.", "InvalidCharacterError");
      let out = "", bits = 0, count = 0;
      for (const ch of s) {
        bits = (bits << 6) | alphabet.indexOf(ch);
        count += 6;
        if (count >= 8) { count -= 8; out += String.fromCharCode((bits >> count) & 255); }
      }
      return out;
    }, true);
  }
  if (typeof crypto === "undefined") {
    const hex = (n) => n.toString(16).padStart(2, "0");
    class Crypto {
      getRandomValues(array) {
        if (!ArrayBuffer.isView(array) || array instanceof Float32Array || array instanceof Float64Array || array instanceof DataView) throw new DOMException("The data argument must be an integer-type TypedArray.", "TypeMismatchError");
        const bytes = new Uint8Array(array.buffer, array.byteOffset, array.byteLength);
        for (let i = 0; i < bytes.length; i++) bytes[i] = Math.floor(Math.random() * 256);
        return array;
      }
      randomUUID() {
        const b = new Uint8Array(16);
        this.getRandomValues(b);
        b[6] = (b[6] & 15) | 64;
        b[8] = (b[8] & 63) | 128;
        const h = Array.from(b, hex).join("");
        return `${h.slice(0, 8)}-${h.slice(8, 12)}-${h.slice(12, 16)}-${h.slice(16, 20)}-${h.slice(20)}`;
      }
    }
    define("Crypto", Crypto);
    define("crypto", new Crypto(), true);
  }
})();
