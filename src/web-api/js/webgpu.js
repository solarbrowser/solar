// WebGPU (https://www.w3.org/TR/webgpu/): the entry point only. navigator.gpu is there in a secure context, as in a browser, and says what a browser says when no adapter can
// be had (requestAdapter() gives null), so that a page that looks for the API finds it and then finds that there is nothing to draw with; the adapters, devices and pipelines
// are what is left for a real implementation, over the graphics of the program (src/paint).
(() => {
  "use strict";
  if (typeof navigator !== "object" || !navigator || "gpu" in navigator || typeof globalThis.isSecureContext !== "undefined" && !globalThis.isSecureContext) return;
  const illegal = () => { throw new TypeError("Illegal constructor"); };
  const token = Symbol("gpu");
  class WGSLLanguageFeatures extends Set {
    constructor(t) { if (t !== token) illegal(); super(); }
  }
  Object.defineProperty(WGSLLanguageFeatures.prototype, Symbol.toStringTag, { value: "WGSLLanguageFeatures", configurable: true });
  class GPU {
    #features;
    constructor(t) { if (t !== token) illegal(); this.#features = new WGSLLanguageFeatures(token); }
    get wgslLanguageFeatures() { return this.#features; }
    async requestAdapter() { return null; }
    getPreferredCanvasFormat() { return "bgra8unorm"; }
  }
  Object.defineProperty(GPU.prototype, Symbol.toStringTag, { value: "GPU", configurable: true });
  Object.defineProperty(globalThis, "GPU", { value: GPU, writable: true, configurable: true, enumerable: false });
  Object.defineProperty(globalThis, "WGSLLanguageFeatures", { value: WGSLLanguageFeatures, writable: true, configurable: true, enumerable: false });
  const gpu = new GPU(token);
  const proto = Object.getPrototypeOf(navigator);
  Object.defineProperty(proto && proto !== Object.prototype ? proto : navigator, "gpu", { get() { return gpu; }, enumerable: true, configurable: true });
})();
