// More of CSSOM View over the layout natives: scrollIntoView, scrollParent, checkVisibility and GeometryUtils.
(function () {
  'use strict';
  const global = globalThis;
  const shared = global.__solarLayoutShared;
  delete global.__solarLayoutShared;
  global.__solarLayoutShared2 = { processedText: shared.processedText };
  const scrollNative = shared.scrollNative;
  const rangeRects = shared.rangeRects;
  const define = (target, name, descriptor) => Object.defineProperty(target, name, { enumerable: true, configurable: true, ...descriptor });

  const px = (text) => {
    const n = parseFloat(text);
    return Number.isFinite(n) ? n : 0;
  };
  const computed = (el, pseudo) => global.getComputedStyle(el, pseudo || null);

  // ---- Scrolling boxes ----

  const flatParent = (node) => {
    if (node.assignedSlot) return node.assignedSlot;
    const parent = node.parentNode;
    if (parent instanceof ShadowRoot) return parent.host;
    return parent;
  };
  const scrollsOverflow = (value) => value === 'hidden' || value === 'scroll' || value === 'auto';
  const isScrollContainer = (el) => {
    const cs = computed(el);
    return scrollsOverflow(cs.overflowX) || scrollsOverflow(cs.overflowY);
  };
  const scrollingElementOf = (doc) => (doc.compatMode === 'BackCompat' ? doc.body : doc.documentElement);

  // ---- scrollIntoView ----

  const ALIGN = ['start', 'center', 'end', 'nearest'];
  const BEHAVIOR = ['auto', 'instant', 'smooth'];

  // The physical sides the starts of the block and inline axes are at, for a writing mode and a direction.
  const axesOf = (cs) => {
    const mode = cs.writingMode;
    const rtl = cs.direction === 'rtl';
    const vertical = mode.startsWith('vertical') || mode.startsWith('sideways');
    if (!vertical) return { blockAxis: 'y', blockStartMin: true, inlineStartMin: !rtl };
    const blockStartMin = mode === 'vertical-lr' || mode === 'sideways-lr';
    let inlineStartMin = !rtl;
    if (mode === 'sideways-lr') inlineStartMin = rtl;
    return { blockAxis: 'x', blockStartMin, inlineStartMin };
  };

  const delta = (alignment, targetMin, targetMax, portMin, portMax, startMin) => {
    const targetSize = targetMax - targetMin, portSize = portMax - portMin;
    switch (alignment) {
      case 'start': return startMin ? targetMin - portMin : targetMax - portMax;
      case 'end': return startMin ? targetMax - portMax : targetMin - portMin;
      case 'center': return (targetMin + targetMax) / 2 - (portMin + portMax) / 2;
      default: {
        const beforeStart = targetMin < portMin, afterEnd = targetMax > portMax;
        if (!beforeStart && !afterEnd) return 0;
        if (beforeStart && afterEnd) return 0;
        if (beforeStart) return targetSize < portSize || !startMin ? targetMin - portMin : targetMax - portMax;
        return targetSize < portSize || startMin ? targetMax - portMax : targetMin - portMin;
      }
    }
  };

  const edges = (cs, prefix) => ({
    top: px(cs[prefix + 'Top']), right: px(cs[prefix + 'Right']), bottom: px(cs[prefix + 'Bottom']), left: px(cs[prefix + 'Left']),
  });

  const scrollIntoViewImpl = function (el, argument) {
    let block = 'start', inline = 'nearest';
    if (argument === undefined) {
      // the defaults
    } else if (argument === true || argument === false) {
      block = argument ? 'start' : 'end';
    } else if (argument === null || typeof argument === 'object' || typeof argument === 'function') {
      const o = argument === null ? {} : argument;
      if (o.behavior !== undefined && !BEHAVIOR.includes(String(o.behavior))) throw new TypeError("Failed to execute 'scrollIntoView' on 'Element': The provided value '" + o.behavior + "' is not a valid enum value of type ScrollBehavior.");
      if (o.block !== undefined) {
        block = String(o.block);
        if (!ALIGN.includes(block)) throw new TypeError("Failed to execute 'scrollIntoView' on 'Element': The provided value '" + block + "' is not a valid enum value of type ScrollLogicalPosition.");
      }
      if (o.inline !== undefined) {
        inline = String(o.inline);
        if (!ALIGN.includes(inline)) throw new TypeError("Failed to execute 'scrollIntoView' on 'Element': The provided value '" + inline + "' is not a valid enum value of type ScrollLogicalPosition.");
      }
    } else {
      block = argument ? 'start' : 'end';
    }
    if (!el.isConnected || el.getClientRects().length === 0) return;
    const doc = el.ownerDocument;
    const targetStyle = computed(el);
    const margin = edges(targetStyle, 'scrollMargin');
    // Every scrolling box that holds the element, the nearest first, and the viewport at the end.
    const boxes = [];
    for (let node = flatParent(el); node; node = flatParent(node)) {
      if (!(node instanceof Element)) continue;
      if (node === scrollingElementOf(doc) || node === doc.documentElement) continue;
      if (isScrollContainer(node)) boxes.push(node);
    }
    boxes.push(null);
    for (const box of boxes) {
      const rects = el.getClientRects();
      let l = Infinity, t = Infinity, r = -Infinity, b = -Infinity;
      for (const q of rects) {
        l = Math.min(l, q.left);
        t = Math.min(t, q.top);
        r = Math.max(r, q.right);
        b = Math.max(b, q.bottom);
      }
      // The target with its scroll margin.
      const target = { left: l - margin.left, top: t - margin.top, right: r + margin.right, bottom: b + margin.bottom };
      let port, cs, current;
      if (box) {
        const br = box.getBoundingClientRect();
        port = { left: br.left + box.clientLeft, top: br.top + box.clientTop, right: br.left + box.clientLeft + box.clientWidth, bottom: br.top + box.clientTop + box.clientHeight };
        cs = computed(box);
        current = scrollNative(box, false, 0, 0);
      } else {
        const root = doc.documentElement;
        const view = scrollingElementOf(doc);
        port = { left: 0, top: 0, right: root.clientWidth, bottom: root.clientHeight };
        cs = computed(root);
        current = scrollNative(null, false, 0, 0);
        void view;
      }
      const padding = edges(cs, 'scrollPadding');
      port = { left: port.left + padding.left, top: port.top + padding.top, right: port.right - padding.right, bottom: port.bottom - padding.bottom };
      const axes = axesOf(cs);
      const alignmentX = axes.blockAxis === 'x' ? block : inline;
      const alignmentY = axes.blockAxis === 'y' ? block : inline;
      const startMinX = axes.blockAxis === 'x' ? axes.blockStartMin : axes.inlineStartMin;
      const startMinY = axes.blockAxis === 'y' ? axes.blockStartMin : axes.inlineStartMin;
      const dx = delta(alignmentX, target.left, target.right, port.left, port.right, startMinX);
      const dy = delta(alignmentY, target.top, target.bottom, port.top, port.bottom, startMinY);
      if (dx !== 0 || dy !== 0) scrollNative(box, true, current[0] + dx, current[1] + dy);
    }
  };

  define(Element.prototype, 'scrollIntoView', {
    value: function scrollIntoView(argument) {
      scrollIntoViewImpl(this, argument);
    },
    writable: true,
  });

  define(Element.prototype, 'scrollIntoViewIfNeeded', {
    value: function scrollIntoViewIfNeeded(centerIfNeeded) {
      const center = centerIfNeeded === undefined ? true : !!centerIfNeeded;
      scrollIntoViewImpl(this, { block: center ? 'center' : 'nearest', inline: center ? 'center' : 'nearest' });
    },
    writable: true,
  });

  // ---- scrollParent ----

  const hasContainingBlockForFixed = (cs) => cs.transform !== 'none' || cs.translate !== 'none' || cs.rotate !== 'none' || cs.scale !== 'none' || cs.perspective !== 'none' || cs.filter !== 'none' || /paint|layout|strict|content/.test(cs.contain || '') || /transform|perspective|filter/.test(cs.willChange || '');
  define(HTMLElement.prototype, 'scrollParent', {
    value: function scrollParent() {
      const el = this;
      const doc = el.ownerDocument;
      if (!el.isConnected || computed(el).display === 'none') return null;
      if (el === doc.documentElement || el === scrollingElementOf(doc) && el === doc.documentElement) return null;
      if (el === scrollingElementOf(doc)) return null;
      const position = computed(el).position;
      let node = flatParent(el);
      const chain = [];
      for (let p = node; p; p = flatParent(p)) if (p instanceof Element) chain.push(p);
      let start = 0;
      if (position === 'absolute' || position === 'fixed') {
        // Starts at the containing block: the nearest box the offsets are relative to.
        start = -1;
        for (let i = 0; i < chain.length; i++) {
          const cs = computed(chain[i]);
          if (cs.display === 'contents') continue;
          const establishes = position === 'absolute' ? cs.position !== 'static' || hasContainingBlockForFixed(cs) : hasContainingBlockForFixed(cs);
          if (establishes) {
            start = i;
            break;
          }
        }
        if (start < 0) {
          // The initial containing block: the viewport; nothing scrolls it, but for the document's scrolling element for an absolute one.
          return position === 'fixed' ? null : scrollingElementOf(doc);
        }
      }
      for (let i = start; i < chain.length; i++) {
        const cs = computed(chain[i]);
        if (cs.display === 'contents') continue;
        if (chain[i] === doc.documentElement || chain[i] === scrollingElementOf(doc)) return scrollingElementOf(doc);
        if (scrollsOverflow(cs.overflowX) || scrollsOverflow(cs.overflowY)) return chain[i];
      }
      return scrollingElementOf(doc);
    },
    writable: true,
  });

  // ---- checkVisibility ----

  define(Element.prototype, 'checkVisibility', {
    value: function checkVisibility(options) {
      const o = options === undefined || options === null ? {} : options;
      const checkOpacity = !!(o.checkOpacity || o.opacityProperty);
      const checkVisibilityCSS = !!(o.checkVisibilityCSS || o.visibilityProperty);
      const contentVisibilityAuto = !!o.contentVisibilityAuto;
      const el = this;
      if (!el.isConnected) return false;
      if (computed(el).display === 'none') return false;
      // No box: display: contents, or inside something that has none.
      if (computed(el).display === 'contents') return false;
      for (let node = el; node; node = flatParent(node)) {
        if (!(node instanceof Element)) continue;
        const cs = computed(node);
        if (cs.display === 'none') return false;
        if (checkOpacity && parseFloat(cs.opacity) === 0) return false;
        // Content that is skipped is not visible, other than the element that skips it.
        if (node !== el) {
          const cv = cs.contentVisibility;
          if (cv === 'hidden') return false;
          if (cv === 'auto' && contentVisibilityAuto) {
            const r = node.getBoundingClientRect();
            const root = el.ownerDocument.documentElement;
            if (r.bottom < 0 || r.right < 0 || r.top > root.clientHeight || r.left > root.clientWidth) return false;
          }
        }
      }
      if (checkVisibilityCSS && computed(el).visibility !== 'visible') return false;
      if (el.getClientRects().length === 0) {
        // (An element with no box that is not display: none is one inside a skipped subtree.)
        return false;
      }
      return true;
    },
    writable: true,
  });

  // ---- The screen, and the event of a media query list ----

  if (typeof global.screen === 'object' && global.screen !== null && typeof global.Screen === 'undefined') {
    const values = { ...global.screen };
    class Screen {
      constructor() {
        throw new TypeError('Illegal constructor');
      }
    }
    for (const name of ['availWidth', 'availHeight', 'width', 'height', 'colorDepth', 'pixelDepth', 'availLeft', 'availTop']) {
      define(Screen.prototype, name, { get() { return values[name]; }, set: undefined });
    }
    Object.defineProperty(Screen.prototype, Symbol.toStringTag, { value: 'Screen', configurable: true });
    const screen = Object.create(Screen.prototype);
    Object.defineProperty(global, 'Screen', { value: Screen, writable: true, configurable: true });
    Object.defineProperty(global, 'screen', { get() { return screen; }, set: undefined, enumerable: true, configurable: true });
  }
  if (typeof global.MediaQueryListEvent === 'undefined' && typeof global.Event === 'function') {
    const state = new WeakMap();
    class MediaQueryListEvent extends Event {
      constructor(type, init = {}) {
        super(type, init);
        const dict = init === undefined || init === null ? {} : init;
        state.set(this, { media: dict.media === undefined ? '' : String(dict.media), matches: !!dict.matches });
      }
      get media() { return state.get(this).media; }
      get matches() { return state.get(this).matches; }
    }
    for (const key of ['media', 'matches']) Object.defineProperty(MediaQueryListEvent.prototype, key, { ...Object.getOwnPropertyDescriptor(MediaQueryListEvent.prototype, key), enumerable: true });
    Object.defineProperty(MediaQueryListEvent.prototype, Symbol.toStringTag, { value: 'MediaQueryListEvent', configurable: true });
    Object.defineProperty(global, 'MediaQueryListEvent', { value: MediaQueryListEvent, writable: true, configurable: true });
  }

  // ---- Range geometry ----

  const rangeBoxes = (range) => {
    const out = [];
    if (range.collapsed) return out;
    const root = range.commonAncestorContainer;
    const walker = range.startContainer.ownerDocument ? range.startContainer.ownerDocument.createTreeWalker(root, 5) : null;
    const visit = (node) => {
      if (node.nodeType === 3) {
        const start = node === range.startContainer ? range.startOffset : 0;
        const end = node === range.endContainer ? range.endOffset : node.data.length;
        if (end > start && range.intersectsNode(node)) {
          const flat = rangeRects(node, start, end);
          for (let i = 0; i < flat.length; i += 4) out.push({ x: flat[i], y: flat[i + 1], width: flat[i + 2], height: flat[i + 3] });
        }
        return;
      }
      if (node.nodeType !== 1 || node === range.commonAncestorContainer) return;
    };
    if (root.nodeType === 3) {
      visit(root);
      return out;
    }
    // Text in the range, and the boxes of the elements the range holds whole.
    const nodes = [];
    if (walker) {
      let node = walker.currentNode;
      void node;
      for (node = walker.nextNode(); node; node = walker.nextNode()) nodes.push(node);
    }
    for (const node of nodes) {
      if (!range.intersectsNode(node)) continue;
      if (node.nodeType === 3) visit(node);
      else if (node.nodeType === 1) {
        // An element selected whole whose parent is not.
        const whole = range.comparePoint(node, 0) >= 0 && range.comparePoint(node, node.childNodes.length) <= 0;
        const parent = node.parentNode;
        const parentWhole = parent && parent !== root && range.comparePoint(parent, 0) >= 0 && range.comparePoint(parent, parent.childNodes.length) <= 0;
        if (whole && !parentWhole) for (const r of node.getClientRects()) out.push({ x: r.left, y: r.top, width: r.width, height: r.height });
      }
    }
    return out;
  };
  define(Range.prototype, 'getClientRects', {
    value: function getClientRects() {
      const rects = rangeBoxes(this).map((r) => new DOMRect(r.x, r.y, r.width, r.height));
      const list = Object.create(global.DOMRectList.prototype);
      rects.forEach((r, i) => { list[i] = r; });
      Object.defineProperty(list, '__length', { value: rects.length, enumerable: false, writable: true });
      return list;
    },
    writable: true,
  });
  define(Range.prototype, 'getBoundingClientRect', {
    value: function getBoundingClientRect() {
      const rects = rangeBoxes(this);
      if (!rects.length) return new DOMRect(0, 0, 0, 0);
      let l = Infinity, t = Infinity, r = -Infinity, b = -Infinity;
      for (const q of rects) {
        l = Math.min(l, q.x);
        t = Math.min(t, q.y);
        r = Math.max(r, q.x + q.width);
        b = Math.max(b, q.y + q.height);
      }
      return new DOMRect(l, t, r - l, b - t);
    },
    writable: true,
  });

  // ---- GeometryUtils ----

  const BOXES = ['margin', 'border', 'padding', 'content'];
  const checkBox = (value, name) => {
    const box = value === undefined ? 'border' : String(value);
    if (!BOXES.includes(box)) throw new TypeError("Failed to execute '" + name + "': The provided value '" + box + "' is not a valid enum value of type CSSBoxType.");
    return box;
  };

  // The rectangles of an element's box of the kind, in the viewport.
  const boxRects = (el, box) => {
    const cs = computed(el);
    const margin = edges(cs, 'margin');
    const border = { top: px(cs.borderTopWidth), right: px(cs.borderRightWidth), bottom: px(cs.borderBottomWidth), left: px(cs.borderLeftWidth) };
    const padding = edges(cs, 'padding');
    const out = [];
    for (const r of el.getClientRects()) {
      let l = r.left, t = r.top, w = r.width, h = r.height;
      const grow = (e, sign) => {
        l -= sign * e.left;
        t -= sign * e.top;
        w += sign * (e.left + e.right);
        h += sign * (e.top + e.bottom);
      };
      if (box === 'margin') grow(margin, 1);
      else if (box === 'padding') grow(border, -1);
      else if (box === 'content') {
        grow(border, -1);
        grow(padding, -1);
      }
      out.push({ x: l, y: t, width: Math.max(0, w), height: Math.max(0, h) });
    }
    return out;
  };

  // Where the coordinate space of a node has its origin, in the viewport: the corner of its border box (the margin, padding or content box for `box`).
  const originOf = (node, box) => {
    if (node.nodeType === 9) return { x: 0, y: 0 };
    const el = node.nodeType === 3 ? node.parentElement : node;
    if (!el) return { x: 0, y: 0 };
    const rects = boxRects(el, box || 'border');
    if (!rects.length) return { x: 0, y: 0 };
    return { x: rects[0].x, y: rects[0].y };
  };

  const quadOf = (rect) => new DOMQuad({ x: rect.x, y: rect.y }, { x: rect.x + rect.width, y: rect.y }, { x: rect.x + rect.width, y: rect.y + rect.height }, { x: rect.x, y: rect.y + rect.height });

  const geometryNode = (value, what) => {
    if (!(value instanceof Node) || ![1, 3, 9].includes(value.nodeType)) throw new TypeError("Failed to execute '" + what + "': The provided value is not a Text, Element or Document.");
    return value;
  };

  const getBoxQuads = function getBoxQuads(options) {
    const o = options === undefined || options === null ? {} : options;
    const box = checkBox(o.box, 'getBoxQuads');
    const relativeTo = o.relativeTo === undefined ? null : geometryNode(o.relativeTo, 'getBoxQuads');
    let rects = [];
    if (this.nodeType === 1) rects = boxRects(this, box);
    else if (this.nodeType === 9) rects = [{ x: 0, y: 0, width: this.documentElement ? this.documentElement.clientWidth : 0, height: this.documentElement ? this.documentElement.clientHeight : 0 }];
    else if (this.nodeType === 3) {
      const range = this.ownerDocument.createRange();
      range.selectNodeContents(this);
      rects = [...range.getClientRects()].map((r) => ({ x: r.left, y: r.top, width: r.width, height: r.height }));
    }
    let shift = { x: 0, y: 0 };
    if (relativeTo) {
      const origin = originOf(relativeTo, 'border');
      shift = { x: -origin.x, y: -origin.y };
    }
    return rects.map((r) => quadOf({ x: r.x + shift.x, y: r.y + shift.y, width: r.width, height: r.height }));
  };

  const convertPoint = function (point, from, options) {
    const node = geometryNode(from, 'convertPointFromNode');
    const o = options === undefined || options === null ? {} : options;
    const fromBox = checkBox(o.fromBox, 'convertPointFromNode'), toBox = checkBox(o.toBox, 'convertPointFromNode');
    const source = originOf(node, fromBox), target = originOf(this, toBox);
    const p = point === undefined || point === null ? {} : point;
    const x = p.x === undefined ? 0 : Number(p.x), y = p.y === undefined ? 0 : Number(p.y), z = p.z === undefined ? 0 : Number(p.z), w = p.w === undefined ? 1 : Number(p.w);
    return new DOMPoint(x + source.x - target.x, y + source.y - target.y, z, w);
  };
  const convertRect = function (rect, from, options) {
    const node = geometryNode(from, 'convertRectFromNode');
    const o = options === undefined || options === null ? {} : options;
    const fromBox = checkBox(o.fromBox, 'convertRectFromNode'), toBox = checkBox(o.toBox, 'convertRectFromNode');
    const source = originOf(node, fromBox), target = originOf(this, toBox);
    const r = rect === undefined || rect === null ? {} : rect;
    return new DOMQuad(
      { x: Number(r.x || 0) + source.x - target.x, y: Number(r.y || 0) + source.y - target.y },
      { x: Number(r.x || 0) + Number(r.width || 0) + source.x - target.x, y: Number(r.y || 0) + source.y - target.y },
      { x: Number(r.x || 0) + Number(r.width || 0) + source.x - target.x, y: Number(r.y || 0) + Number(r.height || 0) + source.y - target.y },
      { x: Number(r.x || 0) + source.x - target.x, y: Number(r.y || 0) + Number(r.height || 0) + source.y - target.y },
    );
  };
  const convertQuad = function (quad, from, options) {
    const node = geometryNode(from, 'convertQuadFromNode');
    const o = options === undefined || options === null ? {} : options;
    const fromBox = checkBox(o.fromBox, 'convertQuadFromNode'), toBox = checkBox(o.toBox, 'convertQuadFromNode');
    const source = originOf(node, fromBox), target = originOf(this, toBox);
    const q = quad === undefined || quad === null ? {} : quad;
    const move = (pt) => ({ x: Number((pt && pt.x) || 0) + source.x - target.x, y: Number((pt && pt.y) || 0) + source.y - target.y, z: Number((pt && pt.z) || 0), w: pt && pt.w !== undefined ? Number(pt.w) : 1 });
    return new DOMQuad(move(q.p1), move(q.p2), move(q.p3), move(q.p4));
  };
  for (const proto of [Element.prototype, Text.prototype, Document.prototype]) {
    define(proto, 'getBoxQuads', { value: getBoxQuads, writable: true });
    define(proto, 'convertQuadFromNode', { value: function convertQuadFromNode(quad, from, options) { return convertQuad.call(this, quad, from, options); }, writable: true });
    define(proto, 'convertRectFromNode', { value: function convertRectFromNode(rect, from, options) { return convertRect.call(this, rect, from, options); }, writable: true });
    define(proto, 'convertPointFromNode', { value: function convertPointFromNode(point, from, options) { return convertPoint.call(this, point, from, options); }, writable: true });
  }
})();
