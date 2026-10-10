// Geometry of elements (CSSOM View): getBoundingClientRect, getClientRects, client*, offset*, scroll*, over the layout natives.
(function () {
  'use strict';
  const global = globalThis;
  const metrics = global.__solarLayoutMetrics;
  const rects = global.__solarLayoutRects;
  const offsetParentOf = global.__solarLayoutOffsetParent;
  delete global.__solarLayoutMetrics;
  delete global.__solarLayoutRects;
  delete global.__solarLayoutOffsetParent;

  const element = (value, name) => {
    if (!(value instanceof Element)) throw new TypeError('Illegal invocation');
    return value;
  };

  class DOMRectList {
    constructor() {
      throw new TypeError('Illegal constructor');
    }
    get length() {
      return this.__length;
    }
    item(index) {
      return index >= 0 && index < this.__length ? this[index] : null;
    }
    *[Symbol.iterator]() {
      for (let i = 0; i < this.__length; i++) yield this[i];
    }
  }
  Object.defineProperty(DOMRectList.prototype, Symbol.toStringTag, { value: 'DOMRectList', configurable: true });
  Object.defineProperty(DOMRectList.prototype, '__length', { writable: true, value: 0, enumerable: false });

  const makeList = (flat) => {
    const list = Object.create(DOMRectList.prototype);
    const n = flat.length / 4;
    for (let i = 0; i < n; i++) list[i] = new DOMRect(flat[4 * i], flat[4 * i + 1], flat[4 * i + 2], flat[4 * i + 3]);
    Object.defineProperty(list, '__length', { value: n, enumerable: false, writable: true });
    return list;
  };

  const define = (target, name, descriptor) => Object.defineProperty(target, name, { enumerable: true, configurable: true, ...descriptor });

  define(Element.prototype, 'getClientRects', {
    value: function getClientRects() {
      return makeList(rects(element(this)));
    },
    writable: true,
  });
  define(Element.prototype, 'getBoundingClientRect', {
    value: function getBoundingClientRect() {
      const flat = rects(element(this));
      if (flat.length === 0) return new DOMRect(0, 0, 0, 0);
      let l = Infinity, t = Infinity, r = -Infinity, b = -Infinity;
      for (let i = 0; i < flat.length; i += 4) {
        l = Math.min(l, flat[i]);
        t = Math.min(t, flat[i + 1]);
        r = Math.max(r, flat[i] + flat[i + 2]);
        b = Math.max(b, flat[i + 1] + flat[i + 3]);
      }
      return new DOMRect(l, t, r - l, b - t);
    },
    writable: true,
  });

  const getter = (proto, name, index, transform) => {
    define(proto, name, {
      get() {
        return transform(metrics(element(this))[index]);
      },
    });
  };
  const integer = (v) => Math.round(v);
  getter(Element.prototype, 'clientTop', 0, integer);
  getter(Element.prototype, 'clientLeft', 1, integer);
  getter(Element.prototype, 'clientWidth', 2, integer);
  getter(Element.prototype, 'clientHeight', 3, integer);
  getter(Element.prototype, 'scrollWidth', 4, integer);
  getter(Element.prototype, 'scrollHeight', 5, integer);
  define(Element.prototype, 'scrollTop', { get() { element(this); return 0; }, set(v) { element(this); } });
  define(Element.prototype, 'scrollLeft', { get() { element(this); return 0; }, set(v) { element(this); } });

  const html = HTMLElement.prototype;
  getter(html, 'offsetWidth', 8, integer);
  getter(html, 'offsetHeight', 9, integer);
  define(html, 'offsetParent', { get() { return offsetParentOf(element(this)); } });
  const offset = (name, index, axis) => {
    define(html, name, {
      get() {
        const el = element(this);
        const m = metrics(el);
        if (!m[10]) return 0;
        const parent = offsetParentOf(el);
        let value = m[index];
        if (parent && !(parent instanceof HTMLBodyElement)) {
          const pm = metrics(parent);
          // relative to the padding edge of the parent: its border box position plus its border
          value -= pm[index] + pm[axis === 'top' ? 0 : 1];
        }
        return Math.round(value);
      },
    });
  };
  offset('offsetTop', 6, 'top');
  offset('offsetLeft', 7, 'left');

  Object.defineProperty(global, 'DOMRectList', { value: DOMRectList, writable: true, configurable: true, enumerable: false });
})();
