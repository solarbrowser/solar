
// ======================================================================================
// Piping
// ======================================================================================

const noop = () => {};
const AbortSignalAddEventListener = global.EventTarget.prototype.addEventListener;
const AbortSignalRemoveEventListener = global.EventTarget.prototype.removeEventListener;
const AbortSignalAbortedGetter = getOwnPropertyDescriptor(global.AbortSignal.prototype, 'aborted').get;
const AbortSignalReasonGetter = getOwnPropertyDescriptor(global.AbortSignal.prototype, 'reason').get;

const promiseAll = (promises) => {
  // Promise.all as the intrinsic would do it, for a list of promises, without reading anything from the page's Promise.
  if (promises.length === 0) return promiseResolvedWith([]);
  const capability = newPromiseCapability();
  const results = [];
  let remaining = promises.length;
  promises.forEach((promise, index) => {
    uponPromise(
      promise,
      (value) => {
        results[index] = value;
        if (--remaining === 0) capability.resolve(results);
      },
      capability.reject);
  });
  return capability.promise;
};

const readableStreamPipeTo = (source, dest, preventClose, preventAbort, preventCancel, signal) => {
  const reader = acquireReadableStreamDefaultReader(source);
  const writer = acquireWritableStreamDefaultWriter(dest);
  const sourceSlots = streamSlots.get(source);
  const destSlots = writableStreamSlots.get(dest);
  sourceSlots.disturbed = true;
  let shuttingDown = false;
  let currentWrite = promiseResolvedWith(undefined);

  const capability = newPromiseCapability();
  let abortAlgorithm;
  const readerClosed = readerGenericSlots(reader).closedPromise.promise;
  const writerClosed = () => writerSlots.get(writer).closedPromise.promise;

  const waitForWritesToFinish = () => {
    // Another write may have started while this one was awaited, and has to be waited for too.
    const oldCurrentWrite = currentWrite;
    return transformPromise(currentWrite, () => (oldCurrentWrite !== currentWrite ? waitForWritesToFinish() : undefined), undefined);
  };
  const finalize = (isError, error) => {
    writableStreamDefaultWriterRelease(writer);
    readableStreamDefaultReaderRelease(reader);
    if (signal !== undefined) reflectApply(AbortSignalRemoveEventListener, signal, ['abort', abortAlgorithm]);
    if (isError) capability.reject(error);
    else capability.resolve(undefined);
  };
  const shutdownWithAction = (action, originalIsError, originalError) => {
    if (shuttingDown) return;
    shuttingDown = true;
    const doTheRest = () => {
      uponPromise(action(), () => finalize(originalIsError, originalError), (newError) => finalize(true, newError));
    };
    if (destSlots.state === 'writable' && !writableStreamCloseQueuedOrInFlight(dest)) uponFulfillment(waitForWritesToFinish(), doTheRest);
    else doTheRest();
  };
  const shutdown = (isError, error) => {
    if (shuttingDown) return;
    shuttingDown = true;
    if (destSlots.state === 'writable' && !writableStreamCloseQueuedOrInFlight(dest)) uponFulfillment(waitForWritesToFinish(), () => finalize(isError, error));
    else finalize(isError, error);
  };

  if (signal !== undefined) {
    abortAlgorithm = () => {
      const error = reflectApply(AbortSignalReasonGetter, signal, []);
      const actions = [];
      if (!preventAbort) {
        actions.push(() => (destSlots.state === 'writable' ? writableStreamAbort(dest, error) : promiseResolvedWith(undefined)));
      }
      if (!preventCancel) {
        actions.push(() => (sourceSlots.state === 'readable' ? readableStreamCancel(source, error) : promiseResolvedWith(undefined)));
      }
      shutdownWithAction(() => promiseAll(actions.map((action) => action())), true, error);
    };
    if (reflectApply(AbortSignalAbortedGetter, signal, [])) {
      abortAlgorithm();
      return capability.promise;
    }
    reflectApply(AbortSignalAddEventListener, signal, ['abort', abortAlgorithm]);
  }

  const pipeStep = () => {
    if (shuttingDown) return promiseResolvedWith(true);
    return transformPromise(writerSlots.get(writer).readyPromise.promise, () => {
      const read = newPromiseCapability();
      readableStreamDefaultReaderRead(reader, {
        chunkSteps: (chunk) => {
          // A microtask, so that enqueue() does not call the write algorithm within itself.
          queueMicrotask(() => {
            currentWrite = transformPromise(writableStreamDefaultWriterWrite(writer, chunk), undefined, noop);
            read.resolve(false);
          });
        },
        closeSteps: () => read.resolve(true),
        errorSteps: read.reject,
      });
      return read.promise;
    }, undefined);
  };
  const pipeLoop = () => {
    const loop = newPromiseCapability();
    const next = (done) => {
      if (done) loop.resolve(undefined);
      else uponPromise(pipeStep(), next, loop.reject);
    };
    next(false);
    return loop.promise;
  };

  const isOrBecomesErrored = (stream, slots, promise, action) => {
    if (slots.state === 'errored') action(slots.storedError);
    else uponRejection(promise, action);
  };
  const isOrBecomesClosed = (slots, promise, action) => {
    if (slots.state === 'closed') action();
    else uponFulfillment(promise, action);
  };

  // Errors must be propagated forward.
  isOrBecomesErrored(source, sourceSlots, readerClosed, (storedError) => {
    if (!preventAbort) shutdownWithAction(() => writableStreamAbort(dest, storedError), true, storedError);
    else shutdown(true, storedError);
  });
  // Errors must be propagated backward.
  isOrBecomesErrored(dest, destSlots, writerClosed(), (storedError) => {
    if (!preventCancel) shutdownWithAction(() => readableStreamCancel(source, storedError), true, storedError);
    else shutdown(true, storedError);
  });
  // Closing must be propagated forward.
  isOrBecomesClosed(sourceSlots, readerClosed, () => {
    if (!preventClose) shutdownWithAction(() => writableStreamDefaultWriterCloseWithErrorPropagation(writer));
    else shutdown();
  });
  // Closing must be propagated backward.
  if (writableStreamCloseQueuedOrInFlight(dest) || destSlots.state === 'closed') {
    const destClosed = new TypeError('the destination writable stream closed before all data could be piped to it');
    if (!preventCancel) shutdownWithAction(() => readableStreamCancel(source, destClosed), true, destClosed);
    else shutdown(true, destClosed);
  }
  setPromiseIsHandledToTrue(pipeLoop());
  return capability.promise;
};

