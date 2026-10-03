
// ======================================================================================
// ReadableStream
// ======================================================================================

const streamSlots = slotsOf(new WeakMap());          // a ReadableStream's
const defaultReaderSlots = slotsOf(new WeakMap());   // a ReadableStreamDefaultReader's
const defaultControllerSlots = slotsOf(new WeakMap());

const isReadableStream = (x) => streamSlots.has(x);
const isReadableStreamDefaultReader = (x) => defaultReaderSlots.has(x);
const isReadableStreamDefaultController = (x) => defaultControllerSlots.has(x);

// The dictionaries of the constructor, converted when the standard says.
const convertUnderlyingSource = (source) => {
  if (source === undefined) return {};
  if (!isObject(source)) throw new TypeError('The underlying source is not an object');
  const result = {};
  // Members are read in alphabetical order.
  const autoAllocateChunkSize = source.autoAllocateChunkSize;
  if (autoAllocateChunkSize !== undefined) {
    const n = Number(autoAllocateChunkSize);
    if (!isFinite(n) || n < 0 || n > 18446744073709551615) throw new TypeError('autoAllocateChunkSize is out of range');
    result.autoAllocateChunkSize = Math.trunc(n);
  }
  const cancel = source.cancel;
  if (cancel !== undefined) {
    if (!isCallable(cancel)) throw new TypeError('The underlying source\'s cancel is not a function');
    result.cancel = cancel;
  }
  const pull = source.pull;
  if (pull !== undefined) {
    if (!isCallable(pull)) throw new TypeError('The underlying source\'s pull is not a function');
    result.pull = pull;
  }
  const start = source.start;
  if (start !== undefined) {
    if (!isCallable(start)) throw new TypeError('The underlying source\'s start is not a function');
    result.start = start;
  }
  const type = source.type;
  if (type !== undefined) {
    const text = String(type);
    if (text !== 'bytes') throw new TypeError('The underlying source\'s type is not a valid ReadableStreamType');
    result.type = text;
  }
  return result;
};

const isReadableStreamLocked = (stream) => streamSlots.get(stream).reader !== undefined;

// ---- The stream ----

class ReadableStream {
  constructor(underlyingSource = undefined, strategy = {}) {
    const strategyDict = convertQueuingStrategy(strategy, 'The queuing strategy');
    const sourceDict = convertUnderlyingSource(underlyingSource);
    streamSlots.set(this, { state: 'readable', reader: undefined, storedError: undefined, disturbed: false, controller: undefined });
    if (sourceDict.type === 'bytes') {
      if (strategyDict.size !== undefined) throw new RangeError('The strategy for a byte stream cannot have a size function');
      const highWaterMark = extractHighWaterMark(strategyDict, 0);
      setUpReadableByteStreamControllerFromUnderlyingSource(this, underlyingSource, sourceDict, highWaterMark);
    } else {
      const sizeAlgorithm = extractSizeAlgorithm(strategyDict);
      const highWaterMark = extractHighWaterMark(strategyDict, 1);
      setUpReadableStreamDefaultControllerFromUnderlyingSource(this, underlyingSource, sourceDict, highWaterMark, sizeAlgorithm);
    }
  }

  get locked() {
    if (!isReadableStream(this)) throw new TypeError('Illegal invocation');
    return isReadableStreamLocked(this);
  }

  cancel(reason = undefined) {
    if (!isReadableStream(this)) return promiseRejectedWith(new TypeError('Illegal invocation'));
    if (isReadableStreamLocked(this)) return promiseRejectedWith(new TypeError('Cannot cancel a stream that already has a reader'));
    return readableStreamCancel(this, reason);
  }

  getReader(options = undefined) {
    if (!isReadableStream(this)) throw new TypeError('Illegal invocation');
    let mode;
    if (options !== undefined && options !== null) {
      if (!isObject(options)) throw new TypeError('The options are not an object');
      mode = options.mode;
      if (mode !== undefined) {
        mode = String(mode);
        if (mode !== 'byob') throw new TypeError('The reader mode is not a valid ReadableStreamReaderMode');
      }
    }
    if (mode === undefined) return acquireReadableStreamDefaultReader(this);
    return acquireReadableStreamBYOBReader(this);
  }

