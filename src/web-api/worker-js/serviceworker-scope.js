// The global scope of a service worker (https://w3c.github.io/ServiceWorker/#serviceworkerglobalscope-interface), made on top of the scope of a dedicated worker
// (scope.js, which leaves the worker's port and info when the worker is a service worker). The page's side is src/web-api/js/serviceworker.js, and what is said
// between them is one object a time, over the port:
//   page -> worker: {t:"state", state}, {t:"lifecycle", step}, {t:"fetch", id, request, clientId}, {t:"message", data, client}, {t:"reply", id, value | error}
//   worker -> page: {t:"done", step, ok, error}, {t:"fetch-result", id, kind, response, error}, {t:"client-message", data, to}, {t:"skipWaiting"},
//                   and questions that want a {t:"reply"}: {t:"clients" | "claim" | "update" | "unregister" | "navigate" | "open-window", id}
(() => {
  "use strict";
  const global = globalThis;
  const info = global.__solarSwInfo;
  if (!info) return;
  const port = global.__solarSwPort;
  const target = global.__solarSwTarget;
  delete global.__solarSwInfo; delete global.__solarSwPort; delete global.__solarSwTarget;
  const define = (name, value, enumerable = false) => Object.defineProperty(global, name, { value, writable: true, configurable: true, enumerable });
  const tag = (cls, name) => Object.defineProperty(cls.prototype, Symbol.toStringTag, { value: name, configurable: true });
  const illegal = () => { throw new TypeError("Illegal constructor"); };

  // A service worker has no postMessage or close of its own.
  for (const name of ["postMessage", "close"]) { try { delete global[name]; } catch (e) { /* (kept) */ } }

  const dispatch = (event) => {
    Object.defineProperty(event, "target", { value: global, configurable: true });
    return target.dispatchEvent(event);
  };
  let nextCall = 1;
  const calls = new Map();  // id -> resolve/reject of a question put to the page
  const ask = (message, transfer) => new Promise((resolve, reject) => {
    const id = nextCall++;
    calls.set(id, { resolve, reject });
    port.postMessage({ ...message, id }, transfer || []);
  });

  // ---- Events ----
  class ExtendableEvent extends Event {
    #promises = []; #open = true;
    constructor(type, init) { super(type, init); }
    waitUntil(promise) {
      if (!this.#open) throw new DOMException("The event handler is already finished and no pending promises exist.", "InvalidStateError");
      this.#promises.push(Promise.resolve(promise));
    }
    // (Used by this file: the event stays open for waitUntil until the handlers have run.)
    static settle(event) { return event.#settle(); }
    static close(event) { event.#open = false; }
    #settle() {
      const wait = () => {
        const current = this.#promises.splice(0);
        if (!current.length) { this.#open = false; return Promise.resolve(); }
        return Promise.all(current).then(wait);
      };
      return wait();
    }
  }
  tag(ExtendableEvent, "ExtendableEvent");
  class FetchEvent extends ExtendableEvent {
    #request; #clientId; #response = undefined; #called = false; #done = false;
    constructor(type, init) {
      super(type, init);
      if (!init || !(init.request instanceof Request)) throw new TypeError("Failed to construct 'FetchEvent': The required member 'request' is undefined.");
      this.#request = init.request;
      this.#clientId = init.clientId === undefined ? "" : String(init.clientId);
    }
    get request() { return this.#request; }
    get clientId() { return this.#clientId; }
    get resultingClientId() { return ""; }
    get replacesClientId() { return ""; }
    get preloadResponse() { return Promise.resolve(undefined); }
    get handled() { return Promise.resolve(undefined); }
    respondWith(response) {
      if (this.#done) throw new DOMException("The event handler is already finished.", "InvalidStateError");
      if (this.#called) throw new DOMException("The FetchEvent has already been responded to.", "InvalidStateError");
      this.#called = true;
      this.#response = Promise.resolve(response);
      this.waitUntil(this.#response.catch(() => {}));
    }
    static answer(event) { event.#done = true; return event.#called ? event.#response : null; }
  }
  tag(FetchEvent, "FetchEvent");
  class ExtendableMessageEvent extends ExtendableEvent {
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
  tag(ExtendableMessageEvent, "ExtendableMessageEvent");
  define("ExtendableEvent", ExtendableEvent);
  define("FetchEvent", FetchEvent);
  define("ExtendableMessageEvent", ExtendableMessageEvent);

  // ---- Clients ----
  const clientToken = Symbol("Client");
  class Client {
    #id; #url; #type; #frameType;
    constructor(token, d) { if (token !== clientToken) illegal(); this.#id = d.id; this.#url = d.url; this.#type = d.type || "window"; this.#frameType = d.frameType || "top-level"; }
    get id() { return this.#id; }
    get url() { return this.#url; }
    get type() { return this.#type; }
    get frameType() { return this.#frameType; }
    postMessage(message, transfer) {
      if (arguments.length === 0) throw new TypeError("Failed to execute 'postMessage' on 'Client': 1 argument required, but only 0 present.");
      port.postMessage({ t: "client-message", data: message, to: this.#id }, Array.isArray(transfer) ? transfer : transfer && transfer.transfer || []);
    }
  }
  tag(Client, "Client");
  class WindowClient extends Client {
    #state;
    constructor(token, d) { super(token, d); this.#state = d; }
    get visibilityState() { return this.#state.visibilityState || "visible"; }
    get focused() { return !!this.#state.focused; }
    get ancestorOrigins() { return Object.freeze([]); }
    focus() { return Promise.resolve(this); }
    navigate(url) { return ask({ t: "navigate", url: String(url), client: this.id }).then(() => this); }
  }
  tag(WindowClient, "WindowClient");
  const makeClient = (d) => new WindowClient(clientToken, d);
  class Clients {
    constructor() { illegal(); }
    get(id) { return ask({ t: "clients" }).then((list) => { const d = list.find((c) => c.id === String(id)); return d ? makeClient(d) : undefined; }); }
    matchAll(options) {
      const type = options && options.type !== undefined ? String(options.type) : "window";
      return ask({ t: "clients" }).then((list) => (type === "window" || type === "all" ? list.map(makeClient) : []));
    }
    openWindow(url) { return ask({ t: "open-window", url: String(url) }).then((d) => (d ? makeClient(d) : null)); }
    claim() { return ask({ t: "claim" }).then(() => undefined); }
  }
  tag(Clients, "Clients");
  define("Client", Client);
  define("WindowClient", WindowClient);
  define("Clients", Clients);
  define("clients", Object.create(Clients.prototype), true);

  // ---- The worker and its registration, as the worker itself sees them ----
  const workerToken = Symbol("ServiceWorker");
  class ServiceWorker extends EventTarget {
    #url; #state;
    constructor(token, url, state) { super(); if (token !== workerToken) illegal(); this.#url = url; this.#state = state; }
    get scriptURL() { return this.#url; }
    get state() { return this.#state; }
    postMessage() { /* (the worker does not post to itself) */ }
    onstatechange = null;
    onerror = null;
    static set(worker, state) { worker.#state = state; }
  }
  tag(ServiceWorker, "ServiceWorker");
  define("ServiceWorker", ServiceWorker);
  const self_ = new ServiceWorker(workerToken, info.url, info.state || "parsed");
  const stateOf = new Map();
  class NavigationPreloadManager {
    constructor() { illegal(); }
    enable() { return Promise.resolve(); }
    disable() { return Promise.resolve(); }
    setHeaderValue() { return Promise.resolve(); }
    getState() { return Promise.resolve({ enabled: false, headerValue: "Service-Worker-Navigation-Preload" }); }
  }
  tag(NavigationPreloadManager, "NavigationPreloadManager");
  const registrationToken = Symbol("ServiceWorkerRegistration");
  class ServiceWorkerRegistration extends EventTarget {
    #navigationPreload = Object.create(NavigationPreloadManager.prototype);
    constructor(token) { super(); if (token !== registrationToken) illegal(); }
    get scope() { return info.scope; }
    get updateViaCache() { return info.updateViaCache || "imports"; }
    get active() { return self_.state === "installing" || self_.state === "installed" ? null : self_; }
    get installing() { return self_.state === "installing" ? self_ : null; }
    get waiting() { return self_.state === "installed" ? self_ : null; }
    get navigationPreload() { return this.#navigationPreload; }
    get pushManager() { return undefined; }
    update() { return ask({ t: "update" }).then(() => this); }
    unregister() { return ask({ t: "unregister" }); }
    showNotification(title, options) {
      if (typeof Notification === "function") { try { new Notification(title, options); } catch (e) { /* (no notifications here) */ } }
      return Promise.resolve();
    }
    getNotifications() { return Promise.resolve([]); }
    onupdatefound = null;
  }
  tag(ServiceWorkerRegistration, "ServiceWorkerRegistration");
  define("ServiceWorkerRegistration", ServiceWorkerRegistration);
  define("NavigationPreloadManager", NavigationPreloadManager);

  // ---- The scope ----
  // (The prototype chain made by scope.js: global -> DedicatedWorkerGlobalScope -> WorkerGlobalScope -> EventTarget.)
  const workerScopeProto = Object.getPrototypeOf(Object.getPrototypeOf(global));
  const scopeClass = class ServiceWorkerGlobalScope {};
  Object.setPrototypeOf(scopeClass.prototype, workerScopeProto);
  Object.defineProperty(scopeClass.prototype, Symbol.toStringTag, { value: "ServiceWorkerGlobalScope", configurable: true });
  Object.defineProperty(scopeClass, "name", { value: "ServiceWorkerGlobalScope" });
  try { Object.setPrototypeOf(global, scopeClass.prototype); } catch (e) { /* (the scope stays a dedicated one in name) */ }
  define("ServiceWorkerGlobalScope", scopeClass);
  const registration = new ServiceWorkerRegistration(registrationToken);
  define("registration", registration, true);
  define("serviceWorker", self_, true);
  define("skipWaiting", function skipWaiting() { port.postMessage({ t: "skipWaiting" }); return Promise.resolve(); }, true);
  const attribute = (name) => {
    let current = null, registered = false;
    Object.defineProperty(global, "on" + name, {
      get() { return current; },
      set(f) {
        if (!registered) { registered = true; target.addEventListener(name, (e) => { if (typeof current === "function") current.call(global, e); }); }
        current = typeof f === "function" ? f : null;
      },
      enumerable: true, configurable: true,
    });
  };
  for (const name of ["install", "activate", "fetch", "contentdelete", "notificationclick", "notificationclose", "push", "pushsubscriptionchange", "sync", "periodicsync", "backgroundfetchsuccess"]) attribute(name);

  // ---- What the page says ----
  const requestOf = (d) => {
    const init = { method: d.method, headers: d.headers, credentials: d.credentials, cache: d.cache, redirect: d.redirect, referrer: d.referrer || undefined };
    if (d.body && d.method !== "GET" && d.method !== "HEAD") init.body = d.body;
    const mode = d.mode === "navigate" ? "same-origin" : d.mode;
    if (mode) init.mode = mode;
    try { return new Request(d.url, init); } catch (e) { delete init.mode; return new Request(d.url, init); }
  };
  const responseData = (response) => response.arrayBuffer().then((body) => ({
    status: response.status, statusText: response.statusText, headers: Array.from(response.headers.entries()), body, url: response.url, type: response.type, redirected: response.redirected,
  }));
  const lifecycle = (step) => {
    const event = new ExtendableEvent(step);
    dispatch(event);
    return ExtendableEvent.settle(event);
  };
  port.onmessage = (e) => {
    const m = e.data;
    if (!m || typeof m !== "object") return;
    switch (m.t) {
      case "state": ServiceWorker.set(self_, m.state); break;
      case "lifecycle":
        lifecycle(m.step).then(() => port.postMessage({ t: "done", step: m.step, ok: true }), (error) => {
          port.postMessage({ t: "done", step: m.step, ok: false, error: String(error && error.message || error) });
        });
        break;
      case "fetch": {
        let event;
        try { event = new FetchEvent("fetch", { request: requestOf(m.request), clientId: m.clientId, cancelable: true }); } catch (error) {
          port.postMessage({ t: "fetch-result", id: m.id, kind: "error", error: String(error && error.message || error) });
          break;
        }
        dispatch(event);
        const answer = FetchEvent.answer(event);
        if (!answer) { port.postMessage({ t: "fetch-result", id: m.id, kind: "fallback" }); ExtendableEvent.close(event); break; }
        answer.then((response) => {
          if (!(response instanceof Response)) throw new TypeError("The FetchEvent resulted in a network error response: the promise was resolved with something other than a Response.");
          return responseData(response);
        }).then(
          (data) => port.postMessage({ t: "fetch-result", id: m.id, kind: "response", response: data }, [data.body]),
          (error) => port.postMessage({ t: "fetch-result", id: m.id, kind: "error", error: String(error && error.message || error) }));
        break;
      }
      case "message": {
        const event = new ExtendableMessageEvent("message", { data: m.data, origin: info.origin, lastEventId: "", source: m.client ? makeClient(m.client) : null, ports: [...(e.ports || [])] });
        dispatch(event);
        break;
      }
      case "reply": {
        const call = calls.get(m.id);
        if (call) { calls.delete(m.id); if (m.error) call.reject(new DOMException(m.error, "InvalidStateError")); else call.resolve(m.value); }
        break;
      }
    }
  };
})();
