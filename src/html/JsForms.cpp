/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/html/HtmlBindings.h"

// Form controls and forms (https://html.spec.whatwg.org/#forms): the value and checkedness of inputs, textareas, selects and options,
// the form that owns a control, labels, what a click on a control does (toggle, submit, reset), and the submission of a form. It is written
// in script, over the attributes of the elements, as the rest of the window's interfaces are.
namespace solar::html {

namespace {

const char* const kFormScript = R"JS(
(function () {
  const setHelper = globalThis.__solarSetHelper;
  const nativeChanged = globalThis.__solarFormChanged;
  const formChanged = (e) => { if (e && nativeChanged) nativeChanged(e); };
  const define = (proto, name, get, set) => Object.defineProperty(proto, name, { get, set, enumerable: true, configurable: true });
  const method = (proto, name, fn) => Object.defineProperty(proto, name, { value: fn, writable: true, enumerable: true, configurable: true });
  const str = (proto, prop, attr = prop.toLowerCase(), fallback = "") => define(proto, prop, function () { const v = this.getAttribute(attr); return v === null ? fallback : v; }, function (v) { this.setAttribute(attr, String(v)); });
  const bool = (proto, prop, attr = prop.toLowerCase()) => define(proto, prop, function () { return this.hasAttribute(attr); }, function (v) { if (v) this.setAttribute(attr, ""); else this.removeAttribute(attr); });
  const int = (proto, prop, attr, fallback) => define(proto, prop, function () { const n = parseInt(this.getAttribute(attr), 10); return Number.isFinite(n) ? n : fallback; }, function (v) { this.setAttribute(attr, String(Math.trunc(Number(v)))); });
  const enumerated = (proto, prop, attr, values, fallback) => define(proto, prop, function () { const v = (this.getAttribute(attr) || "").toLowerCase(); return values.includes(v) ? v : fallback; }, function (v) { this.setAttribute(attr, String(v)); });

  const I = HTMLInputElement.prototype, T = HTMLTextAreaElement.prototype, B = HTMLButtonElement.prototype, S = HTMLSelectElement.prototype;
  const O = HTMLOptionElement.prototype, L = HTMLLabelElement.prototype, F = HTMLFormElement.prototype;
  const inputTypes = ["hidden", "text", "search", "tel", "url", "email", "password", "date", "month", "week", "time", "datetime-local", "number", "range", "color", "checkbox", "radio", "file", "submit", "image", "reset", "button"];
  const textTypes = ["text", "search", "tel", "url", "email", "password", "number", "date", "month", "week", "time", "datetime-local"];
  const state = new WeakMap();  // element → { value, dirty, checked, checkedDirty, indeterminate, start, end }
  const stateOf = (e) => { let s = state.get(e); if (!s) { s = { value: "", dirty: false, checked: false, checkedDirty: false, indeterminate: false, start: 0, end: 0, direction: "none" }; state.set(e, s); } return s; };
  const typeOf = (e) => { const t = (e.getAttribute("type") || "").toLowerCase(); return inputTypes.includes(t) ? t : "text"; };

  // ---- Shared: name, disabled, form, labels, validity ----
  // Reference target: a shadow root can name one element in it that stands for its host when something points at the host.
  const referenceTargets = new WeakMap();
  Object.defineProperty(ShadowRoot.prototype, "referenceTarget", { get() { return referenceTargets.has(this) ? referenceTargets.get(this) : null; }, set(v) { referenceTargets.set(this, v === null ? null : String(v)); }, enumerable: true, configurable: true });
  const attachShadow = Element.prototype.attachShadow;
  Object.defineProperty(Element.prototype, "attachShadow", { value: function attachShadow_(init) { const root = attachShadow.call(this, init); if (init && init.referenceTarget !== undefined && init.referenceTarget !== null) referenceTargets.set(root, String(init.referenceTarget)); return root; }, writable: true, enumerable: true, configurable: true });
  const standsFor = (host, test) => {
    for (let h = host, depth = 0; h && h.shadowRoot && depth < 16; ++depth) {
      const name = h.shadowRoot.referenceTarget;
      if (name === null) return false;
      const inner = h.shadowRoot.getElementById(name);
      if (!inner) return false;
      if (test(inner)) return true;
      h = inner;
    }
    return false;
  };
  const sameRoot = (a, b) => a.getRootNode() === b.getRootNode();
  const formOf = (e) => {
    const id = e.getAttribute("form");
    if (id !== null) {
      const f = e.getRootNode().getElementById ? e.getRootNode().getElementById(id) : null;
      if (f instanceof HTMLFormElement) return f;
      if (f && standsFor(f, (x) => x instanceof HTMLFormElement)) return f;
      return null;
    }
    for (let p = e.parentNode; p; p = p.parentNode) if (p instanceof HTMLFormElement) return p;
    return null;
  };
  const disabledNow = (e) => {
    if (e.hasAttribute("disabled")) return true;
    for (let p = e.parentNode; p; p = p.parentNode) if (p instanceof HTMLFieldSetElement && p.hasAttribute("disabled")) { const legend = Array.prototype.find.call(p.children, (c) => c instanceof HTMLLegendElement); if (!legend || !legend.contains(e)) return true; }
    return false;
  };
  const labelsOf = (e) => {
    const root = e.getRootNode();
    const all = root.querySelectorAll ? Array.from(root.querySelectorAll("label")) : [];
    return all.filter((l) => l.control === e);
  };
  for (const proto of [I, T, B, S, HTMLFieldSetElement.prototype, HTMLOutputElement.prototype, HTMLObjectElement.prototype]) {
    if (!proto) continue;
    define(proto, "form", function () { return formOf(this); });
  }
  for (const proto of [I, T, B, S]) {
    str(proto, "name"); bool(proto, "disabled"); str(proto, "autocomplete");
    define(proto, "labels", function () { return labelsOf(this); });
    define(proto, "willValidate", function () { return !disabledNow(this) && !this.hasAttribute("readonly"); });
    define(proto, "validity", function () { return { valid: true, valueMissing: false, typeMismatch: false, patternMismatch: false, tooLong: false, tooShort: false, rangeUnderflow: false, rangeOverflow: false, stepMismatch: false, badInput: false, customError: false }; });
    define(proto, "validationMessage", function () { return ""; });
    method(proto, "checkValidity", function () { return true; });
    method(proto, "reportValidity", function () { return true; });
    method(proto, "setCustomValidity", function () {});
  }
  for (const proto of [I, T]) {
    bool(proto, "readOnly", "readonly"); bool(proto, "required"); str(proto, "placeholder");
    int(proto, "maxLength", "maxlength", -1); int(proto, "minLength", "minlength", -1);
  }

  // ---- input ----
  define(I, "type", function () { return typeOf(this); }, function (v) { this.setAttribute("type", String(v)); });
  for (const [prop, attr] of [["accept", "accept"], ["alt", "alt"], ["dirName", "dirname"], ["max", "max"], ["min", "min"], ["pattern", "pattern"], ["step", "step"], ["formAction", "formaction"], ["formEnctype", "formenctype"], ["formMethod", "formmethod"], ["formTarget", "formtarget"], ["useMap", "usemap"]]) str(I, prop, attr);
  bool(I, "multiple"); bool(I, "formNoValidate", "formnovalidate"); bool(I, "defaultChecked", "checked");
  int(I, "size", "size", 20); int(I, "width", "width", 0); int(I, "height", "height", 0);
  define(I, "src", function () { const v = this.getAttribute("src"); return v === null ? "" : new URL(v, this.baseURI).href; }, function (v) { this.setAttribute("src", String(v)); });
  define(I, "defaultValue", function () { return this.getAttribute("value") || ""; }, function (v) { this.setAttribute("value", String(v)); });
  define(I, "files", function () { return typeOf(this) === "file" ? [] : null; });
  const sanitize = (e, v) => {
    const t = typeOf(e);
    if (textTypes.includes(t) && t !== "number" && !/^(date|month|week|time|datetime-local)$/.test(t)) {
      v = String(v).replace(/\r\n?|\n/g, " ");
      if (t === "email" && !e.hasAttribute("multiple")) v = v.trim();
      if (t === "url" || t === "tel") v = v.trim();
    }
    if (t === "number") { v = String(v).trim(); if (!/^-?(\d+|\d*\.\d+)([eE][-+]?\d+)?$/.test(v)) v = ""; }
    if (t === "range") { const n = parseFloat(v); const lo = parseFloat(e.min) || 0, hi = e.hasAttribute("max") ? parseFloat(e.max) : 100; v = Number.isFinite(n) ? String(Math.min(hi, Math.max(lo, n))) : String((lo + hi) / 2); }
    if (t === "color") v = /^#[0-9a-f]{6}$/i.test(v) ? v.toLowerCase() : "#000000";
    return v;
  };
  define(I, "value", function () {
    const t = typeOf(this), s = stateOf(this);
    if (t === "checkbox" || t === "radio") { const v = this.getAttribute("value"); return v === null ? "on" : v; }
    if (t === "file") return s.dirty ? s.value : "";
    if (s.dirty) return s.value;
    return sanitize(this, this.getAttribute("value") || (t === "range" ? "" : ""));
  }, function (v) {
    const t = typeOf(this);
    if (t === "checkbox" || t === "radio" || t === "hidden" || t === "submit" || t === "reset" || t === "button" || t === "image") { this.setAttribute("value", String(v)); return; }
    if (t === "file") { if (v === "") stateOf(this).value = ""; else throw new DOMException("Failed to set the 'value' property on 'HTMLInputElement': This input element accepts a filename, which may only be programmatically set to the empty string.", "InvalidStateError"); return; }
    const s = stateOf(this);
    s.value = sanitize(this, String(v)); s.dirty = true; s.start = s.end = s.value.length;
    formChanged(this);
  });
  define(I, "checked", function () { const s = stateOf(this); return s.checkedDirty ? s.checked : this.hasAttribute("checked"); }, function (v) { setChecked(this, !!v); });
  define(I, "indeterminate", function () { return stateOf(this).indeterminate; }, function (v) { stateOf(this).indeterminate = !!v; formChanged(this); });
  define(I, "valueAsNumber", function () { const n = parseFloat(this.value); return Number.isFinite(n) ? n : NaN; }, function (v) { this.value = String(v); });
  define(I, "valueAsDate", function () { return null; });
  method(I, "stepUp", function (n = 1) { const step = parseFloat(this.step) || 1; this.value = String((parseFloat(this.value) || 0) + step * n); });
  method(I, "stepDown", function (n = 1) { this.stepUp(-n); });
  // The radio buttons of a group: same name, same form (or none) and the same tree.
  const groupOf = (e) => {
    const name = e.getAttribute("name");
    if (!name) return [e];
    const form = e.form, root = e.getRootNode();
    return Array.from(root.querySelectorAll ? root.querySelectorAll("input") : []).filter((o) => typeOf(o) === "radio" && o.getAttribute("name") === name && o.form === form);
  };
  const setChecked = (e, v) => {
    const s = stateOf(e);
    s.checked = v; s.checkedDirty = true;
    if (v && typeOf(e) === "radio") for (const o of groupOf(e)) if (o !== e) { const os = stateOf(o); os.checked = false; os.checkedDirty = true; formChanged(o); }
    formChanged(e);
  };
  // Selection in text controls.
  const selectionApplies = (e) => e instanceof HTMLTextAreaElement || ["text", "search", "tel", "url", "password"].includes(typeOf(e));
  const clampSel = (e) => { const s = stateOf(e), n = e.value.length; s.start = Math.min(s.start, n); s.end = Math.min(Math.max(s.end, s.start), n); return s; };
  for (const proto of [I, T]) {
    define(proto, "selectionStart", function () { if (!selectionApplies(this)) return null; return clampSel(this).start; }, function (v) { const s = clampSel(this); s.start = Math.min(Math.max(0, v | 0), this.value.length); if (s.end < s.start) s.end = s.start; formChanged(this); });
    define(proto, "selectionEnd", function () { if (!selectionApplies(this)) return null; return clampSel(this).end; }, function (v) { const s = clampSel(this); s.end = Math.min(Math.max(s.start, v | 0), this.value.length); formChanged(this); });
    define(proto, "selectionDirection", function () { return selectionApplies(this) ? stateOf(this).direction : null; }, function (v) { stateOf(this).direction = String(v); });
    method(proto, "select", function () { const s = stateOf(this); s.start = 0; s.end = this.value.length; formChanged(this); });
    method(proto, "setSelectionRange", function (a, b, d = "none") { const n = this.value.length, s = stateOf(this); s.start = Math.min(a | 0, n); s.end = Math.min(Math.max(b | 0, s.start), n); s.direction = d; formChanged(this); });
    method(proto, "setRangeText", function (text, a, b) {
      const s = clampSel(this), v = this.value;
      if (a === undefined) { a = s.start; b = s.end; }
      this.value = v.slice(0, a) + text + v.slice(b);
      s.start = s.end = a + String(text).length;
    });
  }

  // ---- textarea ----
  const textOf = (e) => { let t = ""; for (const c of e.childNodes) if (c.nodeType === 3) t += c.data; return t.replace(/^\n/, ""); };
  define(T, "defaultValue", function () { return textOf(this); }, function (v) { this.textContent = String(v); });
  define(T, "value", function () { const s = stateOf(this); return (s.dirty ? s.value : textOf(this)).replace(/\r\n?/g, "\n"); }, function (v) { const s = stateOf(this); s.value = String(v); s.dirty = true; s.start = s.end = s.value.length; formChanged(this); });
  define(T, "type", function () { return "textarea"; });
  define(T, "textLength", function () { return this.value.length; });
  int(T, "rows", "rows", 2); int(T, "cols", "cols", 20); str(T, "wrap", "wrap", "soft");

  // ---- button ----
  define(B, "type", function () { const t = (this.getAttribute("type") || "").toLowerCase(); return t === "button" || t === "reset" ? t : "submit"; }, function (v) { this.setAttribute("type", String(v)); });
  str(B, "value"); for (const [prop, attr] of [["formAction", "formaction"], ["formEnctype", "formenctype"], ["formMethod", "formmethod"], ["formTarget", "formtarget"]]) str(B, prop, attr);
  bool(B, "formNoValidate", "formnovalidate");

  // ---- option and select ----
  const selectOf = (o) => { for (let p = o.parentNode; p; p = p.parentNode) { if (p instanceof HTMLSelectElement) return p; if (!(p instanceof HTMLOptGroupElement)) return null; } return null; };
  const optionsOf = (sel) => { const out = []; for (const c of sel.children) { if (c instanceof HTMLOptionElement) out.push(c); else if (c instanceof HTMLOptGroupElement) for (const o of c.children) if (o instanceof HTMLOptionElement) out.push(o); } return out; };
  const strip = (v) => v.replace(/[\t\n\f\r ]+/g, " ").trim();
  define(O, "text", function () { return strip(this.textContent); }, function (v) { this.textContent = String(v); });
  define(O, "value", function () { const v = this.getAttribute("value"); return v === null ? this.text : v; }, function (v) { this.setAttribute("value", String(v)); });
  define(O, "label", function () { const v = this.getAttribute("label"); return v === null ? this.text : v; }, function (v) { this.setAttribute("label", String(v)); });
  bool(O, "disabled"); bool(O, "defaultSelected", "selected");
  define(O, "form", function () { const s = selectOf(this); return s ? s.form : null; });
  define(O, "index", function () { const s = selectOf(this); return s ? optionsOf(s).indexOf(this) : 0; });
  define(O, "selected", function () { const s = stateOf(this); if (s.checkedDirty) return s.checked; const sel = selectOf(this); if (sel) resetSelection(sel); const t = stateOf(this); return t.checkedDirty ? t.checked : this.hasAttribute("selected"); },
    function (v) { const s = stateOf(this); s.checked = !!v; s.checkedDirty = true; const sel = selectOf(this); if (sel && v && !sel.multiple) for (const o of optionsOf(sel)) if (o !== this) { const os = stateOf(o); os.checked = false; os.checkedDirty = true; } formChanged(sel); });
  // A select with no selected option (and not multiple, with display size 1) selects its first non-disabled option.
  const resetSelection = (sel) => {
    const options = optionsOf(sel);
    if (sel.multiple || options.some((o) => stateOf(o).checkedDirty && stateOf(o).checked)) return;
    const withAttr = options.filter((o) => o.hasAttribute("selected") && !stateOf(o).checkedDirty);
    if (withAttr.length) { const keep = sel.multiple ? withAttr : [withAttr[withAttr.length - 1]]; for (const o of options) { const s = stateOf(o); s.checked = keep.includes(o); s.checkedDirty = true; } return; }
    if (parseInt(sel.getAttribute("size"), 10) > 1) return;
    const first = options.find((o) => !o.hasAttribute("disabled"));
    for (const o of options) { const s = stateOf(o); s.checked = o === first; s.checkedDirty = true; }
  };
  define(S, "options", function () { return optionsOf(this); });
  define(S, "selectedOptions", function () { resetSelection(this); return optionsOf(this).filter((o) => o.selected); });
  define(S, "length", function () { return optionsOf(this).length; });
  define(S, "type", function () { return this.multiple ? "select-multiple" : "select-one"; });
  define(S, "selectedIndex", function () { resetSelection(this); return optionsOf(this).findIndex((o) => o.selected); }, function (v) { const options = optionsOf(this); options.forEach((o, i) => { const s = stateOf(o); s.checked = i === v; s.checkedDirty = true; }); formChanged(this); });
  define(S, "value", function () { resetSelection(this); const o = optionsOf(this).find((x) => x.selected); return o ? o.value : ""; }, function (v) { let found = false; for (const o of optionsOf(this)) { const s = stateOf(o); s.checked = !found && o.value === String(v); s.checkedDirty = true; if (s.checked) found = true; } formChanged(this); });
  bool(S, "multiple"); bool(S, "required"); int(S, "size", "size", 0);
  method(S, "item", function (i) { return optionsOf(this)[i] || null; });
  method(S, "namedItem", function (n) { return optionsOf(this).find((o) => o.id === n || o.getAttribute("name") === n) || null; });
  method(S, "add", function (el, before) { const ref = typeof before === "number" ? optionsOf(this)[before] || null : before; (ref && ref.parentNode ? ref.parentNode : this).insertBefore(el, ref || null); });
  method(S, "remove", function (i) { if (arguments.length === 0) return Element.prototype.remove.call(this); const o = optionsOf(this)[i]; if (o) o.remove(); });

  // ---- label ----
  define(L, "htmlFor", function () { return this.getAttribute("for") || ""; }, function (v) { this.setAttribute("for", String(v)); });
  const labelable = (e) => e instanceof HTMLButtonElement || e instanceof HTMLMeterElement || e instanceof HTMLOutputElement || e instanceof HTMLProgressElement || e instanceof HTMLSelectElement || e instanceof HTMLTextAreaElement || (e instanceof HTMLInputElement && typeOf(e) !== "hidden");
  define(L, "control", function () {
    const id = this.getAttribute("for");
    if (id !== null) { const e = this.getRootNode().getElementById ? this.getRootNode().getElementById(id) : null; return e && (labelable(e) || standsFor(e, labelable)) ? e : null; }
    const all = this.querySelectorAll("*");
    for (const e of all) if (labelable(e)) return e;
    return null;
  });
  define(L, "form", function () { const c = this.control; return c ? c.form : null; });

  // ---- form ----
  const controlsOf = (form) => {
    const root = form.getRootNode();
    const all = root.querySelectorAll ? Array.from(root.querySelectorAll("input, button, select, textarea, fieldset, output, object")) : [];
    return all.filter((e) => formOf(e) === form && !(e instanceof HTMLInputElement && typeOf(e) === "image"));
  };
  for (const [prop, attr] of [["acceptCharset", "accept-charset"], ["name", "name"], ["target", "target"], ["rel", "rel"]]) str(F, prop, attr);
  define(F, "action", function () { const v = this.getAttribute("action"); return v === null || v === "" ? this.ownerDocument.URL : new URL(v, this.baseURI).href; }, function (v) { this.setAttribute("action", String(v)); });
  enumerated(F, "method", "method", ["get", "post", "dialog"], "get");
  enumerated(F, "enctype", "enctype", ["application/x-www-form-urlencoded", "multipart/form-data", "text/plain"], "application/x-www-form-urlencoded");
  define(F, "encoding", function () { return this.enctype; }, function (v) { this.enctype = v; });
  bool(F, "noValidate", "novalidate");
  define(F, "elements", function () { const list = controlsOf(this); list.item = (i) => list[i] || null; list.namedItem = (n) => list.find((e) => e.id === n || e.getAttribute("name") === n) || null; return list; });
  define(F, "length", function () { return controlsOf(this).length; });
  method(F, "checkValidity", function () { return true; });
  method(F, "reportValidity", function () { return true; });
  const entriesOf = (form, submitter) => {
    const entries = [];
    for (const e of controlsOf(form)) {
      const name = e.getAttribute("name");
      if (disabledNow(e) || e instanceof HTMLFieldSetElement || e instanceof HTMLOutputElement || e instanceof HTMLObjectElement) continue;
      if (e instanceof HTMLButtonElement || (e instanceof HTMLInputElement && /^(submit|image|reset|button)$/.test(typeOf(e)))) { if (e === submitter && name && !(e instanceof HTMLInputElement && typeOf(e) === "button") && typeOf(e) !== "reset") entries.push([name, e.value]); continue; }
      if (!name) continue;
      if (e instanceof HTMLInputElement) {
        const t = typeOf(e);
        if ((t === "checkbox" || t === "radio")) { if (e.checked) entries.push([name, e.value]); continue; }
        if (t === "file") continue;
        entries.push([name, e.value]);
      } else if (e instanceof HTMLSelectElement) { for (const o of optionsOf(e)) if (o.selected && !o.hasAttribute("disabled")) entries.push([name, o.value]); }
      else if (e instanceof HTMLTextAreaElement) entries.push([name, e.value.replace(/\r?\n/g, "\r\n")]);
    }
    return entries;
  };
  const encode = (s) => encodeURIComponent(s).replace(/%20/g, "+").replace(/[!'()~]/g, (c) => "%" + c.charCodeAt(0).toString(16).toUpperCase());
  const submit = (form, submitter) => {
    const method = submitter && submitter.hasAttribute("formmethod") ? submitter.formMethod : form.method;
    let action = submitter && submitter.hasAttribute("formaction") ? new URL(submitter.getAttribute("formaction"), form.baseURI).href : form.action;
    if (method === "dialog") return;
    const query = entriesOf(form, submitter).map(([n, v]) => encode(n) + "=" + encode(v)).join("&");
    const url = new URL(action);
    if (method === "get" || !/^https?:$/.test(url.protocol)) { url.search = query; location.assign(url.href); return; }
    // POST: the address and the body go to the program, which navigates.
    const enc = submitter && submitter.hasAttribute("formenctype") ? submitter.formEnctype : form.enctype;
    if (typeof globalThis.__solarSubmitPost === "function") __solarSubmitPost(url.href, enc === "text/plain" ? entriesOf(form, submitter).map(([n, v]) => n + "=" + v).join("\r\n") : query, enc === "text/plain" ? "text/plain" : "application/x-www-form-urlencoded");
    else { url.search = query; location.assign(url.href); }
  };
  method(F, "submit", function () { submit(this, null); });
  method(F, "requestSubmit", function (submitter) {
    if (submitter !== undefined && submitter !== null) {
      if (!(submitter instanceof HTMLElement) || !((submitter instanceof HTMLButtonElement && submitter.type === "submit") || (submitter instanceof HTMLInputElement && /^(submit|image)$/.test(typeOf(submitter))))) throw new TypeError("The specified element is not a submit button.");
      if (submitter.form !== this) throw new DOMException("The specified element is not owned by this form element.", "NotFoundError");
    }
    const event = new SubmitEvent("submit", { bubbles: true, cancelable: true, submitter: submitter || null });
    if (this.dispatchEvent(event)) submit(this, submitter || null);
  });
  method(F, "reset", function () {
    if (!this.dispatchEvent(new Event("reset", { bubbles: true, cancelable: true }))) return;
    for (const e of controlsOf(this)) {
      if (e instanceof HTMLInputElement || e instanceof HTMLTextAreaElement) { const s = stateOf(e); s.dirty = false; s.checkedDirty = false; s.value = ""; }
      else if (e instanceof HTMLSelectElement) for (const o of optionsOf(e)) stateOf(o).checkedDirty = false;
      formChanged(e);
    }
  });
  if (typeof SubmitEvent === "undefined") {
    globalThis.SubmitEvent = class SubmitEvent extends Event { constructor(type, init = {}) { super(type, init); Object.defineProperty(this, "submitter", { value: init.submitter || null, enumerable: true }); } };
  }

  // ---- What a click does ----
  // The element a click reaches that has a behaviour of its own: the nearest link, button, input, label or summary.
  const activationTarget = (e) => {
    for (let p = e; p; p = p.parentNode) {
      if (!(p instanceof Element)) continue;
      if (p instanceof HTMLAnchorElement && p.hasAttribute("href")) return p;
      if (p instanceof HTMLButtonElement || p instanceof HTMLInputElement || p instanceof HTMLLabelElement || p instanceof HTMLSelectElement || p instanceof HTMLTextAreaElement || p.localName === "summary") return p;
    }
    return null;
  };
  const preActivate = (t) => {
    if (!(t instanceof HTMLInputElement)) return null;
    const type = typeOf(t);
    if (type === "checkbox") { const s = stateOf(t); const before = { checked: t.checked, indeterminate: s.indeterminate }; setChecked(t, !before.checked); s.indeterminate = false; return before; }
    if (type === "radio") { const before = groupOf(t).map((r) => [r, r.checked]); setChecked(t, true); return before; }
    return null;
  };
  const postActivate = (t, ev, before) => {
    if (ev.defaultPrevented) {
      if (t instanceof HTMLInputElement) {
        const type = typeOf(t);
        if (type === "checkbox" && before) { const s = stateOf(t); s.checked = before.checked; s.indeterminate = before.indeterminate; }
        if (type === "radio" && before) for (const [r, c] of before) { const s = stateOf(r); s.checked = c; s.checkedDirty = true; }
      }
      return;
    }
    if (t instanceof HTMLInputElement) {
      const type = typeOf(t);
      if (type === "checkbox" || type === "radio") { t.dispatchEvent(new Event("input", { bubbles: true, composed: true })); t.dispatchEvent(new Event("change", { bubbles: true })); }
      else if (type === "submit" || type === "image") { const f = t.form; if (f && !disabledNow(t)) f.requestSubmit(t); }
      else if (type === "reset") { const f = t.form; if (f) f.reset(); }
    } else if (t instanceof HTMLButtonElement) {
      const f = t.form;
      if (f && !disabledNow(t)) { if (t.type === "submit") f.requestSubmit(t); else if (t.type === "reset") f.reset(); }
    } else if (t instanceof HTMLLabelElement) {
      const c = t.control;
      if (c && !disabledNow(c) && c !== ev.target && !c.contains(ev.target)) c.click();
    } else if (t.localName === "summary" && t.parentNode instanceof HTMLDetailsElement && t.parentNode.firstElementChild === t) {
      t.parentNode.open = !t.parentNode.open;
    }
  };
  // The click of the program: the event, and then what the target does when no script stopped it. The pointer is where it was.
  const clickEvent = (x, y, button) => new MouseEvent("click", { bubbles: true, cancelable: true, composed: true, view: globalThis, clientX: x, clientY: y, button, detail: 1 });
  // What the user does is an activation (the Pointer Lock API asks for one): its time is kept for a while.
  const activate = () => { try { document[Symbol.for("solar.activation")] = performance.now(); } catch (e) {} };
  setHelper("click", (target, x, y, button) => {
    activate();
    if (disabledNow(target) && target instanceof HTMLInputElement) return false;
    const t = activationTarget(target);
    const before = t ? preActivate(t) : null;
    const ev = clickEvent(x, y, button);
    const proceed = target.dispatchEvent(ev);
    if (t && !(t instanceof HTMLAnchorElement)) postActivate(t, ev, before);
    return proceed;
  });
  // element.click() does the same.
  method(HTMLElement.prototype, "click", function click() {
    if (this.hasAttribute("disabled") && /^(button|input|select|textarea|fieldset|optgroup|option)$/.test(this.localName)) return;
    const t = activationTarget(this);
    const before = t ? preActivate(t) : null;
    const ev = clickEvent(0, 0, 0);
    this.dispatchEvent(ev);
    if (t && !(t instanceof HTMLAnchorElement)) postActivate(t, ev, before);
  });
  // With the pointer locked: the mouse events are the locked element's, and the position stays where it was.
  setHelper("locked", (type, x, y, button, buttons, mx, my) => {
    if (type !== "mousemove") activate();
    const t = document.pointerLockElement;
    if (!t) return true;
    const init = { bubbles: true, cancelable: true, composed: true, view: globalThis, clientX: x, clientY: y, button, buttons, movementX: mx, movementY: my };
    const proceed = t.dispatchEvent(new MouseEvent(type, { ...init, detail: type === "mousemove" ? 0 : 1 }));
    if (type === "mouseup") t.dispatchEvent(new MouseEvent("click", { ...init, detail: 1 }));
    return proceed;
  });
  setHelper("pointer", (target, type, x, y, button, buttons) => {
    if (type !== "mousemove") activate();
    const proceed = target.dispatchEvent(new MouseEvent(type, { bubbles: true, cancelable: true, composed: true, view: globalThis, clientX: x, clientY: y, button, buttons, detail: type === "mousemove" ? 0 : 1 }));
    // A press gives the focus to the nearest thing that can take it, or takes it from what has it.
    if (type === "mousedown" && proceed && button === 0) {
      const before = document.activeElement;
      for (let p = target; p; p = p.parentNode instanceof Element ? p.parentNode : (p.parentNode && p.parentNode.host) || null) {
        if (typeof p.focus !== "function") continue;
        p.focus();
        if (document.activeElement === p || document.activeElement !== before) return proceed;
      }
      if (document.activeElement && document.activeElement !== document.body) document.activeElement.blur();
    }
    return proceed;
  });
  // A key of the keyboard: the event goes to the target (the focused element, or the body).
  setHelper("key", (target, type, key, code, keyCode, repeat, mods) => {
    if (type === "keydown") activate();
    const ev = new KeyboardEvent(type, { bubbles: true, cancelable: true, composed: true, view: globalThis, key, code, keyCode, charCode: type === "keypress" ? keyCode : 0, repeat, ctrlKey: !!(mods & 1), shiftKey: !!(mods & 2), altKey: !!(mods & 4), metaKey: !!(mods & 8) });
    try { if (ev.which === undefined || ev.which === 0) Object.defineProperty(ev, "which", { value: keyCode }); } catch (e) {}
    return target.dispatchEvent(ev);
  });
  // The pointer moved from one element to another: out and leave for the ones it left, over and enter for the ones it came into.
  setHelper("hover", (from, to, x, y) => {
    const chain = (e) => { const list = []; for (let p = e; p; p = p.parentNode instanceof Element ? p.parentNode : (p.parentNode && p.parentNode.host) || null) list.push(p); return list; };
    const before = chain(from), after = chain(to);
    const init = (related, bubbles) => ({ bubbles, cancelable: bubbles, composed: true, view: globalThis, clientX: x, clientY: y, relatedTarget: related });
    if (from) from.dispatchEvent(new MouseEvent("mouseout", init(to, true)));
    for (const e of before) if (!after.includes(e)) e.dispatchEvent(new MouseEvent("mouseleave", init(to, false)));
    if (to) to.dispatchEvent(new MouseEvent("mouseover", init(from, true)));
    for (const e of after.slice().reverse()) if (!before.includes(e)) e.dispatchEvent(new MouseEvent("mouseenter", init(from, false)));
  });
  // Typing: the control takes the text, as the user's input does.
  setHelper("input", (target, text, kind) => {
    if (!(target instanceof HTMLInputElement || target instanceof HTMLTextAreaElement)) return false;
    if (target.readOnly || disabledNow(target)) return false;
    if (target instanceof HTMLInputElement && !textTypes.includes(typeOf(target))) return false;
    if (kind === "enter") {
      if (target instanceof HTMLTextAreaElement) { text = "\n"; kind = "text"; }
      else {
        const f = target.form;
        if (!f) return true;
        // Implicit submission: the form's default button is clicked; with none, the form is submitted if it has one text field.
        const button = Array.from(f.elements).find((e) => (e instanceof HTMLButtonElement && e.type === "submit") || (e instanceof HTMLInputElement && /^(submit|image)$/.test(typeOf(e))));
        if (button) { if (!disabledNow(button)) button.click(); }
        else if (Array.from(f.elements).filter((e) => e instanceof HTMLInputElement && textTypes.includes(typeOf(e))).length <= 1) f.requestSubmit();
        return true;
      }
    }
    const s = clampSel(target), v = target.value;
    let start = s.start, end = s.end, data = text;
    if (kind === "backspace") { if (start === end) start = Math.max(0, start - 1); data = ""; }
    else if (kind === "delete") { if (start === end) end = Math.min(v.length, end + 1); data = ""; }
    else if (kind === "left" || kind === "right" || kind === "home" || kind === "end") {
      const p = kind === "left" ? (start === end ? Math.max(0, start - 1) : start) : kind === "right" ? (start === end ? Math.min(v.length, end + 1) : end) : kind === "home" ? 0 : v.length;
      s.start = s.end = p; formChanged(target); return true;
    } else if (target instanceof HTMLInputElement && target.maxLength >= 0) data = data.slice(0, Math.max(0, target.maxLength - (v.length - (end - start))));
    if (data === "" && start === end) return true;
    const ok = target.dispatchEvent(new InputEvent("beforeinput", { bubbles: true, cancelable: true, composed: true, data: data || null, inputType: kind === "backspace" ? "deleteContentBackward" : kind === "delete" ? "deleteContentForward" : "insertText" }));
    if (!ok) return true;
    const next = v.slice(0, start) + data + v.slice(end);
    const st = stateOf(target); st.value = target instanceof HTMLInputElement ? sanitizeTyping(target, next) : next; st.dirty = true; st.start = st.end = Math.min(start + data.length, st.value.length);
    target.dispatchEvent(new InputEvent("input", { bubbles: true, composed: true, data: data || null, inputType: kind === "backspace" ? "deleteContentBackward" : kind === "delete" ? "deleteContentForward" : "insertText" }));
    formChanged(target);
    return true;
  });
  const sanitizeTyping = (e, v) => (typeOf(e) === "number" || typeOf(e) === "range" || typeOf(e) === "color") ? v : sanitize(e, v);
  // An option chosen with the pointer or a key: mode 0 chooses it alone, 1 toggles it (a list that allows several), 2 extends from the one chosen last.
  setHelper("choose", (sel, index, mode) => {
    const options = optionsOf(sel), o = options[index];
    if (!o || o.hasAttribute("disabled") || disabledNow(sel)) return false;
    resetSelection(sel);
    const before = options.map((x) => x.selected);
    const put = (x, v) => { const s = stateOf(x); s.checked = v; s.checkedDirty = true; };
    const st = stateOf(sel);
    if (sel.multiple) {
      if (mode === 1) put(o, !o.selected);
      else if (mode === 2) { const a = st.anchor === undefined ? before.indexOf(true) : st.anchor; const from = a < 0 ? index : a; options.forEach((x, i) => put(x, i >= Math.min(from, index) && i <= Math.max(from, index) && !x.hasAttribute("disabled"))); }
      else options.forEach((x) => put(x, x === o));
      if (mode !== 2) st.anchor = index;
    } else options.forEach((x) => put(x, x === o));
    formChanged(sel);
    if (!options.some((x, i) => x.selected !== before[i])) return false;
    sel.dispatchEvent(new Event("input", { bubbles: true, composed: true }));
    sel.dispatchEvent(new Event("change", { bubbles: true }));
    return true;
  });
  setHelper("historyGo", (delta) => (typeof globalThis.__solarHistoryStep === "function" ? globalThis.__solarHistoryStep(delta) : false));
  // The selection of the page's text, as the pointer and the keys move it.
  setHelper("selectText", (an, ao, fn, fo) => { getSelection().setBaseAndExtent(an, ao, fn, fo); });
  setHelper("selectBlock", (el) => { getSelection().selectAllChildren(el); });
  setHelper("clearSelection", () => { getSelection().removeAllRanges(); });
  setHelper("selectedText", () => String(getSelection()));
  setHelper("listTop", (sel, top) => { stateOf(sel).top = Math.max(0, top | 0); formChanged(sel); });
  // Tab: the focus goes to the next (or, back, the previous) thing the keyboard reaches.
  setHelper("tab", (back) => {
    const tabIndex = (e) => { const n = parseInt(e.getAttribute("tabindex"), 10); return Number.isFinite(n) ? n : null; };
    const tabbable = (e) => {
      if (e instanceof HTMLInputElement && typeOf(e) === "hidden") return false;
      if ((e instanceof HTMLInputElement || e instanceof HTMLButtonElement || e instanceof HTMLSelectElement || e instanceof HTMLTextAreaElement) && disabledNow(e)) return false;
      const ti = tabIndex(e);
      if (ti !== null && ti < 0) return false;
      if (e.localName === "summary" && ti === null && !(e.parentNode instanceof HTMLDetailsElement && e.parentNode.firstElementChild === e)) return false;
      if (e.localName === "area" || e.localName === "a" && !e.hasAttribute("href") && ti === null) return false;
      if (e.hasAttribute("contenteditable") && e.getAttribute("contenteditable") === "false" && ti === null) return false;
      if (e instanceof HTMLInputElement && typeOf(e) === "radio" && e.getAttribute("name")) { const g = groupOf(e).filter((r) => !disabledNow(r)); const on = g.find((r) => r.checked); if ((on || g[0]) !== e) return false; }
      if (e.getClientRects().length === 0) return false;
      const v = getComputedStyle(e).visibility;
      return v !== "hidden" && v !== "collapse";
    };
    const list = Array.from(document.querySelectorAll("a[href], button, input, select, textarea, summary, [tabindex], [contenteditable]")).filter(tabbable);
    const order = list.filter((e) => (tabIndex(e) || 0) > 0).sort((a, b) => tabIndex(a) - tabIndex(b)).concat(list.filter((e) => !((tabIndex(e) || 0) > 0)));
    if (!order.length) return false;
    const cur = document.activeElement;
    let at = cur && cur !== document.body ? order.indexOf(cur) : -1;
    if (at < 0 && cur && cur !== document.body) {
      // The focus is on something the keyboard does not reach: it goes on from where that is in the document.
      const after = order.findIndex((e) => (cur.compareDocumentPosition(e) & Node.DOCUMENT_POSITION_FOLLOWING) !== 0);
      at = back ? (after < 0 ? order.length : after) : (after < 0 ? order.length - 1 : after - 1);
    }
    const next = back ? (at <= 0 ? order.length - 1 : at - 1) : (at + 1) % order.length;
    const target = order[next];
    target.focus();
    if (target instanceof HTMLInputElement && selectionApplies(target)) { const s = stateOf(target); s.start = 0; s.end = target.value.length; formChanged(target); }
    return true;
  });
  const rangeBounds = (e) => {
    const lo = e.hasAttribute("min") ? parseFloat(e.min) || 0 : 0;
    let hi = e.hasAttribute("max") ? parseFloat(e.max) : 100;
    if (!(hi >= lo)) hi = lo;
    const step = parseFloat(e.step) > 0 ? parseFloat(e.step) : 1;
    return { lo, hi, step };
  };
  const decimals = (n) => (String(n).split(".")[1] || "").length;
  const snapTo = (e, v) => {
    const { lo, hi, step } = rangeBounds(e);
    v = lo + Math.round((v - lo) / step) * step;
    v = Math.min(hi, Math.max(lo, v));
    return Number(v.toFixed(Math.max(decimals(step), decimals(lo), 0)));
  };
  const putValue = (e, v) => {
    const st = stateOf(e), text = String(v);
    if (e.value === text) return false;
    st.value = text; st.dirty = true; formChanged(e);
    e.dispatchEvent(new Event("input", { bubbles: true, composed: true }));
    return true;
  };
  // A range dragged: the thumb is at `fraction` of the way. `final` is the release, which is the change.
  setHelper("range", (e, fraction, final) => {
    if (!(e instanceof HTMLInputElement) || typeOf(e) !== "range" || disabledNow(e)) return false;
    const { lo, hi } = rangeBounds(e), st = stateOf(e);
    if (putValue(e, snapTo(e, lo + (hi - lo) * Math.min(1, Math.max(0, fraction))))) st.moved = true;
    if (final && st.moved) { st.moved = false; e.dispatchEvent(new Event("change", { bubbles: true })); }
    return true;
  });
  // Up and down (or left and right) on a range or a number field.
  setHelper("step", (e, dir) => {
    if (!(e instanceof HTMLInputElement) || disabledNow(e) || e.readOnly) return false;
    const t = typeOf(e);
    let v;
    if (t === "range") { const cur = parseFloat(e.value); v = snapTo(e, (Number.isFinite(cur) ? cur : rangeBounds(e).lo) + dir * rangeBounds(e).step); }
    else if (t === "number") {
      const step = parseFloat(e.step) > 0 ? parseFloat(e.step) : 1, cur = parseFloat(e.value);
      v = (Number.isFinite(cur) ? cur : 0) + dir * step;
      if (e.hasAttribute("max") && v > parseFloat(e.max)) v = parseFloat(e.max);
      if (e.hasAttribute("min") && v < parseFloat(e.min)) v = parseFloat(e.min);
      v = Number(v.toFixed(Math.max(decimals(step), decimals(cur), 0)));
    } else return false;
    if (putValue(e, v)) e.dispatchEvent(new Event("change", { bubbles: true }));
    return true;
  });
  // The arrow keys on a radio button: the one before or after in its group is checked.
  setHelper("radio", (e, dir) => {
    const g = groupOf(e).filter((r) => !disabledNow(r)), i = g.indexOf(e);
    if (i < 0 || g.length < 2) return false;
    const n = g[(i + dir + g.length) % g.length];
    n.focus(); n.click();
    return true;
  });
  setHelper("wheel", (target, x, y, dx, dy) => target.dispatchEvent(new WheelEvent("wheel", { bubbles: true, cancelable: true, composed: true, view: globalThis, clientX: x, clientY: y, deltaX: dx, deltaY: dy, deltaMode: 0 })));
  setHelper("caret", (target, a, b) => { if (target.setSelectionRange) target.setSelectionRange(a, b); formChanged(target); });
  setHelper("change", (target) => { target.dispatchEvent(new Event("change", { bubbles: true })); });
  setHelper("formMeta", (target) => {
    const base = { disabled: false, readonly: false, checked: false, indeterminate: false, placeholder: "", label: "", value: "", start: 0, end: 0 };
    if (target instanceof HTMLInputElement) {
      const t = typeOf(target), s = clampSel(target);
      let label = "";
      if (t === "submit") label = target.hasAttribute("value") ? target.value : "Submit";
      else if (t === "reset") label = target.hasAttribute("value") ? target.value : "Reset";
      else if (t === "button") label = target.getAttribute("value") || "";
      else if (t === "file") label = "Choose File";
      let fraction = 0;
      if (t === "range") { const lo = target.hasAttribute("min") ? parseFloat(target.min) || 0 : 0, hi = target.hasAttribute("max") ? parseFloat(target.max) : 100, v = parseFloat(target.value); fraction = hi > lo && Number.isFinite(v) ? Math.min(1, Math.max(0, (v - lo) / (hi - lo))) : 0; }
      return Object.assign(base, { fraction, kind: "input", type: t, value: t === "password" ? "\u2022".repeat(target.value.length) : target.value, placeholder: target.placeholder, checked: target.checked, indeterminate: s.indeterminate, label, start: s.start, end: s.end, disabled: disabledNow(target), readonly: target.readOnly });
    }
    if (target instanceof HTMLTextAreaElement) { const s = clampSel(target); return Object.assign(base, { kind: "textarea", type: "textarea", value: target.value, placeholder: target.placeholder, start: s.start, end: s.end, disabled: disabledNow(target), readonly: target.readOnly }); }
    if (target instanceof HTMLSelectElement) {
      resetSelection(target);
      const list = optionsOf(target), o = list.find((x) => x.selected);
      const size = parseInt(target.getAttribute("size"), 10);
      const rows = size > 0 ? size : target.multiple ? 4 : 1;
      // (A group's heading is a row of its own in a list.)
      const rowsOf = []; for (const c of target.children) { if (c instanceof HTMLOptionElement) rowsOf.push(c); else if (c instanceof HTMLOptGroupElement) { rowsOf.push(c); for (const x of c.children) if (x instanceof HTMLOptionElement) rowsOf.push(x); } }
      const packed = rowsOf.map((x) => (x instanceof HTMLOptGroupElement ? 4 | (x.hasAttribute("disabled") ? 2 : 0) : (x.selected ? 1 : 0) | (x.hasAttribute("disabled") || (x.parentNode instanceof HTMLOptGroupElement && x.parentNode.hasAttribute("disabled")) ? 2 : 0) | (x.parentNode instanceof HTMLOptGroupElement ? 8 : 0)) + "\x1f" + (x instanceof HTMLOptGroupElement ? x.getAttribute("label") || "" : x.label)).join("\x1e");
      const st = stateOf(target);
      return Object.assign(base, { kind: "select", type: target.type, value: target.value, label: o ? o.label : "", disabled: disabledNow(target), rows, top: st.top || 0, multiple: target.multiple, options: packed });
    }
    if (target instanceof HTMLButtonElement) return Object.assign(base, { kind: "button", type: target.type, disabled: disabledNow(target) });
    return null;
  });
  delete globalThis.__solarSetHelper;
  delete globalThis.__solarFormChanged;
  delete globalThis.__solarSubmitPost;
})();
)JS";

}  // namespace

// Defined here so JsHtml.cpp can evaluate it without knowing its size.
const char* FormScript() { return kFormScript; }

}  // namespace solar::html
