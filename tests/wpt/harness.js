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
    const name = legacyNames[type] || type;
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
  globalThis.test_driver = new Proxy({}, {
    get: (target, name) => (typeof name === 'string' ? () => Promise.reject(new Error('test_driver.' + name + ' is not available')) : undefined),
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
    for (const r of failures.slice(0, 15)) console.log('  FAIL ' + r.name + ': ' + r.message);
    if (failures.length > 15) console.log('  ... and ' + (failures.length - 15) + ' more');
    console.log(results.length - failures.length + '/' + results.length + ' passed' + (skipped ? ', ' + skipped + ' skipped' : ''));
    if (failures.length) throw new Error(failures.length + ' tests failed');
  };
})();

// A page has a window.location of its own; a script run without a page is given the address WPT serves tests from.
if (typeof location === "undefined") {
  globalThis.location = { href: "http://web-platform.test:8000/", protocol: "http:", host: "web-platform.test:8000", hostname: "web-platform.test", port: "8000", pathname: "/", search: "", hash: "", origin: "http://web-platform.test:8000" };
}
