// What the user has allowed an origin to do: kept as one text for the origin ({"notifications":"granted", ...}), read when asked, so that every page and worker of the
// origin sees one answer. There is no prompt in the program yet: what was never decided is "prompt" (and Notification.permission "default"), and asking does not change it;
// whatever will ask the user writes the decision with __solarPermissions.set. The Permissions API (storage.js), Notification.permission and push use this.
(() => {
  "use strict";
  const read = globalThis.__solarKvRead, write = globalThis.__solarKvWrite;
  const KEY = "permissions";
  const origin = () => (typeof location === "object" && location ? location.origin : "null");
  const all = () => {
    if (typeof read !== "function") return {};
    try { return JSON.parse(read(origin(), KEY) || "{}"); } catch (e) { return {}; }
  };
  const api = {
    // "granted", "denied" or "prompt".
    get(name) { const state = all()[name]; return state === "granted" || state === "denied" ? state : "prompt"; },
    set(name, state) {
      if (typeof write !== "function") return false;
      const current = all();
      current[name] = state;
      return write(origin(), KEY, JSON.stringify(current));
    },
  };
  Object.defineProperty(globalThis, "__solarPermissions", { value: api, writable: true, configurable: true, enumerable: false });

  if (typeof Notification === "function") {
    const legacy = (state) => (state === "prompt" ? "default" : state);
    Object.defineProperty(Notification, "permission", { get() { return legacy(api.get("notifications")); }, configurable: true, enumerable: true });
    Object.defineProperty(Notification, "requestPermission", {
      value: function requestPermission(callback) {
        const state = legacy(api.get("notifications"));
        if (typeof callback === "function") callback(state);
        return Promise.resolve(state);
      },
      writable: true, configurable: true, enumerable: true,
    });
  }
})();
