// Media Source Extensions (https://www.w3.org/TR/media-source-2/): MediaSource and SourceBuffer, and URL.createObjectURL(mediaSource) for an element's src. The bytes
// appended to a SourceBuffer are demuxed and kept by the program (src/media/Mse.cpp); a player of a media element plays them (media.js). What a SourceBuffer does
// takes time, and says so through updating and its events, which are told by the program's callback.
(() => {
  "use strict";
  const createN = globalThis.__solarMseCreate, addN = globalThis.__solarMseAddBuffer, appendN = globalThis.__solarMseAppend;
  const bufferN = globalThis.__solarMseBuffer, sourceN = globalThis.__solarMseSource, canPlayN = globalThis.__solarMediaCanPlay;
  for (const n of ["Create", "AddBuffer", "Append", "Buffer", "Source"]) delete globalThis["__solarMse" + n];
  delete globalThis.__solarMediaCanPlay;
  if (typeof createN !== "function" || typeof EventTarget !== "function") return;
  const fail = (name, message) => new DOMException(message, name);
  const hide = (name, value) => Object.defineProperty(globalThis, name, { value, writable: true, configurable: true, enumerable: false });
  const token = Symbol("mse");
  const ranges = (list) => (typeof TimeRanges === "function" ? new TimeRanges(list) : list);

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
  const supported = (type) => {
    const [mime, ...rest] = String(type).split(";");
    let codecs = "";
    for (const part of rest) { const m = /^\s*codecs\s*=\s*"?([^"]*)"?\s*$/i.exec(part); if (m) codecs = m[1]; }
    return canPlayN(mime.trim().toLowerCase(), codecs) > 0;
  };

  // ---- SourceBuffer ----
  class SourceBuffer extends EventTarget {
    #source; #id; #updating = false; #mode = "segments"; #offset = 0; #winStart = 0; #winEnd = Infinity; #removed = false; #type;
    constructor(t, source, id, type) { super(); if (t !== token) throw new TypeError("Illegal constructor"); this.#source = source; this.#id = id; this.#type = type; }
    get updating() { return this.#updating; }
    get mode() { return this.#mode; }
    set mode(v) { v = String(v); if (v !== "segments" && v !== "sequence") return; if (this.#updating) throw fail("InvalidStateError", "This SourceBuffer is still processing an 'appendBuffer' or 'remove' operation."); this.#mode = v; }
    get timestampOffset() { return this.#offset; }
    set timestampOffset(v) {
      v = Number(v);
      if (!Number.isFinite(v)) throw new TypeError("The provided double value is non-finite.");
      this.#check();
      if (this.#updating) throw fail("InvalidStateError", "This SourceBuffer is still processing an 'appendBuffer' or 'remove' operation.");
      this.#offset = v;
      bufferN(this.#id, "offset", v);
    }
    get appendWindowStart() { return this.#winStart; }
    set appendWindowStart(v) { this.#winStart = Number(v) || 0; }
    get appendWindowEnd() { return this.#winEnd; }
    set appendWindowEnd(v) { this.#winEnd = Number(v); }
    get buffered() {
      this.#check();
      const state = JSON.parse(bufferN(this.#id, "state"));
      return ranges(state.buffered);
    }
    get audioTracks() { return []; }
    get videoTracks() { return []; }
    get textTracks() { return []; }
    #check() { if (this.#removed) throw fail("InvalidStateError", "This SourceBuffer has been removed from the parent media source."); }
    #begin() {
      this.#check();
      if (this.#updating) throw fail("InvalidStateError", "The SourceBuffer is still processing an 'appendBuffer' or 'remove' operation.");
      if (this.#source.readyState === "closed") throw fail("InvalidStateError", "The MediaSource's readyState is not 'open'.");
      if (this.#source.readyState === "ended") this.#source._reopen();
    }
    appendBuffer(data) {
      if (arguments.length === 0) throw new TypeError("Failed to execute 'appendBuffer' on 'SourceBuffer': 1 argument required, but only 0 present.");
      this.#begin();
      let bytes;
      if (data instanceof ArrayBuffer) bytes = data.slice(0);
      else if (ArrayBuffer.isView(data)) bytes = data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength);
      else throw new TypeError("Failed to execute 'appendBuffer' on 'SourceBuffer': The provided value is not of type '(ArrayBuffer or ArrayBufferView)'.");
      if (!appendN(this.#id, bytes)) throw fail("QuotaExceededError", "The SourceBuffer is full, and cannot free space to append additional buffers.");
      this.#updating = true;
      Promise.resolve().then(() => this.dispatchEvent(new Event("updatestart")));
    }
    abort() {
      this.#check();
      if (this.#source.readyState !== "open") throw fail("InvalidStateError", "The MediaSource's readyState is not 'open'.");
      if (this.#updating) {
        bufferN(this.#id, "abort");
        this.#updating = false;
        Promise.resolve().then(() => { this.dispatchEvent(new Event("abort")); this.dispatchEvent(new Event("updateend")); });
      }
      this.#winStart = 0;
      this.#winEnd = Infinity;
    }
    remove(start, end) {
      start = Number(start); end = Number(end);
      this.#begin();
      const duration = this.#source.duration;
      if (!(start >= 0) || (!Number.isNaN(duration) && start > duration) || Number.isNaN(start)) throw new TypeError("Failed to execute 'remove' on 'SourceBuffer': The start provided (" + start + ") is outside the allowed range.");
      if (!(end > start)) throw new TypeError("Failed to execute 'remove' on 'SourceBuffer': The end provided (" + end + ") is less than or equal to the start (" + start + ").");
      this.#updating = true;
      Promise.resolve().then(() => {
        this.dispatchEvent(new Event("updatestart"));
        bufferN(this.#id, "remove", start, end);
        this.#updating = false;
        this.dispatchEvent(new Event("update"));
        this.dispatchEvent(new Event("updateend"));
      });
    }
    changeType(type) {
      this.#check();
      if (!supported(type)) throw fail("NotSupportedError", "The type provided ('" + type + "') is not supported.");
      if (this.#updating) throw fail("InvalidStateError", "This SourceBuffer is still processing an 'appendBuffer' or 'remove' operation.");
      this.#type = String(type);
    }
    // The program says an append is done (or went wrong).
    _poll() {
      if (!this.#updating) return;
      const state = JSON.parse(bufferN(this.#id, "state"));
      if (state.updating && !state.failed) return;
      this.#updating = false;
      if (state.failed) {
        this.dispatchEvent(new Event("error"));
        this.dispatchEvent(new Event("updateend"));
        this.#source._decodeError();
        return;
      }
      this.dispatchEvent(new Event("update"));
      this.dispatchEvent(new Event("updateend"));
      this.#source._grew(this.buffered);
    }
    _removed() { this.#removed = true; }
    get _id() { return this.#id; }
  }
  Object.defineProperty(SourceBuffer.prototype, Symbol.toStringTag, { value: "SourceBuffer", configurable: true });
  handler(SourceBuffer.prototype, ["updatestart", "update", "updateend", "error", "abort"]);

  class SourceBufferList extends EventTarget {
    #items = [];
    get length() { return this.#items.length; }
    item(i) { return this.#items[i] || null; }
    [Symbol.iterator]() { return this.#items[Symbol.iterator](); }
    _add(b) { this.#items.push(b); this.#expose(); this.dispatchEvent(new Event("addsourcebuffer")); }
    _remove(b) { const at = this.#items.indexOf(b); if (at >= 0) this.#items.splice(at, 1); this.#expose(); this.dispatchEvent(new Event("removesourcebuffer")); }
    // (list[0] ... list[n]: accessors made once, since a property that is deleted from an event target is not safe to.)
    constructor() {
      super();
      for (let i = 0; i < 32; i++) Object.defineProperty(this, i, { get: () => this.#items[i], configurable: true, enumerable: false });
    }
    #expose() {}
    _list() { return this.#items; }
  }
  Object.defineProperty(SourceBufferList.prototype, Symbol.toStringTag, { value: "SourceBufferList", configurable: true });
  handler(SourceBufferList.prototype, ["addsourcebuffer", "removesourcebuffer"]);

  // ---- MediaSource ----
  const urls = new Map();   // object URL -> MediaSource
  class MediaSource extends EventTarget {
    #id; #state = "closed"; #buffers = new SourceBufferList(); #duration = NaN; #element = null;
    constructor() {
      super();
      this.#id = createN(() => { for (const b of this.#buffers._list()) b._poll(); });
      if (this.#id < 0) throw new TypeError("Failed to construct 'MediaSource': media sources are not available.");
    }
    static isTypeSupported(type) { if (arguments.length === 0) throw new TypeError("Failed to execute 'isTypeSupported' on 'MediaSource': 1 argument required, but only 0 present."); return supported(type); }
    static get canConstructInDedicatedWorker() { return false; }
    get readyState() { return this.#state; }
    get sourceBuffers() { return this.#buffers; }
    get activeSourceBuffers() { return this.#buffers; }
    get duration() { return this.#duration; }
    set duration(v) {
      v = Number(v);
      if (Number.isNaN(v) && arguments[0] !== undefined && typeof arguments[0] === "number" && !Number.isNaN(arguments[0])) v = NaN;
      if (v < 0) throw new TypeError("Failed to set the 'duration' property on 'MediaSource': The value provided (" + v + ") is negative.");
      if (this.#state !== "open") throw fail("InvalidStateError", "The MediaSource's readyState is not 'open'.");
      if (this.#buffers._list().some((b) => b.updating)) throw fail("InvalidStateError", "One or more SourceBuffers are still processing an 'appendBuffer' or 'remove' operation.");
      this.#duration = v;
      sourceN(this.#id, "duration", v);
    }
    get _id() { return this.#id; }
    addSourceBuffer(type) {
      if (arguments.length === 0) throw new TypeError("Failed to execute 'addSourceBuffer' on 'MediaSource': 1 argument required, but only 0 present.");
      type = String(type);
      if (type === "") throw new TypeError("Failed to execute 'addSourceBuffer' on 'MediaSource': The type provided is empty.");
      if (this.#state !== "open") throw fail("InvalidStateError", "The MediaSource's readyState is not 'open'.");
      if (!supported(type)) throw fail("NotSupportedError", "The type provided ('" + type + "') is unsupported.");
      const id = addN(this.#id, type);
      if (id < 0) throw fail("NotSupportedError", "The type provided ('" + type + "') is unsupported.");
      const buffer = new SourceBuffer(token, this, id, type);
      this.#buffers._add(buffer);
      return buffer;
    }
    removeSourceBuffer(buffer) {
      if (!(buffer instanceof SourceBuffer) || !this.#buffers._list().includes(buffer)) throw fail("NotFoundError", "The SourceBuffer provided is not contained in this MediaSource.");
      if (buffer.updating) buffer.abort();
      sourceN(this.#id, "removeBuffer", buffer._id);
      buffer._removed();
      this.#buffers._remove(buffer);
    }
    endOfStream(error) {
      if (this.#state !== "open") throw fail("InvalidStateError", "The MediaSource's readyState is not 'open'.");
      if (this.#buffers._list().some((b) => b.updating)) throw fail("InvalidStateError", "One or more SourceBuffers are still processing an 'appendBuffer' or 'remove' operation.");
      this.#state = "ended";
      if (error === undefined) {
        // The duration becomes the end of what is there.
        const ranges = JSON.parse(sourceN(this.#id, "buffered"));
        if (ranges.length) { const end = ranges[ranges.length - 1][1]; if (!(this.#duration >= end)) { this.#duration = end; sourceN(this.#id, "duration", end); } }
        sourceN(this.#id, "end");
      } else {
        sourceN(this.#id, "end");
        if (this.#element) { this.#element.dispatchEvent(new Event("error")); }
      }
      Promise.resolve().then(() => this.dispatchEvent(new Event("sourceended")));
    }
    setLiveSeekableRange() {}
    clearLiveSeekableRange() {}
    // The element that has this MediaSource as its source loads it: it opens.
    _open(element) {
      this.#element = element;
      this.#state = "open";
      Promise.resolve().then(() => this.dispatchEvent(new Event("sourceopen")));
    }
    _closed() {
      if (this.#state === "closed") return;
      this.#state = "closed";
      this.#element = null;
      Promise.resolve().then(() => this.dispatchEvent(new Event("sourceclose")));
    }
    _reopen() { this.#state = "open"; sourceN(this.#id, "reopen"); Promise.resolve().then(() => this.dispatchEvent(new Event("sourceopen"))); }
    _grew(buffered) {
      // The duration grows with what is appended (when the page left it unset or short).
      let end = 0;
      for (let i = 0; i < buffered.length; i++) end = Math.max(end, buffered.end(i));
      if (this.#state === "open" && (Number.isNaN(this.#duration) || this.#duration < end)) { this.#duration = end; sourceN(this.#id, "duration", end); }
    }
    _decodeError() { if (this.#element) this.#element.dispatchEvent(new Event("error")); }
  }
  Object.defineProperty(MediaSource.prototype, Symbol.toStringTag, { value: "MediaSource", configurable: true });
  handler(MediaSource.prototype, ["sourceopen", "sourceended", "sourceclose"]);
  hide("MediaSource", MediaSource);
  hide("SourceBuffer", SourceBuffer);
  hide("SourceBufferList", SourceBufferList);

  // ---- URL.createObjectURL(mediaSource) ----
  if (typeof URL === "function") {
    const createObjectURL = URL.createObjectURL, revokeObjectURL = URL.revokeObjectURL;
    Object.defineProperty(URL, "createObjectURL", {
      value: function createObjectURL_(object) {
        if (object instanceof MediaSource) {
          const url = "blob:" + location.origin + "/" + crypto.randomUUID();
          urls.set(url, object);
          return url;
        }
        return createObjectURL.apply(this, arguments);
      },
      writable: true, configurable: true,
    });
    Object.defineProperty(URL, "revokeObjectURL", {
      value: function revokeObjectURL_(url) { if (urls.delete(String(url))) return; return revokeObjectURL.apply(this, arguments); },
      writable: true, configurable: true,
    });
  }
  Object.defineProperty(globalThis, "__solarMediaSourceOf", { value: (url) => urls.get(url) || null, configurable: true, writable: true, enumerable: false });
})();
