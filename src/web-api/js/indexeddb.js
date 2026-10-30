// IndexedDB 3.0 (https://w3c.github.io/IndexedDB/): the databases are kept in memory, for as long as the page is.
(() => {
  "use strict";
  const DOMEx = globalThis.DOMException;
  const fail = (name, message) => new DOMEx(message, name);
  const hasOwn = (o, k) => Object.prototype.hasOwnProperty.call(o, k);

  // ---- Tasks: a request is answered in a task of its own, not in the one that made it (and a promise that waits for it goes on in between) ----
  const queued = [];
  let channel = null;
  if (typeof MessageChannel === "function") {
    channel = new MessageChannel();
    channel.port1.onmessage = () => { const f = queued.shift(); if (f) f(); };
  }
  const task = (f) => {
    if (channel) { queued.push(f); channel.port2.postMessage(0); } else setTimeout(f, 0);
  };

  // ---- Keys ----
  const isBinary = (v) => v instanceof ArrayBuffer || ArrayBuffer.isView(v);
  const rank = (k) => typeof k === "number" ? 1 : k instanceof Date ? 2 : typeof k === "string" ? 3 : isBinary(k) ? 4 : 5;
  const bytesOf = (k) => k instanceof ArrayBuffer ? new Uint8Array(k) : new Uint8Array(k.buffer, k.byteOffset, k.byteLength);
  // The key a value is, or nothing (null) where it is not one. Kept as a copy: what the caller changes later does not change a key.
  function keyOf(value, seen) {
    if (typeof value === "number") return Number.isNaN(value) ? null : value;
    if (value instanceof Date) return Number.isNaN(value.getTime()) ? null : new Date(value.getTime());
    if (typeof value === "string") return value;
    if (isBinary(value)) return new Uint8Array(bytesOf(value)).slice().buffer;
    if (Array.isArray(value)) {
      seen = seen || [];
      if (seen.includes(value)) return null;
      seen.push(value);
      const out = [];
      for (let i = 0; i < value.length; i++) {
        if (!(i in value)) return null;
        const k = keyOf(value[i], seen);
        if (k === null) return null;
        out.push(k);
      }
      seen.pop();
      return out;
    }
    return null;
  }
  const toKey = (value) => {
    const k = keyOf(value);
    if (k === null) throw fail("DataError", "The parameter is not a valid key.");
    return k;
  };
  function compare(a, b) {
    const ra = rank(a), rb = rank(b);
    if (ra !== rb) return ra < rb ? -1 : 1;
    switch (ra) {
      case 1: return a < b ? -1 : a > b ? 1 : 0;
      case 2: { const x = a.getTime(), y = b.getTime(); return x < y ? -1 : x > y ? 1 : 0; }
      case 3: return a < b ? -1 : a > b ? 1 : 0;
      case 4: {
        const x = bytesOf(a), y = bytesOf(b);
        const n = Math.min(x.length, y.length);
        for (let i = 0; i < n; i++) if (x[i] !== y[i]) return x[i] < y[i] ? -1 : 1;
        return x.length < y.length ? -1 : x.length > y.length ? 1 : 0;
      }
      default: {
        const n = Math.min(a.length, b.length);
        for (let i = 0; i < n; i++) { const c = compare(a[i], b[i]); if (c) return c; }
        return a.length < b.length ? -1 : a.length > b.length ? 1 : 0;
      }
    }
  }

  // ---- Key paths ----
  const identifier = /^[\p{ID_Start}$_][\p{ID_Continue}$‌‍]*$/u;
  function validPath(p) {
    if (typeof p === "string") return p === "" || p.split(".").every((part) => identifier.test(part));
    if (Array.isArray(p)) return p.length > 0 && p.every((x) => typeof x === "string" && validPath(x));
    return false;
  }
  const clonePath = (p) => Array.isArray(p) ? p.slice() : p;
  function evaluate(value, path) {
    if (path === "") return { found: true, value };
    for (const part of path.split(".")) {
      if (typeof value === "string" && part === "length") { value = value.length; continue; }
      if (Array.isArray(value) && part === "length") { value = value.length; continue; }
      if (value !== null && typeof value === "object" && (hasOwn(value, part) || (typeof Blob === "function" && value instanceof Blob && (part === "size" || part === "type")) || (typeof File === "function" && value instanceof File && (part === "name" || part === "lastModified")))) { value = value[part]; continue; }
      return { found: false };
    }
    return { found: true, value };
  }
  // The key a value has by the path, or null.
  function extractKey(value, path) {
    if (Array.isArray(path)) {
      const out = [];
      for (const p of path) {
        const r = evaluate(value, p);
        if (!r.found) return null;
        const k = keyOf(r.value);
        if (k === null) return null;
        out.push(k);
      }
      return out;
    }
    const r = evaluate(value, path);
    return r.found ? keyOf(r.value) : null;
  }
  function canInject(value, path) {
    const parts = path.split(".");
    parts.pop();
    for (const part of parts) {
      if (value === null || typeof value !== "object") return false;
      if (hasOwn(value, part)) value = value[part]; else return true;
    }
    return value !== null && typeof value === "object";
  }
  function inject(value, path, key) {
    const parts = path.split(".");
    const last = parts.pop();
    for (const part of parts) {
      if (!hasOwn(value, part)) value[part] = {};
      value = value[part];
    }
    value[last] = key;
  }

  // ---- Key ranges ----
  class IDBKeyRange {
    #lower; #upper; #lowerOpen; #upperOpen;
    constructor(token, lower, upper, lowerOpen, upperOpen) {
      if (token !== internal) throw new TypeError("Illegal constructor");
      this.#lower = lower; this.#upper = upper; this.#lowerOpen = lowerOpen; this.#upperOpen = upperOpen;
    }
    get lower() { return this.#lower; }
    get upper() { return this.#upper; }
    get lowerOpen() { return this.#lowerOpen; }
    get upperOpen() { return this.#upperOpen; }
    includes(key) {
      key = toKey(key);
      if (this.#lower !== undefined) { const c = compare(this.#lower, key); if (c > 0 || (c === 0 && this.#lowerOpen)) return false; }
      if (this.#upper !== undefined) { const c = compare(key, this.#upper); if (c > 0 || (c === 0 && this.#upperOpen)) return false; }
      return true;
    }
    static only(value) { const k = toKey(value); return new IDBKeyRange(internal, k, k, false, false); }
    static lowerBound(lower, open = false) { return new IDBKeyRange(internal, toKey(lower), undefined, !!open, true); }
    static upperBound(upper, open = false) { return new IDBKeyRange(internal, undefined, toKey(upper), true, !!open); }
    static bound(lower, upper, lowerOpen = false, upperOpen = false) {
      const l = toKey(lower), u = toKey(upper);
      const c = compare(l, u);
      if (c > 0 || (c === 0 && (lowerOpen || upperOpen))) throw fail("DataError", "The lower key is greater than the upper key.");
      return new IDBKeyRange(internal, l, u, !!lowerOpen, !!upperOpen);
    }
  }
  const internal = Symbol("internal");
  // A query: a key or a key range, as a range (null for none).
  function rangeOf(query, required) {
    if (query === undefined || query === null) {
      if (required) throw fail("DataError", "A key or a key range is required.");
      return null;
    }
    if (query instanceof IDBKeyRange) return query;
    const k = toKey(query);
    return new IDBKeyRange(internal, k, k, false, false);
  }
  const inRange = (range, key) => !range || range.includes(key);

  // ---- Events ----
  class IDBVersionChangeEvent extends Event {
    #old; #new;
    constructor(type, init = {}) {
      super(type, init);
      this.#old = init.oldVersion === undefined ? 0 : Number(init.oldVersion);
      this.#new = init.newVersion === undefined || init.newVersion === null ? null : Number(init.newVersion);
    }
    get oldVersion() { return this.#old; }
    get newVersion() { return this.#new; }
  }
  // The attributes that take what is to be done at an event (onsuccess and the like), over the listeners.
  function handlerAttributes(proto, names) {
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
  }

  // ---- The data ----
  const databases = new Map();  // name -> definition

  // ---- Persistence: a database is a file of the origin's, written (after a short wait for what comes next) when a transaction that wrote is done ----
  const idbRead = globalThis.__solarIdbRead, idbWrite = globalThis.__solarIdbWrite, idbDelete = globalThis.__solarIdbDelete, idbList = globalThis.__solarIdbList;
  delete globalThis.__solarIdbRead; delete globalThis.__solarIdbWrite; delete globalThis.__solarIdbDelete; delete globalThis.__solarIdbList;
  const originOf = () => { try { return String(location.origin); } catch (e) { return "null"; } };
  // (An origin that is none, a file's, has nothing kept: its databases are the page's.)
  const persistent = () => typeof idbWrite === "function" && originOf() !== "null";
  const toBase64 = (bytes) => {
    let text = "";
    for (let i = 0; i < bytes.length; i += 0x8000) text += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
    return btoa(text);
  };
  const fromBase64 = (text) => {
    const raw = atob(text);
    const bytes = new Uint8Array(raw.length);
    for (let i = 0; i < raw.length; i++) bytes[i] = raw.charCodeAt(i);
    return bytes;
  };
  // A value as what JSON can hold, with what it is told by `$` (a repeat of an object, and a cycle, by the number it was given).
  function encode(value) {
    const ids = new Map();
    const enc = (v) => {
      switch (typeof v) {
        case "undefined": return { $: "u" };
        case "number": return Number.isFinite(v) && !Object.is(v, -0) ? v : { $: "n", v: Object.is(v, -0) ? "-0" : String(v) };
        case "bigint": return { $: "b", v: v.toString() };
        case "string": case "boolean": return v;
        case "symbol": case "function": throw new Error("not storable");
        default:
      }
      if (v === null) return null;
      if (ids.has(v)) return { $: "R", i: ids.get(v) };
      const i = ids.size;
      ids.set(v, i);
      if (v instanceof Date) return { $: "d", i, v: Number.isNaN(v.getTime()) ? "NaN" : v.getTime() };
      if (v instanceof RegExp) return { $: "r", i, s: v.source, f: v.flags };
      if (Array.isArray(v)) {
        const e = [];
        for (let k = 0; k < v.length; k++) e.push(k in v ? enc(v[k]) : { $: "h" });
        return { $: "a", i, e };
      }
      if (v instanceof Map) return { $: "m", i, e: [...v].map(([k, x]) => [enc(k), enc(x)]) };
      if (v instanceof Set) return { $: "s", i, e: [...v].map(enc) };
      if (v instanceof ArrayBuffer) return { $: "t", i, c: "ArrayBuffer", b: toBase64(new Uint8Array(v)) };
      if (ArrayBuffer.isView(v)) return { $: "t", i, c: v.constructor.name, b: toBase64(new Uint8Array(v.buffer, v.byteOffset, v.byteLength)) };
      if (v instanceof Error) return { $: "x", i, n: v.name, m: v.message };
      if (typeof Blob === "function" && v instanceof Blob) throw new Error("not storable");
      if (v instanceof Boolean || v instanceof Number || v instanceof String) return { $: "w", i, v: enc(v.valueOf()) };
      const e = [];
      for (const k of Object.keys(v)) e.push([k, enc(v[k])]);
      return { $: "o", i, e };
    };
    return enc(value);
  }
  function decode(node) {
    const refs = new Map();
    const dec = (n) => {
      if (n === null || typeof n !== "object") return n;
      switch (n.$) {
        case "u": return undefined;
        case "n": return Number(n.v);
        case "b": return BigInt(n.v);
        case "R": return refs.get(n.i);
        case "d": { const d = new Date(n.v === "NaN" ? NaN : n.v); refs.set(n.i, d); return d; }
        case "r": { const r = new RegExp(n.s, n.f); refs.set(n.i, r); return r; }
        case "a": { const a = []; refs.set(n.i, a); for (const x of n.e) { if (x !== null && typeof x === "object" && x.$ === "h") a.length++; else a.push(dec(x)); } return a; }
        case "m": { const m = new Map(); refs.set(n.i, m); for (const [k, x] of n.e) m.set(dec(k), dec(x)); return m; }
        case "s": { const set = new Set(); refs.set(n.i, set); for (const x of n.e) set.add(dec(x)); return set; }
        case "t": {
          const bytes = fromBase64(n.b);
          const view = n.c === "ArrayBuffer" ? bytes.buffer : new globalThis[n.c](bytes.buffer);
          refs.set(n.i, view);
          return view;
        }
        case "x": { const E = globalThis[n.n] && globalThis[n.n].prototype instanceof Error ? globalThis[n.n] : Error; const e = new E(n.m); refs.set(n.i, e); return e; }
        case "w": { const w = Object(dec(n.v)); refs.set(n.i, w); return w; }
        case "o": { const o = {}; refs.set(n.i, o); for (const [k, x] of n.e) o[k] = dec(x); return o; }
        default: return undefined;
      }
    };
    return dec(node);
  }
  function save(def) {
    def.saveTimer = null;
    if (!persistent() || def.deleted) return;
    try {
      const stores = [];
      for (const store of def.stores.values()) {
        stores.push({
          name: store.name, keyPath: store.keyPath, autoIncrement: store.autoIncrement, nextKey: store.nextKey,
          indexes: [...store.indexes.values()].map((i) => ({ name: i.name, keyPath: i.keyPath, unique: i.unique, multiEntry: i.multiEntry })),
          records: store.records.map((r) => [encode(r.key), encode(r.value)]),
        });
      }
      idbWrite(originOf(), def.name, JSON.stringify({ name: def.name, version: def.version }) + "\n" + JSON.stringify({ stores }));
    } catch (e) { /* (what is not storable, a Blob, is kept for the page only) */ }
  }
  function persist(def) {
    if (!persistent() || def.saveTimer) return;
    def.saveTimer = setTimeout(() => save(def), 100);
  }
  function loadDef(name) {
    if (!persistent()) return null;
    let text;
    try { text = idbRead(originOf(), name); } catch (e) { return null; }
    if (typeof text !== "string") return null;
    try {
      const at = text.indexOf("\n");
      const header = JSON.parse(text.slice(0, at));
      const body = JSON.parse(text.slice(at + 1));
      const def = { name: header.name, version: header.version, stores: new Map(), connections: new Set(), saveTimer: null };
      for (const saved of body.stores) {
        const store = newStore(saved.name, saved.keyPath, saved.autoIncrement);
        store.nextKey = saved.nextKey;
        for (const i of saved.indexes) store.indexes.set(i.name, newIndex(i.name, i.keyPath, i.unique, i.multiEntry));
        for (const [k, v] of saved.records) {
          const key = decode(k);
          const value = decode(v);
          store.records.push({ key, value });
          for (const index of store.indexes.values()) for (const ik of indexKeys(index, value)) addEntry(index, ik, key);
        }
        def.stores.set(store.name, store);
      }
      return def;
    } catch (e) { return null; }
  }
  const flushAll = () => { for (const def of databases.values()) if (def.saveTimer) { clearTimeout(def.saveTimer); save(def); } };
  if (typeof addEventListener === "function") {
    addEventListener("pagehide", flushAll);
    addEventListener("visibilitychange", () => { if (document.visibilityState === "hidden") flushAll(); });
  }
  const newStore = (name, keyPath, autoIncrement) => ({ name, keyPath, autoIncrement, records: [], nextKey: 1, indexes: new Map() });
  const newIndex = (name, keyPath, unique, multiEntry) => ({ name, keyPath, unique, multiEntry, entries: [] });
  // Where a key goes in a sorted list (the first place that is not before it), and whether it is there.
  function search(list, key, keyOfItem) {
    let lo = 0, hi = list.length;
    while (lo < hi) {
      const mid = (lo + hi) >> 1;
      if (compare(keyOfItem(list[mid]), key) < 0) lo = mid + 1; else hi = mid;
    }
    return lo;
  }
  const recordKey = (r) => r.key;
  const findRecord = (store, key) => {
    const at = search(store.records, key, recordKey);
    return at < store.records.length && compare(store.records[at].key, key) === 0 ? at : -1;
  };
  // The keys an index has for a value.
  function indexKeys(index, value) {
    const k = extractKey(value, index.keyPath);
    if (k === null) {
      return [];
    }
    if (index.multiEntry && Array.isArray(index.keyPath) === false && Array.isArray(k)) {
      const out = [];
      for (const part of k) if (!out.some((o) => compare(o, part) === 0)) out.push(part);
      return out;
    }
    return [k];
  }
  // Index entries are by key, then by the key of the record.
  const entryBefore = (a, b) => { const c = compare(a.key, b.key); return c ? c : compare(a.pk, b.pk); };
  function addEntry(index, key, pk) {
    let lo = 0, hi = index.entries.length;
    const entry = { key, pk };
    while (lo < hi) {
      const mid = (lo + hi) >> 1;
      if (entryBefore(index.entries[mid], entry) < 0) lo = mid + 1; else hi = mid;
    }
    index.entries.splice(lo, 0, entry);
  }
  function removeEntries(index, pk) {
    index.entries = index.entries.filter((e) => compare(e.pk, pk) !== 0);
  }
  function wouldBreakUnique(store, key, value, except) {
    for (const index of store.indexes.values()) {
      if (!index.unique) continue;
      for (const k of indexKeys(index, value)) {
        const at = search(index.entries, k, (e) => e.key);
        for (let i = at; i < index.entries.length && compare(index.entries[i].key, k) === 0; i++) {
          if (except === undefined || compare(index.entries[i].pk, except) !== 0) return true;
        }
      }
    }
    return false;
  }

  // ---- Requests ----
  class IDBRequest extends EventTarget {
    #result; #error = null; #done = false; #source = null; #transaction = null;
    constructor(token, source, transaction) {
      super();
      if (token !== internal) throw new TypeError("Illegal constructor");
      this.#source = source === undefined ? null : source;
      this.#transaction = transaction === undefined ? null : transaction;
    }
    get result() {
      if (!this.#done) throw fail("InvalidStateError", "The request has not finished.");
      return this.#result;
    }
    get error() {
      if (!this.#done) throw fail("InvalidStateError", "The request has not finished.");
      return this.#error;
    }
    get source() { return this.#source; }
    get transaction() { return this.#transaction; }
    get readyState() { return this.#done ? "done" : "pending"; }
    static _set(request, state) {
      if ("result" in state) request.#result = state.result;
      if ("error" in state) request.#error = state.error;
      if ("done" in state) request.#done = state.done;
      if ("transaction" in state) request.#transaction = state.transaction;
    }
  }
  handlerAttributes(IDBRequest.prototype, ["success", "error"]);
  class IDBOpenDBRequest extends IDBRequest {
    constructor(token) { super(token, null, null); }
  }
  handlerAttributes(IDBOpenDBRequest.prototype, ["blocked", "upgradeneeded"]);
  const makeRequest = (source, transaction) => new IDBRequest(internal, source, transaction);

  // An event for a request: at the request, and (for those that bubble) then at its transaction and its database. Told whether it was cancelled.
  function fireOn(request, type, tx, bubbles, cancelable, init) {
    const make = () => {
      const e = init ? new IDBVersionChangeEvent(type, { bubbles, cancelable, ...init }) : new Event(type, { bubbles, cancelable });
      return e;
    };
    let prevented = false;
    const first = make();
    let stopped = false;
    const stop = first.stopPropagation.bind(first);
    first.stopPropagation = () => { stopped = true; stop(); };
    request.dispatchEvent(first);
    if (first.defaultPrevented) prevented = true;
    if (bubbles && !stopped && tx) {
      for (const target of [tx, tx.db]) {
        const e = make();
        Object.defineProperty(e, "target", { value: request, configurable: true });
        const s = e.stopPropagation.bind(e);
        let again = false;
        e.stopPropagation = () => { again = true; s(); };
        target.dispatchEvent(e);
        if (e.defaultPrevented) prevented = true;
        if (again) break;
      }
    }
    return prevented;
  }

  // ---- Transactions ----
  class IDBTransaction extends EventTarget {
    #db; #mode; #names; #error = null; #state = "active"; #queue = []; #scheduled = false; #finished = false;
    #snapshots = new Map(); #durability = "default"; #aborted = false; #open = null;
    constructor(token, db, names, mode, open) {
      super();
      if (token !== internal) throw new TypeError("Illegal constructor");
      this.#db = db; this.#names = names; this.#mode = mode; this.#open = open || null;
      this.requests = 0;
      this.stores = new Map();
      if (mode === "versionchange") {
        // (What the database was: to be had back if this is not done.)
        this.before = { version: db._def.version, stores: new Map([...db._def.stores].map(([n, s]) => [n, snapshotOf(s)])) };
      }
      // It is active until what made it has returned.
      task(() => { if (this.#state === "active") { this.#state = "inactive"; this.#pump(); } });
    }
    get db() { return this.#db; }
    get mode() { return this.#mode; }
    get durability() { return this.#durability; }
    get error() { return this.#error; }
    get objectStoreNames() { return list(this.#names.slice().sort()); }
    get _state() { return this.#state; }
    get _names() { return this.#names; }
    get _open() { return this.#open; }
    objectStore(name) {
      if (this.#finished || this.#aborted) throw fail("InvalidStateError", "The transaction has finished.");
      name = String(name);
      if (!this.#names.includes(name)) throw fail("NotFoundError", "The object store is not in the transaction.");
      let store = this.stores.get(name);
      if (!store) { store = new IDBObjectStore(internal, this, this.#db._def.stores.get(name)); this.stores.set(name, store); }
      return store;
    }
    abort() {
      if (this.#state === "committing" || this.#state === "finished") throw fail("InvalidStateError", "The transaction is being committed or has finished.");
      this.#abort(null);
    }
    commit() {
      if (this.#state !== "active") throw fail("TransactionInactiveError", "The transaction is not active.");
      this.#state = "inactive";
      this.#commitWhenDone = true;
      this.#pump();
    }
    #commitWhenDone = false;
    // The requests are done in the order they were made, one to a task.
    _enqueue(request, work, keepOpen) {
      if (this.#state !== "active") throw fail("TransactionInactiveError", "The transaction is not active.");
      if (this.#aborted || this.#finished) throw fail("TransactionInactiveError", "The transaction has finished.");
      this.#queue.push({ request, work });
      this.requests++;
      this.#pump();
    }
    // A store is to be written: what it was is kept, for an abort.
    _writing(def) {
      if (this.mode === "versionchange" || this.#snapshots.has(def)) return;
      this.#snapshots.set(def, snapshotOf(def));
    }
    #pump() {
      if (this.#scheduled || this.#finished) return;
      this.#scheduled = true;
      task(() => { this.#scheduled = false; this.#step(); });
    }
    #step() {
      if (this.#finished || this.#aborted) return;
      const next = this.#queue.shift();
      if (next) {
        const { request, work } = next;
        this.#state = "active";
        let result, error = null;
        try { result = work(); } catch (e) { error = e instanceof DOMEx ? e : fail("UnknownError", String(e && e.message || e)); }
        if (error) {
          IDBRequest._set(request, { done: true, error, result: undefined });
          const prevented = fireOn(request, "error", this, true, true);
          if (!prevented && !this.#aborted) this.#abort(error);
        } else {
          IDBRequest._set(request, { done: true, error: null, result });
          fireOn(request, "success", this, false, false);
        }
        if (this.#aborted) return;
        // (What the handlers started that goes on in this task's microtasks has its say before the transaction is let go of.)
        this.#pump();
        return;
      }
      // Nothing is asked for: the transaction is done, unless what is waiting for it asks for more.
      if (this.#state === "active" && !this.#commitWhenDone) {
        this.#state = "inactive";
        this.#pump();
        return;
      }
      this.#state = "committing";
      task(() => this.#complete());
    }
    #complete() {
      if (this.#aborted || this.#finished) return;
      if (this.#queue.length) { this.#state = "inactive"; this.#pump(); return; }
      this.#finished = true;
      this.#state = "finished";
      this.#snapshots.clear();
      if (this.#mode !== "readonly") persist(this.#db._def);
      this.dispatchEvent(new Event("complete"));
      this.#db._transactionDone(this);
      if (this.#open) this.#open(true);
    }
    #abort(error) {
      if (this.#aborted || this.#finished) return;
      this.#aborted = true;
      this.#state = "finished";
      this.#error = error;
      // What was written is put back.
      for (const [def, saved] of this.#snapshots) restore(def, saved);
      if (this.mode === "versionchange" && this.before) {
        const def = this.#db._def;
        def.version = this.before.version;
        def.stores = new Map();
        for (const [n, saved] of this.before.stores) { const s = newStore(n, saved.keyPath, saved.autoIncrement); restore(s, saved); def.stores.set(n, s); }
      }
      for (const { request } of this.#queue) {
        IDBRequest._set(request, { done: true, error: fail("AbortError", "The transaction was aborted."), result: undefined });
        fireOn(request, "error", this, true, false);
      }
      this.#queue = [];
      task(() => {
        this.#finished = true;
        this.dispatchEvent(new Event("abort", { bubbles: true }));
        const bubble = new Event("abort", { bubbles: true });
        this.#db.dispatchEvent(bubble);
        this.#db._transactionDone(this);
        if (this.#open) this.#open(false);
      });
    }
  }
  handlerAttributes(IDBTransaction.prototype, ["complete", "abort", "error"]);
  function snapshotOf(def) {
    return {
      keyPath: def.keyPath, autoIncrement: def.autoIncrement, nextKey: def.nextKey,
      records: def.records.slice(),
      indexes: new Map([...def.indexes].map(([n, i]) => [n, { name: i.name, keyPath: i.keyPath, unique: i.unique, multiEntry: i.multiEntry, entries: i.entries.slice() }])),
    };
  }
  function restore(def, saved) {
    def.nextKey = saved.nextKey;
    def.records = saved.records.slice();
    def.indexes = new Map([...saved.indexes].map(([n, i]) => [n, { ...i, entries: i.entries.slice() }]));
  }
  // A list of names, as the platform's DOMStringList.
  function list(names) {
    const out = Object.create(DOMStringListPrototype);
    names.forEach((n, i) => { out[i] = n; });
    Object.defineProperty(out, "length", { value: names.length });
    return out;
  }
  const DOMStringListPrototype = Object.create(Object.prototype, {
    item: { value(i) { return this[i] === undefined ? null : this[i]; } },
    contains: { value(n) { for (let i = 0; i < this.length; i++) if (this[i] === n) return true; return false; } },
    [Symbol.iterator]: { value: function* () { for (let i = 0; i < this.length; i++) yield this[i]; } },
    [Symbol.toStringTag]: { value: "DOMStringList" },
  });

  // ---- Cursors ----
  class IDBCursor {
    #store; #index; #range; #direction; #request; #keyOnly; #key = undefined; #primaryKey = undefined; #value = undefined; #gotValue = false; #position = null;
    constructor(token, source, range, direction, request, keyOnly) {
      if (token !== internal) throw new TypeError("Illegal constructor");
      this.#source = source;
      this.#store = source instanceof IDBIndex ? source._store : source;
      this.#index = source instanceof IDBIndex ? source : null;
      this.#range = range; this.#direction = direction; this.#request = request; this.#keyOnly = keyOnly;
    }
    #source;
    get source() { return this.#source; }
    get direction() { return this.#direction; }
    get key() { return this.#key; }
    get primaryKey() { return this.#primaryKey; }
    get request() { return this.#request; }
    get _val() { return this.#value; }
    #def() { return this.#store._def; }
    #check() {
      const tx = this.#store.transaction;
      if (tx._state !== "active") throw fail("TransactionInactiveError", "The transaction is not active.");
      if (!this.#gotValue) throw fail("InvalidStateError", "The cursor is being iterated or has iterated past its end.");
    }
    // Moves to what is after `count` steps from the position (or to `target`), and answers the request with the cursor, or null at the end.
    _advance(count, target, targetPrimary) {
      const request = this.#request;
      const tx = this.#store.transaction;
      this.#gotValue = false;
      IDBRequest._set(request, { done: false, result: undefined, error: null });
      tx._enqueue(request, () => {
        let found = null;
        for (let i = 0; i < count; i++) {
          found = this.#next(target, targetPrimary);
          if (!found) break;
          target = undefined; targetPrimary = undefined;
          this.#position = { key: found.key, primary: found.primaryKey };
        }
        if (!found) { this.#gotValue = false; this.#key = this.#primaryKey = this.#value = undefined; return null; }
        this.#key = found.key; this.#primaryKey = found.primaryKey;
        if (!this.#keyOnly) this.#value = found.value;
        this.#gotValue = true;
        return this;
      });
    }
    // The record after the position in the direction, in the range, and not before the target.
    #next(target, targetPrimary) {
      const forward = this.#direction === "next" || this.#direction === "nextunique";
      const unique = this.#direction === "nextunique" || this.#direction === "prevunique";
      const range = this.#range;
      const position = this.#position;
      const items = this.#index ? this.#index._def.entries : this.#def().records;
      const keyOfItem = this.#index ? (e) => e.key : recordKey;
      const primaryOf = this.#index ? (e) => e.pk : recordKey;
      const before = (a, b) => {  // a comes before b in the direction
        let c = compare(a.key, b.key);
        if (c === 0 && this.#index) c = compare(a.primary, b.primary);
        return forward ? c < 0 : c > 0;
      };
      let candidate = null;
      const consider = (item) => {
        const key = keyOfItem(item), primary = primaryOf(item);
        if (range && !range.includes(key)) return;
        const info = { key, primary, item };
        if (position) {
          const moved = forward ? compareFull(info, position) > 0 : compareFull(info, position) < 0;
          if (!moved) return;
          if (unique && compare(key, position.key) === 0) return;
        }
        if (target !== undefined) {
          const c = compare(key, target);
          if (forward ? c < 0 : c > 0) return;
          if (targetPrimary !== undefined && c === 0) {
            const p = compare(primary, targetPrimary);
            if (forward ? p < 0 : p > 0) return;
          }
        }
        if (!candidate || before(info, candidate)) candidate = info;
      };
      const index = this.#index;
      const compareFull = (a, b) => { let c = compare(a.key, b.key); if (c === 0 && index) c = compare(a.primary, b.primary); return c; };
      // (The lists are sorted: the first one past the position is found by search, and the first one of a unique run is the first.)
      if (forward) {
        let start = 0;
        const from = position ? position.key : (range && range.lower !== undefined ? range.lower : undefined);
        if (from !== undefined) start = search(items, from, keyOfItem);
        for (let i = start; i < items.length; i++) {
          consider(items[i]);
          if (candidate) break;
          if (range && range.upper !== undefined && compare(keyOfItem(items[i]), range.upper) > 0) break;
        }
      } else {
        const bound = position ? position.key : (range && range.upper !== undefined ? range.upper : undefined);
        let start = items.length - 1;
        if (bound !== undefined) {
          start = search(items, bound, keyOfItem);
          while (start < items.length && compare(keyOfItem(items[start]), bound) <= 0) start++;
          start--;
        }
        for (let i = start; i >= 0; i--) {
          consider(items[i]);
          if (candidate) break;
          if (range && range.lower !== undefined && compare(keyOfItem(items[i]), range.lower) < 0) break;
        }
        // (A prev run is by the key, and the first of it in the direction is the last of the entries with it.)
      }
      if (!candidate) return null;
      if (unique && !forward) {
        // the first record of a run of one key, in the order of the keys
        const key = candidate.key;
        const at = search(items, key, keyOfItem);
        const item = items[at];
        candidate = { key, primary: primaryOf(item), item };
      }
      let value;
      if (!this.#keyOnly) {
        const record = this.#index ? this.#def().records[findRecord(this.#def(), candidate.primary)] : candidate.item;
        value = record ? structuredClone(record.value) : undefined;
      }
      return { key: candidate.key, primaryKey: candidate.primary, value };
    }
    advance(count) {
      count = Number(count);
      if (!Number.isInteger(count) || count < 1) throw new TypeError("The count must be a positive number.");
      this.#check();
      this._advance(count, undefined, undefined);
    }
    continue(key) {
      this.#check();
      if (key !== undefined) {
        key = toKey(key);
        const forward = this.#direction === "next" || this.#direction === "nextunique";
        const c = compare(key, this.#key);
        if (forward ? c <= 0 : c >= 0) throw fail("DataError", "The key is not past the cursor's position.");
      }
      this._advance(1, key, undefined);
    }
    continuePrimaryKey(key, primaryKey) {
      if (!this.#index) throw fail("InvalidAccessError", "The cursor is not of an index.");
      if (this.#direction !== "next" && this.#direction !== "prev") throw fail("InvalidAccessError", "The cursor is a unique one.");
      this.#check();
      key = toKey(key); primaryKey = toKey(primaryKey);
      const forward = this.#direction === "next";
      const c = compare(key, this.#key) || compare(primaryKey, this.#primaryKey);
      if (forward ? c <= 0 : c >= 0) throw fail("DataError", "The position is not past the cursor's.");
      this._advance(1, key, primaryKey);
    }
    update(value) {
      const tx = this.#store.transaction;
      if (tx._state !== "active") throw fail("TransactionInactiveError", "The transaction is not active.");
      if (tx.mode === "readonly") throw fail("ReadOnlyError", "The transaction is read-only.");
      if (this.#keyOnly || !this.#gotValue) throw fail("InvalidStateError", "The cursor has no value.");
      const def = this.#store._def;
      if (def.keyPath !== null) {
        const k = extractKey(value, def.keyPath);
        if (k === null || compare(k, this.#primaryKey) !== 0) throw fail("DataError", "The key of the value is not the cursor's.");
      }
      const copy = clone(value);
      const key = this.#primaryKey;
      const request = makeRequest(this, tx);
      tx._enqueue(request, () => putRecord(tx, def, copy, key, true, true));
      return request;
    }
    delete() {
      const tx = this.#store.transaction;
      if (tx._state !== "active") throw fail("TransactionInactiveError", "The transaction is not active.");
      if (tx.mode === "readonly") throw fail("ReadOnlyError", "The transaction is read-only.");
      if (this.#keyOnly || !this.#gotValue) throw fail("InvalidStateError", "The cursor has no value.");
      const def = this.#store._def;
      const key = this.#primaryKey;
      const request = makeRequest(this, tx);
      tx._enqueue(request, () => { deleteRecords(tx, def, IDBKeyRange.only(key)); return undefined; });
      return request;
    }
  }
  class IDBCursorWithValue extends IDBCursor {
    get value() { return this._val; }
  }

  // ---- Records ----
  const clone = (value) => {
    try { return structuredClone(value); } catch (e) { throw e instanceof DOMEx ? e : fail("DataCloneError", String(e && e.message || e)); }
  };
  // Puts a record in a store; `noOverwrite` is add. Returns the key.
  function putRecord(tx, def, value, key, overwrite, isUpdate) {
    tx._writing(def);
    let useKey;
    if (def.keyPath !== null) {
      const k = extractKey(value, def.keyPath);
      if (k !== null) useKey = k;
      else if (def.autoIncrement && !Array.isArray(def.keyPath) && (typeof value === "object" && value !== null) && canInject(value, def.keyPath)) useKey = undefined;
      else if (def.autoIncrement && !Array.isArray(def.keyPath)) throw fail("DataError", "The value cannot have a key put in it.");
      else throw fail("DataError", "The value has no key at the key path.");
    } else if (key !== undefined) {
      useKey = key;
    } else if (!def.autoIncrement) {
      throw fail("DataError", "A key is required.");
    }
    let generated = false;
    if (useKey === undefined) {
      if (def.nextKey > 9007199254740992) throw fail("ConstraintError", "The key generator has run out.");
      useKey = def.nextKey;
      def.nextKey++;
      generated = true;
      if (def.keyPath !== null) inject(value, def.keyPath, useKey);
    } else if (typeof useKey === "number" && def.autoIncrement && useKey >= def.nextKey) {
      def.nextKey = Math.floor(useKey) + 1;
    }
    const at = findRecord(def, useKey);
    if (at >= 0 && !overwrite) throw fail("ConstraintError", "Key already exists in the object store.");
    if (wouldBreakUnique(def, useKey, value, at >= 0 ? useKey : undefined)) throw fail("ConstraintError", "A record in an index with that key exists.");
    if (at >= 0) {
      for (const index of def.indexes.values()) removeEntries(index, useKey);
      def.records[at] = { key: useKey, value };
    } else {
      def.records.splice(search(def.records, useKey, recordKey), 0, { key: useKey, value });
    }
    for (const index of def.indexes.values()) for (const k of indexKeys(index, value)) addEntry(index, k, useKey);
    return useKey;
  }
  function deleteRecords(tx, def, range) {
    tx._writing(def);
    const kept = [];
    const gone = [];
    for (const r of def.records) (inRange(range, r.key) ? gone : kept).push(r);
    def.records = kept;
    for (const r of gone) for (const index of def.indexes.values()) removeEntries(index, r.key);
  }

  // ---- Object stores ----
  class IDBObjectStore {
    #tx; #def;
    constructor(token, tx, def) {
      if (token !== internal) throw new TypeError("Illegal constructor");
      this.#tx = tx; this.#def = def; this.indexes = new Map();
    }
    get _def() { return this.#def; }
    get name() { return this.#def.name; }
    set name(value) {
      if (this.#tx.mode !== "versionchange") throw fail("InvalidStateError", "The store can be renamed in an upgrade only.");
      this.#alive();
      value = String(value);
      if (value === this.#def.name) return;
      const stores = this.#tx.db._def.stores;
      if (stores.has(value)) throw fail("ConstraintError", "There is a store with that name.");
      const old = this.#def.name;
      stores.delete(old);
      this.#def.name = value;
      stores.set(value, this.#def);
      const names = this.#tx._names;
      const at = names.indexOf(old);
      if (at >= 0) names[at] = value;
    }
    get keyPath() { return clonePath(this.#def.keyPath); }
    get indexNames() { return list([...this.#def.indexes.keys()].sort()); }
    get transaction() { return this.#tx; }
    get autoIncrement() { return this.#def.autoIncrement; }
    #alive() { if (!this.#tx.db._def.stores.has(this.#def.name) || this.#tx.db._def.stores.get(this.#def.name) !== this.#def) throw fail("InvalidStateError", "The store has been deleted."); }
    #writable() {
      this.#alive();
      if (this.#tx._state !== "active") throw fail("TransactionInactiveError", "The transaction is not active.");
      if (this.#tx.mode === "readonly") throw fail("ReadOnlyError", "The transaction is read-only.");
    }
    #readable() {
      this.#alive();
      if (this.#tx._state !== "active") throw fail("TransactionInactiveError", "The transaction is not active.");
    }
    #put(value, key, overwrite) {
      this.#writable();
      const def = this.#def;
      if (def.keyPath !== null && key !== undefined) throw fail("DataError", "The store has a key path: no key is given.");
      if (def.keyPath === null && !def.autoIncrement && key === undefined) throw fail("DataError", "A key is required.");
      if (key !== undefined) key = toKey(key);
      const copy = clone(value);
      const tx = this.#tx;
      const request = makeRequest(this, tx);
      tx._enqueue(request, () => putRecord(tx, def, copy, key, overwrite));
      return request;
    }
    add(value, key) { return this.#put(value, key, false); }
    put(value, key) { return this.#put(value, key, true); }
    delete(query) {
      this.#writable();
      const range = rangeOf(query, true);
      const tx = this.#tx, def = this.#def;
      const request = makeRequest(this, tx);
      tx._enqueue(request, () => { deleteRecords(tx, def, range); return undefined; });
      return request;
    }
    clear() {
      this.#writable();
      const tx = this.#tx, def = this.#def;
      const request = makeRequest(this, tx);
      tx._enqueue(request, () => { tx._writing(def); def.records = []; for (const i of def.indexes.values()) i.entries = []; return undefined; });
      return request;
    }
    get(query) {
      this.#readable();
      const range = rangeOf(query, true);
      const def = this.#def;
      const request = makeRequest(this, this.#tx);
      this.#tx._enqueue(request, () => {
        for (const r of def.records) if (inRange(range, r.key)) return structuredClone(r.value);
        return undefined;
      });
      return request;
    }
    getKey(query) {
      this.#readable();
      const range = rangeOf(query, true);
      const def = this.#def;
      const request = makeRequest(this, this.#tx);
      this.#tx._enqueue(request, () => { for (const r of def.records) if (inRange(range, r.key)) return r.key; return undefined; });
      return request;
    }
    #all(query, count, what) {
      this.#readable();
      const range = rangeOf(query, false);
      if (count !== undefined) count = count >>> 0;
      const def = this.#def;
      const request = makeRequest(this, this.#tx);
      this.#tx._enqueue(request, () => {
        const out = [];
        for (const r of def.records) {
          if (!inRange(range, r.key)) continue;
          out.push(what === "values" ? structuredClone(r.value) : what === "keys" ? r.key : { key: r.key, primaryKey: r.key, value: structuredClone(r.value) });
          if (count && out.length >= count) break;
        }
        return out;
      });
      return request;
    }
    getAll(query, count) {
      if (query !== null && typeof query === "object" && !(query instanceof IDBKeyRange) && !isBinary(query) && !Array.isArray(query) && !(query instanceof Date) && ("query" in query || "count" in query || "direction" in query)) { count = query.count; query = query.query; }
      return this.#all(query, count, "values");
    }
    getAllKeys(query, count) {
      if (query !== null && typeof query === "object" && !(query instanceof IDBKeyRange) && !isBinary(query) && !Array.isArray(query) && !(query instanceof Date) && ("query" in query || "count" in query || "direction" in query)) { count = query.count; query = query.query; }
      return this.#all(query, count, "keys");
    }
    getAllRecords(options = {}) { return this.#all(options.query, options.count, "records"); }
    count(query) {
      this.#readable();
      const range = rangeOf(query, false);
      const def = this.#def;
      const request = makeRequest(this, this.#tx);
      this.#tx._enqueue(request, () => def.records.filter((r) => inRange(range, r.key)).length);
      return request;
    }
    #cursor(query, direction, keyOnly) {
      this.#readable();
      direction = direction === undefined ? "next" : String(direction);
      if (!["next", "nextunique", "prev", "prevunique"].includes(direction)) throw new TypeError("The direction is not one that there is.");
      const range = rangeOf(query, false);
      const request = makeRequest(this, this.#tx);
      const cursor = keyOnly ? new IDBCursor(internal, this, range, direction, request, true) : new IDBCursorWithValue(internal, this, range, direction, request, false);
      cursor._advance(1, undefined, undefined);
      return request;
    }
    openCursor(query, direction) { return this.#cursor(query, direction, false); }
    openKeyCursor(query, direction) { return this.#cursor(query, direction, true); }
    createIndex(name, keyPath, options = {}) {
      if (this.#tx.mode !== "versionchange") throw fail("InvalidStateError", "An index can be made in an upgrade only.");
      this.#alive();
      if (this.#tx._state !== "active") throw fail("TransactionInactiveError", "The transaction is not active.");
      name = String(name);
      if (this.#def.indexes.has(name)) throw fail("ConstraintError", "There is an index with that name.");
      if (!validPath(keyPath)) throw fail("SyntaxError", "The key path is not valid.");
      const multiEntry = !!(options && options.multiEntry), unique = !!(options && options.unique);
      if (Array.isArray(keyPath) && multiEntry) throw fail("InvalidAccessError", "An array key path cannot be a multiEntry one.");
      const index = newIndex(name, clonePath(keyPath), unique, multiEntry);
      // The records there are are indexed.
      for (const r of this.#def.records) {
        for (const k of indexKeys(index, r.value)) {
          if (unique) {
            const at = search(index.entries, k, (e) => e.key);
            if (at < index.entries.length && compare(index.entries[at].key, k) === 0) throw fail("ConstraintError", "The records have a key twice.");
          }
          addEntry(index, k, r.key);
        }
      }
      this.#def.indexes.set(name, index);
      return this.index(name);
    }
    deleteIndex(name) {
      if (this.#tx.mode !== "versionchange") throw fail("InvalidStateError", "An index can be deleted in an upgrade only.");
      this.#alive();
      if (this.#tx._state !== "active") throw fail("TransactionInactiveError", "The transaction is not active.");
      name = String(name);
      if (!this.#def.indexes.has(name)) throw fail("NotFoundError", "There is no index with that name.");
      this.#def.indexes.delete(name);
      this.indexes.delete(name);
    }
    index(name) {
      if (this.#tx._state === "finished") throw fail("InvalidStateError", "The transaction has finished.");
      this.#alive();
      name = String(name);
      const def = this.#def.indexes.get(name);
      if (!def) throw fail("NotFoundError", "There is no index with that name.");
      let index = this.indexes.get(name);
      if (!index || index._def !== def) { index = new IDBIndex(internal, this, def); this.indexes.set(name, index); }
      return index;
    }
  }

  // ---- Indexes ----
  class IDBIndex {
    #store; #def;
    constructor(token, store, def) {
      if (token !== internal) throw new TypeError("Illegal constructor");
      this.#store = store; this.#def = def;
    }
    get _def() { return this.#def; }
    get _store() { return this.#store; }
    get name() { return this.#def.name; }
    get objectStore() { return this.#store; }
    get keyPath() { return clonePath(this.#def.keyPath); }
    get multiEntry() { return this.#def.multiEntry; }
    get unique() { return this.#def.unique; }
    #readable() {
      if (this.#store.transaction._state !== "active") throw fail("TransactionInactiveError", "The transaction is not active.");
    }
    #find(range) {
      const out = [];
      for (const e of this.#def.entries) if (inRange(range, e.key)) out.push(e);
      return out;
    }
    get(query) {
      this.#readable();
      const range = rangeOf(query, true);
      const request = makeRequest(this, this.#store.transaction);
      const records = this.#store._def.records;
      this.#store.transaction._enqueue(request, () => {
        const e = this.#find(range)[0];
        if (!e) return undefined;
        const at = findRecord(this.#store._def, e.pk);
        return at < 0 ? undefined : structuredClone(records[at].value);
      });
      return request;
    }
    getKey(query) {
      this.#readable();
      const range = rangeOf(query, true);
      const request = makeRequest(this, this.#store.transaction);
      this.#store.transaction._enqueue(request, () => { const e = this.#find(range)[0]; return e ? e.pk : undefined; });
      return request;
    }
    #all(query, count, what) {
      this.#readable();
      const range = rangeOf(query, false);
      if (count !== undefined) count = count >>> 0;
      const request = makeRequest(this, this.#store.transaction);
      this.#store.transaction._enqueue(request, () => {
        const out = [];
        for (const e of this.#find(range)) {
          const at = findRecord(this.#store._def, e.pk);
          out.push(what === "values" ? structuredClone(this.#store._def.records[at].value) : what === "keys" ? e.pk : { key: e.key, primaryKey: e.pk, value: structuredClone(this.#store._def.records[at].value) });
          if (count && out.length >= count) break;
        }
        return out;
      });
      return request;
    }
    getAll(query, count) {
      if (query !== null && typeof query === "object" && !(query instanceof IDBKeyRange) && !isBinary(query) && !Array.isArray(query) && !(query instanceof Date) && ("query" in query || "count" in query)) { count = query.count; query = query.query; }
      return this.#all(query, count, "values");
    }
    getAllKeys(query, count) {
      if (query !== null && typeof query === "object" && !(query instanceof IDBKeyRange) && !isBinary(query) && !Array.isArray(query) && !(query instanceof Date) && ("query" in query || "count" in query)) { count = query.count; query = query.query; }
      return this.#all(query, count, "keys");
    }
    getAllRecords(options = {}) { return this.#all(options.query, options.count, "records"); }
    count(query) {
      this.#readable();
      const range = rangeOf(query, false);
      const request = makeRequest(this, this.#store.transaction);
      this.#store.transaction._enqueue(request, () => this.#find(range).length);
      return request;
    }
    #cursor(query, direction, keyOnly) {
      this.#readable();
      direction = direction === undefined ? "next" : String(direction);
      if (!["next", "nextunique", "prev", "prevunique"].includes(direction)) throw new TypeError("The direction is not one that there is.");
      const range = rangeOf(query, false);
      const request = makeRequest(this, this.#store.transaction);
      const cursor = keyOnly ? new IDBCursor(internal, this, range, direction, request, true) : new IDBCursorWithValue(internal, this, range, direction, request, false);
      cursor._advance(1, undefined, undefined);
      return request;
    }
    openCursor(query, direction) { return this.#cursor(query, direction, false); }
    openKeyCursor(query, direction) { return this.#cursor(query, direction, true); }
  }

  // ---- Databases ----
  class IDBDatabase extends EventTarget {
    #def; #closed = false; #closing = false; #running = new Set();
    constructor(token, def) {
      super();
      if (token !== internal) throw new TypeError("Illegal constructor");
      this.#def = def;
      def.connections.add(this);
    }
    get _def() { return this.#def; }
    get _closed() { return this.#closed; }
    get name() { return this.#def.name; }
    get version() { return this.#def.version; }
    get objectStoreNames() { return list([...this.#def.stores.keys()].sort()); }
    transaction(storeNames, mode = "readonly", options) {
      if (this.#closing || this.#closed) throw fail("InvalidStateError", "The database connection is closing.");
      mode = String(mode);
      if (mode !== "readonly" && mode !== "readwrite") throw new TypeError("The mode is not one that there is.");
      const names = Array.isArray(storeNames) || (storeNames && typeof storeNames === "object" && typeof storeNames.length === "number" && typeof storeNames !== "string")
        ? Array.from(storeNames, String) : [String(storeNames)];
      if (names.length === 0) throw fail("InvalidAccessError", "No store is named.");
      for (const n of names) if (!this.#def.stores.has(n)) throw fail("NotFoundError", "The object store is not in the database.");
      if (this._upgrading) throw fail("InvalidStateError", "The database is being upgraded.");
      const tx = new IDBTransaction(internal, this, [...new Set(names)], mode);
      this.#running.add(tx);
      return tx;
    }
    createObjectStore(name, options = {}) {
      const tx = this._upgrade;
      if (!tx) throw fail("InvalidStateError", "A store can be made in an upgrade only.");
      if (tx._state !== "active") throw fail("TransactionInactiveError", "The transaction is not active.");
      name = String(name);
      const keyPath = options && options.keyPath !== undefined && options.keyPath !== null ? options.keyPath : null;
      if (keyPath !== null && !validPath(keyPath)) throw fail("SyntaxError", "The key path is not valid.");
      const autoIncrement = !!(options && options.autoIncrement);
      if (this.#def.stores.has(name)) throw fail("ConstraintError", "There is a store with that name.");
      if (autoIncrement && (keyPath === "" || Array.isArray(keyPath))) throw fail("InvalidAccessError", "A generator cannot go with that key path.");
      const store = newStore(name, clonePath(keyPath), autoIncrement);
      this.#def.stores.set(name, store);
      tx._names.push(name);
      return tx.objectStore(name);
    }
    deleteObjectStore(name) {
      const tx = this._upgrade;
      if (!tx) throw fail("InvalidStateError", "A store can be deleted in an upgrade only.");
      if (tx._state !== "active") throw fail("TransactionInactiveError", "The transaction is not active.");
      name = String(name);
      if (!this.#def.stores.has(name)) throw fail("NotFoundError", "There is no store with that name.");
      this.#def.stores.delete(name);
      const at = tx._names.indexOf(name);
      if (at >= 0) tx._names.splice(at, 1);
      tx.stores.delete(name);
    }
    close() {
      this.#closing = true;
      this.#maybeClose();
    }
    _transactionDone(tx) {
      this.#running.delete(tx);
      this.#maybeClose();
    }
    _forceClose() {
      this.#closing = true;
      this.#closed = true;
      this.#def.connections.delete(this);
      resume(this.#def);
    }
    #maybeClose() {
      if (!this.#closing || this.#closed || this.#running.size) return;
      this.#closed = true;
      this.#def.connections.delete(this);
      resume(this.#def);
    }
    get _running() { return this.#running; }
  }
  handlerAttributes(IDBDatabase.prototype, ["abort", "close", "error", "versionchange"]);

  // What waits for connections to be closed (an upgrade or a deletion) goes on when one is.
  const waiting = new Map();
  function resume(def) {
    const list = waiting.get(def);
    if (!list) return;
    if (def.connections.size === 0 || [...def.connections].every((c) => c._closed)) {
      waiting.delete(def);
      for (const f of list) f();
    }
  }
  function whenClosed(def, f) {
    const open = [...def.connections].filter((c) => !c._closed);
    if (open.length === 0) { f(); return false; }
    const list = waiting.get(def) || [];
    list.push(f);
    waiting.set(def, list);
    return true;
  }

  // ---- The factory ----
  class IDBFactory {
    constructor(token) { if (token !== internal) throw new TypeError("Illegal constructor"); }
    open(name, version) {
      name = String(name);
      if (version !== undefined) {
        version = Number(version);
        if (!Number.isFinite(version) || Math.trunc(version) !== version || version < 1 || version > 9007199254740991) throw new TypeError("The version is not a number that can be one.");
      }
      const request = new IDBOpenDBRequest(internal);
      task(() => openDatabase(request, name, version));
      return request;
    }
    deleteDatabase(name) {
      name = String(name);
      const request = new IDBOpenDBRequest(internal);
      task(() => {
        let def = databases.get(name);
        if (!def || (def.connections.size === 0 && !def.saveTimer)) { const loaded = loadDef(name); if (loaded) { databases.set(name, loaded); def = loaded; } }
        const finish = () => {
          const old = def ? def.version : 0;
          if (def) { def.deleted = true; if (def.saveTimer) clearTimeout(def.saveTimer); }
          databases.delete(name);
          if (persistent()) { try { idbDelete(originOf(), name); } catch (e) { /* (kept) */ } }
          IDBRequest._set(request, { done: true, result: undefined, error: null });
          fireOn(request, "success", null, false, false, { oldVersion: old, newVersion: null });
        };
        if (!def) { finish(); return; }
        const open = [...def.connections].filter((c) => !c._closed);
        for (const c of open) {
          if (!c._closed) c.dispatchEvent(new IDBVersionChangeEvent("versionchange", { oldVersion: def.version, newVersion: null }));
        }
        const still = [...def.connections].filter((c) => !c._closed);
        if (still.length) fireOn(request, "blocked", null, false, false, { oldVersion: def.version, newVersion: null });
        whenClosed(def, finish);
      });
      return request;
    }
    databases() {
      return new Promise((resolve) => {
        const found = new Map();
        for (const line of persistent() ? String(idbList(originOf()) || "").split("\n") : []) {
          if (!line) continue;
          try { const h = JSON.parse(line); found.set(h.name, h.version); } catch (e) { /* (not a header) */ }
        }
        for (const d of databases.values()) if (d.version > 0) found.set(d.name, d.version);
        resolve([...found].map(([name, version]) => ({ name, version })));
      });
    }
    cmp(a, b) { return compare(toKey(a), toKey(b)); }
  }

  function openDatabase(request, name, requested) {
    let def = databases.get(name);
    // What is on disk is what the origin's other pages left, unless this page has it open (or has not written what it changed).
    if (!def || (def.connections.size === 0 && !def.saveTimer)) {
      const loaded = loadDef(name);
      if (loaded) { databases.set(name, loaded); def = loaded; }
    }
    if (!def) {
      def = { name, version: 0, stores: new Map(), connections: new Set(), created: true, saveTimer: null };
      databases.set(name, def);
    }
    const version = requested === undefined ? (def.version === 0 ? 1 : def.version) : requested;
    const failWith = (error) => {
      IDBRequest._set(request, { done: true, result: undefined, error });
      fireOn(request, "error", null, true, true);
      if (def.version === 0 && def.connections.size === 0) databases.delete(name);
    };
    if (def.version > version) { failWith(fail("VersionError", "The requested version is less than the existing one.")); return; }
    const connection = new IDBDatabase(internal, def);
    if (def.version === version) {
      IDBRequest._set(request, { done: true, result: connection, error: null });
      fireOn(request, "success", null, false, false);
      return;
    }
    // An upgrade: what is open is asked to close first.
    const oldVersion = def.version;
    const others = [...def.connections].filter((c) => c !== connection && !c._closed);
    for (const c of others) {
      if (!c._closed) c.dispatchEvent(new IDBVersionChangeEvent("versionchange", { oldVersion, newVersion: version }));
    }
    const start = () => {
      if (connection._closed) { failWith(fail("AbortError", "The connection was closed.")); return; }
      def.version = version;
      const tx = new IDBTransaction(internal, connection, [...def.stores.keys()], "versionchange", (ok) => {
        connection._upgrade = null;
        connection._upgrading = false;
        if (ok) {
          if (connection._closed) { failWith(fail("AbortError", "The connection was closed in the upgrade.")); return; }
          IDBRequest._set(request, { done: true, result: connection, error: null, transaction: null });
          fireOn(request, "success", null, false, false);
        } else {
          connection._forceClose();
          IDBRequest._set(request, { done: true, result: undefined, error: fail("AbortError", "The upgrade was aborted."), transaction: null });
          fireOn(request, "error", null, true, true);
          if (oldVersion === 0) { databases.delete(name); }
        }
      });
      connection._upgrade = tx;
      connection._upgrading = true;
      IDBRequest._set(request, { done: true, result: connection, error: null, transaction: tx });
      const event = new IDBVersionChangeEvent("upgradeneeded", { oldVersion, newVersion: version });
      request.dispatchEvent(event);
      // (An error that came out of the handler aborts the transaction; the transaction goes on when it has returned.)
    };
    const stillOpen = [...def.connections].filter((c) => c !== connection && !c._closed);
    if (stillOpen.length) fireOn(request, "blocked", null, false, false, { oldVersion, newVersion: version });
    const pending = [...def.connections].filter((c) => c !== connection && !c._closed);
    if (pending.length === 0) { start(); return; }
    const list = waiting.get(def) || [];
    list.push(start);
    waiting.set(def, list);
  }

  const factory = new IDBFactory(internal);
  const define = (name, value, enumerable = false) => Object.defineProperty(globalThis, name, { value, writable: true, configurable: true, enumerable });
  const tag = (cls, name) => Object.defineProperty(cls.prototype, Symbol.toStringTag, { value: name, configurable: true });
  for (const [name, cls] of Object.entries({ IDBFactory, IDBRequest, IDBOpenDBRequest, IDBDatabase, IDBTransaction, IDBObjectStore, IDBIndex, IDBKeyRange, IDBCursor, IDBCursorWithValue, IDBVersionChangeEvent })) {
    define(name, cls);
    tag(cls, name);
  }
  Object.defineProperty(globalThis, "indexedDB", { get() { return factory; }, configurable: true, enumerable: true });
})();
