// Web Animations, the timing model: easing functions, the timing of an effect and what it comes to at a time.
// The pieces are shared with the other animations-*.js files through globalThis.__solarAnim, which the last of them takes away.
(function () {
  'use strict';
  const S = (globalThis.__solarAnim = {});

  // ---- Easing ----

  const cubicBezier = (x1, y1, x2, y2) => {
    const bx = (t) => 3 * (1 - t) * (1 - t) * t * x1 + 3 * (1 - t) * t * t * x2 + t * t * t;
    const by = (t) => 3 * (1 - t) * (1 - t) * t * y1 + 3 * (1 - t) * t * t * y2 + t * t * t;
    const dx = (t) => 3 * (1 - t) * (1 - t) * x1 + 6 * (1 - t) * t * (x2 - x1) + 3 * t * t * (1 - x2);
    return (x) => {
      if (x < 0) {
        if (x1 > 0) return (y1 / x1) * x;
        if (y1 === 0 && x2 > 0) return (y2 / x2) * x;
        return 0;
      }
      if (x > 1) {
        if (x2 < 1) return 1 + ((y2 - 1) / (x2 - 1)) * (x - 1);
        if (y2 === 1 && x1 < 1) return 1 + ((y1 - 1) / (x1 - 1)) * (x - 1);
        return 1;
      }
      let t = x;
      for (let i = 0; i < 8; i++) {
        const err = bx(t) - x;
        if (Math.abs(err) < 1e-7) return by(t);
        const slope = dx(t);
        if (Math.abs(slope) < 1e-6) break;
        t -= err / slope;
      }
      let lo = 0, hi = 1;
      t = x;
      while (lo < hi) {
        const v = bx(t);
        if (Math.abs(v - x) < 1e-7) break;
        if (x > v) lo = t; else hi = t;
        t = (hi - lo) / 2 + lo;
        if (hi - lo < 1e-9) break;
      }
      return by(t);
    };
  };

  const stepsFunction = (count, position) => (x, before) => {
    let step = Math.floor(x * count + 1e-12 * 0);
    if (position === 'jump-start' || position === 'jump-both') step++;
    if (x >= 0 && step < 0) step = 0;
    let jumps = count;
    if (position === 'jump-none') jumps = count - 1;
    else if (position === 'jump-both') jumps = count + 1;
    if (x <= 1 && step > jumps) step = jumps;
    if (before && x * count === Math.floor(x * count)) step--;
    if (x >= 0 && step < 0) step = 0;
    return step / jumps;
  };

  const keywordEasings = {
    linear: { fn: (x) => x, text: 'linear' },
    ease: { fn: cubicBezier(0.25, 0.1, 0.25, 1), text: 'ease' },
    'ease-in': { fn: cubicBezier(0.42, 0, 1, 1), text: 'ease-in' },
    'ease-out': { fn: cubicBezier(0, 0, 0.58, 1), text: 'ease-out' },
    'ease-in-out': { fn: cubicBezier(0.42, 0, 0.58, 1), text: 'ease-in-out' },
    'step-start': { fn: stepsFunction(1, 'jump-start'), text: 'steps(1, start)' },
    'step-end': { fn: stepsFunction(1, 'jump-end'), text: 'steps(1)' },
  };

  const numberText = (n) => String(Math.round(n * 1e6) / 1e6);
  const NUMBER = /^[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?$/;

  const splitArguments = (text) => {
    const parts = [];
    let depth = 0, start = 0;
    for (let i = 0; i < text.length; i++) {
      if (text[i] === '(') depth++;
      else if (text[i] === ')') depth--;
      else if (text[i] === ',' && depth === 0) {
        parts.push(text.slice(start, i).trim());
        start = i + 1;
      }
    }
    parts.push(text.slice(start).trim());
    return parts;
  };

  const parseLinear = (args) => {
    if (args.length < 2) return null;
    const points = [];
    for (const arg of args) {
      const words = arg.split(/\s+/).filter(Boolean);
      if (words.length < 1 || words.length > 3 || !NUMBER.test(words[0])) return null;
      const output = parseFloat(words[0]);
      const inputs = [];
      for (const word of words.slice(1)) {
        if (!/^[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?%$/.test(word)) return null;
        inputs.push(parseFloat(word) / 100);
      }
      if (inputs.length === 0) points.push({ output, input: null });
      else for (const input of inputs) points.push({ output, input });
    }
    if (points.length < 2) return null;
    // Missing inputs: the first is 0, the last 1, and none is below one before it.
    if (points[0].input === null) points[0].input = 0;
    if (points[points.length - 1].input === null) points[points.length - 1].input = Math.max(1, points[points.length - 2].input);
    let largest = -Infinity;
    for (const point of points) {
      if (point.input !== null) {
        point.input = Math.max(point.input, largest);
        largest = point.input;
      }
    }
    for (let i = 0; i < points.length; i++) {
      if (points[i].input !== null) continue;
      let j = i;
      while (points[j].input === null) j++;
      const from = points[i - 1].input, to = points[j].input;
      for (let k = i; k < j; k++) points[k].input = from + ((to - from) * (k - i + 1)) / (j - i + 1);
      i = j;
    }
    const fn = (x) => {
      const first = points[0], last = points[points.length - 1];
      let a, b;
      if (x < first.input) {
        a = first;
        b = points.find((p) => p.input > first.input) || last;
        if (b.input === a.input) return a.output;
      } else if (x >= last.input) {
        b = last;
        a = [...points].reverse().find((p) => p.input < last.input) || first;
        if (b.input === a.input) return b.output;
        if (x === last.input) return last.output;
      } else {
        let i = 0;
        while (i < points.length - 1 && points[i + 1].input <= x) i++;
        a = points[i];
        b = points[i + 1];
        if (b.input === a.input) return b.output;
      }
      return a.output + ((b.output - a.output) * (x - a.input)) / (b.input - a.input);
    };
    const text = 'linear(' + points.map((p) => numberText(p.output) + ' ' + numberText(p.input * 100) + '%').join(', ') + ')';
    return { fn, text };
  };

  // The easing a string says: { fn(progress, beforeFlag), text }, or null if it is not one.
  S.parseEasing = (value) => {
    const text = String(value).trim().toLowerCase();
    if (Object.prototype.hasOwnProperty.call(keywordEasings, text)) return keywordEasings[text];
    const match = /^([a-z-]+)\((.*)\)$/s.exec(text);
    if (!match) return null;
    const args = splitArguments(match[2]);
    if (match[1] === 'cubic-bezier') {
      if (args.length !== 4 || !args.every((a) => NUMBER.test(a))) return null;
      const [x1, y1, x2, y2] = args.map(parseFloat);
      if (x1 < 0 || x1 > 1 || x2 < 0 || x2 > 1) return null;
      return { fn: cubicBezier(x1, y1, x2, y2), text: `cubic-bezier(${numberText(x1)}, ${numberText(y1)}, ${numberText(x2)}, ${numberText(y2)})` };
    }
    if (match[1] === 'steps') {
      if (args.length < 1 || args.length > 2 || !/^\+?\d+$/.test(args[0])) return null;
      const count = parseInt(args[0], 10);
      let position = 'jump-end';
      let written = '';
      if (args.length === 2) {
        position = args[1];
        if (!['jump-start', 'jump-end', 'jump-none', 'jump-both', 'start', 'end'].includes(position)) return null;
        written = position;
        if (position === 'start') position = 'jump-start';
        else if (position === 'end') position = 'jump-end';
      }
      if (count < 1 || (position === 'jump-none' && count < 2)) return null;
      const suffix = position === 'jump-end' ? '' : ', ' + written;
      return { fn: stepsFunction(count, position), text: `steps(${count}${suffix})` };
    }
    if (match[1] === 'linear') return parseLinear(args);
    return null;
  };

  // ---- Timing ----

  const FILLS = ['none', 'forwards', 'backwards', 'both', 'auto'];
  const DIRECTIONS = ['normal', 'reverse', 'alternate', 'alternate-reverse'];

  // A time given as a number or as a CSSNumericValue of a time (CSSNumberish), in milliseconds.
  S.toNumberish = (value) => {
    if (typeof CSSNumericValue === 'function' && value instanceof CSSNumericValue) {
      if (value instanceof CSSUnitValue && value.unit === 'number') return value.value;
      return value.to('ms').value;
    }
    return value;
  };

  const finiteNumber = (value, name) => {
    const n = Number(S.toNumberish(value));
    if (!Number.isFinite(n)) throw new TypeError(`Failed to read the '${name}' property from 'EffectTiming': The provided double value is non-finite.`);
    return n;
  };

  S.defaultTiming = () => ({ delay: 0, endDelay: 0, fill: 'auto', iterationStart: 0, iterations: 1, duration: 'auto', direction: 'normal', easing: 'linear', easingFn: keywordEasings.linear });

  const checkTiming = (timing) => {
    if (timing.iterationStart < 0) throw new TypeError("Failed to set the 'iterationStart' property on 'AnimationEffect': Iteration start must be non-negative.");
    if (Number.isNaN(timing.iterations) || timing.iterations < 0) throw new TypeError("Failed to set the 'iterations' property on 'AnimationEffect': Iterations must be non-negative.");
    if (timing.duration !== 'auto' && (Number.isNaN(timing.duration) || timing.duration < 0)) throw new TypeError("Failed to set the 'duration' property on 'AnimationEffect': Duration must be non-negative or auto.");
  };

  // Reads the members of an EffectTiming (or OptionalEffectTiming) dictionary into `into`; members are read in the order of the IDL.
  S.readTiming = (source, into) => {
    if (source === undefined || source === null) return into;
    if (typeof source !== 'object' && typeof source !== 'function') throw new TypeError('The timing is not an object.');
    const next = { ...into };
    let value = source.delay;
    if (value !== undefined) next.delay = finiteNumber(value, 'delay');
    value = source.endDelay;
    if (value !== undefined) next.endDelay = finiteNumber(value, 'endDelay');
    value = source.fill;
    if (value !== undefined) {
      value = String(value);
      if (!FILLS.includes(value)) throw new TypeError(`Failed to read the 'fill' property from 'EffectTiming': The provided value '${value}' is not a valid enum value of type FillMode.`);
      next.fill = value;
    }
    value = source.iterationStart;
    if (value !== undefined) next.iterationStart = finiteNumber(value, 'iterationStart');
    value = source.iterations;
    if (value !== undefined) {
      const n = Number(value);
      if (Number.isNaN(n)) throw new TypeError("Failed to read the 'iterations' property from 'EffectTiming': The provided double value is non-finite.");
      next.iterations = n;
    }
    value = source.duration;
    if (value !== undefined) {
      value = S.toNumberish(value);
      if (typeof value === 'string' || (typeof value === 'object' && value !== null && !(typeof value === 'number'))) {
        const text = String(value);
        if (text !== 'auto') throw new TypeError("Failed to read the 'duration' property from 'EffectTiming': duration must be a number or 'auto'.");
        next.duration = 'auto';
      } else {
        const n = Number(value);
        if (Number.isNaN(n)) throw new TypeError("Failed to read the 'duration' property from 'EffectTiming': The provided double value is non-finite.");
        next.duration = n;
      }
    }
    value = source.direction;
    if (value !== undefined) {
      value = String(value);
      if (!DIRECTIONS.includes(value)) throw new TypeError(`Failed to read the 'direction' property from 'EffectTiming': The provided value '${value}' is not a valid enum value of type PlaybackDirection.`);
      next.direction = value;
    }
    value = source.easing;
    if (value !== undefined) {
      const easing = S.parseEasing(value);
      if (!easing) throw new TypeError(`Failed to read the 'easing' property from 'EffectTiming': '${value}' is not a valid value for easing.`);
      next.easing = easing.text;
      next.easingFn = easing;
    }
    checkTiming(next);
    return next;
  };

  S.activeDuration = (timing) => {
    const duration = timing.duration === 'auto' ? 0 : timing.duration;
    if (duration === 0 || timing.iterations === 0) return 0;
    return duration * timing.iterations;
  };

  S.endTime = (timing) => Math.max(timing.delay + S.activeDuration(timing) + timing.endDelay, 0);

  // What an effect with this timing comes to at the local time `localTime` of its animation, which plays at `rate`.
  S.computeTiming = (timing, localTime, rate) => {
    const duration = timing.duration === 'auto' ? 0 : timing.duration;
    const activeDuration = S.activeDuration(timing);
    const endTime = Math.max(timing.delay + activeDuration + timing.endDelay, 0);
    const result = { phase: 'idle', activeTime: null, overallProgress: null, simpleProgress: null, currentIteration: null, directedProgress: null, progress: null, endTime, activeDuration, localTime };
    if (localTime === null || localTime === undefined) return result;
    const backwards = rate < 0;
    const beforeBoundary = Math.max(Math.min(timing.delay, endTime), 0);
    const afterBoundary = Math.max(Math.min(timing.delay + activeDuration, endTime), 0);
    let phase;
    if (localTime < beforeBoundary || (backwards && localTime === beforeBoundary)) phase = 'before';
    else if (localTime > afterBoundary || (!backwards && localTime === afterBoundary)) phase = 'after';
    else phase = 'active';
    result.phase = phase;
    const fill = timing.fill === 'auto' ? 'none' : timing.fill;
    let activeTime = null;
    if (phase === 'before') {
      if (fill === 'backwards' || fill === 'both') activeTime = Math.max(localTime - timing.delay, 0);
    } else if (phase === 'active') {
      activeTime = localTime - timing.delay;
    } else if (fill === 'forwards' || fill === 'both') {
      activeTime = Math.max(Math.min(localTime - timing.delay, activeDuration), 0);
    }
    result.activeTime = activeTime;
    if (activeTime === null) return result;
    let overall;
    if (duration === 0) overall = phase === 'before' ? 0 : timing.iterations;
    else overall = activeTime / duration;
    overall += timing.iterationStart;
    result.overallProgress = overall;
    let simple = Number.isFinite(overall) ? overall % 1 : timing.iterationStart % 1;
    if (simple === 0 && (phase === 'active' || phase === 'after') && activeTime === activeDuration && timing.iterations !== 0) simple = 1;
    result.simpleProgress = simple;
    let iteration;
    if (phase === 'after' && timing.iterations === Infinity) iteration = Infinity;
    else if (simple === 1) iteration = Math.ceil(overall) - 1;
    else iteration = Math.floor(overall);
    result.currentIteration = iteration;
    let reversed = false;
    const direction = timing.direction;
    if (direction === 'reverse') reversed = true;
    else if (direction === 'alternate' || direction === 'alternate-reverse') {
      const odd = Number.isFinite(iteration) ? iteration % 2 === 1 : false;
      reversed = direction === 'alternate' ? odd : !odd;
    }
    const directed = reversed ? 1 - simple : simple;
    result.directedProgress = directed;
    // Going forwards: the direction of the iteration and of the animation together.
    const goingForwards = (rate >= 0) !== reversed;
    result.progress = timing.easingFn.fn(directed, (phase === 'before' && goingForwards) || (phase === 'after' && !goingForwards));
    return result;
  };

  S.numberText = numberText;
})();
