
// ---- tee ----

const readableStreamTee = (stream, cloneForBranch2) => {
  if (streamSlots.get(stream).controller.kind === 'byte') return readableByteStreamTee(stream);
  return readableStreamDefaultTee(stream, cloneForBranch2);
};

const readableStreamDefaultTee = (stream, cloneForBranch2) => {
  const reader = acquireReadableStreamDefaultReader(stream);
  let reading = false;
  let readAgain = false;
  let canceled1 = false;
  let canceled2 = false;
  let reason1, reason2;
  let branch1, branch2;
  const cancelPromise = newPromiseCapability();

  const pullAlgorithm = () => {
    if (reading) {
      readAgain = true;
      return promiseResolvedWith(undefined);
    }
    reading = true;
    const readRequest = {
      chunkSteps: (chunk) => {
        queueMicrotask(() => {
          readAgain = false;
          const chunk1 = chunk;
          let chunk2 = chunk;
          if (!canceled2 && cloneForBranch2) {
            try {
              chunk2 = structuredCloneChunk(chunk2);
            } catch (e) {
              readableStreamDefaultControllerError(branch1Controller(), e);
              readableStreamDefaultControllerError(branch2Controller(), e);
              cancelPromise.resolve(readableStreamCancel(stream, e));
              return;
            }
          }
          if (!canceled1) readableStreamDefaultControllerEnqueue(branch1Controller(), chunk1);
          if (!canceled2) readableStreamDefaultControllerEnqueue(branch2Controller(), chunk2);
          reading = false;
          if (readAgain) pullAlgorithm();
        });
      },
      closeSteps: () => {
        reading = false;
        if (!canceled1) readableStreamDefaultControllerClose(branch1Controller());
        if (!canceled2) readableStreamDefaultControllerClose(branch2Controller());
        if (!canceled1 || !canceled2) cancelPromise.resolve(undefined);
      },
      errorSteps: () => { reading = false; },
    };
    readableStreamDefaultReaderRead(reader, readRequest);
    return promiseResolvedWith(undefined);
  };
  const cancel1Algorithm = (reason) => {
    canceled1 = true;
    reason1 = reason;
    if (canceled2) {
      const compositeReason = [reason1, reason2];
      cancelPromise.resolve(readableStreamCancel(stream, compositeReason));
    }
    return cancelPromise.promise;
  };
  const cancel2Algorithm = (reason) => {
    canceled2 = true;
    reason2 = reason;
    if (canceled1) {
      const compositeReason = [reason1, reason2];
      cancelPromise.resolve(readableStreamCancel(stream, compositeReason));
    }
    return cancelPromise.promise;
  };
  const startAlgorithm = () => undefined;
  branch1 = createReadableStream(startAlgorithm, pullAlgorithm, cancel1Algorithm);
  branch2 = createReadableStream(startAlgorithm, pullAlgorithm, cancel2Algorithm);
  const branch1Controller = () => reflectApply(WeakMapGet, readableStreamControllers, [branch1]);
  const branch2Controller = () => reflectApply(WeakMapGet, readableStreamControllers, [branch2]);
  uponRejection(readerGenericSlots(reader).closedPromise.promise, (r) => {
    readableStreamDefaultControllerError(branch1Controller(), r);
    readableStreamDefaultControllerError(branch2Controller(), r);
    if (!canceled1 || !canceled2) cancelPromise.resolve(undefined);
  });
  return [branch1, branch2];
};

// ---- Async iteration ----

const createReadableStreamAsyncIterator = (reader, preventCancel) => {
  const iterator = objectCreate(readableStreamAsyncIteratorPrototype);
  asyncIteratorSlots.set(iterator, { reader, preventCancel, ongoing: undefined, finished: false });
  return iterator;
};

const asyncIteratorSlots = slotsOf(new WeakMap());

// The iteration steps the standard gives ReadableStream, and under them the steps Web IDL gives every async
// iterator: next() and return() wait for each other, and the iterator result objects are made by a reaction
// to the promise the stream's steps return, which is one more turn of the job queue.
const endOfIteration = {};
const asyncIteratorMethods = {
  next() {
    if (!asyncIteratorSlots.has(this)) return promiseRejectedWith(new TypeError('Illegal invocation'));
    const slots = asyncIteratorSlots.get(this);
    const nextSteps = () => {
      const inner = (() => {
        if (slots.finished) return promiseResolvedWith(endOfIteration);
        const reader = slots.reader;
        if (readerGenericSlots(reader).stream === undefined) return promiseRejectedWith(new TypeError('The reader was released'));
        const capability = newPromiseCapability();
        readableStreamDefaultReaderRead(reader, {
          chunkSteps: (chunk) => capability.resolve(chunk),
          closeSteps: () => {
            readableStreamDefaultReaderRelease(reader);
            slots.finished = true;
            capability.resolve(endOfIteration);
          },
          errorSteps: (e) => {
            readableStreamDefaultReaderRelease(reader);
            slots.finished = true;
            capability.reject(e);
          },
        });
        return capability.promise;
      })();
      return transformPromise(inner, (v) => (v === endOfIteration ? { value: undefined, done: true } : { value: v, done: false }), undefined);
    };
    slots.ongoing = slots.ongoing === undefined ? nextSteps() : transformPromise(slots.ongoing, nextSteps, nextSteps);
    return slots.ongoing;
  },
  return(value) {
    if (!asyncIteratorSlots.has(this)) return promiseRejectedWith(new TypeError('Illegal invocation'));
    const slots = asyncIteratorSlots.get(this);
    const returnSteps = () => {
      const inner = (() => {
        if (slots.finished) return promiseResolvedWith(undefined);
        slots.finished = true;
        const reader = slots.reader;
        if (readerGenericSlots(reader).stream === undefined) return promiseRejectedWith(new TypeError('The reader was released'));
        if (!slots.preventCancel) {
          const result = readableStreamReaderGenericCancel(reader, value);
          readableStreamDefaultReaderRelease(reader);
          return result;
        }
        readableStreamDefaultReaderRelease(reader);
        return promiseResolvedWith(undefined);
      })();
      return transformPromise(inner, () => ({ value, done: true }), undefined);
    };
    slots.ongoing = slots.ongoing === undefined ? returnSteps() : transformPromise(slots.ongoing, returnSteps, returnSteps);
    return slots.ongoing;
  },
};
const readableStreamAsyncIteratorPrototype = objectCreate(AsyncIteratorPrototype, {
  next: { value: asyncIteratorMethods.next, writable: true, enumerable: true, configurable: true },
  return: { value: asyncIteratorMethods.return, writable: true, enumerable: true, configurable: true },
});
defineProperty(readableStreamAsyncIteratorPrototype, Symbol.toStringTag, { value: 'ReadableStream Async Iterator', configurable: true });

