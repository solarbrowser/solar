// HTMLDialogElement (https://html.spec.whatwg.org/multipage/interactive-elements.html#the-dialog-element): show, showModal, close, requestClose, returnValue, the cancel, close, beforetoggle and
// toggle events, Escape closing a modal dialog. A modal dialog is in the top layer (the place src/web-api/js/popover.js keeps) and has data-solar-modal, which :modal matches; it closes the
// popovers that are not its ancestors. What is outside it is not made inert.
(() => {
  "use strict";
  if (typeof HTMLDialogElement !== "function" || typeof document === "undefined") return;
  const MODAL = "data-solar-modal";
  const MODAL_KEY = Symbol.for("solar.modal");
  const layer = () => globalThis[Symbol.for("solar.toplayer")];
  const define = (target, name, descriptor) => Object.defineProperty(target, name, { configurable: true, enumerable: true, ...descriptor });
  const method = (name, fn, length) => { Object.defineProperty(HTMLDialogElement.prototype, name, { value: fn, writable: true, configurable: true, enumerable: true }); Object.defineProperty(fn, "length", { value: length, configurable: true }); };
  const fail = (name, message) => new DOMException(message, name);
  const info = new WeakMap();  // dialog -> { returnValue, previousFocus, task }
  const infoOf = (d) => { let i = info.get(d); if (!i) { i = { returnValue: "", previousFocus: null, task: null }; info.set(d, i); } return i; };
  const nodeConnected = Reflect.getOwnPropertyDescriptor(Node.prototype, "isConnected").get;
  const connected = (el) => Reflect.apply(nodeConnected, el, []);
  const focusedElement = () => { const a = document.activeElement; return a && a !== document.body && a !== document.documentElement ? a : null; };
  const isPopoverShowing = (d) => d.matches(":popover-open");
  const isModal = (d) => !!d[MODAL_KEY];

  define(HTMLDialogElement.prototype, "open", { get() { return this.hasAttribute("open"); }, set(v) { if (v) this.setAttribute("open", ""); else this.removeAttribute("open"); } });
  define(HTMLDialogElement.prototype, "returnValue", { get() { return infoOf(this).returnValue; }, set(v) { infoOf(this).returnValue = String(v); } });
  define(HTMLDialogElement.prototype, "closedBy", {
    get() { const v = (this.getAttribute("closedby") || "").toLowerCase(); return v === "any" || v === "closerequest" || v === "none" ? v : (this.hasAttribute("closedby") ? "auto" : "auto"); },
    set(v) { this.setAttribute("closedby", String(v)); },
  });

  const fireToggle = (d, type, oldState, newState, cancelable) => d.dispatchEvent(typeof ToggleEvent === "function" ? new ToggleEvent(type, { cancelable, oldState, newState }) : new Event(type, { cancelable }));
  const queueToggle = (d, oldState, newState) => {
    const i = infoOf(d);
    if (i.task) { oldState = i.task.oldState; clearTimeout(i.task.id); }
    const task = { oldState, newState, id: 0 };
    task.id = setTimeout(() => { i.task = null; fireToggle(d, "toggle", task.oldState, task.newState, false); }, 0);
    i.task = task;
  };

  // The focus a dialog takes when it opens: what has autofocus in it, or the first thing that can be focused.
  const focusInside = (d) => {
    let control = null;
    if (d.hasAttribute("autofocus")) control = d;
    else control = d.querySelector("[autofocus]");
    if (!control) {
      const candidates = d.querySelectorAll("a[href], button, input:not([type=hidden]), select, textarea, summary, [tabindex], [contenteditable]");
      for (const c of candidates) { if (!c.matches(":disabled") && c.getAttribute("tabindex") !== "-1" && typeof c.focus === "function") { control = c; break; } }
    }
    if (control && typeof control.focus === "function") control.focus();
  };

  const open = (d, modal) => {
    if (d.hasAttribute("open")) {
      if (isModal(d) === modal) return;
      throw fail("InvalidStateError", modal ? "The dialog is already open as a non-modal dialog." : "The dialog is already open as a modal dialog.");
    }
    if (isPopoverShowing(d)) throw fail("InvalidStateError", "The dialog is already open as a popover.");
    if (modal && !connected(d)) throw fail("InvalidStateError", "The element is not in a document.");
    if (modal && document.fullscreenElement === d) throw fail("InvalidStateError", "The dialog is fullscreen.");
    if (!fireToggle(d, "beforetoggle", "closed", "open", true)) return;
    if (d.hasAttribute("open") || (modal && !connected(d))) return;
    if (modal) {
      // The popovers that are not its ancestors go.
      for (const p of [...document.querySelectorAll(":popover-open")]) {
        let inside = false;
        for (let n = d; n; n = n.assignedSlot || (n.parentNode && n.parentNode.host) || n.parentNode) if (n === p) { inside = true; break; }
        if (!inside && p.popover !== "manual") try { p.hidePopover(); } catch (e) {}
      }
    }
    d.setAttribute("open", "");
    const i = infoOf(d);
    i.previousFocus = focusedElement();
    if (modal) {
      d[MODAL_KEY] = true;
      d.setAttribute(MODAL, "");
      if (layer()) layer().add(d);
    }
    focusInside(d);
    queueToggle(d, "closed", "open");
  };
  method("show", function show() { open(this, false); }, 0);
  method("showModal", function showModal() { open(this, true); }, 0);

  const close = (d, result, fromRemoval) => {
    if (!d.hasAttribute("open")) return;
    const i = infoOf(d);
    if (result !== undefined) i.returnValue = String(result);
    if (!fromRemoval) fireToggle(d, "beforetoggle", "open", "closed", false);
    if (!d.hasAttribute("open") && !isModal(d)) return;
    d.removeAttribute("open");
    cleanup(d);
    queueToggle(d, "open", "closed");
    setTimeout(() => d.dispatchEvent(new Event("close")), 0);
    const previous = i.previousFocus;
    i.previousFocus = null;
    if (previous && connected(previous) && typeof previous.focus === "function") previous.focus();
  };
  const cleanup = (d) => {
    if (d[MODAL_KEY]) { d[MODAL_KEY] = false; d.removeAttribute(MODAL); if (layer()) layer().remove(d); }
  };
  method("close", function close_(returnValue) { close(this, returnValue); }, 0);
  method("requestClose", function requestClose(returnValue) {
    if (!this.hasAttribute("open")) return;
    if (this.dispatchEvent(new Event("cancel", { cancelable: true }))) close(this, returnValue);
  }, 0);

  // `open` taken away or the dialog taken out of the document: it is not modal any more.
  if (typeof MutationObserver === "function") {
    new MutationObserver(() => {
      for (const d of document.querySelectorAll("dialog[" + MODAL + "]")) if (!d.hasAttribute("open")) cleanup(d);
      const l = layer();
      if (l) for (const d of [...(globalThis.__solarModalDialogs || [])]) if (!connected(d)) cleanup(d);
    }).observe(document, { attributes: true, subtree: true, attributeFilter: ["open"], childList: true });
  }

  // Escape asks the topmost modal dialog to close, unless a popover is above it or the page took the key.
  document.addEventListener("keydown", (e) => {
    if (e.key !== "Escape" || e.defaultPrevented) return;
    const modals = [...document.querySelectorAll("dialog[" + MODAL + "]")];
    const top = modals[modals.length - 1];
    if (top && top.closedBy !== "none") top.requestClose();
  });

  // A form with method=dialog closes the dialog it is in; the value of the button that submitted it is the dialog's returnValue.
  document.addEventListener("submit", (e) => {
    const form = e.target;
    if (!(form instanceof HTMLFormElement) || e.defaultPrevented) return;
    const submitter = e.submitter;
    const method = ((submitter && submitter.getAttribute("formmethod")) || form.getAttribute("method") || "").toLowerCase();
    if (method !== "dialog") return;
    let dialog = null;
    for (let n = form.parentNode; n; n = n.parentNode) if (n instanceof HTMLDialogElement) { dialog = n; break; }
    if (!dialog) return;
    e.preventDefault();
    close(dialog, submitter && submitter.hasAttribute("value") ? submitter.value : undefined);
  });
})();
