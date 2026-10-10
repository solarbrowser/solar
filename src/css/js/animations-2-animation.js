// Web Animations, animations and timelines: the state machine of an animation (play, pause, finish, reverse, cancel, the playback rate),
// DocumentTimeline, AnimationPlaybackEvent and the frame that updates animations and sends their events.
(function () {
  'use strict';
  const S = globalThis.__solarAnim;
  const slots = new WeakMap();
  S.animationSlots = slots;

  const domException = (message, name) => new DOMException(message, name);

  // ---- Time ----

  // The time of the timeline: the time of the frame while frames are being made, otherwise the time of the task.
  let frameTime = null;
  let taskTime = null;
  let framesRunning = false;
  S.now = () => {
    if (framesRunning && frameTime !== null) return frameTime;
    if (taskTime === null) {
      taskTime = performance.now();
      setTimeout(() => { taskTime = null; }, 0);
    }
    return taskTime;
  };

  // ---- Events ----

  const pendingEvents = [];
  S.pendingEvents = pendingEvents;

  class AnimationPlaybackEvent extends Event {
    constructor(type, init = {}) {
      super(type, init);
      const dict = init === null || init === undefined ? {} : init;
      const read = (value) => (value === undefined || value === null ? null : Number(value));
      slots.set(this, { currentTime: read(dict.currentTime), timelineTime: read(dict.timelineTime) });
    }
    get currentTime() {
      return slots.get(this).currentTime;
    }
    get timelineTime() {
      return slots.get(this).timelineTime;
    }
  }
  Object.defineProperty(AnimationPlaybackEvent.prototype, Symbol.toStringTag, { value: 'AnimationPlaybackEvent', configurable: true });
  globalThis.AnimationPlaybackEvent = AnimationPlaybackEvent;
  S.queueEvent = (target, event, time) => {
    pendingEvents.push({ target, event, time: time === null || time === undefined ? Infinity : time, order: pendingEvents.length });
    S.wake();
  };

  // onfinish and the like: a listener on the animation that calls the handler there is.
  const defineHandler = (proto, type) => {
    const handlers = new WeakMap();
    Object.defineProperty(proto, 'on' + type, {
      enumerable: true,
      configurable: true,
      get() {
        const entry = handlers.get(this);
        return entry ? entry.handler : null;
      },
      set(value) {
        let entry = handlers.get(this);
        if (!entry) {
          entry = { handler: null, installed: false };
          handlers.set(this, entry);
        }
        entry.handler = typeof value === 'function' || (typeof value === 'object' && value !== null) ? value : null;
        if (!entry.installed) {
          entry.installed = true;
          this.addEventListener(type, (event) => {
            const handler = entry.handler;
            if (typeof handler === 'function') handler.call(this, event);
          });
        }
      },
    });
  };

  // ---- Timelines ----

  class AnimationTimeline {
    constructor() {
      throw new TypeError('Illegal constructor');
    }
    get currentTime() {
      const slot = slots.get(this);
      if (!slot) throw new TypeError('Illegal invocation');
      return slot.active ? S.now() - slot.originTime : null;
    }
    get duration() {
      if (!slots.get(this)) throw new TypeError('Illegal invocation');
      return null;
    }
  }
  Object.defineProperty(AnimationTimeline.prototype, Symbol.toStringTag, { value: 'AnimationTimeline', configurable: true });

  class DocumentTimeline extends AnimationTimeline {
    constructor(options = {}) {
      if (options !== undefined && options !== null && typeof options !== 'object' && typeof options !== 'function') throw new TypeError('The options is not an object.');
      let originTime = 0;
      if (options && options.originTime !== undefined) {
        originTime = Number(options.originTime);
        if (!Number.isFinite(originTime)) throw new TypeError("Failed to construct 'DocumentTimeline': The provided double value is non-finite.");
      }
      // AnimationTimeline's constructor throws: make the object without it.
      const timeline = Object.create(new.target.prototype);
      slots.set(timeline, { originTime, active: true, document });
      return timeline;
    }
  }
  Object.defineProperty(DocumentTimeline.prototype, Symbol.toStringTag, { value: 'DocumentTimeline', configurable: true });
  globalThis.AnimationTimeline = AnimationTimeline;
  globalThis.DocumentTimeline = DocumentTimeline;

  const documentTimeline = new DocumentTimeline();
  S.documentTimeline = documentTimeline;
  Object.defineProperty(Document.prototype, 'timeline', {
    enumerable: true,
    configurable: true,
    get() {
      return this === document ? documentTimeline : documentTimeline;
    },
  });

  // ---- Animation ----

  let sequence = 0;
  const live = new Set();  // the animations that may have something to do on a frame
  S.live = live;

  const isUnresolved = (v) => v === null || v === undefined;

  const timelineTimeOf = (slot) => (slot.timeline ? slot.timeline.currentTime : null);

  const effectEnd = (slot) => (slot.effect ? S.endTime(slotsOfEffect(slot.effect).timing) : 0);
  const slotsOfEffect = (effect) => S.effectSlots.get(effect);

  const currentTimeOf = (slot, ignoreHold = false) => {
    if (!ignoreHold && !isUnresolved(slot.holdTime)) return slot.holdTime;
    const timelineTime = timelineTimeOf(slot);
    if (isUnresolved(timelineTime) || isUnresolved(slot.startTime)) return null;
    const time = (timelineTime - slot.startTime) * slot.playbackRate;
    return time === 0 ? 0 : time;
  };

  const effectiveRate = (slot) => (slot.pendingRate === undefined ? slot.playbackRate : slot.pendingRate);

  const applyPendingRate = (slot) => {
    if (slot.pendingRate !== undefined) {
      slot.playbackRate = slot.pendingRate;
      slot.pendingRate = undefined;
    }
  };

  const makePromise = () => {
    const record = { resolved: false, settled: false };
    record.promise = new Promise((resolve, reject) => {
      record.resolve = resolve;
      record.reject = reject;
    });
    return record;
  };

  const playStateOf = (slot) => {
    const currentTime = currentTimeOf(slot);
    if (isUnresolved(currentTime) && isUnresolved(slot.startTime) && !slot.pendingPlay && !slot.pendingPause) return 'idle';
    if (slot.pendingPause || (isUnresolved(slot.startTime) && !slot.pendingPlay)) return 'paused';
    const rate = effectiveRate(slot);
    if (!isUnresolved(currentTime) && ((rate > 0 && currentTime >= effectEnd(slot)) || (rate < 0 && currentTime <= 0))) return 'finished';
    return 'running';
  };

  const resolveReady = (slot) => {
    slot.ready.resolve(slot.animation);
    slot.ready.settled = true;
  };

  const queueFinishEvent = (slot) => {
    const animation = slot.animation;
    const event = new AnimationPlaybackEvent('finish', { currentTime: currentTimeOf(slot), timelineTime: timelineTimeOf(slot) });
    S.queueEvent(animation, event, timelineTimeOf(slot));
  };

  const finishNotification = (slot) => {
    slot.finishMicrotask = false;
    if (playStateOf(slot) !== 'finished') return;
    if (slot.finished.settled) return;
    slot.finished.resolve(slot.animation);
    slot.finished.settled = true;
    queueFinishEvent(slot);
  };

  const updateFinishedState = (slot, didSeek, notify) => {
    const unconstrained = didSeek ? currentTimeOf(slot) : currentTimeOf(slot, true);
    const timelineTime = timelineTimeOf(slot);
    if (!isUnresolved(unconstrained) && !isUnresolved(slot.startTime) && !slot.pendingPlay && !slot.pendingPause) {
      const end = effectEnd(slot);
      if (slot.playbackRate > 0 && unconstrained >= end) {
        slot.holdTime = didSeek ? unconstrained : isUnresolved(slot.previousCurrentTime) ? end : Math.max(slot.previousCurrentTime, end);
      } else if (slot.playbackRate < 0 && unconstrained <= 0) {
        slot.holdTime = didSeek ? unconstrained : isUnresolved(slot.previousCurrentTime) ? 0 : (slot.previousCurrentTime < 0 ? slot.previousCurrentTime : 0);
      } else if (slot.playbackRate !== 0 && !isUnresolved(timelineTime)) {
        if (didSeek && !isUnresolved(slot.holdTime)) slot.startTime = timelineTime - slot.holdTime / slot.playbackRate;
        slot.holdTime = null;
      }
    }
    slot.previousCurrentTime = currentTimeOf(slot);
    if (playStateOf(slot) === 'finished') {
      if (!slot.finished.settled) {
        if (notify) {
          finishNotification(slot);
        } else if (!slot.finishMicrotask) {
          slot.finishMicrotask = true;
          queueMicrotask(() => {
            if (slot.finishMicrotask) finishNotification(slot);
          });
        }
      }
    } else {
      if (slot.finished.settled) slot.finished = makePromise();
      slot.finishMicrotask = false;
    }
    S.updated(slot.animation);
  };

  const resetPendingTasks = (slot) => {
    if (!slot.pendingPlay && !slot.pendingPause) return;
    slot.pendingPlay = false;
    slot.pendingPause = false;
    applyPendingRate(slot);
    slot.ready.reject(domException('The animation was aborted.', 'AbortError'));
    slot.ready.promise.catch(() => {});
    slot.ready = makePromise();
    slot.ready.resolve(slot.animation);
    slot.ready.settled = true;
  };

  const silentlySetCurrentTime = (slot, seekTime) => {
    if (isUnresolved(seekTime)) return;
    const timelineTime = timelineTimeOf(slot);
    if (!isUnresolved(slot.holdTime) || isUnresolved(slot.startTime) || isUnresolved(timelineTime) || slot.playbackRate === 0) slot.holdTime = seekTime;
    else slot.startTime = timelineTime - seekTime / slot.playbackRate;
    if (isUnresolved(timelineTime)) slot.startTime = null;
    slot.previousCurrentTime = null;
  };

  const setCurrentTime = (slot, seekTime) => {
    silentlySetCurrentTime(slot, seekTime);
    if (slot.pendingPause) {
      slot.holdTime = seekTime;
      applyPendingRate(slot);
      slot.startTime = null;
      slot.pendingPause = false;
      resolveReady(slot);
    }
    updateFinishedState(slot, true, false);
  };

  const play = (slot, autoRewind) => {
    const abortedPause = slot.pendingPause;
    const hadPendingReady = slot.pendingPlay || slot.pendingPause;
    const rate = effectiveRate(slot);
    const currentTime = currentTimeOf(slot);
    const end = effectEnd(slot);
    if (rate > 0 && autoRewind && (isUnresolved(currentTime) || currentTime < 0 || currentTime >= end)) {
      slot.holdTime = 0;
    } else if (rate < 0 && autoRewind && (isUnresolved(currentTime) || currentTime <= 0 || currentTime > end)) {
      if (end === Infinity) throw domException('Cannot play reversed Animation with infinite target effect end.', 'InvalidStateError');
      slot.holdTime = end;
    } else if (rate === 0 && isUnresolved(currentTime)) {
      slot.holdTime = 0;
    }
    if (!isUnresolved(slot.holdTime)) slot.startTime = null;
    const wasPending = slot.pendingPause || slot.pendingPlay;
    if (isUnresolved(slot.holdTime) && !abortedPause && slot.pendingRate === undefined && !wasPending && !isUnresolved(slot.startTime)) {
      // Already running and nothing to change.
      updateFinishedState(slot, false, false);
      return;
    }
    if (!hadPendingReady) slot.ready = makePromise();
    slot.pendingPause = false;
    slot.pendingPlay = true;
    live.add(slot.animation);
    S.wake();
    updateFinishedState(slot, false, false);
  };

  const pause = (slot) => {
    if (slot.pendingPause) return;
    if (playStateOf(slot) === 'paused') return;
    let seekTime = null;
    const rate = effectiveRate(slot);
    if (isUnresolved(currentTimeOf(slot))) {
      if (rate >= 0) seekTime = 0;
      else {
        const end = effectEnd(slot);
        if (end === Infinity) throw domException('Cannot pause reversed Animation with infinite target effect end.', 'InvalidStateError');
        seekTime = end;
      }
    }
    if (!isUnresolved(seekTime)) slot.holdTime = seekTime;
    const hadPendingReady = slot.pendingPlay || slot.pendingPause;
    if (slot.pendingPlay) slot.pendingPlay = false;
    if (!hadPendingReady) slot.ready = makePromise();
    slot.pendingPause = true;
    live.add(slot.animation);
    S.wake();
    updateFinishedState(slot, false, false);
  };

  // The tasks that wait for the animation to be ready run when a frame is made.
  const runPendingTasks = (slot, readyTime) => {
    if (slot.pendingPause) {
      slot.pendingPause = false;
      if (!isUnresolved(slot.startTime) && isUnresolved(slot.holdTime)) slot.holdTime = (readyTime - slot.startTime) * slot.playbackRate;
      applyPendingRate(slot);
      slot.startTime = null;
      resolveReady(slot);
      updateFinishedState(slot, false, false);
    } else if (slot.pendingPlay) {
      slot.pendingPlay = false;
      if (!isUnresolved(slot.holdTime)) {
        applyPendingRate(slot);
        slot.startTime = slot.playbackRate === 0 ? readyTime : readyTime - slot.holdTime / slot.playbackRate;
        if (slot.playbackRate !== 0) slot.holdTime = null;
      } else if (!isUnresolved(slot.startTime) && slot.pendingRate !== undefined) {
        const match = (readyTime - slot.startTime) * slot.playbackRate;
        applyPendingRate(slot);
        if (slot.playbackRate === 0) slot.holdTime = match;
        slot.startTime = slot.playbackRate === 0 ? readyTime : readyTime - match / slot.playbackRate;
      }
      resolveReady(slot);
      updateFinishedState(slot, false, false);
    }
  };

  class Animation extends EventTarget {
    constructor(effect = null, timeline) {
      super();
      if (effect !== null && effect !== undefined && !S.effectSlots.has(effect)) throw new TypeError("Failed to construct 'Animation': parameter 1 is not of type 'AnimationEffect'.");
      if (timeline !== undefined && timeline !== null && !(slots.get(timeline) && slots.get(timeline).originTime !== undefined)) {
        throw new TypeError("Failed to construct 'Animation': parameter 2 is not of type 'AnimationTimeline'.");
      }
      const slot = {
        animation: this,
        effect: null,
        timeline: timeline === undefined ? documentTimeline : timeline,
        startTime: null,
        holdTime: null,
        previousCurrentTime: null,
        playbackRate: 1,
        pendingRate: undefined,
        pendingPlay: false,
        pendingPause: false,
        ready: null,
        finished: makePromise(),
        finishMicrotask: false,
        id: '',
        replaceState: 'active',
        sequence: sequence++,
        category: 0,
      };
      slot.ready = makePromise();
      slot.ready.resolve(this);
      slot.ready.settled = true;
      slots.set(this, slot);
      S.setEffect(slot, effect === undefined ? null : effect);
      live.add(this);
    }
    get id() {
      return slotOf(this).id;
    }
    set id(value) {
      slotOf(this).id = String(value);
    }
    get effect() {
      return slotOf(this).effect;
    }
    set effect(value) {
      if (value !== null && !S.effectSlots.has(value)) throw new TypeError("Failed to set the 'effect' property on 'Animation': The provided value is not of type 'AnimationEffect'.");
      S.setEffect(slotOf(this), value);
    }
    get timeline() {
      return slotOf(this).timeline;
    }
    set timeline(value) {
      const slot = slotOf(this);
      if (value !== null && !(slots.get(value) && slots.get(value).originTime !== undefined)) throw new TypeError("Failed to set the 'timeline' property on 'Animation': The provided value is not of type 'AnimationTimeline'.");
      if (slot.timeline === value) return;
      slot.timeline = value;
      if (!isUnresolved(slot.startTime)) slot.holdTime = null;
      updateFinishedState(slot, false, false);
    }
    get startTime() {
      return slotOf(this).startTime;
    }
    set startTime(value) {
      const slot = slotOf(this);
      if (value !== null && value !== undefined) {
        value = Number(S.toNumberish(value));
        if (!Number.isFinite(value)) throw new TypeError("Failed to set the 'startTime' property on 'Animation': The provided double value is non-finite.");
      } else if (value === undefined) value = null;
      const timelineTime = timelineTimeOf(slot);
      if (isUnresolved(timelineTime) && value !== null) slot.holdTime = null;
      const previous = currentTimeOf(slot);
      applyPendingRate(slot);
      slot.startTime = value;
      if (value !== null) {
        if (slot.playbackRate !== 0) slot.holdTime = null;
      } else {
        slot.holdTime = previous;
      }
      if (slot.pendingPlay || slot.pendingPause) {
        slot.pendingPlay = false;
        slot.pendingPause = false;
        resolveReady(slot);
      }
      updateFinishedState(slot, true, false);
    }
    get currentTime() {
      return currentTimeOf(slotOf(this));
    }
    set currentTime(value) {
      const slot = slotOf(this);
      if (value === null || value === undefined) {
        if (currentTimeOf(slot) !== null) throw new TypeError("Failed to set the 'currentTime' property on 'Animation': The provided value is null.");
        return;
      }
      value = Number(S.toNumberish(value));
      if (!Number.isFinite(value)) throw new TypeError("Failed to set the 'currentTime' property on 'Animation': The provided double value is non-finite.");
      setCurrentTime(slot, value);
    }
    get playbackRate() {
      return slotOf(this).playbackRate;
    }
    set playbackRate(value) {
      const slot = slotOf(this);
      value = Number(value);
      if (!Number.isFinite(value)) throw new TypeError("Failed to set the 'playbackRate' property on 'Animation': The provided double value is non-finite.");
      slot.pendingRate = undefined;
      const previousTime = currentTimeOf(slot);
      slot.playbackRate = value;
      if (!isUnresolved(previousTime)) setCurrentTime(slot, previousTime);
      else updateFinishedState(slot, false, false);
    }
    get playState() {
      return playStateOf(slotOf(this));
    }
    get overallProgress() {
      const slot = slotOf(this);
      const currentTime = currentTimeOf(slot);
      if (isUnresolved(currentTime) || !slot.effect) return null;
      const end = effectEnd(slot);
      if (end === 0) return currentTime < 0 ? 0 : 1;
      if (end === Infinity) return 0;
      return Math.min(Math.max(currentTime / end, 0), 1);
    }
    get replaceState() {
      return slotOf(this).replaceState;
    }
    get pending() {
      const slot = slotOf(this);
      return slot.pendingPlay || slot.pendingPause;
    }
    get ready() {
      return slotOf(this).ready.promise;
    }
    get finished() {
      return slotOf(this).finished.promise;
    }
    cancel() {
      const slot = slotOf(this);
      if (playStateOf(slot) !== 'idle') {
        resetPendingTasks(slot);
        slot.finished.reject(domException('The animation was aborted.', 'AbortError'));
        slot.finished.promise.catch(() => {});
        slot.finished = makePromise();
        slot.finishMicrotask = false;
        const timelineTime = timelineTimeOf(slot);
        S.queueEvent(this, new AnimationPlaybackEvent('cancel', { currentTime: null, timelineTime }), timelineTime);
      }
      slot.holdTime = null;
      slot.startTime = null;
      slot.previousCurrentTime = null;
      S.updated(this);
    }
    finish() {
      const slot = slotOf(this);
      const rate = effectiveRate(slot);
      const end = effectEnd(slot);
      if (rate === 0) throw domException('Cannot finish Animation with a playbackRate of 0.', 'InvalidStateError');
      if (rate > 0 && end === Infinity) throw domException('Cannot finish Animation with an infinite target effect end.', 'InvalidStateError');
      applyPendingRate(slot);
      const limit = slot.playbackRate > 0 ? end : 0;
      silentlySetCurrentTime(slot, limit);
      const timelineTime = timelineTimeOf(slot);
      if (isUnresolved(slot.startTime) && !isUnresolved(timelineTime)) slot.startTime = timelineTime - limit / slot.playbackRate;
      if (slot.pendingPause && !isUnresolved(slot.startTime)) {
        slot.holdTime = null;
        slot.pendingPause = false;
        resolveReady(slot);
      }
      if (slot.pendingPlay && !isUnresolved(slot.startTime)) {
        slot.pendingPlay = false;
        resolveReady(slot);
      }
      updateFinishedState(slot, true, true);
    }
    play() {
      play(slotOf(this), true);
    }
    pause() {
      pause(slotOf(this));
    }
    reverse() {
      const slot = slotOf(this);
      if (!slot.timeline) throw domException('Cannot reverse an animation with no active timeline.', 'InvalidStateError');
      if (isUnresolved(timelineTimeOf(slot))) throw domException('Cannot reverse an animation with no active timeline.', 'InvalidStateError');
      const original = slot.pendingRate;
      slot.pendingRate = effectiveRate(slot) === 0 ? 0 : -effectiveRate(slot);
      try {
        play(slot, true);
      } catch (error) {
        slot.pendingRate = original;
        throw error;
      }
    }
    updatePlaybackRate(rate) {
      const slot = slotOf(this);
      rate = Number(rate);
      if (!Number.isFinite(rate)) throw new TypeError("Failed to execute 'updatePlaybackRate' on 'Animation': The provided double value is non-finite.");
      const previousState = playStateOf(slot);
      slot.pendingRate = rate;
      if (previousState === 'paused' && slot.pendingPause) {
        // The pause task applies it, after the current time it holds has been worked out at the rate there is.
      } else if (previousState === 'idle' || previousState === 'paused' || isUnresolved(currentTimeOf(slot))) {
        applyPendingRate(slot);
        updateFinishedState(slot, false, false);
      } else if (previousState === 'finished') {
        const timelineTime = timelineTimeOf(slot);
        const unconstrained = (timelineTime - slot.startTime) * slot.pendingRate;
        slot.startTime = slot.pendingRate === 0 ? timelineTime : timelineTime - unconstrained / slot.pendingRate;
        applyPendingRate(slot);
        updateFinishedState(slot, false, false);
      } else {
        play(slot, false);
      }
    }
    persist() {
      slotOf(this).replaceState = 'persisted';
    }
    commitStyles() {
      S.commitStyles(slotOf(this));
    }
  }
  Object.defineProperty(Animation.prototype, Symbol.toStringTag, { value: 'Animation', configurable: true });
  for (const type of ['finish', 'cancel', 'remove']) defineHandler(Animation.prototype, type);
  globalThis.Animation = Animation;

  function slotOf(animation) {
    const slot = slots.get(animation);
    if (!slot || slot.animation !== animation) throw new TypeError('Illegal invocation');
    return slot;
  }
  S.slotOf = slotOf;
  S.currentTimeOf = currentTimeOf;
  S.playStateOf = playStateOf;
  S.effectEnd = effectEnd;
  S.updateFinishedState = updateFinishedState;
  S.timelineTimeOf = timelineTimeOf;

  // ---- The effect of an animation ----

  S.setEffect = (slot, effect) => {
    const old = slot.effect;
    if (old === effect) return;
    if (effect) {
      const other = S.effectSlots.get(effect).animation;
      if (other && other !== slot.animation) S.setEffect(slotOf(other), null);
    }
    if (old) S.effectSlots.get(old).animation = null;
    slot.effect = effect;
    if (effect) S.effectSlots.get(effect).animation = slot.animation;
    if (old) S.retarget(old);
    if (slot.finished) updateFinishedState(slot, true, false);
    else S.updated(slot.animation);
  };

  // ---- Frames ----

  // Whether the animation needs the next frame.
  const wantsFrame = (slot) => {
    if (slot.pendingPlay || slot.pendingPause) return true;
    if (!slot.timeline || isUnresolved(slot.startTime) || slot.playbackRate === 0) return false;
    return playStateOf(slot) === 'running';
  };

  S.wantsFrame = () => {
    if (pendingEvents.length) return true;
    for (const animation of live) {
      if (wantsFrame(slots.get(animation))) return true;
    }
    return S.cssWantsFrame ? S.cssWantsFrame() : false;
  };

  S.wake = () => {
    if (S.requestFrame) S.requestFrame();
  };

  // "update animations and send events".
  S.frame = (now) => {
    frameTime = now;
    framesRunning = true;
    if (S.cssBeforeFrame) S.cssBeforeFrame();
    const animations = [...live];
    for (const animation of animations) {
      const slot = slots.get(animation);
      if ((slot.pendingPlay || slot.pendingPause) && !isUnresolved(timelineTimeOf(slot))) runPendingTasks(slot, timelineTimeOf(slot));
    }
    for (const animation of animations) {
      const slot = slots.get(animation);
      updateFinishedState(slot, false, true);
    }
    if (S.cssAfterFrame) S.cssAfterFrame();
    S.refreshAll();
    // Events go in the order of the time they are for, then the order they were made in.
    const events = pendingEvents.splice(0, pendingEvents.length);
    events.sort((a, b) => a.time - b.time || a.order - b.order);
    for (const item of events) {
      try {
        item.target.dispatchEvent(item.event);
      } catch (error) {
        setTimeout(() => { throw error; }, 0);
      }
    }
    for (const animation of animations) {
      const slot = slots.get(animation);
      if (!slot) continue;
      if (playStateOf(slot) === 'idle' && !slot.pendingPlay && !slot.pendingPause && !(slot.effect && S.inEffect(slot))) {
        live.delete(animation);
        S.dropEffect(slot);
      }
    }
  };

  S.endFrame = () => {
    framesRunning = false;
    frameTime = null;
  };
  S.sequenceOf = (slot) => slot.sequence;
  S.makePromise = makePromise;
  S.defineHandler = defineHandler;
  S.domException = domException;
})();
