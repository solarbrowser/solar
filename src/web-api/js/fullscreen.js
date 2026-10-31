// The Fullscreen API (https://fullscreen.spec.whatwg.org/): an element is made to fill the window, and the window is made full screen. The element gets an attribute
// (data-solar-fullscreen) that :fullscreen matches and that the user agent style sheet (src/css/Style.cpp) lays it out by (fixed, the whole window, over everything); it is
// the nearest that is done to a top layer. Escape leaves it.
(() => {
  "use strict";
  const windowFullscreen = globalThis.__solarWindowFullscreen;
  delete globalThis.__solarWindowFullscreen;
  if (typeof Element !== "function" || typeof document === "undefined") return;
  const ATTRIBUTE = "data-solar-fullscreen";
  let current = null;
  const define = (target, name, descriptor) => Object.defineProperty(target, name, { configurable: true, enumerable: true, ...descriptor });
  const fire = (target, names) => { for (const name of names) target.dispatchEvent(new Event(name, { bubbles: true })); };
  const tell = (element) => {
    fire(element, ["fullscreenchange", "webkitfullscreenchange"]);
    if (document !== element.ownerDocument) return;
    fire(document, ["fullscreenchange", "webkitfullscreenchange"]);
  };

  const enter = (element) => {
    if (current === element) return Promise.resolve();
    const previous = current;
    if (previous) previous.removeAttribute(ATTRIBUTE);
    current = element;
    element.setAttribute(ATTRIBUTE, "");
    if (typeof windowFullscreen === "function") windowFullscreen(true);
    return new Promise((resolve) => setTimeout(() => { tell(element); resolve(); }, 0));
  };
  const leave = () => {
    const element = current;
    if (!element) return Promise.reject(new TypeError("Document not active"));
    current = null;
    element.removeAttribute(ATTRIBUTE);
    if (typeof windowFullscreen === "function") windowFullscreen(false);
    return new Promise((resolve) => setTimeout(() => { tell(element); resolve(); }, 0));
  };

  for (const name of ["requestFullscreen", "webkitRequestFullscreen", "webkitRequestFullScreen", "mozRequestFullScreen", "msRequestFullscreen"]) {
    Object.defineProperty(Element.prototype, name, {
      value: function requestFullscreen() {
        if (!this.isConnected) return Promise.reject(new TypeError("Element is not in the document."));
        return enter(this);
      },
      writable: true, configurable: true, enumerable: false,
    });
  }
  const documentProto = Document.prototype;
  for (const name of ["fullscreenElement", "webkitFullscreenElement", "webkitCurrentFullScreenElement", "mozFullScreenElement"]) define(documentProto, name, { get() { return current && current.isConnected ? current : null; } });
  for (const name of ["fullscreenEnabled", "webkitFullscreenEnabled", "mozFullScreenEnabled"]) define(documentProto, name, { get() { return typeof windowFullscreen === "function"; } });
  define(documentProto, "webkitIsFullScreen", { get() { return current !== null; } });
  define(documentProto, "fullScreen", { get() { return current !== null; } });
  for (const name of ["exitFullscreen", "webkitExitFullscreen", "webkitCancelFullScreen", "mozCancelFullScreen"]) {
    Object.defineProperty(documentProto, name, { value: function exitFullscreen() { return leave(); }, writable: true, configurable: true, enumerable: false });
  }
  for (const name of ["onfullscreenchange", "onfullscreenerror"]) {
    const type = name.slice(2);
    if (!Object.getOwnPropertyDescriptor(documentProto, name)) {
      const slot = Symbol(name);
      define(documentProto, name, {
        get() { return this[slot] || null; },
        set(f) { if (this[slot]) this.removeEventListener(type, this[slot]); this[slot] = typeof f === "function" ? f : null; if (this[slot]) this.addEventListener(type, this[slot]); },
      });
    }
  }
  // Escape leaves it (before the page sees the key, as a browser's does).
  document.addEventListener("keydown", (e) => { if (current && e.key === "Escape") { leave(); } }, true);
})();
