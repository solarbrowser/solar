// What an image element knows of its picture (https://html.spec.whatwg.org/multipage/embedded-content.html#the-img-element): naturalWidth and naturalHeight, complete, decode() and
// the load and error events, for images that script makes (new Image(), img.src = ...), and for the ones of the markup when script asks. The file is fetched by the program
// (src/web-api/ImageLoad.cpp) and decoded by the canvas's natives (Skia); a document's own images are loaded by layout as before. canvas.js draws from what is kept here.
(() => {
  "use strict";
  const natives = globalThis.__solarImageNatives || {};
  const fetchN = globalThis.__solarImageFetch, decodeN = natives.decode, freeN = natives.free;
  delete globalThis.__solarImageFetch;
  delete globalThis.__solarImageNatives;
  // (canvas.js, which comes before this, has taken __solarImageDecode and __solarImageFree away: it leaves them where this can reach, as __solarImageNatives.)
  if (typeof HTMLImageElement !== "function") return;
  const proto = HTMLImageElement.prototype;
  const states = new WeakMap();
  const ids = typeof FinalizationRegistry === "function" ? new FinalizationRegistry((id) => { if (typeof freeN === "function") freeN(id); }) : null;
  const define = (target, name, descriptor) => Object.defineProperty(target, name, { configurable: true, enumerable: true, ...descriptor });
  const stateOf = (el) => {
    let s = states.get(el);
    if (!s) { s = { url: "", generation: 0, id: -1, w: 0, h: 0, complete: true, broken: false, started: false, waiters: [] }; states.set(el, s); }
    return s;
  };
  const canDecode = typeof decodeN === "function";

  const finish = (el, s, generation, result) => {
    if (generation !== s.generation) return;  // (the source changed)
    if (s.id >= 0 && ids) ids.unregister(s);
    s.complete = true;
    if (result) {
      s.id = result[0]; s.w = result[1]; s.h = result[2]; s.broken = false;
      if (ids) ids.register(s, result[0], s);
    } else {
      s.id = -1; s.w = s.h = 0; s.broken = true;
    }
    const waiters = s.waiters.splice(0);
    Promise.resolve().then(() => {
      if (generation !== s.generation) { for (const w of waiters) w.reject(new DOMException("The source image cannot be decoded.", "EncodingError")); return; }
      el.dispatchEvent(new Event(result ? "load" : "error"));
      for (const w of waiters) (result ? w.resolve() : w.reject(new DOMException("The source image cannot be decoded.", "EncodingError")));
    });
  };
  const decodeBytes = (el, s, generation, bytes) => {
    let result = null;
    try { result = canDecode && bytes ? decodeN(bytes) : null; } catch (e) { result = null; }
    finish(el, s, generation, result || null);
  };

  // The picture of the element is fetched (if the source is a new one).
  const load = (el) => {
    const s = stateOf(el);
    const attribute = el.getAttribute("src");
    const url = attribute ? new URL(attribute, el.baseURI).href : "";
    if (s.started && url === s.url) return s;
    s.started = true;
    s.url = url;
    const generation = ++s.generation;
    if (s.id >= 0 && ids) { ids.unregister(s); if (typeof freeN === "function") freeN(s.id); }
    s.id = -1; s.w = s.h = 0; s.broken = false;
    if (!url) { s.complete = true; return s; }
    s.complete = false;
    if (/^https?:/i.test(url) && typeof fetchN === "function") {
      const handle = fetchN(url, (status, type, bytes) => {
        if (status >= 200 && status < 300 && bytes) decodeBytes(el, s, generation, bytes); else finish(el, s, generation, null);
      });
      if (handle < 0) Promise.resolve().then(() => finish(el, s, generation, null));
    } else if (typeof fetch === "function") {
      // (A blob or a data address, or a program with no fetching of its own: fetch() has them.)
      fetch(url).then((r) => (r.ok ? r.arrayBuffer() : null)).then((bytes) => decodeBytes(el, s, generation, bytes), () => finish(el, s, generation, null));
    } else {
      Promise.resolve().then(() => finish(el, s, generation, null));
    }
    return s;
  };

  // The interface.
  const srcOriginal = Object.getOwnPropertyDescriptor(proto, "src");
  define(proto, "src", {
    get() { return srcOriginal && srcOriginal.get ? srcOriginal.get.call(this) : this.getAttribute("src") || ""; },
    set(v) { if (srcOriginal && srcOriginal.set) srcOriginal.set.call(this, v); else this.setAttribute("src", String(v)); load(this); },
  });
  const sized = (name) => ({
    get() {
      // (An image of the markup is asked for here the first time: the document's own load is the layout's.)
      const s = load(this);
      return s.complete && !s.broken ? s[name] : 0;
    },
  });
  define(proto, "naturalWidth", sized("w"));
  define(proto, "naturalHeight", sized("h"));
  define(proto, "complete", { get() { return load(this).complete; } });
  define(proto, "currentSrc", { get() { const a = this.getAttribute("src"); return a ? new URL(a, this.baseURI).href : ""; } });
  Object.defineProperty(proto, "decode", {
    value: function decode() {
      const s = load(this);
      if (s.complete) return s.broken ? Promise.reject(new DOMException("The source image cannot be decoded.", "EncodingError")) : Promise.resolve();
      return new Promise((resolve, reject) => s.waiters.push({ resolve, reject }));
    },
    writable: true, configurable: true, enumerable: true,
  });

  // new Image(width, height)
  if (typeof document !== "undefined") {
    const ImageConstructor = function Image(width, height) {
      if (!new.target) throw new TypeError("Failed to construct 'Image': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
      const el = document.createElement("img");
      if (width !== undefined) el.setAttribute("width", String(width));
      if (height !== undefined) el.setAttribute("height", String(height));
      return el;
    };
    ImageConstructor.prototype = proto;
    Object.defineProperty(globalThis, "Image", { value: ImageConstructor, writable: true, configurable: true, enumerable: false });
  }

  // For canvas.js: the element's picture as the natives hold it, with whether it is there.
  Object.defineProperty(globalThis, "__solarImageBitmap", {
    value: (el) => { const s = load(el); return { complete: s.complete, broken: s.broken, id: s.id, w: s.w, h: s.h }; },
    writable: true, configurable: true, enumerable: false,
  });

  // An image of the markup that has a load or an error handler (an attribute) is loaded at once, as a browser does for every one.
  if (typeof MutationObserver === "function" && typeof document !== "undefined") {
    const scan = (root) => { if (root.nodeType === 1) for (const el of root.localName === "img" ? [root] : root.querySelectorAll("img[onload], img[onerror]")) if (el.hasAttribute("src") && (el.hasAttribute("onload") || el.hasAttribute("onerror"))) load(el); };
    new MutationObserver((records) => { for (const r of records) for (const n of r.addedNodes) scan(n); }).observe(document, { childList: true, subtree: true });
    document.addEventListener("DOMContentLoaded", () => scan(document.documentElement), { once: true });
  }
})();
