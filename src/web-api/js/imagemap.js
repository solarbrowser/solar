// Image maps (https://html.spec.whatwg.org/multipage/image-maps.html): a point on an image that has a usemap is on the area of its map that holds it, which is what elementFromPoint
// and elementsFromPoint answer. The areas are not drawn; they are the shapes of the shape and coords attributes of the area elements.
(() => {
  "use strict";
  if (typeof HTMLAreaElement !== "function" || typeof document === "undefined") return;
  const hidden = (target, name, value) => Object.defineProperty(target, name, { value, writable: true, configurable: true, enumerable: true });
  // The rules for parsing a list of floating-point numbers: a number begins each run of what is not a delimiter, and a bad one is 0.
  const numbersOf = (text) => {
    const out = [];
    for (const token of text.split(/[ \t\n\f\r,;]+/)) {
      if (token === "") continue;
      const m = /^-?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?/.exec(token);
      out.push(m ? Number(m[0]) : 0);
    }
    return out;
  };
  const shapeOf = (area) => {
    const v = (area.getAttribute("shape") || "").toLowerCase();
    if (v === "circle" || v === "circ") return "circle";
    if (v === "poly" || v === "polygon") return "poly";
    if (v === "default") return "default";
    return "rect";
  };
  const contains = (area, x, y) => {
    const shape = shapeOf(area);
    if (shape === "default") return true;
    const n = numbersOf(area.getAttribute("coords") || "");
    if (shape === "rect") {
      if (n.length < 4) return false;
      let [x1, y1, x2, y2] = n;
      if (x1 > x2) [x1, x2] = [x2, x1];
      if (y1 > y2) [y1, y2] = [y2, y1];
      return x >= x1 && x <= x2 && y >= y1 && y <= y2;
    }
    if (shape === "circle") {
      if (n.length < 3 || n[2] < 0) return false;
      return (x - n[0]) ** 2 + (y - n[1]) ** 2 <= n[2] ** 2;
    }
    if (n.length < 6) return false;
    // A polygon: the non-zero winding rule.
    const pts = [];
    for (let i = 0; i + 1 < n.length; i += 2) pts.push([n[i], n[i + 1]]);
    let winding = 0;
    for (let i = 0; i < pts.length; i++) {
      const [ax, ay] = pts[i], [bx, by] = pts[(i + 1) % pts.length];
      if (ay <= y) { if (by > y && (bx - ax) * (y - ay) - (x - ax) * (by - ay) > 0) winding++; }
      else if (by <= y && (bx - ax) * (y - ay) - (x - ax) * (by - ay) < 0) winding--;
    }
    return winding !== 0;
  };
  const mapOf = (image) => {
    const value = image.getAttribute("usemap");
    if (value === null || value.length < 2 || value[0] !== "#") return null;
    const name = value.slice(1);
    const root = image.getRootNode();
    if (!root || typeof root.querySelectorAll !== "function") return null;
    for (const map of root.querySelectorAll("map")) if (map.getAttribute("name") === name || (!map.hasAttribute("name") && map.getAttribute("id") === name)) return map;
    return null;
  };
  const areaAt = (image, px, py) => {
    const map = mapOf(image);
    if (!map) return null;
    const rect = image.getBoundingClientRect(), style = getComputedStyle(image);
    const x = px - rect.left - (parseFloat(style.borderLeftWidth) || 0) - (parseFloat(style.paddingLeft) || 0);
    const y = py - rect.top - (parseFloat(style.borderTopWidth) || 0) - (parseFloat(style.paddingTop) || 0);
    for (const area of map.querySelectorAll("area")) if (contains(area, x, y)) return area;
    return null;
  };
  for (const C of [Document, typeof ShadowRoot === "function" ? ShadowRoot : null]) {
    if (!C) continue;
    const point = Object.getOwnPropertyDescriptor(C.prototype, "elementFromPoint");
    if (point && typeof point.value === "function") {
      const original = point.value;
      const wrapper = ({ elementFromPoint(x, y) {
        const hit = Reflect.apply(original, this, arguments);
        if (hit && hit.localName === "img" && hit.hasAttribute("usemap")) { const area = areaAt(hit, Number(x), Number(y)); if (area) return area; }
        return hit;
      } }).elementFromPoint;
      Object.defineProperty(wrapper, "length", { value: original.length, configurable: true });
      Object.defineProperty(C.prototype, "elementFromPoint", { value: wrapper, writable: true, configurable: true, enumerable: true });
    }
    const points = Object.getOwnPropertyDescriptor(C.prototype, "elementsFromPoint");
    if (points && typeof points.value === "function") {
      const original = points.value;
      const wrapper = ({ elementsFromPoint(x, y) {
        const hits = Reflect.apply(original, this, arguments);
        const out = [];
        for (const hit of hits) { if (hit && hit.localName === "img" && hit.hasAttribute("usemap")) { const area = areaAt(hit, Number(x), Number(y)); if (area) out.push(area); } out.push(hit); }
        return out;
      } }).elementsFromPoint;
      Object.defineProperty(wrapper, "length", { value: original.length, configurable: true });
      Object.defineProperty(C.prototype, "elementsFromPoint", { value: wrapper, writable: true, configurable: true, enumerable: true });
    }
  }
  if (typeof HTMLMapElement === "function") {
    Object.defineProperty(HTMLMapElement.prototype, "areas", { get() { return this.getElementsByTagName("area"); }, enumerable: true, configurable: true });
  }
})();
