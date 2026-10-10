// Web Animations, effects: AnimationEffect and KeyframeEffect, the keyframes of an effect, the value an effect has for a property at a time
// and the stack of effects on an element that gives the values to the cascade, with Element.animate() and getAnimations().
(function () {
  'use strict';
  const S = globalThis.__solarAnim;
  const effectSlots = new WeakMap();
  S.effectSlots = effectSlots;

  const natives = {
    interpolate: globalThis.__solarAnimInterpolate,
    add: globalThis.__solarAnimAdd,
    scale: globalThis.__solarAnimScale,
    set: globalThis.__solarAnimSet,
    base: globalThis.__solarAnimComputed,
    compute: globalThis.__solarAnimCompute,
    expand: globalThis.__solarAnimExpand,
    kind: globalThis.__solarAnimPropertyKind,
    version: globalThis.__solarAuthorVersion,
    onFlush: globalThis.__solarAnimOnFlush,
  };
  for (const name of ['__solarAnimInterpolate', '__solarAnimAdd', '__solarAnimScale', '__solarAnimSet', '__solarAnimComputed', '__solarAnimCompute', '__solarAnimExpand', '__solarAnimPropertyKind', '__solarAuthorVersion', '__solarAnimOnFlush', '__solarAnimInterpolable']) delete globalThis[name];

  // ---- Property names ----

  const cssNameOf = (name) => {
    if (name === 'cssFloat') return 'float';
    if (name === 'cssOffset') return 'offset';
    if (!/^[a-z]+(?:[A-Z][a-z0-9]*)*$/.test(name)) return null;
    let css = name.replace(/[A-Z]/g, (m) => '-' + m.toLowerCase());
    if (css.startsWith('webkit-')) css = '-' + css;
    return natives.kind(css) ? css : null;
  };
  const scriptNameOf = (css) => {
    if (css === 'float') return 'cssFloat';
    if (css === 'offset') return 'cssOffset';
    return css.replace(/^-webkit-/, 'webkit-').replace(/-([a-z])/g, (m, c) => c.toUpperCase());
  };
  S.cssNameOf = cssNameOf;
  S.scriptNameOf = scriptNameOf;

  const scratch = document.createElement('div').style;
  // The longhands a declaration of the property comes to, as [name, value] pairs, or null if the value is not one.
  const declare = (property, value) => {
    scratch.cssText = '';
    scratch.setProperty(property, value);
    if (scratch.length === 0) return null;
    const pairs = [];
    for (let i = 0; i < scratch.length; i++) {
      const name = scratch.item(i);
      pairs.push([name, scratch.getPropertyValue(name)]);
    }
    scratch.cssText = '';
    return pairs;
  };
  S.declare = declare;

  // ---- Keyframes ----

  const COMPOSITES = ['replace', 'add', 'accumulate'];
  // An element of any realm: a window's own Element is not the one an iframe's elements are of.
  const isElement = (v) => typeof v === 'object' && v !== null && v.nodeType === 1 && typeof v.localName === 'string';
  const isObject = (v) => (typeof v === 'object' && v !== null) || typeof v === 'function';

  const toEasing = (value) => {
    const easing = S.parseEasing(String(value));
    if (!easing) throw new TypeError(`'${value}' is not a valid value for easing`);
    return easing;
  };
  const toComposite = (value, allowAuto) => {
    value = String(value);
    if (!(COMPOSITES.includes(value) || (allowAuto && value === 'auto'))) throw new TypeError(`'${value}' is not a valid enum value of type CompositeOperation${allowAuto ? 'OrAuto' : ''}`);
    return value;
  };
  const toOffset = (value) => {
    if (value === null || value === undefined) return null;
    const n = Number(value);
    if (!Number.isFinite(n)) throw new TypeError('Keyframe offset must be a finite number or null.');
    return n;
  };

  // Keeps the properties of a keyframe-like object that name animatable properties: [script name, css name, raw value]s, shorthands first.
  const propertiesOf = (object) => {
    const names = [];
    for (const key of Reflect.ownKeys(object)) {
      if (typeof key !== 'string') continue;
      if (key === 'offset' || key === 'easing' || key === 'composite' || key === 'computedOffset') continue;
      const css = cssNameOf(key);
      if (!css) continue;
      const descriptor = Reflect.getOwnPropertyDescriptor(object, key);
      if (!descriptor || !descriptor.enumerable) continue;
      names.push([key, css]);
    }
    names.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));
    const shorthands = names.filter(([, css]) => natives.kind(css) === 2);
    const longhands = names.filter(([, css]) => natives.kind(css) !== 2);
    return [...shorthands, ...longhands].map(([key, css]) => [key, css, object[key]]);
  };

  const isIterable = (value) => value !== null && value !== undefined && typeof value[Symbol.iterator] === 'function';

  const newKeyframe = () => ({ offset: null, computedOffset: null, easing: S.parseEasing('linear'), composite: 'auto', values: new Map() });

  // The longhand values a (property, value) pair declares in the keyframe; an invalid value is left out.
  const addValue = (keyframe, css, raw) => {
    const pairs = declare(css, String(raw));
    if (!pairs) return;
    for (const [name, value] of pairs) keyframe.values.set(name, value);
  };

  const processKeyframes = (input) => {
    if (input === null || input === undefined) return [];
    if (!isObject(input)) throw new TypeError('Keyframes must be an object.');
    let keyframes = [];
    if (isIterable(input)) {
      for (const item of input) {
        if (!isObject(item) && item !== null && item !== undefined) throw new TypeError('A keyframe must be an object.');
        const keyframe = newKeyframe();
        if (item) {
          const composite = item.composite;
          if (composite !== undefined) keyframe.composite = toComposite(composite, true);
          const easing = item.easing;
          if (easing !== undefined) keyframe.easing = toEasing(easing);
          const offset = item.offset;
          if (offset !== undefined) keyframe.offset = toOffset(offset);
          for (const [, css, raw] of propertiesOf(item)) {
            if (Array.isArray(raw) || (isObject(raw) && isIterable(raw) && typeof raw !== 'string')) throw new TypeError('Keyframe values must be strings.');
            addValue(keyframe, css, raw);
          }
        }
        keyframes.push(keyframe);
      }
    } else {
      const composite = input.composite;
      const easing = input.easing;
      const offset = input.offset;
      const toList = (v) => (v === undefined ? [] : isObject(v) && isIterable(v) && typeof v !== 'string' ? [...v] : [v]);
      const composites = toList(composite).map((c) => toComposite(c, true));
      const easings = toList(easing).map(toEasing);
      const offsets = toList(offset).map(toOffset);
      const columns = [];
      let count = 0;
      for (const [, css, raw] of propertiesOf(input)) {
        const values = isObject(raw) && isIterable(raw) && typeof raw !== 'string' ? [...raw].map(String) : [String(raw)];
        columns.push([css, values]);
        count = Math.max(count, values.length);
      }
      for (let i = 0; i < count; i++) {
        const keyframe = newKeyframe();
        for (const [css, values] of columns) if (i < values.length) addValue(keyframe, css, values[i]);
        keyframes.push(keyframe);
      }
      keyframes.forEach((keyframe, i) => {
        if (i < offsets.length) keyframe.offset = offsets[i];
        if (easings.length) keyframe.easing = easings[i % easings.length];
        if (composites.length) keyframe.composite = composites[i % composites.length];
      });
      // A column shorter than the others has no value for the keyframes past its end; offsets given for keyframes that do not exist are dropped.
    }
    // The offsets that were given must be within [0, 1] and not go back.
    let last = -Infinity;
    for (const keyframe of keyframes) {
      if (keyframe.offset === null) continue;
      if (keyframe.offset < 0 || keyframe.offset > 1 || keyframe.offset < last) throw new TypeError('Keyframe offsets must be in order and between 0 and 1.');
      last = keyframe.offset;
    }
    computeOffsets(keyframes);
    return keyframes;
  };

  const computeOffsets = (keyframes) => {
    keyframes.forEach((keyframe) => { keyframe.computedOffset = keyframe.offset; });
    if (keyframes.length === 0) return;
    if (keyframes.length > 1 && keyframes[0].computedOffset === null) keyframes[0].computedOffset = 0;
    if (keyframes[keyframes.length - 1].computedOffset === null) keyframes[keyframes.length - 1].computedOffset = 1;
    let a = 0;
    for (let b = 1; b < keyframes.length; b++) {
      if (keyframes[b].computedOffset === null) continue;
      const n = b - a;
      for (let j = a + 1; j < b; j++) keyframes[j].computedOffset = keyframes[a].computedOffset + ((keyframes[b].computedOffset - keyframes[a].computedOffset) * (j - a)) / n;
      a = b;
    }
  };

  // ---- Effects ----

  const PSEUDO = /^::?(before|after|first-line|first-letter)$|^::(marker|backdrop|placeholder|selection|file-selector-button|target-text|spelling-error|grammar-error|cue|details-content|checkmark|picker-icon|column|scroll-marker|scroll-marker-group|highlight\(.+\)|part\(.+\)|view-transition(?:-(?:group|image-pair|old|new)\(.+\))?|cue\(.+\))$/;
  const normalizePseudo = (value) => {
    if (value === null || value === undefined) return null;
    const text = String(value);
    if (!PSEUDO.test(text)) throw S.domException(`'${text}' is not a valid pseudo-element.`, 'SyntaxError');
    return text.startsWith('::') ? text : ':' + text;
  };
  const nativePseudo = (pseudo) => (pseudo ? pseudo.replace(/^::/, '') : '');

  const effectOf = (value) => {
    const slot = effectSlots.get(value);
    if (!slot || slot.effect !== value) throw new TypeError('Illegal invocation');
    return slot;
  };

  class AnimationEffect {
    constructor() {
      if (new.target === AnimationEffect) throw new TypeError('Illegal constructor');
    }
    getTiming() {
      const t = effectOf(this).timing;
      return { delay: t.delay, endDelay: t.endDelay, fill: t.fill, iterationStart: t.iterationStart, iterations: t.iterations, duration: t.duration, direction: t.direction, easing: t.easing };
    }
    getComputedTiming() {
      const slot = effectOf(this);
      const t = slot.timing;
      const c = S.effectComputed(slot);
      return {
        delay: t.delay,
        endDelay: t.endDelay,
        fill: t.fill === 'auto' ? 'none' : t.fill,
        iterationStart: t.iterationStart,
        iterations: t.iterations,
        duration: t.duration === 'auto' ? 0 : t.duration,
        direction: t.direction,
        easing: t.easing,
        endTime: c.endTime,
        activeDuration: c.activeDuration,
        localTime: c.localTime === undefined ? null : c.localTime,
        progress: c.progress,
        currentIteration: c.currentIteration,
        startTime: 0,
      };
    }
    updateTiming(timing) {
      const slot = effectOf(this);
      slot.timing = S.readTiming(timing === undefined ? {} : timing, slot.timing);
      if (slot.animation) S.updateFinishedState(S.slotOf(slot.animation), false, false);
      else refreshEffect(slot);
    }
  }
  Object.defineProperty(AnimationEffect.prototype, Symbol.toStringTag, { value: 'AnimationEffect', configurable: true });

  class KeyframeEffect extends AnimationEffect {
    constructor(target, keyframes, options) {
      super();
      const slot = { effect: this, animation: null, target: null, pseudo: null, timing: S.defaultTiming(), keyframes: [], composite: 'replace', iterationComposite: 'replace', cache: null };
      if (arguments.length === 1 && effectSlots.has(target)) {
        const source = effectSlots.get(target);
        slot.target = source.target;
        slot.pseudo = source.pseudo;
        slot.timing = { ...source.timing };
        slot.composite = source.composite;
        slot.iterationComposite = source.iterationComposite;
        slot.keyframes = source.keyframes.map((k) => ({ ...k, values: new Map(k.values) }));
        effectSlots.set(this, slot);
        return;
      }
      if (target !== null && !isElement(target)) throw new TypeError("Failed to construct 'KeyframeEffect': parameter 1 is not of type 'Element'.");
      let timing = slot.timing;
      if (typeof options === 'number') {
        timing = S.readTiming({ duration: options }, timing);
      } else if (options !== undefined && options !== null) {
        if (!isObject(options)) throw new TypeError('The options is not an object.');
        timing = S.readTiming(options, timing);
        const composite = options.composite;
        if (composite !== undefined) slot.composite = toComposite(composite, false);
        const iterationComposite = options.iterationComposite;
        if (iterationComposite !== undefined) {
          if (iterationComposite !== 'replace' && iterationComposite !== 'accumulate') throw new TypeError(`'${iterationComposite}' is not a valid enum value of type IterationCompositeOperation.`);
          slot.iterationComposite = iterationComposite;
        }
        const pseudo = options.pseudoElement;
        if (pseudo !== undefined) slot.pseudo = normalizePseudo(pseudo);
      }
      slot.timing = timing;
      slot.keyframes = processKeyframes(keyframes);
      slot.target = target;
      effectSlots.set(this, slot);
    }
    get target() {
      return effectOf(this).target;
    }
    set target(value) {
      const slot = effectOf(this);
      if (value !== null && !isElement(value)) throw new TypeError("Failed to set the 'target' property on 'KeyframeEffect': The provided value is not of type 'Element'.");
      const old = { target: slot.target, pseudo: slot.pseudo };
      unregister(slot);
      slot.target = value;
      register(slot);
      refreshPair(old.target, old.pseudo);
      refreshEffect(slot);
    }
    get pseudoElement() {
      return effectOf(this).pseudo;
    }
    set pseudoElement(value) {
      const slot = effectOf(this);
      const pseudo = normalizePseudo(value);
      const old = slot.pseudo;
      unregister(slot);
      slot.pseudo = pseudo;
      register(slot);
      refreshPair(slot.target, old);
      refreshEffect(slot);
    }
    get composite() {
      return effectOf(this).composite;
    }
    set composite(value) {
      const slot = effectOf(this);
      slot.composite = toComposite(value, false);
      refreshEffect(slot);
    }
    get iterationComposite() {
      return effectOf(this).iterationComposite;
    }
    set iterationComposite(value) {
      const slot = effectOf(this);
      value = String(value);
      if (value !== 'replace' && value !== 'accumulate') throw new TypeError(`'${value}' is not a valid enum value of type IterationCompositeOperation.`);
      slot.iterationComposite = value;
      refreshEffect(slot);
    }
    getKeyframes() {
      const slot = effectOf(this);
      return slot.keyframes.map((keyframe) => {
        const out = { offset: keyframe.offset, computedOffset: keyframe.computedOffset, easing: keyframe.easing.text, composite: keyframe.composite };
        const names = [...keyframe.values.keys()].map((css) => [scriptNameOf(css), css]).sort((a, b) => (a[0] < b[0] ? -1 : 1));
        for (const [name, css] of names) out[name] = keyframe.values.get(css);
        return out;
      });
    }
    setKeyframes(keyframes) {
      const slot = effectOf(this);
      slot.keyframes = processKeyframes(keyframes);
      slot.cache = null;
      refreshEffect(slot);
    }
  }
  Object.defineProperty(KeyframeEffect.prototype, Symbol.toStringTag, { value: 'KeyframeEffect', configurable: true });
  globalThis.AnimationEffect = AnimationEffect;
  globalThis.KeyframeEffect = KeyframeEffect;

  // ---- What an effect comes to ----

  S.effectComputed = (slot) => {
    const animation = slot.animation;
    if (!animation) return S.computeTiming(slot.timing, null, 1);
    const aslot = S.slotOf(animation);
    return S.computeTiming(slot.timing, S.currentTimeOf(aslot), aslot.playbackRate);
  };

  const isRelevant = (aslot) => {
    const slot = aslot.effect ? effectSlots.get(aslot.effect) : null;
    if (!slot) return false;
    const c = S.effectComputed(slot);
    if (c.phase === 'idle') return false;
    const inEffect = c.activeTime !== null;
    const inPlay = c.phase === 'active' && S.playStateOf(aslot) !== 'finished';
    const current = inPlay || (aslot.playbackRate > 0 && c.phase === 'before') || (aslot.playbackRate < 0 && c.phase === 'after');
    return current || inEffect;
  };
  S.inEffect = (aslot) => {
    const slot = aslot.effect ? effectSlots.get(aslot.effect) : null;
    return !!slot && S.effectComputed(slot).activeTime !== null;
  };
  S.isRelevant = isRelevant;

  // The computed value of each keyframe of the effect for the target, made again when what is declared changes.
  const computedKeyframes = (slot) => {
    const version = natives.version();
    if (slot.cache && slot.cache.version === version && slot.cache.target === slot.target && slot.cache.pseudo === slot.pseudo) return slot.cache.byProperty;
    const byProperty = new Map();
    for (const keyframe of slot.keyframes) {
      for (const [css, text] of keyframe.values) {
        const value = natives.compute(slot.target, nativePseudo(slot.pseudo), css, text);
        if (value === null || value === '') continue;
        if (!byProperty.has(css)) byProperty.set(css, []);
        byProperty.get(css).push({ offset: keyframe.computedOffset, value, easing: keyframe.easing, composite: keyframe.composite });
      }
    }
    for (const list of byProperty.values()) list.sort((a, b) => a.offset - b.offset);
    slot.cache = { version, target: slot.target, pseudo: slot.pseudo, byProperty };
    return byProperty;
  };

  const interpolateValues = (property, a, b, t) => {
    if (a === b) return a;
    if (property === 'visibility') {
      if (t <= 0) return a;
      if (t >= 1) return b;
      return a === 'visible' || b === 'visible' ? 'visible' : t < 0.5 ? a : b;
    }
    const mixed = natives.interpolate(property, a, b, t);
    if (mixed !== null) return mixed;
    return t < 0.5 ? a : b;
  };

  // The value of the property the effect has when its progress is `progress`, over the underlying value.
  const valueAt = (slot, property, list, progress, underlying) => {
    const keyframes = list.slice();
    const neutral = (offset) => ({ offset, value: underlying, easing: S.parseEasing('linear'), composite: 'replace', neutral: true });
    if (keyframes[0].offset !== 0) keyframes.unshift(neutral(0));
    if (keyframes[keyframes.length - 1].offset !== 1) keyframes.push(neutral(1));
    if (keyframes.length < 2) return undefined;
    let index;
    if (progress < 0) index = 0;
    else {
      index = 0;
      for (let i = 0; i < keyframes.length; i++) if (keyframes[i].offset <= progress) index = i;
    }
    if (index > keyframes.length - 2) index = keyframes.length - 2;
    const a = keyframes[index], b = keyframes[index + 1];
    const resolve = (keyframe) => {
      if (keyframe.neutral) return underlying;
      const composite = keyframe.composite === 'auto' ? slot.composite : keyframe.composite;
      if (composite === 'replace') return keyframe.value;
      const added = natives.add(property, underlying, keyframe.value);
      return added === null ? keyframe.value : added;
    };
    const from = resolve(a), to = resolve(b);
    if (b.offset === a.offset) return progress < a.offset ? from : to;
    const distance = a.easing.fn((progress - a.offset) / (b.offset - a.offset), false);
    return interpolateValues(property, from, to, distance);
  };

  const effectValue = (slot, property, list, computed, underlying) => {
    let value = valueAt(slot, property, list, computed.progress, underlying);
    if (value === undefined) return undefined;
    if (slot.iterationComposite === 'accumulate' && computed.currentIteration > 0 && Number.isFinite(computed.currentIteration)) {
      const last = valueAt(slot, property, list, 1, underlying);
      const scaled = natives.scale(property, last, computed.currentIteration);
      if (scaled !== null) {
        const sum = natives.add(property, scaled, value);
        if (sum !== null) value = sum;
      }
    }
    return value;
  };

  // ---- The stack of effects on an element ----

  const registry = new Map();  // target → pseudo → effects
  const register = (slot) => {
    if (!slot.target || !slot.animation) return;
    const key = nativePseudo(slot.pseudo);
    let byPseudo = registry.get(slot.target);
    if (!byPseudo) registry.set(slot.target, (byPseudo = new Map()));
    let set = byPseudo.get(key);
    if (!set) byPseudo.set(key, (set = new Set()));
    set.add(slot.effect);
  };
  const unregister = (slot) => {
    if (!slot.target) return;
    const byPseudo = registry.get(slot.target);
    if (!byPseudo) return;
    const key = nativePseudo(slot.pseudo);
    const set = byPseudo.get(key);
    if (!set) return;
    set.delete(slot.effect);
    if (set.size === 0) byPseudo.delete(key);
    if (byPseudo.size === 0) registry.delete(slot.target);
  };
  S.registry = registry;

  const compositeOrder = (a, b) => {
    const as = S.slotOf(a), bs = S.slotOf(b);
    return as.category - bs.category || (as.order !== undefined && bs.order !== undefined && as.category === bs.category && as.owner === bs.owner ? as.order - bs.order : 0) || S.sequenceOf(as) - S.sequenceOf(bs);
  };
  S.compositeOrder = compositeOrder;

  const applied = new Map();  // target → pseudo → last record string, to leave out what has not changed

  const refreshPair = (target, pseudo) => {
    if (!target) return;
    const key = nativePseudo(pseudo);
    const byPseudo = registry.get(target);
    const set = byPseudo && byPseudo.get(key);
    const result = new Map();
    if (set && set.size) {
      const members = [];
      for (const effect of set) {
        const slot = effectSlots.get(effect);
        if (!slot.animation) continue;
        const computed = S.effectComputed(slot);
        if (computed.progress === null) continue;
        members.push({ slot, computed, animation: slot.animation });
      }
      members.sort((x, y) => compositeOrder(x.animation, y.animation));
      const bases = new Map();
      const base = (css) => {
        if (!bases.has(css)) bases.set(css, natives.base(target, key, css));
        return bases.get(css);
      };
      if (target.isConnected) {
        for (const { slot, computed } of members) {
          const lists = computedKeyframes(slot);
          for (const [css, list] of lists) {
            const underlying = result.has(css) ? result.get(css) : base(css);
            if (underlying === null || underlying === '') continue;
            const value = effectValue(slot, css, list, computed, underlying);
            if (value !== undefined) result.set(css, value);
          }
        }
      }
    }
    let records = '';
    for (const [css, value] of result) records += css + '\x1f' + value + '\x1e';
    let byApplied = applied.get(target);
    if (!byApplied) applied.set(target, (byApplied = new Map()));
    byApplied.set(key, result);
    natives.set(target, key, records);
    if (result.size === 0) {
      byApplied.delete(key);
      if (byApplied.size === 0) applied.delete(target);
    }
  };

  const refreshEffect = (slot) => {
    refreshPair(slot.target, slot.pseudo);
  };

  S.updated = (animation) => {
    const aslot = S.slotOf(animation);
    if (aslot.effect) {
      const slot = effectSlots.get(aslot.effect);
      register(slot);
      S.live.add(animation);
      refreshEffect(slot);
    }
  };

  S.retarget = (effect) => {
    const slot = effectSlots.get(effect);
    unregister(slot);
    refreshEffect(slot);
  };

  S.styleFlush = () => S.refreshAll();
  natives.onFlush(() => S.refreshAll());

  S.refreshAll = () => {
    for (const [target, byPseudo] of [...registry]) {
      for (const key of [...byPseudo.keys()]) refreshPair(target, key ? '::' + key : null);
    }
  };

  S.dropEffect = (aslot) => {
    if (!aslot.effect) return;
    const slot = effectSlots.get(aslot.effect);
    unregister(slot);
    refreshEffect(slot);
  };

  S.commitStyles = (aslot) => {
    const slot = aslot.effect ? effectSlots.get(aslot.effect) : null;
    if (!slot || !slot.target) return;
    if (slot.pseudo) throw S.domException('Cannot commit styles of a pseudo-element.', 'NoModificationAllowedError');
    const target = slot.target;
    if (!target.isConnected || getComputedStyle(target).display === 'none') throw S.domException('Cannot commit styles of an element that is not rendered.', 'InvalidStateError');
    if (!target.style) throw S.domException('The target is not styleable.', 'NoModificationAllowedError');
    const values = applied.get(target) && applied.get(target).get('');
    const css = new Set(slot.keyframes.flatMap((k) => [...k.values.keys()]));
    for (const property of css) {
      if (values && values.has(property)) target.style.setProperty(property, values.get(property));
    }
  };

  // ---- Element.animate(), getAnimations() ----

  const relevantAnimations = (filter, keyFilter) => {
    const result = [];
    const seen = new Set();
    for (const [target, byPseudo] of registry) {
      if (!filter(target)) continue;
      for (const [key, set] of byPseudo) {
        if (keyFilter && !keyFilter(key, target)) continue;
        for (const effect of set) {
          const animation = effectSlots.get(effect).animation;
          if (!animation || seen.has(animation)) continue;
          if (!isRelevant(S.slotOf(animation))) continue;
          seen.add(animation);
          result.push(animation);
        }
      }
    }
    return result.sort(compositeOrder);
  };

  const contains = (ancestor, node) => ancestor === node || ancestor.contains(node);

  Object.defineProperty(Element.prototype, 'getAnimations', {
    writable: true, enumerable: true, configurable: true,
    value: function getAnimations(options) {
      if (S.styleFlush) S.styleFlush();
      const subtree = !!(options && options.subtree);
      let pseudo;
      if (options && options.pseudoElement !== undefined && options.pseudoElement !== null) pseudo = nativePseudo(normalizePseudo(options.pseudoElement));
      const keyFilter = pseudo !== undefined ? (key) => key === pseudo : subtree ? null : (key) => key === '';
      return relevantAnimations((target) => (subtree ? contains(this, target) : target === this), keyFilter);
    },
  });
  Object.defineProperty(Document.prototype, 'getAnimations', {
    writable: true, enumerable: true, configurable: true,
    value: function getAnimations() {
      if (S.styleFlush) S.styleFlush();
      return relevantAnimations((target) => target.isConnected && target.getRootNode() === this);
    },
  });
  if (typeof ShadowRoot === 'function') {
    Object.defineProperty(ShadowRoot.prototype, 'getAnimations', {
      writable: true, enumerable: true, configurable: true,
      value: function getAnimations() {
        if (S.styleFlush) S.styleFlush();
        return relevantAnimations((target) => target.getRootNode() === this);
      },
    });
  }

  Object.defineProperty(Element.prototype, 'animate', {
    writable: true, enumerable: true, configurable: true,
    value: function animate(keyframes, options) {
      if (arguments.length === 0) throw new TypeError("Failed to execute 'animate' on 'Element': 1 argument required, but only 0 present.");
      let id;
      let timeline = S.documentTimeline;
      if (isObject(options)) {
        id = options.id;
        if (options.timeline !== undefined) timeline = options.timeline;
      }
      const effect = new KeyframeEffect(this, keyframes, options);
      const animation = new Animation(effect, timeline);
      if (id !== undefined) animation.id = id;
      animation.play();
      return animation;
    },
  });
})();
