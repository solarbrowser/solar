// Text tracks (https://html.spec.whatwg.org/multipage/media.html#timed-text-tracks) and WebVTT (https://www.w3.org/TR/webvtt1/): TextTrack, TextTrackList, TextTrackCueList, TextTrackCue,
// VTTCue, VTTRegion, the track element (it fetches and parses its file), media.textTracks and addTextTrack, the cues that are active at the time of the media with their enter, exit and
// cuechange events. The cues are not drawn yet.
(() => {
  "use strict";
  if (typeof HTMLMediaElement !== "function" || typeof document === "undefined") return;
  const define = (target, name, descriptor) => Object.defineProperty(target, name, { configurable: true, enumerable: true, ...descriptor });
  const hidden = (target, name, value) => Object.defineProperty(target, name, { value, writable: true, configurable: true, enumerable: false });
  const fail = (name, message) => new DOMException(message, name);
  const publish = (C, name) => { Object.defineProperty(C.prototype, Symbol.toStringTag, { value: name, configurable: true }); hidden(globalThis, name, C); };
  const handlerProps = (C, types) => {
    for (const type of types) {
      const slot = Symbol("on" + type);
      define(C.prototype, "on" + type, {
        get() { return this[slot] || null; },
        set(f) { if (this[slot]) this.removeEventListener(type, this[slot]); this[slot] = typeof f === "function" ? f : null; if (this[slot]) this.addEventListener(type, this[slot]); },
      });
    }
  };
  const setCaptions = globalThis.__solarMediaCaptions;
  delete globalThis.__solarMediaCaptions;
  const TOKEN = Symbol("texttrack");
  const finite = (v, what) => { v = Number(v); if (!Number.isFinite(v)) throw new TypeError("The provided double value for " + what + " is non-finite."); return v; };

  // ---- Cues ----
  class TextTrackCue extends EventTarget {
    constructor(token) { if (token !== TOKEN) throw new TypeError("Illegal constructor"); super(); this._track = null; this._id = ""; this._start = 0; this._end = 0; this._pauseOnExit = false; this._active = false; }
    get track() { return this._track; }
    get id() { return this._id; }
    set id(v) { this._id = String(v); }
    get startTime() { return this._start; }
    set startTime(v) { this._start = finite(v, "startTime"); this._moved(); }
    get endTime() { return this._end; }
    set endTime(v) { this._end = finite(v, "endTime"); this._moved(); }
    get pauseOnExit() { return this._pauseOnExit; }
    set pauseOnExit(v) { this._pauseOnExit = !!v; }
    _moved() { if (this._track) this._track._cuesChanged(); }
  }
  for (const name of ["track", "id", "startTime", "endTime", "pauseOnExit"]) Object.defineProperty(TextTrackCue.prototype, name, { enumerable: true });
  handlerProps(TextTrackCue, ["enter", "exit"]);
  publish(TextTrackCue, "TextTrackCue");
  const ALIGN = ["start", "center", "end", "left", "right"];
  // The cue text as DOM: the cue text parsing rules (https://www.w3.org/TR/webvtt1/#cue-text-parsing-rules), with the entities and the tags c, i, b, u, ruby, rt, v, lang and timestamps.
  const ENTITIES = { "&amp;": "&", "&lt;": "<", "&gt;": ">", "&lrm;": "\u200E", "&rlm;": "\u200F", "&nbsp;": "\u00A0" };
  const unescapeText = (t) => t.replace(/&(amp|lt|gt|lrm|rlm|nbsp);/g, (m) => ENTITIES[m]);
  const cueToFragment = (input) => {
    const fragment = document.createDocumentFragment();
    let current = fragment;
    const stack = [];  // names of open elements
    const re = /<(\/?)([^>]*)>|([^<]+)|</g;
    let m;
    while ((m = re.exec(input)) !== null) {
      if (m[3] !== undefined) { current.appendChild(document.createTextNode(unescapeText(m[3]))); continue; }
      if (m[0] === "<") { current.appendChild(document.createTextNode("<")); continue; }
      const closing = m[1] === "/", body = m[2];
      if (closing) {
        const name = body.trim().split(/[.\s]/)[0];
        if (stack.length && stack[stack.length - 1].name === name) { stack.pop(); current = current.parentNode; }
        else if (name === "ruby" && stack.length >= 2 && stack[stack.length - 1].name === "rt" && stack[stack.length - 2].name === "ruby") { stack.pop(); stack.pop(); current = current.parentNode.parentNode; }
        continue;
      }
      const ts = timestamp(body.trim());
      if (ts !== null) { const pi = document.createProcessingInstruction("timestamp", body.trim()); current.appendChild(pi); continue; }
      const parts = /^([a-z]+)((?:\.[^\s.]+)*)(?:[ \t]+(.*))?$/.exec(body.trim());
      if (!parts) continue;
      const tag = parts[1], classes = parts[2].split(".").filter(Boolean), annotation = (parts[3] || "").trim();
      const element = { c: "span", i: "i", b: "b", u: "u", ruby: "ruby", rt: "rt", v: "span", lang: "span" }[tag];
      if (!element) continue;
      if (tag === "rt" && !(stack.length && stack[stack.length - 1].name === "ruby")) continue;
      const node = document.createElement(element);
      if (classes.length) node.setAttribute("class", classes.join(" "));
      if (tag === "v") node.setAttribute("title", annotation);
      if (tag === "lang") node.setAttribute("lang", annotation);
      current.appendChild(node);
      stack.push({ name: tag });
      current = node;
    }
    return fragment;
  };
  class VTTCue extends TextTrackCue {
    constructor(startTime, endTime, text) {
      if (arguments.length < 3) throw new TypeError("Failed to construct 'VTTCue': 3 arguments required, but only " + arguments.length + " present.");
      super(TOKEN);
      this._start = finite(startTime, "startTime"); this._end = finite(endTime, "endTime"); this._text = String(text);
      this._region = null; this._vertical = ""; this._snap = true; this._line = "auto"; this._lineAlign = "start"; this._position = "auto"; this._positionAlign = "auto"; this._size = 100; this._align = "center";
    }
    get region() { return this._region; }
    set region(v) { if (v !== null && !(v instanceof VTTRegion)) throw new TypeError("The provided value is not of type 'VTTRegion'."); this._region = v; }
    get vertical() { return this._vertical; }
    set vertical(v) { v = String(v); if (v === "" || v === "rl" || v === "lr") this._vertical = v; else throw new SyntaxError("The provided value '" + v + "' is not a valid enum value of type DirectionSetting."); }
    get snapToLines() { return this._snap; }
    set snapToLines(v) { this._snap = !!v; }
    get line() { return this._line; }
    set line(v) { this._line = v === "auto" ? "auto" : finite(v, "line"); }
    get lineAlign() { return this._lineAlign; }
    set lineAlign(v) { v = String(v); if (["start", "center", "end"].includes(v)) this._lineAlign = v; }
    get position() { return this._position; }
    set position(v) { if (v === "auto") { this._position = "auto"; return; } v = finite(v, "position"); if (v < 0 || v > 100) throw fail("IndexSizeError", "The value provided (" + v + ") is outside the range (0, 100)."); this._position = v; }
    get positionAlign() { return this._positionAlign; }
    set positionAlign(v) { v = String(v); if (["line-left", "center", "line-right", "auto"].includes(v)) this._positionAlign = v; }
    get size() { return this._size; }
    set size(v) { v = finite(v, "size"); if (v < 0 || v > 100) throw fail("IndexSizeError", "The value provided (" + v + ") is outside the range (0, 100)."); this._size = v; }
    get align() { return this._align; }
    set align(v) { v = String(v); if (ALIGN.includes(v)) this._align = v; }
    get text() { return this._text; }
    set text(v) { this._text = String(v); }
    getCueAsHTML() { return cueToFragment(this._text); }
  }
  Object.defineProperty(VTTCue, "length", { value: 3, configurable: true });
  for (const name of ["region", "vertical", "snapToLines", "line", "lineAlign", "position", "positionAlign", "size", "align", "text", "getCueAsHTML"]) Object.defineProperty(VTTCue.prototype, name, { enumerable: true });
  publish(VTTCue, "VTTCue");

  class VTTRegion {
    constructor() { this._id = ""; this._width = 100; this._lines = 3; this._regionAnchorX = 0; this._regionAnchorY = 100; this._viewportAnchorX = 0; this._viewportAnchorY = 100; this._scroll = ""; }
    get id() { return this._id; } set id(v) { this._id = String(v); }
    get width() { return this._width; } set width(v) { this._width = percent(v, "width"); }
    get lines() { return this._lines; } set lines(v) { this._lines = Number(v) >>> 0; }
    get regionAnchorX() { return this._regionAnchorX; } set regionAnchorX(v) { this._regionAnchorX = percent(v, "regionAnchorX"); }
    get regionAnchorY() { return this._regionAnchorY; } set regionAnchorY(v) { this._regionAnchorY = percent(v, "regionAnchorY"); }
    get viewportAnchorX() { return this._viewportAnchorX; } set viewportAnchorX(v) { this._viewportAnchorX = percent(v, "viewportAnchorX"); }
    get viewportAnchorY() { return this._viewportAnchorY; } set viewportAnchorY(v) { this._viewportAnchorY = percent(v, "viewportAnchorY"); }
    get scroll() { return this._scroll; } set scroll(v) { v = String(v); if (v === "" || v === "up") this._scroll = v; }
  }
  const percent = (v, what) => { v = finite(v, what); if (v < 0 || v > 100) throw fail("IndexSizeError", "The value provided (" + v + ") is outside the range (0, 100)."); return v; };
  for (const name of ["id", "width", "lines", "regionAnchorX", "regionAnchorY", "viewportAnchorX", "viewportAnchorY", "scroll"]) Object.defineProperty(VTTRegion.prototype, name, { enumerable: true });
  publish(VTTRegion, "VTTRegion");

  // ---- Lists of cues and of tracks: array-likes that stay in step ----
  const makeList = (name, itemsOf, extras) => {
    const C = ({ [name]: class { constructor(token) { if (token !== TOKEN) throw new TypeError("Illegal constructor"); } } })[name];
    const index = (k) => (typeof k === "string" && /^(0|[1-9]\d*)$/.test(k) && Number(k) < 4294967295 ? Number(k) : -1);
    const make = (owner) => {
      const target = new C(TOKEN);
      return new Proxy(target, {
        get(t, k, r) { if (k === "length") return itemsOf(owner).length; const i = index(k); if (i >= 0) return itemsOf(owner)[i]; if (k === Symbol.iterator) return () => itemsOf(owner)[Symbol.iterator](); return Reflect.get(t, k, r); },
        has(t, k) { const i = index(k); return i >= 0 ? i < itemsOf(owner).length : Reflect.has(t, k); },
        ownKeys(t) { return [...itemsOf(owner).map((_, i) => String(i)), ...Reflect.ownKeys(t)]; },
        getOwnPropertyDescriptor(t, k) { const i = index(k); if (i >= 0 && i < itemsOf(owner).length) return { value: itemsOf(owner)[i], writable: false, enumerable: true, configurable: true }; return Reflect.getOwnPropertyDescriptor(t, k); },
        set(t, k, v) { return index(k) >= 0 ? false : Reflect.set(t, k, v); },
        defineProperty(t, k, d) { return index(k) >= 0 ? false : Reflect.defineProperty(t, k, d); },
        deleteProperty(t, k) { return index(k) >= 0 ? false : Reflect.deleteProperty(t, k); },
      });
    };
    define(C.prototype, "length", { get() { return 0; } });
    publish(C, name);
    return { C, make };
  };
  const cueLists = makeList("TextTrackCueList", (track) => track._cues);
  method(cueLists.C.prototype, "getCueById", function (id) { return null; });
  const trackLists = makeList("TextTrackList", (owner) => owner._tracks);
  function method(proto, name, fn) { Object.defineProperty(proto, name, { value: fn, writable: true, configurable: true, enumerable: true }); }
  // The proxy's prototype methods look at the owner through a hidden link.
  const ownerOf = new WeakMap();
  const cueListFor = (track) => { if (!track._cueList) { track._cueList = cueLists.make(track); ownerOf.set(track._cueList, track); } return track._cueList; };
  Object.defineProperty(cueLists.C.prototype, "getCueById", { value: function getCueById(id) { const t = ownerOf.get(this); id = String(id); return t ? t._cues.find((c) => c._id === id && id !== "") || null : null; }, writable: true, configurable: true, enumerable: true });
  Object.defineProperty(cueLists.C.prototype, "item", { value: function item() { return undefined; }, writable: true, configurable: true, enumerable: false }); delete cueLists.C.prototype.item;

  // ---- A track ----
  const KINDS = ["subtitles", "captions", "descriptions", "chapters", "metadata"];
  class TextTrack extends EventTarget {
    constructor(token, kind, label, language, owner) { if (token !== TOKEN) throw new TypeError("Illegal constructor"); super(); this._kind = kind; this._label = label; this._language = language; this._mode = "disabled"; this._cues = []; this._owner = owner; this._active = []; this._id = ""; this._cueList = null; this._activeList = null; this._element = null; }
    get kind() { return this._kind; }
    get label() { return this._label; }
    get language() { return this._language; }
    get id() { return this._id; }
    get inBandMetadataTrackDispatchType() { return ""; }
    get mode() { return this._mode; }
    set mode(v) {
      v = String(v);
      if (v !== "disabled" && v !== "hidden" && v !== "showing") return;
      if (v === this._mode) return;
      this._mode = v;
      if (this._owner && this._owner._list) setTimeout(() => this._owner._list.dispatchEvent(new Event("change")), 0);
      if (v === "disabled") this._update(true); else { if (this._element) this._element._load(); this._update(); }
    }
    get cues() { return this._mode === "disabled" ? null : cueListFor(this); }
    get activeCues() { if (this._mode === "disabled") return null; if (!this._activeList) { this._activeList = activeListFor(this); } return this._activeList; }
    addCue(cue) {
      if (arguments.length < 1) throw new TypeError("Failed to execute 'addCue' on 'TextTrack': 1 argument required, but only 0 present.");
      if (!(cue instanceof TextTrackCue)) throw new TypeError("Failed to execute 'addCue' on 'TextTrack': parameter 1 is not of type 'TextTrackCue'.");
      if (cue._track && cue._track !== this) cue._track.removeCue(cue);
      if (this._cues.includes(cue)) return;
      let i = this._cues.length;
      while (i > 0 && this._cues[i - 1]._start > cue._start) i--;
      this._cues.splice(i, 0, cue);
      cue._track = this;
      this._cuesChanged();
    }
    removeCue(cue) {
      if (arguments.length < 1) throw new TypeError("Failed to execute 'removeCue' on 'TextTrack': 1 argument required, but only 0 present.");
      if (!(cue instanceof TextTrackCue)) throw new TypeError("Failed to execute 'removeCue' on 'TextTrack': parameter 1 is not of type 'TextTrackCue'.");
      const i = this._cues.indexOf(cue);
      if (i < 0) throw fail("NotFoundError", "The cue is not in the list of cues of this track.");
      this._cues.splice(i, 1);
      cue._track = null;
      cue._active = false;
      this._cuesChanged();
    }
    _cuesChanged() { this._update(); }
    // The cues that hold at the time of the media: those that came in or went out are told, and the track says the set changed.
    _update(clear) {
      const media = this._owner && this._owner._media;
      const now = media ? media.currentTime : 0;
      const before = this._active;
      const active = clear || this._mode === "disabled" ? [] : this._cues.filter((c) => c._start <= now && now < c._end);
      this._active = active;
      const entered = active.filter((c) => !before.includes(c)), left = before.filter((c) => !active.includes(c));
      if (!entered.length && !left.length) return;
      showCaptions(media);
      for (const c of left) { c._active = false; setTimeout(() => c.dispatchEvent(new Event("exit")), 0); }
      for (const c of entered) { c._active = true; setTimeout(() => c.dispatchEvent(new Event("enter")), 0); }
      setTimeout(() => { this.dispatchEvent(new Event("cuechange")); if (this._element) this._element.dispatchEvent(new Event("cuechange")); }, 0);
    }
  }
  for (const name of ["kind", "label", "language", "id", "inBandMetadataTrackDispatchType", "mode", "cues", "activeCues", "addCue", "removeCue"]) Object.defineProperty(TextTrack.prototype, name, { enumerable: true });
  handlerProps(TextTrack, ["cuechange"]);
  publish(TextTrack, "TextTrack");
  const activeLists = makeList("TextTrackCueList", (track) => track._active);
  const activeListFor = (track) => { const l = activeLists.make(track); ownerOf.set(l, track); Object.setPrototypeOf(l, cueLists.C.prototype); return l; };

  // ---- The tracks of a media element ----
  class MediaTracks {
    constructor(media) { this._media = media; this._tracks = []; this._list = null; }
  }
  const owners = new WeakMap();
  const ownerFor = (media) => { let o = owners.get(media); if (!o) { o = new MediaTracks(media); owners.set(media, o); } return o; };
  const trackListFor = (media) => {
    const o = ownerFor(media);
    if (!o._list) {
      const base = new (class TextTrackListBase extends EventTarget {})();
      const list = new Proxy(Object.setPrototypeOf(base, TextTrackListPrototype), {
        get(t, k, r) { if (k === "length") return o._tracks.length; if (typeof k === "string" && /^(0|[1-9]\d*)$/.test(k)) return o._tracks[Number(k)]; if (k === Symbol.iterator) return () => o._tracks[Symbol.iterator](); return Reflect.get(t, k, t); },
        has(t, k) { return typeof k === "string" && /^(0|[1-9]\d*)$/.test(k) ? Number(k) < o._tracks.length : Reflect.has(t, k); },
        ownKeys(t) { return [...o._tracks.map((_, i) => String(i)), ...Reflect.ownKeys(t)]; },
        getOwnPropertyDescriptor(t, k) { if (typeof k === "string" && /^(0|[1-9]\d*)$/.test(k) && Number(k) < o._tracks.length) return { value: o._tracks[Number(k)], writable: false, enumerable: true, configurable: true }; return Reflect.getOwnPropertyDescriptor(t, k); },
      });
      o._list = list;
      ownerOf.set(list, o);
    }
    return o._list;
  };
  const TextTrackListPrototype = Object.create(EventTarget.prototype);
  Object.defineProperty(TextTrackListPrototype, "constructor", { value: function TextTrackList() { throw new TypeError("Illegal constructor"); }, configurable: true, writable: true });
  Object.defineProperty(TextTrackListPrototype.constructor, "prototype", { value: TextTrackListPrototype });
  Object.defineProperty(TextTrackListPrototype, Symbol.toStringTag, { value: "TextTrackList", configurable: true });
  Object.defineProperty(TextTrackListPrototype, "getTrackById", { value: function getTrackById(id) { const o = ownerOf.get(this); id = String(id); return o ? o._tracks.find((t) => t._id === id) || null : null; }, writable: true, configurable: true, enumerable: true });
  handlerProps({ prototype: TextTrackListPrototype }, ["addtrack", "removetrack", "change"]);
  hidden(globalThis, "TextTrackList", TextTrackListPrototype.constructor);

  const addTrack = (media, track) => {
    const o = ownerFor(media);
    trackListFor(media);
    o._tracks.push(track);
    track._owner = o;
    setTimeout(() => o._list.dispatchEvent(Object.assign(new Event("addtrack"), { track })), 0);
  };
  const removeTrack = (media, track) => {
    const o = ownerFor(media);
    const i = o._tracks.indexOf(track);
    if (i < 0) return;
    o._tracks.splice(i, 1);
    track._update(true);
    setTimeout(() => o._list && o._list.dispatchEvent(Object.assign(new Event("removetrack"), { track })), 0);
  };
  define(HTMLMediaElement.prototype, "textTracks", { get() { return trackListFor(this); } });
  hidden(HTMLMediaElement.prototype, "addTextTrack", function addTextTrack(kind, label = "", language = "") {
    if (arguments.length < 1) throw new TypeError("Failed to execute 'addTextTrack' on 'HTMLMediaElement': 1 argument required, but only 0 present.");
    kind = String(kind);
    if (!KINDS.includes(kind)) throw new TypeError("Failed to execute 'addTextTrack' on 'HTMLMediaElement': The provided value '" + kind + "' is not a valid enum value of type TextTrackKind.");
    const track = new TextTrack(TOKEN, kind, String(label), String(language), null);
    track._mode = "hidden";
    addTrack(this, track);
    return track;
  });
  // The text of the cues that are active on the tracks that show, on a video.
  const showCaptions = (media) => {
    if (typeof setCaptions !== "function" || !media || !(media instanceof HTMLVideoElement)) return;
    const lines = [];
    for (const t of ownerFor(media)._tracks) {
      if (t._mode !== "showing" || (t._kind !== "subtitles" && t._kind !== "captions")) continue;
      for (const c of t._active) lines.push(c._text.replace(/<[^>]*>/g, ""));
    }
    setCaptions(media, lines.join("\n"));
  };
  // The cues follow the time of the media.
  const follow = (media) => { for (const t of ownerFor(media)._tracks) t._update(); };
  document.addEventListener("timeupdate", (e) => { if (e.target instanceof HTMLMediaElement && owners.has(e.target)) follow(e.target); }, true);
  document.addEventListener("seeked", (e) => { if (e.target instanceof HTMLMediaElement && owners.has(e.target)) follow(e.target); }, true);

  // ---- WebVTT ----
  const timestamp = (s) => { const m = /^(?:(\d{2,}):)?(\d{2}):(\d{2})\.(\d{3})$/.exec(s); if (!m) return null; const h = m[1] ? +m[1] : 0, mi = +m[2], se = +m[3]; if (mi > 59 || se > 59) return null; return h * 3600 + mi * 60 + se + +m[4] / 1000; };
  const parseVtt = (text) => {
    text = text.replace(/^﻿/, "").replace(/\r\n?/g, "\n");
    const lines = text.split("\n");
    if (!/^WEBVTT([ \t].*)?$/.test(lines[0] || "")) return null;
    const cues = [], regions = [];
    let i = 1;
    // The header: up to the first blank line.
    while (i < lines.length && lines[i] !== "") i++;
    while (i < lines.length) {
      while (i < lines.length && lines[i] === "") i++;
      if (i >= lines.length) break;
      const block = [];
      while (i < lines.length && lines[i] !== "") block.push(lines[i++]);
      if (/^(NOTE|STYLE|REGION)(\s|$)/.test(block[0])) continue;
      let at = 0;
      if (!block[0].includes("-->") && block.length > 1) at = 1;
      const timing = /^(\S+)[ \t]+-->[ \t]+(\S+)(?:[ \t]+(.*))?$/.exec(block[at] || "");
      if (!timing) continue;
      const start = timestamp(timing[1]), end = timestamp(timing[2]);
      if (start === null || end === null || end <= start && false) continue;
      const cue = new VTTCue(start, end, block.slice(at + 1).join("\n"));
      if (at === 1) cue._id = block[0];
      for (const setting of (timing[3] || "").split(/[ \t]+/).filter(Boolean)) {
        const [k, v] = [setting.slice(0, setting.indexOf(":")), setting.slice(setting.indexOf(":") + 1)];
        try {
          if (k === "vertical" && (v === "rl" || v === "lr")) cue.vertical = v;
          else if (k === "line") { const m = /^(-?[\d.]+)(%)?(?:,(start|center|end))?$/.exec(v); if (m) { cue.line = Number(m[1]); cue.snapToLines = !m[2]; if (m[3]) cue.lineAlign = m[3]; } }
          else if (k === "position") { const m = /^([\d.]+)%(?:,(line-left|center|line-right))?$/.exec(v); if (m) { cue.position = Number(m[1]); if (m[2]) cue.positionAlign = m[2]; } }
          else if (k === "size") { const m = /^([\d.]+)%$/.exec(v); if (m) cue.size = Number(m[1]); }
          else if (k === "align" && ALIGN.includes(v)) cue.align = v;
        } catch (e) { /* a setting out of range is left out */ }
      }
      cues.push(cue);
    }
    return cues;
  };

  // ---- The track element ----
  if (typeof HTMLTrackElement === "function") {
    const P = HTMLTrackElement.prototype;
    const infos = new WeakMap();
    const infoOf = (el) => { let i = infos.get(el); if (!i) { i = { readyState: 0, track: null, media: null, loaded: false }; infos.set(el, i); } return i; };
    const kindOf = (el) => { const v = (el.getAttribute("kind") || "").toLowerCase(); return v === "" && !el.hasAttribute("kind") ? "subtitles" : KINDS.includes(v) ? v : "metadata"; };
    define(P, "kind", { get() { return kindOf(this); }, set(v) { this.setAttribute("kind", String(v)); } });
    define(P, "readyState", { get() { return infoOf(this).readyState; } });
    for (const [name, value] of [["NONE", 0], ["LOADING", 1], ["LOADED", 2], ["ERROR", 3]]) { Object.defineProperty(HTMLTrackElement, name, { value, enumerable: true }); Object.defineProperty(P, name, { value, enumerable: true }); }
    define(P, "track", { get() { return trackOf(this); } });
    const trackOf = (el) => {
      const info = infoOf(el);
      if (!info.track) {
        info.track = new TextTrack(TOKEN, kindOf(el), el.getAttribute("label") || "", el.getAttribute("srclang") || "", null);
        info.track._element = el;
        el._load = () => loadTrack(el);
      }
      return info.track;
    };
    const loadTrack = (el) => {
      const info = infoOf(el), track = trackOf(el);
      if (info.loaded || info.readyState === 1) return;
      const src = el.getAttribute("src");
      if (!src) return;
      info.readyState = 1;
      info.loaded = true;
      let url;
      try { url = new URL(src, el.baseURI).href; } catch (e) { info.readyState = 3; setTimeout(() => el.dispatchEvent(new Event("error")), 0); return; }
      fetch(url).then((r) => (r.ok ? r.text() : Promise.reject(new Error("status")))).then((text) => {
        const cues = parseVtt(text);
        if (cues === null) throw new Error("not WebVTT");
        for (const c of cues) track.addCue(c);
        info.readyState = 2;
        el.dispatchEvent(new Event("load"));
      }).catch(() => { info.readyState = 3; el.dispatchEvent(new Event("error")); });
    };
    // A track element in a media element gives it its track.
    const attach = (el) => {
      const parent = el.parentNode;
      if (!(parent instanceof HTMLMediaElement)) return;
      const info = infoOf(el), track = trackOf(el);
      if (info.media === parent) return;
      info.media = parent;
      const o = ownerFor(parent);
      if (!o._tracks.includes(track)) {
        // The elements' tracks come in tree order, before those a script added.
        trackListFor(parent);
        const first = o._tracks.findIndex((t) => !t._element);
        track._owner = o;
        o._tracks.splice(first < 0 ? o._tracks.length : first, 0, track);
        setTimeout(() => o._list.dispatchEvent(Object.assign(new Event("addtrack"), { track })), 0);
      }
      if (el.hasAttribute("default") && !o._tracks.some((t) => t !== track && t._mode === "showing")) track.mode = "showing";
    };
    const detach = (el) => {
      const info = infos.get(el);
      if (!info || !info.media) return;
      removeTrack(info.media, info.track);
      info.media = null;
    };
    if (typeof MutationObserver === "function") {
      new MutationObserver((records) => {
        for (const r of records) {
          for (const n of r.addedNodes) if (n.nodeType === 1) for (const el of n.localName === "track" ? [n] : n.querySelectorAll("track")) attach(el);
          for (const n of r.removedNodes) if (n.nodeType === 1) for (const el of n.localName === "track" ? [n] : n.querySelectorAll("track")) detach(el);
        }
      }).observe(document, { childList: true, subtree: true });
    }
    document.addEventListener("DOMContentLoaded", () => { for (const el of document.querySelectorAll("track")) attach(el); }, { once: true });
    // (Attributes the track reflects are read when it is asked.)
    for (const [name, attr] of [["label", "label"], ["srclang", "srclang"]]) define(P, name, { get() { return this.getAttribute(attr) || ""; }, set(v) { this.setAttribute(attr, String(v)); } });
  }
})();
