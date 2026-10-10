// CSS Typed OM: reifying values from their text, the unparsed values, and the style property maps.
(function () {
  'use strict';
  const global = globalThis;
  const T = global.__solarTypedOm;
  const { INTERNAL, build, defineInterface, makeGeneric, rectify, makeArray } = T;
  const { CSSStyleValue, CSSKeywordValue, CSSNumericValue, CSSUnitValue, CSSMathSum, CSSMathProduct, CSSMathMin, CSSMathMax, CSSMathNegate, CSSMathInvert, CSSMathClamp } = T;
  const propertyInfo = global.__solarTypedPropertyInfo;
  delete global.__solarTypedPropertyInfo;

  // ---- Tokens ----

  // A value as a tree: ident, number (value, unit), string, url, function (name, children), comma, delim, ws.
  const tokenize = (text) => {
    let i = 0;
    const stack = [[]];
    const out = () => stack[stack.length - 1];
    const isNameStart = (c) => /[A-Za-z_\u0080-￿]/.test(c);
    const isName = (c) => /[A-Za-z0-9_\-\u0080-￿]/.test(c);
    const readIdent = () => {
      let name = '';
      while (i < text.length) {
        if (text[i] === '\\' && i + 1 < text.length) {
          name += text[i + 1];
          i += 2;
        } else if (isName(text[i])) {
          name += text[i++];
        } else {
          break;
        }
      }
      return name;
    };
    while (i < text.length) {
      const c = text[i];
      if (/\s/.test(c)) {
        let ws = '';
        while (i < text.length && /\s/.test(text[i])) ws += text[i++];
        out().push({ type: 'ws', text: ws });
      } else if (c === '/' && text[i + 1] === '*') {
        const end = text.indexOf('*/', i + 2);
        i = end < 0 ? text.length : end + 2;
      } else if (c === '"' || c === "'") {
        let value = '';
        ++i;
        while (i < text.length && text[i] !== c) {
          if (text[i] === '\\' && i + 1 < text.length) {
            value += text[i + 1];
            i += 2;
          } else {
            value += text[i++];
          }
        }
        ++i;
        out().push({ type: 'string', value });
      } else if (/[0-9]/.test(c) || ((c === '.') && /[0-9]/.test(text[i + 1] || '')) || ((c === '+' || c === '-') && (/[0-9]/.test(text[i + 1] || '') || (text[i + 1] === '.' && /[0-9]/.test(text[i + 2] || ''))))) {
        const match = /^[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?/.exec(text.slice(i));
        let numberText = match[0];
        i += numberText.length;
        let unit = '';
        if (text[i] === '%') {
          unit = '%';
          ++i;
        } else if (i < text.length && (isNameStart(text[i]) || text[i] === '-' || text[i] === '\\')) {
          unit = readIdent();
        }
        out().push({ type: 'number', value: Number(numberText), unit, text: numberText + unit });
      } else if (isNameStart(c) || ((c === '-') && (isNameStart(text[i + 1] || '') || text[i + 1] === '-' || text[i + 1] === '\\')) || c === '\\') {
        const name = readIdent();
        if (text[i] === '(') {
          ++i;
          if (name.toLowerCase() === 'url') {
            // url( with an unquoted address is one token.
            let j = i;
            while (j < text.length && /\s/.test(text[j])) ++j;
            if (text[j] !== '"' && text[j] !== "'") {
              const end = text.indexOf(')', j);
              out().push({ type: 'url', value: text.slice(j, end < 0 ? text.length : end).trim() });
              i = end < 0 ? text.length : end + 1;
              continue;
            }
          }
          const node = { type: 'function', name, children: [] };
          out().push(node);
          stack.push(node.children);
        } else {
          out().push({ type: 'ident', value: name });
        }
      } else if (c === '(') {
        ++i;
        const node = { type: 'paren', children: [] };
        out().push(node);
        stack.push(node.children);
      } else if (c === ')') {
        ++i;
        if (stack.length > 1) stack.pop();
      } else if (c === ',') {
        ++i;
        out().push({ type: 'comma' });
      } else if (c === '#') {
        ++i;
        out().push({ type: 'hash', value: readIdent() });
      } else {
        ++i;
        out().push({ type: 'delim', value: c });
      }
    }
    return stack[0];
  };
  const significant = (nodes) => nodes.filter((n) => n.type !== 'ws');
  const containsVar = (text) => /var\(/i.test(text);

  // ---- Numbers ----

  const unitOfToken = (token) => {
    if (token.unit === '') return 'number';
    if (token.unit === '%') return 'percent';
    const lower = token.unit.toLowerCase();
    try {
      return new CSSUnitValue(0, lower).unit;
    } catch (e) {
      return null;
    }
  };

  class ParseFailure extends Error {}

  // calc() operands as typed values; the sum and product are simplified where the units allow it.
  const calcOperand = (nodes) => {
    const items = significant(nodes);
    return calcSum(items);
  };
  const split = (items, operators) => {
    // Splits at the top-level operators, returning [operand items, ...] with the operator before each.
    const parts = [];
    let current = [];
    let operator = null;
    for (const item of items) {
      if (item.type === 'delim' && operators.includes(item.value)) {
        parts.push({ operator, items: current });
        current = [];
        operator = item.value;
      } else {
        current.push(item);
      }
    }
    parts.push({ operator, items: current });
    return parts;
  };
  const calcSum = (items) => {
    // + and - need white space around them, so a - in a number or a name is not one: the tokenizer has taken those.
    const parts = split(items, ['+', '-']);
    const terms = [];
    for (const part of parts) {
      if (part.items.length === 0) throw new ParseFailure();
      let value = calcProduct(part.items);
      if (part.operator === '-') value = value instanceof CSSUnitValue ? new CSSMathNegate(value) : new CSSMathNegate(value);
      terms.push(value);
    }
    if (terms.length === 1) return terms[0];
    // Terms of the same unit, or of units of one absolute kind, are added.
    const merged = [];
    for (const term of terms) {
      if (term instanceof CSSUnitValue) {
        const same = merged.find((m) => m instanceof CSSUnitValue && m.unit !== 'number' ? safeSame(m, term) : m instanceof CSSUnitValue && m.unit === term.unit);
        if (same) {
          const sum = same.add(term.to(same.unit));
          merged[merged.indexOf(same)] = sum;
          continue;
        }
      }
      merged.push(term);
    }
    return merged.length === 1 ? merged[0] : new CSSMathSum(...merged);
  };
  const safeSame = (a, b) => {
    try {
      const converted = b.to(a.unit);
      return converted instanceof CSSUnitValue;
    } catch (e) {
      return false;
    }
  };
  const calcProduct = (items) => {
    const parts = split(items, ['*', '/']);
    const factors = [];
    for (const part of parts) {
      if (part.items.length === 0) throw new ParseFailure();
      let value = calcValue(part.items);
      if (part.operator === '/') {
        if (value instanceof CSSUnitValue && value.unit === 'number') {
          if (value.value === 0) throw new ParseFailure();
          value = new CSSUnitValue(1 / value.value, 'number');
        } else {
          value = new CSSMathInvert(value);
        }
      }
      factors.push(value);
    }
    if (factors.length === 1) return factors[0];
    // Numbers fold into the one value that has a unit.
    const numbers = factors.filter((f) => f instanceof CSSUnitValue && f.unit === 'number');
    const others = factors.filter((f) => !(f instanceof CSSUnitValue && f.unit === 'number'));
    if (others.length === 0) return new CSSUnitValue(numbers.reduce((a, f) => a * f.value, 1), 'number');
    if (others.length === 1 && others[0] instanceof CSSUnitValue) return new CSSUnitValue(numbers.reduce((a, f) => a * f.value, others[0].value), others[0].unit);
    return new CSSMathProduct(...factors);
  };
  const calcValue = (items) => {
    if (items.length !== 1) throw new ParseFailure();
    const item = items[0];
    if (item.type === 'number') {
      const unit = unitOfToken(item);
      if (unit === null) throw new ParseFailure();
      return new CSSUnitValue(item.value, unit);
    }
    if (item.type === 'paren') return calcOperand(item.children);
    if (item.type === 'function') {
      const name = item.name.toLowerCase();
      if (name === 'calc') return calcOperand(item.children);
      const args = splitCommas(item.children).map((a) => calcSum(significant(a)));
      if (name === 'min') return new CSSMathMin(...args);
      if (name === 'max') return new CSSMathMax(...args);
      if (name === 'clamp' && args.length === 3) return new CSSMathClamp(...args);
    }
    throw new ParseFailure();
  };
  const splitCommas = (nodes) => {
    const lists = [[]];
    for (const node of nodes) {
      if (node.type === 'comma') lists.push([]);
      else lists[lists.length - 1].push(node);
    }
    return lists;
  };

  // A numeric value of the nodes, nothing if they are not one.
  const numericOf = (nodes) => {
    const items = significant(nodes);
    if (items.length !== 1) return null;
    const item = items[0];
    try {
      if (item.type === 'number') {
        const unit = unitOfToken(item);
        return unit === null ? null : new CSSUnitValue(item.value, unit);
      }
      if (item.type === 'function') {
        const name = item.name.toLowerCase();
        if (name === 'calc') {
          const value = calcOperand(item.children);
          return value instanceof CSSUnitValue ? new CSSMathSum(value) : value;
        }
        if (name === 'min' || name === 'max' || name === 'clamp') return calcValue([item]);
      }
    } catch (e) {
      if (e instanceof ParseFailure || e instanceof TypeError || e instanceof RangeError) return null;
      throw e;
    }
    return null;
  };

  const parseNumeric = (text) => numericOf(tokenize(text));

  // ---- CSSUnparsedValue and CSSVariableReferenceValue ----

  const validVariable = (name) => typeof name === 'string' && name.length > 2 && name.startsWith('--');
  class CSSVariableReferenceValue {
    #variable;
    #fallback;
    constructor(variable, fallback = null) {
      variable = String(variable);
      if (!validVariable(variable)) throw new TypeError("Failed to construct 'CSSVariableReferenceValue': Invalid variable name");
      if (fallback !== null && fallback !== undefined && !(fallback instanceof CSSUnparsedValue)) throw new TypeError("Failed to construct 'CSSVariableReferenceValue': fallback is not a CSSUnparsedValue");
      this.#variable = variable;
      this.#fallback = fallback === undefined ? null : fallback;
    }
    get variable() { return this.#variable; }
    set variable(value) {
      value = String(value);
      if (!validVariable(value)) throw new TypeError("Failed to set the 'variable' property: Invalid variable name");
      this.#variable = value;
    }
    get fallback() { return this.#fallback; }
    toString() { return 'var(' + this.#variable + (this.#fallback ? ',' + this.#fallback.toString() : '') + ')'; }
  }
  defineInterface('CSSVariableReferenceValue', CSSVariableReferenceValue);

  class CSSUnparsedValue extends CSSStyleValue {
    constructor(members) {
      super();
      const items = [];
      if (members === undefined || members === null) throw new TypeError("Failed to construct 'CSSUnparsedValue': 1 argument required");
      for (const member of members) {
        if (member instanceof CSSVariableReferenceValue) items.push(member);
        else items.push(String(member));
      }
      Object.defineProperty(this, INTERNAL, { value: items, enumerable: false });
      return new Proxy(this, {
        get(target, key, receiver) {
          if (typeof key === 'string' && /^(?:0|[1-9][0-9]*)$/.test(key)) return items[Number(key)];
          return Reflect.get(target, key, receiver === undefined ? target : target);
        },
        set(target, key, value) {
          if (typeof key === 'string' && /^(?:0|[1-9][0-9]*)$/.test(key)) {
            const index = Number(key);
            if (index > items.length) throw new RangeError('The index is out of range');
            const member = value instanceof CSSVariableReferenceValue ? value : String(value);
            items[index] = member;
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
    forEach(callback, thisArg) { return this[INTERNAL].forEach((value, index) => callback.call(thisArg, value, index, this)); }
    entries() { return this[INTERNAL].entries(); }
    keys() { return this[INTERNAL].keys(); }
    values() { return this[INTERNAL].values(); }
    [Symbol.iterator]() { return this[INTERNAL].values(); }
    toString() {
      let out = '';
      let previous = null;
      for (const member of this[INTERNAL]) {
        const text = String(member);
        // Two pieces that would run together into one token are kept apart.
        if (previous !== null && /[A-Za-z0-9_\-\u0080-\uffff]$/.test(previous) && /^[A-Za-z0-9_\-\u0080-\uffff]/.test(text)) out += '/**/';
        out += text;
        previous = text === '' ? previous : text;
      }
      return out;
    }
  }
  defineInterface('CSSUnparsedValue', CSSUnparsedValue);

  // The text of a value with var() in it, as pieces.
  const unparsedOf = (text) => {
    text = text.replace(/\/\*[\s\S]*?\*\//g, '\u0000');
    const pieces = [];
    let literal = '';
    let i = 0;
    while (i < text.length) {
      const match = /^var\(/i.exec(text.slice(i));
      if (match && (i === 0 || !/[A-Za-z0-9_\-]/.test(text[i - 1]))) {
        // find the matching close
        let depth = 1, j = i + 4;
        let quote = null;
        while (j < text.length && depth > 0) {
          const c = text[j];
          if (quote) {
            if (c === '\\') ++j;
            else if (c === quote) quote = null;
          } else if (c === '"' || c === "'") quote = c;
          else if (c === '(') ++depth;
          else if (c === ')') --depth;
          ++j;
        }
        const inner = text.slice(i + 4, j - 1);
        const comma = topLevelComma(inner);
        const name = (comma < 0 ? inner : inner.slice(0, comma)).trim();
        if (literal.replace(/\u0000/g, '')) pieces.push(literal.replace(/\u0000/g, ''));
        literal = '';
        const fallback = comma < 0 ? null : unparsedOf(inner.slice(comma + 1));
        pieces.push(new CSSVariableReferenceValue(name, fallback));
        i = j;
      } else {
        literal += text[i++];
      }
    }
    literal = literal.replace(/\u0000/g, '');
    if (literal || pieces.length === 0) pieces.push(literal);
    return new CSSUnparsedValue(pieces);
  };
  const topLevelComma = (text) => {
    let depth = 0, quote = null;
    for (let i = 0; i < text.length; ++i) {
      const c = text[i];
      if (c === '\\') ++i;
      else if (quote) {
        if (c === quote) quote = null;
      } else if (c === '"' || c === "'") quote = c;
      else if (c === '(') ++depth;
      else if (c === ')') --depth;
      else if (c === ',' && depth === 0) return i;
    }
    return -1;
  };

  // ---- CSSImageValue ----

  class CSSImageValue extends CSSStyleValue {
    constructor() {
      super();
    }
    toString() { return this[INTERNAL]; }
  }
  defineInterface('CSSImageValue', CSSImageValue);

  // ---- Reification ----

  const URL_IMAGE = /^url\(/i;
  const reifyOne = (text, keywords) => {
    const nodes = tokenize(text);
    const items = significant(nodes);
    if (items.length === 1) {
      const item = items[0];
      if (item.type === 'ident') {
        const lower = item.value.toLowerCase();
        if (!keywords || keywords.has(lower) || /^(?:initial|inherit|unset|revert|revert-layer)$/.test(lower)) return new CSSKeywordValue(item.value);
        return makeGeneric(text.trim());
      }
      const numeric = numericOf(nodes);
      if (numeric) return numeric;
      if (item.type === 'url') return build(() => {
        const image = new CSSImageValue();
        Object.defineProperty(image, INTERNAL, { value: text.trim(), enumerable: false });
        return image;
      });
    }
    if (items.length === 1 && items[0].type === 'function' && /^(?:url|.*gradient|image-set|cross-fade|image|element)$/i.test(items[0].name)) {
      return build(() => {
        const image = new CSSImageValue();
        Object.defineProperty(image, INTERNAL, { value: text.trim(), enumerable: false });
        return image;
      });
    }
    return makeGeneric(text.trim());
  };

  // The values of a declared text, for the property: one, or the items of a list.
  // A transform list as a CSSTransformValue; nothing if some function is not understood.
  const reifyTransform = (nodes) => {
    const components = [];
    for (const node of significant(nodes)) {
      if (node.type !== 'function') return null;
      const name = node.name.toLowerCase();
      const args = splitCommas(node.children).map((a) => numericOf(a));
      if (args.some((a) => a === null)) return null;
      const number = (v) => v;
      try {
        switch (name) {
          case 'translate': components.push(new T.CSSTranslate(args[0], args[1] || new CSSUnitValue(0, 'px'))); break;
          case 'translatex': components.push(new T.CSSTranslate(args[0], new CSSUnitValue(0, 'px'))); break;
          case 'translatey': components.push(new T.CSSTranslate(new CSSUnitValue(0, 'px'), args[0])); break;
          case 'translatez': components.push(new T.CSSTranslate(new CSSUnitValue(0, 'px'), new CSSUnitValue(0, 'px'), args[0])); break;
          case 'translate3d': components.push(new T.CSSTranslate(args[0], args[1], args[2])); break;
          case 'rotate': components.push(new T.CSSRotate(args[0])); break;
          case 'rotatex': components.push(new T.CSSRotate(1, 0, 0, args[0])); break;
          case 'rotatey': components.push(new T.CSSRotate(0, 1, 0, args[0])); break;
          case 'rotatez': components.push(new T.CSSRotate(0, 0, 1, args[0])); break;
          case 'rotate3d': components.push(new T.CSSRotate(args[0], args[1], args[2], args[3])); break;
          case 'scale': components.push(new T.CSSScale(args[0], args[1] || args[0])); break;
          case 'scalex': components.push(new T.CSSScale(args[0], 1)); break;
          case 'scaley': components.push(new T.CSSScale(1, args[0])); break;
          case 'scalez': components.push(new T.CSSScale(1, 1, args[0])); break;
          case 'scale3d': components.push(new T.CSSScale(args[0], args[1], args[2])); break;
          case 'skew': components.push(new T.CSSSkew(args[0], args[1] || new CSSUnitValue(0, 'deg'))); break;
          case 'skewx': components.push(new T.CSSSkewX(args[0])); break;
          case 'skewy': components.push(new T.CSSSkewY(args[0])); break;
          case 'perspective': components.push(new T.CSSPerspective(args[0])); break;
          case 'matrix': case 'matrix3d': components.push(new T.CSSMatrixComponent(new DOMMatrixReadOnly(args.map((a) => number(a.value))), { is2D: name === 'matrix' })); break;
          default: return null;
        }
      } catch (e) {
        if (e instanceof TypeError || e instanceof RangeError) return null;
        throw e;
      }
    }
    return components.length ? new T.CSSTransformValue(components) : null;
  };

  const ASSOC = Symbol('associated property');
  const associate = (value, property) => {
    if (Object.getPrototypeOf(value) === CSSStyleValue.prototype) Object.defineProperty(value, ASSOC, { value: property, enumerable: false });
    return value;
  };
  const reify = (property, text, isList, keywords) => {
    if (containsVar(text) || property.startsWith('--')) return [unparsedOf(text.trim())];
    if (property === 'transform') {
      const nodes = tokenize(text);
      if (!(significant(nodes).length === 1 && significant(nodes)[0].type === 'ident')) {
        const transform = reifyTransform(nodes);
        if (transform) return [transform];
      }
    }
    if (!isList) return [associate(reifyOne(text, keywords), property)];
    return splitTopLevel(text).map((part) => associate(reifyOne(part, keywords), property));
  };
  const splitTopLevel = (text) => {
    const parts = [];
    let start = 0, depth = 0, quote = null;
    for (let i = 0; i < text.length; ++i) {
      const c = text[i];
      if (quote) {
        if (c === '\\') ++i;
        else if (c === quote) quote = null;
      } else if (c === '"' || c === "'") quote = c;
      else if (c === '(' || c === '[') ++depth;
      else if (c === ')' || c === ']') --depth;
      else if (c === ',' && depth === 0) {
        parts.push(text.slice(start, i));
        start = i + 1;
      }
    }
    parts.push(text.slice(start));
    return parts;
  };

  // ---- A scratch block of declarations, to ask whether a value is one ----

  let scratch = null;
  const scratchDeclarations = () => {
    if (!scratch) scratch = new CSSStyleSheet();
    if (scratch.cssRules.length === 0) scratch.insertRule('x {}');
    return scratch.cssRules[0].style;
  };
  const infoOf = (property) => {
    const info = propertyInfo(property);
    if (info === null) return null;
    return { kind: info[0], syntax: info[1], name: info[2] || property, list: /#/.test(info[1]), keywords: info[3] ? new Set(info[3].map((k) => k.toLowerCase())) : null };
  };
  const canonicalName = (property) => {
    if (property.startsWith('--')) return property;
    return property.toLowerCase();
  };

  const parseValues = (property, text) => {
    const info = infoOf(property);
    if (info === null) throw new TypeError("Failed to execute 'parse' on 'CSSStyleValue': Invalid property " + property);
    const declarations = scratchDeclarations();
    if (info.kind === 'shorthand') {
      if (containsVar(text)) return [unparsedOf(text.trim())];
      declarations.removeProperty(info.name);
      declarations.setProperty(info.name, text);
      const stored = declarations.getPropertyValue(info.name);
      declarations.removeProperty(info.name);
      if (stored === '') throw new TypeError("Failed to execute 'parse' on 'CSSStyleValue': Invalid value for " + property);
      return [makeGeneric(text.trim())];
    }
    const name = canonicalName(property);
    if (info.kind === 'custom') {
      if (text.trim() === '') throw new TypeError("Failed to execute 'parse' on 'CSSStyleValue': Empty value");
      declarations.setProperty(name, text);
      declarations.removeProperty(name);
      return [unparsedOf(text.trim())];
    }
    declarations.removeProperty(info.name);
    declarations.setProperty(info.name, text);
    const stored = declarations.getPropertyValue(info.name);
    declarations.removeProperty(info.name);
    if (stored === '') throw new TypeError("Failed to execute 'parse' on 'CSSStyleValue': Invalid value for " + property);
    const values = reify(info.name, containsVar(text) ? text : stored, info.list, info.keywords);
    return values;
  };
  const parseSingle = (property, text) => {
    const values = parseValues(property, text);
    return values;
  };
  T.setParsers((property, text) => {
    const values = parseSingle(property, text);
    return values;
  }, parseNumeric);
  // parse() gives the first and parseAll() all, the first being the whole when the property is not a list.
  const original = CSSStyleValue.parse;
  void original;

  // ---- Style property maps ----

  const iterate = (entries) => entries[Symbol.iterator]();
  // How a value goes into a declaration: the text, and whether it needs to be wrapped to be accepted.
  const textOfValue = (value) => {
    if (typeof value === 'string') return value;
    if (value instanceof CSSStyleValue) return value.toString();
    throw new TypeError('Failed to convert value to CSSStyleValue');
  };

  class StylePropertyMapReadOnly {
    constructor() {
      if (!T.__mapBuilding) T.illegal();
    }
    get(property) {
      const values = this.getAll(property);
      return values.length ? values[0] : undefined;
    }
    getAll(property) {
      const name = lookup(String(property));
      const text = this[INTERNAL].read(name.name);
      if (text === '') return [];
      if (name.kind === 'shorthand') {
        if (/^(?:initial|inherit|unset|revert|revert-layer)$/i.test(text.trim())) return [new CSSKeywordValue(text.trim())];
        return [containsVar(text) ? unparsedOf(text.trim()) : associate(makeGeneric(text), name.name)];
      }
      return reify(name.name, text, name.list, name.keywords);
    }
    has(property) {
      const name = lookup(String(property));
      return this[INTERNAL].read(name.name) !== '';
    }
    get size() { return this[INTERNAL].names().length; }
    forEach(callback, thisArg) {
      for (const [name, values] of this.entries()) callback.call(thisArg, values, name, this);
    }
    entries() {
      return iterate(this[INTERNAL].names().map((name) => [name, this.getAll(name)]));
    }
    keys() { return iterate(this[INTERNAL].names()); }
    values() { return iterate(this[INTERNAL].names().map((name) => this.getAll(name))); }
    [Symbol.iterator]() { return this.entries(); }
  }
  defineInterface('StylePropertyMapReadOnly', StylePropertyMapReadOnly);

  const lookup = (property) => {
    const info = infoOf(property);
    if (info === null) throw new TypeError("Invalid property name: " + property);
    return { name: info.kind === 'custom' ? property : info.name, kind: info.kind, list: info.list, keywords: info.keywords };
  };

  class StylePropertyMap extends StylePropertyMapReadOnly {
    set(property, ...values) {
      const info = lookup(String(property));
      if (values.length === 0) throw new TypeError("Failed to execute 'set' on 'StylePropertyMap': 2 arguments required");
      this[INTERNAL].write(info.name, this.#join(info, values, []));
    }
    append(property, ...values) {
      const info = lookup(String(property));
      if (!info.list && info.kind !== 'custom') throw new TypeError("Failed to execute 'append' on 'StylePropertyMap': The property is not list-valued");
      if (values.some((v) => v instanceof CSSUnparsedValue)) throw new TypeError("Failed to execute 'append' on 'StylePropertyMap': A value with var() cannot be appended");
      if (info.kind === 'shorthand') throw new TypeError("Failed to execute 'append' on 'StylePropertyMap': Cannot append to a shorthand");
      const existing = this[INTERNAL].read(info.name);
      this[INTERNAL].write(info.name, this.#join(info, values, existing === '' ? [] : [existing]));
    }
    delete(property) {
      const info = lookup(String(property));
      this[INTERNAL].remove(info.name);
    }
    clear() { this[INTERNAL].clear(); }
    #join(info, values, before) {
      const texts = before.slice();
      if (info.kind === 'shorthand') {
        if (values.length !== 1 || (typeof values[0] !== 'string' && !(values[0] instanceof CSSStyleValue))) throw new TypeError('A shorthand takes one value');
      } else if (!info.list && info.kind !== 'custom' && values.length !== 1) {
        throw new TypeError('The property takes one value');
      }
      if (values.some((v) => v instanceof CSSUnparsedValue) && (before.length > 0 || values.length > 1)) throw new TypeError('A value with var() cannot be combined with other values');
      for (const value of values) {
        if (typeof value !== 'string' && !(value instanceof CSSStyleValue)) throw new TypeError('Failed to convert value to CSSStyleValue');
        if (typeof value === 'string' && containsVar(value) && info.kind !== 'custom' && info.list) throw new TypeError('A value with var() cannot be one of a list');
        if (typeof value !== 'string' && value[ASSOC] !== undefined && value[ASSOC] !== info.name) throw new TypeError('The value is from another property');
        texts.push(textOfValue(value));
      }
      const text = texts.join(info.list || info.kind === 'custom' ? ', ' : ' ');
      // Valid? Asked of a scratch block: out of range numbers are accepted in a calculation, and clamped when used.
      const declarations = scratchDeclarations();
      const check = (candidate) => {
        declarations.removeProperty(info.name);
        declarations.setProperty(info.name, candidate);
        const stored = declarations.getPropertyValue(info.name);
        declarations.removeProperty(info.name);
        return stored !== '';
      };
      const numberZero = values.length === 1 && values[0] instanceof CSSUnitValue && values[0].unit === 'number' && values[0].value === 0;
      if (info.kind === 'custom' || (check(text) && (!numberZero || check('calc(0)')))) return text;
      if (values.length === 1 && values[0] instanceof CSSUnitValue && check('calc(' + text + ')')) return 'calc(' + text + ')';
      throw new TypeError('Failed to set the value: it is not valid for ' + info.name);
    }
  }
  defineInterface('StylePropertyMap', StylePropertyMap);

  const makeMap = (kind, backend) => {
    T.__mapBuilding = true;
    try {
      const map = new (kind === 'readonly' ? StylePropertyMapReadOnly : StylePropertyMap)();
      Object.defineProperty(map, INTERNAL, { value: backend, enumerable: false });
      return map;
    } finally {
      T.__mapBuilding = false;
    }
  };
  const declarationBackend = (getDeclarations) => ({
    read: (name) => getDeclarations().getPropertyValue(name),
    write: (name, text) => {
      const declarations = getDeclarations();
      declarations.setProperty(name, text);
    },
    remove: (name) => { getDeclarations().removeProperty(name); },
    clear: () => { getDeclarations().cssText = ''; },
    names: () => {
      const declarations = getDeclarations();
      const names = [];
      for (let i = 0; i < declarations.length; ++i) names.push(declarations[i]);
      return names;
    },
  });

  // ---- Where the maps are: attributeStyleMap, computedStyleMap(), styleMap ----

  const maps = new WeakMap();
  const mapFor = (owner, make) => {
    let map = maps.get(owner);
    if (!map) {
      map = make();
      maps.set(owner, map);
    }
    return map;
  };
  const defineGetter = (prototype, name, getter) => {
    Object.defineProperty(prototype, name, { get: getter, enumerable: true, configurable: true });
  };
  const install = () => {
    const elementPrototype = global.Element.prototype;
    defineGetter(elementPrototype, 'attributeStyleMap', function () {
      return mapFor(this, () => makeMap('writable', declarationBackend(() => this.style)));
    });
    Object.defineProperty(elementPrototype, 'computedStyleMap', {
      value: function computedStyleMap() {
        const element = this;
        return makeMap('readonly', {
          read: (name) => getComputedStyle(element).getPropertyValue(name),
          names: () => {
            const style = getComputedStyle(element);
            const names = [];
            for (let i = 0; i < style.length; ++i) names.push(style[i]);
            return names;
          },
        });
      },
      writable: true,
      enumerable: true,
      configurable: true,
    });
    for (const name of ['CSSStyleRule', 'CSSKeyframeRule', 'CSSPageRule', 'CSSNestedDeclarations']) {
      const prototype = global[name] && global[name].prototype;
      if (!prototype) continue;
      defineGetter(prototype, 'styleMap', function () {
        return mapFor(this, () => makeMap('writable', declarationBackend(() => this.style)));
      });
    }
    T.installFactories();
  };
  T.install = install;
})();
