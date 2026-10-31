// The part of WPT's testharness.js that the url tests use, and nothing else.
(function () {
  const results = [];
  const pending = [];
  const skips = globalThis.__skip || [];
  let skipped = 0;

  class AssertionError extends Error {}

  function describe(value) {
    try {
      return typeof value === 'string' ? JSON.stringify(value) : String(value);
    } catch (e) {
      return '<unprintable>';
    }
  }

  function fail(message, description) {
    throw new AssertionError(description ? description + ': ' + message : message);
  }

  globalThis.assert_equals = (actual, expected, description) => {
    if (!Object.is(actual, expected)) fail('expected ' + describe(expected) + ' but got ' + describe(actual), description);
  };
  globalThis.assert_not_equals = (actual, unexpected, description) => {
    if (Object.is(actual, unexpected)) fail('got disallowed value ' + describe(actual), description);
  };
  globalThis.assert_true = (actual, description) => {
    if (actual !== true) fail('expected true but got ' + describe(actual), description);
  };
  globalThis.assert_false = (actual, description) => {
    if (actual !== false) fail('expected false but got ' + describe(actual), description);
  };
  // testharness's format_value, for messages: strings quoted, other values as String would write them.
  globalThis.format_value = (value) => (typeof value === 'string' ? JSON.stringify(value) : typeof value === 'symbol' ? String(value) : Array.isArray(value) ? '[' + value.map(globalThis.format_value).join(', ') + ']' : String(value));
  // The promise-returning counterparts of assert_throws_*: the promise must reject with such an error.
  globalThis.promise_rejects_js = (t, constructor, promise, description) =>
    Promise.resolve(promise).then(
      () => fail('promise resolved, but a ' + constructor.name + ' was expected', description),
      (e) => { if (!(e instanceof constructor)) fail('rejected with ' + describe(e) + ' instead of a ' + constructor.name, description); });
  globalThis.promise_rejects_exactly = (t, expected, promise, description) =>
    Promise.resolve(promise).then(
      () => fail('promise resolved, but a rejection was expected', description),
      (e) => { if (e !== expected) fail('rejected with ' + describe(e) + ' instead of the expected value', description); });
  globalThis.promise_rejects_dom = (t, name, promise, description) =>
    Promise.resolve(promise).then(
      () => fail('promise resolved, but a ' + name + ' DOMException was expected', description),
      (e) => { if (!(e instanceof DOMException) || e.name !== name) fail('rejected with ' + describe(e) + ' instead of a ' + name, description); });
  // Deep comparison of the plain objects tests build for expectations (not a full testharness one).
  const sameShape = (a, b) => {
    if (Object.is(a, b)) return true;
    if (typeof a !== 'object' || typeof b !== 'object' || a === null || b === null) return false;
    if (Object.getPrototypeOf(a) !== Object.getPrototypeOf(b)) return false;
    const keysA = Reflect.ownKeys(a), keysB = Reflect.ownKeys(b);
    return keysA.length === keysB.length && keysA.every((k) => keysB.includes(k) && sameShape(a[k], b[k]));
  };
  globalThis.assert_object_equals = (actual, expected, description) => {
    if (!sameShape(actual, expected)) fail('expected ' + describe(JSON.stringify(expected)) + ' but got ' + describe(JSON.stringify(actual)), description);
  };
  globalThis.step_timeout = (f, ms, ...args) => setTimeout(() => f(...args), ms);
  globalThis.assert_regexp_match = (actual, expected, description) => {
    if (!expected.test(actual)) fail('expected a match for ' + String(expected) + ' but got ' + describe(actual), description);
  };
  globalThis.assert_own_property = (object, name, description) => {
    if (!Object.prototype.hasOwnProperty.call(object, name)) fail('expected property ' + String(name) + ' missing', description);
  };
  globalThis.assert_not_own_property = (object, name, description) => {
    if (Object.prototype.hasOwnProperty.call(object, name)) fail('unexpected property ' + String(name) + ' is found on object', description);
  };
  globalThis.assert_greater_than = (actual, expected, description) => {
    if (!(actual > expected)) fail('expected a number greater than ' + describe(expected) + ' but got ' + describe(actual), description);
  };
  globalThis.assert_less_than = (actual, expected, description) => {
    if (!(actual < expected)) fail('expected a number less than ' + describe(expected) + ' but got ' + describe(actual), description);
  };
  globalThis.assert_greater_than_equal = (actual, expected, description) => {
    if (!(actual >= expected)) fail('expected a number greater than or equal to ' + describe(expected) + ' but got ' + describe(actual), description);
  };
  globalThis.assert_less_than_equal = (actual, expected, description) => {
    if (!(actual <= expected)) fail('expected a number less than or equal to ' + describe(expected) + ' but got ' + describe(actual), description);
  };
  globalThis.assert_between_inclusive = (actual, lower, upper, description) => {
    if (!(actual >= lower && actual <= upper)) fail('expected a number between ' + describe(lower) + ' and ' + describe(upper) + ' inclusive but got ' + describe(actual), description);
  };
  globalThis.assert_between_exclusive = (actual, lower, upper, description) => {
    if (!(actual > lower && actual < upper)) fail('expected a number between ' + describe(lower) + ' and ' + describe(upper) + ' exclusive but got ' + describe(actual), description);
  };
  globalThis.assert_in_array = (actual, expected, description) => {
    if (!expected.includes(actual)) fail(describe(actual) + ' not in array', description);
  };
  globalThis.assert_class_string = (object, expected, description) => {
    const actual = Object.prototype.toString.call(object);
    if (actual !== '[object ' + expected + ']') fail('expected [object ' + expected + '] but got ' + actual, description);
  };
  globalThis.assert_throws_exactly = (expected, func, description) => {
    try {
      func();
    } catch (e) {
      if (e !== expected) fail('threw ' + describe(e) + ' instead of the expected value', description);
      return;
    }
    fail('did not throw', description);
  };
  globalThis.self = globalThis;
  // The testharness call that ends a file with asynchronous tests; the runner ends them by itself.
  globalThis.done = () => {};

  globalThis.assert_unreached = (description) => fail('reached unreachable code', description);
  globalThis.assert_array_equals = (actual, expected, description) => {
    if (actual === null || typeof actual !== 'object' || typeof actual.length !== 'number') {
      fail('value is not array-like: ' + describe(actual), description);
    }
    if (actual.length !== expected.length) {
      fail('lengths differ, expected ' + expected.length + ' but got ' + actual.length, description);
    }
    for (let i = 0; i < expected.length; i++) {
      if (!Object.is(actual[i], expected[i])) {
        fail('index ' + i + ': expected ' + describe(expected[i]) + ' but got ' + describe(actual[i]), description);
      }
    }
  };
  globalThis.assert_approx_equals = (actual, expected, epsilon, description) => {
    if (typeof actual !== 'number') fail('expected a number but got ' + describe(actual), description);
    if (!(Math.abs(actual - expected) <= epsilon)) fail('expected ' + describe(expected) + ' +/- ' + epsilon + ' but got ' + describe(actual), description);
  };
  globalThis.assert_array_approx_equals = (actual, expected, epsilon, description) => {
    if (actual.length !== expected.length) fail('lengths differ, expected ' + expected.length + ' but got ' + actual.length, description);
    for (let i = 0; i < expected.length; i++) {
      if (!(Math.abs(actual[i] - expected[i]) <= epsilon)) fail('index ' + i + ': expected ' + describe(expected[i]) + ' +/- ' + epsilon + ' but got ' + describe(actual[i]), description);
    }
  };
  globalThis.assert_throws_js = (constructor, func, description) => {
    try {
      func();
    } catch (e) {
      if (e instanceof constructor) return;
      fail('threw ' + describe(e) + ' instead of ' + constructor.name, description);
    }
    fail('did not throw ' + constructor.name, description);
  };

  // `type` is a DOMException's name (or its legacy constant name, which the tests that use the two
  // spell as the code's name).
  const legacyNames = {
    INDEX_SIZE_ERR: 'IndexSizeError', HIERARCHY_REQUEST_ERR: 'HierarchyRequestError', WRONG_DOCUMENT_ERR: 'WrongDocumentError',
    INVALID_CHARACTER_ERR: 'InvalidCharacterError', NO_MODIFICATION_ALLOWED_ERR: 'NoModificationAllowedError', NOT_FOUND_ERR: 'NotFoundError',
    NOT_SUPPORTED_ERR: 'NotSupportedError', INUSE_ATTRIBUTE_ERR: 'InUseAttributeError', INVALID_STATE_ERR: 'InvalidStateError', SYNTAX_ERR: 'SyntaxError',
    INVALID_MODIFICATION_ERR: 'InvalidModificationError', NAMESPACE_ERR: 'NamespaceError', INVALID_ACCESS_ERR: 'InvalidAccessError',
    TYPE_MISMATCH_ERR: 'TypeMismatchError', SECURITY_ERR: 'SecurityError', NETWORK_ERR: 'NetworkError', ABORT_ERR: 'AbortError',
    URL_MISMATCH_ERR: 'URLMismatchError', QUOTA_EXCEEDED_ERR: 'QuotaExceededError', TIMEOUT_ERR: 'TimeoutError', INVALID_NODE_TYPE_ERR: 'InvalidNodeTypeError',
    DATA_CLONE_ERR: 'DataCloneError',
  };
  // `type` is a DOMException's name or its legacy constant's. The second argument may be the DOMException
  // constructor of another realm, with the function after it.
  globalThis.assert_throws_dom = (type, funcOrConstructor, descriptionOrFunc, maybeDescription) => {
    let func = funcOrConstructor, description = descriptionOrFunc, constructor = DOMException;
    if (typeof descriptionOrFunc === 'function') {
      constructor = funcOrConstructor;
      func = descriptionOrFunc;
      description = maybeDescription;
    }
    let name = legacyNames[type] || type;
    if (typeof type === 'number') {
      const key = Object.keys(legacyNames).find((k) => DOMException[k] === type);
      if (key) name = legacyNames[key];
    }
    try {
      func();
    } catch (e) {
      if (!(e instanceof constructor)) fail('threw ' + describe(e) + ' instead of a DOMException', description);
      if (e.name !== name) fail('threw a DOMException named ' + e.name + ' instead of ' + name, description);
      return;
    }
    fail('did not throw a DOMException named ' + name, description);
  };

  // A skip is the start of a test's name, or, if it begins with "*", a part of it.
  function isSkipped(name) {
    return skips.some((skip) => (skip.startsWith('*') ? String(name).includes(skip.slice(1)) : String(name).startsWith(skip)));
  }

  globalThis.assert_idl_attribute = (object, name, description) => {
    if (!(name in object)) fail('assert_idl_attribute: ' + String(name) + ' is not an attribute', description);
  };

  globalThis.assert_readonly = (object, property, description) => {
    const initial = object[property];
    try {
      try { object[property] = initial + 'a'; } catch (e) { /* a strict-mode write to a read only property throws */ }
      if (!Object.is(object[property], initial)) fail('assert_readonly: property ' + String(property) + ' is not read only', description);
    } finally {
      try { object[property] = initial; } catch (e) { /* it is read only */ }
    }
  };

  // testdriver is what drives the browser from a test (clicks, keys, the accessibility tree), which there is
  // nothing to do here: its operations fail, and a test that needs one is a failure.
  // The BiDi log entries of the console are all there is of it that can be had here: the console tells of each message.
  const consoleListeners = new Set();
  globalThis.__solarConsoleEntry = (entry) => {
    entry.timestamp = Date.now();
    for (const listener of [...consoleListeners]) listener(entry);
  };
  const bidi = {
    log: {
      entry_added: {
        subscribe: async () => async () => {},
        on: (callback) => { consoleListeners.add(callback); return () => consoleListeners.delete(callback); },
      },
    },
  };
  // The user of the tests: what testdriver asks for is done here with events made by the script, as the program does for the real user (clicks, keys, the actions of the
  // WebDriver sequences, a recent action of the user that the Pointer Lock API asks for). What needs more than that is refused.
  const activate = () => { try { document[Symbol.for('solar.activation')] = performance.now(); } catch (e) {} };
  const KEYS = {
    '\uE003': ['Backspace', 'Backspace', 8], '\uE004': ['Tab', 'Tab', 9], '\uE006': ['Enter', 'Enter', 13], '\uE007': ['Enter', 'Enter', 13], '\uE00C': ['Escape', 'Escape', 27],
    '\uE00D': [' ', 'Space', 32], '\uE00E': ['PageUp', 'PageUp', 33], '\uE00F': ['PageDown', 'PageDown', 34], '\uE010': ['End', 'End', 35], '\uE011': ['Home', 'Home', 36],
    '\uE012': ['ArrowLeft', 'ArrowLeft', 37], '\uE013': ['ArrowUp', 'ArrowUp', 38], '\uE014': ['ArrowRight', 'ArrowRight', 39], '\uE015': ['ArrowDown', 'ArrowDown', 40],
    '\uE017': ['Delete', 'Delete', 46], '\uE008': ['Shift', 'ShiftLeft', 16], '\uE009': ['Control', 'ControlLeft', 17], '\uE00A': ['Alt', 'AltLeft', 18], '\uE03D': ['Meta', 'MetaLeft', 91],
  };
  const keyInfo = (value) => {
    if (KEYS[value]) return { key: KEYS[value][0], code: KEYS[value][1], keyCode: KEYS[value][2], printable: value === '\uE00D' };
    const upper = value.toUpperCase();
    const code = /^[a-z]$/i.test(value) ? 'Key' + upper : /^[0-9]$/.test(value) ? 'Digit' + value : '';
    return { key: value, code, keyCode: value.length === 1 ? upper.charCodeAt(0) : 0, printable: true };
  };
  const editable = (el) => el && ((el instanceof HTMLInputElement && !/^(checkbox|radio|button|submit|reset|image|file|range|color|hidden)$/.test(el.type)) || el instanceof HTMLTextAreaElement) && !el.disabled && !el.readOnly;
  const insert = (el, text) => {
    const start = el.selectionStart === null ? el.value.length : el.selectionStart, end = el.selectionEnd === null ? start : el.selectionEnd;
    el.value = el.value.slice(0, start) + text + el.value.slice(end);
    try { el.setSelectionRange(start + text.length, start + text.length); } catch (e) {}
    el.dispatchEvent(new InputEvent('input', { bubbles: true, data: text, inputType: 'insertText' }));
  };
  const erase = (el, forward) => {
    let start = el.selectionStart === null ? el.value.length : el.selectionStart; const end = el.selectionEnd === null ? start : el.selectionEnd;
    let to = end;
    if (start === end) { if (forward) to = Math.min(el.value.length, end + 1); else start = Math.max(0, start - 1); }
    if (start === to) return;
    el.value = el.value.slice(0, start) + el.value.slice(to);
    try { el.setSelectionRange(start, start); } catch (e) {}
    el.dispatchEvent(new InputEvent('input', { bubbles: true, inputType: forward ? 'deleteContentForward' : 'deleteContentBackward' }));
  };
  const modifiers = { shift: false, ctrl: false, alt: false, meta: false };
  const keyDown = (target, value) => {
    const info = keyInfo(value);
    if (info.key === 'Shift') modifiers.shift = true; else if (info.key === 'Control') modifiers.ctrl = true; else if (info.key === 'Alt') modifiers.alt = true; else if (info.key === 'Meta') modifiers.meta = true;
    const init = { bubbles: true, cancelable: true, composed: true, view: globalThis, key: info.key, code: info.code, keyCode: info.keyCode, which: info.keyCode, shiftKey: modifiers.shift, ctrlKey: modifiers.ctrl, altKey: modifiers.alt, metaKey: modifiers.meta };
    activate();
    const proceed = target.dispatchEvent(new KeyboardEvent('keydown', init));
    if (!proceed) return;
    if (info.printable && !modifiers.ctrl && !modifiers.meta) {
      const text = info.key === ' ' ? ' ' : info.key;
      if (target.dispatchEvent(new KeyboardEvent('keypress', { ...init, charCode: text.charCodeAt(0) })) && editable(target)) insert(target, text);
    } else if (editable(target) && info.key === 'Backspace') erase(target, false);
    else if (editable(target) && info.key === 'Delete') erase(target, true);
    else if (info.key === 'Enter' && (target instanceof HTMLButtonElement || target instanceof HTMLAnchorElement)) target.click();
  };
  const keyUp = (target, value) => {
    const info = keyInfo(value);
    if (info.key === 'Shift') modifiers.shift = false; else if (info.key === 'Control') modifiers.ctrl = false; else if (info.key === 'Alt') modifiers.alt = false; else if (info.key === 'Meta') modifiers.meta = false;
    target.dispatchEvent(new KeyboardEvent('keyup', { bubbles: true, cancelable: true, composed: true, view: globalThis, key: info.key, code: info.code, keyCode: info.keyCode, which: info.keyCode, shiftKey: modifiers.shift, ctrlKey: modifiers.ctrl, altKey: modifiers.alt, metaKey: modifiers.meta }));
  };
  const centerOf = (el) => { const r = el.getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; };
  const focusFor = (target) => {
    for (let p = target; p; p = p.parentNode instanceof Element ? p.parentNode : null) {
      if (typeof p.focus === 'function') { p.focus(); if (document.activeElement === p) return; }
    }
    if (document.activeElement && document.activeElement !== document.body) document.activeElement.blur();
  };
  // The pointer: where it is, what is under it and the buttons held; with the pointer locked its events are the lock target's and tell how far it moved.
  const pointers = new Map();
  const pointerState = (id) => { if (!pointers.has(id)) pointers.set(id, { x: 0, y: 0, target: null, buttons: 0, downTarget: null }); return pointers.get(id); };
  const fire = (state, type, button, extra) => {
    const locked = document.pointerLockElement;
    const target = locked || state.target || document.body || document.documentElement;
    const init = { bubbles: true, cancelable: true, composed: true, view: globalThis, clientX: state.x, clientY: state.y, screenX: state.x, screenY: state.y, button, buttons: state.buttons, shiftKey: modifiers.shift, ctrlKey: modifiers.ctrl, altKey: modifiers.alt, metaKey: modifiers.meta, ...extra };
    return target.dispatchEvent(new MouseEvent(type, init));
  };
  const firePointer = (state, type, button, extra) => {
    if (typeof PointerEvent !== 'function') return;
    const target = document.pointerLockElement || state.target || document.body || document.documentElement;
    target.dispatchEvent(new PointerEvent(type, { bubbles: true, cancelable: type !== 'pointerenter' && type !== 'pointerleave', composed: true, view: globalThis, pointerId: 1, pointerType: 'mouse', isPrimary: true,
      clientX: state.x, clientY: state.y, button, buttons: state.buttons, ...extra }));
  };
  const underPoint = (x, y) => (typeof document.elementFromPoint === 'function' && document.elementFromPoint(x, y)) || document.body;
  const pointerMove = (state, action) => {
    let x = action.x || 0, y = action.y || 0;
    const origin = action.origin === undefined ? 'viewport' : action.origin;
    let target = null;
    if (origin === 'pointer') { x += state.x; y += state.y; }
    else if (origin !== 'viewport') { const [cx, cy] = centerOf(origin); x += cx; y += cy; target = origin; }
    const dx = x - state.x, dy = y - state.y;
    state.x = x; state.y = y;
    if (document.pointerLockElement) { const at = { movementX: dx, movementY: dy, clientX: state.lockX === undefined ? x : state.lockX, clientY: state.lockY === undefined ? y : state.lockY }; firePointer(state, 'pointermove', -1, at); fire(state, 'mousemove', 0, at); return; }
    const before = state.target;
    state.target = target || underPoint(x, y);
    if (before !== state.target) {
      if (before) { fire({ ...state, target: before }, 'mouseout', 0); }
      fire(state, 'mouseover', 0);
    }
    firePointer(state, 'pointermove', -1, { movementX: dx, movementY: dy });
    fire(state, 'mousemove', 0, { movementX: dx, movementY: dy });
  };
  const pointerDown = (state, button) => {
    activate();
    state.buttons |= 1 << (button === 1 ? 2 : button === 2 ? 1 : 0);
    if (document.pointerLockElement) state.lockX = state.x, state.lockY = state.y;
    state.downTarget = document.pointerLockElement || state.target;
    firePointer(state, 'pointerdown', button);
    if (fire(state, 'mousedown', button, { detail: 1 }) && button === 0 && state.target && !document.pointerLockElement) focusFor(state.target);
  };
  const pointerUp = (state, button) => {
    activate();
    state.buttons &= ~(1 << (button === 1 ? 2 : button === 2 ? 1 : 0));
    firePointer(state, 'pointerup', button);
    fire(state, 'mouseup', button, { detail: 1 });
    const target = document.pointerLockElement || state.target;
    if (target && target === state.downTarget) {
      if (button === 0 && typeof target.click === 'function' && !document.pointerLockElement) target.click();
      else fire(state, button === 0 ? 'click' : button === 2 ? 'contextmenu' : 'auxclick', button, { detail: 1 });
    }
    state.downTarget = null;
  };
  const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
  const driver = {
    bidi,
    click: async (element) => {
      const [x, y] = centerOf(element);
      const state = pointerState('click');
      state.x = x; state.y = y; state.target = element; state.buttons = 0;
      fire(state, 'mouseover', 0); fire(state, 'mousemove', 0);
      pointerDown(state, 0); pointerUp(state, 0);
    },
    send_keys: async (element, keys) => {
      if (element !== document.activeElement && typeof element.focus === 'function') element.focus();
      const target = document.activeElement && document.activeElement !== document.body ? document.activeElement : element;
      for (const ch of Array.from(String(keys))) { keyDown(target, ch); keyUp(target, ch); }
    },
    bless: async (intent, action) => { activate(); return typeof action === 'function' ? action() : undefined; },
    action_sequence: async (sources) => {
      const ticks = Math.max(0, ...sources.map((s) => s.actions.length));
      for (let i = 0; i < ticks; i++) {
        let wait = 0;
        for (const source of sources) {
          const action = source.actions[i];
          if (!action) continue;
          if (action.type === 'pause') { wait = Math.max(wait, action.duration || 0); continue; }
          if (source.type === 'pointer') {
            const state = pointerState(source.id);
            if (action.type === 'pointerMove') pointerMove(state, action);
            else if (action.type === 'pointerDown') pointerDown(state, action.button || 0);
            else if (action.type === 'pointerUp') pointerUp(state, action.button || 0);
          } else if (source.type === 'key') {
            const target = document.activeElement || document.body;
            if (action.type === 'keyDown') keyDown(target, action.value); else if (action.type === 'keyUp') keyUp(target, action.value);
          }
        }
        if (wait) await sleep(wait);
      }
    },
    set_permission: async () => {},
    set_test_context: () => {},
    minimize_window: async () => {},
    set_window_rect: async () => {},
    get_virtual_sensor_information: undefined,
  };
  for (const name of Object.keys(driver)) if (driver[name] === undefined) delete driver[name];
  globalThis.test_driver = new Proxy(driver, {
    get: (target, name) => (name in target ? target[name] : typeof name === 'string' ? () => Promise.reject(new Error('test_driver.' + name + ' is not available')) : undefined),
  });
  globalThis.test_driver_internal = globalThis.test_driver;

  // assert_implements(condition, description) fails a test that needs what is not there; the optional one only
  // leaves the test aside, as a precondition that did not hold.
  globalThis.assert_implements = (condition, description) => {
    if (!condition) fail('assert_implements: ' + (description || 'a feature is not implemented'), description);
  };
  globalThis.assert_implements_optional = (condition, description) => {
    if (!condition) {
      const error = new Error('precondition failed: ' + (description || ''));
      error.__precondition = true;
      throw error;
    }
  };

  // generate_tests(func, [[name, ...args], ...]): a test of each, which calls func with the args.
  globalThis.generate_tests = (func, args, properties) => {
    for (const row of args) {
      const [name, ...params] = row;
      globalThis.test(function () { return func.apply(this, params); }, name, properties);
    }
  };

  // Setup code is just run; a test file that names its setup tests asynchronously is not supported.
  globalThis.setup = (func) => { if (typeof func === 'function') func(); };

  const messageOf = (e) => String(e && e.message !== undefined ? e.message : e);

  // What testharness passes to a test as `this` and as the argument of its function. `fail` is how a
  // step that throws, or an unreached function, ends the test.
  const makeTestObject = (fail) => {
    const cleanups = [];
    const t = {
      add_cleanup: (f) => cleanups.push(f),
      runCleanups: () => { for (const f of cleanups.splice(0).reverse()) f(); },
      step: (f, ...args) => {
        try { return f.apply(t, args); } catch (e) { fail(e); }
      },
      step_func: (f) => function (...args) { try { return f.apply(t, args); } catch (e) { fail(e); } },
      unreached_func: (message) => () => fail(new Error('unreachable test function was called: ' + message)),
      step_timeout: (f, ms) => setTimeout(t.step_func(f), ms),
    };
    return t;
  };

  globalThis.test = (func, name) => {
    if (isSkipped(name)) {
      skipped++;
      return;
    }
    let failure = null;
    const t = makeTestObject((e) => { failure = failure || e; });
    try {
      func.call(t, t);
    } catch (e) {
      failure = failure || e;
    } finally {
      t.runCleanups();
    }
    results.push(failure && !failure.__precondition ? { name, ok: false, message: messageOf(failure) } : { name, ok: true });
  };

  // The tests that were started and have not ended: when the run is over, they are the ones that timed out.
  const unfinished = new Set();

  // As in testharness, promise tests run one after another: each starts when the one before it is done.
  let promiseTests = Promise.resolve();
  globalThis.promise_test = (func, name) => {
    if (isSkipped(name)) {
      skipped++;
      return;
    }
    const entry = { name };
    unfinished.add(entry);
    promiseTests = promiseTests.then(() => {
      let failure = null;
      const t = makeTestObject((e) => { failure = failure || e; });
      return Promise.resolve()
        .then(() => func.call(t, t))
        .catch((e) => { failure = failure || e; })
        .then(() => {
          t.runCleanups();
          unfinished.delete(entry);
          results.push(failure && !failure.__precondition ? { name, ok: false, message: messageOf(failure) } : { name, ok: true });
        });
    });
    pending.push(promiseTests);
  };

  // A test that ends when it says so, from a callback or a timer.
  globalThis.async_test = (func, name) => {
    if (typeof func !== 'function') { name = func; func = null; }
    if (isSkipped(name)) {
      skipped++;
      return makeTestObject(() => {});
    }
    let finished = false;
    let finish;
    const entry = { name };
    unfinished.add(entry);
    pending.push(new Promise((resolve) => { finish = resolve; }));
    const end = (failure) => {
      if (finished) return;
      finished = true;
      unfinished.delete(entry);
      t.runCleanups();
      results.push(failure && !failure.__precondition ? { name, ok: false, message: messageOf(failure) } : { name, ok: true });
      finish();
    };
    const t = makeTestObject((e) => end(e));
    t.done = () => end(null);
    t.step_func_done = (f) => function (...args) {
      try { if (f) f.apply(t, args); } catch (e) { return end(e); }
      end(null);
    };
    if (func) {
      try { func.call(t, t); } catch (e) { end(e); }
    }
    return t;
  };

  // EventWatcher, as testharness has it: the events of the types watched come as promises, and an event nobody is waiting for fails the test.
  globalThis.EventWatcher = function EventWatcher(test, watchedNode, eventTypes, timeoutPromise) {
    if (typeof eventTypes === 'string') eventTypes = [eventTypes];
    const waitingFor = { current: null };
    let recordedEvents = null;
    const fail = (message) => { throw new Error(message); };
    const eventHandler = test.step_func(function (evt) {
      assert_true(!!waitingFor.current, 'Not expecting event, but got ' + evt.type + ' event');
      assert_equals(evt.type, waitingFor.current.types[0], 'Expected ' + waitingFor.current.types[0] + ' event, but got ' + evt.type + ' event instead');
      if (Array.isArray(recordedEvents)) recordedEvents.push(evt);
      if (waitingFor.current.types.length > 1) {
        waitingFor.current.types.shift();
        return;
      }
      const resolveFunc = waitingFor.current.resolve;
      waitingFor.current = null;
      const result = recordedEvents || evt;
      recordedEvents = null;
      resolveFunc(result);
    });
    for (const type of eventTypes) watchedNode.addEventListener(type, eventHandler, false);
    this.wait_for = function (types, options) {
      if (waitingFor.current) return Promise.reject('Already waiting for an event or events');
      if (typeof types === 'string') types = [types];
      if (options && options.record && options.record === 'all') recordedEvents = [];
      return new Promise((resolve, reject) => {
        const timeout = test.step_func(function () {
          // If the timeout fires after the events have been received or during a later wait_for, it is ignored.
          if (!waitingFor.current || waitingFor.current.resolve !== resolve) return;
          assert_true(waitingFor.current.types.length === 0, 'Timed out waiting for ' + waitingFor.current.types.join(', '));
          const result = recordedEvents;
          recordedEvents = null;
          const resolveFunc = waitingFor.current.resolve;
          waitingFor.current = null;
          resolveFunc(result);
        });
        if (timeoutPromise) timeoutPromise().then(timeout);
        waitingFor.current = { types: types.slice(), resolve, reject };
      });
    };
    this.stop_watching = function () {
      for (const type of eventTypes) watchedNode.removeEventListener(type, eventHandler, false);
    };
    test.add_cleanup(this.stop_watching);
    return this;
  };

  // The subset-tests-by-key helper only narrows a run by a URL variant; there is none here.
  globalThis.subsetTestByKey = (key, testFunc, ...args) => testFunc(...args);

  // The URL tests read their data with fetch("resources/..."), which here comes from the files
  // tests/data holds. A real fetch, when the realm has one, still answers for everything else.
  const realFetch = globalThis.fetch;
  globalThis.fetch = (path, ...rest) => {
    const name = String(path).split('/').pop();
    if (realFetch && !(name in globalThis.__resources)) return realFetch(path, ...rest);
    return Promise.resolve({ json: () => Promise.resolve(globalThis.__resources[name]) });
  };

  globalThis.__wptFinish = () => {
    for (const entry of unfinished) results.push({ name: entry.name, ok: false, message: 'the test never finished' });
    const failures = results.filter((r) => !r.ok);
    for (const r of failures.slice(0, globalThis.__allFailures ? 100000 : 15)) console.log('  FAIL ' + r.name + ': ' + r.message);
    if (failures.length > (globalThis.__allFailures ? 100000 : 15)) console.log('  ... and ' + (failures.length - 15) + ' more');
    console.log(results.length - failures.length + '/' + results.length + ' passed' + (skipped ? ', ' + skipped + ' skipped' : ''));
    if (failures.length) throw new Error(failures.length + ' tests failed');
  };
})();

// GLOBAL, which tells a test what kind of global it is running in: a window when there is a document, otherwise not.
if (typeof self === "undefined") globalThis.self = globalThis;
globalThis.GLOBAL = {
  isWindow: () => typeof window !== "undefined" && typeof document !== "undefined",
  isWorker: () => false,
  isShadowRealm: () => false,
};

// A page has a window.location of its own; a script run without a page is given the address WPT serves tests from.
if (typeof location === "undefined") {
  globalThis.location = { href: "http://web-platform.test:8000/", protocol: "http:", host: "web-platform.test:8000", hostname: "web-platform.test", port: "8000", pathname: "/", search: "", hash: "", origin: "http://web-platform.test:8000" };
}
