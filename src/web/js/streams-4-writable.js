
// ======================================================================================
// WritableStream
// ======================================================================================

const writableStreamSlots = slotsOf(new WeakMap());
const writerSlots = slotsOf(new WeakMap());
const writableControllerSlots = slotsOf(new WeakMap());
const writableStreamControllers = new WeakMap();

const isWritableStream = (x) => writableStreamSlots.has(x);
const isWritableStreamDefaultWriter = (x) => writerSlots.has(x);
const isWritableStreamDefaultController = (x) => writableControllerSlots.has(x);

const AbortControllerIntrinsic = global.AbortController;
const AbortControllerAbort = AbortControllerIntrinsic.prototype.abort;
const AbortControllerSignalGetter = getOwnPropertyDescriptor(AbortControllerIntrinsic.prototype, 'signal').get;
const closeSentinel = {};

// A promise with its resolving functions, which can also say whether it is still pending.
const newTrackedPromise = () => {
  const capability = newPromiseCapability();
  const tracked = { promise: capability.promise, pending: true, resolve: undefined, reject: undefined };
  tracked.resolve = (v) => { tracked.pending = false; capability.resolve(v); };
  tracked.reject = (e) => { tracked.pending = false; capability.reject(e); };
  return tracked;
};

const convertUnderlyingSink = (sink) => {
  if (sink === undefined) return {};
  if (!isObject(sink)) throw new TypeError('The underlying sink is not an object');
  const result = {};
  // alphabetical: abort, close, start, type, write
  for (const name of ['abort', 'close', 'start']) {
    const member = sink[name];
    if (member !== undefined) {
      if (!isCallable(member)) throw new TypeError('The underlying sink\'s ' + name + ' is not a function');
      result[name] = member;
    }
  }
  const type = sink.type;
  if (type !== undefined) result.type = type;
  const write = sink.write;
  if (write !== undefined) {
    if (!isCallable(write)) throw new TypeError('The underlying sink\'s write is not a function');
    result.write = write;
  }
  return result;
};

const isWritableStreamLocked = (stream) => writableStreamSlots.get(stream).writer !== undefined;

class WritableStream {
  constructor(underlyingSink = undefined, strategy = {}) {
    const strategyDict = convertQueuingStrategy(strategy, 'The queuing strategy');
    const sinkDict = convertUnderlyingSink(underlyingSink);
    if (sinkDict.type !== undefined) throw new RangeError('The underlying sink has a type, which is not supported');
    initializeWritableStream(this);
    const sizeAlgorithm = extractSizeAlgorithm(strategyDict);
    const highWaterMark = extractHighWaterMark(strategyDict, 1);
    setUpWritableStreamDefaultControllerFromUnderlyingSink(this, underlyingSink, sinkDict, highWaterMark, sizeAlgorithm);
  }

  get locked() {
    if (!isWritableStream(this)) throw new TypeError('Illegal invocation');
    return isWritableStreamLocked(this);
  }

  abort(reason = undefined) {
    if (!isWritableStream(this)) return promiseRejectedWith(new TypeError('Illegal invocation'));
    if (isWritableStreamLocked(this)) return promiseRejectedWith(new TypeError('Cannot abort a stream that already has a writer'));
    return writableStreamAbort(this, reason);
  }

  close() {
    if (!isWritableStream(this)) return promiseRejectedWith(new TypeError('Illegal invocation'));
    if (isWritableStreamLocked(this)) return promiseRejectedWith(new TypeError('Cannot close a stream that already has a writer'));
    if (writableStreamCloseQueuedOrInFlight(this)) return promiseRejectedWith(new TypeError('Cannot close an already-closing stream'));
    return writableStreamClose(this);
  }

  getWriter() {
    if (!isWritableStream(this)) throw new TypeError('Illegal invocation');
    return acquireWritableStreamDefaultWriter(this);
  }
}
idlify(WritableStream);

