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

  function isSkipped(name) {
    return skips.some((prefix) => String(name).startsWith(prefix));
  }

  globalThis.test = (func, name) => {
    if (isSkipped(name)) {
      skipped++;
      return;
    }
    try {
      func();
      results.push({ name, ok: true });
    } catch (e) {
      results.push({ name, ok: false, message: String(e && e.message !== undefined ? e.message : e) });
    }
  };

  globalThis.promise_test = (func, name) => {
    if (isSkipped(name)) {
      skipped++;
      return;
    }
    pending.push(
      Promise.resolve()
        .then(() => func())
        .then(
          () => results.push({ name, ok: true }),
          (e) => results.push({ name, ok: false, message: String(e && e.message !== undefined ? e.message : e) })
        )
    );
  };

  // The subset-tests-by-key helper only narrows a run by a URL variant; there is none here.
  globalThis.subsetTestByKey = (key, testFunc, ...args) => testFunc(...args);

  globalThis.fetch = (path) => {
    const data = globalThis.__resources[String(path).split('/').pop()];
    return Promise.resolve({ json: () => Promise.resolve(data) });
  };

  globalThis.__wptFinish = () => {
    const failures = results.filter((r) => !r.ok);
    for (const r of failures.slice(0, 15)) console.log('  FAIL ' + r.name + ': ' + r.message);
    if (failures.length > 15) console.log('  ... and ' + (failures.length - 15) + ' more');
    console.log(results.length - failures.length + '/' + results.length + ' passed' + (skipped ? ', ' + skipped + ' skipped' : ''));
    if (failures.length) throw new Error(failures.length + ' tests failed');
  };
})();
