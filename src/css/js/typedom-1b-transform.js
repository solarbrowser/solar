// CSS Typed OM: transform values and their components.
(function () {
  'use strict';
  const global = globalThis;
  const T = global.__solarTypedOm;
  const { INTERNAL, defineInterface, rectify, CSSStyleValue, CSSNumericValue, CSSUnitValue, CSSKeywordValue } = T;

  const dictionaryOf = (value) => value.type();
  // The base types a numeric value can be taken as; a lone percent counts for lengths where the property takes one.
  const keysOf = (dictionary) => Object.keys(dictionary).filter((k) => k !== 'percentHint');
  const isNumber = (value) => keysOf(dictionaryOf(value)).length === 0;
  const isAngle = (value) => {
    const d = dictionaryOf(value);
    return keysOf(d).length === 1 && d.angle === 1;
  };
  const isLength = (value) => {
    const d = dictionaryOf(value);
    return keysOf(d).length === 1 && d.length === 1;
  };
  const isLengthPercentage = (value) => {
    const d = dictionaryOf(value);
    const keys = keysOf(d);
    return keys.length === 1 && (d.length === 1 || d.percent === 1);
  };
  const numberish = (value) => rectify(value);
  const check = (value, test, what) => {
    const v = rectify(value);
    if (!test(v)) throw new TypeError('The value must be ' + what);
    return v;
  };

  const absolute = (value, unit) => {
    // Relative units, and percentages, cannot become a matrix.
    try {
      return value.to(unit).value;
    } catch (e) {
      throw new TypeError('The value cannot be converted to ' + unit);
    }
  };
  const matrixOf = (matrix, is2D) => {
    if (!is2D) return matrix;
    const flat = new DOMMatrix([matrix.m11, matrix.m12, matrix.m21, matrix.m22, matrix.m41, matrix.m42]);
    return flat;
  };

  // ---- CSSTransformComponent ----

  class CSSTransformComponent {
    constructor() {
      if (new.target === CSSTransformComponent) T.illegal();
    }
    get is2D() { return this[INTERNAL].is2D; }
    set is2D(value) { this[INTERNAL].is2D = Boolean(value); }
    toMatrix() { throw new TypeError('Not implemented'); }
  }
  defineInterface('CSSTransformComponent', CSSTransformComponent);
  Object.defineProperty(CSSTransformComponent.prototype, 'is2D', { enumerable: true });

  const init = (object, state) => Object.defineProperty(object, INTERNAL, { value: state, enumerable: false });
  const accessor = (cls, name, validate) => {
    Object.defineProperty(cls.prototype, name, {
      get() { return this[INTERNAL][name]; },
      set(value) { this[INTERNAL][name] = validate(value); },
      enumerable: true,
      configurable: true,
    });
  };

  class CSSTranslate extends CSSTransformComponent {
    constructor(x, y, z) {
      super();
      if (arguments.length < 2) throw new TypeError("Failed to construct 'CSSTranslate': 2 arguments required");
      init(this, {
        x: check(x, isLengthPercentage, 'a length or percentage'),
        y: check(y, isLengthPercentage, 'a length or percentage'),
        z: z === undefined ? new CSSUnitValue(0, 'px') : check(z, isLength, 'a length'),
        is2D: z === undefined,
      });
    }
    toMatrix() {
      const s = this[INTERNAL];
      const matrix = new DOMMatrix().translateSelf(absolute(s.x, 'px'), absolute(s.y, 'px'), s.is2D ? 0 : absolute(s.z, 'px'));
      return matrixOf(matrix, s.is2D);
    }
    toString() {
      const s = this[INTERNAL];
      return s.is2D ? 'translate(' + s.x + ', ' + s.y + ')' : 'translate3d(' + s.x + ', ' + s.y + ', ' + s.z + ')';
    }
  }
  defineInterface('CSSTranslate', CSSTranslate);
  accessor(CSSTranslate, 'x', (v) => check(v, isLengthPercentage, 'a length or percentage'));
  accessor(CSSTranslate, 'y', (v) => check(v, isLengthPercentage, 'a length or percentage'));
  accessor(CSSTranslate, 'z', (v) => check(v, isLength, 'a length'));

  class CSSRotate extends CSSTransformComponent {
    constructor(...args) {
      super();
      if (args.length === 1) {
        init(this, { x: new CSSUnitValue(0, 'number'), y: new CSSUnitValue(0, 'number'), z: new CSSUnitValue(1, 'number'), angle: check(args[0], isAngle, 'an angle'), is2D: true });
      } else if (args.length >= 4) {
        init(this, {
          x: check(args[0], isNumber, 'a number'),
          y: check(args[1], isNumber, 'a number'),
          z: check(args[2], isNumber, 'a number'),
          angle: check(args[3], isAngle, 'an angle'),
          is2D: false,
        });
      } else {
        throw new TypeError("Failed to construct 'CSSRotate': 1 or 4 arguments required");
      }
    }
    toMatrix() {
      const s = this[INTERNAL];
      const angle = absolute(s.angle, 'deg');
      const matrix = s.is2D ? new DOMMatrix().rotateSelf(angle) : new DOMMatrix().rotateAxisAngleSelf(absolute(s.x, 'number'), absolute(s.y, 'number'), absolute(s.z, 'number'), angle);
      return matrixOf(matrix, s.is2D);
    }
    toString() {
      const s = this[INTERNAL];
      return s.is2D ? 'rotate(' + s.angle + ')' : 'rotate3d(' + s.x + ', ' + s.y + ', ' + s.z + ', ' + s.angle + ')';
    }
  }
  defineInterface('CSSRotate', CSSRotate);
  accessor(CSSRotate, 'x', (v) => check(v, isNumber, 'a number'));
  accessor(CSSRotate, 'y', (v) => check(v, isNumber, 'a number'));
  accessor(CSSRotate, 'z', (v) => check(v, isNumber, 'a number'));
  accessor(CSSRotate, 'angle', (v) => check(v, isAngle, 'an angle'));

  class CSSScale extends CSSTransformComponent {
    constructor(x, y, z) {
      super();
      if (arguments.length < 2) throw new TypeError("Failed to construct 'CSSScale': 2 arguments required");
      init(this, {
        x: check(x, isNumber, 'a number'),
        y: check(y, isNumber, 'a number'),
        z: z === undefined ? new CSSUnitValue(1, 'number') : check(z, isNumber, 'a number'),
        is2D: z === undefined,
      });
    }
    toMatrix() {
      const s = this[INTERNAL];
      const matrix = new DOMMatrix().scaleSelf(absolute(s.x, 'number'), absolute(s.y, 'number'), s.is2D ? 1 : absolute(s.z, 'number'));
      return matrixOf(matrix, s.is2D);
    }
    toString() {
      const s = this[INTERNAL];
      return s.is2D ? 'scale(' + s.x + ', ' + s.y + ')' : 'scale3d(' + s.x + ', ' + s.y + ', ' + s.z + ')';
    }
  }
  defineInterface('CSSScale', CSSScale);
  accessor(CSSScale, 'x', (v) => check(v, isNumber, 'a number'));
  accessor(CSSScale, 'y', (v) => check(v, isNumber, 'a number'));
  accessor(CSSScale, 'z', (v) => check(v, isNumber, 'a number'));

  class CSSSkew extends CSSTransformComponent {
    constructor(ax, ay) {
      super();
      if (arguments.length < 2) throw new TypeError("Failed to construct 'CSSSkew': 2 arguments required");
      init(this, { ax: check(ax, isAngle, 'an angle'), ay: check(ay, isAngle, 'an angle'), is2D: true });
    }
    toMatrix() {
      const s = this[INTERNAL];
      return new DOMMatrix().skewXSelf(absolute(s.ax, 'deg')).skewYSelf(absolute(s.ay, 'deg'));
    }
    toString() {
      const s = this[INTERNAL];
      return s.ay instanceof CSSUnitValue && s.ay.value === 0 ? 'skew(' + s.ax + ')' : 'skew(' + s.ax + ', ' + s.ay + ')';
    }
  }
  defineInterface('CSSSkew', CSSSkew);
  accessor(CSSSkew, 'ax', (v) => check(v, isAngle, 'an angle'));
  accessor(CSSSkew, 'ay', (v) => check(v, isAngle, 'an angle'));

  class CSSSkewX extends CSSTransformComponent {
    constructor(ax) {
      super();
      if (arguments.length < 1) throw new TypeError("Failed to construct 'CSSSkewX': 1 argument required");
      init(this, { ax: check(ax, isAngle, 'an angle'), is2D: true });
    }
    toMatrix() { return new DOMMatrix().skewXSelf(absolute(this[INTERNAL].ax, 'deg')); }
    toString() { return 'skewX(' + this[INTERNAL].ax + ')'; }
  }
  defineInterface('CSSSkewX', CSSSkewX);
  accessor(CSSSkewX, 'ax', (v) => check(v, isAngle, 'an angle'));

  class CSSSkewY extends CSSTransformComponent {
    constructor(ay) {
      super();
      if (arguments.length < 1) throw new TypeError("Failed to construct 'CSSSkewY': 1 argument required");
      init(this, { ay: check(ay, isAngle, 'an angle'), is2D: true });
    }
    toMatrix() { return new DOMMatrix().skewYSelf(absolute(this[INTERNAL].ay, 'deg')); }
    toString() { return 'skewY(' + this[INTERNAL].ay + ')'; }
  }
  defineInterface('CSSSkewY', CSSSkewY);
  accessor(CSSSkewY, 'ay', (v) => check(v, isAngle, 'an angle'));

  for (const cls of [CSSSkew, CSSSkewX, CSSSkewY]) {
    Object.defineProperty(cls.prototype, 'is2D', { get() { return true; }, set(value) {}, enumerable: true, configurable: true });
  }
  const perspectiveLength = (value) => {
    if (typeof value === 'string') {
      if (value.toLowerCase() === 'none') return new CSSKeywordValue('none');
      throw new TypeError('The perspective must be a length or none');
    }
    if (value instanceof CSSKeywordValue) {
      if (value.value.toLowerCase() !== 'none') throw new TypeError('The perspective must be a length or none');
      return value;
    }
    return check(value, isLength, 'a length or none');
  };
  class CSSPerspective extends CSSTransformComponent {
    constructor(length) {
      super();
      if (arguments.length < 1) throw new TypeError("Failed to construct 'CSSPerspective': 1 argument required");
      init(this, { length: perspectiveLength(length), is2D: false });
    }
    get is2D() { return false; }
    set is2D(value) {}
    toMatrix() {
      const length = this[INTERNAL].length;
      const matrix = new DOMMatrix();
      if (length instanceof CSSKeywordValue) return matrix;
      const px = absolute(length, 'px');
      matrix.m34 = px === 0 ? 0 : -1 / px;
      return matrixOf(matrix, this[INTERNAL].is2D);
    }
    toString() {
      const length = this[INTERNAL].length;
      return 'perspective(' + (length instanceof CSSUnitValue && length.value < 0 ? 'calc(' + length + ')' : length) + ')';
    }
  }
  defineInterface('CSSPerspective', CSSPerspective);
  Object.defineProperty(CSSPerspective.prototype, 'is2D', { enumerable: true });
  accessor(CSSPerspective, 'length', perspectiveLength);

  class CSSMatrixComponent extends CSSTransformComponent {
    constructor(matrix, options = {}) {
      super();
      if (arguments.length < 1) throw new TypeError("Failed to construct 'CSSMatrixComponent': 1 argument required");
      if (!(matrix instanceof DOMMatrixReadOnly)) throw new TypeError('The matrix must be a DOMMatrixReadOnly');
      const is2D = options !== null && options !== undefined && options.is2D !== undefined ? Boolean(options.is2D) : matrix.is2D;
      init(this, { matrix, is2D });
    }
    toMatrix() {
      const s = this[INTERNAL];
      return matrixOf(DOMMatrix.fromMatrix(s.matrix), s.is2D);
    }
    toString() {
      const s = this[INTERNAL];
      return matrixOf(DOMMatrix.fromMatrix(s.matrix), s.is2D).toString();
    }
  }
  defineInterface('CSSMatrixComponent', CSSMatrixComponent);
  accessor(CSSMatrixComponent, 'matrix', (v) => {
    if (!(v instanceof DOMMatrix)) throw new TypeError('The matrix must be a DOMMatrix');
    return v;
  });

  // ---- CSSTransformValue ----

  const checkComponent = (component) => {
    if (!(component instanceof CSSTransformComponent)) throw new TypeError('The item must be a CSSTransformComponent');
    return component;
  };
  class CSSTransformValue extends CSSStyleValue {
    constructor(transforms) {
      super();
      if (arguments.length < 1) throw new TypeError("Failed to construct 'CSSTransformValue': 1 argument required");
      const items = [...transforms].map(checkComponent);
      if (items.length === 0) throw new TypeError("Failed to construct 'CSSTransformValue': at least one component is required");
      Object.defineProperty(this, INTERNAL, { value: items, enumerable: false });
      return new Proxy(this, {
        get(target, key, receiver) {
          if (typeof key === 'string' && /^(?:0|[1-9][0-9]*)$/.test(key)) return items[Number(key)];
          return Reflect.get(target, key, target);
        },
        set(target, key, value) {
          if (typeof key === 'string' && /^(?:0|[1-9][0-9]*)$/.test(key)) {
            const index = Number(key);
            if (index > items.length) throw new RangeError('The index is out of range');
            items[index] = checkComponent(value);
            return true;
          }
          return Reflect.set(target, key, value);
        },
        has(target, key) {
          if (typeof key === 'string' && /^(?:0|[1-9][0-9]*)$/.test(key)) return Number(key) < items.length;
          return Reflect.has(target, key);
        },
        ownKeys(target) { return [...items.keys()].map(String).concat(Reflect.ownKeys(target)); },
        getOwnPropertyDescriptor(target, key) {
          if (typeof key === 'string' && /^(?:0|[1-9][0-9]*)$/.test(key)) {
            return Number(key) < items.length ? { value: items[Number(key)], writable: true, enumerable: true, configurable: true } : undefined;
          }
          return Reflect.getOwnPropertyDescriptor(target, key);
        },
      });
    }
    get length() { return this[INTERNAL].length; }
    get is2D() { return this[INTERNAL].every((c) => c.is2D); }
    toMatrix() {
      let matrix = new DOMMatrix();
      for (const component of this[INTERNAL]) matrix = matrix.multiplySelf(component.toMatrix());
      return matrix;
    }
    forEach(callback, thisArg) { return this[INTERNAL].forEach((value, index) => callback.call(thisArg, value, index, this)); }
    entries() { return this[INTERNAL].entries(); }
    keys() { return this[INTERNAL].keys(); }
    values() { return this[INTERNAL].values(); }
    [Symbol.iterator]() { return this[INTERNAL].values(); }
    toString() { return this[INTERNAL].map((c) => c.toString()).join(' '); }
  }
  defineInterface('CSSTransformValue', CSSTransformValue);
  Object.defineProperty(CSSTransformValue.prototype, 'is2D', { enumerable: true });

  T.CSSTransformValue = CSSTransformValue;
  T.CSSTransformComponent = CSSTransformComponent;
  T.CSSTranslate = CSSTranslate;
  T.CSSRotate = CSSRotate;
  T.CSSScale = CSSScale;
  T.CSSSkew = CSSSkew;
  T.CSSSkewX = CSSSkewX;
  T.CSSSkewY = CSSSkewY;
  T.CSSPerspective = CSSPerspective;
  T.CSSMatrixComponent = CSSMatrixComponent;
  void numberish;
})();