// ======================================================================================
// TransformStream
// ======================================================================================

const transformStreamSlots = slotsOf(new WeakMap());
const transformControllerSlots = slotsOf(new WeakMap());
const isTransformStream = (x) => transformStreamSlots.has(x);
const isTransformStreamDefaultController = (x) => transformControllerSlots.has(x);

const convertTransformer = (transformer) => {
  if (transformer === undefined) return {};
  if (!isObject(transformer)) throw new TypeError('The transformer is not an object');
  const result = {};
  // alphabetical: cancel, flush, readableType, start, transform, writableType
  for (const name of ['cancel', 'flush']) {
    const member = transformer[name];
    if (member !== undefined) {
      if (!isCallable(member)) throw new TypeError('The transformer\'s ' + name + ' is not a function');
      result[name] = member;
    }
  }
  if (transformer.readableType !== undefined) result.readableType = transformer.readableType;
  const start = transformer.start;
  if (start !== undefined) {
    if (!isCallable(start)) throw new TypeError('The transformer\'s start is not a function');
    result.start = start;
  }
  const transform = transformer.transform;
  if (transform !== undefined) {
    if (!isCallable(transform)) throw new TypeError('The transformer\'s transform is not a function');
    result.transform = transform;
  }
  if (transformer.writableType !== undefined) result.writableType = transformer.writableType;
  return result;
};