  pipeThrough(transform, options = {}) {
    if (!isReadableStream(this)) throw new TypeError('Illegal invocation');
    if (arguments.length < 1) throw new TypeError('Failed to execute \'pipeThrough\' on \'ReadableStream\': 1 argument required, but only 0 present.');
    if (!isObject(transform)) throw new TypeError('The transform is not an object');
    // The pair is converted a member at a time, readable first, and writable is not touched if readable will not do.
    const readable = transform.readable;
    if (!isReadableStream(readable)) throw new TypeError('The transform\'s readable is not a ReadableStream');
    const writable = transform.writable;
    if (!isWritableStream(writable)) throw new TypeError('The transform\'s writable is not a WritableStream');
    const dict = convertPipeOptions(options);
    if (isReadableStreamLocked(this)) throw new TypeError('Cannot pipe a stream that is locked');
    if (isWritableStreamLocked(writable)) throw new TypeError('Cannot pipe to a stream that is locked');
    const promise = readableStreamPipeTo(this, writable, dict.preventClose, dict.preventAbort, dict.preventCancel, dict.signal);
    setPromiseIsHandledToTrue(promise);
    return readable;
  }

  pipeTo(destination, options = {}) {
    if (!isReadableStream(this)) return promiseRejectedWith(new TypeError('Illegal invocation'));
    if (arguments.length < 1) return promiseRejectedWith(new TypeError('Failed to execute \'pipeTo\' on \'ReadableStream\': 1 argument required, but only 0 present.'));
    let dict;
    try {
      if (!isWritableStream(destination)) throw new TypeError('The destination is not a WritableStream');
      dict = convertPipeOptions(options);
    } catch (e) {
      return promiseRejectedWith(e);
    }
    if (isReadableStreamLocked(this)) return promiseRejectedWith(new TypeError('Cannot pipe a stream that is locked'));
    if (isWritableStreamLocked(destination)) return promiseRejectedWith(new TypeError('Cannot pipe to a stream that is locked'));
    return readableStreamPipeTo(this, destination, dict.preventClose, dict.preventAbort, dict.preventCancel, dict.signal);
  }

  tee() {
    if (!isReadableStream(this)) throw new TypeError('Illegal invocation');
    return readableStreamTee(this, false);
  }

  values(options = undefined) {
    if (!isReadableStream(this)) throw new TypeError('Illegal invocation');
    let preventCancel = false;
    if (options !== undefined && options !== null) {
      if (!isObject(options)) throw new TypeError('The options are not an object');
      preventCancel = Boolean(options.preventCancel);
    }
    const reader = acquireReadableStreamDefaultReader(this);
    return createReadableStreamAsyncIterator(reader, preventCancel);
  }

  static from(asyncIterable) {
    if (arguments.length < 1) throw new TypeError('Failed to execute \'from\' on \'ReadableStream\': 1 argument required, but only 0 present.');
    return readableStreamFromIterable(asyncIterable);
  }
}
defineProperty(ReadableStream.prototype, Symbol.asyncIterator, { value: ReadableStream.prototype.values, writable: true, enumerable: false, configurable: true });
idlify(ReadableStream);
// idlify turned @@asyncIterator back to what it was; the standard's @@asyncIterator is the very function `values`.
defineProperty(ReadableStream.prototype, Symbol.asyncIterator, { value: ReadableStream.prototype.values, writable: true, enumerable: false, configurable: true });

const convertPipeOptions = (options) => {
  const dict = { preventAbort: false, preventCancel: false, preventClose: false, signal: undefined };
  if (options === undefined || options === null) return dict;
  if (!isObject(options)) throw new TypeError('The options are not an object');
  // alphabetical: preventAbort, preventCancel, preventClose, signal
  dict.preventAbort = Boolean(options.preventAbort);
  dict.preventCancel = Boolean(options.preventCancel);
  dict.preventClose = Boolean(options.preventClose);
  const signal = options.signal;
  if (signal !== undefined) {
    if (!isAbortSignal(signal)) throw new TypeError('The signal is not an AbortSignal');
    dict.signal = signal;
  }
  return dict;
};
const isAbortSignal = (x) => {
  if (!isObject(x)) return false;
  try {
    reflectApply(AbortSignalAbortedGetter, x, []);  // throws for anything that is no AbortSignal
    return true;
  } catch (e) {
    return false;
  }
};