const acquireWritableStreamDefaultWriter = (stream) => {
  const writer = objectCreate(WritableStreamDefaultWriter.prototype);
  setUpWritableStreamDefaultWriter(writer, stream);
  return writer;
};

const createWritableStream = (startAlgorithm, writeAlgorithm, closeAlgorithm, abortAlgorithm, highWaterMark = 1, sizeAlgorithm = () => 1) => {
  const stream = objectCreate(WritableStream.prototype);
  initializeWritableStream(stream);
  const controller = objectCreate(WritableStreamDefaultController.prototype);
  reflectApply(WeakMapSet, writableStreamControllers, [stream, controller]);
  setUpWritableStreamDefaultController(stream, controller, startAlgorithm, writeAlgorithm, closeAlgorithm, abortAlgorithm, highWaterMark, sizeAlgorithm);
  return stream;
};

const initializeWritableStream = (stream) => {
  writableStreamSlots.set(stream, {
    state: 'writable', storedError: undefined, writer: undefined, controller: undefined, inFlightWriteRequest: undefined,
    closeRequest: undefined, inFlightCloseRequest: undefined, pendingAbortRequest: undefined, writeRequests: [], backpressure: false,
  });
};

const writableStreamAbort = (stream, reason) => {
  const slots = writableStreamSlots.get(stream);
  if (slots.state === 'closed' || slots.state === 'errored') return promiseResolvedWith(undefined);
  reflectApply(AbortControllerAbort, slots.controller.abortController, [reason]);
  const state = slots.state;
  if (state === 'closed' || state === 'errored') return promiseResolvedWith(undefined);
  if (slots.pendingAbortRequest !== undefined) return slots.pendingAbortRequest.promise.promise;
  let wasAlreadyErroring = false;
  if (state === 'erroring') {
    wasAlreadyErroring = true;
    reason = undefined;
  }
  const promise = newTrackedPromise();
  slots.pendingAbortRequest = { promise, reason, wasAlreadyErroring };
  if (!wasAlreadyErroring) writableStreamStartErroring(stream, reason);
  return promise.promise;
};

const writableStreamClose = (stream) => {
  const slots = writableStreamSlots.get(stream);
  const state = slots.state;
  if (state === 'closed' || state === 'errored') return promiseRejectedWith(new TypeError('The stream is not in a state that permits close'));
  const promise = newTrackedPromise();
  slots.closeRequest = promise;
  const writer = slots.writer;
  if (writer !== undefined && slots.backpressure && state === 'writable') writerSlots.get(writer).readyPromise.resolve(undefined);
  writableStreamDefaultControllerClose(slots.controller);
  return promise.promise;
};

const writableStreamAddWriteRequest = (stream) => {
  const promise = newTrackedPromise();
  reflectApply(ArrayPrototypePush, writableStreamSlots.get(stream).writeRequests, [promise]);
  return promise.promise;
};

const writableStreamCloseQueuedOrInFlight = (stream) => {
  const slots = writableStreamSlots.get(stream);
  return slots.closeRequest !== undefined || slots.inFlightCloseRequest !== undefined;
};

const writableStreamDealWithRejection = (stream, error) => {
  const state = writableStreamSlots.get(stream).state;
  if (state === 'writable') {
    writableStreamStartErroring(stream, error);
    return;
  }
  writableStreamFinishErroring(stream);
};

const writableStreamFinishErroring = (stream) => {
  const slots = writableStreamSlots.get(stream);
  slots.state = 'errored';
  slots.controller.errorSteps();
  const storedError = slots.storedError;
  for (const writeRequest of slots.writeRequests) writeRequest.reject(storedError);
  slots.writeRequests = [];
  if (slots.pendingAbortRequest === undefined) {
    writableStreamRejectCloseAndClosedPromiseIfNeeded(stream);
    return;
  }
  const abortRequest = slots.pendingAbortRequest;
  slots.pendingAbortRequest = undefined;
  if (abortRequest.wasAlreadyErroring) {
    abortRequest.promise.reject(storedError);
    writableStreamRejectCloseAndClosedPromiseIfNeeded(stream);
    return;
  }
  const promise = slots.controller.abortSteps(abortRequest.reason);
  uponPromise(
    promise,
    () => {
      abortRequest.promise.resolve(undefined);
      writableStreamRejectCloseAndClosedPromiseIfNeeded(stream);
    },
    (reason) => {
      abortRequest.promise.reject(reason);
      writableStreamRejectCloseAndClosedPromiseIfNeeded(stream);
    });
};

