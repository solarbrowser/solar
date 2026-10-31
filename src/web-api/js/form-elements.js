// What the elements of forms have that is not in the markup's reflection: meter (value, min, max, low, high, optimum clamped as the standard says), navigator.userActivation, and
// input.showPicker() with the checks the standard makes before it shows one.
(() => {
  "use strict";
  if (typeof HTMLElement !== "function" || typeof document === "undefined") return;
  const define = (target, name, descriptor) => Object.defineProperty(target, name, { configurable: true, enumerable: true, ...descriptor });

  // ---- User activation ----
  const ACTIVATION = Symbol.for("solar.activation");
  let everActive = false;
  let consumed = -1;
  const activeAt = () => { const at = document[ACTIVATION]; return typeof at === "number" && at !== consumed ? at : null; };
  const isActive = () => { const at = activeAt(); if (at === null) return false; everActive = true; return performance.now() - at < 5000; };
  class UserActivation {
    constructor() { throw new TypeError("Illegal constructor"); }
    get hasBeenActive() { return everActive || typeof document[ACTIVATION] === "number"; }
    get isActive() { return isActive(); }
  }
  for (const name of ["hasBeenActive", "isActive"]) Object.defineProperty(UserActivation.prototype, name, { enumerable: true });
  Object.defineProperty(UserActivation.prototype, Symbol.toStringTag, { value: "UserActivation", configurable: true });
  Object.defineProperty(globalThis, "UserActivation", { value: UserActivation, writable: true, configurable: true, enumerable: false });
  if (typeof Navigator === "function" && !("userActivation" in Navigator.prototype)) {
    const instance = Object.create(UserActivation.prototype);
    define(Navigator.prototype, "userActivation", { get() { return instance; } });
  }
  const consume = () => { const at = document[ACTIVATION]; if (typeof at === "number") consumed = at; };
  Object.defineProperty(globalThis, Symbol.for("solar.consumeActivation"), { value: { isActive, consume }, configurable: true });

  // ---- showPicker ----
  if (typeof HTMLInputElement === "function") {
    Object.defineProperty(HTMLInputElement.prototype, "showPicker", { value: function showPicker() {
      const type = this.type;
      if (this.disabled || this.matches(":disabled")) throw new DOMException("The input element is disabled.", "InvalidStateError");
      const readonlyApplies = !["button", "checkbox", "color", "file", "hidden", "image", "radio", "range", "reset", "submit"].includes(type);
      if (readonlyApplies && this.hasAttribute("readonly")) throw new DOMException("The input element is read-only.", "InvalidStateError");
      if (globalThis.top !== globalThis) { try { void globalThis.top.document; } catch (e) { throw new DOMException("showPicker() is not allowed in a cross-origin frame.", "SecurityError"); } }
      if (!isActive()) throw new DOMException("showPicker() requires a user gesture.", "NotAllowedError");
      consume();
    }, writable: true, configurable: true, enumerable: true });
  }

  // ---- meter ----
  if (typeof HTMLMeterElement === "function") {
    const P = HTMLMeterElement.prototype;
    // The rules for parsing floating-point number values: leading white space, a sign, digits, a fraction, an exponent.
    const parse = (s) => { if (s === null) return null; const m = /^[ \t\n\f\r]*([+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?)/.exec(s); if (!m) return null; const n = Number(m[1]); return Number.isFinite(n) ? n : null; };
    const num = (el, name, fallback) => { const n = parse(el.getAttribute(name)); return n === null ? fallback : n; };
    const bounds = (el) => { const min = num(el, "min", 0); let max = num(el, "max", 1); if (max < min) max = min; return [min, max]; };
    const clamp = (n, lo, hi) => Math.min(Math.max(n, lo), hi);
    const setNumber = (el, name, v) => { v = Number(v); if (!Number.isFinite(v)) throw new TypeError("Failed to set the '" + name + "' property on 'HTMLMeterElement': The provided double value is non-finite."); el.setAttribute(name, String(v)); };
    const props = {
      min: [(el) => bounds(el)[0]],
      max: [(el) => bounds(el)[1]],
      value: [(el) => { const [lo, hi] = bounds(el); return clamp(num(el, "value", 0), lo, hi); }],
      low: [(el) => { const [lo, hi] = bounds(el); return clamp(num(el, "low", lo), lo, hi); }],
      high: [(el) => { const [lo, hi] = bounds(el); const low = clamp(num(el, "low", lo), lo, hi); return clamp(num(el, "high", hi), low, hi); }],
      optimum: [(el) => { const [lo, hi] = bounds(el); return clamp(num(el, "optimum", (lo + hi) / 2), lo, hi); }],
    };
    for (const [name, [get]] of Object.entries(props)) define(P, name, { get() { return get(this); }, set(v) { setNumber(this, name, v); } });
  }
})();