// ---- Abstract operations on a stream ----

const acquireReadableStreamDefaultReader = (stream) => {
  const reader = objectCreate(ReadableStreamDefaultReader.prototype);
  setUpReadableStreamDefaultReader(reader, stream);
  return reader;
};

// The controller object of each stream the standard makes itself (tee, from, and the transform's readable).
const readableStreamControllers = new WeakMap();

const createReadableStream = (startAlgorithm, pullAlgorithm, cancelAlgorithm, highWaterMark = 1, sizeAlgorithm = () => 1) => {
  const stream = objectCreate(ReadableStream.prototype);
  streamSlots.set(stream, { state: 'readable', reader: undefined, storedError: undefined, disturbed: false, controller: undefined });
  const controller = objectCreate(ReadableStreamDefaultController.prototype);
  reflectApply(WeakMapSet, readableStreamControllers, [stream, controller]);
  setUpReadableStreamDefaultController(stream, controller, startAlgorithm, pullAlgorithm, cancelAlgorithm, highWaterMark, sizeAlgorithm);
  return stream;
};

const readableStreamAddReadRequest = (stream, readRequest) => {
  reflectApply(ArrayPrototypePush, defaultReaderSlots.get(streamSlots.get(stream).reader).readRequests, [readRequest]);
};

const readableStreamCancel = (stream, reason) => {
  const slots = streamSlots.get(stream);
  slots.disturbed = true;
  if (slots.state === 'closed') return promiseResolvedWith(undefined);
  if (slots.state === 'errored') return promiseRejectedWith(slots.storedError);
  readableStreamClose(stream);
  const reader = slots.reader;
  if (reader !== undefined && isReadableStreamBYOBReader(reader)) {
    const readIntoRequests = byobReaderSlots.get(reader).readIntoRequests;
    byobReaderSlots.get(reader).readIntoRequests = [];
    for (const readIntoRequest of readIntoRequests) readIntoRequest.closeSteps(undefined);
  }
  const sourceCancelPromise = slots.controller.cancelSteps(reason);
  return transformPromise(sourceCancelPromise, () => undefined, undefined);
};

const readableStreamClose = (stream) => {
  const slots = streamSlots.get(stream);
  slots.state = 'closed';
  const reader = slots.reader;
  if (reader === undefined) return;
  readerGenericSlots(reader).closedPromise.resolve(undefined);
  if (isReadableStreamDefaultReader(reader)) {
    const readRequests = defaultReaderSlots.get(reader).readRequests;
    defaultReaderSlots.get(reader).readRequests = [];
    for (const readRequest of readRequests) readRequest.closeSteps();
  }
};

const readableStreamError = (stream, e) => {
  const slots = streamSlots.get(stream);
  slots.state = 'errored';
  slots.storedError = e;
  const reader = slots.reader;
  if (reader === undefined) return;
  readerGenericSlots(reader).closedPromise.reject(e);
  setPromiseIsHandledToTrue(readerGenericSlots(reader).closedPromise.promise);
  if (isReadableStreamDefaultReader(reader)) {
    readableStreamDefaultReaderErrorReadRequests(reader, e);
  } else {
    readableStreamBYOBReaderErrorReadIntoRequests(reader, e);
  }
};

const readableStreamFulfillReadRequest = (stream, chunk, done) => {
  const reader = streamSlots.get(stream).reader;
  const readRequest = reflectApply(ArrayPrototypeShift, defaultReaderSlots.get(reader).readRequests, []);
  if (done) readRequest.closeSteps();
  else readRequest.chunkSteps(chunk);
};

const readableStreamGetNumReadRequests = (stream) => defaultReaderSlots.get(streamSlots.get(stream).reader).readRequests.length;
const readableStreamHasDefaultReader = (stream) => {
  const reader = streamSlots.get(stream).reader;
  return reader !== undefined && isReadableStreamDefaultReader(reader);
};