const writableStreamFinishInFlightClose = (stream) => {
  const slots = writableStreamSlots.get(stream);
  slots.inFlightCloseRequest.resolve(undefined);
  slots.inFlightCloseRequest = undefined;
  if (slots.state === 'erroring') {
    slots.storedError = undefined;
    if (slots.pendingAbortRequest !== undefined) {
      slots.pendingAbortRequest.promise.resolve(undefined);
      slots.pendingAbortRequest = undefined;
    }
  }
  slots.state = 'closed';
  const writer = slots.writer;
  if (writer !== undefined) writerSlots.get(writer).closedPromise.resolve(undefined);
};

const writableStreamFinishInFlightCloseWithError = (stream, error) => {
  const slots = writableStreamSlots.get(stream);
  slots.inFlightCloseRequest.reject(error);
  slots.inFlightCloseRequest = undefined;
  if (slots.pendingAbortRequest !== undefined) {
    slots.pendingAbortRequest.promise.reject(error);
    slots.pendingAbortRequest = undefined;
  }
  writableStreamDealWithRejection(stream, error);
};

const writableStreamFinishInFlightWrite = (stream) => {
  const slots = writableStreamSlots.get(stream);
  slots.inFlightWriteRequest.resolve(undefined);
  slots.inFlightWriteRequest = undefined;
};

const writableStreamFinishInFlightWriteWithError = (stream, error) => {
  const slots = writableStreamSlots.get(stream);
  slots.inFlightWriteRequest.reject(error);
  slots.inFlightWriteRequest = undefined;
  writableStreamDealWithRejection(stream, error);
};

const writableStreamHasOperationMarkedInFlight = (stream) => {
  const slots = writableStreamSlots.get(stream);
  return slots.inFlightWriteRequest !== undefined || slots.inFlightCloseRequest !== undefined;
};

const writableStreamMarkCloseRequestInFlight = (stream) => {
  const slots = writableStreamSlots.get(stream);
  slots.inFlightCloseRequest = slots.closeRequest;
  slots.closeRequest = undefined;
};

const writableStreamMarkFirstWriteRequestInFlight = (stream) => {
  const slots = writableStreamSlots.get(stream);
  slots.inFlightWriteRequest = reflectApply(ArrayPrototypeShift, slots.writeRequests, []);
};

const writableStreamRejectCloseAndClosedPromiseIfNeeded = (stream) => {
  const slots = writableStreamSlots.get(stream);
  if (slots.closeRequest !== undefined) {
    slots.closeRequest.reject(slots.storedError);
    slots.closeRequest = undefined;
  }
  const writer = slots.writer;
  if (writer !== undefined) {
    const closed = writerSlots.get(writer).closedPromise;
    closed.reject(slots.storedError);
    setPromiseIsHandledToTrue(closed.promise);
  }
};

const writableStreamStartErroring = (stream, reason) => {
  const slots = writableStreamSlots.get(stream);
  const controller = slots.controller;
  slots.state = 'erroring';
  slots.storedError = reason;
  const writer = slots.writer;
  if (writer !== undefined) writableStreamDefaultWriterEnsureReadyPromiseRejected(writer, reason);
  if (!writableStreamHasOperationMarkedInFlight(stream) && controller.started) writableStreamFinishErroring(stream);
};

const writableStreamUpdateBackpressure = (stream, backpressure) => {
  const slots = writableStreamSlots.get(stream);
  const writer = slots.writer;
  if (writer !== undefined && backpressure !== slots.backpressure) {
    if (backpressure) writerSlots.get(writer).readyPromise = newTrackedPromise();
    else writerSlots.get(writer).readyPromise.resolve(undefined);
  }
  slots.backpressure = backpressure;
};

