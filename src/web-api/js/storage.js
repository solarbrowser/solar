// What a page asks of the browser about itself: navigator.storage (StorageManager), navigator.permissions, navigator.clipboard and scheduler.
// (Nothing here shows a prompt: a permission is what the program's policy says, which is "prompt" for what needs a user and "granted" for what does not.)
(() => {
  "use strict";
  const owner = typeof navigator === "object" && navigator ? navigator : null;
  if (!owner) return;
  const proto = typeof Navigator === "function" ? Navigator.prototype : typeof WorkerNavigator === "function" ? WorkerNavigator.prototype : Object.getPrototypeOf(owner);
  const hide = (name, value) => Object.defineProperty(globalThis, name, { value, writable: true, configurable: true, enumerable: false });
  const attach = (name, value) => { if (!(name in owner)) Object.defineProperty(proto, name, { get() { return value; }, enumerable: true, configurable: true }); };
  const illegal = (name) => class { constructor() { throw new TypeError("Illegal constructor"); } };

  // ---- StorageManager ----
  class StorageManager {
    constructor() { throw new TypeError("Illegal constructor"); }
    estimate() { return Promise.resolve({ quota: 10 * 1024 * 1024 * 1024, usage: 0, usageDetails: {} }); }
    persist() { return Promise.resolve(false); }
    persisted() { return Promise.resolve(false); }
    getDirectory() { return Promise.reject(new DOMException("The origin private file system is not available.", "NotSupportedError")); }
  }
  Object.defineProperty(StorageManager.prototype, Symbol.toStringTag, { value: "StorageManager", configurable: true });
  hide("StorageManager", StorageManager);
  attach("storage", Object.create(StorageManager.prototype));

  // ---- Permissions ----
  const names = {
    "geolocation": "prompt", "notifications": "prompt", "camera": "prompt", "microphone": "prompt",
    "clipboard-read": "prompt", "clipboard-write": "granted", "persistent-storage": "prompt", "push": "prompt", "midi": "prompt", "background-sync": "granted",
    "screen-wake-lock": "granted", "accelerometer": "granted", "gyroscope": "granted", "magnetometer": "granted", "storage-access": "prompt", "payment-handler": "granted",
  };
  class PermissionStatus extends EventTarget {
    #name; #state;
    constructor(token, name, state) { super(); if (token !== PermissionStatus) throw new TypeError("Illegal constructor"); this.#name = name; this.#state = state; }
    get name() { return this.#name; }
    get state() { return this.#state; }
    onchange = null;
  }
  Object.defineProperty(PermissionStatus.prototype, Symbol.toStringTag, { value: "PermissionStatus", configurable: true });
  class Permissions {
    constructor() { throw new TypeError("Illegal constructor"); }
    query(descriptor) {
      return new Promise((resolve) => {
        if (arguments.length === 0 || descriptor === null || typeof descriptor !== "object") throw new TypeError("Failed to execute 'query' on 'Permissions': The provided value is not of type 'PermissionDescriptor'.");
        const name = String(descriptor.name);
        if (!(name in names)) throw new TypeError(`Failed to execute 'query' on 'Permissions': Failed to read the 'name' property from 'PermissionDescriptor': The provided value '${name}' is not a valid enum value of type PermissionName.`);
        let state = names[name];
        if (globalThis.__solarPermissions && globalThis.__solarPermissions.get(name) !== "prompt") state = globalThis.__solarPermissions.get(name);
        else if (name === "notifications" || name === "push") state = "prompt";
        resolve(new PermissionStatus(PermissionStatus, name, state));
      });
    }
  }
  Object.defineProperty(Permissions.prototype, Symbol.toStringTag, { value: "Permissions", configurable: true });
  hide("Permissions", Permissions);
  hide("PermissionStatus", PermissionStatus);
  attach("permissions", Object.create(Permissions.prototype));

  // ---- Clipboard (the text is the program's own: there is no system clipboard to reach from here) ----
  if (typeof Clipboard === "undefined") {
    let text = "";
    class Clipboard extends EventTarget {
      constructor() { super(); throw new TypeError("Illegal constructor"); }
      writeText(data) { text = String(data); return Promise.resolve(); }
      readText() { return Promise.resolve(text); }
      write(items) {
        return Promise.resolve(Array.from(items || [])).then(async (list) => {
          for (const item of list) if (item && item.types && item.types.includes("text/plain")) text = await (await item.getType("text/plain")).text();
        });
      }
      read() { return Promise.resolve([]); }
    }
    Object.defineProperty(Clipboard.prototype, Symbol.toStringTag, { value: "Clipboard", configurable: true });
    hide("Clipboard", Clipboard);
    attach("clipboard", Object.create(Clipboard.prototype));
  }

  // ---- scheduler (Prioritized Task Scheduling) ----
  if (typeof scheduler === "undefined") {
    const delays = { "user-blocking": 0, "user-visible": 0, "background": 20 };
    class TaskPriorityChangeEvent extends Event {
      constructor(type, init) { super(type, init); this.previousPriority = init && init.previousPriority; }
    }
    class TaskController extends AbortController {
      #priority;
      constructor(init = {}) {
        super();
        this.#priority = init.priority === undefined ? "user-visible" : String(init.priority);
        if (!(this.#priority in delays)) throw new TypeError(`The provided value '${this.#priority}' is not a valid enum value of type TaskPriority.`);
        Object.defineProperty(this.signal, "priority", { get: () => this.#priority, configurable: true });
      }
      setPriority(priority) {
        priority = String(priority);
        if (!(priority in delays)) throw new TypeError(`The provided value '${priority}' is not a valid enum value of type TaskPriority.`);
        const previous = this.#priority;
        this.#priority = priority;
        this.signal.dispatchEvent(new TaskPriorityChangeEvent("prioritychange", { previousPriority: previous }));
      }
    }
    const post = (callback, options = {}) => new Promise((resolve, reject) => {
      if (typeof callback !== "function") throw new TypeError("Failed to execute 'postTask' on 'Scheduler': The callback provided as parameter 1 is not a function.");
      const signal = options.signal;
      if (signal && signal.aborted) { reject(signal.reason); return; }
      const priority = options.priority !== undefined ? String(options.priority) : signal && signal.priority ? signal.priority : "user-visible";
      const timer = setTimeout(() => {
        if (signal) signal.removeEventListener("abort", abort);
        try { resolve(callback()); } catch (e) { reject(e); }
      }, Math.max(options.delay | 0, delays[priority] || 0));
      const abort = () => { clearTimeout(timer); reject(signal.reason); };
      if (signal) signal.addEventListener("abort", abort, { once: true });
    });
    class Scheduler {
      constructor() { throw new TypeError("Illegal constructor"); }
      postTask(callback, options) { return post(callback, options); }
      yield() { return new Promise((resolve) => setTimeout(resolve, 0)); }
    }
    Object.defineProperty(Scheduler.prototype, Symbol.toStringTag, { value: "Scheduler", configurable: true });
    hide("Scheduler", Scheduler);
    hide("TaskController", TaskController);
    hide("TaskPriorityChangeEvent", TaskPriorityChangeEvent);
    Object.defineProperty(globalThis, "scheduler", { value: Object.create(Scheduler.prototype), writable: true, configurable: true, enumerable: true });
  }
  void illegal;
})();