class TransformStream {
  constructor(transformer = undefined, writableStrategy = {}, readableStrategy = {}) {
    const writableStrategyDict = convertQueuingStrategy(writableStrategy, 'The writable strategy');
    const readableStrategyDict = convertQueuingStrategy(readableStrategy, 'The readable strategy');
    const transformerDict = convertTransformer(transformer);
    if (transformerDict.readableType !== undefined) throw new RangeError('The transformer has a readableType, which is not supported');
    if (transformerDict.writableType !== undefined) throw new RangeError('The transformer has a writableType, which is not supported');

    const readableHighWaterMark = extractHighWaterMark(readableStrategyDict, 0);
    const readableSizeAlgorithm = extractSizeAlgorithm(readableStrategyDict);
    const writableHighWaterMark = extractHighWaterMark(writableStrategyDict, 1);
    const writableSizeAlgorithm = extractSizeAlgorithm(writableStrategyDict);

    const startPromise = newPromiseCapability();
    initializeTransformStream(this, startPromise, writableHighWaterMark, writableSizeAlgorithm, readableHighWaterMark, readableSizeAlgorithm);
    const controller = setUpTransformStreamDefaultControllerFromTransformer(this, transformer, transformerDict);
    if (transformerDict.start !== undefined) startPromise.resolve(reflectApply(transformerDict.start, transformer, [controller]));
    else startPromise.resolve(undefined);
  }

  get readable() {
    if (!isTransformStream(this)) throw new TypeError('Illegal invocation');
    return transformStreamSlots.get(this).readable;
  }

  get writable() {
    if (!isTransformStream(this)) throw new TypeError('Illegal invocation');
    return transformStreamSlots.get(this).writable;
  }
}
idlify(TransformStream);

const initializeTransformStream = (stream, startPromise, writableHighWaterMark, writableSizeAlgorithm, readableHighWaterMark, readableSizeAlgorithm) => {
  const slots = { backpressure: undefined, backpressureChangePromise: undefined, controller: undefined, readable: undefined, writable: undefined };
  transformStreamSlots.set(stream, slots);
  slots.writable = createWritableStream(
    () => startPromise.promise,
    (chunk) => transformStreamDefaultSinkWriteAlgorithm(stream, chunk),
    () => transformStreamDefaultSinkCloseAlgorithm(stream),
    (reason) => transformStreamDefaultSinkAbortAlgorithm(stream, reason),
    writableHighWaterMark, writableSizeAlgorithm);
  slots.readable = createReadableStream(
    () => startPromise.promise,
    () => transformStreamDefaultSourcePullAlgorithm(stream),
    (reason) => transformStreamDefaultSourceCancelAlgorithm(stream, reason),
    readableHighWaterMark, readableSizeAlgorithm);
  transformStreamSetBackpressure(stream, true);
};

const readableControllerOf = (readable) => reflectApply(WeakMapGet, readableStreamControllers, [readable]);
const writableSlotsControllerOf = (writable) => writableStreamSlots.get(writable).controller;

const transformStreamError = (stream, e) => {
  const slots = transformStreamSlots.get(stream);
  readableStreamDefaultControllerError(readableControllerOf(slots.readable), e);
  transformStreamErrorWritableAndUnblockWrite(stream, e);
};

const transformStreamErrorWritableAndUnblockWrite = (stream, e) => {
  const slots = transformStreamSlots.get(stream);
  transformStreamDefaultControllerClearAlgorithms(slots.controller);
  writableStreamDefaultControllerErrorIfNeededForSlots(writableSlotsControllerOf(slots.writable), e);
  transformStreamUnblockWrite(stream);
};

const transformStreamUnblockWrite = (stream) => {
  if (transformStreamSlots.get(stream).backpressure) transformStreamSetBackpressure(stream, false);
};

const transformStreamSetBackpressure = (stream, backpressure) => {
  const slots = transformStreamSlots.get(stream);
  if (slots.backpressureChangePromise !== undefined) slots.backpressureChangePromise.resolve(undefined);
  slots.backpressureChangePromise = newPromiseCapability();
  slots.backpressure = backpressure;
};

// ---- Controller ----

class TransformStreamDefaultController {
  constructor() { illegalConstructor(); }

  get desiredSize() {
    if (!isTransformStreamDefaultController(this)) throw new TypeError('Illegal invocation');
    const stream = transformControllerSlots.get(this).stream;
    return readableStreamDefaultControllerGetDesiredSize(readableControllerOf(transformStreamSlots.get(stream).readable));
  }