// ---- Writer ----

class WritableStreamDefaultWriter {
  constructor(stream) {
    if (arguments.length < 1) throw new TypeError('Failed to construct \'WritableStreamDefaultWriter\': 1 argument required, but only 0 present.');
    if (!isWritableStream(stream)) throw new TypeError('Failed to construct \'WritableStreamDefaultWriter\': parameter 1 is not of type \'WritableStream\'.');
    setUpWritableStreamDefaultWriter(this, stream);
  }

  get closed() {
    if (!isWritableStreamDefaultWriter(this)) return promiseRejectedWith(new TypeError('Illegal invocation'));
    return writerSlots.get(this).closedPromise.promise;
  }

  get desiredSize() {
    if (!isWritableStreamDefaultWriter(this)) throw new TypeError('Illegal invocation');
    if (writerSlots.get(this).stream === undefined) throw new TypeError('The writer is released and has no desired size');
    return writableStreamDefaultWriterGetDesiredSize(this);
  }

  get ready() {
    if (!isWritableStreamDefaultWriter(this)) return promiseRejectedWith(new TypeError('Illegal invocation'));
    return writerSlots.get(this).readyPromise.promise;
  }

  abort(reason = undefined) {
    if (!isWritableStreamDefaultWriter(this)) return promiseRejectedWith(new TypeError('Illegal invocation'));
    if (writerSlots.get(this).stream === undefined) return promiseRejectedWith(new TypeError('The writer is released'));
    return writableStreamDefaultWriterAbort(this, reason);
  }

  close() {
    if (!isWritableStreamDefaultWriter(this)) return promiseRejectedWith(new TypeError('Illegal invocation'));
    const stream = writerSlots.get(this).stream;
    if (stream === undefined) return promiseRejectedWith(new TypeError('The writer is released'));
    if (writableStreamCloseQueuedOrInFlight(stream)) return promiseRejectedWith(new TypeError('Cannot close an already-closing stream'));
    return writableStreamDefaultWriterClose(this);
  }

  releaseLock() {
    if (!isWritableStreamDefaultWriter(this)) throw new TypeError('Illegal invocation');
    if (writerSlots.get(this).stream === undefined) return;
    writableStreamDefaultWriterRelease(this);
  }

  write(chunk = undefined) {
    if (!isWritableStreamDefaultWriter(this)) return promiseRejectedWith(new TypeError('Illegal invocation'));
    if (writerSlots.get(this).stream === undefined) return promiseRejectedWith(new TypeError('The writer is released'));
    return writableStreamDefaultWriterWrite(this, chunk);
  }
}
idlify(WritableStreamDefaultWriter);

const setUpWritableStreamDefaultWriter = (writer, stream) => {
  if (isWritableStreamLocked(stream)) throw new TypeError('This stream has already been locked for exclusive writing by another writer');
  const slots = writableStreamSlots.get(stream);
  const state = { stream, readyPromise: undefined, closedPromise: undefined };
  writerSlots.set(writer, state);
  slots.writer = writer;
  if (slots.state === 'writable') {
    state.readyPromise = !writableStreamCloseQueuedOrInFlight(stream) && slots.backpressure ? newTrackedPromise() : (() => { const p = newTrackedPromise(); p.resolve(undefined); return p; })();
    state.closedPromise = newTrackedPromise();
  } else if (slots.state === 'erroring') {
    state.readyPromise = newTrackedPromise();
    state.readyPromise.reject(slots.storedError);
    setPromiseIsHandledToTrue(state.readyPromise.promise);
    state.closedPromise = newTrackedPromise();
  } else if (slots.state === 'closed') {
    state.readyPromise = newTrackedPromise();
    state.readyPromise.resolve(undefined);
    state.closedPromise = newTrackedPromise();
    state.closedPromise.resolve(undefined);
  } else {
    state.readyPromise = newTrackedPromise();
    state.readyPromise.reject(slots.storedError);
    setPromiseIsHandledToTrue(state.readyPromise.promise);
    state.closedPromise = newTrackedPromise();
    state.closedPromise.reject(slots.storedError);
    setPromiseIsHandledToTrue(state.closedPromise.promise);
  }
};

