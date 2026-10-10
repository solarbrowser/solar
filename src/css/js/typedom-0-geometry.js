// Geometry Interfaces (https://drafts.fxtf.org/geometry/): points, rectangles and matrices.
(function () {
  'use strict';
  const global = globalThis;
  const define = (name, constructor) => {
    Object.defineProperty(constructor.prototype, Symbol.toStringTag, { value: name, configurable: true });
    Object.defineProperty(global, name, { value: constructor, writable: true, configurable: true });
  };
  const methods = (target, members) => {
    for (const key of Reflect.ownKeys(members)) {
      const descriptor = Object.getOwnPropertyDescriptor(members, key);
      descriptor.enumerable = true;
      Object.defineProperty(target, key, descriptor);
    }
  };
  const num = (value) => Number(value);
  const dictNumber = (init, key, fallback) => {
    if (init === undefined || init === null) return fallback;
    const value = init[key];
    return value === undefined ? fallback : Number(value);
  };

  // ---- DOMPoint ----

  class DOMPointReadOnly {
    #x; #y; #z; #w;
    constructor(x = 0, y = 0, z = 0, w = 1) {
      this.#x = num(x);
      this.#y = num(y);
      this.#z = num(z);
      this.#w = num(w);
    }
    get x() { return this.#x; }
    get y() { return this.#y; }
    get z() { return this.#z; }
    get w() { return this.#w; }
    static fromPoint(other = {}) {
      return new this(dictNumber(other, 'x', 0), dictNumber(other, 'y', 0), dictNumber(other, 'z', 0), dictNumber(other, 'w', 1));
    }
    matrixTransform(matrix = {}) {
      const m = matrix instanceof DOMMatrixReadOnly ? matrix : DOMMatrix.fromMatrix(matrix);
      return DOMPoint.fromPoint(m.transformPoint(this));
    }
    toJSON() { return { x: this.x, y: this.y, z: this.z, w: this.w }; }
    // for the subclass to set
    static _set(point, x, y, z, w) {
      point.#x = x; point.#y = y; point.#z = z; point.#w = w;
    }
    static _get(point) { return [point.#x, point.#y, point.#z, point.#w]; }
  }
  define('DOMPointReadOnly', DOMPointReadOnly);
  Object.defineProperty(DOMPointReadOnly, 'length', { value: 0 });

  class DOMPoint extends DOMPointReadOnly {
    get x() { return DOMPointReadOnly._get(this)[0]; }
    set x(value) { const p = DOMPointReadOnly._get(this); DOMPointReadOnly._set(this, num(value), p[1], p[2], p[3]); }
    get y() { return DOMPointReadOnly._get(this)[1]; }
    set y(value) { const p = DOMPointReadOnly._get(this); DOMPointReadOnly._set(this, p[0], num(value), p[2], p[3]); }
    get z() { return DOMPointReadOnly._get(this)[2]; }
    set z(value) { const p = DOMPointReadOnly._get(this); DOMPointReadOnly._set(this, p[0], p[1], num(value), p[3]); }
    get w() { return DOMPointReadOnly._get(this)[3]; }
    set w(value) { const p = DOMPointReadOnly._get(this); DOMPointReadOnly._set(this, p[0], p[1], p[2], num(value)); }
  }
  define('DOMPoint', DOMPoint);
  for (const name of ['x', 'y', 'z', 'w']) {
    const descriptor = Object.getOwnPropertyDescriptor(DOMPoint.prototype, name);
    descriptor.enumerable = true;
    Object.defineProperty(DOMPoint.prototype, name, descriptor);
    const base = Object.getOwnPropertyDescriptor(DOMPointReadOnly.prototype, name);
    base.enumerable = true;
    Object.defineProperty(DOMPointReadOnly.prototype, name, base);
  }

  // ---- DOMRect ----

  class DOMRectReadOnly {
    #x; #y; #width; #height;
    constructor(x = 0, y = 0, width = 0, height = 0) {
      this.#x = num(x);
      this.#y = num(y);
      this.#width = num(width);
      this.#height = num(height);
    }
    get x() { return this.#x; }
    get y() { return this.#y; }
    get width() { return this.#width; }
    get height() { return this.#height; }
    get top() { return Math.min(this.#y, this.#y + this.#height); }
    get right() { return Math.max(this.#x, this.#x + this.#width); }
    get bottom() { return Math.max(this.#y, this.#y + this.#height); }
    get left() { return Math.min(this.#x, this.#x + this.#width); }
    static fromRect(other = {}) {
      return new this(dictNumber(other, 'x', 0), dictNumber(other, 'y', 0), dictNumber(other, 'width', 0), dictNumber(other, 'height', 0));
    }
    toJSON() {
      return { x: this.x, y: this.y, width: this.width, height: this.height, top: this.top, right: this.right, bottom: this.bottom, left: this.left };
    }
    static _set(rect, x, y, width, height) {
      rect.#x = x; rect.#y = y; rect.#width = width; rect.#height = height;
    }
    static _get(rect) { return [rect.#x, rect.#y, rect.#width, rect.#height]; }
  }
  define('DOMRectReadOnly', DOMRectReadOnly);
  Object.defineProperty(DOMRectReadOnly, 'length', { value: 0 });

  class DOMRect extends DOMRectReadOnly {
    get x() { return DOMRectReadOnly._get(this)[0]; }
    set x(value) { const r = DOMRectReadOnly._get(this); DOMRectReadOnly._set(this, num(value), r[1], r[2], r[3]); }
    get y() { return DOMRectReadOnly._get(this)[1]; }
    set y(value) { const r = DOMRectReadOnly._get(this); DOMRectReadOnly._set(this, r[0], num(value), r[2], r[3]); }
    get width() { return DOMRectReadOnly._get(this)[2]; }
    set width(value) { const r = DOMRectReadOnly._get(this); DOMRectReadOnly._set(this, r[0], r[1], num(value), r[3]); }
    get height() { return DOMRectReadOnly._get(this)[3]; }
    set height(value) { const r = DOMRectReadOnly._get(this); DOMRectReadOnly._set(this, r[0], r[1], r[2], num(value)); }
  }
  define('DOMRect', DOMRect);
  for (const name of ['x', 'y', 'width', 'height']) {
    for (const target of [DOMRect.prototype, DOMRectReadOnly.prototype]) {
      const descriptor = Object.getOwnPropertyDescriptor(target, name);
      descriptor.enumerable = true;
      Object.defineProperty(target, name, descriptor);
    }
  }
  for (const name of ['top', 'right', 'bottom', 'left']) {
    const descriptor = Object.getOwnPropertyDescriptor(DOMRectReadOnly.prototype, name);
    descriptor.enumerable = true;
    Object.defineProperty(DOMRectReadOnly.prototype, name, descriptor);
  }

  // ---- DOMQuad ----

  class DOMQuad {
    #points;
    constructor(p1 = {}, p2 = {}, p3 = {}, p4 = {}) {
      this.#points = [p1, p2, p3, p4].map((p) => DOMPoint.fromPoint(p));
    }
    get p1() { return this.#points[0]; }
    get p2() { return this.#points[1]; }
    get p3() { return this.#points[2]; }
    get p4() { return this.#points[3]; }
    static fromRect(other = {}) {
      const x = dictNumber(other, 'x', 0), y = dictNumber(other, 'y', 0), w = dictNumber(other, 'width', 0), h = dictNumber(other, 'height', 0);
      return new DOMQuad({ x, y }, { x: x + w, y }, { x: x + w, y: y + h }, { x, y: y + h });
    }
    static fromQuad(other = {}) { return new DOMQuad(other.p1, other.p2, other.p3, other.p4); }
    getBounds() {
      const xs = this.#points.map((p) => p.x), ys = this.#points.map((p) => p.y);
      const left = Math.min(...xs), top = Math.min(...ys);
      return new DOMRect(left, top, Math.max(...xs) - left, Math.max(...ys) - top);
    }
    toJSON() { return { p1: this.p1.toJSON(), p2: this.p2.toJSON(), p3: this.p3.toJSON(), p4: this.p4.toJSON() }; }
  }
  define('DOMQuad', DOMQuad);
  for (const name of ['p1', 'p2', 'p3', 'p4']) {
    const descriptor = Object.getOwnPropertyDescriptor(DOMQuad.prototype, name);
    descriptor.enumerable = true;
    Object.defineProperty(DOMQuad.prototype, name, descriptor);
  }

  // ---- DOMMatrix ----

  // The sixteen values are column-major: m11 m12 m13 m14 m21 ...
  const names2d = { a: 0, b: 1, c: 4, d: 5, e: 12, f: 13 };
  const identity = () => [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
  const store = new WeakMap();
  const state = (matrix) => {
    const s = store.get(matrix);
    if (!s) throw new TypeError('Illegal invocation');
    return s;
  };

  const multiplyValues = (a, b) => {
    // a * b, as the matrices: result = a x b with the column-major layout.
    const out = new Array(16).fill(0);
    for (let col = 0; col < 4; ++col) {
      for (let row = 0; row < 4; ++row) {
        let sum = 0;
        for (let k = 0; k < 4; ++k) sum += a[k * 4 + row] * b[col * 4 + k];
        out[col * 4 + row] = sum;
      }
    }
    return out;
  };
  const invertValues = (m) => {
    const a = m.slice();
    const inv = identity();
    // Gauss-Jordan with partial pivoting.
    for (let col = 0; col < 4; ++col) {
      let pivot = col;
      for (let row = col + 1; row < 4; ++row) if (Math.abs(a[col * 4 + row]) > Math.abs(a[col * 4 + pivot])) pivot = row;
      if (a[col * 4 + pivot] === 0 || !Number.isFinite(a[col * 4 + pivot])) return null;
      if (pivot !== col) {
        for (let c = 0; c < 4; ++c) {
          [a[c * 4 + col], a[c * 4 + pivot]] = [a[c * 4 + pivot], a[c * 4 + col]];
          [inv[c * 4 + col], inv[c * 4 + pivot]] = [inv[c * 4 + pivot], inv[c * 4 + col]];
        }
      }
      const scale = a[col * 4 + col];
      for (let c = 0; c < 4; ++c) {
        a[c * 4 + col] /= scale;
        inv[c * 4 + col] /= scale;
      }
      for (let row = 0; row < 4; ++row) {
        if (row === col) continue;
        const factor = a[col * 4 + row];
        if (factor === 0) continue;
        for (let c = 0; c < 4; ++c) {
          a[c * 4 + row] -= factor * a[c * 4 + col];
          inv[c * 4 + row] -= factor * inv[c * 4 + col];
        }
      }
    }
    return inv.every(Number.isFinite) ? inv : null;
  };
  const translation = (x, y, z) => { const m = identity(); m[12] = x; m[13] = y; m[14] = z; return m; };
  const scaling = (x, y, z) => { const m = identity(); m[0] = x; m[5] = y; m[10] = z; return m; };
  const rotationAxis = (x, y, z, degrees) => {
    const length = Math.hypot(x, y, z);
    if (length === 0) return identity();
    x /= length; y /= length; z /= length;
    const half = (degrees * Math.PI) / 360;
    const sc = Math.sin(half) * Math.cos(half);
    const sq = Math.sin(half) * Math.sin(half);
    const m = identity();
    m[0] = 1 - 2 * (y * y + z * z) * sq;
    m[1] = 2 * (x * y * sq + z * sc);
    m[2] = 2 * (x * z * sq - y * sc);
    m[4] = 2 * (x * y * sq - z * sc);
    m[5] = 1 - 2 * (x * x + z * z) * sq;
    m[6] = 2 * (y * z * sq + x * sc);
    m[8] = 2 * (x * z * sq + y * sc);
    m[9] = 2 * (y * z * sq - x * sc);
    m[10] = 1 - 2 * (x * x + y * y) * sq;
    return m;
  };
  const skewing = (xDegrees, yDegrees) => {
    const m = identity();
    m[4] = Math.tan((xDegrees * Math.PI) / 180);
    m[1] = Math.tan((yDegrees * Math.PI) / 180);
    return m;
  };

  const fromSequence = (init) => {
    const values = [...init].map(Number);
    if (values.length === 6) {
      const m = identity();
      m[0] = values[0]; m[1] = values[1]; m[4] = values[2]; m[5] = values[3]; m[12] = values[4]; m[13] = values[5];
      return { values: m, is2D: true };
    }
    if (values.length === 16) return { values, is2D: false };
    throw new TypeError('The sequence must contain 6 or 16 elements');
  };

  // A transform list of a string, for the matrix constructor and setMatrixValue.
  const parseTransformList = (text) => {
    text = String(text).trim();
    if (text === '' || text === 'none') return { values: identity(), is2D: true };
    let result = identity();
    let is2D = true;
    const re = /\s*([a-zA-Z0-9]+)\(([^)]*)\)\s*/y;
    let position = 0;
    while (position < text.length) {
      re.lastIndex = position;
      const match = re.exec(text);
      if (!match) throw new DOMException('Failed to parse the transform list', 'SyntaxError');
      position = re.lastIndex;
      const name = match[1].toLowerCase();
      const args = match[2].split(',').map((a) => a.trim()).filter((a) => a !== '');
      const parse = (value, kind) => {
        const m = /^([+-]?(?:\d+\.?\d*|\.\d+)(?:e[+-]?\d+)?)([a-z%]*)$/i.exec(value);
        if (!m) throw new DOMException('Bad value ' + value, 'SyntaxError');
        const number = Number(m[1]);
        const unit = m[2].toLowerCase();
        if (kind === 'number') {
          if (unit !== '') throw new DOMException('Bad value ' + value, 'SyntaxError');
          return number;
        }
        if (kind === 'length') {
          if (unit === 'px') return number;
          if (unit === '' && number === 0) return 0;
          const factors = { cm: 96 / 2.54, mm: 96 / 25.4, q: 96 / 101.6, in: 96, pt: 96 / 72, pc: 16 };
          if (factors[unit]) return number * factors[unit];
          throw new DOMException('Relative or unknown length ' + value, 'SyntaxError');
        }
        // angle
        if (unit === 'deg') return number;
        if (unit === 'rad') return (number * 180) / Math.PI;
        if (unit === 'grad') return number * 0.9;
        if (unit === 'turn') return number * 360;
        if (unit === '' && number === 0) return 0;
        throw new DOMException('Bad angle ' + value, 'SyntaxError');
      };
      let matrix;
      switch (name) {
        case 'matrix': {
          if (args.length !== 6) throw new DOMException('matrix() takes six numbers', 'SyntaxError');
          const v = args.map((a) => parse(a, 'number'));
          matrix = identity();
          matrix[0] = v[0]; matrix[1] = v[1]; matrix[4] = v[2]; matrix[5] = v[3]; matrix[12] = v[4]; matrix[13] = v[5];
          break;
        }
        case 'matrix3d': {
          if (args.length !== 16) throw new DOMException('matrix3d() takes sixteen numbers', 'SyntaxError');
          matrix = args.map((a) => parse(a, 'number'));
          is2D = false;
          break;
        }
        case 'translate': case 'translatex': case 'translatey': case 'translatez': case 'translate3d': {
          const v = args.map((a) => parse(a, 'length'));
          let x = 0, y = 0, z = 0;
          if (name === 'translate') [x, y = 0] = v;
          else if (name === 'translatex') [x] = v;
          else if (name === 'translatey') [y] = v;
          else if (name === 'translatez') [z] = v;
          else [x, y, z] = v;
          if (name === 'translatez' || name === 'translate3d') is2D = false;
          matrix = translation(x, y, z);
          break;
        }
        case 'scale': case 'scalex': case 'scaley': case 'scalez': case 'scale3d': {
          const v = args.map((a) => parse(a, 'number'));
          let x = 1, y = 1, z = 1;
          if (name === 'scale') { [x, y = x] = v; }
          else if (name === 'scalex') [x] = v;
          else if (name === 'scaley') [y] = v;
          else if (name === 'scalez') [z] = v;
          else [x, y, z] = v;
          if (name === 'scalez' || name === 'scale3d') is2D = false;
          matrix = scaling(x, y, z);
          break;
        }
        case 'rotate': case 'rotatez': matrix = rotationAxis(0, 0, 1, parse(args[0], 'angle')); break;
        case 'rotatex': matrix = rotationAxis(1, 0, 0, parse(args[0], 'angle')); is2D = false; break;
        case 'rotatey': matrix = rotationAxis(0, 1, 0, parse(args[0], 'angle')); is2D = false; break;
        case 'rotate3d': {
          const v = args.slice(0, 3).map((a) => parse(a, 'number'));
          matrix = rotationAxis(v[0], v[1], v[2], parse(args[3], 'angle'));
          is2D = false;
          break;
        }
        case 'skew': matrix = skewing(parse(args[0], 'angle'), args[1] === undefined ? 0 : parse(args[1], 'angle')); break;
        case 'skewx': matrix = skewing(parse(args[0], 'angle'), 0); break;
        case 'skewy': matrix = skewing(0, parse(args[0], 'angle')); break;
        case 'perspective': {
          const length = parse(args[0], 'length');
          matrix = identity();
          matrix[11] = length === 0 ? 0 : -1 / length;
          is2D = false;
          break;
        }
        default: throw new DOMException('Unknown transform function ' + name, 'SyntaxError');
      }
      result = multiplyValues(result, matrix);
    }
    return { values: result, is2D };
  };

  const fromInit = (init) => {
    if (init === undefined) return { values: identity(), is2D: true };
    if (typeof init === 'string') return parseTransformList(init);
    if (init !== null && typeof init === 'object' && typeof init[Symbol.iterator] === 'function') return fromSequence(init);
    throw new TypeError('Failed to construct a DOMMatrix');
  };
  const matrixFromDictionary = (init = {}) => {
    const has = (key) => init[key] !== undefined;
    const names3d = ['m13', 'm14', 'm23', 'm24', 'm31', 'm32', 'm33', 'm34', 'm43', 'm44'];
    const is2DKnown = init.is2D !== undefined;
    let is2D = is2DKnown ? Boolean(init.is2D) : !names3d.some((n) => has(n) && Number(init[n]) !== (n === 'm33' || n === 'm44' ? 1 : 0));
    const values = identity();
    const pick = (long, short, fallback) => {
      if (has(long) && has(short) && !Object.is(Number(init[long]), Number(init[short]))) throw new TypeError(long + ' and ' + short + ' differ');
      if (has(long)) return Number(init[long]);
      if (has(short)) return Number(init[short]);
      return fallback;
    };
    values[0] = pick('m11', 'a', 1);
    values[1] = pick('m12', 'b', 0);
    values[4] = pick('m21', 'c', 0);
    values[5] = pick('m22', 'd', 1);
    values[12] = pick('m41', 'e', 0);
    values[13] = pick('m42', 'f', 0);
    if (!is2D || !is2DKnown) {
      const map = { m13: 2, m14: 3, m23: 6, m24: 7, m31: 8, m32: 9, m33: 10, m34: 11, m43: 14, m44: 15 };
      for (const [key, index] of Object.entries(map)) if (has(key)) values[index] = Number(init[key]);
    }
    if (is2DKnown && is2D) {
      const flat = { m13: 0, m14: 0, m23: 0, m24: 0, m31: 0, m32: 0, m33: 1, m34: 0, m43: 0, m44: 1 };
      for (const [key, value] of Object.entries(flat)) if (has(key) && Number(init[key]) !== value) throw new TypeError(key + ' must be ' + value + ' for a 2D matrix');
    }
    return { values, is2D };
  };

  class DOMMatrixReadOnly {
    constructor(init) {
      const { values, is2D } = fromInit(init);
      store.set(this, { values, is2D });
    }
    get a() { return state(this).values[0]; }
    get b() { return state(this).values[1]; }
    get c() { return state(this).values[4]; }
    get d() { return state(this).values[5]; }
    get e() { return state(this).values[12]; }
    get f() { return state(this).values[13]; }
    get is2D() { return state(this).is2D; }
    get isIdentity() {
      const v = state(this).values;
      const id = identity();
      return v.every((n, i) => n === id[i]);
    }
    translate(tx = 0, ty = 0, tz = 0) { return DOMMatrix.fromMatrix(this).translateSelf(tx, ty, tz); }
    scale(scaleX = 1, scaleY = scaleX, scaleZ = 1, originX = 0, originY = 0, originZ = 0) { return DOMMatrix.fromMatrix(this).scaleSelf(scaleX, scaleY, scaleZ, originX, originY, originZ); }
    scaleNonUniform(scaleX = 1, scaleY = 1) { return DOMMatrix.fromMatrix(this).scaleSelf(scaleX, scaleY, 1, 0, 0, 0); }
    scale3d(scale = 1, originX = 0, originY = 0, originZ = 0) { return DOMMatrix.fromMatrix(this).scale3dSelf(scale, originX, originY, originZ); }
    rotate(rotX = 0, rotY, rotZ) { return DOMMatrix.fromMatrix(this).rotateSelf(rotX, rotY, rotZ); }
    rotateFromVector(x = 0, y = 0) { return DOMMatrix.fromMatrix(this).rotateFromVectorSelf(x, y); }
    rotateAxisAngle(x = 0, y = 0, z = 0, angle = 0) { return DOMMatrix.fromMatrix(this).rotateAxisAngleSelf(x, y, z, angle); }
    skewX(sx = 0) { return DOMMatrix.fromMatrix(this).skewXSelf(sx); }
    skewY(sy = 0) { return DOMMatrix.fromMatrix(this).skewYSelf(sy); }
    multiply(other = {}) { return DOMMatrix.fromMatrix(this).multiplySelf(other); }
    flipX() { return DOMMatrix.fromMatrix(this).multiplySelf(new DOMMatrix([-1, 0, 0, 1, 0, 0])); }
    flipY() { return DOMMatrix.fromMatrix(this).multiplySelf(new DOMMatrix([1, 0, 0, -1, 0, 0])); }
    inverse() { return DOMMatrix.fromMatrix(this).invertSelf(); }
    transformPoint(point = {}) {
      const x = dictNumber(point, 'x', 0), y = dictNumber(point, 'y', 0), z = dictNumber(point, 'z', 0), w = dictNumber(point, 'w', 1);
      const m = state(this).values;
      return new DOMPoint(
        m[0] * x + m[4] * y + m[8] * z + m[12] * w,
        m[1] * x + m[5] * y + m[9] * z + m[13] * w,
        m[2] * x + m[6] * y + m[10] * z + m[14] * w,
        m[3] * x + m[7] * y + m[11] * z + m[15] * w);
    }
    toFloat32Array() { return new Float32Array(state(this).values); }
    toFloat64Array() { return new Float64Array(state(this).values); }
    toJSON() {
      const s = state(this);
      const json = {};
      for (const name of ['a', 'b', 'c', 'd', 'e', 'f', 'm11', 'm12', 'm13', 'm14', 'm21', 'm22', 'm23', 'm24', 'm31', 'm32', 'm33', 'm34', 'm41', 'm42', 'm43', 'm44']) json[name] = this[name];
      json.is2D = s.is2D;
      json.isIdentity = this.isIdentity;
      return json;
    }
    toString() {
      const s = state(this);
      if (!s.values.every(Number.isFinite)) throw new DOMException('The matrix has values that are not finite', 'InvalidStateError');
      const text = (n) => String(Object.is(n, -0) ? 0 : n);
      const v = s.values;
      if (s.is2D) return 'matrix(' + [v[0], v[1], v[4], v[5], v[12], v[13]].map(text).join(', ') + ')';
      return 'matrix3d(' + v.map(text).join(', ') + ')';
    }
    static fromMatrix(other = {}) {
      if (other instanceof DOMMatrixReadOnly) {
        const s = state(other);
        const matrix = new this();
        store.set(matrix, { values: s.values.slice(), is2D: s.is2D });
        return matrix;
      }
      const { values, is2D } = matrixFromDictionary(other);
      const matrix = new this();
      store.set(matrix, { values, is2D });
      return matrix;
    }
    static fromFloat32Array(array) {
      const matrix = new this(Array.from(array));
      return matrix;
    }
    static fromFloat64Array(array) {
      const matrix = new this(Array.from(array));
      return matrix;
    }
  }
  define('DOMMatrixReadOnly', DOMMatrixReadOnly);
  Object.defineProperty(DOMMatrixReadOnly, 'length', { value: 0 });

  const values16 = ['m11', 'm12', 'm13', 'm14', 'm21', 'm22', 'm23', 'm24', 'm31', 'm32', 'm33', 'm34', 'm41', 'm42', 'm43', 'm44'];
  values16.forEach((name, index) => {
    Object.defineProperty(DOMMatrixReadOnly.prototype, name, { get() { return state(this).values[index]; }, enumerable: true, configurable: true });
  });
  for (const name of ['a', 'b', 'c', 'd', 'e', 'f', 'is2D', 'isIdentity']) {
    const descriptor = Object.getOwnPropertyDescriptor(DOMMatrixReadOnly.prototype, name);
    descriptor.enumerable = true;
    Object.defineProperty(DOMMatrixReadOnly.prototype, name, descriptor);
  }

  class DOMMatrix extends DOMMatrixReadOnly {
    constructor(init) { super(init); }
    multiplySelf(other = {}) {
      const o = other instanceof DOMMatrixReadOnly ? state(other) : matrixFromDictionary(other);
      const s = state(this);
      s.values = multiplyValues(s.values, o.values);
      s.is2D = s.is2D && o.is2D;
      return this;
    }
    preMultiplySelf(other = {}) {
      const o = other instanceof DOMMatrixReadOnly ? state(other) : matrixFromDictionary(other);
      const s = state(this);
      s.values = multiplyValues(o.values, s.values);
      s.is2D = s.is2D && o.is2D;
      return this;
    }
    translateSelf(tx = 0, ty = 0, tz = 0) {
      const s = state(this);
      s.values = multiplyValues(s.values, translation(num(tx), num(ty), num(tz)));
      if (num(tz) !== 0) s.is2D = false;
      return this;
    }
    scaleSelf(scaleX = 1, scaleY = scaleX, scaleZ = 1, originX = 0, originY = 0, originZ = 0) {
      const s = state(this);
      scaleX = num(scaleX); scaleY = num(scaleY); scaleZ = num(scaleZ);
      originX = num(originX); originY = num(originY); originZ = num(originZ);
      this.translateSelf(originX, originY, originZ);
      s.values = multiplyValues(s.values, scaling(scaleX, scaleY, scaleZ));
      if (scaleZ !== 1 || originZ !== 0) s.is2D = false;
      this.translateSelf(-originX, -originY, -originZ);
      return this;
    }
    scale3dSelf(scale = 1, originX = 0, originY = 0, originZ = 0) { return this.scaleSelf(scale, scale, scale, originX, originY, originZ); }
    rotateSelf(rotX = 0, rotY, rotZ) {
      if (rotY === undefined && rotZ === undefined) {
        rotZ = rotX;
        rotX = 0;
        rotY = 0;
      }
      rotY = rotY === undefined ? 0 : num(rotY);
      rotZ = rotZ === undefined ? 0 : num(rotZ);
      rotX = num(rotX);
      const s = state(this);
      if (rotX !== 0 || rotY !== 0) s.is2D = false;
      if (rotZ !== 0) s.values = multiplyValues(s.values, rotationAxis(0, 0, 1, rotZ));
      if (rotY !== 0) s.values = multiplyValues(s.values, rotationAxis(0, 1, 0, rotY));
      if (rotX !== 0) s.values = multiplyValues(s.values, rotationAxis(1, 0, 0, rotX));
      return this;
    }
    rotateFromVectorSelf(x = 0, y = 0) {
      x = num(x); y = num(y);
      const s = state(this);
      if (x === 0 && y === 0) return this;
      s.values = multiplyValues(s.values, rotationAxis(0, 0, 1, (Math.atan2(y, x) * 180) / Math.PI));
      return this;
    }
    rotateAxisAngleSelf(x = 0, y = 0, z = 0, angle = 0) {
      x = num(x); y = num(y); z = num(z); angle = num(angle);
      const s = state(this);
      s.values = multiplyValues(s.values, rotationAxis(x, y, z, angle));
      if (x !== 0 || y !== 0) s.is2D = false;
      return this;
    }
    skewXSelf(sx = 0) { const s = state(this); s.values = multiplyValues(s.values, skewing(num(sx), 0)); return this; }
    skewYSelf(sy = 0) { const s = state(this); s.values = multiplyValues(s.values, skewing(0, num(sy))); return this; }
    invertSelf() {
      const s = state(this);
      const inverse = invertValues(s.values);
      if (inverse === null) {
        s.values = s.values.map(() => NaN);
        s.is2D = false;
      } else {
        s.values = inverse;
      }
      return this;
    }
    setMatrixValue(transformList) {
      const { values, is2D } = parseTransformList(transformList);
      const s = state(this);
      s.values = values;
      s.is2D = is2D;
      return this;
    }
    static fromMatrix(other) { return DOMMatrixReadOnly.fromMatrix.call(this, other); }
  }
  define('DOMMatrix', DOMMatrix);
  const setters = {
    a: 0, b: 1, c: 4, d: 5, e: 12, f: 13,
  };
  values16.forEach((name, index) => {
    Object.defineProperty(DOMMatrix.prototype, name, {
      get() { return state(this).values[index]; },
      set(value) {
        const s = state(this);
        s.values[index] = num(value);
        if (!names2dSet.has(index) && num(value) !== (index === 10 || index === 15 ? 1 : 0)) s.is2D = false;
      },
      enumerable: true,
      configurable: true,
    });
  });
  const names2dSet = new Set([0, 1, 4, 5, 12, 13]);
  for (const [name, index] of Object.entries(setters)) {
    Object.defineProperty(DOMMatrix.prototype, name, {
      get() { return state(this).values[index]; },
      set(value) { state(this).values[index] = num(value); },
      enumerable: true,
      configurable: true,
    });
  }
  Object.defineProperty(DOMMatrix.prototype, 'is2D', { get() { return state(this).is2D; }, enumerable: true, configurable: true });
  void names2d;
  methods;
})();
