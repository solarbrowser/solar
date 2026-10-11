// ContentVisibilityAutoStateChangeEvent (https://drafts.csswg.org/css-contain-2/#content-visibility-auto-state-change): sent in a frame to the content-visibility: auto
// elements whose contents were skipped, or stopped being.
(function () {
  'use strict';
  const global = globalThis;
  const take = global.__solarLayoutCvTake;
  const setWake = global.__solarLayoutSetCvWake;
  delete global.__solarLayoutCvTake;
  delete global.__solarLayoutSetCvWake;
  const skippedOf = new WeakMap();

  class ContentVisibilityAutoStateChangeEvent extends Event {
    constructor(type, init = {}) {
      if (arguments.length === 0) throw new TypeError("Failed to construct 'ContentVisibilityAutoStateChangeEvent': 1 argument required, but only 0 present.");
      super(type, init);
      const dict = init === null || init === undefined ? {} : init;
      skippedOf.set(this, !!dict.skipped);
    }
    get skipped() {
      return skippedOf.get(this);
    }
  }
  Object.defineProperty(ContentVisibilityAutoStateChangeEvent.prototype, Symbol.toStringTag, { value: 'ContentVisibilityAutoStateChangeEvent', configurable: true });
  Object.defineProperty(global, 'ContentVisibilityAutoStateChangeEvent', { value: ContentVisibilityAutoStateChangeEvent, writable: true, configurable: true });

  let scheduled = false;
  const send = () => {
    scheduled = false;
    const changes = take();
    for (let i = 0; i < changes.length; i += 2) {
      const element = changes[i];
      try {
        element.dispatchEvent(new ContentVisibilityAutoStateChangeEvent('contentvisibilityautostatechange', { bubbles: true, skipped: changes[i + 1] }));
      } catch (error) {
        setTimeout(() => { throw error; }, 0);
      }
    }
  };
  // Every frame updates the rendering first, which is where the elements that came to be skipped or not are found.
  const addStep = global.__solarAddFrameStep;
  delete global.__solarAddFrameStep;
  // The end of the rendering update, for the observers (layout-5-observers.js).
  global.__solarRendering = { addEndStep: global.__solarAddFrameEndStep, want: global.__solarLayoutWantUpdates, rendered: send };
  delete global.__solarAddFrameEndStep;
  delete global.__solarLayoutWantUpdates;
  if (typeof addStep === 'function') addStep(() => send());
  setWake(() => {
    if (scheduled || typeof global.requestAnimationFrame !== 'function') return;
    scheduled = true;
    global.requestAnimationFrame(send);
  });
})();

// Focusing an element scrolls it into view, unless it is asked not to (https://html.spec.whatwg.org/multipage/interaction.html#focusing-steps).
(function () {
  'use strict';
  for (const proto of [globalThis.HTMLElement && HTMLElement.prototype, globalThis.SVGElement && SVGElement.prototype]) {
    if (!proto) continue;
    const descriptor = Object.getOwnPropertyDescriptor(proto, 'focus');
    if (!descriptor || typeof descriptor.value !== 'function') continue;
    const original = descriptor.value;
    Object.defineProperty(proto, 'focus', {
      ...descriptor,
      value: {
        focus(options) {
          const before = this.ownerDocument.activeElement;
          const result = original.apply(this, arguments);
          if (!(options && options.preventScroll) && this.ownerDocument.activeElement === this && this.isConnected && typeof this.scrollIntoView === 'function') {
            this.scrollIntoView({ block: 'nearest', inline: 'nearest' });
          }
          return result;
        },
      }.focus,
    });
  }
})();
