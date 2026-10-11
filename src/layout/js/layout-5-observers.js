// ResizeObserver (https://drafts.csswg.org/resize-observer/) and IntersectionObserver (https://w3c.github.io/IntersectionObserver/): observations are gathered at the
// end of the rendering update, from the geometry layout gives.
(function () {
  'use strict';
  const global = globalThis;
  const R = global.__solarRendering;
  const boxSizes = global.__solarLayoutBoxSizes;
  delete global.__solarRendering;
  delete global.__solarLayoutBoxSizes;
  if (!R || typeof R.addEndStep !== 'function') return;

  const define = (name, value) => Object.defineProperty(global, name, { value, writable: true, configurable: true });
  const reportLater = (error) => setTimeout(() => { throw error; }, 0);
  const isElement = (value) => typeof Element === 'function' && value instanceof Element;
  const illegal = () => { throw new TypeError('Illegal constructor'); };
  const rect = (x, y, w, h) => new DOMRectReadOnly(x, y, w, h);

  // Whether a frame is wanted when the tree changes: while there is something observed.
  const resizeObservers = new Set();
  const intersectionObservers = new Set();
  const updateWanted = () => R.want(resizeObservers.size > 0 || intersectionObservers.size > 0);
  const startFrame = () => { if (typeof global.requestAnimationFrame === 'function') global.requestAnimationFrame(() => {}); };

  // ---- ResizeObserver ----

  const roSlots = new WeakMap();
  const sizeSlots = new WeakMap();
  const entrySlots = new WeakMap();

  class ResizeObserverSize {
    constructor() { illegal(); }
    get inlineSize() { return sizeSlots.get(this).inline; }
    get blockSize() { return sizeSlots.get(this).block; }
  }
  Object.defineProperty(ResizeObserverSize.prototype, Symbol.toStringTag, { value: 'ResizeObserverSize', configurable: true });
  const makeSize = (inline, block) => {
    const size = Object.create(ResizeObserverSize.prototype);
    sizeSlots.set(size, { inline, block });
    return size;
  };

  class ResizeObserverEntry {
    constructor() { illegal(); }
    get target() { return entrySlots.get(this).target; }
    get contentRect() { return entrySlots.get(this).contentRect; }
    get borderBoxSize() { return entrySlots.get(this).borderBoxSize; }
    get contentBoxSize() { return entrySlots.get(this).contentBoxSize; }
    get devicePixelContentBoxSize() { return entrySlots.get(this).devicePixelContentBoxSize; }
  }
  Object.defineProperty(ResizeObserverEntry.prototype, Symbol.toStringTag, { value: 'ResizeObserverEntry', configurable: true });

  // [inline, block] of the box the observation is of; 0 for what has no box.
  const sizeOf = (target, box) => {
    const sizes = boxSizes(target);
    if (!sizes || !target.isConnected) return { border: [0, 0], content: [0, 0], pad: [0, 0], vertical: false };
    let vertical = false;
    try {
      const mode = global.getComputedStyle(target).writingMode || '';
      vertical = mode.startsWith('vertical') || mode.startsWith('sideways');
    } catch (e) { /* no style */ }
    // An element with no layout box of its own (an SVG one) has the size of its bounding box.
    let [bw, bh, cw, ch, pl, pt] = sizes;
    if (sizes[0] === 0 && sizes[1] === 0 && target.namespaceURI === 'http://www.w3.org/2000/svg' && target.localName !== 'svg') {
      const r = target.getBoundingClientRect();
      bw = cw = r.width;
      bh = ch = r.height;
    }
    return { border: vertical ? [bh, bw] : [bw, bh], content: vertical ? [ch, cw] : [cw, ch], pad: [pl, pt], vertical, raw: [cw, ch] };
  };
  const observedSize = (observation) => {
    const s = sizeOf(observation.target, observation.box);
    if (observation.box === 'border-box') return s.border;
    if (observation.box === 'device-pixel-content-box') return s.content.map((v) => Math.round(v));
    return s.content;
  };
  const depthOf = (node) => {
    let depth = 0;
    for (let n = node; n; n = n.parentNode || n.host) depth++;
    return depth;
  };

  const makeEntry = (observation) => {
    const s = sizeOf(observation.target, observation.box);
    const entry = Object.create(ResizeObserverEntry.prototype);
    const raw = s.raw || [0, 0];
    entrySlots.set(entry, {
      target: observation.target,
      contentRect: rect(s.pad[0], s.pad[1], raw[0], raw[1]),
      borderBoxSize: Object.freeze([makeSize(s.border[0], s.border[1])]),
      contentBoxSize: Object.freeze([makeSize(s.content[0], s.content[1])]),
      devicePixelContentBoxSize: Object.freeze([makeSize(Math.round(s.content[0]), Math.round(s.content[1]))]),
    });
    observation.last = observedSize(observation);
    return entry;
  };

  const isActive = (observation) => {
    const now = observedSize(observation);
    return Math.abs(now[0] - observation.last[0]) > 1e-6 || Math.abs(now[1] - observation.last[1]) > 1e-6;
  };

  class ResizeObserver {
    constructor(callback) {
      if (arguments.length === 0) throw new TypeError("Failed to construct 'ResizeObserver': 1 argument required, but only 0 present.");
      if (typeof callback !== 'function') throw new TypeError("Failed to construct 'ResizeObserver': The callback provided as parameter 1 is not a function.");
      roSlots.set(this, { callback, observations: [], active: [], skipped: [] });
    }
    observe(target, options) {
      const slot = roSlots.get(this);
      if (!slot) throw new TypeError('Illegal invocation');
      if (arguments.length === 0) throw new TypeError("Failed to execute 'observe' on 'ResizeObserver': 1 argument required, but only 0 present.");
      if (!isElement(target)) throw new TypeError("Failed to execute 'observe' on 'ResizeObserver': parameter 1 is not of type 'Element'.");
      if (options !== undefined && options !== null && typeof options !== 'object' && typeof options !== 'function') throw new TypeError("Failed to execute 'observe' on 'ResizeObserver': The provided value is not of type 'ResizeObserverOptions'.");
      const box = options && options.box !== undefined ? String(options.box) : 'content-box';
      if (box !== 'content-box' && box !== 'border-box' && box !== 'device-pixel-content-box') {
        throw new TypeError(`Failed to execute 'observe' on 'ResizeObserver': The provided value '${box}' is not a valid enum value of type ResizeObserverBoxOptions.`);
      }
      const at = slot.observations.findIndex((o) => o.target === target);
      if (at >= 0) {
        if (slot.observations[at].box === box) return;
        slot.observations.splice(at, 1);
      }
      // (Nothing has been reported yet: whatever the size is, the first time is a change.)
      slot.observations.push({ target, box, last: [-1, -1] });
      resizeObservers.add(this);
      updateWanted();
      startFrame();
    }
    unobserve(target) {
      const slot = roSlots.get(this);
      if (!slot) throw new TypeError('Illegal invocation');
      if (arguments.length === 0) throw new TypeError("Failed to execute 'unobserve' on 'ResizeObserver': 1 argument required, but only 0 present.");
      if (!isElement(target)) throw new TypeError("Failed to execute 'unobserve' on 'ResizeObserver': parameter 1 is not of type 'Element'.");
      const at = slot.observations.findIndex((o) => o.target === target);
      if (at >= 0) slot.observations.splice(at, 1);
      if (!slot.observations.length) { resizeObservers.delete(this); updateWanted(); }
    }
    disconnect() {
      const slot = roSlots.get(this);
      if (!slot) throw new TypeError('Illegal invocation');
      slot.observations.length = 0;
      slot.active.length = 0;
      slot.skipped.length = 0;
      resizeObservers.delete(this);
      updateWanted();
    }
  }
  Object.defineProperty(ResizeObserver.prototype, Symbol.toStringTag, { value: 'ResizeObserver', configurable: true });

  const broadcastResize = () => {
    let depth = 0;
    for (;;) {
      let any = false;
      for (const observer of resizeObservers) {
        const slot = roSlots.get(observer);
        slot.active = [];
        slot.skipped = [];
        for (const observation of slot.observations) {
          if (!isActive(observation)) continue;
          (depthOf(observation.target) > depth ? slot.active : slot.skipped).push(observation);
          if (slot.active.includes(observation)) any = true;
        }
      }
      if (!any) break;
      let shallowest = Infinity;
      for (const observer of [...resizeObservers]) {
        const slot = roSlots.get(observer);
        if (!slot.active.length) continue;
        const entries = [];
        for (const observation of slot.active) {
          entries.push(makeEntry(observation));
          shallowest = Math.min(shallowest, depthOf(observation.target));
        }
        slot.active = [];
        try {
          slot.callback.call(observer, entries, observer);
        } catch (error) {
          reportLater(error);
        }
      }
      depth = shallowest;
    }
    let skipped = false;
    for (const observer of resizeObservers) if (roSlots.get(observer).skipped.length) skipped = true;
    if (skipped) {
      try {
        global.dispatchEvent(new ErrorEvent('error', { message: 'ResizeObserver loop completed with undelivered notifications.' }));
      } catch (e) { /* no ErrorEvent */ }
    }
  };

  // ---- IntersectionObserver ----

  const ioSlots = new WeakMap();
  const ieSlots = new WeakMap();

  class IntersectionObserverEntry {
    constructor(init) {
      if (arguments.length === 0) throw new TypeError("Failed to construct 'IntersectionObserverEntry': 1 argument required, but only 0 present.");
      if (init === null || typeof init !== 'object') throw new TypeError("Failed to construct 'IntersectionObserverEntry': The provided value is not of type 'IntersectionObserverEntryInit'.");
      if (init.target === undefined) throw new TypeError("Failed to construct 'IntersectionObserverEntry': Failed to read the 'target' property from 'IntersectionObserverEntryInit': Required member is undefined.");
      if (!isElement(init.target)) throw new TypeError("Failed to construct 'IntersectionObserverEntry': Failed to read the 'target' property from 'IntersectionObserverEntryInit': Failed to convert value to 'Element'.");
      const toRect = (r) => {
        if (r === undefined) return rect(0, 0, 0, 0);
        return rect(r.x || 0, r.y || 0, r.width || 0, r.height || 0);
      };
      ieSlots.set(this, {
        time: Number(init.time),
        rootBounds: init.rootBounds === null || init.rootBounds === undefined ? null : toRect(init.rootBounds),
        boundingClientRect: toRect(init.boundingClientRect),
        intersectionRect: toRect(init.intersectionRect),
        isIntersecting: !!init.isIntersecting,
        isVisible: !!init.isVisible,
        intersectionRatio: Number(init.intersectionRatio),
        target: init.target,
      });
    }
    get time() { return ieSlots.get(this).time; }
    get rootBounds() { return ieSlots.get(this).rootBounds; }
    get boundingClientRect() { return ieSlots.get(this).boundingClientRect; }
    get intersectionRect() { return ieSlots.get(this).intersectionRect; }
    get isIntersecting() { return ieSlots.get(this).isIntersecting; }
    get isVisible() { return ieSlots.get(this).isVisible; }
    get intersectionRatio() { return ieSlots.get(this).intersectionRatio; }
    get target() { return ieSlots.get(this).target; }
    toJSON() {
      const s = ieSlots.get(this);
      return { time: s.time, rootBounds: s.rootBounds && s.rootBounds.toJSON(), boundingClientRect: s.boundingClientRect.toJSON(), intersectionRect: s.intersectionRect.toJSON(), isIntersecting: s.isIntersecting, isVisible: s.isVisible, intersectionRatio: s.intersectionRatio };
    }
  }
  Object.defineProperty(IntersectionObserverEntry.prototype, Symbol.toStringTag, { value: 'IntersectionObserverEntry', configurable: true });

  // "10px 5% 0px": the margin of the root, as [top, right, bottom, left] of {value, percent}.
  const parseMargin = (text, what) => {
    const parts = String(text).trim().split(/\s+/).filter((x) => x !== '');
    if (parts.length === 0) parts.push('0px');
    const fail = () => { throw new DOMException(`Failed to construct 'IntersectionObserver': ${what} must be specified in pixels or percent.`, 'SyntaxError'); };
    if (parts.length > 4) fail();
    const values = parts.map((p) => {
      const m = /^(-?(?:\d+\.?\d*|\.\d+)(?:e[+-]?\d+)?)(px|%|cm|mm|q|in|pt|pc)$/i.exec(p);
      if (!m) fail();
      const unit = m[2].toLowerCase();
      const inPixels = { px: 1, cm: 96 / 2.54, mm: 96 / 25.4, q: 96 / 101.6, in: 96, pt: 96 / 72, pc: 16 }[unit];
      if (unit === '%') return { value: Number(m[1]), percent: true };
      return { value: unit === 'px' ? Number(m[1]) : Math.trunc(Number(m[1]) * inPixels), percent: false };
    });
    const [top, right = top, bottom = top, left = right] = values;
    return [top, right, bottom, left];
  };
  const marginText = (margins) => margins.map((m) => m.value + (m.percent ? '%' : 'px')).join(' ');

  class IntersectionObserver {
    constructor(callback, options) {
      if (arguments.length === 0) throw new TypeError("Failed to construct 'IntersectionObserver': 1 argument required, but only 0 present.");
      if (typeof callback !== 'function') throw new TypeError("Failed to construct 'IntersectionObserver': The callback provided as parameter 1 is not a function.");
      const init = options === undefined || options === null ? {} : options;
      if (typeof init !== 'object' && typeof init !== 'function') throw new TypeError("Failed to construct 'IntersectionObserver': The provided value is not of type 'IntersectionObserverInit'.");
      let root = init.root === undefined ? null : init.root;
      if (root !== null && !isElement(root) && !(typeof Document === 'function' && root instanceof Document)) {
        throw new TypeError("Failed to construct 'IntersectionObserver': Failed to read the 'root' property from 'IntersectionObserverInit': The provided value is not of type '(Document or Element)'.");
      }
      const margins = parseMargin(init.rootMargin === undefined ? '0px' : init.rootMargin, 'rootMargin');
      const scrollMargins = parseMargin(init.scrollMargin === undefined ? '0px' : init.scrollMargin, 'scrollMargin');
      let thresholds = [0];
      if (init.threshold !== undefined) {
        const list = typeof init.threshold === 'object' && init.threshold !== null && Symbol.iterator in init.threshold ? [...init.threshold] : [init.threshold];
        thresholds = list.map(Number);
        for (const t of thresholds) if (!Number.isFinite(t)) throw new TypeError("Failed to construct 'IntersectionObserver': The provided double value is non-finite.");
        for (const t of thresholds) if (!(t >= 0 && t <= 1)) throw new RangeError("Failed to construct 'IntersectionObserver': Threshold values must be numbers between 0 and 1.");
        thresholds.sort((a, b) => a - b);
        if (thresholds.length === 0) thresholds = [0];
      }
      const delay = init.delay === undefined ? 0 : Number(init.delay);
      ioSlots.set(this, { callback, root, margins, scrollMargins, thresholds, delay, trackVisibility: !!init.trackVisibility, observations: [], queue: [], scheduled: false });
    }
    get root() { return ioSlots.get(this).root; }
    get rootMargin() { return marginText(ioSlots.get(this).margins); }
    get scrollMargin() { return marginText(ioSlots.get(this).scrollMargins); }
    get thresholds() { return Object.freeze([...ioSlots.get(this).thresholds]); }
    get delay() { return ioSlots.get(this).delay; }
    get trackVisibility() { return ioSlots.get(this).trackVisibility; }
    observe(target) {
      const slot = ioSlots.get(this);
      if (!slot) throw new TypeError('Illegal invocation');
      if (arguments.length === 0) throw new TypeError("Failed to execute 'observe' on 'IntersectionObserver': 1 argument required, but only 0 present.");
      if (!isElement(target)) throw new TypeError("Failed to execute 'observe' on 'IntersectionObserver': parameter 1 is not of type 'Element'.");
      if (slot.observations.some((o) => o.target === target)) return;
      slot.observations.push({ target, index: -1, intersecting: false });
      intersectionObservers.add(this);
      updateWanted();
      startFrame();
    }
    unobserve(target) {
      const slot = ioSlots.get(this);
      if (!slot) throw new TypeError('Illegal invocation');
      if (arguments.length === 0) throw new TypeError("Failed to execute 'unobserve' on 'IntersectionObserver': 1 argument required, but only 0 present.");
      if (!isElement(target)) throw new TypeError("Failed to execute 'unobserve' on 'IntersectionObserver': parameter 1 is not of type 'Element'.");
      const at = slot.observations.findIndex((o) => o.target === target);
      if (at >= 0) slot.observations.splice(at, 1);
      if (!slot.observations.length) { intersectionObservers.delete(this); updateWanted(); }
    }
    disconnect() {
      const slot = ioSlots.get(this);
      if (!slot) throw new TypeError('Illegal invocation');
      slot.observations.length = 0;
      intersectionObservers.delete(this);
      updateWanted();
    }
    takeRecords() {
      const slot = ioSlots.get(this);
      if (!slot) throw new TypeError('Illegal invocation');
      return slot.queue.splice(0, slot.queue.length);
    }
  }
  Object.defineProperty(IntersectionObserver.prototype, Symbol.toStringTag, { value: 'IntersectionObserver', configurable: true });

  const overflowClips = (element) => {
    const cs = global.getComputedStyle(element);
    const clips = (v) => v !== 'visible';
    return clips(cs.overflowX) || clips(cs.overflowY) || /paint|strict|content/.test(cs.contain || '');
  };
  // The intersection of two rectangles ({left, top, right, bottom}), edges that touch counting; null if there is none.
  const intersect = (a, b) => {
    const left = Math.max(a.left, b.left), top = Math.max(a.top, b.top), right = Math.min(a.right, b.right), bottom = Math.min(a.bottom, b.bottom);
    return left <= right && top <= bottom ? { left, top, right, bottom } : null;
  };
  const edges = (r) => ({ left: r.left, top: r.top, right: r.right, bottom: r.bottom });
  const toRect = (e) => rect(e.left, e.top, e.right - e.left, e.bottom - e.top);

  const rootRectOf = (slot, target) => {
    let root = slot.root;
    if (root && typeof Document === 'function' && root instanceof Document) root = root.documentElement;
    let base;
    if (root === null) {
      const de = document.documentElement;
      base = { left: 0, top: 0, right: de.clientWidth, bottom: de.clientHeight };
    } else if (!overflowClips(root)) {
      // No clip: the border box.
      const r = root.getBoundingClientRect();
      base = { left: r.left, top: r.top, right: r.right, bottom: r.bottom };
    } else {
      const r = root.getBoundingClientRect();
      // The box that clips: the padding box.
      base = { left: r.left + root.clientLeft, top: r.top + root.clientTop, right: r.left + root.clientLeft + root.clientWidth, bottom: r.top + root.clientTop + root.clientHeight };
    }
    const [top, right, bottom, left] = slot.margins;
    const w = base.right - base.left, h = base.bottom - base.top;
    return {
      left: base.left - (left.percent ? w * left.value / 100 : left.value),
      top: base.top - (top.percent ? h * top.value / 100 : top.value),
      right: base.right + (right.percent ? w * right.value / 100 : right.value),
      bottom: base.bottom + (bottom.percent ? h * bottom.value / 100 : bottom.value),
    };
  };

  const compute = (slot, target) => {
    const rootEdges = rootRectOf(slot, target);
    const targetRect = target.getBoundingClientRect();
    const box = edges(targetRect);
    let root = slot.root;
    if (root && typeof Document === 'function' && root instanceof Document) root = root.documentElement;
    let inside = target.isConnected && !!(target.getClientRects().length || targetRect.width || targetRect.height);
    let visible = inside ? box : null;
    if (target.isConnected && root && !root.contains(target)) visible = null;
    if (target.isConnected && !inside) visible = null;
    // The boxes between the target and the root that clip what is in them.
    if (visible) {
      for (let a = target.parentNode; a && a !== root; a = a.parentNode || a.host) {
        if (!isElement(a) || a === document.documentElement) continue;
        if (overflowClips(a)) {
          const r = a.getBoundingClientRect();
          const clip = { left: r.left + a.clientLeft, top: r.top + a.clientTop, right: r.left + a.clientLeft + a.clientWidth, bottom: r.top + a.clientTop + a.clientHeight };
          // The scroll margin grows the box that clips, in the scrollers on the way.
          if (slot.scrollMargins.some((m) => m.value !== 0)) {
            const [mt, mr, mb, ml] = slot.scrollMargins;
            const w = clip.right - clip.left, h = clip.bottom - clip.top;
            clip.left -= ml.percent ? w * ml.value / 100 : ml.value;
            clip.top -= mt.percent ? h * mt.value / 100 : mt.value;
            clip.right += mr.percent ? w * mr.value / 100 : mr.value;
            clip.bottom += mb.percent ? h * mb.value / 100 : mb.value;
          }
          visible = intersect(visible, clip);
          if (!visible) break;
        }
      }
    }
    if (visible) visible = intersect(visible, rootEdges);
    const targetArea = (box.right - box.left) * (box.bottom - box.top);
    let ratio = 0;
    if (visible) {
      const area = (visible.right - visible.left) * (visible.bottom - visible.top);
      ratio = targetArea === 0 ? 1 : area / targetArea;
    }
    // With a threshold above 0 the target is not intersecting until it is as much in view.
    const isIntersecting = !!visible && ratio >= slot.thresholds[0];
    return { rootEdges, box, visible, ratio, isIntersecting };
  };

  const updateIntersections = (observer, time) => {
    const slot = ioSlots.get(observer);
    for (const observation of slot.observations) {
      let c;
      if (!observation.target.isConnected) {
        // Never seen in the document: nothing to tell. Seen, and gone: everything is empty.
        if (observation.index === -1) continue;
        const zero = { left: 0, top: 0, right: 0, bottom: 0 };
        c = { rootEdges: zero, box: zero, visible: null, ratio: 0, isIntersecting: false };
      } else {
        c = compute(slot, observation.target);
      }
      // The index of the first threshold above the ratio; 0 when the target is not intersecting.
      let index = 0;
      if (c.isIntersecting) {
        index = slot.thresholds.findIndex((t) => t > c.ratio);
        if (index < 0) index = slot.thresholds.length;
      }
      if (index === observation.index && c.isIntersecting === observation.intersecting) continue;
      observation.index = index;
      observation.intersecting = c.isIntersecting;
      const entry = new IntersectionObserverEntry({
        time,
        rootBounds: toRect(c.rootEdges),
        boundingClientRect: toRect(c.box),
        intersectionRect: c.visible ? toRect(c.visible) : rect(0, 0, 0, 0),
        isIntersecting: c.isIntersecting,
        isVisible: false,
        intersectionRatio: c.ratio,
        target: observation.target,
      });
      slot.queue.push(entry);
    }
    if (slot.queue.length && !slot.scheduled) {
      slot.scheduled = true;
      setTimeout(() => {
        slot.scheduled = false;
        const entries = slot.queue.splice(0, slot.queue.length);
        if (!entries.length) return;
        try {
          slot.callback.call(observer, entries, observer);
        } catch (error) {
          reportLater(error);
        }
      }, 0);
    }
  };

  R.addEndStep((now) => {
    if (resizeObservers.size) broadcastResize();
    for (const observer of [...intersectionObservers]) updateIntersections(observer, now);
  });

  define('ResizeObserver', ResizeObserver);
  define('ResizeObserverEntry', ResizeObserverEntry);
  define('ResizeObserverSize', ResizeObserverSize);
  define('IntersectionObserver', IntersectionObserver);
  define('IntersectionObserverEntry', IntersectionObserverEntry);
})();
