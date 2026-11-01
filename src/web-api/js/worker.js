// Dedicated workers (https://html.spec.whatwg.org/multipage/workers.html), the page's side: the Worker class. The worker itself is a realm made by the
// program (src/web-api/Worker.cpp) and its global scope is src/web-api/worker-js/scope.js; what is said between the two goes over a MessageChannel.
(() => {
  "use strict";
  const create = globalThis.__solarWorkerCreate;
  const start = globalThis.__solarWorkerStart;
  const terminate = globalThis.__solarWorkerTerminate;
  const source = globalThis.__solarWorkerSource;
  delete globalThis.__solarWorkerCreate;
  delete globalThis.__solarWorkerStart;
  delete globalThis.__solarWorkerTerminate;
  delete globalThis.__solarWorkerSource;
  // A service worker makes no workers; a dedicated or shared worker makes dedicated ones (and no shared one: a shared worker is the page's to ask for).
  const inWorker = typeof document === "undefined";
  if (typeof create !== "function" || typeof MessageChannel !== "function" || globalThis.__solarSwInfo) return;

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

  class Worker extends EventTarget {
    #id = -1;
    #port = null;
    #done = false;
    constructor(scriptURL, options = {}) {
      super();
      if (arguments.length === 0) throw new TypeError("Failed to construct 'Worker': 1 argument required, but only 0 present.");
      let url;
      try { url = new URL(String(scriptURL), inWorker ? location.href : document.baseURI); } catch (e) {
        throw new DOMException(`Failed to construct 'Worker': Script at '${scriptURL}' is not a valid URL.`, "SyntaxError");
      }
      if (url.protocol !== "blob:" && url.protocol !== "data:" && url.origin !== location.origin) {
        // (A script of another origin is a worker that fails to load: an error event, not an exception.)
        setTimeout(() => this.dispatchEvent(new ErrorEvent("error", { message: `Script at '${url.href}' cannot be accessed from origin '${location.origin}'.`, filename: url.href, cancelable: true })), 0);
        return;
      }
      const given = typeof options === "object" && options !== null ? options : {};
      const name = given.name === undefined ? "" : String(given.name);
      const type = given.type === "module" ? "module" : "classic";
      const channel = new MessageChannel();
      let swClientIdValue = "";
      const swClientId = () => swClientIdValue;
      this.#port = channel.port1;
      const serviceWorkerFetch = typeof globalThis.__solarSwWorkerFetch === "function" ? globalThis.__solarSwWorkerFetch : null;
      channel.port1.onmessage = (e) => {
        if (this.#done) return;
        // The worker's fetch, for the service worker that serves it.
        if (e.data && typeof e.data === "object" && e.data.__solarSwFetch !== undefined) {
          const call = e.data.__solarSwFetch;
          const answer = serviceWorkerFetch ? serviceWorkerFetch(swClientId(), e.data.request) : Promise.resolve({ kind: "fallback" });
          answer.then((result) => { if (!this.#done) channel.port1.postMessage({ __solarSwReply: call, result }, result && result.response && result.response.body instanceof ArrayBuffer ? [result.response.body] : []); });
          return;
        }
        this.dispatchEvent(new MessageEvent("message", { data: e.data, ports: [...e.ports], origin: "", lastEventId: "" }));
      };
      channel.port1.onmessageerror = () => { if (!this.#done) this.dispatchEvent(new MessageEvent("messageerror", {})); };
      // An error of the worker, or of its start: an error event; whether the page took it (a handler that cancelled it) is answered.
      const fail = (message, filename, lineno, colno) => {
        if (this.#done) return true;
        const init = { message: String(message), filename: filename || url.href, lineno: lineno >>> 0, colno: colno >>> 0, cancelable: true };
        const event = typeof ErrorEvent === "function" ? new ErrorEvent("error", init) : Object.assign(new Event("error", { cancelable: true }), init);
        return !this.dispatchEvent(event);
      };
      // (A shared worker is a dedicated worker with a wrapper made as a blob: what it is, for its scripts, is the address it was asked for.)
      const effective = given.__solarClient ? new URL(given.__solarClient.url) : url;
      this.#id = create(effective.href);
      // (A worker is a client of the service worker that serves the page that made it.)
      let swClient = "";
      if (this.#id >= 0 && typeof globalThis.__solarSwWorkerClient === "function") {
        const client = given.__solarClient;
        try {
          const made = globalThis.__solarSwWorkerClient(client ? client.url : url.href, client ? client.kind : "worker");
          if (made && made.controlled) swClient = swClientIdValue = made.id;
        } catch (e) { /* (no service worker) */ }
      }
      if (this.#id < 0) {
        setTimeout(() => fail("Workers are not available."), 0);
        return;
      }
      const id = this.#id;
      const info = JSON.stringify({
        url: effective.href, name, type, userAgent: navigator.userAgent, language: navigator.language, languages: Array.from(navigator.languages || [navigator.language]),
        platform: navigator.platform, hardwareConcurrency: navigator.hardwareConcurrency, onLine: navigator.onLine, appVersion: navigator.appVersion,
        origin: effective.protocol === "blob:" || effective.protocol === "data:" ? location.origin : effective.origin, swClient,
      });
      // The script is asked for, and the worker starts when it has come; what the page posts meanwhile waits in the port.
      // (A blob, a data address and a file are not the network's: the program has them.)
      const text = url.protocol === "http:" || url.protocol === "https:"
        ? fetch(url.href, { credentials: "same-origin" }).then((response) => {
          if (!response.ok) throw new Error(`Failed to load the worker script (status ${response.status}).`);
          return response.text();
        })
        : new Promise((resolve, reject) => {
          const loaded = source(url.href);
          if (loaded === undefined) reject(new Error(`Failed to load the worker script at '${url.href}'.`)); else resolve(loaded);
        });
      text
        .then((text) => { if (!this.#done) start(id, channel.port2, text, url.href, info, fail, type === "module"); })
        .catch((e) => fail(e && e.message ? e.message : String(e)));
    }
    postMessage(message, transfer) {
      if (this.#done || !this.#port) return;
      if (arguments.length === 0) throw new TypeError("Failed to execute 'postMessage' on 'Worker': 1 argument required, but only 0 present.");
      this.#port.postMessage(message, transfer);
    }
    terminate() {
      if (this.#done) return;
      this.#done = true;
      if (this.#id >= 0) terminate(this.#id);
      if (this.#port) this.#port.close();
    }
  }
  handler(Worker.prototype, ["message", "messageerror", "error"]);
  Object.defineProperty(Worker.prototype, Symbol.toStringTag, { value: "Worker", configurable: true });
  Object.defineProperty(globalThis, "Worker", { value: Worker, writable: true, configurable: true, enumerable: false });

  // ---- SharedWorker: a worker that every SharedWorker of this page with the same address and name connects to; it is a dedicated worker with a wrapper that turns a
  // message into a connect event with the port of the connection (a worker is never shared between pages: a page is a realm of its own here) ----
  const shared = new Map();
  class SharedWorker extends EventTarget {
    #port;
    constructor(scriptURL, options) {
      super();
      if (arguments.length === 0) throw new TypeError("Failed to construct 'SharedWorker': 1 argument required, but only 0 present.");
      let url;
      try { url = new URL(String(scriptURL), inWorker ? location.href : document.baseURI); } catch (e) {
        throw new DOMException(`Failed to construct 'SharedWorker': Script at '${scriptURL}' is not a valid URL.`, "SyntaxError");
      }
      if (url.protocol !== "blob:" && url.protocol !== "data:" && url.origin !== location.origin) {
        this.#port = new MessageChannel().port1;
        setTimeout(() => this.dispatchEvent(new ErrorEvent("error", { message: `Script at '${url.href}' cannot be accessed from origin '${location.origin}'.`, filename: url.href, cancelable: true })), 0);
        return;
      }
      const given = typeof options === "string" ? { name: options } : typeof options === "object" && options !== null ? options : {};
      const name = given.name === undefined ? "" : String(given.name);
      const module = given.type === "module";
      const key = url.href + "|" + name;
      let worker = shared.get(key);
      if (!worker) {
        const source = `
          Object.defineProperty(self, "name", { value: ${JSON.stringify(name)}, configurable: true });
          // (This is a shared worker's scope, not a dedicated one's: scripts ask which they are in.)
          (() => {
            try {
              const Dedicated = self.DedicatedWorkerGlobalScope;
              const parent = Dedicated ? Object.getPrototypeOf(Dedicated.prototype) : Object.prototype;
              const Shared = function SharedWorkerGlobalScope() { throw new TypeError("Illegal constructor"); };
              Shared.prototype = Object.create(parent, { constructor: { value: Shared, writable: true, configurable: true }, [Symbol.toStringTag]: { value: "SharedWorkerGlobalScope", configurable: true } });
              Object.setPrototypeOf(self, Shared.prototype);
              Object.defineProperty(self, "SharedWorkerGlobalScope", { value: Shared, writable: true, configurable: true });
              delete self.DedicatedWorkerGlobalScope;
            } catch (e) { /* (the scope stays what it was) */ }
          })();
          (() => {
            let h = null, ready = ${module ? "false" : "true"}; const waiting = [];
            Object.defineProperty(self, "onconnect", { get() { return h; }, set(f) { h = typeof f === "function" ? f : null; }, configurable: true, enumerable: true });
            addEventListener("connect", (e) => { if (h) h.call(self, e); });
            const connect = (port) => { const ev = new MessageEvent("connect", { ports: [port] }); Object.defineProperty(ev, "source", { value: port }); dispatchEvent(ev); };
            addEventListener("message", (e) => {
              if (!e.data || e.data.__solarShared !== true) return;
              if (ready) connect(e.ports[0]); else waiting.push(e.ports[0]);
            });
            ${module ? `import(${JSON.stringify(url.href)}).then(() => { ready = true; for (const p of waiting.splice(0)) connect(p); }, (error) => { throw error; });` : `importScripts(${JSON.stringify(url.href)});`}
          })();`;
        const blob = new Blob([source], { type: "text/javascript" });
        worker = new Worker(URL.createObjectURL(blob), { type: module ? "module" : "classic", name, __solarClient: { url: url.href, kind: "sharedworker" } });
        shared.set(key, worker);
      }
      worker.addEventListener("error", (e) => this.dispatchEvent(new ErrorEvent("error", { message: e.message, filename: e.filename, lineno: e.lineno, colno: e.colno, cancelable: true })));
      const channel = new MessageChannel();
      this.#port = channel.port1;
      worker.postMessage({ __solarShared: true }, [channel.port2]);
    }
    get port() { return this.#port; }
  }
  handler(SharedWorker.prototype, ["error"]);
  Object.defineProperty(SharedWorker.prototype, Symbol.toStringTag, { value: "SharedWorker", configurable: true });
  if (!inWorker) Object.defineProperty(globalThis, "SharedWorker", { value: SharedWorker, writable: true, configurable: true, enumerable: false });
})();
