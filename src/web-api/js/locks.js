// Web Locks (https://w3c.github.io/web-locks/): navigator.locks, for the pages and workers of this realm's process-wide lock table (named, exclusive or shared,
// waiting in the order asked, ifAvailable, steal, an AbortSignal).
(() => {
  "use strict";
  const proto = typeof Navigator === "function" ? Navigator.prototype : typeof WorkerNavigator === "function" ? WorkerNavigator.prototype : null;
  const owner = typeof navigator === "object" && navigator ? navigator : null;
  if (!owner) return;
  const abortError = (signal) => (signal && signal.reason !== undefined ? signal.reason : new DOMException("The lock request is aborted", "AbortError"));
  const state = new Map();  // name -> { held: [lock], waiting: [request] }
  const entry = (name) => { let e = state.get(name); if (!e) { e = { held: [], waiting: [] }; state.set(name, e); } return e; };
  let counter = 0;

  class Lock {
    #name; #mode;
    constructor(token, name, mode) { if (token !== Lock) throw new TypeError("Illegal constructor"); this.#name = name; this.#mode = mode; }
    get name() { return this.#name; }
    get mode() { return this.#mode; }
  }
  Object.defineProperty(Lock.prototype, Symbol.toStringTag, { value: "Lock", configurable: true });

  const grantable = (e, mode) => (mode === "shared" ? e.held.every((l) => l.mode === "shared") : e.held.length === 0);
  const pump = (name) => {
    const e = state.get(name);
    if (!e) return;
    while (e.waiting.length && grantable(e, e.waiting[0].mode)) {
      const request = e.waiting.shift();
      grant(name, e, request);
    }
    if (!e.held.length && !e.waiting.length) state.delete(name);
  };
  const grant = (name, e, request) => {
    const lock = new Lock(Lock, name, request.mode);
    const record = { mode: request.mode, lock, release: null, stolen: false };
    e.held.push(record);
    Promise.resolve().then(() => request.callback(lock)).then(
      (value) => { release(name, record); request.resolve(value); },
      (error) => { release(name, record); request.reject(error); });
    record.release = () => release(name, record);
  };
  const release = (name, record) => {
    const e = state.get(name);
    if (!e) return;
    const at = e.held.indexOf(record);
    if (at >= 0) e.held.splice(at, 1);
    pump(name);
  };

  class LockManager {
    constructor() { throw new TypeError("Illegal constructor"); }
    request(name, a, b) {
      return new Promise((resolve, reject) => {
        if (arguments.length < 2) throw new TypeError("Failed to execute 'request' on 'LockManager': 2 arguments required.");
        let options = {}, callback = a;
        if (typeof a !== "function") { options = a || {}; callback = b; }
        if (typeof callback !== "function") throw new TypeError("Failed to execute 'request' on 'LockManager': The callback provided as parameter is not a function.");
        name = String(name);
        if (name.startsWith("-")) throw new DOMException("Names cannot start with '-'.", "NotSupportedError");
        const mode = options.mode === undefined ? "exclusive" : String(options.mode);
        if (mode !== "exclusive" && mode !== "shared") throw new TypeError(`The provided value '${mode}' is not a valid enum value of type LockMode.`);
        if (options.steal && options.ifAvailable) throw new DOMException("The 'steal' and 'ifAvailable' options cannot be used together.", "NotSupportedError");
        if (options.steal && mode !== "exclusive") throw new DOMException("The 'steal' option can only be used with 'exclusive' locks.", "NotSupportedError");
        if (options.signal && (options.steal || options.ifAvailable)) throw new DOMException("The 'signal' option cannot be used with 'steal' or 'ifAvailable'.", "NotSupportedError");
        const signal = options.signal;
        if (signal && signal.aborted) { reject(abortError(signal)); return; }
        const e = entry(name);
        const request = { mode, callback, resolve, reject };
        if (options.steal) {
          // The held locks are lost: their holders' promises reject with an AbortError, and this one goes first.
          for (const record of e.held.splice(0)) record.stolen = true;
          e.waiting.unshift(request);
          pump(name);
          return;
        }
        if (options.ifAvailable) {
          if (grantable(e, mode) && e.waiting.length === 0) { grant(name, e, request); }
          else { Promise.resolve().then(() => callback(null)).then(resolve, reject); if (!e.held.length && !e.waiting.length) state.delete(name); }
          return;
        }
        if (grantable(e, mode) && e.waiting.length === 0) { grant(name, e, request); return; }
        e.waiting.push(request);
        if (signal) signal.addEventListener("abort", () => {
          const at = e.waiting.indexOf(request);
          if (at >= 0) { e.waiting.splice(at, 1); reject(abortError(signal)); pump(name); }
        }, { once: true });
      });
    }
    query() {
      const snapshot = { held: [], pending: [] };
      for (const [name, e] of state) {
        for (const l of e.held) snapshot.held.push({ name, mode: l.mode, clientId: "main" });
        for (const w of e.waiting) snapshot.pending.push({ name, mode: w.mode, clientId: "main" });
      }
      return Promise.resolve(snapshot);
    }
  }
  Object.defineProperty(LockManager.prototype, Symbol.toStringTag, { value: "LockManager", configurable: true });
  const manager = Object.create(LockManager.prototype);
  Object.defineProperty(proto || owner, "locks", { get() { return manager; }, enumerable: true, configurable: true });
  Object.defineProperty(globalThis, "LockManager", { value: LockManager, writable: true, configurable: true });
  Object.defineProperty(globalThis, "Lock", { value: Lock, writable: true, configurable: true });
  void counter;
})();
