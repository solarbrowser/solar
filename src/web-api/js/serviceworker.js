// Service workers (https://w3c.github.io/ServiceWorker/), the page's side: navigator.serviceWorker, the registrations of this origin (kept on disk, with the source of
// the worker that is active), the lifecycle (install, activate, skipWaiting, claim), messages, and fetch() of a controlled page going through the worker's fetch
// event. The worker is a realm like a dedicated worker's (src/web-api/Worker.cpp) with the scope of worker-js/serviceworker-scope.js, talked to over a MessageChannel.
// What is not here: navigations and the loads of subresources (images, styles, scripts) do not go through the worker, only fetch() does; a worker does not stop when it is
// idle, and does not outlive its page (the next page starts the worker again from the source that was kept); push, sync and navigation preload are stubs.
(() => {
  "use strict";
  // (The URL made here: a page may put its own in the place of the global -- soundcloud's iframes do, with one that throws 'Invalid URL' for the address of the document -- and what this script reads with it is not for the page to change.)
  const URL = globalThis.URL;
  const create = globalThis.__solarWorkerCreate, start = globalThis.__solarWorkerStart, terminate = globalThis.__solarWorkerTerminate;
  const kvRead = globalThis.__solarKvRead, kvWrite = globalThis.__solarKvWrite;
  const tellControlled = globalThis.__solarSwControlled, resourceDone = globalThis.__solarSwResourceDone;
  const isNested = globalThis.__solarNested;
  const mark = globalThis.__solarSwMark, adopt = globalThis.__solarSwAdopt, attach = globalThis.__solarWorkerAttach;
  const pushManager = globalThis.__solarPushManager;
  delete globalThis.__solarKvRead; delete globalThis.__solarKvWrite; delete globalThis.__solarSwControlled; delete globalThis.__solarSwResourceDone;
  delete globalThis.__solarNested; delete globalThis.__solarSwMark; delete globalThis.__solarSwAdopt; delete globalThis.__solarWorkerAttach; delete globalThis.__solarPushManager;
  if (typeof create !== "function" || typeof MessageChannel !== "function" || typeof document === "undefined" || typeof fetch !== "function") return;
  const nativeFetch = globalThis.fetch;
  const secure = typeof isSecureContext === "boolean" ? isSecureContext : true;
  const origin = location.origin;
  const KEY = "sw-registrations";

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
  const tag = (cls, name) => Object.defineProperty(cls.prototype, Symbol.toStringTag, { value: name, configurable: true });
  const hide = (name, value) => Object.defineProperty(globalThis, name, { value, writable: true, configurable: true, enumerable: false });
  const token = Symbol("sw");

  // ---- The state ----
  const registrations = [];  // records: { scope, scriptURL, source, updateViaCache, installing, waiting, active (workers), object (ServiceWorkerRegistration) }
  let controller = null;     // the worker that controls this page
  let clientId = "";
  const clientIdOf = () => clientId || (clientId = crypto.randomUUID());
  const matching = (href) => {
    let best = null;
    for (const r of registrations) if (href.startsWith(r.scope) && (!best || r.scope.length > best.scope.length)) best = r;
    return best;
  };

  // ---- ServiceWorker objects ----
  class ServiceWorker extends EventTarget {
    #w;
    constructor(t, w) { super(); if (t !== token) throw new TypeError("Illegal constructor"); this.#w = w; }
    get scriptURL() { return this.#w.url; }
    get state() { return this.#w.state; }
    postMessage(message, transfer) {
      if (arguments.length === 0) throw new TypeError("Failed to execute 'postMessage' on 'ServiceWorker': 1 argument required, but only 0 present.");
      if (this.#w.state === "redundant") throw new DOMException("ServiceWorker is in redundant state.", "InvalidStateError");
      this.#w.port.postMessage({ t: "message", data: message, client: clientDescriptor() }, Array.isArray(transfer) ? transfer : (transfer && transfer.transfer) || []);
    }
  }
  tag(ServiceWorker, "ServiceWorker");
  handler(ServiceWorker.prototype, ["statechange", "error"]);
  hide("ServiceWorker", ServiceWorker);
  const creationURL = location.href;  // (what a client's url is: the address the document was created for, not the one pushState has made of it)
  const clientDescriptor = () => ({ id: clientIdOf(), url: creationURL, type: "window", frameType: (isNested ? isNested() : window.top !== window) ? "nested" : "top-level", visibilityState: document.visibilityState, focused: typeof document.hasFocus === "function" ? document.hasFocus() : true });

  // Tells the worker who this page is, and whether the worker controls it (a client that is not controlled is not listed by clients.matchAll unless asked for).
  const announce = (w) => { try { if (w && w.port && !w.gone) w.port.postMessage({ t: "hello", client: { ...clientDescriptor(), controlled: controller === w } }); } catch (e) { /* (gone) */ } };

  const workerClientsServed = new Map();  // client id -> the address of the worker
  // The workers this page makes are clients of the workers that serve it, as the page is: Worker and SharedWorker say so through this.
  Object.defineProperty(globalThis, "__solarSwWorkerClient", {
    value(url, kind) {
      const id = crypto.randomUUID();
      const scope = controller && controller.reg ? controller.reg.scope : null;
      for (const r of registrations) {
        const w = r.active;
        if (!w || !w.port || w.gone) continue;
        try { w.port.postMessage({ t: "client", op: "add", client: { id, url, type: kind, frameType: "none", visibilityState: "hidden", focused: false, controlled: url.startsWith("blob:") || url.startsWith("data:") ? w === controller : matching(url) === r } }); } catch (e) { /* (gone) */ }
      }
      workerClientsServed.set(id, url);
      return { id, controlled: true };
    },
    writable: true, configurable: true, enumerable: false,
  });
  // A request of a worker that is served (its fetch asks the page, because the worker has no port to the service worker): the service worker is asked with the worker as the client.
  Object.defineProperty(globalThis, "__solarSwWorkerFetch", {
    value(clientId, request) {
      // (Which worker serves it is asked now: the page may have been claimed since the worker was made.)
      const url = workerClientsServed.get(clientId);
      const serving = url === undefined ? null : registrations.find((r) => r.active && r.active.port && !r.active.gone && (url.startsWith("blob:") || url.startsWith("data:") ? r.active === controller : matching(url) === r));
      const w = serving ? serving.active : null;
      if (!w) return Promise.resolve({ kind: "fallback" });
      return new Promise((resolve, reject) => {
        const id = nextFetch++;
        w.fetches.set(id, { resolve, reject });
        w.port.postMessage({ t: "fetch", id, clientId, request });
      }).catch(() => ({ kind: "error" }));
    },
    writable: true, configurable: true, enumerable: false,
  });

  // ---- Workers: made, told their state, asked ----
  const setState = (w, state) => {
    w.state = state;
    try { if (w.port) w.port.postMessage({ t: "state", state }); } catch (e) { /* (gone) */ }
    w.object.dispatchEvent(new Event("statechange"));
  };
  // A worker for the registration: made from its source, or (`existing`) one that is already running, which has kept running since an earlier page. Either way this page
  // gives it a port of its own, and says who it is.
  const spawn = (reg, scriptURL, source, state, existing) => {
    let id = existing;
    const w = { id: -1, port: null, state, url: scriptURL, object: null, waiters: new Map(), fetches: new Map(), rpcs: new Map(), skip: false, reg, failed: "" };
    w.object = new ServiceWorker(token, w);
    if (id === undefined) {
      id = create(scriptURL);
      if (id < 0) throw new TypeError("Failed to register a ServiceWorker: workers are not available.");
      mark(id, origin, reg.scope);
      const info = JSON.stringify({
        url: scriptURL, name: "", type: "classic", serviceWorker: true, scope: reg.scope, state, updateViaCache: reg.updateViaCache, origin,
        userAgent: navigator.userAgent, language: navigator.language, languages: Array.from(navigator.languages || [navigator.language]), platform: navigator.platform,
        hardwareConcurrency: navigator.hardwareConcurrency, onLine: navigator.onLine, appVersion: navigator.appVersion,
      });
      w.id = id;
      const unused = new MessageChannel();  // (a worker is started with a port; a service worker is given its ports one for each page, below)
      start(id, unused.port2, source, scriptURL, info, (message) => { if (!w.failed) w.failed = String(message); return false; }, false);
      if (w.failed) return w;
    }
    w.id = id;
    const channel = new MessageChannel();
    w.port = channel.port1;
    w.port.onmessage = (e) => fromWorker(w, e);
    if (!attach(id, channel.port2)) { w.failed = "the worker could not be reached"; return w; }
    announce(w);
    // (A frame is not yet in its parent when its scripts first run: what it is is told again once it is.)
    setTimeout(() => announce(w), 0);
    addEventListener("pagehide", () => { try { w.port.postMessage({ t: "bye" }); } catch (e) { /* (gone) */ } });
    return w;
  };
  const retire = (w) => {
    if (w.state === "redundant" && w.gone) return;
    w.gone = true;
    for (const f of w.fetches.values()) f.reject(new TypeError("Failed to fetch"));
    w.fetches.clear();
    if (w.state !== "redundant") setState(w, "redundant");
    try { terminate(w.id); } catch (e) { /* (already) */ }
    try { if (w.port) w.port.close(); } catch (e) { /* (already) */ }
    for (const f of w.rpcs.values()) f.reject(new DOMException("The service worker is gone.", "InvalidStateError"));
    w.rpcs.clear();
    if (controller === w) { controller = null; if (tellControlled) tellControlled(false); }
  };
  const step = (w, name) => new Promise((resolve) => {
    w.waiters.set(name, resolve);
    w.port.postMessage({ t: "lifecycle", step: name });
  });
  const persist = () => {
    const list = [];
    for (const r of registrations) {
      if (r.active) list.push({ scope: r.scope, scriptURL: r.active.url, source: r.activeSource, updateViaCache: r.updateViaCache });
      else if (r.stored) list.push({ scope: r.scope, scriptURL: r.stored.scriptURL, source: r.stored.source, updateViaCache: r.updateViaCache });
      // (A registration whose worker is not active yet is there all the same: it keeps a page from being claimed by a shorter scope.)
      else if (r.installing || r.waiting) list.push({ scope: r.scope, pending: true, updateViaCache: r.updateViaCache });
    }
    kvWrite(origin, KEY, JSON.stringify(list));
  };
  let readyResolve = null;
  const checkReady = () => {
    if (!readyResolve) return;
    const r = matching(location.href);
    if (r && r.active) { readyResolve(r.object); readyResolve = null; }
  };
  const setController = (w) => {
    if (controller === w) return;
    const old = controller;
    controller = w;
    announce(old);
    announce(w);
    if (tellControlled) tellControlled(!!w);
    containerObject.dispatchEvent(new Event("controllerchange"));
  };

  // ---- Lifecycle ----
  const install = async (reg, w, source) => {
    reg.installing = w;
    setState(w, "installing");
    // (After register() has answered: a page listens for it once it has the registration.)
    setTimeout(() => reg.object.dispatchEvent(new Event("updatefound")), 0);
    const done = await step(w, "install");
    if (reg.installing === w) reg.installing = null;
    if (!done.ok || w.gone) { retire(w); return; }
    reg.waiting = w;
    reg.waitingSource = source;
    setState(w, "installed");
    // A worker that is waiting takes over when nothing it would replace is in use, or when it asks (skipWaiting).
    if (!reg.active || w.skip || controller !== reg.active) await activate(reg, w);
  };
  const activate = async (reg, w) => {
    if (reg.waiting !== w || w.gone) return;
    reg.waiting = null;
    const old = reg.active;
    reg.active = w;
    reg.activeSource = reg.waitingSource;
    persist();  // (a page that opens now finds the worker as soon as it is the active one)
    setState(w, "activating");
    const done = await step(w, "activate");
    if (!done.ok || w.gone) {
      reg.active = old;
      retire(w);
      return;
    }
    setState(w, "activated");
    if (old) {
      const was = controller === old;
      retire(old);
      if (was) setController(w);
    }
    persist();
    checkReady();
    // The other pages this registration controlled have a new worker: they are told.
    try { const channel = new BroadcastChannel("solar-sw-claim:" + origin); channel.postMessage({ t: "activated", scope: reg.scope }); channel.close(); } catch (e) { /* (no channel) */ }
  };
  const unregisterRecord = (reg) => {
    const at = registrations.indexOf(reg);
    if (at < 0) return false;
    registrations.splice(at, 1);
    for (const w of [reg.installing, reg.waiting, reg.active]) if (w) retire(w);
    reg.installing = reg.waiting = reg.active = null;
    persist();
    return true;
  };

  // ---- What the worker says ----
  const fromWorker = (w, e) => {
    const m = e.data;
    if (!m || typeof m !== "object") return;
    const reg = w.reg;
    const reply = (id, value, error) => { try { w.port.postMessage({ t: "reply", id, value, error }); } catch (x) { /* (gone) */ } };
    switch (m.t) {
      case "done": { const f = w.waiters.get(m.step); if (f) { w.waiters.delete(m.step); f(m); } break; }
      case "fetch-result": { const f = w.fetches.get(m.id); if (f) { w.fetches.delete(m.id); f.resolve(m); } break; }
      case "client-message":
        if (m.to === clientIdOf()) containerObject.dispatchEvent(new MessageEvent("message", { data: m.data, origin, source: w.object, ports: [...(e.ports || [])] }));
        break;
      case "skipWaiting":
        w.skip = true;
        if (reg.waiting === w) activate(reg, w);
        break;
      case "rpc-result": { const f = w.rpcs.get(m.id); if (f) { w.rpcs.delete(m.id); if (m.error) f.reject(new DOMException(m.error, "InvalidStateError")); else f.resolve(m.value); } break; }
      case "claimed": if (reg.active === w && location.href.startsWith(reg.scope)) setController(w); break;
      case "clients": reply(m.id, [clientDescriptor()]); break;
      case "describe": reply(m.id, { ...clientDescriptor(), controlled: controller === w }); break;
      case "claim":
        if (reg.active === w && location.href.startsWith(reg.scope)) { setController(w); reply(m.id, undefined); } else reply(m.id, undefined, "The service worker is not active.");
        break;
      case "update": updateRecord(reg).then(() => reply(m.id, undefined), (error) => reply(m.id, undefined, String(error && error.message || error))); break;
      case "unregister": reply(m.id, unregisterRecord(reg)); break;
      case "navigate": try { location.href = m.url; reply(m.id, undefined); } catch (x) { reply(m.id, undefined, "navigate failed"); } break;
      case "open-window": reply(m.id, null); break;
    }
  };

  // A question for the registration's worker (the one that is active, or else the one that is coming): its managers (sync, preload) and notifications are its own.
  let nextRpc = 1;
  const rpc = (reg, call, args) => new Promise((resolve, reject) => {
    let w = reg.active || reg.waiting || reg.installing;
    if (!w && reg.stored) {
      // (A registration of the origin that this page does not have a worker for: its kept worker is started for it.)
      w = spawn(reg, reg.stored.scriptURL, reg.stored.source, "activated");
      if (w.failed) { retire(w); reject(new DOMException("The service worker could not be started.", "InvalidStateError")); return; }
      reg.active = w;
    }
    if (!w || !w.port) { reject(new DOMException("There is no service worker.", "InvalidStateError")); return; }
    const id = nextRpc++;
    w.rpcs.set(id, { resolve, reject });
    w.port.postMessage({ t: "rpc", id, call, args });
  });

  // ---- ServiceWorkerRegistration ----
  const stub = (name, methods) => { const o = {}; for (const [k, v] of Object.entries(methods)) o[k] = v; return Object.freeze(Object.setPrototypeOf(o, { [Symbol.toStringTag]: name })); };
  class ServiceWorkerRegistration extends EventTarget {
    #r;
    constructor(t, r) { super(); if (t !== token) throw new TypeError("Illegal constructor"); this.#r = r; }
    get scope() { return this.#r.scope; }
    get updateViaCache() { return this.#r.updateViaCache; }
    get installing() { return this.#r.installing ? this.#r.installing.object : null; }
    get waiting() { return this.#r.waiting ? this.#r.waiting.object : null; }
    get active() { return this.#r.active && this.#r.active.state !== "installing" ? this.#r.active.object : null; }
    get navigationPreload() {
      const call = (name, ...args) => rpc(this.#r, "preload." + name, args);
      return stub("NavigationPreloadManager", { enable: () => call("enable"), disable: () => call("disable"), setHeaderValue: (v) => call("setHeaderValue", v), getState: () => call("getState") });
    }
    get sync() { return stub("SyncManager", { register: (tag) => rpc(this.#r, "sync.register", [String(tag)]), getTags: () => rpc(this.#r, "sync.getTags", []) }); }
    get periodicSync() {
      return stub("PeriodicSyncManager", {
        register: (tag, options) => rpc(this.#r, "periodicSync.register", [String(tag), { minInterval: Number(options && options.minInterval) || 0 }]),
        unregister: (tag) => rpc(this.#r, "periodicSync.unregister", [String(tag)]), getTags: () => rpc(this.#r, "periodicSync.getTags", []),
      });
    }
    get pushManager() { return this.#r.push || (this.#r.push = pushManager ? pushManager(this.#r.scope, () => !!this.#r.active) : undefined); }
    update() { return updateRecord(this.#r).then(() => this); }
    unregister() { return Promise.resolve(unregisterRecord(this.#r)); }
    showNotification(title, options) { return rpc(this.#r, "notifications.show", [String(title), options ? JSON.parse(JSON.stringify(options)) : {}]).then(() => undefined); }
    getNotifications(filter) { return rpc(this.#r, "notifications.get", [filter ? { tag: filter.tag } : undefined]).then((list) => list.map((n) => Object.assign(Object.create(null), n, { close() {} }))); }
  }
  tag(ServiceWorkerRegistration, "ServiceWorkerRegistration");
  handler(ServiceWorkerRegistration.prototype, ["updatefound"]);
  hide("ServiceWorkerRegistration", ServiceWorkerRegistration);
  const newRecord = (scope, updateViaCache) => {
    const r = { scope, updateViaCache: updateViaCache || "imports", installing: null, waiting: null, active: null, activeSource: "", waitingSource: "", object: null };
    r.object = new ServiceWorkerRegistration(token, r);
    registrations.push(r);
    return r;
  };

  // ---- Fetching the script, and updating ----
  const scriptTypes = /^(text|application)\/(x-)?(java|ecma)script$/i;
  const loadScript = async (url, bypass) => {
    const response = await nativeFetch.call(globalThis, url, { cache: bypass ? "no-cache" : "default", credentials: "same-origin", redirect: "follow" });
    if (!response.ok) throw new TypeError(`Failed to register a ServiceWorker for scope: A bad HTTP response code (${response.status}) was received when fetching the script.`);
    const type = (response.headers.get("content-type") || "").split(";")[0].trim();
    if (!scriptTypes.test(type)) throw new DOMException(`Failed to register a ServiceWorker: The script has an unsupported MIME type ('${type}').`, "SecurityError");
    return { source: await response.text(), allowed: response.headers.get("service-worker-allowed") };
  };
  const updateRecord = async (reg) => {
    const url = (reg.active || reg.waiting || reg.installing).url;
    const { source } = await loadScript(url, true);
    if (source === (reg.waitingSource || reg.activeSource)) return;
    const w = spawn(reg, url, source, "parsed");
    if (w.failed) { retire(w); throw new TypeError("ServiceWorker script evaluation failed"); }
    if (reg.installing) retire(reg.installing);
    install(reg, w, source);
  };

  // ---- ServiceWorkerContainer ----
  class ServiceWorkerContainer extends EventTarget {
    #ready = null;
    constructor(t) { super(); if (t !== token) throw new TypeError("Illegal constructor"); }
    get controller() { return controller && controller.state !== "redundant" ? controller.object : null; }
    get ready() {
      if (!this.#ready) {
        this.#ready = new Promise((resolve) => { readyResolve = resolve; checkReady(); });
      }
      return this.#ready;
    }
    startMessages() {}
    register(scriptURL, options) {
      return (async () => {
        if (arguments.length === 0) throw new TypeError("Failed to execute 'register' on 'ServiceWorkerContainer': 1 argument required, but only 0 present.");
        if (!secure) throw new DOMException("Failed to register a ServiceWorker: The operation is insecure.", "SecurityError");
        const url = new URL(String(scriptURL), document.baseURI);
        if (url.origin !== origin) throw new DOMException(`Failed to register a ServiceWorker: The origin of the provided scriptURL ('${url.origin}') does not match the current origin ('${origin}').`, "SecurityError");
        if (/%2f|%5c/i.test(url.pathname)) throw new TypeError("Failed to register a ServiceWorker: The provided scriptURL ('" + url.href + "') includes a disallowed escape character.");
        url.hash = "";
        const scopeUrl = options && options.scope !== undefined ? new URL(String(options.scope), document.baseURI) : new URL("./", url);
        scopeUrl.hash = "";
        if (scopeUrl.origin !== origin) throw new DOMException("Failed to register a ServiceWorker: The origin of the provided scope does not match the current origin.", "SecurityError");
        const { source, allowed } = await loadScript(url.href, options && options.updateViaCache === "none");
        const maxScope = allowed ? new URL(allowed, url).pathname : new URL("./", url).pathname;
        if (!scopeUrl.pathname.startsWith(maxScope)) {
          throw new DOMException(`Failed to register a ServiceWorker: The path of the provided scope ('${scopeUrl.pathname}') is not under the max scope allowed ('${maxScope}'). Adjust the scope, move the Service Worker script, or use the Service-Worker-Allowed HTTP header to allow the scope.`, "SecurityError");
        }
        const scope = scopeUrl.href;
        let reg = registrations.find((r) => r.scope === scope);
        const existing = reg && (reg.active || reg.waiting || reg.installing);
        if (existing && existing.url === url.href && source === (reg.waitingSource || reg.activeSource)) return reg.object;
        const fresh = !reg;
        if (!reg) reg = newRecord(scope, options && options.updateViaCache);
        const w = spawn(reg, url.href, source, "parsed");
        if (w.failed) {
          retire(w);
          if (fresh) registrations.splice(registrations.indexOf(reg), 1);
          throw new TypeError(`Failed to register a ServiceWorker for scope ('${scope}') with script ('${url.href}'): ServiceWorker script evaluation failed`);
        }
        if (reg.installing) retire(reg.installing);
        install(reg, w, source);
        persist();
        return reg.object;
      })();
    }
    getRegistration(clientURL) {
      return new Promise((resolve) => {
        const href = clientURL === undefined ? location.href : new URL(String(clientURL), document.baseURI).href;
        const r = matching(href);
        resolve(r ? r.object : undefined);
      });
    }
    getRegistrations() { return Promise.resolve(registrations.map((r) => r.object)); }
  }
  tag(ServiceWorkerContainer, "ServiceWorkerContainer");
  handler(ServiceWorkerContainer.prototype, ["controllerchange", "message", "messageerror", "error"]);
  hide("ServiceWorkerContainer", ServiceWorkerContainer);
  const containerObject = new ServiceWorkerContainer(token);

  // ---- A controlled page's fetch() goes to the worker first ----
  let nextFetch = 1;
  const nullBody = (status) => status === 101 || status === 204 || status === 205 || status === 304;
  // The referrer policy the requests of the document have unless they say: the one of its meta, else the default.
  const documentReferrerPolicy = () => {
    const meta = document.querySelector('meta[name="referrer" i]');
    const value = meta ? (meta.getAttribute("content") || "").trim().toLowerCase() : "";
    return value || "strict-origin-when-cross-origin";
  };
  const intercepted = async (w, input, init, thisArg) => {
    let request;
    // A Request of another realm (a page's frame is asked with its parent's) is rebuilt here from what it holds.
    if (typeof input === "object" && input !== null && !(input instanceof Request) && Object.prototype.toString.call(input) === "[object Request]") {
      const base = { method: input.method, headers: Array.from(input.headers.entries()), mode: input.mode === "navigate" ? "same-origin" : input.mode, credentials: input.credentials, cache: input.cache, redirect: input.redirect, referrer: input.referrer === "about:client" ? undefined : input.referrer, signal: input.signal };
      if (input.method !== "GET" && input.method !== "HEAD") base.body = await input.clone().arrayBuffer();
      input = new Request(input.url, base);
    }
    request = new Request(input, init);
    const url = new URL(request.url);
    if (url.protocol !== "http:" && url.protocol !== "https:") return nativeFetch.call(thisArg, input, init);
    const signal = request.signal;
    if (signal && signal.aborted) throw signal.reason !== undefined ? signal.reason : new DOMException("The user aborted a request.", "AbortError");
    const body = request.method === "GET" || request.method === "HEAD" ? undefined : await request.clone().arrayBuffer();
    const id = nextFetch++;
    const result = await new Promise((resolve, reject) => {
      w.fetches.set(id, { resolve, reject });
      if (signal) signal.addEventListener("abort", () => { if (w.fetches.delete(id)) reject(signal.reason !== undefined ? signal.reason : new DOMException("The user aborted a request.", "AbortError")); }, { once: true });
      w.port.postMessage({
        t: "fetch", id, clientId: clientIdOf(),
        request: { url: request.url, method: request.method, headers: Array.from(request.headers.entries()), body, mode: request.mode, credentials: request.credentials, cache: request.cache, redirect: request.redirect, referrer: request.referrer === "about:client" ? location.href : request.referrer, referrerPolicy: request.referrerPolicy || documentReferrerPolicy() },
      });
    });
    if (result.kind === "fallback") return nativeFetch.call(thisArg, input, init);
    if (result.kind === "error") throw new TypeError("Failed to fetch");
    const d = result.response;
    const response = new Response(nullBody(d.status) ? null : d.body, { status: d.status, statusText: d.statusText, headers: d.headers });
    try {
      Object.defineProperty(response, "url", { value: d.url || request.url, configurable: true });
      Object.defineProperty(response, "type", { value: url.origin === origin ? "basic" : "cors", configurable: true });
      if (d.redirected) Object.defineProperty(response, "redirected", { value: true, configurable: true });
    } catch (e) { /* (read-only here) */ }
    return response;
  };
  // The program asks for each resource of a controlled page (pictures, styles, scripts) through this, before the network: true if the worker will answer.
  Object.defineProperty(globalThis, "__solarSwResource", {
    value(url, id) {
      const w = controller;
      if (!w || w.state === "redundant" || w.gone || !resourceDone) return false;
      const u = new URL(url);
      if (u.protocol !== "http:" && u.protocol !== "https:") return false;
      const fid = nextFetch++;
      new Promise((resolve, reject) => {
        w.fetches.set(fid, { resolve, reject });
        w.port.postMessage({
          t: "fetch", id: fid, clientId: clientIdOf(),
          request: { url, method: "GET", headers: [], mode: u.origin === origin ? "same-origin" : "no-cors", credentials: "include", cache: "default", redirect: "follow", referrer: location.href },
        });
      }).then((result) => {
        if (result.kind === "fallback") resourceDone(id, 1, 0, "", undefined);
        else if (result.kind === "error") resourceDone(id, 2, 0, "", undefined);
        else {
          const d = result.response;
          const type = d.headers.find((h) => h[0].toLowerCase() === "content-type");
          resourceDone(id, 0, d.status, type ? type[1] : "", d.body);
        }
      }, () => resourceDone(id, 2, 0, "", undefined));
      return true;
    },
    writable: true, configurable: true, enumerable: false,
  });
  const descriptor = Object.getOwnPropertyDescriptor(globalThis, "fetch");
  Object.defineProperty(globalThis, "fetch", {
    ...descriptor,
    value: { fetch(input, init) {
      const w = controller;
      if (!w || w.state === "redundant" || w.gone) return nativeFetch.apply(this, arguments);
      return intercepted(w, input, init, this);
    } }.fetch,
  });

  // ---- On this page's load: the registrations that were kept, and the worker that controls it ----
  if (secure) {
    // The registrations that were kept (by this page or by others), and the worker that serves this page, if one is running or kept for it. `claimed` is for a worker that
    // took the page over (clients.claim()) while it was already open.
    const adoptKept = (claimedScope) => {
      const text = kvRead(origin, KEY);
      if (!text) return;
      let kept = [];
      try { kept = JSON.parse(text); } catch (e) { kept = []; }
      for (const k of kept) {
        if (registrations.some((r) => r.scope === k.scope)) continue;
        const reg = newRecord(k.scope, k.updateViaCache);
        if (k.pending) continue;
        reg.activeSource = k.source;
        reg.stored = { scriptURL: k.scriptURL, source: k.source };
      }
      // (A page is claimed by the registration whose scope matches its address the most: a longer one that is there keeps it.)
      const best = matching(location.href);
      const reg = claimedScope ? (best && best.scope === claimedScope ? best : null) : best;
      if (reg && (!reg.active || claimedScope)) {
        if (reg.active && controller === reg.active) return;
        const k = kept.find((x) => x.scope === reg.scope);
        if (!k || k.pending) return;
        // The worker may be there already (kept since an earlier page, or started for the navigation that made this page): then it is that one this page is served by.
        const running = adopt(origin, location.href);
        const w = spawn(reg, k.scriptURL, k.source, "activated", running >= 0 ? running : undefined);
        if (w.failed) { retire(w); } else { reg.active = w; if (claimedScope) setController(w); else { controller = w; if (tellControlled) tellControlled(true); announce(w); } }
      }
    };
    // A page that a registration controls, when the registration has a new active worker (it activated in another page): the page goes on with that one.
    const replaceActive = (scope) => {
      const text = kvRead(origin, KEY);
      if (!text) return;
      let kept = [];
      try { kept = JSON.parse(text); } catch (e) { kept = []; }
      for (const k of kept) {
        if (registrations.some((r) => r.scope === k.scope)) continue;
        const added = newRecord(k.scope, k.updateViaCache);
        if (k.pending) continue;
        added.activeSource = k.source;
        added.stored = { scriptURL: k.scriptURL, source: k.source };
      }
      const best = matching(location.href);
      if (!best || best.scope !== scope || !best.active || controller !== best.active) return;
      const k = kept.find((x) => x.scope === scope);
      if (!k || k.pending || (best.active.url === k.scriptURL && best.activeSource === k.source)) return;
      const old = best.active;
      const running = adopt(origin, location.href);
      const w = spawn(best, k.scriptURL, k.source, "activated", running >= 0 ? running : undefined);
      if (w.failed) return;
      best.active = w;
      best.activeSource = k.source;
      setController(w);
      // (The old worker is not ended here: it is the other page's to end, and may be serving others.)
      old.gone = true;
      try { old.port.postMessage({ t: "bye" }); old.port.close(); } catch (e) { /* (gone) */ }
    };
    adoptKept();
    // A worker that claims the pages of its scope tells them all, the ones that are open as well.
    try {
      const claims = new BroadcastChannel("solar-sw-claim:" + origin);
      claims.onmessage = (e) => {
        if (!e.data) return;
        if (e.data.t === "claim" && location.href.startsWith(e.data.scope)) adoptKept(e.data.scope);
        else if (e.data.t === "activated" && location.href.startsWith(e.data.scope)) replaceActive(e.data.scope);
      };
      // (Held by the window: a channel nothing refers to is collected, and with it the way a claim reaches this page.)
      Object.defineProperty(globalThis, Symbol.for("solar.swClaims"), { value: claims, configurable: true });
    } catch (e) { /* (no channel) */ }
    Object.defineProperty(Navigator.prototype, "serviceWorker", { get() { return containerObject; }, enumerable: true, configurable: true });
  }
})();
