// What an image element knows of its picture (https://html.spec.whatwg.org/multipage/embedded-content.html#the-img-element): naturalWidth and naturalHeight, complete, decode() and
// the load and error events, for images that script makes (new Image(), img.src = ...), and for the ones of the markup when script asks. The file is fetched by the program
// (src/web-api/ImageLoad.cpp) and decoded by the canvas's natives (Skia); a document's own images are loaded by layout as before. canvas.js draws from what is kept here.
(() => {
  "use strict";
  const natives = globalThis.__solarImageNatives || {};
  const fetchN = globalThis.__solarImageFetch, decodeN = natives.decode, freeN = natives.free;
  delete globalThis.__solarImageFetch;
  delete globalThis.__solarImageNatives;
  const delayLoad = globalThis.__solarDelayLoad, resumeLoad = globalThis.__solarResumeLoad;
  delete globalThis.__solarDelayLoad;
  delete globalThis.__solarResumeLoad;
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
    if (s.release) { const release = s.release; s.release = null; release(); }
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


  // ---- Which picture: src, srcset, sizes and picture (https://html.spec.whatwg.org/multipage/images.html#updating-the-image-data) ----
  const isSpace = (c) => c === " " || c === "\t" || c === "\n" || c === "\f" || c === "\r";
  // Parsing a srcset attribute: candidates with a URL and a width or density.
  const parseSrcset = (input) => {
    const candidates = [];
    let pos = 0;
    const n = input.length;
    for (;;) {
      while (pos < n && (isSpace(input[pos]) || input[pos] === ",")) pos++;
      if (pos >= n) return candidates;
      let start = pos;
      while (pos < n && !isSpace(input[pos])) pos++;
      let url = input.slice(start, pos);
      const descriptors = [];
      if (url.endsWith(",")) { url = url.replace(/,+$/, ""); }
      else {
        // The descriptors: up to the comma that is outside parentheses.
        let current = "", state = "in-descriptor";
        let depth = 0;
        for (;;) {
          while (pos < n && isSpace(input[pos]) && state !== "after") pos++;
          if (pos >= n) { if (current) descriptors.push(current); break; }
          const c = input[pos];
          if (state === "in-descriptor") {
            if (isSpace(c)) { if (current) { descriptors.push(current); current = ""; } state = "after"; }
            else if (c === ",") { pos++; if (current) descriptors.push(current); break; }
            else if (c === "(") { current += c; state = "in-parens"; }
            else current += c;
          } else if (state === "in-parens") {
            if (c === ")") { current += c; state = "in-descriptor"; } else current += c;
          } else {
            if (!isSpace(c)) state = "in-descriptor"; else { pos++; continue; }
            continue;
          }
          pos++;
        }
        if (state === "after" && current) descriptors.push(current);
      }
      if (url === "") continue;
      // Descriptors: w, x, h (h is accepted and ignored, and not with w and x); an error drops the candidate.
      let width = null, density = null, error = false, height = null;
      for (const d of descriptors) {
        const last = d[d.length - 1], value = d.slice(0, -1);
        if (last === "w" && /^\d+$/.test(value)) { if (width !== null || density !== null) error = true; else { width = parseInt(value, 10); if (width === 0) error = true; } }
        else if (last === "x" && /^-?(\d+|\d*\.\d+)([eE][-+]?\d+)?$/.test(value)) { if (width !== null || density !== null || height !== null) error = true; else { density = Number(value); if (density < 0) error = true; } }
        else if (last === "h" && /^\d+$/.test(value)) { if (density !== null || height !== null) error = true; else height = parseInt(value, 10); }
        else error = true;
      }
      if (height !== null && width === null) error = true;
      if (error) continue;
      candidates.push({ url, w: width, d: density });
    }
  };
  // The length a sizes attribute gives: the first entry whose media condition holds, or 100vw.
  const lengthPx = (text) => {
    text = text.trim();
    if (!/^(calc|min|max|clamp)\(|^[-+]?(\d+|\d*\.\d+)([eE][-+]?\d+)?[a-zA-Z%]*$/.test(text)) return null;
    if (/^[-+]?(\d+|\d*\.\d+)([eE][-+]?\d+)?$/.test(text) && Number(text) !== 0) return null;
    if (/%$/.test(text)) return null;
    if (/^-/.test(text) && !/^-?0/.test(text) && !/^calc|^min|^max|^clamp/.test(text)) return null;
    if (typeof document === "undefined" || !document.documentElement) return null;
    const probe = document.createElement("div");
    probe.style.cssText = "position:absolute;visibility:hidden;margin:0;padding:0;border:0;height:0;width:" + text;
    if (probe.style.width === "") return null;
    document.documentElement.appendChild(probe);
    const w = probe.getBoundingClientRect().width;
    probe.remove();
    return w;
  };
  const splitTop = (text, separator) => {
    const parts = []; let depth = 0, start = 0;
    for (let i = 0; i < text.length; i++) { const c = text[i]; if (c === "(") depth++; else if (c === ")") depth--; else if (c === separator && depth === 0) { parts.push(text.slice(start, i)); start = i + 1; } }
    parts.push(text.slice(start));
    return parts;
  };
  const sourceSize = (sizes) => {
    const fallback = () => (typeof innerWidth === "number" ? innerWidth : 800);
    if (sizes === null || sizes === undefined) return fallback();
    const entries = splitTop(sizes, ",").map((e) => e.trim()).filter((e) => e !== "");
    for (let i = 0; i < entries.length; i++) {
      const entry = entries[i];
      // The length is the last token (or function) of the entry; what is before is the media condition.
      const m = /^(.*?)\s*((?:calc|min|max|clamp)\([^]*\)|[^\s()]+)$/.exec(entry);
      if (!m) continue;
      const width = lengthPx(m[2]);
      if (width === null) continue;
      const condition = m[1].trim();
      if (condition === "") { return width; }
      if (typeof matchMedia === "function" && matchMedia(condition.startsWith("(") || /^(not|only)\b/i.test(condition) ? condition : "(" + condition + ")").matches) return width;
    }
    return fallback();
  };
  const SUPPORTED_TYPES = new Set(["image/png", "image/jpeg", "image/gif", "image/webp", "image/avif", "image/svg+xml", "image/bmp", "image/x-icon", "image/vnd.microsoft.icon", "image/apng", "image/pjpeg"]);
  const pick = (candidates, sizes) => {
    if (candidates.length === 0) return null;
    const size = sourceSize(sizes);
    const list = candidates.map((c) => ({ url: c.url, d: c.w !== null ? c.w / size : c.d === null ? 1 : c.d }));
    const unique = [];
    for (const c of list) if (!unique.some((u) => u.d === c.d)) unique.push(c);
    const dpr = typeof devicePixelRatio === "number" ? devicePixelRatio : 1;
    for (const c of unique) if (c.d >= dpr) return c.url;
    return unique[unique.length - 1].url;
  };
  const selectedUrl = (el) => {
    const base = el.baseURI;
    const resolve = (u) => { try { return new URL(u, base).href; } catch (e) { return ""; } };
    const choose = (srcsetText, sizesText, src) => {
      const candidates = parseSrcset(srcsetText || "");
      if (src) candidates.push({ url: src, w: null, d: null });
      const picked = pick(candidates, sizesText);
      return picked === null ? "" : resolve(picked);
    };
    const parent = el.parentNode;
    if (parent && parent.localName === "picture") {
      for (const child of parent.children) {
        if (child === el) break;
        if (child.localName !== "source") continue;
        const srcset = child.getAttribute("srcset");
        if (srcset === null) continue;
        const type = child.getAttribute("type");
        if (type !== null && type.trim() !== "" && !SUPPORTED_TYPES.has(type.trim().toLowerCase().split(";")[0].trim())) continue;
        const media = child.getAttribute("media");
        if (media !== null && media.trim() !== "" && typeof matchMedia === "function" && !matchMedia(media).matches) continue;
        const candidates = parseSrcset(srcset);
        if (candidates.length === 0) continue;
        const picked = pick(candidates, child.getAttribute("sizes"));
        if (picked) return resolve(picked);
      }
    }
    const src = el.getAttribute("src");
    return choose(el.getAttribute("srcset"), el.getAttribute("sizes"), src ? src : "");
  };

  // The picture of the element is fetched (if the source is a new one).
  const load = (el) => {
    const s = stateOf(el);
    const url = selectedUrl(el);
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
  define(proto, "currentSrc", { get() { return load(this).url; } });
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

  // The images of the markup are loaded as the page is made, and the window's load event waits for them.
  if (typeof MutationObserver === "function" && typeof document !== "undefined") {
    const start = (el) => {
      if (!(el.hasAttribute("src") || el.hasAttribute("srcset") || (el.parentNode && el.parentNode.localName === "picture"))) return;
      const s = stateOf(el);
      if (s.started) return;
      const holding = typeof delayLoad === "function" && document.readyState !== "complete" && delayLoad();
      load(el);
      if (holding) { if (s.complete) resumeLoad(); else s.release = resumeLoad; }
    };
    const scan = (root) => { if (root.nodeType === 1) for (const el of root.localName === "img" ? [root] : root.querySelectorAll("img")) start(el); };
    new MutationObserver((records) => { for (const r of records) for (const n of r.addedNodes) scan(n); }).observe(document, { childList: true, subtree: true });
    document.addEventListener("DOMContentLoaded", () => scan(document.documentElement), { once: true });
  }
})();
