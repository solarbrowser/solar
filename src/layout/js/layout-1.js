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
  const scrollNative = global.__solarLayoutScroll;
  const elementsAt = global.__solarLayoutElementsAt;
  delete global.__solarLayoutScroll;
  delete global.__solarLayoutElementsAt;

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
  const toNumber = (v) => {
    const n = Number(v);
    return Number.isFinite(n) ? n : 0;
  };
  define(Element.prototype, 'scrollTop', {
    get() { return scrollNative(element(this), false, 0, 0)[1]; },
    set(v) { const el = element(this); const [x] = scrollNative(el, false, 0, 0); scrollNative(el, true, x, toNumber(v)); },
  });
  define(Element.prototype, 'scrollLeft', {
    get() { return scrollNative(element(this), false, 0, 0)[0]; },
    set(v) { const el = element(this); const [, y] = scrollNative(el, false, 0, 0); scrollNative(el, true, toNumber(v), y); },
  });
  // scroll(x, y) and scroll({ left, top, behavior }); an element scrolls to where it is told, a missing coordinate being where it is.
  const scrollArguments = (args, current) => {
    if (args.length === 0) return current;
    if (args.length === 1) {
      const o = args[0];
      if (o === null || (typeof o !== 'object' && typeof o !== 'function' && o !== undefined)) throw new TypeError('The provided value is not of type ScrollToOptions.');
      return [o && o.left !== undefined ? toNumber(o.left) : current[0], o && o.top !== undefined ? toNumber(o.top) : current[1]];
    }
    return [toNumber(args[0]), toNumber(args[1])];
  };
  const scrollMethods = (target, resolve) => {
    define(target, 'scroll', { value: function scroll(...args) { const el = resolve(this); const cur = scrollNative(el, false, 0, 0); const [x, y] = scrollArguments(args, cur); scrollNative(el, true, x, y); }, writable: true });
    define(target, 'scrollTo', { value: function scrollTo(...args) { const el = resolve(this); const cur = scrollNative(el, false, 0, 0); const [x, y] = scrollArguments(args, cur); scrollNative(el, true, x, y); }, writable: true });
    define(target, 'scrollBy', {
      value: function scrollBy(...args) {
        const el = resolve(this);
        const cur = scrollNative(el, false, 0, 0);
        const delta = scrollArguments(args, [0, 0]);
        if (args.length === 1) {
          const o = args[0] || {};
          scrollNative(el, true, cur[0] + toNumber(o.left), cur[1] + toNumber(o.top));
        } else {
          scrollNative(el, true, cur[0] + delta[0], cur[1] + delta[1]);
        }
      },
      writable: true,
    });
  };
  scrollMethods(Element.prototype, element);
  scrollMethods(global, () => null);
  for (const [name, index] of [['scrollX', 0], ['pageXOffset', 0], ['scrollY', 1], ['pageYOffset', 1]]) {
    Object.defineProperty(global, name, { get() { return scrollNative(null, false, 0, 0)[index]; }, set(v) {}, enumerable: true, configurable: true });
  }
  define(Document.prototype, 'scrollingElement', {
    get() { return this.compatMode === 'BackCompat' ? this.body : this.documentElement; },
  });
  // What is under a point, as seen from a tree: nodes in other trees are stood for by their hosts there.
  const seenFrom = (scope, x, y) => {
    const out = [];
    // A node is seen as it is when its tree is the scope's or one around it, and by the host of its tree otherwise.
    const around = (root) => {
      for (let r = scope; r; r = r instanceof ShadowRoot ? r.host.getRootNode() : null) if (r === root) return true;
      return false;
    };
    for (let node of elementsAt(Number(x), Number(y))) {
      while (node && !around(node.getRootNode())) {
        const root = node.getRootNode();
        node = root instanceof ShadowRoot ? root.host : null;
      }
      if (node && !out.includes(node)) out.push(node);
    }
    return out;
  };
  for (const [prototype, name] of [[Document.prototype, 'Document'], [ShadowRoot.prototype, 'ShadowRoot']]) {
    define(prototype, 'elementFromPoint', {
      value: function elementFromPoint(x, y) {
        if (arguments.length < 2) throw new TypeError("Failed to execute 'elementFromPoint' on '" + name + "': 2 arguments required, but only " + arguments.length + ' present.');
        const list = seenFrom(this, x, y);
        return list.length ? list[0] : null;
      },
      writable: true,
    });
    define(prototype, 'elementsFromPoint', {
      value: function elementsFromPoint(x, y) {
        if (arguments.length < 2) throw new TypeError("Failed to execute 'elementsFromPoint' on '" + name + "': 2 arguments required, but only " + arguments.length + ' present.');
        return seenFrom(this, x, y);
      },
      writable: true,
    });
  }

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