const writableStreamDefaultWriterAbort = (writer, reason) => writableStreamAbort(writerSlots.get(writer).stream, reason);

const writableStreamDefaultWriterClose = (writer) => writableStreamClose(writerSlots.get(writer).stream);

const writableStreamDefaultWriterCloseWithErrorPropagation = (writer) => {
  const stream = writerSlots.get(writer).stream;
  const slots = writableStreamSlots.get(stream);
  if (writableStreamCloseQueuedOrInFlight(stream) || slots.state === 'closed') return promiseResolvedWith(undefined);
  if (slots.state === 'errored') return promiseRejectedWith(slots.storedError);
  return writableStreamDefaultWriterClose(writer);
};

const writableStreamDefaultWriterEnsureClosedPromiseRejected = (writer, error) => {
  const state = writerSlots.get(writer);
  if (state.closedPromise.pending) {
    state.closedPromise.reject(error);
  } else {
    state.closedPromise = newTrackedPromise();
    state.closedPromise.reject(error);
  }
  setPromiseIsHandledToTrue(state.closedPromise.promise);
};

const writableStreamDefaultWriterEnsureReadyPromiseRejected = (writer, error) => {
  const state = writerSlots.get(writer);
  if (state.readyPromise.pending) {
    state.readyPromise.reject(error);
  } else {
    state.readyPromise = newTrackedPromise();
    state.readyPromise.reject(error);
  }
  setPromiseIsHandledToTrue(state.readyPromise.promise);
};

const writableStreamDefaultWriterGetDesiredSize = (writer) => {
  const stream = writerSlots.get(writer).stream;
  const slots = writableStreamSlots.get(stream);
  if (slots.state === 'errored' || slots.state === 'erroring') return null;
  if (slots.state === 'closed') return 0;
  return writableStreamDefaultControllerGetDesiredSize(slots.controller);
};

const writableStreamDefaultWriterRelease = (writer) => {
  const state = writerSlots.get(writer);
  const stream = state.stream;
  const releasedError = new TypeError('The writer was released');
  writableStreamDefaultWriterEnsureReadyPromiseRejected(writer, releasedError);
  writableStreamDefaultWriterEnsureClosedPromiseRejected(writer, releasedError);
  writableStreamSlots.get(stream).writer = undefined;
  state.stream = undefined;
};

const writableStreamDefaultWriterWrite = (writer, chunk) => {
  const state = writerSlots.get(writer);
  const stream = state.stream;
  const slots = writableStreamSlots.get(stream);
  const controller = slots.controller;
  const chunkSize = writableStreamDefaultControllerGetChunkSize(controller, chunk);
  if (stream !== state.stream) return promiseRejectedWith(new TypeError('The writer is released'));
  const streamState = slots.state;
  if (streamState === 'errored') return promiseRejectedWith(slots.storedError);
  if (writableStreamCloseQueuedOrInFlight(stream) || streamState === 'closed') return promiseRejectedWith(new TypeError('The stream is closing or closed and cannot be written to'));
  if (streamState === 'erroring') return promiseRejectedWith(slots.storedError);
  const promise = writableStreamAddWriteRequest(stream);
  writableStreamDefaultControllerWrite(controller, chunk, chunkSize);
  return promise;
};

// ---- Controller ----

class WritableStreamDefaultController {
  constructor() { illegalConstructor(); }

  get signal() {
    if (!isWritableStreamDefaultController(this)) throw new TypeError('Illegal invocation');
    return reflectApply(AbortControllerSignalGetter, writableControllerSlots.get(this).abortController, []);
  }