// ---- ReadableStream.from ----

const asyncFromSyncIterator = (syncRecord) => {
  const iterator = syncRecord.iterator;
  const continuation = (result, closeOnRejection) => {
    if (!isObject(result)) return promiseRejectedWith(new TypeError('The iterator result is not an object'));
    let done, value;
    try {
      done = Boolean(result.done);
      value = result.value;
    } catch (e) {
      return promiseRejectedWith(e);
    }
    const valueWrapper = (() => { try { return promiseResolvedWith(value); } catch (e) { return promiseRejectedWith(e); } })();
    return transformPromise(valueWrapper, (v) => ({ value: v, done }), closeOnRejection && !done ? (reason) => {
      try {
        const ret = iterator.return;
        if (ret !== undefined && ret !== null) reflectApply(ret, iterator, []);
      } catch (e) { /* the rejection stands */ }
      throw reason;
    } : undefined);
  };
  return {
    next(value) {
      try {
        const result = arguments.length > 0 ? reflectApply(syncRecord.next, iterator, [value]) : reflectApply(syncRecord.next, iterator, []);
        return continuation(result, true);
      } catch (e) {
        return promiseRejectedWith(e);
      }
    },
    return(value) {
      try {
        const ret = iterator.return;
        if (ret === undefined || ret === null) return promiseResolvedWith({ value, done: true });
        const result = arguments.length > 0 ? reflectApply(ret, iterator, [value]) : reflectApply(ret, iterator, []);
        if (!isObject(result)) return promiseRejectedWith(new TypeError('The iterator result is not an object'));
        return continuation(result, false);
      } catch (e) {
        return promiseRejectedWith(e);
      }
    },
  };
};

const getIterator = (object) => {
  const asyncMethod = object[Symbol.asyncIterator];
  if (asyncMethod !== undefined && asyncMethod !== null) {
    if (!isCallable(asyncMethod)) throw new TypeError('The @@asyncIterator is not a function');
    const iterator = reflectApply(asyncMethod, object, []);
    if (!isObject(iterator)) throw new TypeError('The iterator is not an object');
    return { iterator, next: iterator.next };
  }
  const syncMethod = object[Symbol.iterator];
  if (syncMethod === undefined || syncMethod === null) throw new TypeError('The object is not iterable');
  if (!isCallable(syncMethod)) throw new TypeError('The @@iterator is not a function');
  const syncIterator = reflectApply(syncMethod, object, []);
  if (!isObject(syncIterator)) throw new TypeError('The iterator is not an object');
  const syncRecord = { iterator: syncIterator, next: syncIterator.next };
  const wrapper = asyncFromSyncIterator(syncRecord);
  return { iterator: wrapper, next: wrapper.next };
};

const readableStreamFromIterable = (asyncIterable) => {
  if (!isObject(asyncIterable)) throw new TypeError('ReadableStream.from needs an object');
  const record = getIterator(asyncIterable);
  let stream;
  const startAlgorithm = () => undefined;
  const pullAlgorithm = () => {
    let nextResult;
    try {
      nextResult = reflectApply(record.next, record.iterator, []);
    } catch (e) {
      return promiseRejectedWith(e);
    }
    return transformPromise(promiseResolvedWith(nextResult), (iterResult) => {
      if (!isObject(iterResult)) throw new TypeError('The iterator result is not an object');
      if (Boolean(iterResult.done)) {
        readableStreamDefaultControllerClose(reflectApply(WeakMapGet, readableStreamControllers, [stream]));
      } else {
        readableStreamDefaultControllerEnqueue(reflectApply(WeakMapGet, readableStreamControllers, [stream]), iterResult.value);
      }
    }, undefined);
  };
  const cancelAlgorithm = (reason) => {
    const iterator = record.iterator;
    let returnMethod;
    try {
      returnMethod = iterator.return;
      if (returnMethod === null) returnMethod = undefined;
      if (returnMethod !== undefined && !isCallable(returnMethod)) throw new TypeError('The iterator\'s return is not a function');
    } catch (e) {
      return promiseRejectedWith(e);
    }
    if (returnMethod === undefined) return promiseResolvedWith(undefined);
    let returnResult;
    try {
      returnResult = reflectApply(returnMethod, iterator, [reason]);
    } catch (e) {
      return promiseRejectedWith(e);
    }
    return transformPromise(promiseResolvedWith(returnResult), (iterResult) => {
      if (!isObject(iterResult)) throw new TypeError('The iterator result is not an object');
    }, undefined);
  };
  stream = createReadableStream(startAlgorithm, pullAlgorithm, cancelAlgorithm, 0);
  return stream;
};
