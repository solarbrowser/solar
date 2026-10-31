// What the controls of a form do with their values (https://html.spec.whatwg.org/multipage/input.html) and the constraint validation of them
// (https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#constraint-validation). This is script, part of the one in JsForms.cpp (it is put inside it,
// to use the state of the controls it keeps): the value sanitization of each type, valueAsNumber and valueAsDate, stepUp and stepDown, what a change of type does to the
// value, ValidityState, checkValidity and reportValidity of controls, fieldsets and forms, and the validation a submission does.
const char* const kValidityScript = R"JS(
  // ---- Numbers and dates ----
  const FLOAT = /^-?(\d+|\d*\.\d+)([eE][-+]?\d+)?$/;
  const parseFloatStrict = (s) => (FLOAT.test(s) ? Number(s) : NaN);
  const daysIn = (y, m) => (m === 2 ? ((y % 4 === 0 && y % 100 !== 0) || y % 400 === 0 ? 29 : 28) : [4, 6, 9, 11].includes(m) ? 30 : 31);
  const utcMs = (y, m, d, h = 0, mi = 0, s = 0, ms = 0) => { const t = new Date(0); t.setUTCFullYear(y, m - 1, d); t.setUTCHours(h, mi, s, ms); return t.getTime(); };
  const weeksIn = (y) => { const jan1 = new Date(0); jan1.setUTCFullYear(y, 0, 1); const dow = jan1.getUTCDay(); return dow === 4 || (dow === 3 && daysIn(y, 2) === 29) ? 53 : 52; };
  const MAX_YEAR = 275760;
  const pad = (n, w) => String(n).padStart(w, "0");
  const parseDateStr = (s) => { const m = /^(\d{4,})-(\d\d)-(\d\d)$/.exec(s); if (!m) return null; const y = +m[1], mo = +m[2], d = +m[3]; if (y < 1 || y > MAX_YEAR || mo < 1 || mo > 12 || d < 1 || d > daysIn(y, mo)) return null; return { y, mo, d }; };
  const parseTimeStr = (s) => { const m = /^(\d\d):(\d\d)(?::(\d\d)(?:\.(\d{1,3}))?)?$/.exec(s); if (!m) return null; const h = +m[1], mi = +m[2], se = m[3] === undefined ? 0 : +m[3]; if (h > 23 || mi > 59 || se > 59) return null; const ms = m[4] === undefined ? 0 : +(m[4] + "00").slice(0, 3); return { h, mi, se, ms }; };
  const toNumber = {
    date: (s) => { const p = parseDateStr(s); return p ? utcMs(p.y, p.mo, p.d) : NaN; },
    month: (s) => { const m = /^(\d{4,})-(\d\d)$/.exec(s); if (!m) return NaN; const y = +m[1], mo = +m[2]; if (y < 1 || y > MAX_YEAR || mo < 1 || mo > 12) return NaN; return (y - 1970) * 12 + mo - 1; },
    week: (s) => { const m = /^(\d{4,})-W(\d\d)$/.exec(s); if (!m) return NaN; const y = +m[1], w = +m[2]; if (y < 1 || y > MAX_YEAR || w < 1 || w > weeksIn(y)) return NaN; const jan4 = utcMs(y, 1, 4); const dow = (new Date(jan4).getUTCDay() + 6) % 7; return jan4 - dow * 86400000 + (w - 1) * 604800000; },
    time: (s) => { const p = parseTimeStr(s); return p ? ((p.h * 60 + p.mi) * 60 + p.se) * 1000 + p.ms : NaN; },
    "datetime-local": (s) => { const m = /^(.+?)[T ](.+)$/.exec(s); if (!m) return NaN; const d = parseDateStr(m[1]), t = parseTimeStr(m[2]); return d && t ? utcMs(d.y, d.mo, d.d, t.h, t.mi, t.se, t.ms) : NaN; },
    number: parseFloatStrict, range: parseFloatStrict,
  };
  const timeString = (ms, force) => {
    const h = Math.floor(ms / 3600000) % 24, mi = Math.floor(ms / 60000) % 60, se = Math.floor(ms / 1000) % 60, f = ms % 1000;
    let out = pad(h, 2) + ":" + pad(mi, 2);
    if (se || f || force) out += ":" + pad(se, 2);
    if (f) out += "." + pad(f, 3).replace(/0+$/, "");
    return out;
  };
  const dateString = (ms) => { const d = new Date(ms); return pad(d.getUTCFullYear(), 4) + "-" + pad(d.getUTCMonth() + 1, 2) + "-" + pad(d.getUTCDate(), 2); };
  const fromNumber = {
    date: (n) => dateString(n),
    month: (n) => { const y = Math.floor(n / 12) + 1970, m = ((n % 12) + 12) % 12 + 1; return pad(y, 4) + "-" + pad(m, 2); },
    week: (n) => { const d = new Date(n + 3 * 86400000); const y0 = d.getUTCFullYear(); const jan1 = utcMs(y0, 1, 1); const week = Math.floor((d.getTime() - jan1) / 86400000 / 7) + 1; const thursday = d; const doy = Math.floor((thursday.getTime() - jan1) / 86400000); return pad(y0, 4) + "-W" + pad(Math.floor(doy / 7) + 1, 2); },
    time: (n) => timeString(n, false),
    "datetime-local": (n) => dateString(n) + "T" + timeString(((n % 86400000) + 86400000) % 86400000, false),
    number: (n) => String(n), range: (n) => String(n),
  };
  const isDateType = (t) => t === "date" || t === "month" || t === "week" || t === "time" || t === "datetime-local";
  const stepApplies = (t) => isDateType(t) || t === "number" || t === "range";
  const rangeApplies = stepApplies;
  const STEP = { number: [1, 1], range: [1, 1], date: [1, 86400000], month: [1, 1], week: [1, 604800000], time: [60, 1000], "datetime-local": [60, 1000] };
  const textLike = (t) => ["text", "search", "tel", "url", "email", "password"].includes(t);

  // ---- Value sanitization ----
  function sanitizeFull(e, v) {
    const t = typeOf(e);
    v = String(v);
    if (textLike(t)) {
      v = v.replace(/[\r\n]/g, "");
      if (t === "url") v = v.replace(/^[ \t\n\f\r]+|[ \t\n\f\r]+$/g, "");
      if (t === "email") v = e.hasAttribute("multiple") ? v.split(",").map((x) => x.replace(/^[ \t\n\f\r]+|[ \t\n\f\r]+$/g, "")).join(",") : v.replace(/^[ \t\n\f\r]+|[ \t\n\f\r]+$/g, "");
      return v;
    }
    if (t === "number") return FLOAT.test(v) && Number.isFinite(Number(v)) ? v : "";
    if (t === "range") {
      const lo = rangeBound(e, "min", 0), hi0 = rangeBound(e, "max", 100);
      const hi = hi0 < lo ? lo : hi0;
      let n = parseFloatStrict(v);
      if (!Number.isFinite(n)) n = lo + (hi - lo) / 2 - ((lo + (hi - lo) / 2 - lo) % 1 === 0 ? 0 : 0);
      if (n < lo) n = lo; if (n > hi) n = hi;
      const step = allowedStep(e);
      if (step) { const base = stepBase(e); const k = Math.round((n - base) / step); let c = base + k * step; if (c < lo) c += step; if (c > hi) c -= step; if (c >= lo && c <= hi) n = c; }
      return String(Number(n.toPrecision(15)));
    }
    if (isDateType(t)) {
      if (t === "datetime-local") { const n = toNumber[t](v); return Number.isNaN(n) ? "" : fromNumber[t](n); }
      return Number.isNaN(toNumber[t](v)) ? "" : v;
    }
    if (t === "color") return /^#[0-9a-fA-F]{6}$/.test(v) ? v.toLowerCase() : "#000000";
    return v;
  }
  const rangeBound = (e, name, fallback) => { const n = toNumber[typeOf(e)](e.getAttribute(name) || ""); return Number.isFinite(n) ? n : fallback; };
  const allowedStep = (e) => {
    const t = typeOf(e), [def, scale] = STEP[t];
    const a = e.getAttribute("step");
    if (a !== null && a.toLowerCase() === "any") return null;
    let n = parseFloatStrict(a === null ? "" : a);
    if (!(n > 0)) n = def;
    return n * scale;
  };
  const stepBase = (e) => {
    const t = typeOf(e);
    for (const name of ["min", "value"]) { const a = e.getAttribute(name); if (a !== null) { const n = toNumber[t](a); if (Number.isFinite(n)) return n; } }
    return t === "week" ? -259200000 : 0;
  };

  // ---- valueAsNumber, valueAsDate, stepUp, stepDown ----
  const numberOf = (e) => { const t = typeOf(e); if (!stepApplies(t)) return NaN; const v = e.value; if (v === "") return NaN; return toNumber[t](v); };
  const notApplicable = (what) => new DOMException("This input element does not support " + what + ".", "InvalidStateError");
  define(I, "valueAsNumber", function () { return numberOf(this); }, function (v) {
    const t = typeOf(this);
    if (!stepApplies(t)) throw notApplicable("valueAsNumber");
    v = Number(v);
    if (!Number.isFinite(v) && !Number.isNaN(v)) throw new TypeError("Failed to set the 'valueAsNumber' property on 'HTMLInputElement': The value provided is not finite.");
    this.value = Number.isNaN(v) ? "" : fromNumber[t](t === "time" || t === "datetime-local" || t === "date" || t === "week" ? Math.trunc(v) : v);
  });
  define(I, "valueAsDate", function () {
    const t = typeOf(this);
    if (!(t === "date" || t === "month" || t === "week" || t === "time")) { if (isDateType(t) || t === "number" || t === "range") return null; return null; }
    const n = numberOf(this);
    if (Number.isNaN(n)) return null;
    return new Date(t === "month" ? utcMs(Math.floor(n / 12) + 1970, ((n % 12) + 12) % 12 + 1, 1) : n);
  }, function (v) {
    const t = typeOf(this);
    if (!(t === "date" || t === "month" || t === "week" || t === "time")) throw notApplicable("valueAsDate");
    if (v !== null && v !== undefined && !(v instanceof Date)) throw new TypeError("Failed to set the 'valueAsDate' property on 'HTMLInputElement': The provided value is not a Date.");
    if (v === null || v === undefined || Number.isNaN(v.getTime())) { this.value = ""; return; }
    const ms = v.getTime();
    this.value = t === "month" ? fromNumber.month((v.getUTCFullYear() - 1970) * 12 + v.getUTCMonth()) : t === "time" ? timeString(((ms % 86400000) + 86400000) % 86400000, false) : fromNumber[t](ms);
  });
  const stepBy = (e, count, direction) => {
    const t = typeOf(e);
    if (!stepApplies(t)) throw notApplicable("stepUp/stepDown");
    const step = allowedStep(e);
    if (step === null) throw new DOMException("The element has no allowed value step.", "InvalidStateError");
    const min = e.hasAttribute("min") ? toNumber[t](e.getAttribute("min")) : NaN, max = e.hasAttribute("max") ? toNumber[t](e.getAttribute("max")) : NaN;
    const hasMin = Number.isFinite(min), hasMax = Number.isFinite(max);
    if (hasMin && hasMax && min > max) return;
    const base = stepBase(e);
    const smallestInRange = hasMin ? base + Math.ceil((min - base) / step - 1e-9) * step : -Infinity;
    if (hasMin && hasMax && smallestInRange > max) return;
    let value = numberOf(e);
    if (Number.isNaN(value)) value = 0;
    const before = value;
    const delta = direction * count * step;
    const q = (value - base) / step;
    const aligned = Math.abs(q - Math.round(q)) < 1e-9;
    if (!aligned) {
      value = direction > 0 ? base + Math.ceil(q) * step : base + Math.floor(q) * step;
      if (count !== 1) value += (count - 1) * direction * step;
    } else value += delta;
    if (hasMax && value > max) value = base + Math.floor((max - base) / step + 1e-9) * step;
    if (hasMin && value < min) value = base + Math.ceil((min - base) / step - 1e-9) * step;
    if ((direction > 0 && value < before) || (direction < 0 && value > before)) return;
    e.value = fromNumber[t](t === "number" || t === "range" ? Number(value.toPrecision(15)) : value);
  };
  method(I, "stepUp", function (n = 1) { stepBy(this, Math.trunc(Number(n)) || 0, 1); });
  method(I, "stepDown", function (n = 1) { stepBy(this, Math.trunc(Number(n)) || 0, -1); });
  Object.defineProperty(I.stepUp, "length", { value: 0, configurable: true });
  Object.defineProperty(I.stepDown, "length", { value: 0, configurable: true });

  // ---- A change of type ----
  const modeOf = (t) => (t === "checkbox" || t === "radio" ? "default/on" : ["hidden", "submit", "image", "reset", "button"].includes(t) ? "default" : t === "file" ? "filename" : "value");
  const typeChanged = (e, from, to) => {
    if (from === to) return;
    const s = stateOf(e), before = modeOf(from), after = modeOf(to);
    if (before === "value" && (after === "default" || after === "default/on")) {
      const v = s.dirty ? s.value : e.getAttribute("value") || "";
      if (v !== "") Reflect.apply(Element.prototype.setAttribute, e, ["value", v]);
      s.dirty = false;
    } else if ((before === "default" || before === "default/on") && after === "value") {
      s.value = e.getAttribute("value") || "";
      s.dirty = true;
    } else if (before === "filename" && after !== "filename") { s.value = ""; s.dirty = false; }
    if (after === "value") { s.value = sanitizeFull(e, s.dirty ? s.value : e.getAttribute("value") || ""); }
    // (a value that was set stays the dirty value; one from the attribute is sanitized when it is read)
    s.start = Math.min(s.start, s.value.length); s.end = Math.min(s.end, s.value.length);
    formChanged(e);
  };
  for (const name of ["setAttribute", "removeAttribute", "toggleAttribute"]) {
    const original = Element.prototype[name];
    const wrapper = ({ [name](...args) {
      if (this instanceof HTMLInputElement && String(args[0]).toLowerCase() === "type") {
        const from = typeOf(this);
        const result = Reflect.apply(original, this, args);
        typeChanged(this, from, typeOf(this));
        return result;
      }
      return Reflect.apply(original, this, args);
    } })[name];
    Object.defineProperty(wrapper, "length", { value: original.length, configurable: true });
    Object.defineProperty(Element.prototype, name, { value: wrapper, writable: true, configurable: true, enumerable: true });
  }

  // ---- Constraint validation ----
  const customMessages = new WeakMap();
  const barredType = (t) => t === "hidden" || t === "reset" || t === "button" || t === "image";
  const inDatalist = (e) => { for (let p = e.parentNode; p; p = p.parentNode) if (p instanceof HTMLDataListElement) return true; return false; };
  const willValidate = (e) => {
    if (e instanceof HTMLInputElement) { const t = typeOf(e); if (barredType(t) || (e.hasAttribute("readonly") && !["checkbox", "radio", "range", "color", "file"].includes(t))) return false; }
    else if (e instanceof HTMLTextAreaElement) { if (e.hasAttribute("readonly")) return false; }
    else if (e instanceof HTMLButtonElement) { if (e.type !== "submit") return false; }
    else if (e instanceof HTMLSelectElement) { /* disabled only */ }
    else if (e instanceof HTMLOutputElement || e instanceof HTMLObjectElement || e instanceof HTMLFieldSetElement) return false;
    return !disabledNow(e) && !inDatalist(e);
  };
  const emailOk = (v) => /^[a-zA-Z0-9.!#$%&'*+\/=?^_`{|}~-]+@[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?(?:\.[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)*$/.test(v);
  const flagsOf = (e) => {
    const f = { valueMissing: false, typeMismatch: false, patternMismatch: false, tooLong: false, tooShort: false, rangeUnderflow: false, rangeOverflow: false, stepMismatch: false, badInput: false, customError: false };
    const custom = customMessages.get(e);
    if (custom) f.customError = true;
    if (!willValidate(e)) return f;
    if (e instanceof HTMLInputElement) {
      const t = typeOf(e), v = e.value;
      if (e.hasAttribute("required")) {
        if (t === "checkbox") f.valueMissing = !e.checked;
        else if (t === "radio") f.valueMissing = !groupOf(e).some((r) => r.checked);
        else if (t === "file") f.valueMissing = true;
        else if (!barredType(t) && t !== "color" && t !== "range") f.valueMissing = v === "";
      }
      if (t === "email" && v !== "") f.typeMismatch = e.hasAttribute("multiple") ? v.split(",").some((x) => !emailOk(x)) : !emailOk(v);
      if (t === "url" && v !== "") { try { new URL(v); } catch (err) { f.typeMismatch = true; } }
      if (["text", "search", "url", "tel", "email", "password"].includes(t) && e.hasAttribute("pattern") && v !== "") {
        let re = null;
        try { re = new RegExp("^(?:" + e.getAttribute("pattern") + ")$", "v"); } catch (err) { re = null; }
        if (re) f.patternMismatch = e.hasAttribute("multiple") && t === "email" ? v.split(",").some((x) => !re.test(x)) : !re.test(v);
      }
      if (rangeApplies(t) && v !== "") {
        const n = toNumber[t](v);
        if (Number.isFinite(n)) {
          const lo = e.hasAttribute("min") ? toNumber[t](e.getAttribute("min")) : NaN, hi = e.hasAttribute("max") ? toNumber[t](e.getAttribute("max")) : NaN;
          const hasLo = Number.isFinite(lo), hasHi = Number.isFinite(hi);
          if (hasLo && hasHi && lo > hi) { /* a time range wraps; the others have none */
            if (t === "time") { if (n < lo && n > hi) { f.rangeUnderflow = true; f.rangeOverflow = true; } }
          } else { if (hasLo && n < lo) f.rangeUnderflow = true; if (hasHi && n > hi) f.rangeOverflow = true; }
          if (stepApplies(t)) { const step = allowedStep(e); if (step !== null) { const q = (n - stepBase(e)) / step; if (Math.abs(q - Math.round(q)) > 1e-9) f.stepMismatch = true; } }
        }
      }
    } else if (e instanceof HTMLTextAreaElement) {
      if (e.hasAttribute("required") && e.value === "") f.valueMissing = true;
    } else if (e instanceof HTMLSelectElement) {
      if (e.hasAttribute("required")) {
        const list = Array.from(e.options), multiple = e.hasAttribute("multiple"), size = parseInt(e.getAttribute("size"), 10) || (multiple ? 4 : 1);
        const selected = list.filter((o) => o.selected);
        if (multiple || size > 1) f.valueMissing = selected.length === 0;
        else { const placeholder = list[0] && list[0].value === "" && list[0].parentNode === e ? list[0] : null; f.valueMissing = selected.length === 0 || (selected.length === 1 && selected[0] === placeholder); }
      }
    }
    return f;
  };
  const MESSAGES = { valueMissing: "Please fill out this field.", typeMismatch: "Please enter a valid value.", patternMismatch: "Please match the requested format.", tooLong: "Please shorten this text.", tooShort: "Please lengthen this text.",
    rangeUnderflow: "Value must be greater than or equal to the minimum.", rangeOverflow: "Value must be less than or equal to the maximum.", stepMismatch: "Please enter a valid value.", badInput: "Please enter a valid value." };
  const validityStates = new WeakMap();
  class ValidityState {
    constructor(token, element) { if (token !== validityStates) throw new TypeError("Illegal constructor"); Object.defineProperty(this, "__element", { value: element }); }
  }
  for (const name of ["valueMissing", "typeMismatch", "patternMismatch", "tooLong", "tooShort", "rangeUnderflow", "rangeOverflow", "stepMismatch", "badInput", "customError"]) {
    Object.defineProperty(ValidityState.prototype, name, { get() { return flagsOf(this.__element)[name]; }, enumerable: true, configurable: true });
  }
  Object.defineProperty(ValidityState.prototype, "valid", { get() { const f = flagsOf(this.__element); return !Object.values(f).some(Boolean); }, enumerable: true, configurable: true });
  Object.defineProperty(ValidityState.prototype, Symbol.toStringTag, { value: "ValidityState", configurable: true });
  Object.defineProperty(globalThis, "ValidityState", { value: ValidityState, writable: true, configurable: true, enumerable: false });
  const validityOf = (e) => { let v = validityStates.get(e); if (!v) { v = new ValidityState(validityStates, e); validityStates.set(e, v); } return v; };
  const invalidNow = (e) => willValidate(e) && Object.values(flagsOf(e)).some(Boolean);
  const checkOne = (e) => {
    if (!willValidate(e)) return true;
    if (!invalidNow(e)) return true;
    e.dispatchEvent(new Event("invalid", { cancelable: true }));
    return false;
  };
  for (const proto of [I, T, B, S, HTMLFieldSetElement.prototype, HTMLOutputElement.prototype, HTMLObjectElement.prototype]) {
    if (!proto) continue;
    define(proto, "willValidate", function () { return willValidate(this); });
    define(proto, "validity", function () { return validityOf(this); });
    define(proto, "validationMessage", function () {
      if (!willValidate(this)) return "";
      const custom = customMessages.get(this);
      if (custom) return custom;
      const f = flagsOf(this);
      for (const k of Object.keys(MESSAGES)) if (f[k]) return MESSAGES[k];
      return "";
    });
    method(proto, "setCustomValidity", function (message) { const m = String(message); if (m === "") customMessages.delete(this); else customMessages.set(this, m); });
    if (proto === HTMLFieldSetElement.prototype) {
      method(proto, "checkValidity", function () { let ok = true; for (const c of this.querySelectorAll("input, button, select, textarea, output, object")) if (c.form === this.form && !checkOne(c)) ok = false; return ok; });
      method(proto, "reportValidity", function () { return this.checkValidity(); });
    } else {
      method(proto, "checkValidity", function () { return checkOne(this); });
      method(proto, "reportValidity", function () { return checkOne(this); });
    }
  }
  const validateForm = (form) => {
    const invalid = [];
    for (const e of controlsOf(form)) { if (willValidate(e) && invalidNow(e)) invalid.push(e); }
    for (const e of invalid) e.dispatchEvent(new Event("invalid", { cancelable: true }));
    return invalid.length === 0;
  };
  method(F, "checkValidity", function () { return validateForm(this); });
  method(F, "reportValidity", function () { return validateForm(this); });
  const originalRequestSubmit = F.requestSubmit;
  method(F, "requestSubmit", function (submitter) {
    // A submission validates the form first, unless the form or the button that submits it says not to.
    if (submitter !== undefined && submitter !== null && submitter instanceof HTMLElement && submitter.form === this && !this.hasAttribute("novalidate") && !submitter.hasAttribute("formnovalidate") && !validateForm(this)) return;
    if ((submitter === undefined || submitter === null) && !this.hasAttribute("novalidate") && !validateForm(this)) return;
    return originalRequestSubmit.call(this, submitter);
  });
  Object.defineProperty(F.requestSubmit, "length", { value: 0, configurable: true });
)JS";
