// WebSocket (https://websockets.spec.whatwg.org/), the page's and a worker's: the class is script, and the connection is the program's (src/net/WebSocket.cpp,
// src/web-api/WebSocket.cpp), which tells what happens on it through one callback.
(() => {
  "use strict";
  const open = globalThis.__solarSocketOpen;
  const send = globalThis.__solarSocketSend;
  const close = globalThis.__solarSocketClose;
  delete globalThis.__solarSocketOpen;
  delete globalThis.__solarSocketSend;
  delete globalThis.__solarSocketClose;
  if (typeof open !== "function") return;

  const define = (name, value) => Object.defineProperty(globalThis, name, { value, writable: true, configurable: true, enumerable: false });
  const CONNECTING = 0, OPEN = 1, CLOSING = 2, CLOSED = 3;
  const token = /^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/;

  const handler = (proto, names) => {
    for (const name of names) {
      const slot = Symbol("on" + name);
      Object.defineProperty(proto, "on" + name, {
        get() { return this[slot] === undefined ? null : this[slot]; },
        set(f) {
          if (this[slot] === undefined) this.addEventListener(name, (e) => { const h = this[slot]; if (typeof h === "function") h.call(this, e); });
          this[slot] = typeof f === "function" || (f !== null && typeof f === "object") ? f : null;
        },
        enumerable: true, configurable: true,
      });
    }
  };

  // (A worker's scope has no CloseEvent of its own.)
  const CloseEventClass = () => typeof CloseEvent === "function" ? CloseEvent : class CloseEvent extends Event {
    constructor(type, init = {}) { super(type, init); this.code = init.code | 0; this.reason = init.reason === undefined ? "" : String(init.reason); this.wasClean = !!init.wasClean; }
  };

  const baseOf = () => (typeof document !== "undefined" && document.baseURI) || location.href;
  const originOf = () => location.origin;

  class WebSocket extends EventTarget {
    static get CONNECTING() { return CONNECTING; }
    static get OPEN() { return OPEN; }
    static get CLOSING() { return CLOSING; }
    static get CLOSED() { return CLOSED; }
    #id = -1;
    #state = CONNECTING;
    #url = "";
    #protocol = "";
    #extensions = "";
    #binaryType = "blob";
    #buffered = 0;
    constructor(url, protocols) {
      super();
      if (arguments.length === 0) throw new TypeError("Failed to construct 'WebSocket': 1 argument required, but only 0 present.");
      let parsed;
      try { parsed = new URL(String(url), baseOf()); } catch (e) {
        throw new DOMException(`Failed to construct 'WebSocket': The URL '${url}' is invalid.`, "SyntaxError");
      }
      if (parsed.protocol === "http:") parsed.protocol = "ws:";
      else if (parsed.protocol === "https:") parsed.protocol = "wss:";
      if (parsed.protocol !== "ws:" && parsed.protocol !== "wss:") {
        throw new DOMException(`Failed to construct 'WebSocket': The URL's scheme must be either 'ws' or 'wss'. '${parsed.protocol.slice(0, -1)}' is not allowed.`, "SyntaxError");
      }
      if (parsed.hash) throw new DOMException(`Failed to construct 'WebSocket': The URL contains a fragment identifier ('${parsed.hash}'). Fragment identifiers are not allowed in WebSocket URLs.`, "SyntaxError");
      if (location.protocol === "https:" && parsed.protocol === "ws:" && parsed.hostname !== "localhost" && parsed.hostname !== "127.0.0.1" && parsed.hostname !== "[::1]") {
        throw new DOMException(`Failed to construct 'WebSocket': An insecure WebSocket connection may not be initiated from a page loaded over HTTPS.`, "SecurityError");
      }
      let list = protocols === undefined ? [] : typeof protocols === "string" ? [protocols] : Array.from(protocols, String);
      for (const p of list) {
        if (!token.test(p)) throw new DOMException(`Failed to construct 'WebSocket': The subprotocol '${p}' is invalid.`, "SyntaxError");
      }
      if (new Set(list).size !== list.length) throw new DOMException("Failed to construct 'WebSocket': The subprotocol '" + list.find((p, i) => list.indexOf(p) !== i) + "' is duplicated.", "SyntaxError");
      this.#url = parsed.href;
      this.#id = open(parsed.href, list.join(","), originOf(), (what, a, b, c, d) => this.#event(what, a, b, c, d));
      if (this.#id < 0) {
        this.#state = CLOSING;
        setTimeout(() => this.#finish(false, 1006, "", "no connection"), 0);
      }
    }
    #event(what, a, b, c, d) {
      if (what === "open") {
        if (this.#state !== CONNECTING) return;
        this.#state = OPEN;
        this.#protocol = a;
        this.#extensions = b;
        this.dispatchEvent(new Event("open"));
      } else if (what === "message") {
        if (this.#state !== OPEN) return;
        let data = a;
        if (b) {
          if (this.#binaryType === "blob") data = new Blob([a]);
        }
        this.dispatchEvent(new MessageEvent("message", { data, origin: new URL(this.#url).origin, lastEventId: "" }));
      } else if (what === "close") {
        this.#finish(a, b, c, d);
      }
    }
    #finish(clean, code, reason, error) {
      if (this.#state === CLOSED) return;
      this.#state = CLOSED;
      if (!clean) this.dispatchEvent(new Event("error"));
      this.dispatchEvent(new (CloseEventClass())("close", { wasClean: !!clean, code, reason }));
    }
    get url() { return this.#url; }
    get readyState() { return this.#state; }
    get bufferedAmount() { return this.#buffered; }
    get extensions() { return this.#extensions; }
    get protocol() { return this.#protocol; }
    get binaryType() { return this.#binaryType; }
    set binaryType(v) {
      v = String(v);
      if (v === "blob" || v === "arraybuffer") this.#binaryType = v;
    }
    send(data) {
      if (arguments.length === 0) throw new TypeError("Failed to execute 'send' on 'WebSocket': 1 argument required, but only 0 present.");
      if (this.#state === CONNECTING) throw new DOMException("Failed to execute 'send' on 'WebSocket': Still in CONNECTING state.", "InvalidStateError");
      if (this.#state !== OPEN) return;
      if (typeof Blob === "function" && data instanceof Blob) {
        // (A blob is read before it goes; what is sent after it waits behind it.)
        const id = this.#id;
        this.#queue = (this.#queue || Promise.resolve()).then(() => data.arrayBuffer()).then((buffer) => { if (this.#state === OPEN) send(id, buffer); });
        return;
      }
      if (this.#queue) {
        const id = this.#id;
        const copy = typeof data === "string" ? data : data;
        this.#queue = this.#queue.then(() => { if (this.#state === OPEN) send(id, typeof copy === "string" ? copy : copy); });
        return;
      }
      send(this.#id, typeof data === "string" ? data : ArrayBuffer.isView(data) || data instanceof ArrayBuffer ? data : String(data));
    }
    #queue = null;
    close(code, reason) {
      if (code !== undefined) {
        code = Number(code) & 0xFFFF;
        if (code !== 1000 && (code < 3000 || code > 4999)) throw new DOMException(`Failed to execute 'close' on 'WebSocket': The code must be either 1000, or between 3000 and 4999. ${code} is neither.`, "InvalidAccessError");
      }
      if (reason !== undefined) {
        reason = String(reason);
        if (new TextEncoder().encode(reason).length > 123) throw new DOMException("Failed to execute 'close' on 'WebSocket': The message must not be greater than 123 bytes.", "SyntaxError");
      }
      if (this.#state === CLOSING || this.#state === CLOSED) return;
      const connecting = this.#state === CONNECTING;
      this.#state = CLOSING;
      close(this.#id, code === undefined ? 1005 : code, reason === undefined ? "" : reason);
      void connecting;
    }
  }
  for (const [name, value] of [["CONNECTING", CONNECTING], ["OPEN", OPEN], ["CLOSING", CLOSING], ["CLOSED", CLOSED]]) {
    Object.defineProperty(WebSocket.prototype, name, { value, enumerable: true });
  }
  handler(WebSocket.prototype, ["open", "message", "error", "close"]);
  Object.defineProperty(WebSocket.prototype, Symbol.toStringTag, { value: "WebSocket", configurable: true });
  define("WebSocket", WebSocket);
})();