// ---- Readers, generic ----

const readerSlotsAny = new WeakMap();  // the state both kinds of reader share, by reader
const readerGenericSlots = (reader) => reflectApply(WeakMapGet, readerSlotsAny, [reader]);

const readableStreamReaderGenericInitialize = (reader, stream) => {
  const closed = newPromiseCapability();
  reflectApply(WeakMapSet, readerSlotsAny, [reader, { stream, closedPromise: closed }]);
  streamSlots.get(stream).reader = reader;
  const state = streamSlots.get(stream).state;
  if (state === 'readable') {
    // pending
  } else if (state === 'closed') {
    closed.resolve(undefined);
  } else {
    closed.reject(streamSlots.get(stream).storedError);
    setPromiseIsHandledToTrue(closed.promise);
  }
};

const readableStreamReaderGenericCancel = (reader, reason) => {
  const stream = readerGenericSlots(reader).stream;
  return readableStreamCancel(stream, reason);
};

const readableStreamReaderGenericRelease = (reader) => {
  const generic = readerGenericSlots(reader);
  const stream = generic.stream;
  const slots = streamSlots.get(stream);
  const releasedError = new TypeError('The reader was released');
  if (slots.state === 'readable') {
    generic.closedPromise.reject(releasedError);
  } else {
    generic.closedPromise = newPromiseCapability();
    generic.closedPromise.reject(releasedError);
  }
  setPromiseIsHandledToTrue(generic.closedPromise.promise);
  slots.controller.releaseSteps();
  slots.reader = undefined;
  generic.stream = undefined;
};

// ---- Default reader ----

const setUpReadableStreamDefaultReader = (reader, stream) => {
  if (isReadableStreamLocked(stream)) throw new TypeError('This stream has already been locked for exclusive reading by another reader');
  defaultReaderSlots.set(reader, { readRequests: [] });
  readableStreamReaderGenericInitialize(reader, stream);
};

const readableStreamDefaultReaderErrorReadRequests = (reader, e) => {
  const readRequests = defaultReaderSlots.get(reader).readRequests;
  defaultReaderSlots.get(reader).readRequests = [];
  for (const readRequest of readRequests) readRequest.errorSteps(e);
};

const readableStreamDefaultReaderRead = (reader, readRequest) => {
  const stream = readerGenericSlots(reader).stream;
  const slots = streamSlots.get(stream);
  slots.disturbed = true;
  if (slots.state === 'closed') readRequest.closeSteps();
  else if (slots.state === 'errored') readRequest.errorSteps(slots.storedError);
  else slots.controller.pullSteps(readRequest);
};

const readableStreamDefaultReaderRelease = (reader) => {
  readableStreamReaderGenericRelease(reader);
  readableStreamDefaultReaderErrorReadRequests(reader, new TypeError('The reader was released'));
};

class ReadableStreamDefaultReader {
  constructor(stream) {
    if (arguments.length < 1) throw new TypeError('Failed to construct \'ReadableStreamDefaultReader\': 1 argument required, but only 0 present.');
    if (!isReadableStream(stream)) throw new TypeError('Failed to construct \'ReadableStreamDefaultReader\': parameter 1 is not of type \'ReadableStream\'.');
    setUpReadableStreamDefaultReader(this, stream);
  }

  get closed() {
    if (!isReadableStreamDefaultReader(this)) return promiseRejectedWith(new TypeError('Illegal invocation'));
    return readerGenericSlots(this).closedPromise.promise;
  }

  cancel(reason = undefined) {
    if (!isReadableStreamDefaultReader(this)) return promiseRejectedWith(new TypeError('Illegal invocation'));
    if (readerGenericSlots(this).stream === undefined) return promiseRejectedWith(new TypeError('This reader has been released and cannot be used to cancel its previous owner stream'));
    return readableStreamReaderGenericCancel(this, reason);
  }

