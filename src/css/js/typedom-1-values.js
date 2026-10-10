// CSS Typed OM (https://drafts.css-houdini.org/css-typed-om-1/): the style values.
(function () {
  'use strict';
  const global = globalThis;

  // ---- Class plumbing ----

  const INTERNAL = Symbol('typed-om internal');
  let building = 0;
  // Builds an instance of an interface that script cannot construct.
  const build = (make) => {
    ++building;
    try { return make(); } finally { --building; }
  };
  const defineInterface = (name, constructor, parent) => {
    Object.defineProperty(constructor.prototype, Symbol.toStringTag, { value: name, configurable: true });
    Object.defineProperty(global, name, { value: constructor, writable: true, configurable: true });
    return constructor;
  };
  const illegal = () => { throw new TypeError('Illegal constructor'); };
  const define = (target, members) => {
    for (const key of Reflect.ownKeys(members)) {
      const descriptor = Object.getOwnPropertyDescriptor(members, key);
      descriptor.enumerable = true;
      Object.defineProperty(target, key, descriptor);
    }
  };
  const defineStatic = (target, members) => {
    for (const key of Reflect.ownKeys(members)) {
      const descriptor = Object.getOwnPropertyDescriptor(members, key);
      descriptor.enumerable = true;
      Object.defineProperty(target, key, descriptor);
    }
  };

  // ---- Units ----

  const LENGTHS = ['em', 'ex', 'cap', 'ch', 'ic', 'lh', 'rem', 'rex', 'rcap', 'rch', 'ric', 'rlh', 'vw', 'vh', 'vi', 'vb', 'vmin', 'vmax',
    'svw', 'svh', 'svi', 'svb', 'svmin', 'svmax', 'lvw', 'lvh', 'lvi', 'lvb', 'lvmin', 'lvmax', 'dvw', 'dvh', 'dvi', 'dvb', 'dvmin', 'dvmax',
    'cqw', 'cqh', 'cqi', 'cqb', 'cqmin', 'cqmax', 'cm', 'mm', 'q', 'in', 'pt', 'pc', 'px'];
  const ANGLES = ['deg', 'grad', 'rad', 'turn'];
  const TIMES = ['s', 'ms'];
  const FREQUENCIES = ['hz', 'khz'];
  const RESOLUTIONS = ['dpi', 'dpcm', 'dppx', 'x'];
  const CATEGORY = new Map();
  for (const unit of LENGTHS) CATEGORY.set(unit, 'length');
  for (const unit of ANGLES) CATEGORY.set(unit, 'angle');
  for (const unit of TIMES) CATEGORY.set(unit, 'time');
  for (const unit of FREQUENCIES) CATEGORY.set(unit, 'frequency');
  for (const unit of RESOLUTIONS) CATEGORY.set(unit, 'resolution');
  CATEGORY.set('fr', 'flex');
  CATEGORY.set('percent', 'percent');
  CATEGORY.set('number', 'number');
  // The factor to the canonical unit of the unit's kind, for the units that have one (the absolute ones).
  const FACTOR = new Map([
    ['px', 1], ['cm', 96 / 2.54], ['mm', 96 / 25.4], ['q', 96 / 101.6], ['in', 96], ['pt', 96 / 72], ['pc', 16],
    ['deg', 1], ['grad', 0.9], ['rad', 180 / Math.PI], ['turn', 360],
    ['s', 1], ['ms', 0.001], ['hz', 1], ['khz', 1000],
    ['dppx', 1], ['x', 1], ['dpi', 1 / 96], ['dpcm', 2.54 / 96],
  ]);
  const CANONICAL = { length: 'px', angle: 'deg', time: 's', frequency: 'hz', resolution: 'dppx' };
  const canonicalUnit = (unit) => {
    const category = CATEGORY.get(unit);
    return FACTOR.has(unit) ? CANONICAL[category] : unit;
  };
  const BASE_TYPES = ['length', 'angle', 'time', 'frequency', 'resolution', 'flex', 'percent'];

  // The unit of the name, lowercased; nothing if it is not one.
  const unitOf = (name) => {
    const lower = String(name).toLowerCase();
    return CATEGORY.has(lower) ? lower : null;
  };

  // ---- Numeric types ----

  const newType = () => ({ exponents: new Map(), hint: null });
  const copyType = (type) => ({ exponents: new Map(type.exponents), hint: type.hint });
  const typeOfUnit = (unit) => {
    const type = newType();
    const category = CATEGORY.get(unit);
    if (category !== 'number') type.exponents.set(category, 1);
    return type;
  };
  const entry = (type, name) => type.exponents.get(name) || 0;
  const applyHint = (type, hint) => {
    const percent = entry(type, 'percent');
    type.exponents.delete('percent');
    if (percent !== 0) type.exponents.set(hint, entry(type, hint) + percent);
    type.hint = hint;
  };
  const sameExponents = (a, b) => {
    for (const name of BASE_TYPES) {
      if (entry(a, name) !== entry(b, name)) return false;
    }
    return true;
  };
  const addTypes = (first, second) => {
    const a = copyType(first), b = copyType(second);
    if (a.hint !== null && b.hint !== null && a.hint !== b.hint) return null;
    if (a.hint !== null && b.hint === null) applyHint(b, a.hint);
    else if (b.hint !== null && a.hint === null) applyHint(a, b.hint);
    if (sameExponents(a, b)) return a;
    // A percent alone takes the hint of the one thing the other has.
    const only = (type, name) => [...type.exponents].filter(([, n]) => n !== 0).length === 1 && entry(type, name) !== 0;
    const nonZero = (type) => [...type.exponents].filter(([, n]) => n !== 0).map(([k]) => k);
    if (only(a, 'percent') && entry(b, 'percent') === 0 && nonZero(b).length === 1) {
      applyHint(a, nonZero(b)[0]);
    } else if (only(b, 'percent') && entry(a, 'percent') === 0 && nonZero(a).length === 1) {
      applyHint(b, nonZero(a)[0]);
    } else {
      return null;
    }
    if (!sameExponents(a, b)) return null;
    a.hint = a.hint !== null ? a.hint : b.hint;
    return a;
  };
  const multiplyTypes = (first, second) => {
    const a = copyType(first), b = copyType(second);
    if (a.hint !== null && b.hint !== null && a.hint !== b.hint) return null;
    if (a.hint !== null && b.hint === null) applyHint(b, a.hint);
    else if (b.hint !== null && a.hint === null) applyHint(a, b.hint);
    const result = newType();
    result.hint = a.hint;
    for (const name of BASE_TYPES) {
      const n = entry(a, name) + entry(b, name);
      if (n !== 0) result.exponents.set(name, n);
    }
    return result;
  };
  const invertType = (type) => {
    const result = newType();
    result.hint = type.hint;
    for (const [name, n] of type.exponents) if (n !== 0) result.exponents.set(name, -n);
    return result;
  };
  const typeToDictionary = (type) => {
    const dictionary = {};
    for (const name of BASE_TYPES) {
      const n = entry(type, name);
      if (n !== 0) dictionary[name] = n;
    }
    if (type.hint !== null) dictionary.percentHint = type.hint;
    return dictionary;
  };

  // ---- CSSStyleValue ----

  class CSSStyleValue {
    constructor() {
      if (new.target === CSSStyleValue && !building) illegal();
    }
    toString() {
      return this[INTERNAL] === undefined ? '' : String(this[INTERNAL]);
    }
    static parse(property, cssText) {
      property = String(property);
      cssText = String(cssText);
      const values = parseValues(property, cssText);
      return values[0];
    }
    static parseAll(property, cssText) {
      property = String(property);
      cssText = String(cssText);
      return parseValues(property, cssText);
    }
  }
  defineInterface('CSSStyleValue', CSSStyleValue);
  for (const name of ['parse', 'parseAll']) Object.defineProperty(CSSStyleValue, name, { enumerable: true });

  // A style value that is just text: what has no better representation.
  const makeGeneric = (text) => build(() => {
    const value = new CSSStyleValue();
    Object.defineProperty(value, INTERNAL, { value: text, enumerable: false });
    return value;
  });

  // ---- CSSKeywordValue ----

  class CSSKeywordValue extends CSSStyleValue {
    #value;
    constructor(value) {
      super();
      value = String(value);
      if (value === '') throw new TypeError("Failed to construct 'CSSKeywordValue': Expected a non-empty string");
      this.#value = value;
    }
    get value() { return this.#value; }
    set value(value) {
      value = String(value);
      if (value === '') throw new TypeError("Failed to set the 'value' property on 'CSSKeywordValue': Expected a non-empty string");
      this.#value = value;
    }
    toString() { return CSS.escape(this.#value); }
  }
  defineInterface('CSSKeywordValue', CSSKeywordValue);
  Object.defineProperty(CSSKeywordValue, 'length', { value: 1 });

  // ---- CSSNumericArray ----

  class CSSNumericArray {
    constructor() { if (!building) illegal(); }
    get length() { return this[INTERNAL].length; }
    forEach(callback, thisArg) { return this[INTERNAL].forEach((value, index) => callback.call(thisArg, value, index, this)); }
    entries() { return this[INTERNAL].entries(); }
    keys() { return this[INTERNAL].keys(); }
    values() { return this[INTERNAL].values(); }
    [Symbol.iterator]() { return this[INTERNAL].values(); }
  }
  defineInterface('CSSNumericArray', CSSNumericArray);
  const makeArray = (items) => build(() => {
    const array = new CSSNumericArray();
    Object.defineProperty(array, INTERNAL, { value: Object.freeze([...items]), enumerable: false });
    items.forEach((item, index) => Object.defineProperty(array, index, { value: item, enumerable: true, writable: false, configurable: false }));
    return array;
  });

  // ---- CSSNumericValue ----

  const isNumeric = (value) => value instanceof CSSNumericValue;
  // A number or a numeric value, as a numeric value.
  const rectify = (value) => {
    if (typeof value === 'number') return new CSSUnitValue(value, 'number');
    if (isNumeric(value)) return value;
    throw new TypeError('Failed to convert value to CSSNumericValue');
  };

  // A canonical sum of the value: terms with their units; nothing when it is not one (see "create a sum value").
  const sumTerm = (value, units) => ({ value, units });
  const unitsKey = (units) => [...units].filter(([, n]) => n !== 0).sort((a, b) => (a[0] < b[0] ? -1 : 1)).map(([u, n]) => u + ':' + n).join(',');
  const mergeTerms = (terms) => {
    const merged = new Map();
    for (const term of terms) {
      const key = unitsKey(term.units);
      const existing = merged.get(key);
      if (existing) existing.value += term.value;
      else merged.set(key, sumTerm(term.value, new Map([...term.units].filter(([, n]) => n !== 0))));
    }
    return [...merged.values()];
  };
  const sumValue = (value) => {
    if (value instanceof CSSUnitValue) {
      const unit = value.unit;
      if (unit === 'number') return [sumTerm(value.value, new Map())];
      const canonical = canonicalUnit(unit);
      const factor = FACTOR.has(unit) ? FACTOR.get(unit) : 1;
      return [sumTerm(value.value * factor, new Map([[canonical, 1]]))];
    }
    if (value instanceof CSSMathSum) {
      const terms = [];
      for (const item of value.values) {
        const sum = sumValue(item);
        if (sum === null) return null;
        terms.push(...sum);
      }
      return mergeTerms(terms);
    }
    if (value instanceof CSSMathNegate) {
      const sum = sumValue(value.value);
      return sum === null ? null : sum.map((t) => sumTerm(-t.value, t.units));
    }
    if (value instanceof CSSMathProduct) {
      let terms = [sumTerm(1, new Map())];
      for (const item of value.values) {
        const sum = sumValue(item);
        if (sum === null) return null;
        const next = [];
        for (const a of terms) {
          for (const b of sum) {
            const units = new Map(a.units);
            for (const [u, n] of b.units) units.set(u, (units.get(u) || 0) + n);
            next.push(sumTerm(a.value * b.value, units));
          }
        }
        terms = mergeTerms(next);
      }
      return terms;
    }
    if (value instanceof CSSMathInvert) {
      const sum = sumValue(value.value);
      if (sum === null || sum.length !== 1) return null;
      const units = new Map();
      for (const [u, n] of sum[0].units) units.set(u, -n);
      return [sumTerm(1 / sum[0].value, units)];
    }
    if (value instanceof CSSMathMin || value instanceof CSSMathMax || value instanceof CSSMathClamp) {
      const items = value instanceof CSSMathClamp ? [value.lower, value.value, value.upper] : [...value.values];
      const sums = items.map(sumValue);
      if (sums.some((s) => s === null || s.length !== 1)) return null;
      const key = unitsKey(sums[0][0].units);
      if (sums.some((s) => unitsKey(s[0].units) !== key)) return null;
      const numbers = sums.map((s) => s[0].value);
      let result;
      if (value instanceof CSSMathMin) result = Math.min(...numbers);
      else if (value instanceof CSSMathMax) result = Math.max(...numbers);
      else result = Math.max(numbers[0], Math.min(numbers[2], numbers[1]));
      return [sumTerm(result, sums[0][0].units)];
    }
    return null;
  };

  const negate = (value) => {
    if (value instanceof CSSUnitValue) return new CSSUnitValue(-value.value, value.unit);
    if (value instanceof CSSMathNegate) return value.value;
    return new CSSMathNegate(value);
  };
  const invert = (value) => {
    if (value instanceof CSSUnitValue && value.unit === 'number') return new CSSUnitValue(1 / value.value, 'number');
    if (value instanceof CSSMathInvert) return value.value;
    return new CSSMathInvert(value);
  };

  const sameUnitSimplify = (values, combine) => {
    if (values.every((v) => v instanceof CSSUnitValue && v.unit === values[0].unit)) {
      return new CSSUnitValue(values.map((v) => v.value).reduce(combine), values[0].unit);
    }
    return null;
  };

  class CSSNumericValue extends CSSStyleValue {
    constructor() {
      super();
      if (new.target === CSSNumericValue) illegal();
    }
    add(...values) {
      if (values.length === 0) return this;
      const items = [this, ...values.map(rectify)];
      return makeSum(items);
    }
    sub(...values) {
      if (values.length === 0) return this;
      const items = [this, ...values.map(rectify).map(negate)];
      return makeSum(items);
    }
    mul(...values) {
      if (values.length === 0) return this;
      return makeProduct([this, ...values.map(rectify)]);
    }
    div(...values) {
      if (values.length === 0) return this;
      const items = values.map(rectify);
      for (const item of items) {
        if (item instanceof CSSUnitValue && item.unit === 'number' && item.value === 0) throw new RangeError('Division by zero');
      }
      return makeProduct([this, ...items.map(invert)]);
    }
    min(...values) {
      if (values.length === 0) return this;
      return makeMinMax([this, ...values.map(rectify)], true);
    }
    max(...values) {
      if (values.length === 0) return this;
      return makeMinMax([this, ...values.map(rectify)], false);
    }
    equals(...values) {
      const items = values.map(rectify);
      return items.every((other) => equalValues(this, other));
    }
    to(unit) {
      const target = unitOf(unit);
      if (target === null) throw new DOMException("Failed to execute 'to' on 'CSSNumericValue': Invalid unit", 'SyntaxError');
      const sum = sumValue(this);
      if (sum === null || sum.length !== 1) throw new TypeError("Failed to execute 'to' on 'CSSNumericValue': Cannot convert");
      return convertTerm(sum[0], target);
    }
    toSum(...units) {
      const targets = units.map((unit) => {
        const target = unitOf(unit);
        if (target === null) throw new DOMException("Failed to execute 'toSum' on 'CSSNumericValue': Invalid unit", 'SyntaxError');
        return target;
      });
      const sum = sumValue(this);
      if (sum === null) throw new TypeError("Failed to execute 'toSum' on 'CSSNumericValue': Cannot convert");
      if (targets.length === 0) {
        const items = [];
        for (const term of sum) {
          const own = [...term.units].filter(([, n]) => n !== 0);
          if (own.length === 0) items.push(new CSSUnitValue(term.value, 'number'));
          else if (own.length === 1 && own[0][1] === 1) items.push(new CSSUnitValue(term.value, own[0][0]));
          else throw new TypeError("Failed to execute 'toSum' on 'CSSNumericValue': Cannot convert");
        }
        items.sort((a, b) => (a.unit < b.unit ? -1 : a.unit > b.unit ? 1 : 0));
        return makeMathSum(items);
      }
      // The units to sum into must add to each other.
      for (const target of targets) {
        const type = typeOfUnit(target);
        if (addTypes(typeOfUnit(targets[0]), type) === null) throw new TypeError("Failed to execute 'toSum' on 'CSSNumericValue': Units are not addable");
      }
      const result = targets.map((target) => ({ target, value: 0 }));
      for (const term of sum) {
        const own = [...term.units].filter(([, n]) => n !== 0);
        const wanted = term.units.size === 0 ? 'number' : own.length === 1 && own[0][1] === 1 ? own[0][0] : null;
        const slot = wanted === null ? undefined : result.find((r) => canonicalUnit(r.target) === wanted);
        if (!slot) throw new TypeError("Failed to execute 'toSum' on 'CSSNumericValue': Cannot convert");
        slot.value += convertTerm(term, slot.target).value;
      }
      return makeMathSum(result.map((r) => new CSSUnitValue(r.value, r.target)));
    }
    type() {
      return typeToDictionary(this[INTERNAL].type);
    }
    static parse(cssText) {
      const value = parseNumeric(String(cssText));
      if (!value) throw new DOMException("Failed to execute 'parse' on 'CSSNumericValue': Invalid numeric value", 'SyntaxError');
      return value;
    }
  }
  defineInterface('CSSNumericValue', CSSNumericValue);
  Object.defineProperty(CSSNumericValue, 'parse', { enumerable: true });

  const convertTerm = (term, target) => {
    const units = [...term.units].filter(([, n]) => n !== 0);
    if (target === 'number') {
      if (units.length !== 0) throw new TypeError('Cannot convert to number');
      return new CSSUnitValue(term.value, 'number');
    }
    const canonical = canonicalUnit(target);
    if (units.length !== 1 || units[0][0] !== canonical || units[0][1] !== 1) throw new TypeError('Cannot convert to ' + target);
    const factor = FACTOR.has(target) ? FACTOR.get(target) : 1;
    return new CSSUnitValue(term.value / factor, target);
  };

  const equalValues = (a, b) => {
    if (a === b) return true;
    if (Object.getPrototypeOf(a) !== Object.getPrototypeOf(b)) return false;
    if (a instanceof CSSUnitValue) return a.value === b.value && a.unit === b.unit;
    if (a instanceof CSSMathNegate || a instanceof CSSMathInvert) return equalValues(a.value, b.value);
    if (a instanceof CSSMathClamp) return equalValues(a.lower, b.lower) && equalValues(a.value, b.value) && equalValues(a.upper, b.upper);
    if (a.values.length !== b.values.length) return false;
    for (let i = 0; i < a.values.length; ++i) if (!equalValues(a.values[i], b.values[i])) return false;
    return true;
  };

  // ---- CSSUnitValue ----

  class CSSUnitValue extends CSSNumericValue {
    #value;
    #unit;
    constructor(value, unit) {
      super();
      const known = unitOf(unit);
      if (known === null) throw new TypeError("Failed to construct 'CSSUnitValue': Invalid unit: " + unit);
      this.#value = Number(value);
      this.#unit = known;
      Object.defineProperty(this, INTERNAL, { value: { type: typeOfUnit(known) }, enumerable: false });
    }
    get value() { return this.#value; }
    set value(value) { this.#value = Number(value); }
    get unit() { return this.#unit; }
    toString() {
      const number = this.#value;
      const text = Number.isFinite(number) ? numberText(number) : null;
      if (text === null) return 'calc(' + (Number.isNaN(number) ? 'NaN' : number > 0 ? 'infinity' : '-infinity') + (this.#unit === 'number' ? '' : ' * 1' + (this.#unit === 'percent' ? '%' : this.#unit)) + ')';
      if (this.#unit === 'number') return text;
      if (this.#unit === 'percent') return text + '%';
      return text + this.#unit;
    }
  }
  defineInterface('CSSUnitValue', CSSUnitValue);
  Object.defineProperty(CSSUnitValue, 'length', { value: 2 });
  define(CSSUnitValue.prototype, {});

  const numberText = (n) => {
    if (Object.is(n, -0)) return '-0';
    let text = String(n);
    if (/e/i.test(text)) {
      // No exponent: expand.
      text = n.toFixed(20).replace(/0+$/, '').replace(/\.$/, '');
      if (Math.abs(n) >= 1e21) text = BigInt(n).toString();
    }
    return text;
  };

  // ---- CSSMathValue and its subclasses ----

  class CSSMathValue extends CSSNumericValue {
    constructor() {
      super();
      if (new.target === CSSMathValue) illegal();
    }
    get operator() { return this[INTERNAL].operator; }
  }
  defineInterface('CSSMathValue', CSSMathValue);

  const initMath = (object, operator, type, values) => {
    Object.defineProperty(object, INTERNAL, { value: { operator, type, values }, enumerable: false });
  };
  const typeOf = (value) => value[INTERNAL].type;
  const sumType = (items) => {
    let type = typeOf(items[0]);
    for (let i = 1; i < items.length; ++i) {
      type = addTypes(type, typeOf(items[i]));
      if (type === null) throw new TypeError('The values cannot be added: their types are not compatible');
    }
    return type;
  };
  const arrayOfOperands = (items) => items.map(rectify);

  class CSSMathSum extends CSSMathValue {
    constructor(...values) {
      super();
      const items = arrayOfOperands(values);
      if (items.length === 0) throw new DOMException("Failed to construct 'CSSMathSum': at least one value is required", 'SyntaxError');
      initMath(this, 'sum', sumType(items), makeArray(items));
    }
    get values() { return this[INTERNAL].values; }
    toString() { return serializeMath(this, 0); }
  }
  defineInterface('CSSMathSum', CSSMathSum);
  Object.defineProperty(CSSMathSum, 'length', { value: 0 });

  class CSSMathProduct extends CSSMathValue {
    constructor(...values) {
      super();
      const items = arrayOfOperands(values);
      if (items.length === 0) throw new DOMException("Failed to construct 'CSSMathProduct': at least one value is required", 'SyntaxError');
      let type = typeOf(items[0]);
      for (let i = 1; i < items.length; ++i) {
        type = multiplyTypes(type, typeOf(items[i]));
        if (type === null) throw new TypeError('The values cannot be multiplied: their percent hints differ');
      }
      initMath(this, 'product', type, makeArray(items));
    }
    get values() { return this[INTERNAL].values; }
    toString() { return serializeMath(this, 0); }
  }
  defineInterface('CSSMathProduct', CSSMathProduct);

  class CSSMathMin extends CSSMathValue {
    constructor(...values) {
      super();
      const items = arrayOfOperands(values);
      if (items.length === 0) throw new DOMException("Failed to construct 'CSSMathMin': at least one value is required", 'SyntaxError');
      initMath(this, 'min', sumType(items), makeArray(items));
    }
    get values() { return this[INTERNAL].values; }
    toString() { return serializeMath(this, 0); }
  }
  defineInterface('CSSMathMin', CSSMathMin);

  class CSSMathMax extends CSSMathValue {
    constructor(...values) {
      super();
      const items = arrayOfOperands(values);
      if (items.length === 0) throw new DOMException("Failed to construct 'CSSMathMax': at least one value is required", 'SyntaxError');
      initMath(this, 'max', sumType(items), makeArray(items));
    }
    get values() { return this[INTERNAL].values; }
    toString() { return serializeMath(this, 0); }
  }
  defineInterface('CSSMathMax', CSSMathMax);

  class CSSMathNegate extends CSSMathValue {
    constructor(value) {
      super();
      const item = rectify(value);
      initMath(this, 'negate', typeOf(item), item);
    }
    get value() { return this[INTERNAL].values; }
    toString() { return serializeMath(this, 0); }
  }
  defineInterface('CSSMathNegate', CSSMathNegate);

  class CSSMathInvert extends CSSMathValue {
    constructor(value) {
      super();
      const item = rectify(value);
      initMath(this, 'invert', invertType(typeOf(item)), item);
    }
    get value() { return this[INTERNAL].values; }
    toString() { return serializeMath(this, 0); }
  }
  defineInterface('CSSMathInvert', CSSMathInvert);

  class CSSMathClamp extends CSSMathValue {
    constructor(lower, value, upper) {
      super();
      if (arguments.length < 3) throw new TypeError("Failed to construct 'CSSMathClamp': 3 arguments required");
      const items = [rectify(lower), rectify(value), rectify(upper)];
      initMath(this, 'clamp', sumType(items), items);
    }
    get lower() { return this[INTERNAL].values[0]; }
    get value() { return this[INTERNAL].values[1]; }
    get upper() { return this[INTERNAL].values[2]; }
    toString() { return serializeMath(this, 0); }
  }
  defineInterface('CSSMathClamp', CSSMathClamp);

  // The results of the arithmetic methods: simplified where all there is to add is the same unit.
  const makeMathSum = (items) => new CSSMathSum(...items);
  const makeSum = (given) => {
    const items = [];
    for (const item of given) {
      if (item instanceof CSSMathSum) items.push(...item.values);
      else items.push(item);
    }
    const simplified = sameUnitSimplify(items, (a, b) => a + b);
    if (simplified) {
      // (the types must still be addable)
      sumType(items);
      return simplified;
    }
    return new CSSMathSum(...items);
  };
  const makeProduct = (given) => {
    const items = [];
    for (const item of given) {
      if (item instanceof CSSMathProduct) items.push(...item.values);
      else items.push(item);
    }
    const product = new CSSMathProduct(...items);
    if (items.every((v) => v instanceof CSSUnitValue)) {
      const nonNumbers = items.filter((v) => v.unit !== 'number');
      if (nonNumbers.length <= 1) {
        return new CSSUnitValue(items.map((v) => v.value).reduce((a, b) => a * b, 1), nonNumbers.length ? nonNumbers[0].unit : 'number');
      }
    }
    return product;
  };
  const makeMinMax = (given, isMin) => {
    const items = [];
    const kind = isMin ? CSSMathMin : CSSMathMax;
    for (const item of given) {
      if (item instanceof kind) items.push(...item.values);
      else items.push(item);
    }
    sumType(items);
    const simplified = sameUnitSimplify(items, isMin ? (a, b) => Math.min(a, b) : (a, b) => Math.max(a, b));
    return simplified || new kind(...items);
  };

  // ---- Serialization of calculations ----

  // mode: 0 at the top (calc( ) around it), 1 nested in another calculation (parentheses), 2 as the argument of min(), max() or clamp().
  const serializeMath = (value, mode) => {
    const wrap = (body) => (mode === 0 ? 'calc(' + body + ')' : mode === 1 ? '(' + body + ')' : body);
    const part = (item) => (item instanceof CSSUnitValue ? item.toString() : serializeMath(item, 1));
    if (value instanceof CSSMathSum) {
      const body = [...value.values].map((item, index) => {
        if (index === 0) return part(item);
        if (item instanceof CSSMathNegate) return '- ' + part(item.value);
        return '+ ' + part(item);
      }).join(' ');
      return wrap(body);
    }
    if (value instanceof CSSMathProduct) {
      const body = [...value.values].map((item, index) => {
        if (index === 0) return part(item);
        if (item instanceof CSSMathInvert) return '/ ' + part(item.value);
        return '* ' + part(item);
      }).join(' ');
      return wrap(body);
    }
    if (value instanceof CSSMathNegate) return wrap('-' + part(value.value));
    if (value instanceof CSSMathInvert) return wrap('1 / ' + part(value.value));
    const name = value instanceof CSSMathMin ? 'min' : value instanceof CSSMathMax ? 'max' : 'clamp';
    const items = value instanceof CSSMathClamp ? [value.lower, value.value, value.upper] : [...value.values];
    return name + '(' + items.map((item) => (item instanceof CSSUnitValue ? item.toString() : serializeMath(item, 2))).join(', ') + ')';
  };

  // ---- Factory functions ----

  const factories = { number: 'number', percent: 'percent', fr: 'fr' };
  for (const unit of [...LENGTHS, ...ANGLES, ...TIMES, ...FREQUENCIES, ...RESOLUTIONS]) factories[unit] = unit;
  const canonicalNames = { q: 'Q', hz: 'Hz', khz: 'kHz' };
  const installFactories = () => {
    const css = global.CSS;
    for (const [name, unit] of Object.entries(factories)) {
      const exposed = canonicalNames[name] || name;
      Object.defineProperty(css, exposed, {
        value: { [exposed](value) { return new CSSUnitValue(value, unit); } }[exposed],
        writable: true,
        enumerable: true,
        configurable: true,
      });
    }
  };

  // ---- Shared with the other parts of the Typed OM ----

  global.__solarTypedOm = {
    INTERNAL, build, defineInterface, illegal, define, defineStatic, makeGeneric, rectify, installFactories, numberText,
    CSSStyleValue, CSSKeywordValue, CSSNumericValue, CSSUnitValue, CSSMathValue, CSSMathSum, CSSMathProduct, CSSMathMin, CSSMathMax,
    CSSMathNegate, CSSMathInvert, CSSMathClamp, CSSNumericArray, makeArray, typeOf,
  };

  // Parsing hooks, set by the next part.
  var parseValues = (property, text) => { throw new TypeError('not ready'); };
  var parseNumeric = (text) => null;
  global.__solarTypedOm.setParsers = (values, numeric) => {
    parseValues = values;
    parseNumeric = numeric;
  };
})();
