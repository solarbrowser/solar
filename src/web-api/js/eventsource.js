// EventSource (https://html.spec.whatwg.org/multipage/server-sent-events.html), over fetch: the response body is read as it comes, and the stream of events in it is
// parsed as the specification says (lines, fields, the last event ID, retry); a connection that ends is made again after the retry time.
(() => {
  "use strict";
  if (typeof fetch !== "function") return;
  const CONNECTING = 0, OPEN = 1, CLOSED = 2;

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

  class EventSource extends EventTarget {
    static get CONNECTING() { return CONNECTING; }
    static get OPEN() { return OPEN; }
    static get CLOSED() { return CLOSED; }
    #url = "";
    #withCredentials = false;
    #state = CONNECTING;
    #lastEventId = "";
    #retry = 3000;
    #controller = null;
    #timer = 0;
    constructor(url, init) {
      super();
      if (arguments.length === 0) throw new TypeError("Failed to construct 'EventSource': 1 argument required, but only 0 present.");
      let parsed;
      try { parsed = new URL(String(url), (typeof document !== "undefined" && document.baseURI) || location.href); } catch (e) {
        throw new DOMException(`Failed to construct 'EventSource': Cannot parse url '${url}'`, "SyntaxError");
      }
      this.#url = parsed.href;
      this.#withCredentials = !!(init && init.withCredentials);
      this.#connect();
    }
    get url() { return this.#url; }
    get withCredentials() { return this.#withCredentials; }
    get readyState() { return this.#state; }
    close() {
      this.#state = CLOSED;
      if (this.#timer) { clearTimeout(this.#timer); this.#timer = 0; }
      if (this.#controller) { this.#controller.abort(); this.#controller = null; }
    }
    // The connection failed, or ended: told to the page, and made again unless the page closed it or the failure is for good.
    #fail(forever) {
      if (this.#state === CLOSED) return;
      if (forever) { this.#state = CLOSED; this.dispatchEvent(new Event("error")); return; }
      this.#state = CONNECTING;
      this.dispatchEvent(new Event("error"));
      if (this.#state !== CONNECTING) return;
      this.#timer = setTimeout(() => { this.#timer = 0; if (this.#state === CONNECTING) this.#connect(); }, this.#retry);
    }
    #connect() {
      const controller = new AbortController();
      this.#controller = controller;
      const headers = { "Accept": "text/event-stream", "Cache-Control": "no-store" };
      if (this.#lastEventId) headers["Last-Event-ID"] = this.#lastEventId;
      fetch(this.#url, { headers, cache: "no-store", mode: "cors", credentials: this.#withCredentials ? "include" : "same-origin", signal: controller.signal })
        .then((response) => {
          if (this.#state === CLOSED || this.#controller !== controller) return;
          const type = (response.headers.get("content-type") || "").split(";")[0].trim().toLowerCase();
          if (response.status !== 200 || type !== "text/event-stream" || !response.body) {
            // (A reply other than a stream, or an error, is for good; a 204 as well. A server error is tried again.)
            this.#fail(response.status < 500 || response.status === 501);
            return;
          }
          this.#state = OPEN;
          this.dispatchEvent(new Event("open"));
          return this.#read(response.body.getReader(), controller);
        })
        .catch(() => { if (this.#state !== CLOSED && this.#controller === controller) this.#fail(false); });
    }
    async #read(reader, controller) {
      const decoder = new TextDecoder("utf-8");
      let buffer = "", first = true;
      let data = "", type = "", id = this.#lastEventId;
      let hasData = false;
      const dispatch = () => {
        this.#lastEventId = id;
        if (!hasData) { data = ""; type = ""; return; }
        if (data.endsWith("\n")) data = data.slice(0, -1);
        const event = new MessageEvent(type || "message", { data, origin: new URL(this.#url).origin, lastEventId: this.#lastEventId });
        data = ""; type = ""; hasData = false;
        if (this.#state === OPEN) this.dispatchEvent(event);
      };
      const line = (text) => {
        if (text === "") return dispatch();
        if (text[0] === ":") return;
        const colon = text.indexOf(":");
        let field = text, value = "";
        if (colon >= 0) { field = text.slice(0, colon); value = text.slice(colon + 1); if (value[0] === " ") value = value.slice(1); }
        if (field === "event") type = value;
        else if (field === "data") { data += value + "\n"; hasData = true; }
        else if (field === "id") { if (!value.includes("\0")) id = value; }
        else if (field === "retry") { if (/^[0-9]+$/.test(value)) this.#retry = Number(value); }
      };
      try {
        for (;;) {
          const { done, value } = await reader.read();
          if (this.#state === CLOSED || this.#controller !== controller) return;
          if (done) break;
          let text = decoder.decode(value, { stream: true });
          if (first) { first = false; if (text.charCodeAt(0) === 0xFEFF) text = text.slice(1); }
          buffer += text;
          // Lines end with CRLF, LF or CR (a CR at the end of what has come waits for the LF that may follow).
          let start = 0;
          for (let i = 0; i < buffer.length; i++) {
            const c = buffer[i];
            if (c === "\n" || (c === "\r" && i + 1 < buffer.length)) {
              line(buffer.slice(start, i));
              if (c === "\r" && buffer[i + 1] === "\n") i++;
              start = i + 1;
            }
          }
          buffer = buffer.slice(start);
        }
      } catch (e) { /* (the connection broke) */ }
      if (this.#state !== CLOSED && this.#controller === controller) this.#fail(false);
    }
  }
  for (const [name, value] of [["CONNECTING", CONNECTING], ["OPEN", OPEN], ["CLOSED", CLOSED]]) {
    Object.defineProperty(EventSource.prototype, name, { value, enumerable: true });
  }
  handler(EventSource.prototype, ["open", "message", "error"]);
  Object.defineProperty(EventSource.prototype, Symbol.toStringTag, { value: "EventSource", configurable: true });
  Object.defineProperty(globalThis, "EventSource", { value: EventSource, writable: true, configurable: true, enumerable: false });
})();