  enqueue(chunk = undefined) {
    if (!isTransformStreamDefaultController(this)) throw new TypeError('Illegal invocation');
    transformStreamDefaultControllerEnqueue(this, chunk);
  }

  error(reason = undefined) {
    if (!isTransformStreamDefaultController(this)) throw new TypeError('Illegal invocation');
    transformStreamError(transformControllerSlots.get(this).stream, reason);
  }

  terminate() {
    if (!isTransformStreamDefaultController(this)) throw new TypeError('Illegal invocation');
    transformStreamDefaultControllerTerminate(this);
  }
}
idlify(TransformStreamDefaultController);

const setUpTransformStreamDefaultController = (stream, controller, transformAlgorithm, flushAlgorithm, cancelAlgorithm) => {
  transformControllerSlots.set(controller, { stream, transformAlgorithm, flushAlgorithm, cancelAlgorithm, finishPromise: undefined });
  transformStreamSlots.get(stream).controller = controller;
};

const setUpTransformStreamDefaultControllerFromTransformer = (stream, transformer, transformerDict) => {
  const controller = objectCreate(TransformStreamDefaultController.prototype);
  const transformAlgorithm = transformerDict.transform !== undefined
    ? (chunk) => promiseCall(transformerDict.transform, transformer, [chunk, controller])
    : (chunk) => {
        try {
          transformStreamDefaultControllerEnqueue(controller, chunk);
          return promiseResolvedWith(undefined);
        } catch (e) {
          return promiseRejectedWith(e);
        }
      };
  const flushAlgorithm = transformerDict.flush !== undefined ? () => promiseCall(transformerDict.flush, transformer, [controller]) : () => promiseResolvedWith(undefined);
  const cancelAlgorithm = transformerDict.cancel !== undefined ? (reason) => promiseCall(transformerDict.cancel, transformer, [reason]) : () => promiseResolvedWith(undefined);
  setUpTransformStreamDefaultController(stream, controller, transformAlgorithm, flushAlgorithm, cancelAlgorithm);
  return controller;
};

const transformStreamDefaultControllerClearAlgorithms = (controller) => {
  const slots = transformControllerSlots.get(controller);
  slots.transformAlgorithm = undefined;
  slots.flushAlgorithm = undefined;
  slots.cancelAlgorithm = undefined;
};

const transformStreamDefaultControllerEnqueue = (controller, chunk) => {
  const stream = transformControllerSlots.get(controller).stream;
  const streamSlotsRecord = transformStreamSlots.get(stream);
  const readableController = readableControllerOf(streamSlotsRecord.readable);
  if (!readableStreamDefaultControllerCanCloseOrEnqueue(readableController)) throw new TypeError('The readable side is not in a state that permits enqueue');
  try {
    readableStreamDefaultControllerEnqueue(readableController, chunk);
  } catch (e) {
    transformStreamErrorWritableAndUnblockWrite(stream, e);
    throw streamSlots.get(streamSlotsRecord.readable).storedError;
  }
  const backpressure = !readableStreamDefaultControllerShouldCallPull(readableController);
  if (backpressure !== streamSlotsRecord.backpressure) transformStreamSetBackpressure(stream, true);
};

const transformStreamDefaultControllerPerformTransform = (controller, chunk) => {
  const slots = transformControllerSlots.get(controller);
  const transformPromiseResult = slots.transformAlgorithm(chunk);
  return transformPromise(transformPromiseResult, undefined, (r) => {
    transformStreamError(slots.stream, r);
    throw r;
  });
};

const transformStreamDefaultControllerTerminate = (controller) => {
  const stream = transformControllerSlots.get(controller).stream;
  const readableController = readableControllerOf(transformStreamSlots.get(stream).readable);
  readableStreamDefaultControllerClose(readableController);
  const error = new TypeError('TransformStream terminated');
  transformStreamErrorWritableAndUnblockWrite(stream, error);
};

const transformStreamDefaultSinkWriteAlgorithm = (stream, chunk) => {
  const slots = transformStreamSlots.get(stream);
  const controller = slots.controller;
  if (slots.backpressure) {
    const backpressureChangePromise = slots.backpressureChangePromise;
    return transformPromise(backpressureChangePromise.promise, () => {
      const writable = slots.writable;
      const writableSlotsRecord = writableStreamSlots.get(writable);
      if (writableSlotsRecord.state === 'erroring') throw writableSlotsRecord.storedError;
      return transformStreamDefaultControllerPerformTransform(controller, chunk);
    }, undefined);
  }
  return transformStreamDefaultControllerPerformTransform(controller, chunk);
};

