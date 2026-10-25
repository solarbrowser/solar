// The IDL attributes of the HTML elements that are only a content attribute seen through a property (href, alt,
// colSpan, open ...), the URL decomposition members of a and area, and the table interfaces' own methods. They are
// written once, from a table of what each interface reflects, and put on the prototypes that do not have them.

namespace solar::html {

namespace {

const char* const kReflectScript = R"JS(
(function () {
  const define = (target, name, descriptor) => Object.defineProperty(target, name, { enumerable: true, configurable: true, ...descriptor });
  const proto = (name) => (typeof globalThis[name] === "function" ? globalThis[name].prototype : null);
  const has = (target, name) => Object.prototype.hasOwnProperty.call(target, name);
  const numberOr = (text, fallback) => { const n = Number.parseInt(text, 10); return Number.isFinite(n) ? n : fallback; };
  const floatOr = (text, fallback) => { const n = Number.parseFloat(text); return Number.isFinite(n) ? n : fallback; };
  const baseOf = (element) => { try { return element.baseURI || document.URL; } catch (e) { return document.URL; } };
  const resolve = (element, value) => { try { return new URL(value, baseOf(element)).href; } catch (e) { return value; } };

  // Kinds: s string, u URL (resolved against the document), b boolean, l long, ul unsigned long, f double, n nullable string.
  const reflectors = {
    s: (attr) => ({ get() { const v = this.getAttribute(attr); return v === null ? "" : v; }, set(v) { this.setAttribute(attr, String(v)); } }),
    n: (attr) => ({ get() { return this.getAttribute(attr); }, set(v) { if (v === null) this.removeAttribute(attr); else this.setAttribute(attr, String(v)); } }),
    u: (attr) => ({ get() { const v = this.getAttribute(attr); return v === null ? "" : resolve(this, v); }, set(v) { this.setAttribute(attr, String(v)); } }),
    b: (attr) => ({ get() { return this.hasAttribute(attr); }, set(v) { if (v) this.setAttribute(attr, ""); else this.removeAttribute(attr); } }),
    l: (attr, fallback = 0) => ({ get() { return numberOr(this.getAttribute(attr), fallback); }, set(v) { this.setAttribute(attr, String(Math.trunc(Number(v)) || 0)); } }),
    ul: (attr, fallback = 0) => ({ get() { const n = numberOr(this.getAttribute(attr), fallback); return n < 0 ? fallback : n; }, set(v) { this.setAttribute(attr, String((Number(v) >>> 0))); } }),
    f: (attr, fallback = 0) => ({ get() { return floatOr(this.getAttribute(attr), fallback); }, set(v) { this.setAttribute(attr, String(Number(v))); } }),
  };
  const reflect = (interfaceName, members) => {
    const target = proto(interfaceName);
    if (!target) return;
    for (const [name, kind, attr = name.toLowerCase(), fallback] of members) {
      if (has(target, name)) continue;
      define(target, name, reflectors[kind](attr, fallback));
    }
  };

  reflect("HTMLElement", [
    ["nonce", "s"], ["autofocus", "b"], ["popover", "n"], ["writingSuggestions", "s"],
  ]);
  if (proto("HTMLElement") && !has(proto("HTMLElement"), "contentEditable")) {
    define(proto("HTMLElement"), "contentEditable", { get() { const v = this.getAttribute("contenteditable"); return v === null ? "inherit" : v === "" ? "true" : v.toLowerCase(); }, set(v) { this.setAttribute("contenteditable", String(v)); } });
  }
  if (proto("HTMLElement") && !has(proto("HTMLElement"), "isContentEditable")) {
    define(proto("HTMLElement"), "isContentEditable", { get() { for (let e = this; e && e.nodeType === 1; e = e.parentElement) { const v = e.getAttribute("contenteditable"); if (v !== null) return v === "" || v.toLowerCase() === "true" || v.toLowerCase() === "plaintext-only"; } return false; } });
  }
  // ARIA reflection: ariaLabel and the others are the aria-* attributes.
  if (proto("Element")) {
    for (const name of ["Atomic", "AutoComplete", "Busy", "Checked", "ColCount", "ColIndex", "ColSpan", "Current", "Description", "Disabled", "Expanded", "HasPopup", "Hidden", "Invalid", "KeyShortcuts", "Label", "Level", "Live", "Modal", "MultiLine", "MultiSelectable", "Orientation", "Placeholder", "PosInSet", "Pressed", "ReadOnly", "Relevant", "Required", "RoleDescription", "RowCount", "RowIndex", "RowSpan", "Selected", "SetSize", "Sort", "ValueMax", "ValueMin", "ValueNow", "ValueText"]) {
      const property = "aria" + name;
      const attribute = "aria-" + name.toLowerCase();
      if (!has(proto("Element"), property)) define(proto("Element"), property, reflectors.n(attribute));
    }
    if (!has(proto("Element"), "role")) define(proto("Element"), "role", reflectors.n("role"));
    if (!has(proto("Element"), "part")) define(proto("Element"), "part", { get() { return this.getAttribute("part") || ""; }, set(v) { this.setAttribute("part", String(v)); } });
  }

  // a and area: the URL of the link, and its parts.
  const linkUrl = (element) => {
    const value = element.getAttribute("href");
    if (value === null) return null;
    try { return new URL(value, baseOf(element)); } catch (e) { return null; }
  };
  const utils = {
    href: { get() { const v = this.getAttribute("href"); if (v === null) return ""; const url = linkUrl(this); return url ? url.href : v; }, set(v) { this.setAttribute("href", String(v)); } },
    origin: { get() { const url = linkUrl(this); return url ? url.origin : ""; } },
    protocol: { get() { const url = linkUrl(this); return url ? url.protocol : ":"; }, set(v) { const url = linkUrl(this); if (url) { url.protocol = v; this.setAttribute("href", url.href); } } },
  };
  for (const part of ["username", "password", "host", "hostname", "port", "pathname", "search", "hash"]) {
    utils[part] = { get() { const url = linkUrl(this); return url ? url[part] : ""; }, set(v) { const url = linkUrl(this); if (url) { url[part] = v; this.setAttribute("href", url.href); } } };
  }
  for (const interfaceName of ["HTMLAnchorElement", "HTMLAreaElement"]) {
    const target = proto(interfaceName);
    if (!target) continue;
    for (const [name, descriptor] of Object.entries(utils)) if (!has(target, name)) define(target, name, descriptor);
    if (!has(target, "toString")) Object.defineProperty(target, "toString", { value: function toString() { return this.href; }, writable: true, enumerable: true, configurable: true });
  }
  reflect("HTMLAnchorElement", [["target", "s"], ["download", "s"], ["ping", "s"], ["rel", "s"], ["hreflang", "s"], ["type", "s"], ["referrerPolicy", "s", "referrerpolicy"], ["name", "s"], ["charset", "s"], ["coords", "s"], ["rev", "s"], ["shape", "s"]]);
  if (proto("HTMLAnchorElement") && !has(proto("HTMLAnchorElement"), "text")) define(proto("HTMLAnchorElement"), "text", { get() { return this.textContent; }, set(v) { this.textContent = v; } });
  reflect("HTMLAreaElement", [["alt", "s"], ["coords", "s"], ["shape", "s"], ["target", "s"], ["download", "s"], ["ping", "s"], ["rel", "s"], ["referrerPolicy", "s", "referrerpolicy"], ["noHref", "b", "nohref"]]);

  reflect("HTMLBaseElement", [["href", "u"], ["target", "s"]]);
  reflect("HTMLMetaElement", [["name", "s"], ["httpEquiv", "s", "http-equiv"], ["content", "s"], ["media", "s"], ["scheme", "s"]]);
  if (proto("HTMLTitleElement") && !has(proto("HTMLTitleElement"), "text")) define(proto("HTMLTitleElement"), "text", { get() { return this.textContent; }, set(v) { this.textContent = v; } });

  reflect("HTMLImageElement", [
    ["alt", "s"], ["src", "u"], ["srcset", "s"], ["sizes", "s"], ["crossOrigin", "n", "crossorigin"], ["useMap", "s", "usemap"], ["isMap", "b", "ismap"],
    ["decoding", "s"], ["loading", "s"], ["referrerPolicy", "s", "referrerpolicy"], ["fetchPriority", "s", "fetchpriority"], ["name", "s"], ["longDesc", "s", "longdesc"],
    ["align", "s"], ["border", "s"], ["hspace", "ul"], ["vspace", "ul"], ["lowsrc", "u"],
  ]);
  if (proto("HTMLImageElement")) {
    const image = proto("HTMLImageElement");
    for (const [name, descriptor] of Object.entries({
      width: { get() { const rect = this.getBoundingClientRect(); return rect.width || numberOr(this.getAttribute("width"), 0); }, set(v) { this.setAttribute("width", String(Number(v) >>> 0)); } },
      height: { get() { const rect = this.getBoundingClientRect(); return rect.height || numberOr(this.getAttribute("height"), 0); }, set(v) { this.setAttribute("height", String(Number(v) >>> 0)); } },
      naturalWidth: { get() { return 0; } },
      naturalHeight: { get() { return 0; } },
      complete: { get() { const src = this.getAttribute("src"); return (src === null || src === "") && this.getAttribute("srcset") === null; } },
      currentSrc: { get() { const src = this.getAttribute("src"); return src ? resolve(this, src) : ""; } },
      x: { get() { let x = 0; for (let e = this; e; e = e.offsetParent) x += e.offsetLeft; return x; } },
      y: { get() { let y = 0; for (let e = this; e; e = e.offsetParent) y += e.offsetTop; return y; } },
    })) if (!has(image, name)) define(image, name, descriptor);
    if (!has(image, "decode")) Object.defineProperty(image, "decode", { value: function decode() { return Promise.resolve(); }, writable: true, enumerable: true, configurable: true });
  }

  reflect("HTMLScriptElement", [["async", "b"], ["crossOrigin", "n", "crossorigin"], ["referrerPolicy", "s", "referrerpolicy"], ["htmlFor", "s", "for"], ["event", "s"], ["blocking", "s"]]);
  reflect("HTMLDetailsElement", [["open", "b"], ["name", "s"]]);
  reflect("HTMLSourceElement", [["src", "u"], ["type", "s"], ["srcset", "s"], ["sizes", "s"], ["media", "s"], ["width", "ul"], ["height", "ul"]]);
  reflect("HTMLTrackElement", [["kind", "s"], ["src", "u"], ["srclang", "s"], ["label", "s"], ["default", "b"]]);
  reflect("HTMLMediaElement", [["src", "u"], ["crossOrigin", "n", "crossorigin"], ["preload", "s"], ["autoplay", "b"], ["loop", "b"], ["controls", "b"], ["defaultMuted", "b", "muted"], ["disableRemotePlayback", "b"]]);
  reflect("HTMLVideoElement", [["poster", "u"], ["width", "ul"], ["height", "ul"], ["playsInline", "b", "playsinline"]]);
  if (proto("HTMLVideoElement")) for (const name of ["videoWidth", "videoHeight"]) if (!has(proto("HTMLVideoElement"), name)) define(proto("HTMLVideoElement"), name, { get() { return 0; } });
  reflect("HTMLObjectElement", [["data", "u"], ["type", "s"], ["name", "s"], ["useMap", "s", "usemap"], ["width", "s"], ["height", "s"]]);
  reflect("HTMLEmbedElement", [["src", "u"], ["type", "s"], ["width", "s"], ["height", "s"]]);
  reflect("HTMLProgressElement", [["max", "f", "max", 1]]);
  reflect("HTMLMeterElement", [["min", "f", "min", 0], ["max", "f", "max", 1]]);
  reflect("HTMLOListElement", [["reversed", "b"], ["start", "l", "start", 1], ["type", "s"]]);
  reflect("HTMLLIElement", [["value", "l"]]);
  reflect("HTMLQuoteElement", [["cite", "u"]]);
  reflect("HTMLModElement", [["cite", "u"], ["dateTime", "s", "datetime"]]);
  reflect("HTMLTimeElement", [["dateTime", "s", "datetime"]]);
  reflect("HTMLDataElement", [["value", "s"]]);
  reflect("HTMLOptGroupElement", [["disabled", "b"], ["label", "s"]]);
  reflect("HTMLFieldSetElement", [["disabled", "b"], ["name", "s"]]);
  reflect("HTMLMapElement", [["name", "s"]]);
  reflect("HTMLTableColElement", [["span", "ul", "span", 1]]);
  reflect("HTMLTableCellElement", [["colSpan", "ul", "colspan", 1], ["rowSpan", "ul", "rowspan", 1], ["headers", "s"], ["abbr", "s"], ["scope", "s"], ["align", "s"], ["vAlign", "s", "valign"], ["width", "s"], ["height", "s"], ["noWrap", "b", "nowrap"], ["bgColor", "s", "bgcolor"]]);
  reflect("HTMLTableRowElement", [["align", "s"], ["vAlign", "s", "valign"], ["bgColor", "s", "bgcolor"]]);
  reflect("HTMLTableSectionElement", [["align", "s"], ["vAlign", "s", "valign"]]);
  reflect("HTMLTableElement", [["align", "s"], ["border", "s"], ["frame", "s"], ["rules", "s"], ["summary", "s"], ["width", "s"], ["bgColor", "s", "bgcolor"], ["cellPadding", "s", "cellpadding"], ["cellSpacing", "s", "cellspacing"]]);
  reflect("HTMLDivElement", [["align", "s"]]);
  reflect("HTMLParagraphElement", [["align", "s"]]);
  reflect("HTMLHeadingElement", [["align", "s"]]);
  reflect("HTMLHRElement", [["align", "s"], ["color", "s"], ["noShade", "b", "noshade"], ["size", "s"], ["width", "s"]]);
  reflect("HTMLBodyElement", [["text", "s"], ["link", "s"], ["vLink", "s", "vlink"], ["aLink", "s", "alink"], ["bgColor", "s", "bgcolor"], ["background", "s"]]);
  reflect("HTMLHtmlElement", [["version", "s"]]);
  reflect("HTMLIFrameElement", [["sandbox", "s"], ["scrolling", "s"], ["frameBorder", "s", "frameborder"], ["align", "s"], ["longDesc", "s", "longdesc"], ["marginHeight", "s", "marginheight"], ["marginWidth", "s", "marginwidth"]]);

  // The tables: their parts, and the methods that make rows and cells.
  const childrenNamed = (parent, name) => Array.prototype.filter.call(parent.children, (child) => child.localName === name && child.namespaceURI === "http://www.w3.org/1999/xhtml");
  const firstNamed = (parent, name) => childrenNamed(parent, name)[0] || null;
  const makeCollection = (items) => Object.assign(items, { item: (i) => items[i] || null, namedItem: (name) => items.find((e) => e.id === name || e.getAttribute("name") === name) || null });
  const tableRows = (table) => {
    const rows = [];
    for (const child of table.children) {
      if (child.localName === "thead") rows.push(...childrenNamed(child, "tr"));
    }
    for (const child of table.children) {
      if (child.localName === "tr") rows.push(child);
      else if (child.localName === "tbody") rows.push(...childrenNamed(child, "tr"));
    }
    for (const child of table.children) {
      if (child.localName === "tfoot") rows.push(...childrenNamed(child, "tr"));
    }
    return rows;
  };
  const indexError = () => new DOMException("The index provided is out of range.", "IndexSizeError");
  const insertRowInto = (parent, rows, index) => {
    if (index < -1 || index > rows.length) throw indexError();
    const row = parent.ownerDocument.createElement("tr");
    if (index === -1 || index === rows.length) parent.appendChild(row);
    else rows[index].parentNode.insertBefore(row, rows[index]);
    return row;
  };
  const deleteRowFrom = (rows, index) => {
    if (index < -1 || index >= rows.length) throw indexError();
    if (rows.length === 0) return;
    (index === -1 ? rows[rows.length - 1] : rows[index]).remove();
  };
  const methods = (target, members) => {
    if (!target) return;
    for (const [name, descriptor] of Object.entries(members)) if (!has(target, name)) Object.defineProperty(target, name, { enumerable: true, configurable: true, ...(typeof descriptor === "function" ? { value: descriptor, writable: true } : descriptor) });
  };
  methods(proto("HTMLTableElement"), {
    caption: { get() { return firstNamed(this, "caption"); }, set(v) { const old = firstNamed(this, "caption"); if (old) old.remove(); if (v) this.insertBefore(v, this.firstChild); } },
    tHead: { get() { return firstNamed(this, "thead"); }, set(v) { const old = firstNamed(this, "thead"); if (old) old.remove(); if (v) this.insertBefore(v, this.firstElementChild && this.firstElementChild.localName === "caption" ? this.firstElementChild.nextSibling : this.firstChild); } },
    tFoot: { get() { return firstNamed(this, "tfoot"); }, set(v) { const old = firstNamed(this, "tfoot"); if (old) old.remove(); if (v) this.appendChild(v); } },
    tBodies: { get() { return makeCollection(childrenNamed(this, "tbody")); } },
    rows: { get() { return makeCollection(tableRows(this)); } },
    createCaption() { let c = firstNamed(this, "caption"); if (!c) { c = this.ownerDocument.createElement("caption"); this.insertBefore(c, this.firstChild); } return c; },
    deleteCaption() { const c = firstNamed(this, "caption"); if (c) c.remove(); },
    createTHead() { let h = firstNamed(this, "thead"); if (!h) { h = this.ownerDocument.createElement("thead"); this.tHead = h; } return h; },
    deleteTHead() { const h = firstNamed(this, "thead"); if (h) h.remove(); },
    createTFoot() { let f = firstNamed(this, "tfoot"); if (!f) { f = this.ownerDocument.createElement("tfoot"); this.appendChild(f); } return f; },
    deleteTFoot() { const f = firstNamed(this, "tfoot"); if (f) f.remove(); },
    createTBody() { const b = this.ownerDocument.createElement("tbody"); const bodies = childrenNamed(this, "tbody"); if (bodies.length) bodies[bodies.length - 1].after(b); else this.appendChild(b); return b; },
    insertRow(index = -1) {
      index = Number(index) | 0;
      const rows = tableRows(this);
      if (index < -1 || index > rows.length) throw indexError();
      if (rows.length === 0) {
        let body = childrenNamed(this, "tbody").pop();
        if (!body) { body = this.ownerDocument.createElement("tbody"); this.appendChild(body); }
        return insertRowInto(body, [], -1);
      }
      if (index === -1 || index === rows.length) return insertRowInto(rows[rows.length - 1].parentNode, [], -1);
      const row = this.ownerDocument.createElement("tr");
      rows[index].parentNode.insertBefore(row, rows[index]);
      return row;
    },
    deleteRow(index) { deleteRowFrom(tableRows(this), Number(index) | 0); },
  });
  methods(proto("HTMLTableSectionElement"), {
    rows: { get() { return makeCollection(childrenNamed(this, "tr")); } },
    insertRow(index = -1) { const rows = childrenNamed(this, "tr"); index = Number(index) | 0; if (index < -1 || index > rows.length) throw indexError(); const row = this.ownerDocument.createElement("tr"); if (index === -1 || index === rows.length) this.appendChild(row); else this.insertBefore(row, rows[index]); return row; },
    deleteRow(index) { deleteRowFrom(childrenNamed(this, "tr"), Number(index) | 0); },
  });
  const cellsOf = (row) => Array.prototype.filter.call(row.children, (c) => c.localName === "td" || c.localName === "th");
  methods(proto("HTMLTableRowElement"), {
    cells: { get() { return makeCollection(cellsOf(this)); } },
    rowIndex: { get() { const table = this.closest("table"); return table ? tableRows(table).indexOf(this) : -1; } },
    sectionRowIndex: { get() { return this.parentElement ? childrenNamed(this.parentElement, "tr").indexOf(this) : -1; } },
    insertCell(index = -1) { const cells = cellsOf(this); index = Number(index) | 0; if (index < -1 || index > cells.length) throw indexError(); const cell = this.ownerDocument.createElement("td"); if (index === -1 || index === cells.length) this.appendChild(cell); else this.insertBefore(cell, cells[index]); return cell; },
    deleteCell(index) { deleteRowFrom(cellsOf(this), Number(index) | 0); },
  });
  methods(proto("HTMLTableCellElement"), {
    cellIndex: { get() { return this.parentElement ? cellsOf(this.parentElement).indexOf(this) : -1; } },
  });
  methods(proto("HTMLProgressElement"), {
    value: { get() { return floatOr(this.getAttribute("value"), 0); }, set(v) { this.setAttribute("value", String(Number(v))); } },
    position: { get() { if (this.getAttribute("value") === null) return -1; const max = floatOr(this.getAttribute("max"), 1); return max > 0 ? Math.min(Math.max(floatOr(this.getAttribute("value"), 0), 0), max) / max : 0; } },
  });
  methods(proto("HTMLMeterElement"), {
    value: { get() { return floatOr(this.getAttribute("value"), 0); }, set(v) { this.setAttribute("value", String(Number(v))); } },
    low: { get() { return floatOr(this.getAttribute("low"), floatOr(this.getAttribute("min"), 0)); }, set(v) { this.setAttribute("low", String(Number(v))); } },
    high: { get() { return floatOr(this.getAttribute("high"), floatOr(this.getAttribute("max"), 1)); }, set(v) { this.setAttribute("high", String(Number(v))); } },
    optimum: { get() { const min = floatOr(this.getAttribute("min"), 0), max = floatOr(this.getAttribute("max"), 1); return floatOr(this.getAttribute("optimum"), (min + max) / 2); }, set(v) { this.setAttribute("optimum", String(Number(v))); } },
  });
})();
)JS";

}  // namespace

const char* ReflectScript() { return kReflectScript; }

}  // namespace solar::html
