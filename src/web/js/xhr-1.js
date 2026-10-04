// XMLHttpRequest (https://xhr.spec.whatwg.org/), written on top of fetch and of the program's own loader. A request
// the loader can answer (the files of a page and its frames) is answered from it, which is the only way a request
// can be synchronous; any other is sent with fetch, and only asynchronously.
(function () {
  const load = __solarLoadText;
  const contentTypeOf = __solarContentTypeOf;
  delete globalThis.__solarLoadText;
  delete globalThis.__solarContentTypeOf;

  const UNSENT = 0, OPENED = 1, HEADERS_RECEIVED = 2, LOADING = 3, DONE = 4;
  const internal = new WeakMap();
  const handlerStore = new WeakMap();

  const defineHandlers = (prototype, types) => {
    for (const type of types) {
      Object.defineProperty(prototype, "on" + type, {
        get() { const entry = handlerStore.get(this); return entry && entry[type] ? entry[type].handler : null; },
        set(value) {
          let entry = handlerStore.get(this);
          if (!entry) { entry = {}; handlerStore.set(this, entry); }
          let record = entry[type];
          if (!record) {
            record = { handler: null };
            record.listener = (event) => { if (record.handler) record.handler.call(this, event); };
            entry[type] = record;
            this.addEventListener(type, record.listener);
          }
          record.handler = typeof value === "function" || (typeof value === "object" && value !== null) ? value : null;
        },
        enumerable: true,
        configurable: true,
      });
    }
  };

  class XMLHttpRequestEventTarget extends EventTarget {
    constructor() {
      super();
    }
  }
  defineHandlers(XMLHttpRequestEventTarget.prototype, ["loadstart", "progress", "abort", "error", "load", "timeout", "loadend"]);

  class XMLHttpRequestUpload extends XMLHttpRequestEventTarget {}

  const forbiddenHeaders = new Set(["accept-charset", "accept-encoding", "access-control-request-headers", "access-control-request-method", "connection", "content-length", "cookie", "cookie2", "date", "dnt", "expect", "host", "keep-alive", "origin", "referer", "set-cookie", "te", "trailer", "transfer-encoding", "upgrade", "via"]);
  const methods = new Set(["GET", "HEAD", "POST", "PUT", "DELETE", "OPTIONS"]);
  const token = /^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/;

  const fire = (target, type, init) => {
    const s = internal.get(target);
    let event;
    if (init && init.total !== undefined) event = new ProgressEvent(type, init);
    else event = new ProgressEvent(type, { lengthComputable: false, loaded: 0, total: 0 });
    target.dispatchEvent(event);
  };

  const encoder = new TextEncoder();

  function fail(self, kind) {
    const s = internal.get(self);
    s.errorFlag = true;
    s.sendFlag = false;
    s.response = null; s.status = 0; s.statusText = ""; s.responseHeaders = null; s.text = ""; s.bytes = null;
    if (s.timer) { clearTimeout(s.timer); s.timer = null; }
    s.state = DONE;
    if (!s.async && kind !== "abort") return;
    self.dispatchEvent(new Event("readystatechange"));
    if (s.body !== null && s.upload) { fire(s.upload, kind); fire(s.upload, "loadend"); }
    fire(self, kind);
    fire(self, "loadend");
  }

  function run(self, id) {
    const s = internal.get(self);
    if (s.id !== id || !s.sendFlag) return;
    const finish = (status, statusText, headers, bytes, text) => {
      if (s.id !== id || !s.sendFlag) return;
      s.status = status; s.statusText = statusText; s.responseHeaders = headers; s.bytes = bytes; s.text = text;
      s.responseUrl = s.url.replace(/#.*$/, "");
      if (s.async) {
        s.state = HEADERS_RECEIVED;
        self.dispatchEvent(new Event("readystatechange"));
        if (s.id !== id) return;
        s.state = LOADING;
        self.dispatchEvent(new Event("readystatechange"));
        fire(self, "progress", { lengthComputable: true, loaded: bytes.length, total: bytes.length });
        if (s.id !== id) return;
      }
      s.sendFlag = false;
      if (s.timer) { clearTimeout(s.timer); s.timer = null; }
      s.state = DONE;
      self.dispatchEvent(new Event("readystatechange"));
      if (s.id !== id) return;
      if (s.async) {
        fire(self, "load", { lengthComputable: true, loaded: bytes.length, total: bytes.length });
        fire(self, "loadend", { lengthComputable: true, loaded: bytes.length, total: bytes.length });
      }
    };
    const local = s.method === "GET" || s.method === "HEAD" ? load(s.url) : undefined;
    if (local !== undefined) {
      const headers = new Headers({ "content-type": contentTypeOf(s.url) });
      finish(200, "OK", headers, s.method === "HEAD" ? new Uint8Array(0) : encoder.encode(local), s.method === "HEAD" ? "" : local);
      return;
    }
    if (!s.async) { self.__fail("error"); return; }
    const init = { method: s.method, headers: s.headers, credentials: s.withCredentials ? "include" : "same-origin" };
    if (s.body !== null) init.body = s.body;
    fetch(s.url, init).then(async (response) => {
      const bytes = new Uint8Array(await response.arrayBuffer());
      finish(response.status, response.statusText, response.headers, bytes, new TextDecoder().decode(bytes));
    }, () => { if (s.id === id && s.sendFlag) self.__fail("error"); });
  }


  class XMLHttpRequest extends XMLHttpRequestEventTarget {
    constructor() {
      super();
      internal.set(this, {
        state: UNSENT, method: "GET", url: "", async: true, headers: new Headers(), body: null, sendFlag: false, errorFlag: false,
        responseType: "", timeout: 0, withCredentials: false, response: null, status: 0, statusText: "", responseHeaders: null,
        responseUrl: "", text: "", bytes: null, timer: null, upload: new XMLHttpRequestUpload(), overrideMime: null, id: 0, aborted: false,
      });
    }

    get readyState() { return internal.get(this).state; }
    get upload() { return internal.get(this).upload; }
    get status() { return internal.get(this).status; }
    get statusText() { return internal.get(this).statusText; }
    get responseURL() { return internal.get(this).responseUrl; }
    get withCredentials() { return internal.get(this).withCredentials; }
    set withCredentials(value) {
      const s = internal.get(this);
      if (s.state !== UNSENT && s.state !== OPENED) throw new DOMException("The XMLHttpRequest must be unsent or opened.", "InvalidStateError");
      if (s.sendFlag) throw new DOMException("The XMLHttpRequest is being sent.", "InvalidStateError");
      s.withCredentials = Boolean(value);
    }
    get timeout() { return internal.get(this).timeout; }
    set timeout(value) {
      const s = internal.get(this);
      if (!s.async && globalThis.document) throw new DOMException("Timeouts cannot be set for synchronous requests made from a document.", "InvalidAccessError");
      s.timeout = value >>> 0;
    }
    get responseType() { return internal.get(this).responseType; }
    set responseType(value) {
      const s = internal.get(this);
      value = String(value);
      if (!["", "arraybuffer", "blob", "document", "json", "text"].includes(value)) return;
      if (s.state === LOADING || s.state === DONE) throw new DOMException("The response type cannot be set if the object's state is LOADING or DONE.", "InvalidStateError");
      if (!s.async && globalThis.document) throw new DOMException("The response type cannot be changed for synchronous requests made from a document.", "InvalidAccessError");
      s.responseType = value;
    }

    open(method, url, async = true, username = null, password = null) {
      if (arguments.length < 2) throw new TypeError("Failed to execute 'open' on 'XMLHttpRequest': 2 arguments required, but only " + arguments.length + " present.");
      const s = internal.get(this);
      method = String(method);
      if (!token.test(method)) throw new DOMException("'" + method + "' is not a valid HTTP method.", "SyntaxError");
      const upper = method.toUpperCase();
      if (["CONNECT", "TRACE", "TRACK"].includes(upper)) throw new DOMException("'" + method + "' HTTP method is unsupported.", "SecurityError");
      if (methods.has(upper)) method = upper;
      let parsed;
      try { parsed = new URL(String(url), globalThis.document ? globalThis.document.baseURI || globalThis.document.URL : undefined); } catch (e) { throw new DOMException("Failed to parse URL '" + url + "'.", "SyntaxError"); }
      if (arguments.length > 2 && !async && globalThis.document && (s.timeout !== 0 || s.withCredentials || s.responseType !== "")) {
        throw new DOMException("Synchronous requests must not set timeout, withCredentials or responseType.", "InvalidAccessError");
      }
      s.id++;
      if (s.timer) { clearTimeout(s.timer); s.timer = null; }
      s.method = method; s.url = parsed.href; s.async = Boolean(async); s.headers = new Headers(); s.body = null;
      s.sendFlag = false; s.errorFlag = false; s.response = null; s.responseHeaders = null; s.status = 0; s.statusText = ""; s.text = ""; s.bytes = null; s.aborted = false;
      if (s.state !== OPENED) {
        s.state = OPENED;
        this.dispatchEvent(new Event("readystatechange"));
      } else {
        s.state = OPENED;
      }
    }

    setRequestHeader(name, value) {
      if (arguments.length < 2) throw new TypeError("Failed to execute 'setRequestHeader' on 'XMLHttpRequest': 2 arguments required, but only " + arguments.length + " present.");
      const s = internal.get(this);
      if (s.state !== OPENED) throw new DOMException("The XMLHttpRequest state must be OPENED.", "InvalidStateError");
      if (s.sendFlag) throw new DOMException("The XMLHttpRequest send() flag is set.", "InvalidStateError");
      name = String(name); value = String(value).replace(/^[\t\n\r ]+|[\t\n\r ]+$/g, "");
      if (!token.test(name) || /[\0\r\n]/.test(value)) throw new DOMException("'" + name + "' is not a valid HTTP header field name or value.", "SyntaxError");
      const lower = name.toLowerCase();
      if (forbiddenHeaders.has(lower) || lower.startsWith("proxy-") || lower.startsWith("sec-")) return;
      s.headers.append(name, value);
    }

    send(body = null) {
      const s = internal.get(this);
      if (s.state !== OPENED) throw new DOMException("The XMLHttpRequest state must be OPENED.", "InvalidStateError");
      if (s.sendFlag) throw new DOMException("The XMLHttpRequest send() flag is set.", "InvalidStateError");
      if (s.method === "GET" || s.method === "HEAD") body = null;
      let payload = null;
      if (body !== null && body !== undefined) {
        if (typeof body === "string") {
          payload = body;
          if (!s.headers.has("content-type")) s.headers.set("content-type", "text/plain;charset=UTF-8");
        } else if (body instanceof URLSearchParams) {
          payload = body.toString();
          if (!s.headers.has("content-type")) s.headers.set("content-type", "application/x-www-form-urlencoded;charset=UTF-8");
        } else if (typeof Document === "function" && body instanceof Document) {
          payload = new XMLSerializer().serializeToString(body);
          if (!s.headers.has("content-type")) s.headers.set("content-type", body.contentType === "text/html" ? "text/html;charset=UTF-8" : "application/xml;charset=UTF-8");
        } else {
          payload = body;
        }
      }
      s.body = payload;
      s.errorFlag = false;
      s.sendFlag = true;
      const id = s.id;
      if (s.async) {
        fire(this, "loadstart");
        if (!s.uploadComplete && payload !== null) fire(s.upload, "loadstart");
        setTimeout(() => run(this, id), 0);
        if (s.timeout) s.timer = setTimeout(() => { if (s.id === id && s.sendFlag) fail(this, "timeout"); }, s.timeout);
      } else {
        run(this, id);
        if (s.errorFlag) throw new DOMException("Failed to load '" + s.url + "'.", "NetworkError");
      }
    }

    abort() {
      const s = internal.get(this);
      if (s.sendFlag && (s.state === OPENED || s.state === HEADERS_RECEIVED || s.state === LOADING)) {
        s.id++;
        s.aborted = true;
        fail(this, "abort");
      }
      if (s.state === DONE) {
        s.state = UNSENT;
        s.response = null; s.status = 0; s.statusText = ""; s.responseHeaders = null; s.text = ""; s.bytes = null;
      }
    }

    getResponseHeader(name) {
      const s = internal.get(this);
      if (s.state < HEADERS_RECEIVED || s.errorFlag || !s.responseHeaders) return null;
      return s.responseHeaders.get(String(name));
    }

    getAllResponseHeaders() {
      const s = internal.get(this);
      if (s.state < HEADERS_RECEIVED || s.errorFlag || !s.responseHeaders) return "";
      const lines = [];
      for (const [name, value] of s.responseHeaders) lines.push(name + ": " + value);
      return lines.sort().map((l) => l).join("\r\n") + (lines.length ? "\r\n" : "");
    }

    overrideMimeType(mime) {
      const s = internal.get(this);
      if (s.state === LOADING || s.state === DONE) throw new DOMException("The state must not be LOADING or DONE.", "InvalidStateError");
      s.overrideMime = String(mime);
    }

    get responseText() {
      const s = internal.get(this);
      if (s.responseType !== "" && s.responseType !== "text") throw new DOMException("The value is only accessible if the object's 'responseType' is '' or 'text'.", "InvalidStateError");
      if (s.state !== LOADING && s.state !== DONE) return "";
      return s.errorFlag ? "" : s.text;
    }

    get response() {
      const s = internal.get(this);
      if (s.responseType === "" || s.responseType === "text") {
        if (s.state !== LOADING && s.state !== DONE) return "";
        return s.errorFlag ? "" : s.text;
      }
      if (s.state !== DONE || s.errorFlag) return null;
      if (s.response !== null) return s.response;
      const mime = ((s.overrideMime || (s.responseHeaders && s.responseHeaders.get("content-type")) || "") + "").split(";")[0].trim().toLowerCase();
      switch (s.responseType) {
        case "arraybuffer": s.response = s.bytes.buffer.slice(s.bytes.byteOffset, s.bytes.byteOffset + s.bytes.byteLength); break;
        case "blob": s.response = new Blob([s.bytes], { type: mime }); break;
        case "json":
          try { s.response = JSON.parse(s.text); } catch (e) { s.response = null; }
          break;
        case "document": {
          const parser = new DOMParser();
          try {
            if (mime === "text/html") s.response = parser.parseFromString(s.text, "text/html");
            else if (/^(text|application)\/(.+\+)?xml$/.test(mime) || mime === "image/svg+xml") s.response = parser.parseFromString(s.text, mime === "text/xml" || mime === "application/xml" || mime === "application/xhtml+xml" || mime === "image/svg+xml" ? mime : "application/xml");
            else s.response = null;
          } catch (e) { s.response = null; }
          if (s.response) {
            try { Object.defineProperty(s.response, "URL", { value: s.responseUrl, configurable: true }); } catch (e) { /* the document keeps its own */ }
          }
          break;
        }
      }
      return s.response;
    }

    get responseXML() {
      const s = internal.get(this);
      if (s.responseType !== "" && s.responseType !== "document") throw new DOMException("The value is only accessible if the object's 'responseType' is '' or 'document'.", "InvalidStateError");
      if (s.state !== DONE || s.errorFlag) return null;
      const mime = ((s.overrideMime || (s.responseHeaders && s.responseHeaders.get("content-type")) || "") + "").split(";")[0].trim().toLowerCase();
      if (s.responseType === "") {
        if (!(mime === "text/xml" || mime === "application/xml" || mime === "image/svg+xml" || /\+xml$/.test(mime))) return null;
      }
      if (s.xmlDocument === undefined) {
        try { s.xmlDocument = new DOMParser().parseFromString(s.text, mime === "text/html" ? "text/html" : mime || "application/xml"); } catch (e) { s.xmlDocument = null; }
      }
      return s.xmlDocument;
    }
  }
  defineHandlers(XMLHttpRequest.prototype, ["readystatechange"]);
  for (const [name, value] of Object.entries({ UNSENT, OPENED, HEADERS_RECEIVED, LOADING, DONE })) {
    Object.defineProperty(XMLHttpRequest, name, { value, enumerable: true });
    Object.defineProperty(XMLHttpRequest.prototype, name, { value, enumerable: true });
  }
  for (const [name, value] of [["XMLHttpRequestEventTarget", XMLHttpRequestEventTarget], ["XMLHttpRequestUpload", XMLHttpRequestUpload], ["XMLHttpRequest", XMLHttpRequest]]) {
    Object.defineProperty(globalThis, name, { value, writable: true, enumerable: false, configurable: true });
  }
})();
