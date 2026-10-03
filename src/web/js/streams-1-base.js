// The Streams Standard (https://streams.spec.whatwg.org/), written as the script it is. It is one program,
// the files of src/web/js in order, run in each realm that has the Fetch API. Everything it uses from the
// language is taken once, here, before any page script can change it, and the standard's internal slots
// live in WeakMaps that only this program can reach.
(function (global) {
'use strict';

const { Object, Reflect, Symbol, Promise, TypeError, RangeError, Number, Math, Array, WeakMap, ArrayBuffer, DataView, Uint8Array, Error } = global;
const { defineProperty, getPrototypeOf, setPrototypeOf, create: objectCreate, getOwnPropertyNames, getOwnPropertyDescriptor } = Object;
const { apply: reflectApply, ownKeys, construct: reflectConstruct } = Reflect;
const { isNaN, isFinite } = Number;
const queueMicrotaskIntrinsic = global.queueMicrotask;

const PromisePrototypeThen = Promise.prototype.then;
const PromiseResolveIntrinsic = Promise.resolve;
const PromiseRejectIntrinsic = Promise.reject;
const WeakMapGet = WeakMap.prototype.get;
const WeakMapSet = WeakMap.prototype.set;
const WeakMapHas = WeakMap.prototype.has;
const ArrayPrototypePush = Array.prototype.push;
const ArrayPrototypeShift = Array.prototype.shift;
const ArrayPrototypeSplice = Array.prototype.splice;
const ArrayBufferPrototypeSlice = ArrayBuffer.prototype.slice;
const ArrayBufferByteLength = getOwnPropertyDescriptor(ArrayBuffer.prototype, 'byteLength').get;
const ArrayBufferDetached = getOwnPropertyDescriptor(ArrayBuffer.prototype, 'detached').get;
const ArrayBufferTransfer = ArrayBuffer.prototype.transfer;
const TypedArrayPrototype = getPrototypeOf(Uint8Array.prototype);
const TypedArrayBufferGetter = getOwnPropertyDescriptor(TypedArrayPrototype, 'buffer').get;
const TypedArrayByteOffsetGetter = getOwnPropertyDescriptor(TypedArrayPrototype, 'byteOffset').get;
const TypedArrayByteLengthGetter = getOwnPropertyDescriptor(TypedArrayPrototype, 'byteLength').get;
const TypedArrayLengthGetter = getOwnPropertyDescriptor(TypedArrayPrototype, 'length').get;
const TypedArrayToStringTag = getOwnPropertyDescriptor(TypedArrayPrototype, Symbol.toStringTag).get;
const DataViewBufferGetter = getOwnPropertyDescriptor(DataView.prototype, 'buffer').get;
const DataViewByteOffsetGetter = getOwnPropertyDescriptor(DataView.prototype, 'byteOffset').get;
const DataViewByteLengthGetter = getOwnPropertyDescriptor(DataView.prototype, 'byteLength').get;
const AsyncIteratorPrototype = getPrototypeOf(getPrototypeOf(async function* () {}.prototype));

// ---- Promises, the way the standard names them ----

const promiseResolvedWith = (value) => reflectApply(PromiseResolveIntrinsic, Promise, [value]);
const promiseRejectedWith = (reason) => reflectApply(PromiseRejectIntrinsic, Promise, [reason]);
const newPromiseCapability = () => {
  let resolve, reject;
  const promise = new Promise((res, rej) => { resolve = res; reject = rej; });
  return { promise, resolve, reject };
};
// Reacting to a promise without making a promise that someone might leave unhandled.
const uponPromise = (promise, onFulfilled, onRejected) => {
  reflectApply(PromisePrototypeThen, promise, [onFulfilled, onRejected]);
};
const uponFulfillment = (promise, onFulfilled) => uponPromise(promise, onFulfilled, undefined);
const uponRejection = (promise, onRejected) => uponPromise(promise, undefined, onRejected);
const transformPromise = (promise, onFulfilled, onRejected) => reflectApply(PromisePrototypeThen, promise, [onFulfilled, onRejected]);
const setPromiseIsHandledToTrue = (promise) => { reflectApply(PromisePrototypeThen, promise, [undefined, () => {}]); };
// "Invoke a callback and give a promise for the result", for the algorithms that may throw.
const promiseCall = (func, thisArg, args) => {
  try {
    return promiseResolvedWith(reflectApply(func, thisArg, args));
  } catch (e) {
    return promiseRejectedWith(e);
  }
};
const queueMicrotask = (steps) => { reflectApply(queueMicrotaskIntrinsic, global, [steps]); };

// ---- Slots ----
// A class's internal slots are an object in a WeakMap; "has the slot" is "is in the map".
const slotsOf = (map) => ({
  get: (o) => reflectApply(WeakMapGet, map, [o]),
  has: (o) => reflectApply(WeakMapHas, map, [o]),
  set: (o, v) => { reflectApply(WeakMapSet, map, [o, v]); },
});

// ---- Interfaces ----
// Web IDL makes operations and attributes enumerable, which class syntax does not.
const idlify = (Class, constants) => {
  for (const target of [Class.prototype, Class]) {
    for (const key of ownKeys(target)) {
      if (key === 'constructor' || key === 'prototype' || key === 'length' || key === 'name') continue;
      const descriptor = getOwnPropertyDescriptor(target, key);
      if (descriptor.enumerable === false && key !== Symbol.toStringTag && key !== Symbol.asyncIterator) {
        defineProperty(target, key, { ...descriptor, enumerable: true });
      }
    }
  }
  defineProperty(Class.prototype, Symbol.toStringTag, { value: Class.name, configurable: true });
};
const exposeGlobal = (Class) => {
  defineProperty(global, Class.name, { value: Class, writable: true, enumerable: false, configurable: true });
};
// A "constructor" that cannot be called, for the classes only the standard makes.
const illegalConstructor = () => { throw new TypeError('Illegal constructor'); };

// ---- Numbers and values ----

const isNonNegativeNumber = (v) => typeof v === 'number' && !isNaN(v) && v >= 0;
const isObject = (v) => (typeof v === 'object' && v !== null) || typeof v === 'function';
const isCallable = (v) => typeof v === 'function';

// A queue with sizes: [[queue]] and [[queueTotalSize]] of a controller.
const enqueueValueWithSize = (controller, value, size) => {
  size = Number(size);
  if (!isNonNegativeNumber(size) || size === Infinity) throw new RangeError('The return value of a queuing strategy\'s size function must be a finite, non-NaN, non-negative number');
  reflectApply(ArrayPrototypePush, controller.queue, [{ value, size }]);
  controller.queueTotalSize += size;
};
const dequeueValue = (controller) => {
  const pair = reflectApply(ArrayPrototypeShift, controller.queue, []);
  controller.queueTotalSize -= pair.size;
  if (controller.queueTotalSize < 0) controller.queueTotalSize = 0;
  return pair.value;
};
const peekQueueValue = (controller) => controller.queue[0].value;
const resetQueue = (controller) => {
  controller.queue = [];
  controller.queueTotalSize = 0;
};

// ---- Queuing strategies ----

const convertQueuingStrategy = (strategy, context) => {
  if (strategy === undefined || strategy === null) return {};
  if (!isObject(strategy)) throw new TypeError(context + ' is not an object');
  const result = {};
  const highWaterMark = strategy.highWaterMark;
  if (highWaterMark !== undefined) result.highWaterMark = Number(highWaterMark);
  const size = strategy.size;
  if (size !== undefined) {
    if (!isCallable(size)) throw new TypeError(context + ' has a size that is not a function');
    result.size = size;
  }
  return result;
};
const validateAndNormalizeHighWaterMark = (highWaterMark) => {
  highWaterMark = Number(highWaterMark);
  if (isNaN(highWaterMark) || highWaterMark < 0) throw new RangeError('A queuing strategy\'s highWaterMark must be a non-negative, non-NaN number');
  return highWaterMark;
};
const extractHighWaterMark = (strategy, defaultHighWaterMark) => {
  if (strategy.highWaterMark === undefined) return defaultHighWaterMark;
  return validateAndNormalizeHighWaterMark(strategy.highWaterMark);
};
const extractSizeAlgorithm = (strategy) => {
  const size = strategy.size;
  if (size === undefined) return () => 1;
  return (chunk) => reflectApply(size, undefined, [chunk]);
};

// Methods, so that these are not constructors and have no prototype.
const countSize = ({ size() { return 1; } }).size;
const byteLengthSize = ({ size(chunk) { return chunk.byteLength; } }).size;
const strategySlots = slotsOf(new WeakMap());

class CountQueuingStrategy {
  constructor(init) {
    if (arguments.length < 1) throw new TypeError('Failed to construct \'CountQueuingStrategy\': 1 argument required, but only 0 present.');
    if (!isObject(init)) throw new TypeError('Failed to construct \'CountQueuingStrategy\': The provided value is not of type \'QueuingStrategyInit\'.');
    const highWaterMark = init.highWaterMark;
    if (highWaterMark === undefined) throw new TypeError('Failed to construct \'CountQueuingStrategy\': required member highWaterMark is undefined.');
    strategySlots.set(this, { highWaterMark: Number(highWaterMark) });
  }
  get highWaterMark() {
    if (!strategySlots.has(this)) throw new TypeError('Illegal invocation');
    return strategySlots.get(this).highWaterMark;
  }
  get size() {
    if (!strategySlots.has(this)) throw new TypeError('Illegal invocation');
    return countSize;
  }
}
idlify(CountQueuingStrategy);

class ByteLengthQueuingStrategy {
  constructor(init) {
    if (arguments.length < 1) throw new TypeError('Failed to construct \'ByteLengthQueuingStrategy\': 1 argument required, but only 0 present.');
    if (!isObject(init)) throw new TypeError('Failed to construct \'ByteLengthQueuingStrategy\': The provided value is not of type \'QueuingStrategyInit\'.');
    const highWaterMark = init.highWaterMark;
    if (highWaterMark === undefined) throw new TypeError('Failed to construct \'ByteLengthQueuingStrategy\': required member highWaterMark is undefined.');
    strategySlots.set(this, { highWaterMark: Number(highWaterMark) });
  }
  get highWaterMark() {
    if (!strategySlots.has(this)) throw new TypeError('Illegal invocation');
    return strategySlots.get(this).highWaterMark;
  }
  get size() {
    if (!strategySlots.has(this)) throw new TypeError('Illegal invocation');
    return byteLengthSize;
  }
}
idlify(ByteLengthQueuingStrategy);

exposeGlobal(CountQueuingStrategy);
exposeGlobal(ByteLengthQueuingStrategy);