  read() {
    if (!isReadableStreamDefaultReader(this)) return promiseRejectedWith(new TypeError('Illegal invocation'));
    if (readerGenericSlots(this).stream === undefined) return promiseRejectedWith(new TypeError('This reader has been released and cannot be used to read from its previous owner stream'));
    const capability = newPromiseCapability();
    readableStreamDefaultReaderRead(this, {
      chunkSteps: (chunk) => capability.resolve({ value: chunk, done: false }),
      closeSteps: () => capability.resolve({ value: undefined, done: true }),
      errorSteps: (e) => capability.reject(e),
    });
    return capability.promise;
  }

  releaseLock() {
    if (!isReadableStreamDefaultReader(this)) throw new TypeError('Illegal invocation');
    if (readerGenericSlots(this).stream === undefined) return;
    readableStreamDefaultReaderRelease(this);
  }
}
idlify(ReadableStreamDefaultReader);

// ---- Default controller ----

class ReadableStreamDefaultController {
  constructor() { illegalConstructor(); }

  get desiredSize() {
    if (!isReadableStreamDefaultController(this)) throw new TypeError('Illegal invocation');
    return readableStreamDefaultControllerGetDesiredSize(this);
  }

  close() {
    if (!isReadableStreamDefaultController(this)) throw new TypeError('Illegal invocation');
    if (!readableStreamDefaultControllerCanCloseOrEnqueue(this)) throw new TypeError('The stream is not in a state that permits close');
    readableStreamDefaultControllerClose(this);
  }

  enqueue(chunk = undefined) {
    if (!isReadableStreamDefaultController(this)) throw new TypeError('Illegal invocation');
    if (!readableStreamDefaultControllerCanCloseOrEnqueue(this)) throw new TypeError('The stream is not in a state that permits enqueue');
    readableStreamDefaultControllerEnqueue(this, chunk);
  }

  error(e = undefined) {
    if (!isReadableStreamDefaultController(this)) throw new TypeError('Illegal invocation');
    readableStreamDefaultControllerError(this, e);
  }
}
idlify(ReadableStreamDefaultController);

// [[CancelSteps]], [[PullSteps]] and [[ReleaseSteps]] are methods of the slots, set at set-up.
const readableStreamDefaultControllerGetDesiredSize = (controller) => {
  const slots = defaultControllerSlots.get(controller);
  const state = streamSlots.get(slots.stream).state;
  if (state === 'errored') return null;
  if (state === 'closed') return 0;
  return slots.strategyHWM - slots.queueTotalSize;
};

const readableStreamDefaultControllerCanCloseOrEnqueue = (controller) => {
  const slots = defaultControllerSlots.get(controller);
  return streamSlots.get(slots.stream).state === 'readable' && !slots.closeRequested;
};

const readableStreamDefaultControllerShouldCallPull = (controller) => {
  const slots = defaultControllerSlots.get(controller);
  const stream = slots.stream;
  if (!readableStreamDefaultControllerCanCloseOrEnqueue(controller)) return false;
  if (!slots.started) return false;
  if (isReadableStreamLocked(stream) && readableStreamGetNumReadRequests(stream) > 0) return true;
  const desiredSize = readableStreamDefaultControllerGetDesiredSize(controller);
  return desiredSize > 0;
};

const readableStreamDefaultControllerClearAlgorithms = (controller) => {
  const slots = defaultControllerSlots.get(controller);
  slots.pullAlgorithm = undefined;
  slots.cancelAlgorithm = undefined;
  slots.strategySizeAlgorithm = undefined;
};

const readableStreamDefaultControllerClose = (controller) => {
  if (!readableStreamDefaultControllerCanCloseOrEnqueue(controller)) return;
  const slots = defaultControllerSlots.get(controller);
  slots.closeRequested = true;
  if (slots.queue.length === 0) {
    readableStreamDefaultControllerClearAlgorithms(controller);
    readableStreamClose(slots.stream);
  }
};

const readableStreamDefaultControllerEnqueue = (controller, chunk) => {
  if (!readableStreamDefaultControllerCanCloseOrEnqueue(controller)) return;
  const slots = defaultControllerSlots.get(controller);
  const stream = slots.stream;
  if (isReadableStreamLocked(stream) && readableStreamGetNumReadRequests(stream) > 0) {
    readableStreamFulfillReadRequest(stream, chunk, false);
  } else {
    let chunkSize;
    try {
      chunkSize = slots.strategySizeAlgorithm(chunk);
    } catch (e) {
      readableStreamDefaultControllerError(controller, e);
      throw e;
    }
    try {
      enqueueValueWithSize(slots, chunk, chunkSize);
    } catch (e) {
      readableStreamDefaultControllerError(controller, e);
      throw e;
    }
  }
  readableStreamDefaultControllerCallPullIfNeeded(controller);
};