  error(e = undefined) {
    if (!isWritableStreamDefaultController(this)) throw new TypeError('Illegal invocation');
    const state = writableStreamSlots.get(writableControllerSlots.get(this).stream).state;
    if (state !== 'writable') return;
    writableStreamDefaultControllerError(this, e);
  }
}
idlify(WritableStreamDefaultController);

const setUpWritableStreamDefaultController = (stream, controller, startAlgorithm, writeAlgorithm, closeAlgorithm, abortAlgorithm, highWaterMark, sizeAlgorithm) => {
  const slots = {
    stream, queue: [], queueTotalSize: 0, abortController: new AbortControllerIntrinsic(), started: false,
    strategySizeAlgorithm: sizeAlgorithm, strategyHWM: highWaterMark, writeAlgorithm, closeAlgorithm, abortAlgorithm,
  };
  slots.abortSteps = (reason) => {
    const result = slots.abortAlgorithm(reason);
    writableStreamDefaultControllerClearAlgorithms(controller);
    return result;
  };
  slots.errorSteps = () => { resetQueue(slots); };
  writableControllerSlots.set(controller, slots);
  writableStreamSlots.get(stream).controller = slots;

  const backpressure = writableStreamDefaultControllerGetBackpressure(controller);
  writableStreamUpdateBackpressure(stream, backpressure);

  const startResult = startAlgorithm();
  const startPromise = promiseResolvedWith(startResult);
  uponPromise(
    startPromise,
    () => {
      slots.started = true;
      writableStreamDefaultControllerAdvanceQueueIfNeeded(controller);
    },
    (r) => {
      slots.started = true;
      writableStreamDealWithRejection(stream, r);
    });
};

const setUpWritableStreamDefaultControllerFromUnderlyingSink = (stream, underlyingSink, sinkDict, highWaterMark, sizeAlgorithm) => {
  const controller = objectCreate(WritableStreamDefaultController.prototype);
  const startAlgorithm = sinkDict.start !== undefined ? () => reflectApply(sinkDict.start, underlyingSink, [controller]) : () => undefined;
  const writeAlgorithm = sinkDict.write !== undefined ? (chunk) => promiseCall(sinkDict.write, underlyingSink, [chunk, controller]) : () => promiseResolvedWith(undefined);
  const closeAlgorithm = sinkDict.close !== undefined ? () => promiseCall(sinkDict.close, underlyingSink, []) : () => promiseResolvedWith(undefined);
  const abortAlgorithm = sinkDict.abort !== undefined ? (reason) => promiseCall(sinkDict.abort, underlyingSink, [reason]) : () => promiseResolvedWith(undefined);
  setUpWritableStreamDefaultController(stream, controller, startAlgorithm, writeAlgorithm, closeAlgorithm, abortAlgorithm, highWaterMark, sizeAlgorithm);
};

const writableStreamDefaultControllerClearAlgorithms = (controller) => {
  const slots = writableControllerSlots.get(controller);
  slots.writeAlgorithm = undefined;
  slots.closeAlgorithm = undefined;
  slots.abortAlgorithm = undefined;
  slots.strategySizeAlgorithm = undefined;
};

const writableStreamDefaultControllerClose = (controllerSlots) => {
  // Called with the slots object, which the stream holds.
  enqueueValueWithSize(controllerSlots, closeSentinel, 0);
  writableStreamDefaultControllerAdvanceQueueIfNeededForSlots(controllerSlots);
};

const writableStreamDefaultControllerAdvanceQueueIfNeededForSlots = (slots) => {
  const stream = slots.stream;
  if (!slots.started) return;
  const streamSlotsRecord = writableStreamSlots.get(stream);
  if (streamSlotsRecord.inFlightWriteRequest !== undefined) return;
  const state = streamSlotsRecord.state;
  if (state === 'erroring') {
    writableStreamFinishErroring(stream);
    return;
  }
  if (slots.queue.length === 0) return;
  const value = peekQueueValue(slots);
  if (value === closeSentinel) writableStreamDefaultControllerProcessCloseForSlots(slots);
  else writableStreamDefaultControllerProcessWriteForSlots(slots, value);
};
const writableStreamDefaultControllerAdvanceQueueIfNeeded = (controller) => writableStreamDefaultControllerAdvanceQueueIfNeededForSlots(writableControllerSlots.get(controller));

