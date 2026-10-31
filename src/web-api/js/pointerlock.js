// The Pointer Lock API (https://w3c.github.io/pointerlock/): requestPointerLock hides the pointer and gives the page how far it moves (movementX, movementY of the mouse events,
// which go to the locked element); exitPointerLock, Escape, or the element leaving the document give it back. The window does the locking (src/gui/Main.cpp); here is the interface.
(() => {
  "use strict";
  const lockNative = globalThis.__solarPointerLock;
  delete globalThis.__solarPointerLock;
  if (typeof Element !== "function" || typeof document === "undefined") return;
  let locked = null, observer = null;
  const define = (target, name, descriptor) => Object.defineProperty(target, name, { configurable: true, enumerable: true, ...descriptor });
  const fire = (type) => document.dispatchEvent(new Event(type));
  // A request needs a recent action of the user (the program keeps the time of the last under this symbol).
  const ACTIVATION = Symbol.for("solar.activation");
  const activated = () => { const at = document[ACTIVATION]; return typeof at === "number" && performance.now() - at < 5000; };
  const release = () => {
    if (!locked) return;
    locked = null;
    if (observer) { observer.disconnect(); observer = null; }
    if (typeof lockNative === "function") lockNative(false);
    setTimeout(() => fire("pointerlockchange"), 0);
  };
  const fail = (name, message) => { setTimeout(() => fire("pointerlockerror"), 0); return new DOMException(message, name); };

  Object.defineProperty(Element.prototype, "requestPointerLock", {
    value: function requestPointerLock(options) {
      if (!this.isConnected) return Promise.reject(fail("WrongDocumentError", "The element is not in the document."));
      if (locked === this) return Promise.resolve();
      if (!activated()) return Promise.reject(fail("NotAllowedError", "Pointer lock needs a user gesture."));
      const element = this;
      const changed = locked !== null;
      locked = element;
      if (typeof lockNative === "function") lockNative(true);
      if (!observer && typeof MutationObserver === "function") {
        // The element that leaves the document loses the lock.
        observer = new MutationObserver(() => { if (locked && !locked.isConnected) release(); });
        observer.observe(document, { childList: true, subtree: true });
      }
      return new Promise((resolve) => setTimeout(() => { fire("pointerlockchange"); resolve(); }, 0));
    },
    writable: true, configurable: true, enumerable: false,
  });
  Object.defineProperty(Document.prototype, "exitPointerLock", { value: function exitPointerLock() { release(); }, writable: true, configurable: true, enumerable: false });
  define(Document.prototype, "pointerLockElement", { get() { return locked && locked.isConnected ? locked : null; } });
  for (const name of ["onpointerlockchange", "onpointerlockerror"]) {
    const type = name.slice(2), slot = Symbol(name);
    if (Object.getOwnPropertyDescriptor(Document.prototype, name)) continue;
    define(Document.prototype, name, {
      get() { return this[slot] || null; },
      set(f) { if (this[slot]) this.removeEventListener(type, this[slot]); this[slot] = typeof f === "function" ? f : null; if (this[slot]) this.addEventListener(type, this[slot]); },
    });
  }
  // Escape gives the pointer back before the page sees the key.
  document.addEventListener("keydown", (e) => { if (locked && e.key === "Escape") release(); }, true);
})();
