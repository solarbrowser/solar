// CSS Font Loading: FontFace, FontFaceSet, document.fonts (https://drafts.csswg.org/css-font-loading/), over the font natives.
(function () {
  'use strict';
  const global = globalThis;
  const N = {
    open: global.__solarFontOpen,
    fetch: global.__solarFontFetch,
    local: global.__solarFontLocal,
    descriptor: global.__solarFontDescriptor,
    shorthand: global.__solarFontShorthand,
    faces: global.__solarDocumentFontFaces,
    loadRule: global.__solarFontLoadRule,
    ruleState: global.__solarFontRuleState,
    setScript: global.__solarFontsSetScript,
  };
  for (const name of ['Open', 'Fetch', 'Local', 'Descriptor', 'Shorthand', 'LoadRule', 'RuleState', 'sSetScript']) delete global['__solarFont' + name];
  delete global.__solarDocumentFontFaces;

  const syntaxError = (message) => new DOMException(message, 'SyntaxError');
  const handled = (promise) => {
    promise.catch(() => {});
    return promise;
  };
  const deferred = () => {
    const d = {};
    d.promise = new Promise((resolve, reject) => {
      d.resolve = resolve;
      d.reject = reject;
    });
    handled(d.promise);
    return d;
  };

  // ---- FontFace ----

  const DESCRIPTORS = [
    ['style', 'font-style', 'normal'],
    ['weight', 'font-weight', 'normal'],
    ['stretch', 'font-stretch', 'normal'],
    ['unicodeRange', 'unicode-range', 'U+0-10FFFF'],
    ['variant', 'font-variant', 'normal'],
    ['featureSettings', 'font-feature-settings', 'normal'],
    ['variationSettings', 'font-variation-settings', 'normal'],
    ['display', 'font-display', 'auto'],
    ['ascentOverride', 'ascent-override', 'normal'],
    ['descentOverride', 'descent-override', 'normal'],
    ['lineGapOverride', 'line-gap-override', 'normal'],
  ];
  const FACE = new WeakMap();
  const sets = new Set();  // every set, to tell of a face that settles

  const stateOf = (face, name) => {
    const state = FACE.get(face);
    if (!state) throw new TypeError(`Illegal invocation`);
    return state;
  };

  const baseUrl = () => {
    try {
      return global.document.baseURI;
    } catch (e) {
      return global.location.href;
    }
  };

  // The sources of a src descriptor: [{ url } | { local }].
  const sourcesOf = (text) => {
    const out = [];
    let depth = 0, start = 0, quote = '';
    const pieces = [];
    for (let i = 0; i <= text.length; i++) {
      const c = text[i];
      if (quote) {
        if (c === '\\') i++;
        else if (c === quote) quote = '';
      } else if (c === '"' || c === "'") quote = c;
      else if (c === '(') depth++;
      else if (c === ')') depth--;
      else if ((c === ',' && depth === 0) || i === text.length) {
        pieces.push(text.slice(start, i).trim());
        start = i + 1;
      }
    }
    const unquote = (s) => {
      s = s.trim();
      if ((s[0] === '"' || s[0] === "'") && s[s.length - 1] === s[0]) s = s.slice(1, -1);
      return s.replace(/\\(.)/g, '$1');
    };
    for (const piece of pieces) {
      let m = /^url\(\s*([\s\S]*?)\s*\)/i.exec(piece);
      if (m) out.push({ url: unquote(m[1]) });
      else if ((m = /^local\(\s*([\s\S]*?)\s*\)/i.exec(piece))) out.push({ local: unquote(m[1]) });
    }
    return out;
  };

  const settle = (state, status, reason) => {
    if (state.status === 'loaded' || state.status === 'error') return;
    state.status = status;
    if (status === 'loaded') state.loaded.resolve(state.face);
    else state.loaded.reject(reason);
    for (const set of sets) set.faceSettled(state);
  };

  const startLoading = (state) => {
    state.status = 'loading';
    for (const set of sets) set.faceLoading(state);
    const fail = (e) => settle(state, 'error', e instanceof Error || e instanceof DOMException ? e : new DOMException('A network error occurred.', 'NetworkError'));
    if (state.rule >= 0) {
      Promise.resolve().then(() => {
        if (N.loadRule(state.rule)) settle(state, 'loaded');
        else fail(null);
      });
      return;
    }
    if (state.bytes) {
      Promise.resolve().then(() => {
        const handle = N.open(state.bytes);
        if (handle < 0) return settle(state, 'error', syntaxError('The font data could not be parsed.'));
        state.handle = handle;
        settle(state, 'loaded');
      });
      return;
    }
    (async () => {
      for (const source of state.sources) {
        try {
          if (source.local !== undefined) {
            const handle = N.local(source.local);
            if (handle >= 0) {
              state.handle = handle;
              return settle(state, 'loaded');
            }
            continue;
          }
          const handle = N.fetch(new URL(source.url, state.base).href);
          if (handle >= 0) {
            state.handle = handle;
            return settle(state, 'loaded');
          }
        } catch (e) {
          // the next source
        }
      }
      fail(null);
    })();
  };

  class FontFace {
    constructor(family, source, descriptors = {}) {
      if (arguments.length < 2) throw new TypeError(`Failed to construct 'FontFace': 2 arguments required, but only ${arguments.length} present.`);
      family = String(family);
      const canonicalFamily = N.descriptor('font-family', family);
      if (canonicalFamily === null) throw syntaxError(`Failed to construct 'FontFace': Failed to set 'family' property: invalid value.`);
      if (descriptors === null || (typeof descriptors !== 'object' && typeof descriptors !== 'function' && descriptors !== undefined)) {
        throw new TypeError(`Failed to construct 'FontFace': The provided value is not of type 'FontFaceDescriptors'.`);
      }
      const values = {};
      for (const [name, css, initial] of DESCRIPTORS) {
        const given = descriptors[name];
        if (given === undefined) {
          values[name] = initial;
          continue;
        }
        const text = String(given);
        const canonical = N.descriptor(css, text);
        if (canonical === null) throw syntaxError(`Failed to construct 'FontFace': Failed to set '${name}' property: invalid value.`);
        values[name] = canonical;
      }
      const state = { family: canonicalFamily, values, status: 'unloaded', loaded: deferred(), sources: [], bytes: null, handle: -1, rule: -1, base: baseUrl() };
      state.face = this;
      FACE.set(this, state);
      if (typeof source === 'string' || (source !== null && typeof source === 'object' && !ArrayBuffer.isView(source) && !(source instanceof ArrayBuffer) && !(typeof SharedArrayBuffer !== 'undefined' && source instanceof SharedArrayBuffer)) || typeof source === 'number' || typeof source === 'boolean' || source === undefined) {
        const text = String(source);
        const canonical = N.descriptor('src', text);
        if (canonical === null) {
          state.status = 'error';
          state.loaded.reject(syntaxError('The source could not be parsed.'));
        } else {
          state.sources = sourcesOf(canonical);
        }
      } else if (ArrayBuffer.isView(source) || source instanceof ArrayBuffer || (typeof SharedArrayBuffer !== 'undefined' && source instanceof SharedArrayBuffer)) {
        state.bytes = source instanceof ArrayBuffer || (typeof SharedArrayBuffer !== 'undefined' && source instanceof SharedArrayBuffer)
          ? new Uint8Array(source.slice(0))
          : new Uint8Array(source.buffer.slice(source.byteOffset, source.byteOffset + source.byteLength));
        startLoading(state);
      } else {
        throw new TypeError(`Failed to construct 'FontFace': The provided value is not of type '(DOMString or ArrayBuffer or ArrayBufferView)'.`);
      }
    }

    get family() {
      return stateOf(this).family;
    }
    set family(value) {
      const state = stateOf(this);
      const canonical = N.descriptor('font-family', String(value));
      if (canonical === null) throw syntaxError(`Failed to set the 'family' property on 'FontFace': Invalid font-family.`);
      state.family = canonical;
      FontFaceSet.sync();
    }

    get status() {
      return stateOf(this).status;
    }
    get loaded() {
      return stateOf(this).loaded.promise;
    }

    load() {
      const state = FACE.get(this);
      if (!state) return Promise.reject(new TypeError('Illegal invocation'));
      if (state.status === 'unloaded') startLoading(state);
      return state.loaded.promise;
    }
  }

  for (const [name, css] of DESCRIPTORS) {
    Object.defineProperty(FontFace.prototype, name, {
      get() {
        return stateOf(this).values[name];
      },
      set(value) {
        const state = stateOf(this);
        const canonical = N.descriptor(css, String(value));
        if (canonical === null) throw syntaxError(`Failed to set the '${name}' property on 'FontFace': Invalid value.`);
        state.values[name] = canonical;
        FontFaceSet.sync();
      },
      enumerable: true,
      configurable: true,
    });
  }
  Object.defineProperty(FontFace.prototype, Symbol.toStringTag, { value: 'FontFace', configurable: true });

  // A face standing for an @font-face rule of the document.
  const connected = (record, index) => {
    const [family, src, weight, style, stretch, unicodeRange, variant, featureSettings, variationSettings, display, ascentOverride, descentOverride, lineGapOverride] = record;
    const face = Object.create(FontFace.prototype);
    const status = N.ruleState(index);
    const state = {
      family,
      values: { style, weight, stretch, unicodeRange, variant, featureSettings, variationSettings, display, ascentOverride, descentOverride, lineGapOverride },
      status: status === 1 ? 'loaded' : status === 2 ? 'error' : 'unloaded',
      loaded: deferred(),
      sources: [],
      bytes: null,
      handle: -1,
      rule: index,
      base: baseUrl(),
      face,
    };
    if (state.status === 'loaded') state.loaded.resolve(face);
    else if (state.status === 'error') state.loaded.reject(new DOMException('A network error occurred.', 'NetworkError'));
    FACE.set(face, state);
    return face;
  };

  // ---- Events ----

  const EVENT_FACES = new WeakMap();
  class FontFaceSetLoadEvent extends Event {
    constructor(type, init = {}) {
      super(type, init);
      const faces = init && init.fontfaces !== undefined ? Array.from(init.fontfaces) : [];
      for (const face of faces) if (!FACE.has(face)) throw new TypeError(`Failed to construct 'FontFaceSetLoadEvent': member fontfaces is not of type FontFace.`);
      EVENT_FACES.set(this, Object.freeze(faces));
    }
    get fontfaces() {
      return EVENT_FACES.get(this);
    }
  }

  // ---- FontFaceSet ----

  const SET = new WeakMap();
  const k_self = Symbol('self');
  const k_list = Symbol('list');
  const k_refresh = Symbol('refresh');
  const k_sync = Symbol('sync');
  const k_loading = Symbol('loading');
  const k_contains = Symbol('contains');
  const k_settled = Symbol('settled');
  const k_matching = Symbol('matching');

  const familyKey = (name) => name.replace(/^["']|["']$/g, '').toLowerCase();
  const familiesOf = (text) => {
    const parsed = N.shorthand(String(text));
    if (parsed === null) return null;
    return parsed.split('\x1f')[0].split(',').map((s) => familyKey(s.trim()));
  };
  const covers = (face, text) => {
    const ranges = FACE.get(face).values.unicodeRange.split(',').map((r) => {
      const m = /^U\+([0-9A-F?]+)(?:-([0-9A-F]+))?$/i.exec(r.trim());
      if (!m) return [0, 0x10ffff];
      if (m[2]) return [parseInt(m[1], 16), parseInt(m[2], 16)];
      if (m[1].includes('?')) return [parseInt(m[1].replace(/\?/g, '0'), 16), parseInt(m[1].replace(/\?/g, 'F'), 16)];
      const v = parseInt(m[1], 16);
      return [v, v];
    });
    for (const ch of text) {
      const cp = ch.codePointAt(0);
      if (ranges.some(([lo, hi]) => cp >= lo && cp <= hi)) return true;
    }
    return false;
  };

  class FontFaceSet extends EventTarget {
    constructor(initialFaces = []) {
      super();
      const faces = Array.from(initialFaces);
      for (const face of faces) if (!FACE.has(face)) throw new TypeError(`Failed to construct 'FontFaceSet': member of the sequence is not of type FontFace.`);
      const set = {
        members: [],
        record: [],
        isDocument: false,
        loading: [],
        loadedFaces: [],
        failedFaces: [],
        status: 'loaded',
        ready: deferred(),
        handlers: {},
        object: this,
      };
      set.ready.resolve(this);
      SET.set(this, set);
      sets.add(set);
      set.faceLoading = (state) => this[k_loading](set, state);
      set.faceSettled = (state) => this[k_settled](set, state);
      for (const face of faces) this.add(face);
    }

    static sync() {
      for (const set of sets) if (set.isDocument) set.object[k_sync](set);
    }

    [k_self]() {
      const set = SET.get(this);
      if (!set) throw new TypeError('Illegal invocation');
      return set;
    }

    // The document's set holds the @font-face rules' faces first, then those scripts added.
    [k_list](set) {
      if (set.isDocument) this[k_refresh](set);
      return set.isDocument ? [...set.connectedFaces, ...set.members] : set.members;
    }

    [k_refresh](set) {
      const text = N.faces();
      const records = text === '' ? [] : text.split('\x1e').filter((s) => s !== '');
      const previous = set.connectedByRecord || new Map();
      const next = new Map();
      const faces = [];
      records.forEach((record, index) => {
        let key = record;
        let n = 0;
        while (next.has(key + '\x00' + n)) n++;
        key = key + '\x00' + n;
        const prior = previous.get(key);
        const face = prior && FACE.get(prior).rule === index ? prior : connected(record.split('\x1f'), index);
        next.set(key, face);
        faces.push(face);
      });
      set.connectedByRecord = next;
      set.connectedFaces = faces;
    }

    [k_sync](set) {
      if (!set.isDocument) return;
      this[k_refresh](set);
      const out = [];
      for (const face of set.members) {
        const s = FACE.get(face);
        if (s.status === 'loaded' && s.handle >= 0) out.push([s.family, s.values.weight, s.values.style, s.values.stretch, s.values.unicodeRange, s.handle].join('\x1f'));
      }
      N.setScript(out.join('\x1e'));
    }

    [k_loading](set, state) {
      if (!this[k_contains](set, state.face)) return;
      if (set.loading.length === 0) {
        set.status = 'loading';
        if (set.ready.settled) {
          set.ready = deferred();
        }
        queueMicrotask(() => this.dispatchEvent(new Event('loading')));
      }
      if (!set.loading.includes(state.face)) set.loading.push(state.face);
    }

    [k_contains](set, face) {
      return this[k_list](set).includes(face);
    }

    [k_settled](set, state) {
      const index = set.loading.indexOf(state.face);
      if (index < 0) {
        if (state.status === 'loaded') this[k_sync](set);
        return;
      }
      set.loading.splice(index, 1);
      (state.status === 'loaded' ? set.loadedFaces : set.failedFaces).push(state.face);
      if (state.status === 'loaded') this[k_sync](set);
      if (set.loading.length > 0) return;
      // Done: the events once the settling is over, then ready.
      const ok = set.loadedFaces, bad = set.failedFaces;
      set.loadedFaces = [];
      set.failedFaces = [];
      queueMicrotask(() => {
        if (set.loading.length > 0) return;
        set.status = 'loaded';
        this.dispatchEvent(new FontFaceSetLoadEvent('loadingdone', { fontfaces: ok }));
        if (bad.length > 0) this.dispatchEvent(new FontFaceSetLoadEvent('loadingerror', { fontfaces: bad }));
        set.ready.settled = true;
        set.ready.resolve(this);
      });
    }

    get size() {
      return this[k_list](this[k_self]()).length;
    }
    get status() {
      return this[k_self]().status;
    }
    get ready() {
      return this[k_self]().ready.promise;
    }

    add(face) {
      const set = this[k_self]();
      if (!FACE.has(face)) throw new TypeError(`Failed to execute 'add' on 'FontFaceSet': parameter 1 is not of type 'FontFace'.`);
      const state = FACE.get(face);
      if (state.rule >= 0 || this[k_list](set).includes(face)) return this;
      set.members.push(face);
      if (state.status === 'loading') this[k_loading](set, state);
      this[k_sync](set);
      return this;
    }
    delete(face) {
      const set = this[k_self]();
      const index = set.members.indexOf(face);
      if (index < 0) return false;
      set.members.splice(index, 1);
      const loading = set.loading.indexOf(face);
      if (loading >= 0) set.loading.splice(loading, 1);
      this[k_sync](set);
      return true;
    }
    has(face) {
      return this[k_list](this[k_self]()).includes(face);
    }
    clear() {
      const set = this[k_self]();
      set.members.length = 0;
      set.loading.length = 0;
      this[k_sync](set);
    }
    forEach(callback, thisArg) {
      const set = this[k_self]();
      if (typeof callback !== 'function') throw new TypeError(`Failed to execute 'forEach' on 'FontFaceSet': The callback provided as parameter 1 is not a function.`);
      for (const face of [...this[k_list](set)]) callback.call(thisArg, face, face, this);
    }
    values() {
      return [...this[k_list](this[k_self]())].values();
    }
    keys() {
      return this.values();
    }
    entries() {
      return this[k_list](this[k_self]()).map((f) => [f, f]).values();
    }
    [Symbol.iterator]() {
      return this.values();
    }

    [k_matching](font, text) {
      const set = this[k_self]();
      const families = familiesOf(font);
      if (families === null) return null;
      const faces = this[k_list](set).filter((f) => families.includes(familyKey(FACE.get(f).family)));
      return faces.filter((f) => covers(f, text));
    }

    load(font, text = ' ') {
      let faces;
      try {
        if (arguments.length < 1) throw new TypeError(`Failed to execute 'load' on 'FontFaceSet': 1 argument required, but only 0 present.`);
        faces = this[k_matching](String(font), String(text));
      } catch (e) {
        return Promise.reject(e);
      }
      if (faces === null) return Promise.reject(syntaxError(`Failed to execute 'load' on 'FontFaceSet': Could not resolve '${font}' as a font.`));
      return Promise.all(faces.map((f) => f.load()));
    }

    check(font, text = ' ') {
      if (arguments.length < 1) throw new TypeError(`Failed to execute 'check' on 'FontFaceSet': 1 argument required, but only 0 present.`);
      const faces = this[k_matching](String(font), String(text));
      if (faces === null) throw syntaxError(`Failed to execute 'check' on 'FontFaceSet': Could not resolve '${font}' as a font.`);
      return faces.every((f) => FACE.get(f).status === 'loaded');
    }
  }

  for (const type of ['loading', 'loadingdone', 'loadingerror']) {
    Object.defineProperty(FontFaceSet.prototype, 'on' + type, {
      get() {
        const set = SET.get(this);
        if (!set) throw new TypeError('Illegal invocation');
        return set.handlers[type] ? set.handlers[type].callback : null;
      },
      set(value) {
        const set = SET.get(this);
        if (!set) throw new TypeError('Illegal invocation');
        const callback = typeof value === 'function' || (value !== null && typeof value === 'object') ? value : null;
        if (!set.handlers[type]) {
          const entry = { callback: null };
          entry.listener = (event) => {
            if (typeof entry.callback === 'function') entry.callback.call(this, event);
          };
          this.addEventListener(type, entry.listener);
          set.handlers[type] = entry;
        }
        set.handlers[type].callback = callback;
      },
      enumerable: true,
      configurable: true,
    });
  }
  Object.defineProperty(FontFaceSet.prototype, Symbol.toStringTag, { value: 'FontFaceSet', configurable: true });
  Object.defineProperty(FontFaceSet.prototype, 'size', { enumerable: true });

  const DOCUMENT_SETS = new WeakMap();
  Object.defineProperty(global.Document.prototype, 'fonts', {
    get() {
      let set = DOCUMENT_SETS.get(this);
      if (!set) {
        set = new FontFaceSet();
        SET.get(set).isDocument = true;
        DOCUMENT_SETS.set(this, set);
      }
      return set;
    },
    enumerable: true,
    configurable: true,
  });

  for (const [name, value] of [['FontFace', FontFace], ['FontFaceSet', FontFaceSet], ['FontFaceSetLoadEvent', FontFaceSetLoadEvent]]) {
    Object.defineProperty(global, name, { value, writable: true, configurable: true, enumerable: false });
  }
})();
