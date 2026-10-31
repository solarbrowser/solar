// Canvas 2D (https://html.spec.whatwg.org/multipage/canvas.html): HTMLCanvasElement's getContext, CanvasRenderingContext2D, Path2D, gradients and patterns, ImageBitmap and
// OffscreenCanvas. The drawing is Skia's (src/paint/Canvas.cpp), which the natives of src/paint/CanvasNatives.cpp give: the context here is the interface — what the page may
// set and read, what it throws — and keeps the values that it has to give back; the surface keeps the state that draws.
(() => {
  "use strict";
  const N = {};
  for (const n of ["CanvasCreate", "CanvasResize", "CanvasFree", "CanvasAttach", "Cv", "CvDash", "CvRoundRect", "CvStr", "CvStyle", "CvColor", "CvPath", "CvAddPath", "CvDrawImage",
    "CvGetImageData", "CvPutImageData", "ImageDecode", "ImageFromCanvas", "ImageFromPixels", "ImageFromVideo", "ImageFree", "ImageEncode"]) { N[n] = globalThis["__solar" + n]; delete globalThis["__solar" + n]; }
  Object.defineProperty(globalThis, "__solarImageNatives", { value: { decode: N.ImageDecode, free: N.ImageFree }, configurable: true, enumerable: false });
  if (typeof N.CanvasCreate !== "function") {
    // (A program with no drawing has no context: getContext answers null, as for a type that is not supported; Path2D and the rest are still there to be constructed.)
    if (typeof Path2D === "undefined") {
      Object.defineProperty(globalThis, "Path2D", { value: class Path2D { addPath() {} closePath() {} moveTo() {} lineTo() {} bezierCurveTo() {} quadraticCurveTo() {} arc() {} arcTo() {} ellipse() {} rect() {} roundRect() {} }, writable: true, configurable: true });
    }
    if (typeof CanvasRenderingContext2D === "undefined") Object.defineProperty(globalThis, "CanvasRenderingContext2D", { value: class CanvasRenderingContext2D { constructor() { throw new TypeError("Illegal constructor"); } }, writable: true, configurable: true });
    return;
  }

  // (A worker has no elements: it has OffscreenCanvas, and the rest of this file.)
  const HTMLCanvas = typeof HTMLCanvasElement === "function" ? HTMLCanvasElement : class HTMLCanvasElement {};
  const define = (target, name, value) => Object.defineProperty(target, name, { value, writable: true, configurable: true, enumerable: false });
  if (typeof ImageData === "undefined") {
    define(globalThis, "ImageData", class ImageData {
      constructor(a, b, c) {
        const data = a instanceof Uint8ClampedArray ? a : null;
        const width = data ? b : a, height = data ? (c === undefined ? data.length / 4 / width : c) : b;
        if (!(width > 0) || !(height > 0)) throw new DOMException("The source width is zero or not a number.", "IndexSizeError");
        Object.defineProperties(this, { width: { value: width, enumerable: true }, height: { value: height, enumerable: true }, data: { value: data || new Uint8ClampedArray(width * height * 4), enumerable: true }, colorSpace: { value: "srgb", enumerable: true } });
      }
    });
  }
  const tag = (cls, name) => Object.defineProperty(cls.prototype, Symbol.toStringTag, { value: name, configurable: true });
  const fail = (name, message) => new DOMException(message, name);
  const token = Symbol("canvas");
  const finite = (...v) => v.every(Number.isFinite);
  const num = (v) => Number(v);

  // The operations of the natives (src/paint/CanvasNatives.cpp).
  const OP = { save: 0, restore: 1, scale: 2, rotate: 3, translate: 4, transform: 5, setTransform: 6, resetTransform: 7, globalAlpha: 8, lineWidth: 9, lineCap: 10, lineJoin: 11,
    miterLimit: 12, lineDashOffset: 13, shadowBlur: 14, shadowOffsetX: 15, shadowOffsetY: 16, smoothing: 17, textAlign: 18, textBaseline: 19, direction: 20, beginPath: 30,
    closePath: 31, moveTo: 32, lineTo: 33, bezierCurveTo: 34, quadraticCurveTo: 35, arc: 36, arcTo: 37, ellipse: 38, rect: 39, fillRect: 50, strokeRect: 51, clearRect: 52 };

  // ---- Colors: what is set is kept as the specification says to give it back ----
  const hex2 = (n) => n.toString(16).padStart(2, "0");
  // The alpha as a color is given back: the shortest of two or three digits that is the same to eight bits.
  const alphaText = (a) => {
    const eight = Math.round(a * 255);
    let v = Math.round(a * 100) / 100;
    if (Math.round(v * 255) !== eight) v = Math.round(a * 1000) / 1000;
    return String(v);
  };
  const serialize = (rgba) => (rgba[3] === 1 ? "#" + hex2(rgba[0]) + hex2(rgba[1]) + hex2(rgba[2]) : `rgba(${rgba[0]}, ${rgba[1]}, ${rgba[2]}, ${alphaText(rgba[3])})`);
  const colorOf = (text) => N.CvColor(String(text));

  // ---- Matrices (a b c d e f), for what has to be given back ----
  const identity = () => [1, 0, 0, 1, 0, 0];
  const multiply = (m, n) => [m[0] * n[0] + m[2] * n[1], m[1] * n[0] + m[3] * n[1], m[0] * n[2] + m[2] * n[3], m[1] * n[2] + m[3] * n[3], m[0] * n[4] + m[2] * n[5] + m[4], m[1] * n[4] + m[3] * n[5] + m[5]];
  const invert = (m) => {
    const det = m[0] * m[3] - m[1] * m[2];
    if (!det || !Number.isFinite(det)) return null;
    return [m[3] / det, -m[1] / det, -m[2] / det, m[0] / det, (m[2] * m[5] - m[3] * m[4]) / det, (m[1] * m[4] - m[0] * m[5]) / det];
  };
  const matrixOf = (init) => {
    if (init === undefined) return identity();
    if (init && typeof init === "object") {
      if ("a" in init) return [init.a, init.b, init.c, init.d, init.e, init.f].map(num);
      if ("m11" in init) return [init.m11, init.m12, init.m21, init.m22, init.m41, init.m42].map(num);
    }
    return identity();
  };

  // ---- Gradients and patterns ----
  class CanvasGradient {
    #t; #c; #stops = [];
    constructor(t, kind, coords) { if (t !== token) throw new TypeError("Illegal constructor"); this.#t = kind; this.#c = coords; }
    addColorStop(offset, color) {
      if (arguments.length < 2) throw new TypeError("Failed to execute 'addColorStop' on 'CanvasGradient': 2 arguments required, but only " + arguments.length + " present.");
      offset = num(offset);
      if (!(offset >= 0 && offset <= 1)) throw fail("IndexSizeError", "Failed to execute 'addColorStop' on 'CanvasGradient': The provided value (" + offset + ") is outside the range (0.0, 1.0).");
      if (!colorOf(color)) throw fail("SyntaxError", "Failed to execute 'addColorStop' on 'CanvasGradient': The value provided ('" + color + "') could not be parsed as a color.");
      const at = this.#stops.findIndex((s) => s[0] > offset);
      this.#stops.splice(at < 0 ? this.#stops.length : at, 0, [offset, String(color)]);
    }
    static describe(g) { return JSON.stringify({ t: g.#t, c: g.#c, s: g.#stops }); }
  }
  tag(CanvasGradient, "CanvasGradient");
  const images = new FinalizationRegistry((id) => N.ImageFree(id));
  class CanvasPattern {
    #image; #repeat; #matrix = identity();
    constructor(t, image, repeat) { if (t !== token) throw new TypeError("Illegal constructor"); this.#image = image; this.#repeat = repeat; }
    setTransform(matrix) { this.#matrix = matrixOf(matrix); }
    static describe(p) { return JSON.stringify({ p: p.#image.id, r: p.#repeat, m: p.#matrix }); }
  }
  tag(CanvasPattern, "CanvasPattern");

  // ---- Path2D: its commands as numbers, which the surface plays back (see PathWriter::AddCommands) ----
  const V = { move: 0, line: 1, cubic: 2, quad: 3, close: 4, ellipse: 5, arcTo: 6, rect: 7, roundRect: 8, matrix: 9 };
  const radiiOf = (r) => {
    // roundRect's radii: a number or a DOMPointInit, or a list of one to four of them, as the four corners (upper left, upper right, lower right, lower left), each x and y.
    const list = Array.isArray(r) ? r : [r];
    const one = (v) => (typeof v === "number" ? [v, v] : v && typeof v === "object" ? [num(v.x ?? 0), num(v.y ?? 0)] : [num(v), num(v)]);
    const p = list.map(one);
    if (p.some((q) => !finite(q[0], q[1]))) return null;
    if (p.some((q) => q[0] < 0 || q[1] < 0)) throw new RangeError("Radius value must be non-negative.");
    const [a, b, c, d] = p.length === 1 ? [p[0], p[0], p[0], p[0]] : p.length === 2 ? [p[0], p[1], p[0], p[1]] : p.length === 3 ? [p[0], p[1], p[2], p[1]] : p.length === 4 ? p : [null];
    if (!a) throw new RangeError("roundRect: 1 to 4 radii are expected.");
    return [a[0], a[1], b[0], b[1], c[0], c[1], d[0], d[1]];
  };
  // A path's commands, written by the same calls for Path2D and the context's path.
  const writer = (push) => ({
    moveTo: (x, y) => finite(x, y) && push(V.move, x, y),
    lineTo: (x, y) => finite(x, y) && push(V.line, x, y),
    bezierCurveTo: (a, b, c, d, e, f) => finite(a, b, c, d, e, f) && push(V.cubic, a, b, c, d, e, f),
    quadraticCurveTo: (a, b, c, d) => finite(a, b, c, d) && push(V.quad, a, b, c, d),
    closePath: () => push(V.close),
  });
  class Path2D {
    #commands = [];
    constructor(source) {
      if (source instanceof Path2D) this.#commands = Path2D.commandsOf(source).slice();
      else if (source !== undefined) this.#commands = parseSvgPath(String(source));
    }
    static commandsOf(p) { return p.#commands; }
    #push(...v) { this.#commands.push(...v); }
    addPath(path, matrix) {
      if (!(path instanceof Path2D)) throw new TypeError("Failed to execute 'addPath' on 'Path2D': parameter 1 is not of type 'Path2D'.");
      const m = matrixOf(matrix);
      this.#commands.push(V.matrix, ...m, ...path.#commands, V.matrix, 1, 0, 0, 1, 0, 0);
    }
    closePath() { this.#push(V.close); }
    moveTo(x, y) { x = num(x); y = num(y); if (finite(x, y)) this.#push(V.move, x, y); }
    lineTo(x, y) { x = num(x); y = num(y); if (finite(x, y)) this.#push(V.line, x, y); }
    bezierCurveTo(a, b, c, d, e, f) { const v = [a, b, c, d, e, f].map(num); if (finite(...v)) this.#push(V.cubic, ...v); }
    quadraticCurveTo(a, b, c, d) { const v = [a, b, c, d].map(num); if (finite(...v)) this.#push(V.quad, ...v); }
    arc(x, y, r, s, e, ccw = false) {
      [x, y, r, s, e] = [x, y, r, s, e].map(num);
      if (!finite(x, y, r, s, e)) return;
      if (r < 0) throw fail("IndexSizeError", "Failed to execute 'arc' on 'Path2D': The radius provided (" + r + ") is negative.");
      this.#push(V.ellipse, x, y, r, r, 0, s, e, ccw ? 1 : 0);
    }
    arcTo(a, b, c, d, r) {
      [a, b, c, d, r] = [a, b, c, d, r].map(num);
      if (!finite(a, b, c, d, r)) return;
      if (r < 0) throw fail("IndexSizeError", "Failed to execute 'arcTo' on 'Path2D': The radius provided (" + r + ") is negative.");
      this.#push(V.arcTo, a, b, c, d, r);
    }
    ellipse(x, y, rx, ry, rot, s, e, ccw = false) {
      [x, y, rx, ry, rot, s, e] = [x, y, rx, ry, rot, s, e].map(num);
      if (!finite(x, y, rx, ry, rot, s, e)) return;
      if (rx < 0 || ry < 0) throw fail("IndexSizeError", "Failed to execute 'ellipse' on 'Path2D': The radius provided is negative.");
      this.#push(V.ellipse, x, y, rx, ry, rot, s, e, ccw ? 1 : 0);
    }
    rect(x, y, w, h) { const v = [x, y, w, h].map(num); if (finite(...v)) this.#push(V.rect, ...v); }
    roundRect(x, y, w, h, radii = 0) {
      const v = [x, y, w, h].map(num);
      const r = radiiOf(radii);
      if (finite(...v) && r) this.#push(V.roundRect, ...v, ...r);
    }
  }
  tag(Path2D, "Path2D");

  // An SVG path's data (https://www.w3.org/TR/SVG2/paths.html) as commands: the relative and shorthand forms made absolute, the arcs made curves.
  const parseSvgPath = (d) => {
    const out = [];
    const tokens = d.match(/[a-zA-Z]|[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?/g) || [];
    let i = 0, cx = 0, cy = 0, sx = 0, sy = 0, command = "", lastCubic = null, lastQuad = null;
    const read = (n) => { const v = []; for (let k = 0; k < n; k++) { if (i >= tokens.length || /^[a-zA-Z]$/.test(tokens[i])) return null; v.push(Number(tokens[i++])); } return v; };
    const arc = (x1, y1, rx, ry, phi, large, sweep, x2, y2) => {
      if (rx === 0 || ry === 0) { out.push(V.line, x2, y2); return; }
      rx = Math.abs(rx); ry = Math.abs(ry);
      const p = (phi * Math.PI) / 180, cp = Math.cos(p), sp = Math.sin(p);
      const dx = (x1 - x2) / 2, dy = (y1 - y2) / 2;
      const x1p = cp * dx + sp * dy, y1p = -sp * dx + cp * dy;
      const lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
      if (lambda > 1) { const s = Math.sqrt(lambda); rx *= s; ry *= s; }
      const sign = large === sweep ? -1 : 1;
      const num_ = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
      const co = sign * Math.sqrt(Math.max(0, num_ / (rx * rx * y1p * y1p + ry * ry * x1p * x1p)));
      const cxp = (co * rx * y1p) / ry, cyp = (-co * ry * x1p) / rx;
      const ccx = cp * cxp - sp * cyp + (x1 + x2) / 2, ccy = sp * cxp + cp * cyp + (y1 + y2) / 2;
      const ang = (ux, uy, vx, vy) => { const a = Math.atan2(ux * vy - uy * vx, ux * vx + uy * vy); return a; };
      const theta = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry);
      let delta = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry);
      if (!sweep && delta > 0) delta -= 2 * Math.PI;
      else if (sweep && delta < 0) delta += 2 * Math.PI;
      out.push(V.ellipse, ccx, ccy, rx, ry, p, theta, theta + delta, delta < 0 ? 1 : 0);
    };
    while (i < tokens.length) {
      if (/^[a-zA-Z]$/.test(tokens[i])) command = tokens[i++];
      else if (!command) break;
      const rel = command === command.toLowerCase();
      const C = command.toUpperCase();
      let v;
      switch (C) {
        case "M": if (!(v = read(2))) return out; cx = rel ? cx + v[0] : v[0]; cy = rel ? cy + v[1] : v[1]; sx = cx; sy = cy; out.push(V.move, cx, cy); command = rel ? "l" : "L"; lastCubic = lastQuad = null; break;
        case "L": if (!(v = read(2))) return out; cx = rel ? cx + v[0] : v[0]; cy = rel ? cy + v[1] : v[1]; out.push(V.line, cx, cy); lastCubic = lastQuad = null; break;
        case "H": if (!(v = read(1))) return out; cx = rel ? cx + v[0] : v[0]; out.push(V.line, cx, cy); lastCubic = lastQuad = null; break;
        case "V": if (!(v = read(1))) return out; cy = rel ? cy + v[0] : v[0]; out.push(V.line, cx, cy); lastCubic = lastQuad = null; break;
        case "C": { if (!(v = read(6))) return out; const o = rel ? [cx, cy] : [0, 0]; const p = [v[0] + o[0], v[1] + o[1], v[2] + o[0], v[3] + o[1], v[4] + o[0], v[5] + o[1]]; out.push(V.cubic, ...p); lastCubic = [p[2], p[3]]; lastQuad = null; cx = p[4]; cy = p[5]; break; }
        case "S": { if (!(v = read(4))) return out; const o = rel ? [cx, cy] : [0, 0]; const r = lastCubic ? [2 * cx - lastCubic[0], 2 * cy - lastCubic[1]] : [cx, cy]; const p = [r[0], r[1], v[0] + o[0], v[1] + o[1], v[2] + o[0], v[3] + o[1]]; out.push(V.cubic, ...p); lastCubic = [p[2], p[3]]; lastQuad = null; cx = p[4]; cy = p[5]; break; }
        case "Q": { if (!(v = read(4))) return out; const o = rel ? [cx, cy] : [0, 0]; const p = [v[0] + o[0], v[1] + o[1], v[2] + o[0], v[3] + o[1]]; out.push(V.quad, ...p); lastQuad = [p[0], p[1]]; lastCubic = null; cx = p[2]; cy = p[3]; break; }
        case "T": { if (!(v = read(2))) return out; const o = rel ? [cx, cy] : [0, 0]; const r = lastQuad ? [2 * cx - lastQuad[0], 2 * cy - lastQuad[1]] : [cx, cy]; const p = [r[0], r[1], v[0] + o[0], v[1] + o[1]]; out.push(V.quad, ...p); lastQuad = [p[0], p[1]]; lastCubic = null; cx = p[2]; cy = p[3]; break; }
        case "A": { if (!(v = read(7))) return out; const x = rel ? cx + v[5] : v[5], y = rel ? cy + v[6] : v[6]; arc(cx, cy, v[0], v[1], v[2], v[3] !== 0, v[4] !== 0, x, y); cx = x; cy = y; lastCubic = lastQuad = null; break; }
        case "Z": out.push(V.close); cx = sx; cy = sy; lastCubic = lastQuad = null; command = ""; break;
        default: return out;
      }
    }
    return out;
  };

  // ---- TextMetrics, ImageBitmap ----
  class TextMetrics {
    constructor(t, values) { if (t !== token) throw new TypeError("Illegal constructor"); for (const [k, v] of Object.entries(values)) Object.defineProperty(this, k, { value: v, enumerable: true, configurable: true }); }
  }
  tag(TextMetrics, "TextMetrics");
  class ImageBitmap {
    #id; #w; #h; #closed = false;
    constructor(t, id, w, h) { if (t !== token) throw new TypeError("Illegal constructor"); this.#id = id; this.#w = w; this.#h = h; images.register(this, id, this); }
    get width() { return this.#closed ? 0 : this.#w; }
    get height() { return this.#closed ? 0 : this.#h; }
    close() { if (!this.#closed) { this.#closed = true; images.unregister(this); N.ImageFree(this.#id); } }
    static id(b) { return b.#closed ? -1 : b.#id; }
  }
  tag(ImageBitmap, "ImageBitmap");

  // ---- What can be drawn: an image, a canvas, a video, a bitmap ----
  // A picture made of the source for the natives: {id, w, h, temporary}, or null where there is none to draw (nothing is drawn), or it throws where the specification says so.
  const pictureOf = (source, method) => {
    if (source instanceof ImageBitmap) {
      const id = ImageBitmap.id(source);
      if (id < 0) throw fail("InvalidStateError", `Failed to execute '${method}' on 'CanvasRenderingContext2D': The image source is detached.`);
      return { id, w: source.width, h: source.height, temporary: false };
    }
    if (typeof HTMLImageElement === "function" && source instanceof HTMLImageElement || (typeof SVGImageElement === "function" && source instanceof SVGImageElement)) {
      const state = globalThis.__solarImageBitmap ? globalThis.__solarImageBitmap(source) : null;
      if (!state || !state.complete) return null;
      if (state.broken) throw fail("InvalidStateError", `Failed to execute '${method}' on 'CanvasRenderingContext2D': The HTMLImageElement provided is in the 'broken' state.`);
      return { id: state.id, w: state.w, h: state.h, temporary: false };
    }
    if (source instanceof HTMLCanvas || (typeof OffscreenCanvas === "function" && source instanceof OffscreenCanvas)) {
      if (source.width === 0 || source.height === 0) throw fail("InvalidStateError", `Failed to execute '${method}' on 'CanvasRenderingContext2D': The image argument is a canvas element with a width or height of 0.`);
      const gl = glContexts.get(source);
      if (gl) gl._present();
      const surface = canvasSurfaces.get(source);
      if (surface === undefined) return { id: -1, w: source.width, h: source.height, temporary: false, blank: true };
      const made = N.ImageFromCanvas(surface);
      return made ? { id: made[0], w: made[1], h: made[2], temporary: true } : null;
    }
    if (typeof HTMLVideoElement === "function" && source instanceof HTMLVideoElement) {
      const made = N.ImageFromVideo(source);
      return made ? { id: made[0], w: made[1], h: made[2], temporary: true } : null;
    }
    throw new TypeError(`Failed to execute '${method}' on 'CanvasRenderingContext2D': The provided value is not of type '(CSSImageValue or HTMLImageElement or SVGImageElement or HTMLVideoElement or HTMLCanvasElement or ImageBitmap or OffscreenCanvas or VideoFrame)'.`);
  };
  const canvasSurfaces = new WeakMap();  // a canvas or an offscreen canvas -> the number of its surface

  // ---- The context ----
  const MODES = { lineCap: ["butt", "round", "square"], lineJoin: ["miter", "round", "bevel"], textAlign: ["start", "end", "left", "right", "center"],
    textBaseline: ["alphabetic", "top", "hanging", "middle", "ideographic", "bottom"], direction: ["ltr", "rtl", "inherit"], imageSmoothingQuality: ["low", "medium", "high"] };

  class CanvasRenderingContext2D {
    #canvas; #id; #state; #stack = []; #path = []; #lost = false;
    constructor(t, canvas, id) {
      if (t !== token) throw new TypeError("Illegal constructor");
      this.#canvas = canvas; this.#id = id; this.#state = CanvasRenderingContext2D.fresh();
    }
    static fresh() {
      return { fillStyle: "#000000", strokeStyle: "#000000", fillObject: null, strokeObject: null, globalAlpha: 1, globalCompositeOperation: "source-over", lineWidth: 1, lineCap: "butt", lineJoin: "miter",
        miterLimit: 10, dash: [], dashOffset: 0, shadowBlur: 0, shadowOffsetX: 0, shadowOffsetY: 0, shadowColor: "rgba(0, 0, 0, 0)", smoothing: true, quality: "low", font: "10px sans-serif",
        textAlign: "start", textBaseline: "alphabetic", direction: "ltr", filter: "none", letterSpacing: "0px", wordSpacing: "0px", fontKerning: "auto", fontStretch: "normal", fontVariantCaps: "normal",
        textRendering: "auto", matrix: identity() };
    }
    get canvas() { return this.#canvas; }
    get _surface() { return this.#id; }
    _reset(id) { this.#id = id; this.#state = CanvasRenderingContext2D.fresh(); this.#stack = []; this.#path = []; }
    getContextAttributes() { return { alpha: true, colorSpace: "srgb", desynchronized: false, willReadFrequently: false }; }
    isContextLost() { return this.#lost; }
    reset() { N.CanvasResize(this.#id, this.#canvas.width, this.#canvas.height); this.#state = CanvasRenderingContext2D.fresh(); this.#stack = []; this.#path = []; }

    // state
    save() { this.#stack.push({ ...this.#state, dash: this.#state.dash.slice(), matrix: this.#state.matrix.slice() }); N.Cv(this.#id, OP.save); }
    restore() { if (!this.#stack.length) return; this.#state = this.#stack.pop(); N.Cv(this.#id, OP.restore); }
    scale(x, y) { x = num(x); y = num(y); if (!finite(x, y)) return; this.#state.matrix = multiply(this.#state.matrix, [x, 0, 0, y, 0, 0]); N.Cv(this.#id, OP.scale, x, y); }
    rotate(a) { a = num(a); if (!finite(a)) return; const c = Math.cos(a), s = Math.sin(a); this.#state.matrix = multiply(this.#state.matrix, [c, s, -s, c, 0, 0]); N.Cv(this.#id, OP.rotate, a); }
    translate(x, y) { x = num(x); y = num(y); if (!finite(x, y)) return; this.#state.matrix = multiply(this.#state.matrix, [1, 0, 0, 1, x, y]); N.Cv(this.#id, OP.translate, x, y); }
    transform(a, b, c, d, e, f) {
      const v = [a, b, c, d, e, f].map(num);
      if (!finite(...v)) return;
      this.#state.matrix = multiply(this.#state.matrix, v);
      N.Cv(this.#id, OP.transform, ...v);
    }
    setTransform(a, b, c, d, e, f) {
      let v;
      if (a !== null && typeof a === "object") v = matrixOf(a);
      else { if (arguments.length === 0) v = identity(); else v = [a, b, c, d, e, f].map(num); }
      if (!finite(...v)) return;
      this.#state.matrix = v;
      N.Cv(this.#id, OP.setTransform, ...v);
    }
    resetTransform() { this.#state.matrix = identity(); N.Cv(this.#id, OP.resetTransform); }
    getTransform() { const m = this.#state.matrix; return typeof DOMMatrix === "function" ? new DOMMatrix(m) : { a: m[0], b: m[1], c: m[2], d: m[3], e: m[4], f: m[5] }; }

    get globalAlpha() { return this.#state.globalAlpha; }
    set globalAlpha(v) { v = num(v); if (v >= 0 && v <= 1) { this.#state.globalAlpha = v; N.Cv(this.#id, OP.globalAlpha, v); } }
    get globalCompositeOperation() { return this.#state.globalCompositeOperation; }
    set globalCompositeOperation(v) { v = String(v); if (N.CvStr(this.#id, 1, v)) this.#state.globalCompositeOperation = v; }
    get lineWidth() { return this.#state.lineWidth; }
    set lineWidth(v) { v = num(v); if (v > 0 && Number.isFinite(v)) { this.#state.lineWidth = v; N.Cv(this.#id, OP.lineWidth, v); } }
    get lineCap() { return this.#state.lineCap; }
    set lineCap(v) { const i = MODES.lineCap.indexOf(String(v)); if (i >= 0) { this.#state.lineCap = MODES.lineCap[i]; N.Cv(this.#id, OP.lineCap, i); } }
    get lineJoin() { return this.#state.lineJoin; }
    set lineJoin(v) { const i = MODES.lineJoin.indexOf(String(v)); if (i >= 0) { this.#state.lineJoin = MODES.lineJoin[i]; N.Cv(this.#id, OP.lineJoin, i); } }
    get miterLimit() { return this.#state.miterLimit; }
    set miterLimit(v) { v = num(v); if (v > 0 && Number.isFinite(v)) { this.#state.miterLimit = v; N.Cv(this.#id, OP.miterLimit, v); } }
    setLineDash(list) {
      const v = Array.from(list, num);
      if (!v.every((x) => Number.isFinite(x) && x >= 0)) return;
      const dash = v.length % 2 ? v.concat(v) : v;
      this.#state.dash = dash;
      N.CvDash(this.#id, new Float64Array(dash));
    }
    getLineDash() { return this.#state.dash.slice(); }
    get lineDashOffset() { return this.#state.dashOffset; }
    set lineDashOffset(v) { v = num(v); if (Number.isFinite(v)) { this.#state.dashOffset = v; N.Cv(this.#id, OP.lineDashOffset, v); } }
    get shadowBlur() { return this.#state.shadowBlur; }
    set shadowBlur(v) { v = num(v); if (v >= 0 && Number.isFinite(v)) { this.#state.shadowBlur = v; N.Cv(this.#id, OP.shadowBlur, v); } }
    get shadowOffsetX() { return this.#state.shadowOffsetX; }
    set shadowOffsetX(v) { v = num(v); if (Number.isFinite(v)) { this.#state.shadowOffsetX = v; N.Cv(this.#id, OP.shadowOffsetX, v); } }
    get shadowOffsetY() { return this.#state.shadowOffsetY; }
    set shadowOffsetY(v) { v = num(v); if (Number.isFinite(v)) { this.#state.shadowOffsetY = v; N.Cv(this.#id, OP.shadowOffsetY, v); } }
    get shadowColor() { return this.#state.shadowColor; }
    set shadowColor(v) { const c = colorOf(v); if (c && N.CvStyle(this.#id, 2, String(v))) this.#state.shadowColor = serialize(c); }
    get imageSmoothingEnabled() { return this.#state.smoothing; }
    set imageSmoothingEnabled(v) { this.#state.smoothing = !!v; N.Cv(this.#id, OP.smoothing, v ? 1 : 0, MODES.imageSmoothingQuality.indexOf(this.#state.quality)); }
    get imageSmoothingQuality() { return this.#state.quality; }
    set imageSmoothingQuality(v) { const i = MODES.imageSmoothingQuality.indexOf(String(v)); if (i >= 0) { this.#state.quality = MODES.imageSmoothingQuality[i]; N.Cv(this.#id, OP.smoothing, this.#state.smoothing ? 1 : 0, i); } }
    get filter() { return this.#state.filter; }
    set filter(v) { this.#state.filter = String(v); }

    // styles
    #style(which, v, key, objectKey) {
      if (typeof v === "string" || (v !== null && typeof v === "object" && !(v instanceof CanvasGradient) && !(v instanceof CanvasPattern)) || typeof v !== "object") {
        const text = String(v);
        const c = colorOf(text);
        if (!c || !N.CvStyle(this.#id, which, text)) return;
        this.#state[key] = serialize(c);
        this.#state[objectKey] = null;
      } else if (v instanceof CanvasGradient) {
        if (N.CvStyle(this.#id, which, CanvasGradient.describe(v))) this.#state[objectKey] = v;
      } else if (v instanceof CanvasPattern) {
        if (N.CvStyle(this.#id, which, CanvasPattern.describe(v))) this.#state[objectKey] = v;
      }
    }
    get fillStyle() { return this.#state.fillObject || this.#state.fillStyle; }
    set fillStyle(v) { this.#style(0, v, "fillStyle", "fillObject"); }
    get strokeStyle() { return this.#state.strokeObject || this.#state.strokeStyle; }
    set strokeStyle(v) { this.#style(1, v, "strokeStyle", "strokeObject"); }
    createLinearGradient(x0, y0, x1, y1) {
      const c = [x0, y0, x1, y1].map(num);
      if (!finite(...c)) throw new TypeError("Failed to execute 'createLinearGradient' on 'CanvasRenderingContext2D': The provided double value is non-finite.");
      return new CanvasGradient(token, 0, c);
    }
    createRadialGradient(x0, y0, r0, x1, y1, r1) {
      const c = [x0, y0, r0, x1, y1, r1].map(num);
      if (!finite(...c)) throw new TypeError("Failed to execute 'createRadialGradient' on 'CanvasRenderingContext2D': The provided double value is non-finite.");
      if (r0 < 0 || r1 < 0) throw fail("IndexSizeError", "Failed to execute 'createRadialGradient' on 'CanvasRenderingContext2D': The radius provided is negative.");
      return new CanvasGradient(token, 1, c);
    }
    createConicGradient(angle, x, y) {
      const c = [x, y, angle].map(num);
      if (!finite(...c)) throw new TypeError("Failed to execute 'createConicGradient' on 'CanvasRenderingContext2D': The provided double value is non-finite.");
      return new CanvasGradient(token, 2, c);
    }
    createPattern(source, repetition) {
      const repeat = repetition === null || repetition === undefined || repetition === "" ? "repeat" : String(repetition);
      if (!["repeat", "repeat-x", "repeat-y", "no-repeat"].includes(repeat)) throw fail("SyntaxError", "Failed to execute 'createPattern' on 'CanvasRenderingContext2D': The provided type ('" + repetition + "') is not a valid repetition.");
      const picture = pictureOf(source, "createPattern");
      if (!picture) return null;
      if (picture.blank) return new CanvasPattern(token, { id: -1 }, repeat);
      // (The pattern keeps its own copy: the source may change, and a temporary picture is the pattern's.)
      if (picture.temporary) images.register(source, picture.id);
      return new CanvasPattern(token, picture, repeat);
    }

    // text
    get font() { return this.#state.font; }
    set font(v) { v = String(v); if (N.CvStr(this.#id, 0, v)) this.#state.font = v; }
    get textAlign() { return this.#state.textAlign; }
    set textAlign(v) { const i = MODES.textAlign.indexOf(String(v)); if (i >= 0) { this.#state.textAlign = MODES.textAlign[i]; N.Cv(this.#id, OP.textAlign, i); } }
    get textBaseline() { return this.#state.textBaseline; }
    set textBaseline(v) { const i = MODES.textBaseline.indexOf(String(v)); if (i >= 0) { this.#state.textBaseline = MODES.textBaseline[i]; N.Cv(this.#id, OP.textBaseline, i); } }
    get direction() { return this.#state.direction; }
    set direction(v) { const i = MODES.direction.indexOf(String(v)); if (i >= 0) { this.#state.direction = MODES.direction[i]; N.Cv(this.#id, OP.direction, i === 2 ? 0 : i); } }
    get letterSpacing() { return this.#state.letterSpacing; }
    set letterSpacing(v) { this.#state.letterSpacing = String(v); }
    get wordSpacing() { return this.#state.wordSpacing; }
    set wordSpacing(v) { this.#state.wordSpacing = String(v); }
    get fontKerning() { return this.#state.fontKerning; }
    set fontKerning(v) { this.#state.fontKerning = String(v); }
    get fontStretch() { return this.#state.fontStretch; }
    set fontStretch(v) { this.#state.fontStretch = String(v); }
    get fontVariantCaps() { return this.#state.fontVariantCaps; }
    set fontVariantCaps(v) { this.#state.fontVariantCaps = String(v); }
    get textRendering() { return this.#state.textRendering; }
    set textRendering(v) { this.#state.textRendering = String(v); }
    fillText(text, x, y, maxWidth) {
      if (arguments.length < 3) throw new TypeError("Failed to execute 'fillText' on 'CanvasRenderingContext2D': 3 arguments required, but only " + arguments.length + " present.");
      x = num(x); y = num(y);
      if (finite(x, y)) N.CvStr(this.#id, 2, String(text), x, y, maxWidth === undefined ? NaN : num(maxWidth));
    }
    strokeText(text, x, y, maxWidth) {
      if (arguments.length < 3) throw new TypeError("Failed to execute 'strokeText' on 'CanvasRenderingContext2D': 3 arguments required, but only " + arguments.length + " present.");
      x = num(x); y = num(y);
      if (finite(x, y)) N.CvStr(this.#id, 3, String(text), x, y, maxWidth === undefined ? NaN : num(maxWidth));
    }
    measureText(text) {
      if (arguments.length === 0) throw new TypeError("Failed to execute 'measureText' on 'CanvasRenderingContext2D': 1 argument required, but only 0 present.");
      return new TextMetrics(token, JSON.parse(N.CvStr(this.#id, 4, String(text))));
    }

    // rectangles and paths
    fillRect(x, y, w, h) { N.Cv(this.#id, OP.fillRect, num(x), num(y), num(w), num(h)); }
    strokeRect(x, y, w, h) { N.Cv(this.#id, OP.strokeRect, num(x), num(y), num(w), num(h)); }
    clearRect(x, y, w, h) { N.Cv(this.#id, OP.clearRect, num(x), num(y), num(w), num(h)); }
    beginPath() { N.Cv(this.#id, OP.beginPath); }
    closePath() { N.Cv(this.#id, OP.closePath); }
    moveTo(x, y) { N.Cv(this.#id, OP.moveTo, num(x), num(y)); }
    lineTo(x, y) { N.Cv(this.#id, OP.lineTo, num(x), num(y)); }
    bezierCurveTo(a, b, c, d, e, f) { N.Cv(this.#id, OP.bezierCurveTo, num(a), num(b), num(c), num(d), num(e), num(f)); }
    quadraticCurveTo(a, b, c, d) { N.Cv(this.#id, OP.quadraticCurveTo, num(a), num(b), num(c), num(d)); }
    arc(x, y, r, s, e, ccw = false) {
      [x, y, r, s, e] = [x, y, r, s, e].map(num);
      if (finite(x, y, r, s, e) && r < 0) throw fail("IndexSizeError", "Failed to execute 'arc' on 'CanvasRenderingContext2D': The radius provided (" + r + ") is negative.");
      N.Cv(this.#id, OP.arc, x, y, r, s, e, ccw ? 1 : 0);
    }
    arcTo(a, b, c, d, r) {
      [a, b, c, d, r] = [a, b, c, d, r].map(num);
      if (finite(a, b, c, d, r) && r < 0) throw fail("IndexSizeError", "Failed to execute 'arcTo' on 'CanvasRenderingContext2D': The radius provided (" + r + ") is negative.");
      N.Cv(this.#id, OP.arcTo, a, b, c, d, r);
    }
    ellipse(x, y, rx, ry, rot, s, e, ccw = false) {
      [x, y, rx, ry, rot, s, e] = [x, y, rx, ry, rot, s, e].map(num);
      if (finite(x, y, rx, ry, rot, s, e) && (rx < 0 || ry < 0)) throw fail("IndexSizeError", "Failed to execute 'ellipse' on 'CanvasRenderingContext2D': The radius provided is negative.");
      N.Cv(this.#id, OP.ellipse, x, y, rx, ry, rot, s, e, ccw ? 1 : 0);
    }
    rect(x, y, w, h) { N.Cv(this.#id, OP.rect, num(x), num(y), num(w), num(h)); }
    roundRect(x, y, w, h, radii = 0) {
      const v = [x, y, w, h].map(num);
      const r = radiiOf(radii);
      if (finite(...v) && r) N.CvRoundRect(this.#id, ...v, new Float64Array(r));
    }
    #pathArgs(args) {
      // (fill(), fill(rule), fill(path), fill(path, rule))
      let commands, rule = "nonzero";
      if (args[0] instanceof Path2D) { commands = new Float64Array(Path2D.commandsOf(args[0])); if (args[1] !== undefined) rule = String(args[1]); }
      else if (args[0] !== undefined) rule = String(args[0]);
      if (rule !== "nonzero" && rule !== "evenodd") throw new TypeError(`The provided value '${rule}' is not a valid enum value of type CanvasFillRule.`);
      return [commands, rule === "evenodd"];
    }
    fill(...args) { const [c, e] = this.#pathArgs(args); N.CvPath(this.#id, 0, c, e); }
    stroke(path) { N.CvPath(this.#id, 1, path instanceof Path2D ? new Float64Array(Path2D.commandsOf(path)) : undefined, false); }
    clip(...args) { const [c, e] = this.#pathArgs(args); N.CvPath(this.#id, 2, c, e); }
    isPointInPath(...args) {
      let path;
      if (args[0] instanceof Path2D) path = args.shift();
      const [x, y, rule] = args;
      const [c, e] = this.#pathArgs(path ? [path, rule] : [rule]);
      return !!N.CvPath(this.#id, 3, c, e, num(x), num(y));
    }
    isPointInStroke(...args) {
      let path;
      if (args[0] instanceof Path2D) path = args.shift();
      return !!N.CvPath(this.#id, 4, path ? new Float64Array(Path2D.commandsOf(path)) : undefined, false, num(args[0]), num(args[1]));
    }
    addPath(path, matrix) {  // (not a context method: a Path2D's; here so that the path of the context can take one, as it can in the specification's roundabout way)
      N.CvAddPath(this.#id, new Float64Array(Path2D.commandsOf(path)), ...matrixOf(matrix));
    }
    drawFocusIfNeeded() {}
    scrollPathIntoView() {}

    // images
    drawImage(source, ...a) {
      if (arguments.length < 3) throw new TypeError("Failed to execute 'drawImage' on 'CanvasRenderingContext2D': 3 arguments required, but only " + arguments.length + " present.");
      const picture = pictureOf(source, "drawImage");
      const v = a.map(num);
      if (!picture) return;
      let sx = 0, sy = 0, sw = picture.w, sh = picture.h, dx, dy, dw = picture.w, dh = picture.h;
      if (v.length === 2) [dx, dy] = v;
      else if (v.length === 4) [dx, dy, dw, dh] = v;
      else if (v.length === 8) [sx, sy, sw, sh, dx, dy, dw, dh] = v;
      else throw new TypeError("Failed to execute 'drawImage' on 'CanvasRenderingContext2D': Valid arities are: [3, 5, 9], but " + arguments.length + " arguments provided.");
      try {
        if (!finite(sx, sy, sw, sh, dx, dy, dw, dh) || picture.blank || sw === 0 || sh === 0 || dw === 0 || dh === 0) return;
        N.CvDrawImage(this.#id, picture.id, sx, sy, sw, sh, dx, dy, dw, dh);
      } finally {
        if (picture.temporary) N.ImageFree(picture.id);
      }
    }
    createImageData(a, b, settings) {
      if (a !== null && typeof a === "object" && "data" in a) return new ImageData(a.width, a.height);
      a = Math.trunc(num(a)); b = Math.trunc(num(b));
      if (!a || !b) throw fail("IndexSizeError", "Failed to execute 'createImageData' on 'CanvasRenderingContext2D': The source width is 0.");
      return new ImageData(Math.abs(a), Math.abs(b));
    }
    getImageData(x, y, w, h) {
      if (arguments.length < 4) throw new TypeError("Failed to execute 'getImageData' on 'CanvasRenderingContext2D': 4 arguments required, but only " + arguments.length + " present.");
      x = Math.trunc(num(x)); y = Math.trunc(num(y)); w = Math.trunc(num(w)); h = Math.trunc(num(h));
      if (!finite(x, y, w, h)) throw new TypeError("Failed to execute 'getImageData' on 'CanvasRenderingContext2D': The provided double value is non-finite.");
      if (!w || !h) throw fail("IndexSizeError", "Failed to execute 'getImageData' on 'CanvasRenderingContext2D': The source width is 0.");
      if (w < 0) { x += w; w = -w; }
      if (h < 0) { y += h; h = -h; }
      const bytes = N.CvGetImageData(this.#id, x, y, w, h);
      if (!bytes) throw fail("IndexSizeError", "Failed to execute 'getImageData' on 'CanvasRenderingContext2D': Out of memory.");
      return new ImageData(new Uint8ClampedArray(bytes), w, h);
    }
    putImageData(data, dx, dy, sx, sy, sw, sh) {
      if (arguments.length < 3) throw new TypeError("Failed to execute 'putImageData' on 'CanvasRenderingContext2D': 3 arguments required, but only " + arguments.length + " present.");
      if (!(data && typeof data === "object" && "data" in data)) throw new TypeError("Failed to execute 'putImageData' on 'CanvasRenderingContext2D': parameter 1 is not of type 'ImageData'.");
      dx = Math.trunc(num(dx)); dy = Math.trunc(num(dy));
      let x = 0, y = 0, w = data.width, h = data.height;
      if (arguments.length >= 7) {
        x = Math.trunc(num(sx)); y = Math.trunc(num(sy)); w = Math.trunc(num(sw)); h = Math.trunc(num(sh));
        if (w < 0) { x += w; w = -w; }
        if (h < 0) { y += h; h = -h; }
      }
      N.CvPutImageData(this.#id, data.data, data.width, data.height, dx, dy, x, y, w, h);
    }
  }
  tag(CanvasRenderingContext2D, "CanvasRenderingContext2D");

  // ---- HTMLCanvasElement ----
  const contexts = new WeakMap();  // canvas -> its 2d context
  const sizeOf = (el, name, fallback) => {
    const v = el.getAttribute(name);
    if (v === null) return fallback;
    const n = parseInt(v, 10);
    return Number.isFinite(n) && n >= 0 && n <= 2147483647 ? n : fallback;
  };
  const makeContext = (canvas, w, h) => {
    const id = N.CanvasCreate(w, h);
    if (id < 0) return null;
    const context = new CanvasRenderingContext2D(token, canvas, id);
    contexts.set(canvas, context);
    canvasSurfaces.set(canvas, id);
    if (canvas instanceof HTMLCanvas) N.CanvasAttach(id, canvas);
    return context;
  };
  const resized = (canvas) => {
    const gl = glContexts.get(canvas);
    if (gl) { gl._resize(canvas.width, canvas.height); return; }
    const context = contexts.get(canvas);
    if (!context) return;
    N.CanvasResize(context._surface, canvas.width, canvas.height);
    context._reset(context._surface);
  };
  // WebGL (src/web-api/js/webgl.js) gives the factory of its contexts here, and takes what it needs of the canvas.
  const glContexts = new WeakMap();  // canvas -> its WebGL context
  let glFactory = null;
  const pixelsOf = (source, method) => {
    if (typeof ImageData === "function" && source instanceof ImageData) return { width: source.width, height: source.height, data: new Uint8Array(source.data.buffer, source.data.byteOffset, source.data.length) };
    const picture = pictureOf(source, method);
    if (!picture) return null;
    if (picture.id < 0) return { width: picture.w, height: picture.h, data: new Uint8Array(picture.w * picture.h * 4) };
    const surface = N.CanvasCreate(picture.w, picture.h);
    N.CvDrawImage(surface, picture.id, 0, 0, picture.w, picture.h, 0, 0, picture.w, picture.h);
    const bytes = N.CvGetImageData(surface, 0, 0, picture.w, picture.h);
    N.CanvasFree(surface);
    if (picture.temporary) N.ImageFree(picture.id);
    return bytes ? { width: picture.w, height: picture.h, data: new Uint8Array(bytes) } : null;
  };
  Object.defineProperty(globalThis, "__solarCanvasInternals", { configurable: true, enumerable: false, value: {
    setWebGl(factory) { glFactory = factory; }, attach: N.CanvasAttach, pixelsOf, surfaces: canvasSurfaces, isCanvas: (v) => v instanceof HTMLCanvas || (typeof OffscreenCanvas === "function" && v instanceof OffscreenCanvas),
  } });
  const getAnyContext = (canvas, type, attributes, w, h) => {
    type = String(type);
    if (type === "2d") return glContexts.has(canvas) ? null : (contexts.get(canvas) || makeContext(canvas, w, h));
    if ((type === "webgl" || type === "webgl2" || type === "experimental-webgl") && glFactory) {
      if (contexts.has(canvas)) return null;
      const existing = glContexts.get(canvas);
      if (existing) return existing._kind === (type === "webgl2" ? 2 : 1) ? existing : null;
      const made = glFactory(canvas, type === "webgl2" ? 2 : 1, attributes || {}, w, h);
      if (made) glContexts.set(canvas, made);
      return made;
    }
    return null;
  };
  const canvasProto = HTMLCanvas.prototype;
  const dimension = (name, fallback) => ({
    get() { return sizeOf(this, name, fallback); },
    set(v) { v = Number(v); const n = Number.isFinite(v) && v >= 0 ? Math.trunc(v) % 4294967296 : fallback; this.setAttribute(name, String(n)); resized(this); },
    enumerable: true, configurable: true,
  });
  Object.defineProperty(canvasProto, "width", dimension("width", 300));
  Object.defineProperty(canvasProto, "height", dimension("height", 150));
  define(canvasProto, "getContext", function getContext(type, attributes) {
    if (arguments.length === 0) throw new TypeError("Failed to execute 'getContext' on 'HTMLCanvasElement': 1 argument required, but only 0 present.");
    return getAnyContext(this, type, attributes, this.width, this.height);
  });
  const encode = (isImage, id, type, quality) => {
    const mime = ["image/png", "image/jpeg", "image/webp"].includes(String(type).toLowerCase()) ? String(type).toLowerCase() : "image/png";
    const bytes = N.ImageEncode(isImage, id, mime, typeof quality === "number" ? quality : -1);
    return bytes ? { mime, bytes } : null;
  };
  const dataUrl = (made) => {
    const bytes = new Uint8Array(made.bytes);
    let binary = "";
    for (let i = 0; i < bytes.length; i += 0x8000) binary += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
    return `data:${made.mime};base64,${btoa(binary)}`;
  };
  define(canvasProto, "toDataURL", function toDataURL(type = "image/png", quality) {
    if (this.width === 0 || this.height === 0) return "data:,";
    const gl = glContexts.get(this);
    if (gl) gl._present();
    const surface = gl ? gl._surface : (contexts.get(this) || makeContext(this, this.width, this.height))._surface;
    const made = encode(false, surface, type, quality);
    return made ? dataUrl(made) : "data:,";
  });
  define(canvasProto, "toBlob", function toBlob(callback, type = "image/png", quality) {
    if (typeof callback !== "function") throw new TypeError("Failed to execute 'toBlob' on 'HTMLCanvasElement': The callback provided as parameter 1 is not a function.");
    const gl = glContexts.get(this);
    if (gl) gl._present();
    const context = gl ? { _surface: gl._surface } : this.width && this.height ? (contexts.get(this) || makeContext(this, this.width, this.height)) : null;
    const made = context ? encode(false, context._surface, type, quality) : null;
    setTimeout(() => callback(made ? new Blob([made.bytes], { type: made.mime }) : null), 0);
  });
  // requestPaint(): the canvas says when it has been painted (a paint event, the next task).
  define(canvasProto, "requestPaint", function requestPaint() { setTimeout(() => this.dispatchEvent(new Event("paint")), 0); });
  define(canvasProto, "transferControlToOffscreen", function transferControlToOffscreen() { throw fail("InvalidStateError", "transferControlToOffscreen is not supported."); });

  // ---- OffscreenCanvas ----
  class OffscreenCanvas extends EventTarget {
    #w; #h;
    constructor(w, h) {
      super();
      if (arguments.length < 2) throw new TypeError("Failed to construct 'OffscreenCanvas': 2 arguments required, but only " + arguments.length + " present.");
      this.#w = Math.max(0, Math.trunc(num(w)) || 0); this.#h = Math.max(0, Math.trunc(num(h)) || 0);
    }
    get width() { return this.#w; }
    set width(v) { this.#w = Math.max(0, Math.trunc(num(v)) || 0); resized(this); }
    get height() { return this.#h; }
    set height(v) { this.#h = Math.max(0, Math.trunc(num(v)) || 0); resized(this); }
    getContext(type, attributes) { return getAnyContext(this, type, attributes, this.#w, this.#h); }
    convertToBlob(options = {}) {
      return new Promise((resolve, reject) => {
        if (!this.#w || !this.#h) { reject(fail("IndexSizeError", "The canvas has a width or height of 0.")); return; }
        const context = contexts.get(this) || makeContext(this, this.#w, this.#h);
        const made = encode(false, context._surface, options.type || "image/png", options.quality);
        if (!made) reject(fail("EncodingError", "The image could not be encoded.")); else resolve(new Blob([made.bytes], { type: made.mime }));
      });
    }
    transferToImageBitmap() {
      const context = contexts.get(this) || makeContext(this, this.#w, this.#h);
      const made = N.ImageFromCanvas(context._surface);
      if (!made) throw fail("InvalidStateError", "The canvas could not be transferred.");
      N.CanvasResize(context._surface, this.#w, this.#h);
      context._reset(context._surface);
      return new ImageBitmap(token, made[0], made[1], made[2]);
    }
  }
  tag(OffscreenCanvas, "OffscreenCanvas");

  // ---- createImageBitmap ----
  const bitmapFrom = (picture) => new ImageBitmap(token, picture.id, picture.w, picture.h);
  define(globalThis, "createImageBitmap", function createImageBitmap(source, ...a) {
    return new Promise((resolve, reject) => {
      try {
        let crop = null;
        if (a.length >= 4 && typeof a[0] === "number") crop = a.slice(0, 4).map(num);
        const finish = (picture) => {
          if (!picture) { reject(fail("InvalidStateError", "The source image could not be decoded.")); return; }
          if (!crop) { resolve(bitmapFrom({ ...picture })); return; }
          // A cut of it: drawn on a surface of the size.
          let [sx, sy, sw, sh] = crop;
          if (sw < 0) { sx += sw; sw = -sw; }
          if (sh < 0) { sy += sh; sh = -sh; }
          if (!sw || !sh) { reject(new RangeError("The crop rectangle has a width or height of 0.")); return; }
          const surface = N.CanvasCreate(Math.ceil(sw), Math.ceil(sh));
          N.CvDrawImage(surface, picture.id, sx, sy, sw, sh, 0, 0, Math.ceil(sw), Math.ceil(sh));
          const made = N.ImageFromCanvas(surface);
          N.CanvasFree(surface);
          if (picture.temporary) N.ImageFree(picture.id);
          resolve(new ImageBitmap(token, made[0], made[1], made[2]));
        };
        if (source instanceof Blob) {
          source.arrayBuffer().then((bytes) => { const made = N.ImageDecode(bytes); finish(made ? { id: made[0], w: made[1], h: made[2], temporary: true } : null); }, reject);
        } else if (typeof ImageData === "function" && source instanceof ImageData) {
          const made = N.ImageFromPixels(source.width, source.height, source.data);
          finish(made ? { id: made[0], w: made[1], h: made[2], temporary: true } : null);
        } else if (source instanceof ImageBitmap) {
          const id = ImageBitmap.id(source);
          if (id < 0) { reject(fail("InvalidStateError", "The ImageBitmap is closed.")); return; }
          // (A copy: the bitmap is closed on its own.)
          const surface = N.CanvasCreate(source.width, source.height);
          N.CvDrawImage(surface, id, 0, 0, source.width, source.height, 0, 0, source.width, source.height);
          const made = N.ImageFromCanvas(surface);
          N.CanvasFree(surface);
          finish({ id: made[0], w: made[1], h: made[2], temporary: true });
        } else {
          const picture = pictureOf(source, "createImageBitmap");
          if (!picture) { reject(fail("InvalidStateError", "The source is not ready.")); return; }
          if (picture.temporary) { finish(picture); return; }
          // An image element's picture is shared: it is copied for the bitmap.
          const surface = N.CanvasCreate(picture.w, picture.h);
          if (picture.id >= 0) N.CvDrawImage(surface, picture.id, 0, 0, picture.w, picture.h, 0, 0, picture.w, picture.h);
          const made = N.ImageFromCanvas(surface);
          N.CanvasFree(surface);
          finish({ id: made[0], w: made[1], h: made[2], temporary: true });
        }
      } catch (e) { reject(e); }
    });
  });

  define(globalThis, "CanvasRenderingContext2D", CanvasRenderingContext2D);
  define(globalThis, "OffscreenCanvasRenderingContext2D", CanvasRenderingContext2D);
  define(globalThis, "CanvasGradient", CanvasGradient);
  define(globalThis, "CanvasPattern", CanvasPattern);
  define(globalThis, "Path2D", Path2D);
  define(globalThis, "TextMetrics", TextMetrics);
  define(globalThis, "ImageBitmap", ImageBitmap);
  define(globalThis, "OffscreenCanvas", OffscreenCanvas);

  // The width and height of a canvas that were changed with setAttribute or the markup: its surface follows.
  if (typeof MutationObserver === "function" && typeof document !== "undefined" && HTMLCanvas !== undefined && typeof HTMLCanvasElement === "function") {
    new MutationObserver((records) => {
      for (const r of records) if (r.target instanceof HTMLCanvas && (contexts.has(r.target) || glContexts.has(r.target))) resized(r.target);
    }).observe(document, { attributes: true, subtree: true, attributeFilter: ["width", "height"] });
  }
})();
