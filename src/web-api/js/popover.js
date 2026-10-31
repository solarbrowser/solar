// The Popover API (https://html.spec.whatwg.org/multipage/popover.html): the popover attribute, showPopover, hidePopover and togglePopover, beforetoggle and toggle (ToggleEvent), the
// stacks of auto and hint popovers with light dismiss (a press outside, Escape), popovertarget and popovertargetaction of buttons, :popover-open.
// What the rendering needs is attributes of ours (src/css/Style.cpp): data-solar-popover-open (what :popover-open matches) and data-solar-layer (the place in the top layer, which
// the user agent style sheet turns into a z-index).
(() => {
  "use strict";
  if (typeof Element !== "function" || typeof document === "undefined" || typeof HTMLElement !== "function") return;
  const define = (target, name, descriptor) => Object.defineProperty(target, name, { configurable: true, enumerable: true, ...descriptor });
  const method = (target, name, fn) => Object.defineProperty(target, name, { value: fn, writable: true, configurable: true, enumerable: true });
  const OPEN = "data-solar-popover-open", LAYER = "data-solar-layer";

  // ---- ToggleEvent ----
  class ToggleEvent extends Event {
    #oldState; #newState; #source;
    constructor(type, init = {}) {
      if (arguments.length < 1) throw new TypeError("Failed to construct 'ToggleEvent': 1 argument required, but only 0 present.");
      super(type, init);
      init = init || {};
      this.#oldState = init.oldState === undefined ? "" : String(init.oldState);
      this.#newState = init.newState === undefined ? "" : String(init.newState);
      const source = init.source === undefined ? null : init.source;
      if (source !== null && !(source instanceof Element)) throw new TypeError("Failed to construct 'ToggleEvent': Failed to read the 'source' property from 'ToggleEventInit': Failed to convert value to 'Element'.");
      this.#source = source;
    }
    get oldState() { return this.#oldState; }
    get newState() { return this.#newState; }
    get source() { return this.#source; }
  }
  Object.defineProperty(ToggleEvent, "length", { value: 1, configurable: true });
  for (const name of ["oldState", "newState", "source"]) Object.defineProperty(ToggleEvent.prototype, name, { enumerable: true });
  Object.defineProperty(ToggleEvent.prototype, Symbol.toStringTag, { value: "ToggleEvent", configurable: true });
  Object.defineProperty(globalThis, "ToggleEvent", { value: ToggleEvent, writable: true, configurable: true, enumerable: false });

  // ---- State ----
  const states = new WeakMap();  // element -> { showing, invoker, previousFocus, task, nested }
  const stateOf = (el) => { let s = states.get(el); if (!s) { s = { showing: false, invoker: null, previousFocus: null, task: null, hiding: false }; states.set(el, s); } return s; };
  const autoStack = [], hintStack = [], topLayer = [];  // elements; the top layer holds the popovers and (src/web-api/js/dialog.js) the modal dialogs, in the order they were added
  const popoverType = (el) => {
    const v = el.getAttribute("popover");
    if (v === null) return "none";
    const l = v.toLowerCase();
    if (l === "" || l === "auto") return "auto";
    if (l === "hint") return "hint";
    return "manual";
  };
  const isShowing = (el) => { const s = states.get(el); return !!(s && s.showing); };
  const setLayers = () => { topLayer.forEach((el, i) => el.setAttribute(LAYER, String(Math.min(i + 1, 60)))); };
  const addToTopLayer = (el) => { if (!topLayer.includes(el)) topLayer.push(el); setLayers(); };
  const removeFromTopLayer = (el) => { const i = topLayer.indexOf(el); if (i >= 0) topLayer.splice(i, 1); try { el.removeAttribute(LAYER); } catch (e) {} setLayers(); };
  Object.defineProperty(globalThis, Symbol.for("solar.toplayer"), { value: { add: addToTopLayer, remove: removeFromTopLayer, has: (el) => topLayer.includes(el) }, configurable: true });

  // The parent in the flat tree: the slot an element is assigned to, or the host of a shadow root.
  const flatParent = (n) => {
    if (n.assignedSlot) return n.assignedSlot;
    const p = n.parentNode;
    if (!p) return null;
    return typeof ShadowRoot === "function" && p instanceof ShadowRoot ? p.host : p;
  };
  const flatContains = (ancestor, node) => { for (let n = node; n; n = flatParent(n)) if (n === ancestor) return true; return false; };
  const inStack = (el) => autoStack.includes(el) || hintStack.includes(el);
  const stackOf = (el) => (popoverType(el) === "hint" ? hintStack : autoStack);
  // The nearest inclusive ancestor of the node that is an open popover (an auto or hint one that is showing).
  const nearestOpenPopover = (node) => { for (let n = node; n; n = flatParent(n)) if (n instanceof Element && isShowing(n) && inStack(n)) return n; return null; };
  const position = (el) => { const a = autoStack.indexOf(el); return a >= 0 ? a : autoStack.length + hintStack.indexOf(el); };

  // The topmost popover that is an ancestor of the node: through the tree and through the invoker of an open popover (a button that shows a popover is part of what it shows).
  const topmostAncestor = (node, invoker, self) => {
    let topmost = null;
    const check = (candidate) => {
      let found = null;
      for (let n = candidate; n && !found; n = flatParent(n)) {
        if (!(n instanceof Element) || n === self) continue;
        if (isShowing(n) && inStack(n)) found = n;
        else {
          // an invoker of an open popover counts as being inside it
          const target = invokerTarget(n);
          if (target && target !== self && isShowing(target) && inStack(target) && stateOf(target).invoker === n) found = target;
        }
      }
      if (found && (!topmost || position(found) > position(topmost))) topmost = found;
    };
    if (node) check(node);
    if (invoker) check(invoker);
    return topmost;
  };

  // ---- Showing and hiding ----
  const fail = (name, message) => new DOMException(message, name);
  const nodeConnected = Reflect.getOwnPropertyDescriptor(Node.prototype, "isConnected").get;
  const connected = (el) => Reflect.apply(nodeConnected, el, []);
  const checkValidity = (el, expectedShowing, throwExceptions) => {
    if (popoverType(el) === "none") { if (throwExceptions) throw fail("NotSupportedError", "Not supported: the element is not a popover."); return false; }
    if (isShowing(el) !== expectedShowing) return false;
    if (!connected(el)) { if (throwExceptions) throw fail("InvalidStateError", "Invalid state: the element is not connected."); return false; }
    if (el instanceof HTMLDialogElement && (el.hasAttribute("open") || el[Symbol.for("solar.modal")])) { if (throwExceptions) throw fail("InvalidStateError", "Invalid state: the dialog is open."); return false; }
    if (document.fullscreenElement === el) { if (throwExceptions) throw fail("InvalidStateError", "Invalid state: the element is fullscreen."); return false; }
    return true;
  };
  const queueToggle = (el, oldState, newState) => {
    const s = stateOf(el);
    if (s.task) { oldState = s.task.oldState; clearTimeout(s.task.id); }
    const task = { oldState, newState, id: 0 };
    task.id = setTimeout(() => { s.task = null; el.dispatchEvent(new ToggleEvent("toggle", { oldState: task.oldState, newState: task.newState })); }, 0);
    s.task = task;
  };
  const fireBeforeToggle = (el, oldState, newState, cancelable, source) => el.dispatchEvent(new ToggleEvent("beforetoggle", { cancelable, oldState, newState, source: source || null }));
  const focusedElement = () => { const a = document.activeElement; return a && a !== document.body && a !== document.documentElement ? a : null; };

  const hideUntil = (endpoint, focusPrevious, fireEvents, stackHint) => {
    // Hint popovers go first; then the auto ones, down to the endpoint (which stays open).
    const closeStack = (stack, stopAt) => {
      let guard = 1000;
      while (stack.length && stack[stack.length - 1] !== stopAt && guard--) {
        const top = stack[stack.length - 1];
        const last = stack.length === 1 || stack[stack.length - 2] === stopAt;
        hidePopover(top, focusPrevious && last, fireEvents, false, null, true);
        if (stack[stack.length - 1] === top) stack.pop();  // (a handler may have left it: it goes anyway)
      }
    };
    if (endpoint && popoverType(endpoint) === "hint" && isShowing(endpoint)) { closeStack(hintStack, endpoint); return; }
    closeStack(hintStack, null);
    closeStack(autoStack, endpoint && autoStack.includes(endpoint) ? endpoint : null);
  };

  const showPopover = (el, throwExceptions, invoker) => {
    if (!checkValidity(el, false, throwExceptions)) return;
    const s = stateOf(el);
    const nestedShow = s.hiding;
    s.hiding = true;
    try {
      const type = popoverType(el);
      if (!fireBeforeToggle(el, "closed", "open", true, invoker)) return;
      if (!checkValidity(el, false, throwExceptions) || popoverType(el) !== type) return;
      let ancestor = null;
      if (type !== "manual") {
        s.invoker = invoker || null;
        ancestor = topmostAncestor(el.parentNode ? flatParent(el) : null, invoker, el);
        // An auto popover hides the ones that are not its ancestors; a hint one only the hints (and an auto one that was shown by it stays).
        if (type === "auto") hideUntil(ancestor, false, !nestedShow);
        else {
          let stop = null;
          if (ancestor && popoverType(ancestor) === "hint") stop = ancestor;
          else if (ancestor === null) stop = null;
          closeHintsUntil(stop);
        }
        if (!checkValidity(el, false, throwExceptions) || popoverType(el) !== type) { s.invoker = null; return; }
      }
      const previous = focusedElement();
      addToTopLayer(el);
      s.showing = true;
      el.setAttribute(OPEN, "");
      if (type === "auto") autoStack.push(el); else if (type === "hint") hintStack.push(el);
      s.invoker = type === "manual" ? (invoker || null) : s.invoker;
      s.previousFocus = null;
      // The popover takes the focus when it, or what is in it, asks for it with autofocus.
      let control = null;
      if (el.hasAttribute("autofocus")) control = el;
      else { for (const d of el.querySelectorAll("[autofocus]")) { if (flatContains(el, d)) { control = d; break; } } }
      if (control && typeof control.focus === "function") {
        if (type !== "manual") s.previousFocus = previous;
        control.focus();
      } else if (type !== "manual") s.previousFocus = previous;
      queueToggle(el, "closed", "open");
    } finally {
      if (!nestedShow) s.hiding = false;
    }
  };
  const closeHintsUntil = (stop) => {
    let guard = 1000;
    while (hintStack.length && hintStack[hintStack.length - 1] !== stop && guard--) {
      const top = hintStack[hintStack.length - 1];
      hidePopover(top, false, true, false, null, true);
      if (hintStack[hintStack.length - 1] === top) hintStack.pop();
    }
  };

  const hidePopover = (el, focusPrevious, fireEvents, throwExceptions, invoker, fromUntil) => {
    if (!checkValidity(el, true, throwExceptions)) { if (!throwExceptions && isShowing(el) && !connected(el)) {} else return; }
    const s = stateOf(el);
    const nested = s.hiding;
    s.hiding = true;
    try {
      const type = popoverType(el);
      if (!fromUntil && type !== "manual") {
        // Hiding a popover hides what was shown on top of it.
        if (type === "hint") closeHintsUntil(el); else hideUntil(el, false, fireEvents);
        if (!checkValidity(el, true, throwExceptions) && connected(el)) return;
      }
      if (fireEvents) {
        fireBeforeToggle(el, "open", "closed", false, null);
        if (!isShowing(el)) return;
        if (popoverType(el) !== type && !fromUntil) { /* the type was changed by a handler: it is hidden all the same */ }
      }
      const previous = s.previousFocus;
      s.previousFocus = null;
      s.showing = false;
      s.invoker = null;
      removeFromTopLayer(el);
      el.removeAttribute(OPEN);
      for (const stack of [autoStack, hintStack]) { const i = stack.indexOf(el); if (i >= 0) stack.splice(i, 1); }
      if (fireEvents) queueToggle(el, "open", "closed");
      if (focusPrevious && previous && connected(previous)) {
        const active = document.activeElement;
        if (!active || active === document.body || flatContains(el, active)) previous.focus();
      }
    } finally {
      if (!nested) s.hiding = false;
    }
  };

  // ---- The interface ----
  define(HTMLElement.prototype, "popover", {
    get() { const t = popoverType(this); return t === "none" ? null : t; },
    set(value) { if (value === null || value === undefined) this.removeAttribute("popover"); else this.setAttribute("popover", String(value)); },
  });
  const optionsSource = (options) => {
    if (options === undefined || options === null) return null;
    if (typeof options !== "object") throw new TypeError("Failed to execute 'showPopover' on 'HTMLElement': The provided value is not of type 'ShowPopoverOptions'.");
    const source = options.source;
    if (source === undefined) return null;
    if (!(source instanceof HTMLElement)) throw new TypeError("Failed to execute 'showPopover' on 'HTMLElement': Failed to read the 'source' property from 'ShowPopoverOptions': Failed to convert value to 'HTMLElement'.");
    return source;
  };
  method(HTMLElement.prototype, "showPopover", function showPopoverMethod(options) { showPopover(this, true, optionsSource(options)); });
  method(HTMLElement.prototype, "hidePopover", function hidePopoverMethod() { hidePopover(this, true, true, true, null); });
  method(HTMLElement.prototype, "togglePopover", function togglePopoverMethod(options) {
    let force;
    let source = null;
    if (options !== undefined && options !== null && typeof options === "object") { if (options.force !== undefined) force = !!options.force; source = optionsSource(options); }
    else if (options !== undefined) force = !!options;
    if (isShowing(this) && (force === undefined || force === false)) hidePopover(this, true, true, true, null);
    else if (force === undefined || force === true) showPopover(this, true, source);
    else checkValidity(this, isShowing(this), true);
    return isShowing(this);
  });
  // The lengths the interface says.
  for (const name of ["showPopover", "togglePopover"]) Object.defineProperty(HTMLElement.prototype[name], "length", { value: 0, configurable: true });

  // ---- A change of the attribute, a removal ----
  const attributeChanged = (el, name, oldValue) => {
    if (name !== "popover" || !isShowing(el)) return;
    const was = (() => { if (oldValue === null) return "none"; const l = oldValue.toLowerCase(); return l === "" || l === "auto" ? "auto" : l === "hint" ? "hint" : "manual"; })();
    const now = popoverType(el);
    if (was !== now) hidePopover(el, false, true, false, null, false);
  };
  for (const name of ["setAttribute", "removeAttribute", "toggleAttribute", "setAttributeNS", "removeAttributeNS"]) {
    const original = Element.prototype[name];
    const wrapper = ({ [name](...args) {
      const attrName = (name.endsWith("NS") ? String(args[1]) : String(args[0])).toLowerCase();
      if (attrName === "popovertarget") explicitTargets.delete(this);
      if (attrName !== "popover") return Reflect.apply(original, this, args);
      const before = this.getAttribute("popover");
      const result = Reflect.apply(original, this, args);
      attributeChanged(this, "popover", before);
      return result;
    } })[name];
    Object.defineProperty(wrapper, "length", { value: original.length, configurable: true });
    Object.defineProperty(Element.prototype, name, { value: wrapper, writable: true, configurable: true, enumerable: true });
  }
  // What leaves the document is hidden without a word.
  const sweep = () => { for (const el of [...topLayer]) if (isShowing(el) && !connected(el)) hidePopover(el, false, false, false, null, true); };
  if (typeof MutationObserver === "function") new MutationObserver(sweep).observe(document, { childList: true, subtree: true });
  const connectedCheck = (el) => { if (isShowing(el) && !connected(el)) hidePopover(el, false, false, false, null, true); };
  for (const [proto, name] of [[Element.prototype, "isConnected"], [Node.prototype, "isConnected"]]) {
    const d = Object.getOwnPropertyDescriptor(proto, name);
    if (d && d.get && !d.get.__solarPopover) {
      const get = d.get;
      const wrapped = ({ get isConnected() { const v = Reflect.apply(get, this, []); if (!v && this instanceof Element && states.has(this)) connectedCheck(this); return v; } });
      const g = Object.getOwnPropertyDescriptor(wrapped, "isConnected").get;
      g.__solarPopover = true;
      Object.defineProperty(proto, name, { get: g, set: d.set, enumerable: d.enumerable, configurable: true });
    }
  }

  // ---- Invokers: popovertarget and popovertargetaction ----
  const explicitTargets = new WeakMap();
  // A button or input that submits or resets its form is not an invoker; one with no form is (whatever its type).
  const isInvokerElement = (el) => {
    if (typeof HTMLButtonElement === "function" && el instanceof HTMLButtonElement) return el.type === "button" || !el.form;
    if (typeof HTMLInputElement === "function" && el instanceof HTMLInputElement && /^(button|reset|submit|image)$/i.test(el.type)) return el.type.toLowerCase() === "button" || !el.form;
    return false;
  };
  function invokerTarget(el) {
    if (!isInvokerElement(el) || !el.hasAttribute("popovertarget")) return null;
    const explicit = explicitTargets.get(el);
    let target;
    if (explicit !== undefined) target = explicit.getRootNode() === el.getRootNode() ? explicit : null;
    else { const root = el.getRootNode(); const id = el.getAttribute("popovertarget"); target = root && typeof root.getElementById === "function" ? root.getElementById(id) : null; }
    return target instanceof HTMLElement && popoverType(target) !== "none" ? target : null;
  }
  for (const C of [HTMLButtonElement, HTMLInputElement]) {
    define(C.prototype, "popoverTargetElement", {
      get() { return invokerTarget(this); },
      set(value) {
        if (value !== null && value !== undefined && !(value instanceof Element)) throw new TypeError("Failed to set the 'popoverTargetElement' property: The provided value is not of type 'Element'.");
        if (value === null || value === undefined) { this.removeAttribute("popovertarget"); return; }
        Reflect.apply(Element.prototype.setAttribute, this, ["popovertarget", ""]);
        explicitTargets.set(this, value);
      },
    });
    define(C.prototype, "popoverTargetAction", {
      get() { const v = (this.getAttribute("popovertargetaction") || "").toLowerCase(); return v === "show" || v === "hide" ? v : "toggle"; },
      set(value) { this.setAttribute("popovertargetaction", String(value)); },
    });
  }
  const invoke = (node) => {
    const target = invokerTarget(node);
    if (!target) return;
    const action = node.popoverTargetAction;
    if (isShowing(target) && (action === "hide" || action === "toggle")) hidePopover(target, true, true, false, node);
    else if (!isShowing(target) && (action === "show" || action === "toggle")) showPopover(target, false, node);
  };
  const disabledNow = (el) => el.matches && el.matches(":disabled");
  document.addEventListener("click", (event) => {
    if (event.defaultPrevented) return;
    for (let n = event.target; n; n = flatParent(n)) {
      if (!(n instanceof Element)) continue;
      if (isInvokerElement(n)) { if (!disabledNow(n)) invoke(n); return; }
    }
  });

  // ---- Light dismiss ----
  let downTarget;  // the popover a press began in (null: none; undefined: no press)
  const target = (event) => (event.composedPath && event.composedPath()[0]) || event.target;
  const lightDismissDown = (event) => {
    if (!autoStack.length && !hintStack.length) { downTarget = undefined; return; }
    const t = target(event);
    downTarget = topmostAncestor(t, null, null);
  };
  const lightDismissUp = (event) => {
    if (downTarget === undefined) return;
    const t = target(event);
    const ancestor = topmostAncestor(t, null, null);
    const same = ancestor === downTarget;
    downTarget = undefined;
    if (same && (autoStack.length || hintStack.length)) hideUntil(ancestor, false, true);
  };
  let sawPointer = false;
  document.addEventListener("pointerdown", (e) => { sawPointer = true; lightDismissDown(e); }, true);
  document.addEventListener("pointerup", (e) => { lightDismissUp(e); }, true);
  document.addEventListener("mousedown", (e) => { if (!sawPointer) lightDismissDown(e); }, true);
  document.addEventListener("mouseup", (e) => { if (!sawPointer) lightDismissUp(e); sawPointer = false; }, true);
  // Escape closes the topmost popover that is open (the hint ones first).
  document.addEventListener("keydown", (e) => {
    if (e.key !== "Escape" || e.defaultPrevented) return;
    const top = hintStack.length ? hintStack[hintStack.length - 1] : autoStack[autoStack.length - 1];
    if (top) hidePopover(top, true, true, false, null);
  });
})();
