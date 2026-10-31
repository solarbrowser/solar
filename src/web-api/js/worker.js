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
  if (typeof create !== "function" || typeof MessageChannel !== "function") return;

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
      try { url = new URL(String(scriptURL), document.baseURI); } catch (e) {
        throw new DOMException(`Failed to construct 'Worker': Script at '${scriptURL}' is not a valid URL.`, "SyntaxError");
      }
      if (url.protocol !== "blob:" && url.protocol !== "data:" && url.origin !== location.origin) {
        throw new DOMException(`Failed to construct 'Worker': Script at '${url.href}' cannot be accessed from origin '${location.origin}'.`, "SecurityError");
      }
      const given = typeof options === "object" && options !== null ? options : {};
      const name = given.name === undefined ? "" : String(given.name);
      const type = given.type === "module" ? "module" : "classic";
      const channel = new MessageChannel();
      this.#port = channel.port1;
      channel.port1.onmessage = (e) => {
        if (this.#done) return;
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
      this.#id = create(url.href);
      if (this.#id < 0) {
        setTimeout(() => fail("Workers are not available."), 0);
        return;
      }
      const id = this.#id;
      const info = JSON.stringify({
        url: url.href, name, type, userAgent: navigator.userAgent, language: navigator.language, languages: Array.from(navigator.languages || [navigator.language]),
        platform: navigator.platform, hardwareConcurrency: navigator.hardwareConcurrency, onLine: navigator.onLine, appVersion: navigator.appVersion,
        origin: url.protocol === "blob:" || url.protocol === "data:" ? location.origin : url.origin,
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
      try { url = new URL(String(scriptURL), document.baseURI); } catch (e) {
        throw new DOMException(`Failed to construct 'SharedWorker': Script at '${scriptURL}' is not a valid URL.`, "SyntaxError");
      }
      if (url.protocol !== "blob:" && url.protocol !== "data:" && url.origin !== location.origin) {
        throw new DOMException(`Failed to construct 'SharedWorker': Script at '${url.href}' cannot be accessed from origin '${location.origin}'.`, "SecurityError");
      }
      const given = typeof options === "string" ? { name: options } : typeof options === "object" && options !== null ? options : {};
      const name = given.name === undefined ? "" : String(given.name);
      const module = given.type === "module";
      const key = url.href + "|" + name;
      let worker = shared.get(key);
      if (!worker) {
        const source = `
          Object.defineProperty(self, "name", { value: ${JSON.stringify(name)}, configurable: true });
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
        worker = new Worker(URL.createObjectURL(blob), { type: module ? "module" : "classic", name });
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
  Object.defineProperty(globalThis, "SharedWorker", { value: SharedWorker, writable: true, configurable: true, enumerable: false });
})();
