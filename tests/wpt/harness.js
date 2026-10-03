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

  // A skip is the start of a test's name, or, if it begins with "*", a part of it.
  function isSkipped(name) {
    return skips.some((skip) => (skip.startsWith('*') ? String(name).includes(skip.slice(1)) : String(name).startsWith(skip)));
  }

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
    results.push(failure ? { name, ok: false, message: messageOf(failure) } : { name, ok: true });
  };

  // As in testharness, promise tests run one after another: each starts when the one before it is done.
  let promiseTests = Promise.resolve();
  globalThis.promise_test = (func, name) => {
    if (isSkipped(name)) {
      skipped++;
      return;
    }
    promiseTests = promiseTests.then(() => {
      let failure = null;
      const t = makeTestObject((e) => { failure = failure || e; });
      return Promise.resolve()
        .then(() => func.call(t, t))
        .catch((e) => { failure = failure || e; })
        .then(() => {
          t.runCleanups();
          results.push(failure ? { name, ok: false, message: messageOf(failure) } : { name, ok: true });
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
    pending.push(new Promise((resolve) => { finish = resolve; }));
    const end = (failure) => {
      if (finished) return;
      finished = true;
      t.runCleanups();
      results.push(failure ? { name, ok: false, message: messageOf(failure) } : { name, ok: true });
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
    const failures = results.filter((r) => !r.ok);
    for (const r of failures.slice(0, 15)) console.log('  FAIL ' + r.name + ': ' + r.message);
    if (failures.length > 15) console.log('  ... and ' + (failures.length - 15) + ' more');
    console.log(results.length - failures.length + '/' + results.length + ' passed' + (skipped ? ', ' + skipped + ' skipped' : ''));
    if (failures.length) throw new Error(failures.length + ' tests failed');
  };
})();
