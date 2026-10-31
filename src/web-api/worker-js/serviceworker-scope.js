// The global scope of a service worker (https://w3c.github.io/ServiceWorker/#serviceworkerglobalscope-interface), made on top of the scope of a dedicated worker
// (scope.js, which leaves the worker's info and target when the worker is a service worker). A service worker outlives its page and serves every page of its scope: each
// page gives it a port of its own (__solarSwAttach), and the program asks it for navigations (__solarSwNavigate) with no page at all. What a page and the worker say
// to each other is one object a time, over the page's port:
//   page -> worker: {t:"hello", client}, {t:"bye"}, {t:"state", state}, {t:"lifecycle", step}, {t:"fetch", id, request, clientId}, {t:"message", data, client},
//                   {t:"reply", id, value | error}, {t:"rpc", id, call, args}
//   worker -> page: {t:"done", step, ok, error}, {t:"fetch-result", id, kind, response, error}, {t:"client-message", data, to}, {t:"skipWaiting"}, {t:"claimed"},
//                   {t:"rpc-result", id, value | error}, and questions that want a {t:"reply"}: {t:"update" | "unregister" | "navigate" | "open-window", id}
// A worker that has no page and nothing to do for a while stops itself; the next navigation or page starts it again from the source that was kept.
(() => {
  "use strict";
  const global = globalThis;
  const info = global.__solarSwInfo;
  if (!info) return;
  const target = global.__solarSwTarget;
  const kvRead = global.__solarKvRead, kvWrite = global.__solarKvWrite;
  const makePushManager = global.__solarPushManager;
  delete global.__solarPushManager;
  const navigationDone = global.__solarSwNavigateDone;
  const stopSelf = global.__solarSwStop;
  const idleMs = typeof global.__solarSwIdleMs === "number" ? global.__solarSwIdleMs : 30000;
  for (const name of ["__solarSwInfo", "__solarSwTarget", "__solarSwNavigateDone", "__solarSwIdleMs", "__solarSwStop", "__solarKvRead", "__solarKvWrite"]) delete global[name];
  const define = (name, value, enumerable = false) => Object.defineProperty(global, name, { value, writable: true, configurable: true, enumerable });
  const tag = (cls, name) => Object.defineProperty(cls.prototype, Symbol.toStringTag, { value: name, configurable: true });
  const illegal = () => { throw new TypeError("Illegal constructor"); };
  const KEY = "sw-registrations";
  const random = () => Array.from(crypto.getRandomValues(new Uint8Array(16)), (b) => b.toString(16).padStart(2, "0")).join("");

  // A service worker has no postMessage or close of its own.
  for (const name of ["postMessage", "close"]) { try { delete global[name]; } catch (e) { /* (kept) */ } }

  const dispatch = (event) => {
    Object.defineProperty(event, "target", { value: global, configurable: true });
    return target.dispatchEvent(event);
  };

  // ---- Activity: what keeps the worker alive ----
  let inflight = 0, lastActivity = Date.now();
  const busy = (promise) => {
    inflight++;
    lastActivity = Date.now();
    return Promise.resolve(promise).finally(() => { inflight--; lastActivity = Date.now(); });
  };
  const clientList = new Map();  // port -> { port, desc }
  const ticker = setInterval(() => {
    if (clientList.size === 0 && inflight === 0 && Date.now() - lastActivity >= idleMs) {
      clearInterval(ticker);
      stopSelf();
    }
  }, Math.max(50, Math.min(5000, idleMs / 4)));

  // ---- Talking to the pages ----
  let nextCall = 1;
  const calls = new Map();  // id -> resolve/reject of a question put to a page
  const primary = () => { let last = null; for (const c of clientList.values()) last = c; return last; };
  const ask = (message, transfer) => new Promise((resolve, reject) => {
    const page = primary();
    if (!page) { reject(new DOMException("No page is there to answer.", "InvalidStateError")); return; }
    const id = nextCall++;
    calls.set(id, { resolve, reject });
    page.port.postMessage({ ...message, id }, transfer || []);
  });

  // ---- Events ----
  class ExtendableEvent extends Event {
    #promises = []; #open = true;
    constructor(type, init) { super(type, init); }
    waitUntil(promise) {
      if (!this.#open) throw new DOMException("The event handler is already finished and no pending promises exist.", "InvalidStateError");
      this.#promises.push(Promise.resolve(promise));
    }
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
    #request; #clientId; #resulting; #preload; #response = undefined; #called = false; #done = false;
    constructor(type, init) {
      super(type, init);
      if (!init || !(init.request instanceof Request)) throw new TypeError("Failed to construct 'FetchEvent': The required member 'request' is undefined.");
      this.#request = init.request;
      this.#clientId = init.clientId === undefined ? "" : String(init.clientId);
      this.#resulting = init.resultingClientId === undefined ? "" : String(init.resultingClientId);
      this.#preload = init.preloadResponse === undefined ? Promise.resolve(undefined) : init.preloadResponse;
    }
    get request() { return this.#request; }
    get clientId() { return this.#clientId; }
    get resultingClientId() { return this.#resulting; }
    get replacesClientId() { return ""; }
    get preloadResponse() { return this.#preload; }
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
  class SyncEvent extends ExtendableEvent {
    #tag; #lastChance;
    constructor(type, init) {
      super(type, init);
      if (!init || init.tag === undefined) throw new TypeError("Failed to construct 'SyncEvent': required member tag is undefined.");
      this.#tag = String(init.tag); this.#lastChance = !!init.lastChance;
    }
    get tag() { return this.#tag; }
    get lastChance() { return this.#lastChance; }
  }
  tag(SyncEvent, "SyncEvent");
  class PeriodicSyncEvent extends ExtendableEvent {
    #tag;
    constructor(type, init) {
      super(type, init);
      if (!init || init.tag === undefined) throw new TypeError("Failed to construct 'PeriodicSyncEvent': required member tag is undefined.");
      this.#tag = String(init.tag);
    }
    get tag() { return this.#tag; }
  }
  tag(PeriodicSyncEvent, "PeriodicSyncEvent");
  class PushMessageData {
    #bytes;
    constructor(bytes) { this.#bytes = bytes; }
    arrayBuffer() { return this.#bytes.slice().buffer; }
    blob(type) { return new Blob([this.#bytes], { type: type || "" }); }
    bytes() { return this.#bytes.slice(); }
    json() { return JSON.parse(this.text()); }
    text() { return new TextDecoder().decode(this.#bytes); }
  }
  tag(PushMessageData, "PushMessageData");
  class PushEvent extends ExtendableEvent {
    #data;
    constructor(type, init = {}) {
      super(type, init);
      const d = init.data;
      this.#data = d === undefined || d === null ? null : new PushMessageData(typeof d === "string" ? new TextEncoder().encode(d) : ArrayBuffer.isView(d) ? new Uint8Array(d.buffer.slice(d.byteOffset, d.byteOffset + d.byteLength)) : new Uint8Array(d.slice(0)));
    }
    get data() { return this.#data; }
  }
  tag(PushEvent, "PushEvent");
  class NotificationEvent extends ExtendableEvent {
    #notification; #action;
    constructor(type, init) {
      super(type, init);
      if (!init || !init.notification) throw new TypeError("Failed to construct 'NotificationEvent': required member notification is undefined.");
      this.#notification = init.notification; this.#action = init.action === undefined ? "" : String(init.action);
    }
    get notification() { return this.#notification; }
    get action() { return this.#action; }
  }
  tag(NotificationEvent, "NotificationEvent");
  for (const [name, cls] of [["ExtendableEvent", ExtendableEvent], ["FetchEvent", FetchEvent], ["ExtendableMessageEvent", ExtendableMessageEvent], ["SyncEvent", SyncEvent],
    ["PeriodicSyncEvent", PeriodicSyncEvent], ["PushEvent", PushEvent], ["PushMessageData", PushMessageData], ["NotificationEvent", NotificationEvent]]) define(name, cls);

  // ---- Clients ----
  const clientToken = Symbol("Client");
  class Client {
    #id; #url; #type; #frameType; #port;
    constructor(token, d, port) { if (token !== clientToken) illegal(); this.#id = d.id; this.#url = d.url; this.#type = d.type || "window"; this.#frameType = d.frameType || "top-level"; this.#port = port; }
    get id() { return this.#id; }
    get url() { return this.#url; }
    get type() { return this.#type; }
    get frameType() { return this.#frameType; }
    postMessage(message, transfer) {
      if (arguments.length === 0) throw new TypeError("Failed to execute 'postMessage' on 'Client': 1 argument required, but only 0 present.");
      this.#port.postMessage({ t: "client-message", data: message, to: this.#id }, Array.isArray(transfer) ? transfer : (transfer && transfer.transfer) || []);
    }
    static port(client) { return client.#port; }
  }
  tag(Client, "Client");
  class WindowClient extends Client {
    #state;
    constructor(token, d, port) { super(token, d, port); this.#state = d; }
    get visibilityState() { return this.#state.visibilityState || "visible"; }
    get focused() { return !!this.#state.focused; }
    get ancestorOrigins() { return Object.freeze([]); }
    focus() { return Promise.resolve(this); }
    navigate(url) {
      const port = Client.port(this);
      return new Promise((resolve, reject) => {
        const id = nextCall++;
        calls.set(id, { resolve: () => resolve(this), reject });
        port.postMessage({ t: "navigate", url: String(url), id });
      });
    }
  }
  tag(WindowClient, "WindowClient");
  const makeClient = (record) => new WindowClient(clientToken, record.desc, record.port);
  class Clients {
    constructor() { illegal(); }
    get(id) { return Promise.resolve(Array.from(clientList.values()).filter((c) => c.desc.id === String(id)).map(makeClient)[0]); }
    matchAll(options) {
      const type = options && options.type !== undefined ? String(options.type) : "window";
      return Promise.resolve(type === "window" || type === "all" ? Array.from(clientList.values()).map(makeClient) : []);
    }
    openWindow(url) { return ask({ t: "open-window", url: String(url) }).then((d) => (d ? makeClient({ desc: d, port: primary().port }) : null), () => null); }
    claim() {
      for (const c of clientList.values()) if (c.desc.url.startsWith(info.scope)) c.port.postMessage({ t: "claimed" });
      return Promise.resolve(undefined);
    }
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
    static set(worker, state) { worker.#state = state; }
  }
  tag(ServiceWorker, "ServiceWorker");
  define("ServiceWorker", ServiceWorker);
  const self_ = new ServiceWorker(workerToken, info.url, info.state || "parsed");

  // Navigation preload: whether a navigation's request is also sent on to the network when the event begins, for the worker to use or ignore (event.preloadResponse).
  const preload = { enabled: false, headerValue: "true" };
  const preloadKey = "sw-preload-" + info.scope;
  try { const kept = kvRead(location.origin, preloadKey); if (kept) Object.assign(preload, JSON.parse(kept)); } catch (e) { /* (none kept) */ }
  const savePreload = () => { try { kvWrite(location.origin, preloadKey, JSON.stringify(preload)); } catch (e) { /* (not kept) */ } };
  class NavigationPreloadManager {
    constructor() { illegal(); }
    enable() { preload.enabled = true; savePreload(); return Promise.resolve(); }
    disable() { preload.enabled = false; savePreload(); return Promise.resolve(); }
    setHeaderValue(value) { preload.headerValue = String(value); savePreload(); return Promise.resolve(); }
    getState() { return Promise.resolve({ enabled: preload.enabled, headerValue: preload.headerValue }); }
  }
  tag(NavigationPreloadManager, "NavigationPreloadManager");
  define("NavigationPreloadManager", NavigationPreloadManager);

  // Background Sync and Periodic Background Sync: tags kept (on disk, so that they survive the worker), fired when the worker is there and the network is.
  const loadTags = (prefix) => { try { return new Map(Object.entries(JSON.parse(kvRead(location.origin, prefix + info.scope) || "{}"))); } catch (e) { return new Map(); } };
  const saveTags = (prefix, map) => { try { kvWrite(location.origin, prefix + info.scope, JSON.stringify(Object.fromEntries(map))); } catch (e) { /* (not kept) */ } };
  const syncTags = loadTags("sw-sync-"), periodicTags = loadTags("sw-psync-");
  const fireSync = (name) => {
    const entry = syncTags.get(name);
    if (!entry) return;
    const lastChance = (entry.tries || 0) >= 2;
    const event = new SyncEvent("sync", { tag: name, lastChance });
    dispatch(event);
    busy(ExtendableEvent.settle(event)).then(() => { syncTags.delete(name); saveTags("sw-sync-", syncTags); }, () => {
      entry.tries = (entry.tries || 0) + 1;
      if (lastChance) { syncTags.delete(name); saveTags("sw-sync-", syncTags); return; }
      saveTags("sw-sync-", syncTags);
      setTimeout(() => fireSync(name), 5000 * 2 ** entry.tries);
    });
  };
  const scheduleSync = (name) => setTimeout(() => { if (navigator.onLine !== false) fireSync(name); else addEventListener("online", () => fireSync(name), { once: true }); }, 0);
  const periodicTimers = new Map();
  const schedulePeriodic = (name) => {
    const entry = periodicTags.get(name);
    if (!entry || periodicTimers.has(name)) return;
    periodicTimers.set(name, setInterval(() => {
      const event = new PeriodicSyncEvent("periodicsync", { tag: name });
      dispatch(event);
      busy(ExtendableEvent.settle(event)).catch(() => {});
    }, Math.max(60000, entry.minInterval || 0)));
  };
  class SyncManager {
    constructor() { illegal(); }
    register(name) { name = String(name); if (!syncTags.has(name)) { syncTags.set(name, { tries: 0 }); saveTags("sw-sync-", syncTags); scheduleSync(name); } return Promise.resolve(); }
    getTags() { return Promise.resolve(Array.from(syncTags.keys())); }
  }
  tag(SyncManager, "SyncManager");
  class PeriodicSyncManager {
    constructor() { illegal(); }
    register(name, options) {
      name = String(name);
      periodicTags.set(name, { minInterval: Math.max(0, Number(options && options.minInterval) || 0) });
      saveTags("sw-psync-", periodicTags);
      schedulePeriodic(name);
      return Promise.resolve();
    }
    unregister(name) { name = String(name); periodicTags.delete(name); saveTags("sw-psync-", periodicTags); clearInterval(periodicTimers.get(name)); periodicTimers.delete(name); return Promise.resolve(); }
    getTags() { return Promise.resolve(Array.from(periodicTags.keys())); }
  }
  tag(PeriodicSyncManager, "PeriodicSyncManager");
  define("SyncManager", SyncManager);
  define("PeriodicSyncManager", PeriodicSyncManager);
  for (const name of periodicTags.keys()) schedulePeriodic(name);
  for (const name of syncTags.keys()) scheduleSync(name);

  // Notifications shown by this registration: kept here, since a worker has no window to show one in; the program's UI is told through the page, and a click comes back as an event.
  const shown = [];
  class WorkerNotification extends EventTarget {
    #o;
    constructor(title, options = {}) {
      super();
      this.#o = { title: String(title), body: options.body === undefined ? "" : String(options.body), tag: options.tag === undefined ? "" : String(options.tag), icon: options.icon || "", data: options.data === undefined ? null : options.data, silent: options.silent === undefined ? null : !!options.silent, requireInteraction: !!options.requireInteraction, actions: Array.from(options.actions || []) };
    }
    get title() { return this.#o.title; } get body() { return this.#o.body; } get tag() { return this.#o.tag; } get icon() { return this.#o.icon; }
    get data() { return this.#o.data; } get silent() { return this.#o.silent; } get requireInteraction() { return this.#o.requireInteraction; } get actions() { return this.#o.actions; }
    close() { const at = shown.indexOf(this); if (at >= 0) shown.splice(at, 1); }
  }
  tag(WorkerNotification, "Notification");

  const registrationToken = Symbol("ServiceWorkerRegistration");
  class ServiceWorkerRegistration extends EventTarget {
    #navigationPreload = Object.create(NavigationPreloadManager.prototype);
    #sync = Object.create(SyncManager.prototype);
    #periodicSync = Object.create(PeriodicSyncManager.prototype);
    #push = null;
    constructor(token) { super(); if (token !== registrationToken) illegal(); }
    get scope() { return info.scope; }
    get updateViaCache() { return info.updateViaCache || "imports"; }
    get active() { return self_.state === "installing" || self_.state === "installed" ? null : self_; }
    get installing() { return self_.state === "installing" ? self_ : null; }
    get waiting() { return self_.state === "installed" ? self_ : null; }
    get navigationPreload() { return this.#navigationPreload; }
    get sync() { return this.#sync; }
    get periodicSync() { return this.#periodicSync; }
    get pushManager() { return this.#push || (this.#push = makePushManager ? makePushManager(info.scope, () => true) : undefined); }
    update() { return ask({ t: "update" }).then(() => this, () => this); }
    unregister() { return ask({ t: "unregister" }).catch(() => headlessUnregister()); }
    showNotification(title, options) {
      const n = new WorkerNotification(title, options);
      if (n.tag) { const at = shown.findIndex((x) => x.tag === n.tag); if (at >= 0) shown.splice(at, 1); }
      shown.push(n);
      return Promise.resolve();
    }
    getNotifications(filter) { return Promise.resolve(shown.filter((n) => !filter || filter.tag === undefined || n.tag === String(filter.tag))); }
  }
  tag(ServiceWorkerRegistration, "ServiceWorkerRegistration");
  define("ServiceWorkerRegistration", ServiceWorkerRegistration);
  // With no page to ask, the registration is taken out of the kept ones here, and the worker stops.
  const headlessUnregister = () => {
    try {
      const list = JSON.parse(kvRead(location.origin, KEY) || "[]").filter((r) => r.scope !== info.scope);
      kvWrite(location.origin, KEY, JSON.stringify(list));
    } catch (e) { /* (nothing kept) */ }
    setTimeout(() => stopSelf(), 0);
    return true;
  };

  // ---- The scope ----
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
  define("skipWaiting", function skipWaiting() { for (const c of clientList.values()) c.port.postMessage({ t: "skipWaiting" }); return Promise.resolve(); }, true);
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

  // ---- Requests: from a page's fetch, a subresource, or a navigation ----
  const requestOf = (d) => {
    const init = { method: d.method, headers: d.headers, credentials: d.credentials, cache: d.cache, redirect: d.redirect, referrer: d.referrer || undefined };
    if (d.body && d.method !== "GET" && d.method !== "HEAD") init.body = d.body;
    const mode = d.mode === "navigate" ? "same-origin" : d.mode;
    if (mode) init.mode = mode;
    let request;
    try { request = new Request(d.url, init); } catch (e) { delete init.mode; request = new Request(d.url, init); }
    if (d.mode === "navigate") {
      // (A Request cannot be made a navigation; this one says it is.)
      Object.defineProperty(request, "mode", { value: "navigate" });
      Object.defineProperty(request, "destination", { value: "document" });
    }
    return request;
  };
  const responseData = (response) => response.arrayBuffer().then((body) => ({
    status: response.status, statusText: response.statusText, headers: Array.from(response.headers.entries()), body, url: response.url, type: response.type, redirected: response.redirected,
  }));
  // The event is dispatched; the answer (or the lack of one) is given to `reply`.
  const serveFetch = (descriptor, clientId, resultingClientId, reply) => {
    let event;
    try {
      const request = requestOf(descriptor);
      let preloadResponse;
      if (descriptor.mode === "navigate" && preload.enabled) {
        preloadResponse = fetch(descriptor.url, { headers: { "Service-Worker-Navigation-Preload": preload.headerValue }, credentials: "include", redirect: "manual" }).catch(() => undefined);
        busy(preloadResponse);
      }
      event = new FetchEvent("fetch", { request, clientId, resultingClientId, preloadResponse, cancelable: true });
    } catch (error) {
      reply({ kind: "error", error: String(error && error.message || error) });
      return;
    }
    dispatch(event);
    const answer = FetchEvent.answer(event);
    if (!answer) { reply({ kind: "fallback" }); ExtendableEvent.close(event); return; }
    busy(answer.then((response) => {
      if (!(response instanceof Response)) throw new TypeError("The FetchEvent resulted in a network error response: the promise was resolved with something other than a Response.");
      return responseData(response);
    }).then(
      (data) => reply({ kind: "response", response: data }, [data.body]),
      (error) => reply({ kind: "error", error: String(error && error.message || error) })));
  };
  // The program asks for a navigation (no page asks): __solarSwNavigate(id, url, method, body, bodyType) answers through __solarSwNavigateDone(id, mode, status, contentType, body, url).
  define("__solarSwNavigate", function __solarSwNavigate(id, url, method, body, bodyType) {
    lastActivity = Date.now();
    const descriptor = { url, method, headers: bodyType ? [["content-type", bodyType]] : [], body };
    serveFetch({ ...descriptor, mode: "navigate", credentials: "include", cache: "default", redirect: "manual", referrer: "" }, "", random(), (result) => {
      if (result.kind === "fallback") navigationDone(id, 1, 0, "", undefined, "");
      else if (result.kind === "error") navigationDone(id, 2, 0, "", undefined, "");
      else {
        const d = result.response;
        const type = d.headers.find((h) => h[0].toLowerCase() === "content-type");
        navigationDone(id, 0, d.status, type ? type[1] : "", d.body, d.url || descriptor.url);
      }
    });
  });

  // ---- Push (the receiving end: nothing here is a push service) ----
  define("__solarSwPush", function __solarSwPush(data) {
    const event = new PushEvent("push", { data });
    dispatch(event);
    return busy(ExtendableEvent.settle(event));
  });
  define("__solarSwNotificationEvent", function __solarSwNotificationEvent(type, index, action) {
    const notification = shown[index];
    if (!notification) return Promise.resolve();
    const event = new NotificationEvent(type, { notification, action });
    dispatch(event);
    if (type === "notificationclose") notification.close();
    return busy(ExtendableEvent.settle(event));
  });

  // ---- What a page says ----
  const lifecycle = (step) => {
    const event = new ExtendableEvent(step);
    dispatch(event);
    return ExtendableEvent.settle(event);
  };
  const rpc = {
    "sync.register": (name) => SyncManager.prototype.register.call(null, name),
    "sync.getTags": () => SyncManager.prototype.getTags.call(null),
    "periodicSync.register": (name, options) => PeriodicSyncManager.prototype.register.call(null, name, options),
    "periodicSync.unregister": (name) => PeriodicSyncManager.prototype.unregister.call(null, name),
    "periodicSync.getTags": () => PeriodicSyncManager.prototype.getTags.call(null),
    "preload.enable": () => NavigationPreloadManager.prototype.enable.call(null),
    "preload.disable": () => NavigationPreloadManager.prototype.disable.call(null),
    "preload.setHeaderValue": (v) => NavigationPreloadManager.prototype.setHeaderValue.call(null, v),
    "preload.getState": () => NavigationPreloadManager.prototype.getState.call(null),
    "notifications.get": (filter) => registration.getNotifications(filter).then((list) => list.map((n) => ({ title: n.title, body: n.body, tag: n.tag, data: n.data }))),
    "notifications.show": (title, options) => registration.showNotification(title, options),
  };
  const handle = (record, e) => {
    const m = e.data;
    if (!m || typeof m !== "object") return;
    lastActivity = Date.now();
    const port = record.port;
    switch (m.t) {
      case "hello": record.desc = m.client; clientList.set(port, record); break;
      case "bye": clientList.delete(port); try { port.close(); } catch (x) { /* (gone) */ } lastActivity = Date.now(); break;
      case "state": ServiceWorker.set(self_, m.state); break;
      case "lifecycle":
        busy(lifecycle(m.step)).then(() => port.postMessage({ t: "done", step: m.step, ok: true }), (error) => {
          port.postMessage({ t: "done", step: m.step, ok: false, error: String(error && error.message || error) });
        });
        break;
      case "fetch":
        serveFetch(m.request, m.clientId, "", (result, transfer) => port.postMessage({ t: "fetch-result", id: m.id, ...result }, transfer || []));
        break;
      case "message": {
        const event = new ExtendableMessageEvent("message", { data: m.data, origin: info.origin, lastEventId: "", source: record.desc ? makeClient(record) : null, ports: [...(e.ports || [])] });
        dispatch(event);
        busy(ExtendableEvent.settle(event)).catch(() => {});
        break;
      }
      case "reply": {
        const call = calls.get(m.id);
        if (call) { calls.delete(m.id); if (m.error) call.reject(new DOMException(m.error, "InvalidStateError")); else call.resolve(m.value); }
        break;
      }
      case "rpc": {
        const run = rpc[m.call];
        if (!run) { port.postMessage({ t: "rpc-result", id: m.id, error: "unknown call" }); break; }
        Promise.resolve().then(() => run(...(m.args || []))).then((value) => port.postMessage({ t: "rpc-result", id: m.id, value }), (error) => port.postMessage({ t: "rpc-result", id: m.id, error: String(error && error.message || error) }));
        break;
      }
    }
  };
  // The program gives the worker a port for each page it serves.
  const byToken = new Map();
  define("__solarSwAttach", function __solarSwAttach(port, token) {
    const record = { port, desc: null, token };
    byToken.set(token, record);
    port.onmessage = (e) => handle(record, e);
    lastActivity = Date.now();
  });
  // The page that gave the port is gone (the program tells).
  define("__solarSwDetach", function __solarSwDetach(token) {
    const record = byToken.get(token);
    if (!record) return;
    byToken.delete(token);
    clientList.delete(record.port);
    try { record.port.close(); } catch (e) { /* (gone) */ }
    lastActivity = Date.now();
  });
})();
