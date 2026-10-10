// CSS Transitions: CSSTransition, TransitionEvent, and the transitions that a change of style starts, retargets and cancels.
(function () {
  'use strict';
  const S = globalThis.__solarAnim;
  const N = S.natives;
  const slots = S.animationSlots;

  class TransitionEvent extends Event {
    constructor(type, init = {}) {
      super(type, init);
      const dict = init === null || init === undefined ? {} : init;
      slots.set(this, {
        propertyName: dict.propertyName === undefined ? '' : String(dict.propertyName),
        elapsedTime: dict.elapsedTime === undefined ? 0 : Number(dict.elapsedTime),
        pseudoElement: dict.pseudoElement === undefined ? '' : String(dict.pseudoElement),
      });
    }
    get propertyName() {
      return slots.get(this).propertyName;
    }
    get elapsedTime() {
      return slots.get(this).elapsedTime;
    }
    get pseudoElement() {
      return slots.get(this).pseudoElement;
    }
  }
  Object.defineProperty(TransitionEvent.prototype, Symbol.toStringTag, { value: 'TransitionEvent', configurable: true });
  globalThis.TransitionEvent = TransitionEvent;

  class CSSTransition extends Animation {
    constructor(effect, timeline) {
      if (!S.constructAllowed()) throw new TypeError('Illegal constructor');
      super(effect, timeline);
    }
    get transitionProperty() {
      return S.slotOf(this).transitionProperty;
    }
  }
  Object.defineProperty(CSSTransition.prototype, Symbol.toStringTag, { value: 'CSSTransition', configurable: true });
  globalThis.CSSTransition = CSSTransition;

  // ---- Reading style ----

  const readList = (el, pseudo, property) => {
    const text = N.read(el, pseudo, property);
    if (text === null || text === '') return [];
    return S.splitTop(text);
  };
  const at = (list, i, fallback) => (list.length ? list[i % list.length] : fallback);

  let allNames = null;
  const everyProperty = () => {
    if (allNames) return allNames;
    allNames = [...getComputedStyle(document.documentElement)].filter((name) => {
      if (name.startsWith('transition') || name.startsWith('animation') || name.startsWith('--')) return false;
      // A logical property is another name for a physical one, which is what changes.
      if (/(^|-)(inline|block)(-|$)/.test(name) || /^border-(start|end)-/.test(name)) return false;
      return N.kind(name) === 1;
    });
    return allNames;
  };

  const expand = (name) => {
    const lower = name.toLowerCase();
    if (lower === 'all') return everyProperty();
    if (lower.startsWith('--')) return [];
    const kind = N.kind(lower);
    if (kind === 1) return [lower];
    if (kind === 2) {
      const pairs = S.declare(lower, 'inherit');
      return pairs ? pairs.map(([n]) => n) : [];
    }
    return [];
  };

  // The transitions the style of the element asks for: property → { duration, delay, timing, behavior, index }.
  const readTransitions = (el, pseudo) => {
    const result = new Map();
    const durations = readList(el, pseudo, 'transition-duration');
    const delays = readList(el, pseudo, 'transition-delay');
    // A transition needs a time to run for, which most elements have none of.
    const count = Math.max(durations.length, delays.length);
    let timed = false;
    for (let i = 0; i < count; i++) {
      if (S.parseTime(at(durations, i, '0s')) + S.parseTime(at(delays, i, '0s')) > 0) timed = true;
    }
    if (!timed) return result;
    const properties = readList(el, pseudo, 'transition-property');
    if (!properties.length || (properties.length === 1 && properties[0].toLowerCase() === 'none')) return result;
    const timings = readList(el, pseudo, 'transition-timing-function');
    const behaviors = readList(el, pseudo, 'transition-behavior');
    properties.forEach((name, index) => {
      const params = {
        duration: S.parseTime(at(durations, index, '0s')),
        delay: S.parseTime(at(delays, index, '0s')),
        timing: at(timings, index, 'ease'),
        behavior: at(behaviors, index, 'normal'),
        index,
      };
      for (const property of expand(name)) result.set(property, params);
    });
    return result;
  };

  // ---- Starting and ending ----

  const running = new Map();  // element → pseudo → property → transition
  const seen = new Map();     // element → pseudo → property → the value at the last change of style

  const mapIn = (outer, el, pseudo) => {
    let byPseudo = outer.get(el);
    if (!byPseudo) outer.set(el, (byPseudo = new Map()));
    let inner = byPseudo.get(pseudo);
    if (!inner) byPseudo.set(pseudo, (inner = new Map()));
    return inner;
  };

  const canTransition = (property, before, after, behavior) => {
    if (property === 'visibility') return before === 'visible' || after === 'visible';
    if (N.interpolate(property, before, after, 0.5) !== null) return true;
    return behavior === 'allow-discrete';
  };

  const isRunning = (animation) => {
    const state = S.playStateOf(S.slotOf(animation));
    return state !== 'finished' && state !== 'idle';
  };

  const cancelTransition = (animation) => {
    S.cssCancelEvent(animation);
    S.cssAnimations.delete(animation);
    S.slotOf(animation).cssEvents = false;
    animation.cancel();
  };

  let order = 0;
  const startTransition = (el, pseudo, property, start, end, params, old) => {
    let { duration, delay } = params;
    let factor = 1;
    let adjustedStart = start;
    if (old) {
      const oldSlot = S.slotOf(old);
      const state = oldSlot.transition;
      if (state.reversingStart === end && duration + delay > 0) {
        const effectSlot = S.effectSlots.get(oldSlot.effect);
        const progress = S.effectComputed(effectSlot).progress;
        factor = Math.min(Math.max(Math.abs((progress === null ? 0 : progress) * state.factor + (1 - state.factor)), 0), 1);
        duration *= factor;
        if (delay < 0) delay *= factor;
        adjustedStart = state.endValue;
      }
    }
    const easing = S.parseEasing(params.timing) || S.parseEasing('ease');
    const effect = new KeyframeEffect(el, null, 0);
    const effectSlot = S.effectSlots.get(effect);
    const linear = S.parseEasing('linear');
    effectSlot.pseudo = pseudo ? '::' + pseudo : null;
    effectSlot.timing = { delay, endDelay: 0, fill: 'backwards', iterationStart: 0, iterations: 1, duration, direction: 'normal', easing: easing.text, easingFn: easing };
    effectSlot.keyframes = [
      { offset: 0, computedOffset: 0, easing: linear, composite: 'auto', values: new Map([[property, start]]) },
      { offset: 1, computedOffset: 1, easing: linear, composite: 'auto', values: new Map([[property, end]]) },
    ];
    const animation = S.construct(CSSTransition, effect, S.documentTimeline);
    const aslot = S.slotOf(animation);
    aslot.category = 0;
    aslot.cssKind = 'transition';
    aslot.transitionProperty = property;
    aslot.owner = el;
    aslot.order = params.index;
    aslot.transition = { startValue: start, endValue: end, reversingStart: adjustedStart, factor };
    aslot.css = { pseudo, phase: 'idle', iteration: null };
    aslot.cssEvents = true;
    S.cssAnimations.add(animation);
    animation.play();
    return animation;
  };

  const pseudoCandidates = ['', 'before', 'after', 'marker'];

  S.transitionsUpdate = (el, pseudo) => {
    const params = el.isConnected ? readTransitions(el, pseudo) : new Map();
    if (!params.size && !(running.has(el) && running.get(el).has(pseudo))) return;
    const runMap = mapIn(running, el, pseudo);
    const seenMap = mapIn(seen, el, pseudo);
    // What has ended is no longer running.
    for (const [property, animation] of [...runMap]) {
      if (!isRunning(animation)) runMap.delete(property);
    }
    // What the style does not ask for any longer is canceled.
    for (const [property, animation] of [...runMap]) {
      const p = params.get(property);
      if (!p || p.duration + p.delay <= 0) {
        cancelTransition(animation);
        runMap.delete(property);
      }
    }
    for (const [property, p] of params) {
      const after = S.afterChangeValue(el, pseudo, property);
      if (after === null || after === '') continue;
      // What an animation has hold of does not change with what is declared, so there is nothing to transition from.
      if (after !== N.base(el, pseudo, property)) {
        seenMap.set(property, after);
        continue;
      }
      const run = runMap.get(property);
      let before;
      if (run) before = S.appliedValue(el, pseudo, property);
      if (before === undefined || before === null) before = seenMap.has(property) ? seenMap.get(property) : N.lastRead(el, pseudo, property);
      seenMap.set(property, after);
      if (before === null || before === undefined) continue;
      if (run) {
        if (S.slotOf(run).transition.endValue === after) continue;
      } else if (before === after) {
        continue;
      }
      if (p.duration + p.delay <= 0 || !canTransition(property, before, after, p.behavior)) {
        if (run) {
          cancelTransition(run);
          runMap.delete(property);
        }
        continue;
      }
      if (run) {
        cancelTransition(run);
        runMap.delete(property);
      }
      runMap.set(property, startTransition(el, pseudo, property, before, after, p, run));
    }
    // A property no longer asked for has nothing to start from next time.
    for (const property of [...seenMap.keys()]) if (!params.has(property)) seenMap.delete(property);
  };

  S.transitionElements = () => new Set([...running.keys(), ...seen.keys()]);

  // Elements that have gone lose their transitions.
  S.transitionsCleanup = (liveElements) => {
    for (const [el, byPseudo] of [...running]) {
      if (liveElements.has(el) && el.isConnected) continue;
      for (const runMap of byPseudo.values()) for (const animation of runMap.values()) if (isRunning(animation)) cancelTransition(animation);
      running.delete(el);
      seen.delete(el);
    }
  };
})();