const transformStreamDefaultSinkAbortAlgorithm = (stream, reason) => {
  const slots = transformStreamSlots.get(stream);
  const controller = slots.controller;
  const controllerSlots = transformControllerSlots.get(controller);
  if (controllerSlots.finishPromise !== undefined) return controllerSlots.finishPromise.promise;
  const readable = slots.readable;
  controllerSlots.finishPromise = newPromiseCapability();
  const finish = controllerSlots.finishPromise;
  const cancelPromise = controllerSlots.cancelAlgorithm(reason);
  transformStreamDefaultControllerClearAlgorithms(controller);
  uponPromise(
    cancelPromise,
    () => {
      if (streamSlots.get(readable).state === 'errored') {
        finish.reject(streamSlots.get(readable).storedError);
      } else {
        readableStreamDefaultControllerError(readableControllerOf(readable), reason);
        finish.resolve(undefined);
      }
    },
    (r) => {
      readableStreamDefaultControllerError(readableControllerOf(readable), r);
      finish.reject(r);
    });
  return finish.promise;
};

const transformStreamDefaultSinkCloseAlgorithm = (stream) => {
  const slots = transformStreamSlots.get(stream);
  const controller = slots.controller;
  const controllerSlots = transformControllerSlots.get(controller);
  if (controllerSlots.finishPromise !== undefined) return controllerSlots.finishPromise.promise;
  const readable = slots.readable;
  controllerSlots.finishPromise = newPromiseCapability();
  const finish = controllerSlots.finishPromise;
  const flushPromise = controllerSlots.flushAlgorithm();
  transformStreamDefaultControllerClearAlgorithms(controller);
  uponPromise(
    flushPromise,
    () => {
      if (streamSlots.get(readable).state === 'errored') {
        finish.reject(streamSlots.get(readable).storedError);
      } else {
        readableStreamDefaultControllerClose(readableControllerOf(readable));
        finish.resolve(undefined);
      }
    },
    (r) => {
      readableStreamDefaultControllerError(readableControllerOf(readable), r);
      finish.reject(r);
    });
  return finish.promise;
};

const transformStreamDefaultSourcePullAlgorithm = (stream) => {
  transformStreamSetBackpressure(stream, false);
  return transformStreamSlots.get(stream).backpressureChangePromise.promise;
};

const transformStreamDefaultSourceCancelAlgorithm = (stream, reason) => {
  const slots = transformStreamSlots.get(stream);
  const controller = slots.controller;
  const controllerSlots = transformControllerSlots.get(controller);
  if (controllerSlots.finishPromise !== undefined) return controllerSlots.finishPromise.promise;
  const writable = slots.writable;
  controllerSlots.finishPromise = newPromiseCapability();
  const finish = controllerSlots.finishPromise;
  const cancelPromise = controllerSlots.cancelAlgorithm(reason);
  transformStreamDefaultControllerClearAlgorithms(controller);
  uponPromise(
    cancelPromise,
    () => {
      if (writableStreamSlots.get(writable).state === 'errored') {
        finish.reject(writableStreamSlots.get(writable).storedError);
      } else {
        writableStreamDefaultControllerErrorIfNeededForSlots(writableSlotsControllerOf(writable), reason);
        transformStreamUnblockWrite(stream);
        finish.resolve(undefined);
      }
    },
    (r) => {
      writableStreamDefaultControllerErrorIfNeededForSlots(writableSlotsControllerOf(writable), r);
      transformStreamUnblockWrite(stream);
      finish.reject(r);
    });
  return finish.promise;
};

// ======================================================================================
// Structured clone, for a tee of a byte stream, and the end
// ======================================================================================

const structuredCloneChunk = (chunk) => {
  if (typeof global.structuredClone === 'function') return reflectApply(global.structuredClone, global, [chunk]);
  throw new TypeError('structuredClone is not available');
};