const readableStreamDefaultControllerError = (controller, e) => {
  const slots = defaultControllerSlots.get(controller);
  const stream = slots.stream;
  if (streamSlots.get(stream).state !== 'readable') return;
  resetQueue(slots);
  readableStreamDefaultControllerClearAlgorithms(controller);
  readableStreamError(stream, e);
};

const readableStreamDefaultControllerCallPullIfNeeded = (controller) => {
  if (!readableStreamDefaultControllerShouldCallPull(controller)) return;
  const slots = defaultControllerSlots.get(controller);
  if (slots.pulling) {
    slots.pullAgain = true;
    return;
  }
  slots.pulling = true;
  const pullPromise = slots.pullAlgorithm();
  uponPromise(
    pullPromise,
    () => {
      slots.pulling = false;
      if (slots.pullAgain) {
        slots.pullAgain = false;
        readableStreamDefaultControllerCallPullIfNeeded(controller);
      }
    },
    (e) => readableStreamDefaultControllerError(controller, e));
};

const setUpReadableStreamDefaultController = (stream, controller, startAlgorithm, pullAlgorithm, cancelAlgorithm, highWaterMark, sizeAlgorithm) => {
  const slots = {
    kind: 'default',
    stream, queue: [], queueTotalSize: 0, started: false, closeRequested: false, pullAgain: false, pulling: false,
    strategySizeAlgorithm: sizeAlgorithm, strategyHWM: highWaterMark, pullAlgorithm, cancelAlgorithm,
  };
  slots.cancelSteps = (reason) => {
    resetQueue(slots);
    const result = slots.cancelAlgorithm(reason);
    readableStreamDefaultControllerClearAlgorithms(controller);
    return result;
  };
  slots.pullSteps = (readRequest) => {
    const stream = slots.stream;
    if (slots.queue.length > 0) {
      const chunk = dequeueValue(slots);
      if (slots.closeRequested && slots.queue.length === 0) {
        readableStreamDefaultControllerClearAlgorithms(controller);
        readableStreamClose(stream);
      } else {
        readableStreamDefaultControllerCallPullIfNeeded(controller);
      }
      readRequest.chunkSteps(chunk);
    } else {
      readableStreamAddReadRequest(stream, readRequest);
      readableStreamDefaultControllerCallPullIfNeeded(controller);
    }
  };
  slots.releaseSteps = () => {};
  defaultControllerSlots.set(controller, slots);
  streamSlots.get(stream).controller = slots;

  const startResult = startAlgorithm();
  const startPromise = promiseResolvedWith(startResult);
  uponPromise(
    startPromise,
    () => {
      slots.started = true;
      readableStreamDefaultControllerCallPullIfNeeded(controller);
    },
    (r) => readableStreamDefaultControllerError(controller, r));
};

const setUpReadableStreamDefaultControllerFromUnderlyingSource = (stream, underlyingSource, sourceDict, highWaterMark, sizeAlgorithm) => {
  const controller = objectCreate(ReadableStreamDefaultController.prototype);
  const startAlgorithm = sourceDict.start !== undefined ? () => reflectApply(sourceDict.start, underlyingSource, [controller]) : () => undefined;
  const pullAlgorithm = sourceDict.pull !== undefined ? () => promiseCall(sourceDict.pull, underlyingSource, [controller]) : () => promiseResolvedWith(undefined);
  const cancelAlgorithm = sourceDict.cancel !== undefined ? (reason) => promiseCall(sourceDict.cancel, underlyingSource, [reason]) : () => promiseResolvedWith(undefined);
  setUpReadableStreamDefaultController(stream, controller, startAlgorithm, pullAlgorithm, cancelAlgorithm, highWaterMark, sizeAlgorithm);
};
