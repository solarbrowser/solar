// CSS Animations: CSSAnimation, AnimationEvent, and the animations the animation-* properties of elements make, update and cancel.
(function () {
  'use strict';
  const S = globalThis.__solarAnim;
  const N = S.natives;
  const slots = S.animationSlots;

  // ---- Events ----

  class AnimationEvent extends Event {
    constructor(type, init = {}) {
      super(type, init);
      const dict = init === null || init === undefined ? {} : init;
      slots.set(this, {
        animationName: dict.animationName === undefined ? '' : String(dict.animationName),
        elapsedTime: dict.elapsedTime === undefined ? 0 : Number(dict.elapsedTime),
        pseudoElement: dict.pseudoElement === undefined ? '' : String(dict.pseudoElement),
        animation: dict.animation === undefined ? null : dict.animation,
      });
    }
    get animation() {
      return slots.get(this).animation;
    }
    get animationName() {
      return slots.get(this).animationName;
    }
    get elapsedTime() {
      return slots.get(this).elapsedTime;
    }
    get pseudoElement() {
      return slots.get(this).pseudoElement;
    }
  }
  Object.defineProperty(AnimationEvent.prototype, Symbol.toStringTag, { value: 'AnimationEvent', configurable: true });
  globalThis.AnimationEvent = AnimationEvent;

  // ---- CSSAnimation ----

  let allowConstruct = false;
  const construct = (Class, ...args) => {
    allowConstruct = true;
    try {
      return new Class(...args);
    } finally {
      allowConstruct = false;
    }
  };
  S.construct = construct;
  S.constructAllowed = () => allowConstruct;

  class CSSAnimation extends Animation {
    constructor(effect, timeline) {
      if (!allowConstruct) throw new TypeError('Illegal constructor');
      super(effect, timeline);
    }
    get animationName() {
      return S.slotOf(this).cssName;
    }
  }
  Object.defineProperty(CSSAnimation.prototype, Symbol.toStringTag, { value: 'CSSAnimation', configurable: true });
  globalThis.CSSAnimation = CSSAnimation;

  // ---- Reading style ----

  const splitTop = (text) => {
    const parts = [];
    let depth = 0, start = 0, quote = null;
    for (let i = 0; i < text.length; i++) {
      const c = text[i];
      if (quote) {
        if (c === '\\') i++;
        else if (c === quote) quote = null;
      } else if (c === '"' || c === "'") quote = c;
      else if (c === '(') depth++;
      else if (c === ')') depth--;
      else if (c === ',' && depth === 0) {
        parts.push(text.slice(start, i).trim());
        start = i + 1;
      }
    }
    parts.push(text.slice(start).trim());
    return parts;
  };
  S.splitTop = splitTop;

  const readList = (el, pseudo, property) => {
    const text = N.read(el, pseudo, property);
    if (text === null || text === '') return [];
    return splitTop(text);
  };

  const parseTime = (text) => {
    const m = /^([+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)(ms|s)$/.exec(text.trim());
    if (!m) return 0;
    return m[2] === 's' ? parseFloat(m[1]) * 1000 : parseFloat(m[1]);
  };
  S.parseTime = parseTime;

  const unquote = (text) => {
    if (text.length >= 2 && (text[0] === '"' || text[0] === "'")) {
      return text.slice(1, -1).replace(/\\(.)/gs, '$1');
    }
    return text;
  };

  // The value of each list property for entry i, the list being repeated to be as long as the names.
  const at = (list, i, fallback) => (list.length ? list[i % list.length] : fallback);

  const readAnimations = (el, pseudo, mayBeUnrendered) => {
    const names = readList(el, pseudo, 'animation-name');
    const entries = [];
    if (!names.length || names.every((name) => name.toLowerCase() === 'none')) return entries;
    if (mayBeUnrendered && !el.isConnected || (mayBeUnrendered && !rendered(el))) return entries;
    const durations = readList(el, pseudo, 'animation-duration');
    const timings = readList(el, pseudo, 'animation-timing-function');
    const delays = readList(el, pseudo, 'animation-delay-start');
    const endDelays = readList(el, pseudo, 'animation-delay-end');
    const counts = readList(el, pseudo, 'animation-iteration-count');
    const directions = readList(el, pseudo, 'animation-direction');
    const fills = readList(el, pseudo, 'animation-fill-mode');
    const states = readList(el, pseudo, 'animation-play-state');
    const compositions = readList(el, pseudo, 'animation-composition');
    names.forEach((raw, i) => {
      if (raw.toLowerCase() === 'none') return;
      const count = at(counts, i, '1');
      entries.push({
        index: i,
        name: unquote(raw),
        duration: at(durations, i, '0s'),
        timing: at(timings, i, 'ease'),
        delay: at(delays, i, '0s'),
        endDelay: at(endDelays, i, '0s'),
        count: count === 'infinite' ? Infinity : parseFloat(count),
        direction: at(directions, i, 'normal'),
        fill: at(fills, i, 'none'),
        state: at(states, i, 'running'),
        composition: at(compositions, i, 'replace'),
      });
    });
    return entries;
  };

  // ---- @keyframes ----

  const keyframesFor = (el, name) => {
    let root = el.getRootNode();
    const found = [];
    const layers = new Map();
    const layerIndex = (path) => {
      if (!layers.has(path)) layers.set(path, layers.size);
      return layers.get(path);
    };
    let order = 0;
    const visit = (rules, layer) => {
      for (const rule of rules) {
        const type = rule.constructor && rule.constructor.name;
        if (type === 'CSSKeyframesRule') {
          if (rule.name === name) found.push({ rule, layer, order: order++ });
        } else if (type === 'CSSMediaRule') {
          if (matchMedia(rule.media.mediaText).matches) visit(rule.cssRules, layer);
        } else if (type === 'CSSSupportsRule') {
          if (CSS.supports(rule.conditionText)) visit(rule.cssRules, layer);
        } else if (type === 'CSSLayerBlockRule') {
          const path = layer === null ? rule.name : layer + '.' + rule.name;
          layerIndex(path);
          visit(rule.cssRules, path);
        } else if (type === 'CSSLayerStatementRule') {
          for (const n of rule.nameList) layerIndex(layer === null ? n : layer + '.' + n);
        } else if (type === 'CSSImportRule') {
          if (rule.styleSheet && (!rule.media || matchMedia(rule.media.mediaText).matches)) visit(rule.styleSheet.cssRules, layer);
        } else if (rule.cssRules && type !== 'CSSStyleRule') {
          visit(rule.cssRules, layer);
        }
      }
    };
    const sheets = [];
    if (root && root.styleSheets) for (const s of root.styleSheets) sheets.push(s);
    if (root && root.adoptedStyleSheets) for (const s of root.adoptedStyleSheets) sheets.push(s);
    for (const sheet of sheets) {
      if (sheet.disabled) continue;
      try {
        if (sheet.media && sheet.media.mediaText && !matchMedia(sheet.media.mediaText).matches) continue;
        visit(sheet.cssRules, null);
      } catch (e) {
        // a sheet from another origin has no rules to read
      }
    }
    if (!found.length) return null;
    // Unlayered rules win over layered ones, a later layer over an earlier, and a later rule over an earlier.
    found.sort((a, b) => (a.layer === null ? Infinity : layerIndex(a.layer)) - (b.layer === null ? Infinity : layerIndex(b.layer)) || a.order - b.order);
    return found[found.length - 1].rule;
  };

  const parseKeyText = (text) => {
    const offsets = [];
    for (const part of text.split(',')) {
      const word = part.trim().toLowerCase();
      if (word === 'from') offsets.push(0);
      else if (word === 'to') offsets.push(1);
      else if (word.endsWith('%')) offsets.push(parseFloat(word) / 100);
    }
    return offsets;
  };

  const COMPOSITES = ['replace', 'add', 'accumulate'];

  // The keyframes of a @keyframes rule, for an animation whose timing function is `timing`: those of the same offset,
  // easing and composite are one, and for a property at one offset the last declaration wins.
  const keyframesOfRule = (el, pseudo, rule, timing) => {
    const byOffset = new Map();
    const declared = [];
    if (rule) {
      for (const keyframe of rule.cssRules) {
        const offsets = parseKeyText(keyframe.keyText);
        for (const offset of offsets) declared.push({ offset, style: keyframe.style });
      }
    }
    // For one offset the later rule wins, so the rules are put in the order of their offset first (stable).
    const ordered = declared.map((d, i) => ({ ...d, i })).sort((a, b) => a.offset - b.offset || a.i - b.i);
    const result = [];
    const properties = new Map();  // offset → css name → value
    const easings = new Map();
    const composites = new Map();
    for (const { offset, style } of ordered) {
      let easing = null;
      let composite = null;
      const values = [];
      for (let i = 0; i < style.length; i++) {
        const name = style.item(i);
        if (style.getPropertyPriority(name) === 'important') continue;
        const value = style.getPropertyValue(name);
        if (name === 'animation-timing-function') {
          easing = value;
          continue;
        }
        if (name === 'animation-composition') {
          composite = value;
          continue;
        }
        if (name.startsWith('--') || name.startsWith('animation-') || name.startsWith('transition-')) continue;
        if (name === 'animation' || name === 'transition' || name === 'display' && false) continue;
        values.push([name, value]);
      }
      if (!properties.has(offset)) {
        properties.set(offset, []);
      }
      properties.get(offset).push({ values, easing, composite });
    }
    for (const [offset, groups] of properties) {
      // Groups of one offset with different easing/composite are separate keyframes.
      const keyed = new Map();
      for (const group of groups) {
        const key = (group.easing === null ? '\u0000' : group.easing) + '|' + (group.composite === null ? '\u0000' : group.composite);
        let entry = keyed.get(key);
        if (!entry) keyed.set(key, (entry = { easing: group.easing, composite: group.composite, values: new Map() }));
        for (const [name, value] of group.values) entry.values.set(name, value);
        entry.touched = true;
      }
      // A property declared in several groups of the offset belongs to the last of them.
      const last = new Map();
      [...keyed.values()].forEach((entry, idx) => {
        for (const name of entry.values.keys()) last.set(name, idx);
      });
      [...keyed.values()].forEach((entry, idx) => {
        for (const name of [...entry.values.keys()]) if (last.get(name) !== idx) entry.values.delete(name);
      });
      for (const entry of keyed.values()) {
        const parsedEasing = S.parseEasing(entry.easing === null ? timing : entry.easing) || S.parseEasing('ease');
        const composite = entry.composite !== null && COMPOSITES.includes(entry.composite) ? entry.composite : 'auto';
        const keyframe = { offset, computedOffset: offset, easing: parsedEasing, composite, values: new Map() };
        for (const [name, value] of entry.values) {
          const kind = N.kind(name);
          if (!kind) continue;
          const pairs = S.declare(name, value);
          if (!pairs) continue;
          for (const [longhand, text] of pairs) keyframe.values.set(longhand, text);
        }
        if (keyframe.values.size) result.push(keyframe);
      }
    }
    result.sort((a, b) => a.offset - b.offset);
    return result;
  };

  // ---- Making and updating the animations of an element ----

  const records = new Map();  // element → pseudo → [{ animation, entry, ... }]
  const all = new Set();
  S.cssAnimations = all;

  const timingOf = (entry) => ({
    delay: parseTime(entry.delay),
    endDelay: parseTime(entry.endDelay),
    fill: entry.fill === 'none' ? 'none' : entry.fill,
    iterationStart: 0,
    iterations: Number.isNaN(entry.count) ? 1 : entry.count,
    duration: entry.duration === 'auto' ? 'auto' : parseTime(entry.duration),
    direction: entry.direction,
    easing: 'linear',
    easingFn: S.parseEasing('linear'),
  });

  const pseudoName = (pseudo) => (pseudo ? '::' + pseudo : null);

  const effectOfAnimation = (animation) => S.effectSlots.get(S.slotOf(animation).effect);

  const makeAnimation = (el, pseudo, entry, order) => {
    const rule = keyframesFor(el, entry.name);
    const effect = new KeyframeEffect(el, null, 0);
    const effectSlot = S.effectSlots.get(effect);
    effectSlot.pseudo = pseudoName(pseudo);
    effectSlot.timing = timingOf(entry);
    effectSlot.composite = COMPOSITES.includes(entry.composition) ? entry.composition : 'replace';
    effectSlot.keyframes = keyframesOfRule(el, pseudo, rule, entry.timing);
    const animation = construct(CSSAnimation, effect, S.documentTimeline);
    const aslot = S.slotOf(animation);
    aslot.category = 1;
    aslot.cssName = entry.name;
    aslot.owner = el;
    aslot.order = order;
    aslot.cssKind = 'animation';
    aslot.css = { pseudo, entry, signature: rule ? rule.cssText : null, phase: 'idle', iteration: null };
    aslot.cssEvents = true;
    all.add(animation);
    // The start of the animation is not in the frame but at it: the animation is made pending and starts with the next.
    animation.play();
    if (entry.state === 'paused') animation.pause();
    return animation;
  };

  const sameEntry = (a, b, keys) => keys.every((k) => (Object.is(a[k], b[k])));

  const updateAnimation = (animation, el, pseudo, entry, order) => {
    const aslot = S.slotOf(animation);
    const state = aslot.css;
    const old = state.entry;
    const effectSlot = effectOfAnimation(animation);
    aslot.order = order;
    // What changed in style is what is set; what script set stays.
    const timingKeys = ['duration', 'delay', 'endDelay', 'count', 'direction', 'fill'];
    if (!sameEntry(old, entry, timingKeys)) {
      const next = timingOf(entry);
      const was = timingOf(old);
      for (const k of Object.keys(next)) {
        if (k === 'easingFn' || k === 'easing') continue;
        if (!Object.is(next[k], was[k])) effectSlot.timing[k] = next[k];
      }
      S.updateFinishedState(aslot, false, false);
    }
    const rule = keyframesFor(el, entry.name);
    const signature = rule ? rule.cssText : null;
    if (signature !== state.signature || old.timing !== entry.timing || old.composition !== entry.composition) {
      effectSlot.keyframes = keyframesOfRule(el, pseudo, rule, entry.timing);
      effectSlot.cache = null;
      if (old.composition !== entry.composition) effectSlot.composite = COMPOSITES.includes(entry.composition) ? entry.composition : 'replace';
      state.signature = signature;
    }
    if (old.state !== entry.state) {
      if (entry.state === 'paused') animation.pause();
      else animation.play();
    }
    state.entry = entry;
    S.updated(animation);
  };

  const cancelAnimation = (animation) => {
    const aslot = S.slotOf(animation);
    queueCssCancel(animation);
    all.delete(animation);
    aslot.cssEvents = false;
    animation.cancel();
  };

  // ---- Scanning ----

  const tracked = new Map();  // element → Map(pseudo → [animations])

  const rendered = (el) => {
    for (let node = el; node && node.nodeType === 1; node = node.parentNode instanceof Element ? node.parentNode : (node.parentNode && node.parentNode.host) || null) {
      if (N.read(node, '', 'display') === 'none') return false;
    }
    return true;
  };

  const elementsOf = (root, out) => {
    for (const el of root.querySelectorAll('*')) {
      out.push(el);
      if (el.shadowRoot) elementsOf(el.shadowRoot, out);
    }
    return out;
  };

  let pseudoPossible = false;
  const pseudoTexts = () => {
    const found = [];
    const visit = (rules) => {
      for (const rule of rules) {
        if (rule.selectorText !== undefined) {
          if (/::?(before|after|marker)/i.test(rule.selectorText)) pseudoPossible = true;
        }
        if (rule.cssRules) visit(rule.cssRules);
      }
    };
    pseudoPossible = false;
    for (const sheet of document.styleSheets) {
      try { visit(sheet.cssRules); } catch (e) { /* cross-origin */ }
    }
  };

  let scannedVersion = -1;
  // Anything that changes the tree or what is declared may start or end an animation: a frame is asked for when something has
  // been declared that asks for animations.
  let mentions = 0;
  const watch = () => {
    if (typeof MutationObserver !== 'function') return;
    const observer = new MutationObserver(() => {
      const now = N.mentions();
      if (now > 0 || tracked.size) {
        mentions = now;
        S.wake();
      }
    });
    observer.observe(document, { subtree: true, childList: true, attributes: true, characterData: true });
  };
  watch();

  let lastVersion = -1;
  S.cssUpdate = () => {
    const version = N.version();
    // Nothing starts or ends but with a change of style, and the animations and transitions there are were seen to by the last.
    if (version === lastVersion && !S.cssForce) return;
    lastVersion = version;
    const elements = elementsOf(document, []);
    if (document.documentElement) elements.unshift(document.documentElement);
    if (scannedVersion !== version) pseudoTexts();
    scannedVersion = version;
    const live = new Set();
    for (const el of elements) {
      for (const pseudo of pseudoPossible ? ['', 'before', 'after', 'marker'] : ['']) {
        const entries = readAnimations(el, pseudo, true);
        let byPseudo = tracked.get(el);
        const existing = byPseudo && byPseudo.get(pseudo) ? byPseudo.get(pseudo) : [];
        if (entries.length || existing.length) {
          if (!byPseudo) tracked.set(el, (byPseudo = new Map()));
          // The animations that stay are those of the same name, one for one in order.
          const used = new Set();
          const next = [];
          entries.forEach((entry, order) => {
            const match = existing.find((animation) => !used.has(animation) && S.slotOf(animation).cssName === entry.name);
            if (match) {
              used.add(match);
              updateAnimation(match, el, pseudo, entry, order);
              next.push(match);
            } else {
              next.push(makeAnimation(el, pseudo, entry, order));
            }
          });
          for (const animation of existing) if (!used.has(animation)) cancelAnimation(animation);
          if (next.length) byPseudo.set(pseudo, next);
          else byPseudo.delete(pseudo);
          if (byPseudo.size === 0) tracked.delete(el);
          live.add(el);
        }
        if (S.transitionsUpdate) S.transitionsUpdate(el, pseudo);
        live.add(el);
      }
    }
    if (S.transitionsCleanup) S.transitionsCleanup(live);
    // Elements that are gone lose their animations.
    for (const [el, byPseudo] of [...tracked]) {
      if (live.has(el) && el.isConnected) continue;
      for (const animations of byPseudo.values()) for (const animation of animations) cancelAnimation(animation);
      tracked.delete(el);
    }
  };

  // ---- Events ----

  const eventFor = (animation, type, elapsed, time) => {
    const aslot = S.slotOf(animation);
    const effectSlot = effectOfAnimation(animation);
    const target = effectSlot.target;
    let event;
    if (aslot.cssKind === 'transition') {
      event = new TransitionEvent(type, { bubbles: true, cancelable: false, propertyName: aslot.transitionProperty, elapsedTime: elapsed / 1000, pseudoElement: effectSlot.pseudo || '' });
    } else {
      event = new AnimationEvent(type, { bubbles: true, cancelable: false, animation, animationName: aslot.cssName, elapsedTime: elapsed / 1000, pseudoElement: effectSlot.pseudo || '' });
    }
    S.queueEvent(target, event, time);
  };
  S.cssEvent = eventFor;

  function queueCssCancel(animation) {
    const aslot = S.slotOf(animation);
    if (!aslot.css || !aslot.cssEvents) return;
    const state = aslot.css;
    if (state.phase === 'idle') return;
    const effectSlot = effectOfAnimation(animation);
    const t = effectSlot.timing;
    const currentTime = S.currentTimeOf(aslot);
    const activeDuration = S.activeDuration(t);
    const elapsed = currentTime === null ? 0 : Math.max(Math.min(currentTime - t.delay, activeDuration), 0);
    eventFor(animation, aslot.cssKind === 'transition' ? 'transitioncancel' : 'animationcancel', elapsed, S.timelineTimeOf(aslot));
    state.phase = 'idle';
  }
  S.cssCancelEvent = queueCssCancel;

  // The events the phase an animation or a transition is in now, and the one it was in at the last frame, ask for.
  S.cssAfterFrame = () => {
    for (const animation of [...all]) {
      const aslot = S.slotOf(animation);
      if (!aslot.cssEvents) continue;
      const transition = aslot.cssKind === 'transition';
      const state = aslot.css;
      const effectSlot = effectOfAnimation(animation);
      const t = effectSlot.timing;
      const c = S.effectComputed(effectSlot);
      const phase = c.phase;
      const prev = state.phase;
      const duration = t.duration === 'auto' ? 0 : t.duration;
      const activeDuration = S.activeDuration(t);
      const rate = aslot.playbackRate;
      const startElapsed = Math.max(Math.min(-t.delay, activeDuration), 0);
      const timeFor = (boundaryLocal) => (aslot.startTime === null || aslot.startTime === undefined ? S.timelineTimeOf(aslot) : aslot.startTime + boundaryLocal / (rate || 1));
      const iteration = c.currentIteration;
      if (phase === 'idle') {
        if (prev !== 'idle') eventFor(animation, transition ? 'transitioncancel' : 'animationcancel', 0, S.timelineTimeOf(aslot));
        state.phase = 'idle';
        continue;
      }
      const send = (name, elapsed, local) => eventFor(animation, name, elapsed, timeFor(local));
      if (transition) {
        const names = { run: 'transitionrun', start: 'transitionstart', end: 'transitionend' };
        if (prev === 'idle') {
          send(names.run, startElapsed, t.delay);
          if (phase === 'active' || phase === 'after') send(names.start, startElapsed, t.delay);
          if (phase === 'after') send(names.end, activeDuration, t.delay + activeDuration);
        } else if (prev === 'before') {
          if (phase === 'active' || phase === 'after') send(names.start, startElapsed, t.delay);
          if (phase === 'after') send(names.end, activeDuration, t.delay + activeDuration);
        } else if (prev === 'active') {
          if (phase === 'after') send(names.end, activeDuration, t.delay + activeDuration);
          else if (phase === 'before') send(names.end, startElapsed, t.delay);
        } else if (prev === 'after') {
          if (phase === 'active') send(names.start, activeDuration, t.delay + activeDuration);
          else if (phase === 'before') {
            send(names.start, activeDuration, t.delay + activeDuration);
            send(names.end, startElapsed, t.delay);
          }
        }
      } else {
        const wasBeforeLike = prev === 'idle' || prev === 'before';
        if (wasBeforeLike && phase === 'active') {
          send('animationstart', startElapsed, t.delay);
        } else if (wasBeforeLike && phase === 'after') {
          send('animationstart', startElapsed, t.delay);
          send('animationend', activeDuration, t.delay + activeDuration);
        } else if (prev === 'active' && phase === 'active') {
          if (iteration !== state.iteration && state.iteration !== null) {
            const boundary = (Math.max(iteration, state.iteration) - t.iterationStart) * duration;
            send('animationiteration', boundary, t.delay + boundary);
          }
        } else if (prev === 'active' && phase === 'after') {
          send('animationend', activeDuration, t.delay + activeDuration);
        } else if (prev === 'active' && phase === 'before') {
          send('animationend', startElapsed, t.delay);
        } else if (prev === 'after' && phase === 'active') {
          send('animationstart', activeDuration, t.delay + activeDuration);
        } else if (prev === 'after' && phase === 'before') {
          send('animationstart', activeDuration, t.delay + activeDuration);
          send('animationend', startElapsed, t.delay);
        }
      }
      state.phase = phase;
      state.iteration = iteration;
    }
  };

  S.cssBeforeFrame = () => S.cssUpdate();

  S.cssWantsFrame = () => false;

  const baseFlush = S.styleFlush;
  S.cssFlush = () => S.cssUpdate();
})();
