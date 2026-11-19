// The navigator the way the engines that follow the standard have it: no keys of its own, what it tells is on Navigator.prototype as accessors (with Solar's own values: nothing here says it is
// another browser), and userAgentData and getBattery are there. It runs after the other scripts of the interface, which add to the navigator as they go.
(() => {
  "use strict";
  if (typeof navigator === "undefined" || typeof Navigator !== "function") return;
  const proto = Navigator.prototype;
  const nav = navigator;
  const major = (/Solar\/(\d+)/.exec(nav.userAgent) || [0, "1"])[1];
  const values = Object.create(null);
  const accessor = (name, value) => {
    values[name] = value;
    const getter = { get [name]() { return values[name]; } };
    const descriptor = Object.getOwnPropertyDescriptor(getter, name);
    Object.defineProperty(descriptor.get, "name", { value: "get " + name, configurable: true });
    Object.defineProperty(proto, name, { get: descriptor.get, set: undefined, enumerable: true, configurable: true });
  };
  for (const name of Object.getOwnPropertyNames(nav)) {
    const d = Object.getOwnPropertyDescriptor(nav, name);
    if (!d || !d.configurable) continue;
    if (d.get || d.set) {
      Object.defineProperty(proto, name, { get: d.get, set: d.set, enumerable: true, configurable: true });
    } else if (typeof d.value === "function") {
      Object.defineProperty(proto, name, { value: d.value, writable: true, enumerable: true, configurable: true });
    } else {
      accessor(name, d.value);
    }
    delete nav[name];
  }
  if (!("vendor" in values)) accessor("vendor", "");
  const brands = [{ brand: "Solar", version: major }, { brand: "Not_A Brand", version: "24" }];
  class NavigatorUAData {
    get brands() { return brands.map((b) => ({ ...b })); }
    get mobile() { return false; }
    get platform() { return "Linux"; }
    toJSON() { return { brands: this.brands, mobile: false, platform: "Linux" }; }
    getHighEntropyValues(hints) {
      if (!Array.isArray(hints)) return Promise.reject(new TypeError("Failed to execute 'getHighEntropyValues' on 'NavigatorUAData': The provided value cannot be converted to a sequence."));
      const all = { brands: this.brands, mobile: false, platform: "Linux", architecture: "x86", bitness: "64", model: "", platformVersion: "6.0.0", uaFullVersion: major + ".0.0.0", wow64: false,
        fullVersionList: brands.map((b) => ({ brand: b.brand, version: b.version + ".0.0.0" })), formFactors: ["Desktop"] };
      const out = { brands: all.brands, mobile: false, platform: "Linux" };
      for (const hint of hints) if (hint in all) out[hint] = all[hint];
      return Promise.resolve(out);
    }
  }
  Object.defineProperty(NavigatorUAData.prototype, Symbol.toStringTag, { value: "NavigatorUAData", configurable: true });
  Object.defineProperty(globalThis, "NavigatorUAData", { value: NavigatorUAData, writable: true, enumerable: false, configurable: true });
  const uaData = new NavigatorUAData();
  Object.defineProperty(proto, "userAgentData", { get: Object.getOwnPropertyDescriptor({ get userAgentData() { return uaData; } }, "userAgentData").get, enumerable: true, configurable: true });
  if (!("getBattery" in proto)) {
    Object.defineProperty(proto, "getBattery", { value: function getBattery() {
      return Promise.resolve(Object.assign(new EventTarget(), { charging: true, chargingTime: 0, dischargingTime: Infinity, level: 1, onchargingchange: null, onchargingtimechange: null, ondischargingtimechange: null, onlevelchange: null }));
    }, writable: true, enumerable: true, configurable: true });
  }
})();
