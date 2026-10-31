// The media elements (https://html.spec.whatwg.org/multipage/media.html): HTMLMediaElement, on the players of src/media (FFmpeg) which the natives of
// src/web-api/Media.cpp hold. The element's attributes, the resource selection, the events and the promises of play() are done here; the decoding, the clock and
// the sound are the program's, and tell what happened by a callback, which reads the state and the events.
(() => {
  "use strict";
  const canPlayN = globalThis.__solarMediaCanPlay, openN = globalThis.__solarMediaOpen, controlN = globalThis.__solarMediaControl;
  const stateN = globalThis.__solarMediaState, eventsN = globalThis.__solarMediaEvents, attachN = globalThis.__solarMediaAttach, openMseN = globalThis.__solarMediaOpenMse;
  // (mediasource.js, which comes after, takes CanPlay too and lets go of it.)
  for (const n of ["Open", "Control", "State", "Events", "Attach", "OpenMse"]) delete globalThis["__solarMedia" + n];
  if (typeof canPlayN !== "function" || typeof HTMLMediaElement !== "function") return;

  const proto = HTMLMediaElement.prototype;
  const define = (target, name, descriptor) => Object.defineProperty(target, name, { configurable: true, enumerable: true, ...descriptor });
  const fail = (name, message) => new DOMException(message, name);
  const states = new WeakMap();

  // ---- TimeRanges and MediaError ----
  class TimeRanges {
    #r;
    constructor(ranges) { this.#r = ranges; }
    get length() { return this.#r.length; }
    start(i) { if (arguments.length === 0) throw new TypeError("Failed to execute 'start' on 'TimeRanges': 1 argument required, but only 0 present."); if (!(i >= 0 && i < this.#r.length)) throw fail("IndexSizeError", `Failed to execute 'start' on 'TimeRanges': The index provided (${i}) is greater than or equal to the maximum bound (${this.#r.length}).`); return this.#r[i][0]; }
    end(i) { if (arguments.length === 0) throw new TypeError("Failed to execute 'end' on 'TimeRanges': 1 argument required, but only 0 present."); if (!(i >= 0 && i < this.#r.length)) throw fail("IndexSizeError", `Failed to execute 'end' on 'TimeRanges': The index provided (${i}) is greater than or equal to the maximum bound (${this.#r.length}).`); return this.#r[i][1]; }
  }
  Object.defineProperty(TimeRanges.prototype, Symbol.toStringTag, { value: "TimeRanges", configurable: true });
  class MediaError {
    #code; #message;
    constructor(code, message) { this.#code = code; this.#message = message || ""; }
    get code() { return this.#code; }
    get message() { return this.#message; }
  }
  for (const [name, value] of [["MEDIA_ERR_ABORTED", 1], ["MEDIA_ERR_NETWORK", 2], ["MEDIA_ERR_DECODE", 3], ["MEDIA_ERR_SRC_NOT_SUPPORTED", 4]]) {
    Object.defineProperty(MediaError, name, { value, enumerable: true });
    Object.defineProperty(MediaError.prototype, name, { value, enumerable: true });
  }
  Object.defineProperty(MediaError.prototype, Symbol.toStringTag, { value: "MediaError", configurable: true });
  Object.defineProperty(globalThis, "TimeRanges", { value: TimeRanges, writable: true, configurable: true });
  Object.defineProperty(globalThis, "MediaError", { value: MediaError, writable: true, configurable: true });

  // ---- The state of an element ----
  const stateOf = (el) => {
    let s = states.get(el);
    if (!s) {
      s = {
        id: -1, key: null, currentSrc: "", paused: true, networkState: 0, readyState: 0, native: null, volume: 1, muted: !!el.defaultMuted, rate: 1, defaultRate: 1,
        error: null, plays: [], pendingSeek: null, startPosition: 0, timer: 0, lastTime: -1, autoplayDone: false, playing: false, sources: [], deferred: false, seekTarget: null,
        preservesPitch: true, ended: false, loadId: 0,
      };
      states.set(el, s);
    }
    return s;
  };
  const fire = (el, name) => Promise.resolve().then(() => el.dispatchEvent(new Event(name)));
  const stopTimer = (s) => { if (s.timer) { clearInterval(s.timer); s.timer = 0; } };
  const timeupdate = (el, s) => {
    const t = s.native ? s.native.currentTime : 0;
    if (t !== s.lastTime) { s.lastTime = t; fire(el, "timeupdate"); }
  };
  const settlePlays = (s, ok, error) => {
    const plays = s.plays.splice(0);
    for (const p of plays) (ok ? p.resolve() : p.reject(error));
  };

  // ---- What the player says ----
  const sync = (el, s, id) => {
    if (s.id !== id) return;  // (an older load)
    let st;
    try { st = JSON.parse(stateN(id)); } catch (e) { return; }
    const before = s.native;
    s.native = st;
    let names = [];
    try { names = JSON.parse(eventsN(id)); } catch (e) { /* (none) */ }
    // Things the element keeps in step with its attributes.
    controlN(id, "loop", !!el.loop);
    const wasReady = s.readyState;
    s.readyState = st.readyState;
    s.ended = st.ended;
    for (const name of names) {
      switch (name) {
        case "durationchange": fire(el, "durationchange"); break;
        case "loadedmetadata":
          s.networkState = 1;
          fire(el, "loadedmetadata");
          if (st.hasVideo) fire(el, "resize");
          if (s.startPosition > 0) { controlN(id, "seek", s.startPosition); s.startPosition = 0; }
          if (s.pendingSeek !== null) { controlN(id, "seek", s.pendingSeek); s.pendingSeek = null; }
          break;
        case "loadeddata": fire(el, "loadeddata"); break;
        case "canplay":
          fire(el, "canplay");
          if (!s.paused && !s.playing) { s.playing = true; fire(el, "playing"); settlePlays(s, true); }
          if (el.autoplay && s.paused && !s.autoplayDone) {
            s.autoplayDone = true;
            el.play().catch(() => {});
          }
          break;
        case "canplaythrough": fire(el, "canplaythrough"); break;
        case "waiting": if (!s.paused) { s.playing = false; fire(el, "waiting"); } break;
        case "seeking": fire(el, "seeking"); break;
        case "seeked": s.seekingLocal = false; fire(el, "timeupdate"); fire(el, "seeked"); if (!s.paused && !s.playing && st.readyState >= 3) { s.playing = true; fire(el, "playing"); } break;
        case "loop": fire(el, "timeupdate"); fire(el, "seeked"); break;
        case "ended":
          timeupdate(el, s);
          if (!el.loop) {
            if (!s.paused) { s.paused = true; s.playing = false; fire(el, "pause"); }
            fire(el, "ended");
            stopTimer(s);
          }
          break;
        case "error":
          s.error = new MediaError(st.error || 4, st.errorMessage);
          s.networkState = st.error === 4 ? 3 : 1;
          settlePlays(s, false, fail("NotSupportedError", "The element has no supported sources."));
          fire(el, "error");
          break;
      }
    }
    // A stall that has ended, while playing.
    if (before && before.stalled && !st.stalled && !s.paused && st.readyState >= 3 && !s.playing) { s.playing = true; fire(el, "playing"); }
    void wasReady;
  };

  // ---- Resource selection ----
  const mediaTypeOk = (type) => {
    if (!type) return true;
    const [mime, ...rest] = String(type).split(";");
    let codecs = "";
    for (const part of rest) { const m = /^\s*codecs\s*=\s*"?([^"]*)"?\s*$/i.exec(part); if (m) codecs = m[1]; }
    return canPlayN(mime.trim(), codecs) > 0;
  };
  const candidatesOf = (el) => {
    if (el.hasAttribute("src")) return [{ url: el.getAttribute("src"), source: null }];
    const out = [];
    for (const child of el.children) {
      if (child.localName === "source" && child.hasAttribute("src")) out.push({ url: child.getAttribute("src"), type: child.getAttribute("type"), source: child });
    }
    return out;
  };
  const keyOf = (el) => JSON.stringify(candidatesOf(el).map((c) => [c.url, c.type || ""]));

  const closePlayer = (el, s) => {
    stopTimer(s);
    if (s.id >= 0) { attachN(-1, el); controlN(s.id, "close"); }
    s.id = -1;
    if (s.mse) { s.mse._closed(); s.mse = null; }
  };

  const load = (el, s) => {
    const hadResource = s.id >= 0 || s.networkState !== 0;
    s.loadId++;
    const was = s.native;
    closePlayer(el, s);
    if (hadResource) {
      // https://html.spec.whatwg.org/multipage/media.html#media-element-load-algorithm
      if (s.plays.length) settlePlays(s, false, fail("AbortError", "The play() request was interrupted by a new load request."));
      if (was && was.readyState > 0) fire(el, "emptied");
      s.paused = true;
      s.playing = false;
    }
    s.native = null;
    s.readyState = 0;
    s.error = null;
    s.ended = false;
    s.currentSrc = "";
    s.lastTime = -1;
    s.key = keyOf(el);
    s.autoplayDone = false;
    s.deferred = false;
    s.networkState = 2;
    const candidates = candidatesOf(el);
    if (candidates.length === 0) { s.networkState = 0; return; }
    fire(el, "loadstart");
    // The first candidate that can be played (a source with a type that is not playable is passed over, with an error event at it).
    let chosen = null;
    for (const c of candidates) {
      if (c.source && !mediaTypeOk(c.type)) { fire(c.source, "error"); continue; }
      try { c.resolved = new URL(c.url, el.baseURI).href; } catch (e) { if (c.source) fire(c.source, "error"); continue; }
      chosen = c;
      break;
    }
    if (!chosen) {
      s.networkState = 3;
      if (!candidates[0].source) { s.error = new MediaError(4, "The URL is invalid."); fire(el, "error"); }
      return;
    }
    s.currentSrc = chosen.resolved;
    if (el.preload === "none" && !s.playWanted) { s.networkState = 1; s.deferred = true; fire(el, "suspend"); return; }
    start(el, s);
  };
  const start = (el, s) => {
    s.deferred = false;
    const myLoad = s.loadId;
    let id = -1;
    const mse = globalThis.__solarMediaSourceOf ? globalThis.__solarMediaSourceOf(s.currentSrc) : null;
    if (mse) {
      // The page makes the media: what it appends is what plays.
      id = openMseN(mse._id, () => { if (s.loadId === myLoad) sync(el, s, id); });
      if (id >= 0) { s.mse = mse; mse._open(el); }
    } else {
      id = openN(s.currentSrc, () => { if (s.loadId === myLoad) sync(el, s, id); });
    }
    if (id < 0) { s.networkState = 3; s.error = new MediaError(4, "The resource could not be opened."); fire(el, "error"); return; }
    s.id = id;
    attachN(id, el);
    controlN(id, "volume", s.volume);
    controlN(id, "muted", s.muted);
    controlN(id, "rate", s.rate);
    controlN(id, "loop", !!el.loop);
    // How much is fetched ahead: a video that is to be played, or whose page asked for it all (preload=auto), a lot; one that only wants its metadata, a little.
    controlN(id, "ahead", !s.paused || el.preload === "auto" ? 16 * 1048576 : 2 * 1048576);
    if (!s.paused) controlN(id, "play");
    // timeupdate while it plays
    s.timer = setInterval(() => { if (s.id === id && !s.paused) { s.native = JSON.parse(stateN(id)); timeupdate(el, s); } }, 250);
  };

  const ensure = (el) => {
    const s = stateOf(el);
    if (keyOf(el) !== s.key) load(el, s);
  };

  // ---- The element's interface ----
  const original = Object.getOwnPropertyDescriptor(proto, "src");
  define(proto, "src", {
    get() { return original && original.get ? original.get.call(this) : this.getAttribute("src") || ""; },
    set(v) { if (original && original.set) original.set.call(this, v); else this.setAttribute("src", String(v)); ensure(this); },
  });
  define(proto, "currentSrc", { get() { return stateOf(this).currentSrc; } });
  define(proto, "networkState", { get() { return stateOf(this).networkState; } });
  define(proto, "readyState", { get() { return stateOf(this).readyState; } });
  define(proto, "paused", { get() { return stateOf(this).paused; } });
  define(proto, "ended", { get() { return stateOf(this).ended; } });
  define(proto, "seeking", { get() { const s = stateOf(this); return s.seekingLocal || !!(s.native && s.native.seeking); } });
  define(proto, "error", { get() { return stateOf(this).error; } });
  define(proto, "duration", { get() { const s = stateOf(this); return s.native && s.native.duration !== null ? s.native.duration : NaN; } });
  define(proto, "currentTime", {
    get() {
      const s = stateOf(this);
      if (s.seekTarget !== null && (s.seekingLocal || (s.native && s.native.seeking))) return s.seekTarget;
      if (s.id >= 0) { try { s.native = JSON.parse(stateN(s.id)); } catch (e) { /* (kept) */ } }
      return s.native ? s.native.currentTime : s.pendingSeek !== null ? s.pendingSeek : 0;
    },
    set(v) {
      v = Number(v);
      if (!Number.isFinite(v)) throw new TypeError("Failed to set the 'currentTime' property on 'HTMLMediaElement': The provided double value is non-finite.");
      const s = stateOf(this);
      s.seekTarget = v;
      if (s.id >= 0 && s.readyState >= 1) { s.seekingLocal = true; controlN(s.id, "seek", v); } else s.pendingSeek = v;
    },
  });
  define(proto, "volume", {
    get() { return stateOf(this).volume; },
    set(v) {
      v = Number(v);
      if (!(v >= 0 && v <= 1)) throw fail("IndexSizeError", `Failed to set the 'volume' property on 'HTMLMediaElement': The volume provided (${v}) is outside the range [0, 1].`);
      const s = stateOf(this);
      if (s.volume === v) return;
      s.volume = v;
      if (s.id >= 0) controlN(s.id, "volume", v);
      fire(this, "volumechange");
    },
  });
  define(proto, "muted", {
    get() { return stateOf(this).muted; },
    set(v) {
      const s = stateOf(this);
      v = !!v;
      if (s.muted === v) return;
      s.muted = v;
      if (s.id >= 0) controlN(s.id, "muted", v);
      fire(this, "volumechange");
    },
  });
  define(proto, "playbackRate", {
    get() { return stateOf(this).rate; },
    set(v) {
      v = Number(v);
      if (!(v > 0) || !Number.isFinite(v)) throw fail("NotSupportedError", "Failed to set the 'playbackRate' property on 'HTMLMediaElement': The provided playback rate (" + v + ") is not supported.");
      const s = stateOf(this);
      if (s.rate === v) return;
      s.rate = v;
      if (s.id >= 0) controlN(s.id, "rate", v);
      fire(this, "ratechange");
    },
  });
  define(proto, "defaultPlaybackRate", { get() { return stateOf(this).defaultRate; }, set(v) { stateOf(this).defaultRate = Number(v) || 1; fire(this, "ratechange"); } });
  define(proto, "preservesPitch", { get() { return stateOf(this).preservesPitch; }, set(v) { stateOf(this).preservesPitch = !!v; } });
  define(proto, "buffered", { get() { const s = stateOf(this); return new TimeRanges(s.native ? s.native.buffered : []); } });
  define(proto, "seekable", { get() { const s = stateOf(this); return new TimeRanges(s.native && s.native.readyState > 0 && s.native.duration ? [[0, s.native.duration]] : []); } });
  define(proto, "played", { get() { const s = stateOf(this); return new TimeRanges(s.native && s.native.currentTime > 0 ? [[0, s.native.currentTime]] : []); } });
  define(proto, "srcObject", { get() { return stateOf(this).srcObject || null; }, set(v) { stateOf(this).srcObject = v; } });
  define(proto, "textTracks", { get() { return Object.assign([], { addEventListener() {}, removeEventListener() {}, getTrackById() { return null; } }); } });

  define(proto, "canPlayType", {
    value: function canPlayType(type) {
      if (arguments.length === 0) throw new TypeError("Failed to execute 'canPlayType' on 'HTMLMediaElement': 1 argument required, but only 0 present.");
      const [mime, ...rest] = String(type).split(";");
      let codecs = "";
      for (const part of rest) { const m = /^\s*codecs\s*=\s*"?([^"]*)"?\s*$/i.exec(part); if (m) codecs = m[1]; }
      return ["", "maybe", "probably"][canPlayN(mime.trim().toLowerCase(), codecs)];
    },
    writable: true,
  });
  define(proto, "load", { value: function load_() { load(this, stateOf(this)); }, writable: true });
  define(proto, "play", {
    value: function play() {
      const s = stateOf(this);
      if (s.error && s.error.code === 4) return Promise.reject(fail("NotSupportedError", "The element has no supported sources."));
      ensure(this);
      const wasPaused = s.paused;
      s.paused = false;
      s.playWanted = true;
      if (s.deferred) start(this, s);
      if (s.id >= 0) { controlN(s.id, "ahead", 16 * 1048576); controlN(s.id, "play"); }
      if (s.ended && s.id >= 0) { s.ended = false; }
      return new Promise((resolve, reject) => {
        if (wasPaused) {
          fire(this, "play");
          if (s.readyState >= 3) { s.playing = true; fire(this, "playing"); resolve(); } else { fire(this, "waiting"); s.plays.push({ resolve, reject }); }
        } else if (s.readyState >= 3) resolve(); else s.plays.push({ resolve, reject });
      });
    },
    writable: true,
  });
  define(proto, "pause", {
    value: function pause() {
      const s = stateOf(this);
      if (s.paused) return;
      s.paused = true;
      s.playing = false;
      if (s.id >= 0) controlN(s.id, "pause");
      settlePlays(s, false, fail("AbortError", "The play() request was interrupted by a call to pause()."));
      if (s.native) { try { s.native = JSON.parse(stateN(s.id)); } catch (e) { /* (kept) */ } }
      fire(this, "timeupdate");
      fire(this, "pause");
    },
    writable: true,
  });
  define(proto, "fastSeek", { value: function fastSeek(t) { this.currentTime = t; }, writable: true });
  define(proto, "getStartDate", { value: function getStartDate() { return new Date(NaN); }, writable: true });

  // The video element's size is the picture's.
  if (typeof HTMLVideoElement === "function") {
    const vp = HTMLVideoElement.prototype;
    define(vp, "videoWidth", { get() { const s = stateOf(this); return s.native ? s.native.videoWidth : 0; } });
    define(vp, "videoHeight", { get() { const s = stateOf(this); return s.native ? s.native.videoHeight : 0; } });
    define(vp, "getVideoPlaybackQuality", { value: function getVideoPlaybackQuality() { return { creationTime: performance.now(), totalVideoFrames: 0, droppedVideoFrames: 0, corruptedVideoFrames: 0, totalFrameDelay: 0 }; }, writable: true });
    // requestVideoFrameCallback: called about as often as the video has pictures (when it plays).
    define(vp, "requestVideoFrameCallback", {
      value: function requestVideoFrameCallback(callback) {
        const el = this;
        const handle = setTimeout(() => {
          const s = stateOf(el);
          callback(performance.now(), { presentationTime: performance.now(), expectedDisplayTime: performance.now() + 16, width: el.videoWidth, height: el.videoHeight, mediaTime: s.native ? s.native.currentTime : 0, presentedFrames: 0 });
        }, 33);
        return handle;
      },
      writable: true,
    });
    define(vp, "cancelVideoFrameCallback", { value: function cancelVideoFrameCallback(handle) { clearTimeout(handle); }, writable: true });
  }

  // new Audio(src): an audio element that begins to load (it is not in a document, where nothing would see it).
  if (typeof HTMLAudioElement === "function" && typeof document !== "undefined") {
    const AudioConstructor = function Audio(src) {
      if (!new.target) throw new TypeError("Failed to construct 'Audio': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
      const el = document.createElement("audio");
      el.setAttribute("preload", "auto");
      if (src !== undefined) el.src = String(src);
      return el;
    };
    AudioConstructor.prototype = HTMLAudioElement.prototype;
    Object.defineProperty(globalThis, "Audio", { value: AudioConstructor, writable: true, configurable: true, enumerable: false });
  }

  // ---- Finding the elements (the parser makes them without script) ----
  const scan = (root) => {
    if (root.nodeType !== 1) return;
    const list = [];
    if (root instanceof HTMLMediaElement) list.push(root);
    if (root.querySelectorAll) list.push(...root.querySelectorAll("video,audio"));
    for (const el of list) ensure(el);
  };
  if (typeof MutationObserver === "function" && typeof document !== "undefined") {
    const observer = new MutationObserver((records) => {
      for (const r of records) {
        if (r.type === "attributes") { if (r.target instanceof HTMLMediaElement) ensure(r.target); continue; }
        for (const node of r.addedNodes) {
          if (node.nodeType !== 1) continue;
          if (node.localName === "source" && node.parentNode instanceof HTMLMediaElement) ensure(node.parentNode);
          else scan(node);
        }
      }
    });
    observer.observe(document, { childList: true, subtree: true, attributes: true, attributeFilter: ["src"] });
    const begin = () => { if (document.documentElement) scan(document.documentElement); };
    begin();
    document.addEventListener("DOMContentLoaded", begin, { once: true });
  }
})();
