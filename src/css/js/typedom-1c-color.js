// CSS Typed OM: color values.
(function () {
  'use strict';
  const global = globalThis;
  const T = global.__solarTypedOm;
  const { INTERNAL, defineInterface, CSSStyleValue, CSSNumericValue, CSSUnitValue, CSSKeywordValue } = T;
  const computeColor = global.__solarTypedComputeColor;
  delete global.__solarTypedComputeColor;

  const syntaxError = (message) => new DOMException(message, 'SyntaxError');
  const keysOf = (dictionary) => Object.keys(dictionary).filter((k) => k !== 'percentHint');
  const isNumberType = (value) => keysOf(value.type()).length === 0;
  const isPercentType = (value) => {
    const d = value.type();
    return keysOf(d).length === 1 && d.percent === 1;
  };
  const isAngleType = (value) => {
    const d = value.type();
    return keysOf(d).length === 1 && d.angle === 1;
  };
  const numeric = (value) => {
    if (value instanceof CSSNumericValue) return value;
    throw syntaxError('The channel must be a number or a CSSNumericValue');
  };

  // The channel kinds: how a given value becomes the one kept.
  const channel = {
    // a number is a fraction, and becomes a percentage; a percentage stays one
    percent(value) {
      if (typeof value === 'number') return new CSSUnitValue(value * 100, 'percent');
      return isPercentType(numeric(value)) ? value : fail();
    },
    // a number or a percentage
    number(value) {
      if (typeof value === 'number') return new CSSUnitValue(value, 'number');
      numeric(value);
      return isNumberType(value) || isPercentType(value) ? value : fail();
    },
    // a fraction as a percentage, a number or a percentage as it is
    rgb(value) {
      if (typeof value === 'number') return new CSSUnitValue(value * 100, 'percent');
      numeric(value);
      return isNumberType(value) || isPercentType(value) ? value : fail();
    },
    angle(value) {
      if (value === undefined) return new CSSKeywordValue('undefined');
      if (typeof value === 'number') return new CSSUnitValue(value, 'deg');
      return isAngleType(numeric(value)) ? value : fail();
    },
    // (hwb() takes only a numeric value for its hue)
    angleOnly(value) {
      if (!(value instanceof CSSNumericValue)) throw new TypeError('The hue must be a CSSNumericValue');
      return isAngleType(value) ? value : fail();
    },
    alphaSet(value) {
      if (value === undefined) fail();
      return channel.alpha(value);
    },
    alpha(value) {
      if (value === undefined) return new CSSUnitValue(100, 'percent');
      if (typeof value === 'number') return new CSSUnitValue(value * 100, 'percent');
      return isPercentType(numeric(value)) ? value : fail();
    },
  };
  const fail = () => { throw syntaxError('The channel has the wrong type'); };

  class CSSColorValue extends CSSStyleValue {
    constructor() {
      super();
      if (new.target === CSSColorValue) T.illegal();
    }
    static parse(cssText) {
      const value = parseColor(String(cssText));
      if (value === null) throw syntaxError("Failed to execute 'parse' on 'CSSColorValue': Invalid color");
      return value;
    }
  }
  defineInterface('CSSColorValue', CSSColorValue);
  Object.defineProperty(CSSColorValue, 'parse', { enumerable: true });

  // Defines a color interface with channels [name, kind]... and the function it is written with.
  const defineColor = (name, fn, channels, separator) => {
    const kinds = channels.map(([, kind]) => kind);
    const cls = {
      [name]: class extends CSSColorValue {
        constructor(...args) {
          super();
          if (args.length < channels.length) throw new TypeError("Failed to construct '" + name + "': " + channels.length + ' arguments required');
          const values = {};
          channels.forEach(([key], index) => { values[key] = channel[kinds[index]](args[index]); });
          values.alpha = channel.alpha(args[channels.length]);
          Object.defineProperty(this, INTERNAL, { value: values, enumerable: false });
        }
        toString() {
          const v = this[INTERNAL];
          return fn + '(' + channels.map(([key]) => v[key]).join(' ') + ' / ' + v.alpha + ')';
        }
      },
    }[name];
    Object.defineProperty(cls, 'length', { value: channels.length });
    for (const [key, kind] of [...channels, ['alpha', 'alpha']]) {
      Object.defineProperty(cls.prototype, key, {
        get() { return this[INTERNAL][key]; },
        set(value) { this[INTERNAL][key] = channel[kind === 'alpha' ? 'alphaSet' : kind](value); },
        enumerable: true,
        configurable: true,
      });
    }
    defineInterface(name, cls);
    return cls;
  };
  const CSSRGB = defineColor('CSSRGB', 'rgb', [['r', 'rgb'], ['g', 'rgb'], ['b', 'rgb']]);
  const CSSHSL = defineColor('CSSHSL', 'hsl', [['h', 'angle'], ['s', 'percent'], ['l', 'percent']]);
  const CSSHWB = defineColor('CSSHWB', 'hwb', [['h', 'angleOnly'], ['w', 'percent'], ['b', 'percent']]);
  const CSSLab = defineColor('CSSLab', 'lab', [['l', 'percent'], ['a', 'number'], ['b', 'number']]);
  const CSSLCH = defineColor('CSSLCH', 'lch', [['l', 'percent'], ['c', 'percent'], ['h', 'angle']]);
  const CSSOKLab = defineColor('CSSOKLab', 'oklab', [['l', 'percent'], ['a', 'number'], ['b', 'number']]);
  const CSSOKLCH = defineColor('CSSOKLCH', 'oklch', [['l', 'percent'], ['c', 'percent'], ['h', 'angle']]);

  class CSSColor extends CSSColorValue {
    constructor(colorSpace, channels, alpha) {
      super();
      if (arguments.length < 2) throw new TypeError("Failed to construct 'CSSColor': 2 arguments required");
      const space = colorSpace instanceof CSSKeywordValue ? colorSpace : new CSSKeywordValue(String(colorSpace));
      const items = [...channels].map((c) => channel.number(c));
      Object.defineProperty(this, INTERNAL, { value: { colorSpace: space, channels: items, alpha: channel.alpha(alpha) }, enumerable: false });
    }
    get colorSpace() { return this[INTERNAL].colorSpace; }
    set colorSpace(value) { this[INTERNAL].colorSpace = value instanceof CSSKeywordValue ? value : new CSSKeywordValue(String(value)); }
    get channels() { return this[INTERNAL].channels; }
    get alpha() { return this[INTERNAL].alpha; }
    set alpha(value) { this[INTERNAL].alpha = channel.alpha(value); }
    toString() {
      const v = this[INTERNAL];
      return 'color(' + v.colorSpace + ' ' + v.channels.join(' ') + ' / ' + v.alpha + ')';
    }
  }
  defineInterface('CSSColor', CSSColor);
  for (const key of ['colorSpace', 'channels', 'alpha']) {
    const descriptor = Object.getOwnPropertyDescriptor(CSSColor.prototype, key);
    descriptor.enumerable = true;
    Object.defineProperty(CSSColor.prototype, key, descriptor);
  }

  // ---- Parsing ----

  const SYSTEM_COLORS = new Set(['canvas', 'canvastext', 'linktext', 'visitedtext', 'activetext', 'buttonface', 'buttontext', 'buttonborder', 'field', 'fieldtext', 'highlight',
    'highlighttext', 'selecteditem', 'selecteditemtext', 'mark', 'marktext', 'graytext', 'accentcolor', 'accentcolortext', 'activeborder', 'activecaption', 'appworkspace',
    'background', 'buttonhighlight', 'buttonshadow', 'captiontext', 'inactiveborder', 'inactivecaption', 'inactivecaptiontext', 'infobackground', 'infotext', 'menu', 'menutext',
    'scrollbar', 'threeddarkshadow', 'threedface', 'threedhighlight', 'threedlightshadow', 'threedshadow', 'window', 'windowframe', 'windowtext', 'currentcolor']);

  // The numbers of the arguments of a color function, in the Typed OM's way: a number stays a number, a percentage a percentage.
  const argumentValues = (text) => {
    const match = /^([A-Za-z0-9-]+)\(([\s\S]*)\)$/.exec(text.trim());
    if (!match) return null;
    const name = match[1].toLowerCase();
    // split on commas, spaces and the slash
    const parts = [];
    let current = '';
    let depth = 0;
    for (const c of match[2]) {
      if (c === '(') ++depth;
      if (c === ')') --depth;
      if (depth === 0 && (c === ',' || c === '/' || /\s/.test(c))) {
        if (current) parts.push(current);
        current = '';
        if (c === '/') parts.push('/');
      } else {
        current += c;
      }
    }
    if (current) parts.push(current);
    return { name, parts };
  };
  const value = (text) => {
    if (text === 'none') return new CSSKeywordValue('undefined');
    const parsed = CSSNumericValue.parse(text);
    return parsed;
  };
  const withAlpha = (parts, count) => {
    const slash = parts.indexOf('/');
    const channels = (slash < 0 ? parts : parts.slice(0, slash)).slice();
    let alphaText = slash < 0 ? undefined : parts[slash + 1];
    if (slash < 0 && channels.length === count + 1) alphaText = channels.pop();
    if (channels.length !== count) throw syntaxError('Wrong number of channels');
    let alpha;
    if (alphaText !== undefined) {
      const a = value(alphaText);
      alpha = a instanceof CSSUnitValue && a.unit === 'number' ? new CSSUnitValue(a.value * 100, 'percent') : a;
    }
    return { channels: channels.map(value), alpha };
  };
  const asAngle = (v) => (v instanceof CSSUnitValue && v.unit === 'number' ? new CSSUnitValue(v.value, 'deg') : v);
  const parseColor = (text) => {
    text = text.trim();
    if (text === '') return null;
    const lower = text.toLowerCase();
    if (/^(?:initial|inherit|unset|revert|revert-layer|revert-rule|default)$/.test(lower) || text.startsWith('--')) return null;
    try {
      const call = argumentValues(text);
      if (call) {
        const { name, parts } = call;
        if (name === 'rgb' || name === 'rgba') {
          const { channels, alpha } = withAlpha(parts, 3);
          return new CSSRGB(channels[0], channels[1], channels[2], alpha);
        }
        if (name === 'hsl' || name === 'hsla') {
          const { channels, alpha } = withAlpha(parts, 3);
          return new CSSHSL(asAngle(channels[0]), channels[1], channels[2], alpha);
        }
        if (name === 'hwb') {
          const { channels, alpha } = withAlpha(parts, 3);
          return new CSSHWB(asAngle(channels[0]), channels[1], channels[2], alpha);
        }
        if (name === 'lab' || name === 'oklab') {
          const { channels, alpha } = withAlpha(parts, 3);
          return new (name === 'lab' ? CSSLab : CSSOKLab)(channels[0], channels[1], channels[2], alpha);
        }
        if (name === 'lch' || name === 'oklch') {
          const { channels, alpha } = withAlpha(parts, 3);
          return new (name === 'lch' ? CSSLCH : CSSOKLCH)(channels[0], channels[1], asAngle(channels[2]), alpha);
        }
        if (name === 'color') {
          const slash = parts.indexOf('/');
          const channels = parts.slice(1, slash < 0 ? undefined : slash).map(value);
          let alpha;
          if (slash >= 0) {
            const a = value(parts[slash + 1]);
            alpha = a instanceof CSSUnitValue && a.unit === 'number' ? new CSSUnitValue(a.value * 100, 'percent') : a;
          }
          return new CSSColor(parts[0], channels, alpha);
        }
        return null;
      }
      if (SYSTEM_COLORS.has(lower)) return new CSSKeywordValue(lower);
      const computed = computeColor(text);
      if (computed === null) return null;
      const m = /^rgba?\(\s*([\d.+-]+)\s*,\s*([\d.+-]+)\s*,\s*([\d.+-]+)\s*(?:,\s*([\d.+-]+)\s*)?\)$/.exec(computed);
      if (!m) return null;
      return new CSSRGB(new CSSUnitValue(Number(m[1]), 'number'), new CSSUnitValue(Number(m[2]), 'number'), new CSSUnitValue(Number(m[3]), 'number'), m[4] === undefined ? undefined : new CSSUnitValue(Number(m[4]) * 100, 'percent'));
    } catch (e) {
      if (e instanceof DOMException || e instanceof TypeError || e instanceof RangeError) return null;
      throw e;
    }
  };
  void CSSColor;
})();