const writableStreamDefaultControllerProcessCloseForSlots = (slots) => {
  const stream = slots.stream;
  writableStreamMarkCloseRequestInFlight(stream);
  dequeueValue(slots);
  const sinkClosePromise = slots.closeAlgorithm();
  clearAlgorithmsForSlots(slots);
  uponPromise(
    sinkClosePromise,
    () => writableStreamFinishInFlightClose(stream),
    (reason) => writableStreamFinishInFlightCloseWithError(stream, reason));
};

const clearAlgorithmsForSlots = (slots) => {
  slots.writeAlgorithm = undefined;
  slots.closeAlgorithm = undefined;
  slots.abortAlgorithm = undefined;
  slots.strategySizeAlgorithm = undefined;
};

const writableStreamDefaultControllerProcessWriteForSlots = (slots, chunk) => {
  const stream = slots.stream;
  writableStreamMarkFirstWriteRequestInFlight(stream);
  const sinkWritePromise = slots.writeAlgorithm(chunk);
  uponPromise(
    sinkWritePromise,
    () => {
      writableStreamFinishInFlightWrite(stream);
      const state = writableStreamSlots.get(stream).state;
      dequeueValue(slots);
      if (!writableStreamCloseQueuedOrInFlight(stream) && state === 'writable') {
        const backpressure = slots.strategyHWM - slots.queueTotalSize <= 0;
        writableStreamUpdateBackpressure(stream, backpressure);
      }
      writableStreamDefaultControllerAdvanceQueueIfNeededForSlots(slots);
    },
    (reason) => {
      if (writableStreamSlots.get(stream).state === 'writable') clearAlgorithmsForSlots(slots);
      writableStreamFinishInFlightWriteWithError(stream, reason);
    });
};

const writableStreamDefaultControllerGetBackpressure = (controller) => {
  const slots = writableControllerSlots.get(controller);
  return slots.strategyHWM - slots.queueTotalSize <= 0;
};

const writableStreamDefaultControllerGetChunkSize = (slots, chunk) => {
  if (slots.strategySizeAlgorithm === undefined) return 1;
  try {
    return slots.strategySizeAlgorithm(chunk);
  } catch (chunkSizeE) {
    writableStreamDefaultControllerErrorIfNeededForSlots(slots, chunkSizeE);
    return 1;
  }
};

const writableStreamDefaultControllerGetDesiredSize = (slots) => slots.strategyHWM - slots.queueTotalSize;

const writableStreamDefaultControllerWrite = (slots, chunk, chunkSize) => {
  try {
    enqueueValueWithSize(slots, chunk, chunkSize);
  } catch (enqueueE) {
    writableStreamDefaultControllerErrorIfNeededForSlots(slots, enqueueE);
    return;
  }
  const stream = slots.stream;
  if (!writableStreamCloseQueuedOrInFlight(stream) && writableStreamSlots.get(stream).state === 'writable') {
    const backpressure = slots.strategyHWM - slots.queueTotalSize <= 0;
    writableStreamUpdateBackpressure(stream, backpressure);
  }
  writableStreamDefaultControllerAdvanceQueueIfNeededForSlots(slots);
};

const writableStreamDefaultControllerErrorIfNeededForSlots = (slots, error) => {
  if (writableStreamSlots.get(slots.stream).state === 'writable') writableStreamDefaultControllerErrorForSlots(slots, error);
};

const writableStreamDefaultControllerErrorForSlots = (slots, error) => {
  const stream = slots.stream;
  clearAlgorithmsForSlots(slots);
  writableStreamStartErroring(stream, error);
};
const writableStreamDefaultControllerError = (controller, error) => writableStreamDefaultControllerErrorForSlots(writableControllerSlots.get(controller), error);
