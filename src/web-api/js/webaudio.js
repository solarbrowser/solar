// Web Audio (https://webaudio.github.io/web-audio-api/): the interfaces are written here; the graph and the mixing are the program's (src/media/AudioGraph.cpp), which the
// one native __solarAudio(op, context, ...) of src/web-api/WebAudio.cpp holds it by. The engine makes the sound on a thread of its own; what is of the page's thread is the
// callbacks: a ScriptProcessorNode's onaudioprocess and a source's onended, which are polled for here.
(() => {
  "use strict";
  const audioN = globalThis.__solarAudio, decodeN = globalThis.__solarAudioDecode;
  delete globalThis.__solarAudio;
  delete globalThis.__solarAudioDecode;
  if (typeof audioN !== "function" || typeof EventTarget !== "function") return;

  const OP = { New: 0, Close: 1, Start: 2, Suspend: 3, Time: 4, Node: 5, Delete: 6, Connect: 7, ConnectParam: 8, ConnectDest: 9, Disconnect: 10, SetParam: 11, GetParam: 12, Set: 13, StartSource: 14, StopSource: 15, Poll: 16, ProcIn: 17, ProcOut: 18, ProcDone: 19, AnTime: 20, AnFreq: 21, SetBuffer: 22, Render: 23 };
  const KIND = { Gain: 0, BufferSource: 1, Oscillator: 2, ScriptProcessor: 3, StereoPanner: 4, Biquad: 5, Delay: 6, Analyser: 7, Constant: 8, Splitter: 9, Merger: 10, Compressor: 11 };
  const PARAM_OP = { Set: 0, SetAt: 1, Linear: 2, Exp: 3, Target: 4, Cancel: 5, CancelHold: 6 };
  const SETTING = { Wave: 0, Filter: 1, Loop: 2, LoopStart: 3, LoopEnd: 4, Fft: 5, Smoothing: 6, Channels: 7 };
  const DESTINATION = 4294967295;
  const PRIVATE = Symbol("private"), kStart = Symbol("start");

  const fail = (name, message) => new DOMException(message, name);
  const typeError = (message) => new TypeError(message);
  const num = (v, what) => { const n = +v; if (!Number.isFinite(n)) throw typeError(`The provided float value for '${what}' is non-finite.`); return n; };
  const hidden = new WeakMap();  // node, param, buffer -> its private state
  const define = (target, name, descriptor) => Object.defineProperty(target, name, { configurable: true, enumerable: true, ...descriptor });
  const tag = (cls, name) => Object.defineProperty(cls.prototype, Symbol.toStringTag, { value: name, configurable: true });
  const handlerOf = (cls, type) => define(cls.prototype, "on" + type, {
    get() { return (hidden.get(this).handlers ||= {})[type] || null; },
    set(fn) {
      const h = (hidden.get(this).handlers ||= {});
      if (h[type]) this.removeEventListener(type, h[type].wrapper);
      h[type] = null;
      if (typeof fn === "function") {
        const wrapper = (e) => fn.call(this, e);
        h[type] = { fn, wrapper };
        this.addEventListener(type, wrapper);
      }
    },
  });
  // (The getter hands back the function that was set.)
  const fixHandlers = (cls, types) => {
    for (const type of types) {
      const d = Object.getOwnPropertyDescriptor(cls.prototype, "on" + type);
      define(cls.prototype, "on" + type, { get() { const h = (hidden.get(this).handlers || {})[type]; return h ? h.fn : null; }, set: d.set });
    }
  };

  // ---- AudioBuffer ----
  class AudioBuffer {
    constructor(options) {
      if (arguments.length === 0 || options === null || typeof options !== "object") throw typeError("Failed to construct 'AudioBuffer': 1 argument required, but only 0 present.");
      const channels = options.numberOfChannels === undefined ? 1 : options.numberOfChannels >>> 0, length = options.length >>> 0, rate = +options.sampleRate;
      AudioBuffer.check(channels, length, rate, "construct");
      const data = [];
      for (let c = 0; c < channels; ++c) data.push(new Float32Array(length));
      hidden.set(this, { channels: data, length, rate });
    }
    static check(channels, length, rate, verb) {
      if (channels < 1 || channels > 32) throw fail("NotSupportedError", `Failed to ${verb} 'AudioBuffer': The number of channels provided (${channels}) is outside the range [1, 32].`);
      if (length < 1) throw fail("NotSupportedError", `Failed to ${verb} 'AudioBuffer': The number of frames provided (${length}) is less than or equal to the minimum bound (0).`);
      if (!(rate >= 3000 && rate <= 768000)) throw fail("NotSupportedError", `Failed to ${verb} 'AudioBuffer': The sample rate provided (${rate}) is outside the range [3000, 768000].`);
    }
    static make(data, rate) { const b = Object.create(AudioBuffer.prototype); hidden.set(b, { channels: data, length: data[0].length, rate }); return b; }
    get sampleRate() { return hidden.get(this).rate; }
    get length() { return hidden.get(this).length; }
    get duration() { const s = hidden.get(this); return s.length / s.rate; }
    get numberOfChannels() { return hidden.get(this).channels.length; }
    getChannelData(channel) {
      const s = hidden.get(this);
      if (!(channel >= 0 && channel < s.channels.length)) throw fail("IndexSizeError", "Failed to execute 'getChannelData' on 'AudioBuffer': channel index (" + channel + ") exceeds number of channels (" + s.channels.length + ")");
      return s.channels[channel];
    }
    copyFromChannel(destination, channel, start = 0) {
      const s = hidden.get(this);
      if (!(channel >= 0 && channel < s.channels.length)) throw fail("IndexSizeError", "Failed to execute 'copyFromChannel' on 'AudioBuffer': channelNumber provided (" + channel + ") is outside the range [0, " + s.channels.length + ")");
      start >>>= 0;
      if (start >= s.length) return;
      destination.set(s.channels[channel].subarray(start, start + destination.length));
    }
    copyToChannel(source, channel, start = 0) {
      const s = hidden.get(this);
      if (!(channel >= 0 && channel < s.channels.length)) throw fail("IndexSizeError", "Failed to execute 'copyToChannel' on 'AudioBuffer': channelNumber provided (" + channel + ") is outside the range [0, " + s.channels.length + ")");
      start >>>= 0;
      if (start >= s.length) return;
      s.channels[channel].set(source.subarray(0, s.length - start), start);
    }
  }
  tag(AudioBuffer, "AudioBuffer");

  // ---- AudioParam ----
  class AudioParam {
    constructor() { throw typeError("Illegal constructor"); }
    static make(node, index, name, value, min, max, rate) {
      const p = Object.create(AudioParam.prototype);
      hidden.set(p, { node, index, name, def: value, min, max, rate });
      return p;
    }
    get value() { const s = hidden.get(this); const ctx = hidden.get(s.node).ctx; return audioN(OP.GetParam, cid(ctx), hidden.get(s.node).id, s.index); }
    set value(v) { this.#op(PARAM_OP.Set, num(v, "value"), hidden.get(hidden.get(this).node).ctx.currentTime); }
    get defaultValue() { return hidden.get(this).def; }
    get minValue() { return hidden.get(this).min; }
    get maxValue() { return hidden.get(this).max; }
    get automationRate() { return hidden.get(this).rate; }
    set automationRate(v) { if (v === "a-rate" || v === "k-rate") hidden.get(this).rate = v; }
    #op(op, value, time, extra = 0) {
      const s = hidden.get(this), n = hidden.get(s.node);
      audioN(OP.SetParam, cid(n.ctx), n.id, s.index, op, value, time, extra);
      return this;
    }
    setValueAtTime(value, time) { value = num(value, "value"); time = num(time, "startTime"); if (time < 0) throw new RangeError("Failed to execute 'setValueAtTime' on 'AudioParam': The startTime provided (" + time + ") is less than the minimum bound (0)."); return this.#op(PARAM_OP.SetAt, value, time); }
    linearRampToValueAtTime(value, time) { value = num(value, "value"); time = num(time, "endTime"); if (time < 0) throw new RangeError("Failed to execute 'linearRampToValueAtTime' on 'AudioParam': The endTime provided (" + time + ") is less than the minimum bound (0)."); return this.#op(PARAM_OP.Linear, value, time); }
    exponentialRampToValueAtTime(value, time) {
      value = num(value, "value"); time = num(time, "endTime");
      if (value === 0) throw new RangeError("Failed to execute 'exponentialRampToValueAtTime' on 'AudioParam': The float target value provided (0) should not be in the range (-1.17549e-38, 1.17549e-38).");
      if (time < 0) throw new RangeError("Failed to execute 'exponentialRampToValueAtTime' on 'AudioParam': The endTime provided (" + time + ") is less than the minimum bound (0).");
      return this.#op(PARAM_OP.Exp, value, time);
    }
    setTargetAtTime(target, time, constant) { target = num(target, "target"); time = num(time, "startTime"); constant = num(constant, "timeConstant"); if (time < 0) throw new RangeError("Failed to execute 'setTargetAtTime' on 'AudioParam': The startTime provided (" + time + ") is less than the minimum bound (0)."); return this.#op(PARAM_OP.Target, target, time, constant); }
    setValueCurveAtTime(values, time, duration) {
      time = num(time, "startTime"); duration = num(duration, "duration");
      if (values.length < 2) throw fail("InvalidStateError", "Failed to execute 'setValueCurveAtTime' on 'AudioParam': The curve length provided (" + values.length + ") is less than the minimum bound (2).");
      if (!(duration > 0)) throw new RangeError("Failed to execute 'setValueCurveAtTime' on 'AudioParam': The duration provided (" + duration + ") is less than or equal to the minimum bound (0).");
      const n = values.length;
      this.#op(PARAM_OP.SetAt, values[0], time);
      for (let i = 1; i < n; ++i) this.#op(PARAM_OP.Linear, values[i], time + duration * i / (n - 1));
      return this.#op(PARAM_OP.SetAt, values[n - 1], time + duration);
    }
    cancelScheduledValues(time) { return this.#op(PARAM_OP.Cancel, 0, num(time, "cancelTime")); }
    cancelAndHoldAtTime(time) { return this.#op(PARAM_OP.CancelHold, 0, num(time, "cancelTime")); }
  }
  tag(AudioParam, "AudioParam");

  // ---- AudioNode ----
  const cid = (ctx) => hidden.get(ctx).id;
  const nodeState = (node) => hidden.get(node);
  class AudioNode extends EventTarget {
    constructor(token) { if (token !== PRIVATE) throw typeError("Illegal constructor"); super(); }
    static init(node, ctx, kind, a, b, c, extra = {}) {
      const id = audioN(OP.Node, cid(ctx), kind, a || 0, b || 0, c || 0);
      hidden.set(node, { ctx, id, kind, mode: "max", count: 2, interpretation: "speakers", handlers: null, params: {}, ...extra });
      hidden.get(ctx).nodes.set(id, node);
      return node;
    }
    static param(node, index, name, def, min, max) {
      hidden.get(node).params[name] = AudioParam.make(node, index, name, def, min, max, "a-rate");
      return hidden.get(node).params[name];
    }
    get context() { return nodeState(this).ctx; }
    get numberOfInputs() { return nodeState(this).inputs ?? 1; }
    get numberOfOutputs() { return nodeState(this).outputs ?? 1; }
    get channelCount() { return nodeState(this).count; }
    set channelCount(v) {
      v >>>= 0;
      if (v < 1 || v > 32) throw fail("NotSupportedError", "Failed to set the 'channelCount' property on 'AudioNode': The channel count provided (" + v + ") is outside the range [1, 32].");
      const s = nodeState(this);
      s.count = v;
      if (s.mode === "explicit") audioN(OP.Set, cid(s.ctx), s.id, SETTING.Channels, Math.min(v, 8));
    }
    get channelCountMode() { return nodeState(this).mode; }
    set channelCountMode(v) {
      if (v !== "max" && v !== "clamped-max" && v !== "explicit") return;
      const s = nodeState(this);
      s.mode = v;
      audioN(OP.Set, cid(s.ctx), s.id, SETTING.Channels, v === "explicit" ? Math.min(s.count, 8) : 0);
    }
    get channelInterpretation() { return nodeState(this).interpretation; }
    set channelInterpretation(v) { if (v === "speakers" || v === "discrete") nodeState(this).interpretation = v; }
    connect(destination, output = 0, input = 0) {
      const s = nodeState(this);
      if (destination instanceof AudioParam) {
        const p = hidden.get(destination), pn = hidden.get(p.node);
        if (pn.ctx !== s.ctx) throw fail("InvalidAccessError", "Failed to execute 'connect' on 'AudioNode': cannot connect to an AudioParam belonging to a different BaseAudioContext.");
        audioN(OP.ConnectParam, cid(s.ctx), s.id, pn.id, p.index);
        return undefined;
      }
      if (!(destination instanceof AudioNode)) throw typeError("Failed to execute 'connect' on 'AudioNode': Overload resolution failed.");
      const d = nodeState(destination);
      if (d.ctx !== s.ctx) throw fail("InvalidAccessError", "Failed to execute 'connect' on 'AudioNode': cannot connect to a destination belonging to a different audio context.");
      output >>>= 0; input >>>= 0;
      if (output >= (s.outputs ?? 1)) throw fail("IndexSizeError", "Failed to execute 'connect' on 'AudioNode': output index (" + output + ") exceeds number of outputs (" + (s.outputs ?? 1) + ").");
      if (input >= (d.inputs ?? 1)) throw fail("IndexSizeError", "Failed to execute 'connect' on 'AudioNode': input index (" + input + ") exceeds number of inputs (" + (d.inputs ?? 1) + ").");
      if (d.kind === "destination") audioN(OP.ConnectDest, cid(s.ctx), s.id);
      else audioN(OP.Connect, cid(s.ctx), s.id, d.id, output, input);
      return destination;
    }
    disconnect(destination) {
      const s = nodeState(this);
      if (destination === undefined) audioN(OP.Disconnect, cid(s.ctx), s.id, 0);
      else if (destination instanceof AudioParam) audioN(OP.Disconnect, cid(s.ctx), s.id, hidden.get(hidden.get(destination).node).id);
      else if (destination instanceof AudioNode) {
        const d = nodeState(destination);
        audioN(OP.Disconnect, cid(s.ctx), s.id, d.kind === "destination" ? DESTINATION : d.id);
      }
    }
  }
  tag(AudioNode, "AudioNode");
  const makeParams = (node, list) => { for (const [index, name, def, min, max] of list) { const p = AudioNode.param(node, index, name, def, min, max); define(Object.getPrototypeOf(node), name, { get() { return hidden.get(this).params[name]; } }); } };
  const F32 = 3.4028234663852886e38;

  // Each node class has a constructor that is its context's `create...` too; the options of the constructors are read by `apply`.
  const assign = (node, options, names) => { if (options) for (const n of names) if (options[n] !== undefined) node[n] = options[n]; };
  const paramsOf = (node, options, names) => { if (options) for (const n of names) if (options[n] !== undefined) node[n].value = options[n]; };
  const contextOf = (ctx, name) => { if (!(ctx instanceof BaseAudioContext)) throw typeError("Failed to construct '" + name + "': parameter 1 is not of type 'BaseAudioContext'."); return ctx; };

  class AudioDestinationNode extends AudioNode {
    constructor(token) { super(token); }
    get maxChannelCount() { return 8; }
  }
  tag(AudioDestinationNode, "AudioDestinationNode");
  AudioDestinationNode.make = (ctx, channels) => {
    const n = new AudioDestinationNode(PRIVATE);
    hidden.set(n, { ctx, id: 0, kind: "destination", mode: "explicit", count: channels, interpretation: "speakers", handlers: null, params: {}, outputs: 0 });
    return n;
  };

  class GainNode extends AudioNode {
    constructor(ctx, options) { super(PRIVATE); contextOf(ctx, "GainNode"); AudioNode.init(this, ctx, KIND.Gain); makeParams(this, [[0, "gain", 1, -F32, F32]]); paramsOf(this, options, ["gain"]); }
  }
  tag(GainNode, "GainNode");

  class AudioScheduledSourceNode extends AudioNode {
    constructor(token) { super(token); }
    start(when = 0, ...rest) {
      const s = nodeState(this);
      when = num(when, "when");
      if (when < 0) throw new RangeError("Failed to execute 'start' on 'AudioScheduledSourceNode': The start time provided (" + when + ") is less than the minimum bound (0).");
      if (s.started) throw fail("InvalidStateError", "Failed to execute 'start' on 'AudioScheduledSourceNode': cannot call start more than once.");
      s.started = true;
      this[kStart](s, when, ...rest);
    }
    stop(when = 0) {
      const s = nodeState(this);
      if (!s.started) throw fail("InvalidStateError", "Failed to execute 'stop' on 'AudioScheduledSourceNode': cannot call stop without calling start first.");
      audioN(OP.StopSource, cid(s.ctx), s.id, num(when, "when"));
    }
  }
  tag(AudioScheduledSourceNode, "AudioScheduledSourceNode");
  fixHandlers((handlerOf(AudioScheduledSourceNode, "ended"), AudioScheduledSourceNode), ["ended"]);

  class OscillatorNode extends AudioScheduledSourceNode {
    constructor(ctx, options) { super(PRIVATE); contextOf(ctx, "OscillatorNode"); AudioNode.init(this, ctx, KIND.Oscillator, 0, 0, 0, { inputs: 0, wave: "sine" }); makeParams(this, [[0, "frequency", 440, -24000, 24000], [1, "detune", 0, -153600, 153600]]); paramsOf(this, options, ["frequency", "detune"]); assign(this, options, ["type"]); }
    [kStart](s, when) { audioN(OP.StartSource, cid(s.ctx), s.id, when, 0, -1); }
    get type() { return nodeState(this).wave; }
    set type(v) {
      const i = ["sine", "square", "sawtooth", "triangle"].indexOf(v);
      if (i < 0) { if (v === "custom") throw fail("InvalidStateError", "Failed to set the 'type' property on 'OscillatorNode': 'type' cannot be set directly to 'custom'.  The PeriodicWave must be set explicitly."); return; }
      const s = nodeState(this); s.wave = v; audioN(OP.Set, cid(s.ctx), s.id, SETTING.Wave, i);
    }
    setPeriodicWave() {}
  }
  tag(OscillatorNode, "OscillatorNode");

  class ConstantSourceNode extends AudioScheduledSourceNode {
    constructor(ctx, options) { super(PRIVATE); contextOf(ctx, "ConstantSourceNode"); AudioNode.init(this, ctx, KIND.Constant, 0, 0, 0, { inputs: 0 }); makeParams(this, [[0, "offset", 1, -F32, F32]]); paramsOf(this, options, ["offset"]); }
    [kStart](s, when) { audioN(OP.StartSource, cid(s.ctx), s.id, when, 0, -1); }
  }
  tag(ConstantSourceNode, "ConstantSourceNode");

  class AudioBufferSourceNode extends AudioScheduledSourceNode {
    constructor(ctx, options) { super(PRIVATE); contextOf(ctx, "AudioBufferSourceNode"); AudioNode.init(this, ctx, KIND.BufferSource, 0, 0, 0, { inputs: 0, buffer: null, loop: false, loopStart: 0, loopEnd: 0 }); makeParams(this, [[0, "playbackRate", 1, -F32, F32], [1, "detune", 0, -F32, F32]]); assign(this, options, ["buffer", "loop", "loopStart", "loopEnd"]); paramsOf(this, options, ["playbackRate", "detune"]); }
    [kStart](s, when, offset = 0, duration) {
      // (The buffer is taken as it is when the sound starts.)
      const b = s.buffer && hidden.get(s.buffer);
      if (b) audioN(OP.SetBuffer, cid(s.ctx), s.id, b.rate, b.length, b.channels);
      audioN(OP.StartSource, cid(s.ctx), s.id, when, num(offset, "offset"), duration === undefined ? -1 : num(duration, "duration"));
    }
    get buffer() { return nodeState(this).buffer; }
    set buffer(v) {
      if (v !== null && !(v instanceof AudioBuffer)) throw typeError("Failed to set the 'buffer' property on 'AudioBufferSourceNode': Failed to convert value to 'AudioBuffer'.");
      const s = nodeState(this);
      if (v && s.buffer) throw fail("InvalidStateError", "Failed to set the 'buffer' property on 'AudioBufferSourceNode': Cannot set buffer to non-null after it has been already been set to a non-null buffer");
      s.buffer = v;
      if (s.started && v) { const b = hidden.get(v); audioN(OP.SetBuffer, cid(s.ctx), s.id, b.rate, b.length, b.channels); }
    }
    get loop() { return nodeState(this).loop; }
    set loop(v) { const s = nodeState(this); s.loop = !!v; audioN(OP.Set, cid(s.ctx), s.id, SETTING.Loop, v ? 1 : 0); }
    get loopStart() { return nodeState(this).loopStart; }
    set loopStart(v) { const s = nodeState(this); s.loopStart = +v; audioN(OP.Set, cid(s.ctx), s.id, SETTING.LoopStart, +v); }
    get loopEnd() { return nodeState(this).loopEnd; }
    set loopEnd(v) { const s = nodeState(this); s.loopEnd = +v; audioN(OP.Set, cid(s.ctx), s.id, SETTING.LoopEnd, +v); }
  }
  tag(AudioBufferSourceNode, "AudioBufferSourceNode");

  class StereoPannerNode extends AudioNode {
    constructor(ctx, options) { super(PRIVATE); contextOf(ctx, "StereoPannerNode"); AudioNode.init(this, ctx, KIND.StereoPanner); makeParams(this, [[0, "pan", 0, -1, 1]]); paramsOf(this, options, ["pan"]); }
  }
  tag(StereoPannerNode, "StereoPannerNode");

  class BiquadFilterNode extends AudioNode {
    constructor(ctx, options) { super(PRIVATE); contextOf(ctx, "BiquadFilterNode"); AudioNode.init(this, ctx, KIND.Biquad, 0, 0, 0, { filter: "lowpass" }); makeParams(this, [[0, "frequency", 350, 0, 24000], [1, "detune", 0, -153600, 153600], [2, "Q", 1, -F32, F32], [3, "gain", 0, -F32, 1541]]); paramsOf(this, options, ["frequency", "detune", "Q", "gain"]); assign(this, options, ["type"]); }
    get type() { return nodeState(this).filter; }
    set type(v) { const i = ["lowpass", "highpass", "bandpass", "lowshelf", "highshelf", "peaking", "notch", "allpass"].indexOf(v); if (i < 0) return; const s = nodeState(this); s.filter = v; audioN(OP.Set, cid(s.ctx), s.id, SETTING.Filter, i); }
    getFrequencyResponse(frequencies, magnitudes, phases) { for (let i = 0; i < frequencies.length; ++i) { magnitudes[i] = 1; phases[i] = 0; } }
  }
  tag(BiquadFilterNode, "BiquadFilterNode");

  class DelayNode extends AudioNode {
    constructor(ctx, options) { super(PRIVATE); contextOf(ctx, "DelayNode"); const max = options && options.maxDelayTime !== undefined ? +options.maxDelayTime : 1; if (!(max > 0 && max < 180)) throw fail("NotSupportedError", "Failed to construct 'DelayNode': The max delay time provided (" + max + ") is outside the range (0, 180)."); AudioNode.init(this, ctx, KIND.Delay, Math.ceil(max * 1000)); makeParams(this, [[0, "delayTime", 0, 0, max]]); paramsOf(this, options, ["delayTime"]); }
  }
  tag(DelayNode, "DelayNode");

  class DynamicsCompressorNode extends AudioNode {
    constructor(ctx, options) { super(PRIVATE); contextOf(ctx, "DynamicsCompressorNode"); AudioNode.init(this, ctx, KIND.Compressor); makeParams(this, [[0, "threshold", -24, -100, 0], [1, "knee", 30, 0, 40], [2, "ratio", 12, 1, 20], [3, "attack", 0.003, 0, 1], [4, "release", 0.25, 0, 1]]); paramsOf(this, options, ["threshold", "knee", "ratio", "attack", "release"]); }
    get reduction() { return 0; }
  }
  tag(DynamicsCompressorNode, "DynamicsCompressorNode");

  class ChannelSplitterNode extends AudioNode {
    constructor(ctx, options) { super(PRIVATE); contextOf(ctx, "ChannelSplitterNode"); const n = options && options.numberOfOutputs !== undefined ? options.numberOfOutputs >>> 0 : 6; if (n < 1 || n > 32) throw fail("IndexSizeError", "Failed to construct 'ChannelSplitterNode': Number of outputs (" + n + ") must be between 1 and 32."); AudioNode.init(this, ctx, KIND.Splitter, Math.min(n, 8), 0, 0, { outputs: n, mode: "explicit", count: n }); }
  }
  tag(ChannelSplitterNode, "ChannelSplitterNode");
  class ChannelMergerNode extends AudioNode {
    constructor(ctx, options) { super(PRIVATE); contextOf(ctx, "ChannelMergerNode"); const n = options && options.numberOfInputs !== undefined ? options.numberOfInputs >>> 0 : 6; if (n < 1 || n > 32) throw fail("IndexSizeError", "Failed to construct 'ChannelMergerNode': Number of inputs (" + n + ") must be between 1 and 32."); AudioNode.init(this, ctx, KIND.Merger, Math.min(n, 8), 0, 0, { inputs: n, mode: "explicit", count: 1 }); }
  }
  tag(ChannelMergerNode, "ChannelMergerNode");

  class AnalyserNode extends AudioNode {
    constructor(ctx, options) { super(PRIVATE); contextOf(ctx, "AnalyserNode"); AudioNode.init(this, ctx, KIND.Analyser, 0, 0, 0, { fft: 2048, min: -100, max: -30, smoothing: 0.8 }); assign(this, options, ["fftSize", "minDecibels", "maxDecibels", "smoothingTimeConstant"]); }
    get fftSize() { return nodeState(this).fft; }
    set fftSize(v) { v >>>= 0; if (v < 32 || v > 32768 || (v & (v - 1))) throw fail("IndexSizeError", "Failed to set the 'fftSize' property on 'AnalyserNode': The FFT size provided (" + v + ") is not a power of two in the range 32 to 32768."); const s = nodeState(this); s.fft = v; audioN(OP.Set, cid(s.ctx), s.id, SETTING.Fft, v); }
    get frequencyBinCount() { return nodeState(this).fft / 2; }
    get minDecibels() { return nodeState(this).min; }
    set minDecibels(v) { v = +v; const s = nodeState(this); if (v >= s.max) throw fail("IndexSizeError", "Failed to set the 'minDecibels' property on 'AnalyserNode': The minDecibels provided (" + v + ") is greater than or equal to the maximum bound (" + s.max + ")."); s.min = v; }
    get maxDecibels() { return nodeState(this).max; }
    set maxDecibels(v) { v = +v; const s = nodeState(this); if (v <= s.min) throw fail("IndexSizeError", "Failed to set the 'maxDecibels' property on 'AnalyserNode': The maxDecibels provided (" + v + ") is less than or equal to the minimum bound (" + s.min + ")."); s.max = v; }
    get smoothingTimeConstant() { return nodeState(this).smoothing; }
    set smoothingTimeConstant(v) { v = +v; if (!(v >= 0 && v <= 1)) throw fail("IndexSizeError", "Failed to set the 'smoothingTimeConstant' property on 'AnalyserNode': The smoothing value provided (" + v + ") is outside the range [0, 1]."); const s = nodeState(this); s.smoothing = v; audioN(OP.Set, cid(s.ctx), s.id, SETTING.Smoothing, v); }
    getFloatTimeDomainData(array) { const s = nodeState(this); audioN(OP.AnTime, cid(s.ctx), s.id, array.subarray(0, s.fft)); }
    getByteTimeDomainData(array) {
      const s = nodeState(this), tmp = new Float32Array(Math.min(array.length, s.fft));
      audioN(OP.AnTime, cid(s.ctx), s.id, tmp);
      for (let i = 0; i < tmp.length; ++i) array[i] = Math.max(0, Math.min(255, Math.round(128 * (1 + tmp[i]))));
    }
    getFloatFrequencyData(array) { const s = nodeState(this); audioN(OP.AnFreq, cid(s.ctx), s.id, array.subarray(0, s.fft / 2)); }
    getByteFrequencyData(array) {
      const s = nodeState(this), tmp = new Float32Array(Math.min(array.length, s.fft / 2));
      audioN(OP.AnFreq, cid(s.ctx), s.id, tmp);
      const range = s.max - s.min;
      for (let i = 0; i < tmp.length; ++i) array[i] = Math.max(0, Math.min(255, Math.floor(255 * (tmp[i] - s.min) / range)));
    }
  }
  tag(AnalyserNode, "AnalyserNode");

  class AudioProcessingEvent extends Event {
    #input; #output; #time;
    constructor(type, init) { super(type); this.#input = init.inputBuffer; this.#output = init.outputBuffer; this.#time = init.playbackTime; }
    get inputBuffer() { return this.#input; }
    get outputBuffer() { return this.#output; }
    get playbackTime() { return this.#time; }
  }
  tag(AudioProcessingEvent, "AudioProcessingEvent");

  class ScriptProcessorNode extends AudioNode {
    constructor(token) { super(token); }
    get bufferSize() { return nodeState(this).bufferSize; }
  }
  tag(ScriptProcessorNode, "ScriptProcessorNode");
  fixHandlers((handlerOf(ScriptProcessorNode, "audioprocess"), ScriptProcessorNode), ["audioprocess"]);

  // Nodes the engine does not mix yet are a gain of one: the sound passes (a panner is not placed, a shaper does not shape, a convolver has no room).
  class PannerNode extends AudioNode {
    constructor(ctx) {
      super(PRIVATE); contextOf(ctx, "PannerNode"); AudioNode.init(this, ctx, KIND.Gain, 0, 0, 0, { count: 2, mode: "clamped-max" });
      for (const name of ["positionX", "positionY", "positionZ", "orientationX", "orientationY", "orientationZ"]) hidden.get(this).params[name] = AudioParam.make(this, -1, name, 0, -F32, F32, "a-rate");
    }
    setPosition() {} setOrientation() {}
  }
  for (const name of ["positionX", "positionY", "positionZ", "orientationX", "orientationY", "orientationZ"]) define(PannerNode.prototype, name, { get() { return hidden.get(this).params[name]; } });
  Object.assign(PannerNode.prototype, { panningModel: "equalpower", distanceModel: "inverse", refDistance: 1, maxDistance: 10000, rolloffFactor: 1, coneInnerAngle: 360, coneOuterAngle: 360, coneOuterGain: 0 });
  tag(PannerNode, "PannerNode");
  class WaveShaperNode extends AudioNode { constructor(ctx) { super(PRIVATE); contextOf(ctx, "WaveShaperNode"); AudioNode.init(this, ctx, KIND.Gain); this.curve = null; this.oversample = "none"; } }
  tag(WaveShaperNode, "WaveShaperNode");
  class ConvolverNode extends AudioNode { constructor(ctx) { super(PRIVATE); contextOf(ctx, "ConvolverNode"); AudioNode.init(this, ctx, KIND.Gain); this.buffer = null; this.normalize = true; } }
  tag(ConvolverNode, "ConvolverNode");
  class MediaElementAudioSourceNode extends AudioNode { constructor(ctx) { super(PRIVATE); AudioNode.init(this, ctx, KIND.Gain, 0, 0, 0, { inputs: 0 }); } }
  tag(MediaElementAudioSourceNode, "MediaElementAudioSourceNode");
  class AudioListener {
    constructor() { throw typeError("Illegal constructor"); }
    setPosition() {} setOrientation() {}
  }
  tag(AudioListener, "AudioListener");

  // ---- The contexts ----
  class BaseAudioContext extends EventTarget {
    constructor(token) { if (token !== PRIVATE) throw typeError("Illegal constructor"); super(); }
    get destination() { return hidden.get(this).destination; }
    get sampleRate() { return hidden.get(this).rate; }
    get currentTime() { const s = hidden.get(this); return s.closed ? s.lastTime : (s.lastTime = audioN(OP.Time, s.id)); }
    get listener() { return hidden.get(this).listener ||= Object.create(AudioListener.prototype); }
    get state() { return hidden.get(this).state; }
    createBuffer(channels, length, rate) {
      if (arguments.length < 3) throw typeError("Failed to execute 'createBuffer' on 'BaseAudioContext': 3 arguments required, but only " + arguments.length + " present.");
      channels >>>= 0; length >>>= 0; rate = +rate;
      AudioBuffer.check(channels, length, rate, "execute 'createBuffer' on 'BaseAudioContext'");
      return new AudioBuffer({ numberOfChannels: channels, length, sampleRate: rate });
    }
    createGain() { return new GainNode(this); }
    createOscillator() { return new OscillatorNode(this); }
    createConstantSource() { return new ConstantSourceNode(this); }
    createBufferSource() { return new AudioBufferSourceNode(this); }
    createStereoPanner() { return new StereoPannerNode(this); }
    createBiquadFilter() { return new BiquadFilterNode(this); }
    createDelay(max = 1) { return new DelayNode(this, { maxDelayTime: max }); }
    createDynamicsCompressor() { return new DynamicsCompressorNode(this); }
    createAnalyser() { return new AnalyserNode(this); }
    createChannelSplitter(n = 6) { return new ChannelSplitterNode(this, { numberOfOutputs: n }); }
    createChannelMerger(n = 6) { return new ChannelMergerNode(this, { numberOfInputs: n }); }
    createPanner() { return new PannerNode(this); }
    createWaveShaper() { return new WaveShaperNode(this); }
    createConvolver() { return new ConvolverNode(this); }
    createMediaElementSource() { return new MediaElementAudioSourceNode(this); }
    createPeriodicWave() { return {}; }
    createScriptProcessor(bufferSize = 4096, inputs = 2, outputs = 2) {
      bufferSize = bufferSize >>> 0; inputs >>>= 0; outputs >>>= 0;
      if (bufferSize === 0) bufferSize = 4096;
      if (![256, 512, 1024, 2048, 4096, 8192, 16384].includes(bufferSize)) throw fail("IndexSizeError", "Failed to execute 'createScriptProcessor' on 'BaseAudioContext': buffer size (" + bufferSize + ") must be 0 or a power of two between 256 and 16384.");
      if (inputs > 32 || outputs > 32 || (inputs === 0 && outputs === 0)) throw fail("IndexSizeError", "Failed to execute 'createScriptProcessor' on 'BaseAudioContext': number of input channels (" + inputs + ") and output channels (" + outputs + ") must be between 0 and 32, and not both 0.");
      const node = new ScriptProcessorNode(PRIVATE);
      AudioNode.init(node, this, KIND.ScriptProcessor, bufferSize, Math.max(1, inputs), Math.max(1, outputs), { bufferSize, count: Math.max(1, inputs), mode: "explicit", inChannels: Math.max(1, inputs), outChannels: Math.max(1, outputs) });
      return node;
    }
    decodeAudioData(data, success, failure) {
      const promise = new Promise((resolve, reject) => {
        if (!(data instanceof ArrayBuffer) && !ArrayBuffer.isView(data)) { reject(typeError("Failed to execute 'decodeAudioData' on 'BaseAudioContext': parameter 1 is not of type 'ArrayBuffer'.")); return; }
        const bytes = data instanceof ArrayBuffer ? new Uint8Array(data.slice(0)) : new Uint8Array(data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength));
        const rate = hidden.get(this).rate;
        Promise.resolve().then(() => decode(bytes, rate)).then(resolve, () => reject(fail("EncodingError", "Unable to decode audio data")));
      });
      promise.then((buffer) => { if (success) success(buffer); }, (error) => { if (failure) failure(error); });
      return promise;
    }
  }
  tag(BaseAudioContext, "BaseAudioContext");
  fixHandlers((handlerOf(BaseAudioContext, "statechange"), BaseAudioContext), ["statechange"]);

  // WAV (PCM 8/16/24/32 bits and float), the one format that is read here; the rest is the decoder's.
  function decodeWav(bytes) {
    const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const tag4 = (o) => String.fromCharCode(bytes[o], bytes[o + 1], bytes[o + 2], bytes[o + 3]);
    if (bytes.length < 44 || tag4(0) !== "RIFF" || tag4(8) !== "WAVE") return null;
    let o = 12, fmt = null, dataAt = -1, dataLen = 0;
    while (o + 8 <= bytes.length) {
      const id = tag4(o), size = v.getUint32(o + 4, true);
      if (id === "fmt ") fmt = { format: v.getUint16(o + 8, true), channels: v.getUint16(o + 10, true), rate: v.getUint32(o + 12, true), bits: v.getUint16(o + 22, true) };
      else if (id === "data") { dataAt = o + 8; dataLen = Math.min(size, bytes.length - dataAt); break; }
      o += 8 + size + (size & 1);
    }
    if (!fmt || dataAt < 0 || fmt.channels < 1 || fmt.channels > 32) return null;
    const bytesPer = fmt.bits >> 3, frames = Math.floor(dataLen / (bytesPer * fmt.channels));
    if (frames < 1 || !(fmt.format === 1 || fmt.format === 3 || fmt.format === 0xFFFE)) return null;
    const out = [];
    for (let c = 0; c < fmt.channels; ++c) out.push(new Float32Array(frames));
    for (let i = 0; i < frames; ++i) for (let c = 0; c < fmt.channels; ++c) {
      const p = dataAt + (i * fmt.channels + c) * bytesPer;
      let s;
      if (fmt.format === 3) s = fmt.bits === 32 ? v.getFloat32(p, true) : v.getFloat64(p, true);
      else if (fmt.bits === 8) s = (bytes[p] - 128) / 128;
      else if (fmt.bits === 16) s = v.getInt16(p, true) / 32768;
      else if (fmt.bits === 24) s = ((bytes[p] | (bytes[p + 1] << 8) | (bytes[p + 2] << 16)) << 8 >> 8) / 8388608;
      else s = v.getInt32(p, true) / 2147483648;
      out[c][i] = s;
    }
    return { data: out, rate: fmt.rate };
  }
  const resample = (data, from, to) => {
    if (from === to) return data;
    const length = Math.max(1, Math.round(data[0].length * to / from));
    return data.map((d) => { const r = new Float32Array(length); for (let i = 0; i < length; ++i) { const p = i * from / to, a = Math.floor(p), f = p - a; r[i] = d[a] + ((a + 1 < d.length ? d[a + 1] : d[a]) - d[a]) * f; } return r; });
  };
  async function decode(bytes, rate) {
    // (The decoder reads all the formats, wav too; the wav reader here is for where it is not there.)
    const channels = decodeN(bytes, rate);
    if (channels && channels.length) return AudioBuffer.make(channels.map((b) => new Float32Array(b)), rate);
    const wav = decodeWav(bytes);
    if (!wav) throw new Error("undecodable");
    return AudioBuffer.make(resample(wav.data, wav.rate, rate), rate);
  }

  // ---- The ticking: what is of the page's thread ----
  function poll(ctx) {
    const s = hidden.get(ctx);
    if (s.closed) return;
    const list = audioN(OP.Poll, s.id);
    let i = 0;
    for (; i < list.length && list[i] !== -1; ++i) {
      const node = hidden.get(ctx).nodes.get(list[i]);
      if (!node) { audioN(OP.ProcDone, s.id, list[i]); continue; }
      const n = hidden.get(node);
      const input = [], output = [];
      for (let c = 0; c < n.inChannels; ++c) { const f = new Float32Array(n.bufferSize); audioN(OP.ProcIn, s.id, n.id, c, f); input.push(f); }
      for (let c = 0; c < n.outChannels; ++c) output.push(new Float32Array(n.bufferSize));
      const event = new AudioProcessingEvent("audioprocess", { inputBuffer: AudioBuffer.make(input, s.rate), outputBuffer: AudioBuffer.make(output, s.rate), playbackTime: ctx.currentTime + n.bufferSize / s.rate });
      try { node.dispatchEvent(event); } catch (e) { reportError(e); }
      // (What the callback left in the output buffer: it may have replaced a channel's array contents only, so the channels are read from the buffer.)
      const result = hidden.get(event.outputBuffer).channels;
      for (let c = 0; c < n.outChannels; ++c) audioN(OP.ProcOut, s.id, n.id, c, result[c]);
      audioN(OP.ProcDone, s.id, n.id);
    }
    for (++i; i < list.length; ++i) {
      const node = hidden.get(ctx).nodes.get(list[i]);
      if (node) { try { node.dispatchEvent(new Event("ended")); } catch (e) { reportError(e); } }
    }
  }

  const stateEvent = (ctx, state) => {
    const s = hidden.get(ctx);
    if (s.state === state) return;
    s.state = state;
    queueMicrotask(() => ctx.dispatchEvent(new Event("statechange")));
  };

  class AudioContext extends BaseAudioContext {
    constructor(options) {
      const rate = options && options.sampleRate !== undefined ? +options.sampleRate : 48000;
      if (!(rate >= 3000 && rate <= 768000)) throw fail("NotSupportedError", "Failed to construct 'AudioContext': The sampleRate provided (" + rate + ") is outside the range [3000, 768000].");
      const id = audioN(OP.New, 0, rate, 2, 1);
      if (id < 0) throw fail("NotSupportedError", "Failed to construct 'AudioContext': The number of hardware contexts provided (6) is greater than or equal to the maximum bound (6).");
      super(PRIVATE);
      const ctx = this;
      hidden.set(ctx, { id, rate, state: "suspended", closed: false, lastTime: 0, handlers: null, nodes: new Map() });
      hidden.get(ctx).destination = AudioDestinationNode.make(ctx, 2);
      // (Sound starts at once: there is no gesture to wait for in a browser the page's user has opened it in.)
      audioN(OP.Start, id);
      hidden.get(ctx).timer = setInterval(() => poll(ctx), 4);
      stateEvent(ctx, "running");
    }
    get baseLatency() { return 0.01; }
    get outputLatency() { return 0.04; }
    getOutputTimestamp() { return { contextTime: this.currentTime, performanceTime: performance.now() }; }
    resume() {
      const s = hidden.get(this);
      if (s.closed) return Promise.reject(fail("InvalidStateError", "Cannot resume a closed AudioContext"));
      audioN(OP.Start, s.id);
      stateEvent(this, "running");
      return Promise.resolve();
    }
    suspend() {
      const s = hidden.get(this);
      if (s.closed) return Promise.reject(fail("InvalidStateError", "Cannot suspend a closed AudioContext"));
      audioN(OP.Suspend, s.id);
      stateEvent(this, "suspended");
      return Promise.resolve();
    }
    close() {
      const s = hidden.get(this);
      if (s.closed) return Promise.reject(fail("InvalidStateError", "Cannot close a closed AudioContext."));
      s.lastTime = this.currentTime;
      s.closed = true;
      clearInterval(s.timer);
      audioN(OP.Close, s.id);
      stateEvent(this, "closed");
      return Promise.resolve();
    }
  }
  tag(AudioContext, "AudioContext");

  class OfflineAudioContext extends BaseAudioContext {
    constructor(a, b, c) {
      let channels, length, rate;
      if (a !== null && typeof a === "object") ({ numberOfChannels: channels = 1, length, sampleRate: rate } = a);
      else [channels, length, rate] = [a, b, c];
      channels >>>= 0; length >>>= 0; rate = +rate;
      if (arguments.length === 0) throw typeError("Failed to construct 'OfflineAudioContext': 3 arguments required, but only 0 present.");
      if (channels < 1 || channels > 32) throw fail("NotSupportedError", "Failed to construct 'OfflineAudioContext': The number of channels provided (" + channels + ") is outside the range [1, 32].");
      if (length < 1) throw fail("NotSupportedError", "Failed to construct 'OfflineAudioContext': The number of frames provided (" + length + ") is less than or equal to the minimum bound (0).");
      if (!(rate >= 3000 && rate <= 768000)) throw fail("NotSupportedError", "Failed to construct 'OfflineAudioContext': The sampleRate provided (" + rate + ") is outside the range [3000, 768000].");
      const id = audioN(OP.New, 0, rate, Math.min(channels, 8), 0);
      super(PRIVATE);
      const ctx = this;
      hidden.set(ctx, { id, rate, state: "suspended", closed: false, lastTime: 0, handlers: null, length, channels, started: false, nodes: new Map() });
      hidden.get(ctx).destination = AudioDestinationNode.make(ctx, channels);
    }
    get length() { return hidden.get(this).length; }
    startRendering() {
      const s = hidden.get(this);
      if (s.started) return Promise.reject(fail("InvalidStateError", "cannot call startRendering more than once"));
      s.started = true;
      stateEvent(this, "running");
      return new Promise((resolve) => {
        // (Rendered in one go; where a script processor is in the graph, in pieces, between which the page's callbacks run.)
        const data = [];
        for (let c = 0; c < s.channels; ++c) data.push(new Float32Array(s.length));
        const scripted = [...hidden.get(this).nodes.values()].some((n) => hidden.get(n).kind === KIND.ScriptProcessor);
        const finish = () => {
          const buffer = AudioBuffer.make(data, s.rate);
          stateEvent(this, "closed");
          s.lastTime = s.length / s.rate;
          const event = new Event("complete");
          event.renderedBuffer = buffer;
          resolve(buffer);
          this.dispatchEvent(event);
        };
        if (!scripted) {
          const tmp = []; for (let c = 0; c < Math.min(s.channels, 8); ++c) tmp.push(data[c]);
          audioN(OP.Render, s.id, s.length, tmp);
          poll(this);
          audioN(OP.Close, s.id);
          finish();
          return;
        }
        let done = 0;
        const step = () => {
          const n = Math.min(512, s.length - done);
          const pieces = []; for (let c = 0; c < s.channels; ++c) pieces.push(new Float32Array(n));
          audioN(OP.Render, s.id, n, pieces);
          for (let c = 0; c < s.channels; ++c) data[c].set(pieces[c], done);
          done += n;
          poll(this);
          if (done < s.length) setTimeout(step, 0); else { audioN(OP.Close, s.id); finish(); }
        };
        step();
      });
    }
    resume() { return Promise.resolve(); }
    suspend() { return Promise.resolve(); }
  }
  tag(OfflineAudioContext, "OfflineAudioContext");
  fixHandlers((handlerOf(OfflineAudioContext, "complete"), OfflineAudioContext), ["complete"]);

  for (const [name, value] of Object.entries({ AudioContext, OfflineAudioContext, BaseAudioContext, AudioBuffer, AudioParam, AudioNode, AudioDestinationNode, GainNode, AudioScheduledSourceNode, OscillatorNode, ConstantSourceNode, AudioBufferSourceNode, StereoPannerNode, BiquadFilterNode, DelayNode, DynamicsCompressorNode, ChannelSplitterNode, ChannelMergerNode, AnalyserNode, ScriptProcessorNode, AudioProcessingEvent, PannerNode, WaveShaperNode, ConvolverNode, AudioListener, MediaElementAudioSourceNode })) {
    Object.defineProperty(globalThis, name, { value, writable: true, configurable: true, enumerable: false });
  }
  Object.defineProperty(globalThis, "webkitAudioContext", { value: AudioContext, writable: true, configurable: true, enumerable: false });
  Object.defineProperty(globalThis, "webkitOfflineAudioContext", { value: OfflineAudioContext, writable: true, configurable: true, enumerable: false });
  // `createMediaElementSource(element)` is not routed: the element still plays by itself.
})();
