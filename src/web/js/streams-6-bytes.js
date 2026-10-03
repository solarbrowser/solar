
// ======================================================================================
// Byte streams
// ======================================================================================

const byteControllerSlots = slotsOf(new WeakMap());
const byobReaderSlots = slotsOf(new WeakMap());
const byobRequestSlots = slotsOf(new WeakMap());

const isReadableStreamBYOBReader = (x) => byobReaderSlots.has(x);
const isReadableByteStreamController = (x) => byteControllerSlots.has(x);
const isReadableStreamBYOBRequest = (x) => byobRequestSlots.has(x);

const ArrayBufferTransferToFixedLength = ArrayBuffer.prototype.transferToFixedLength || ArrayBufferTransfer;
const isDetachedBuffer = (buffer) => reflectApply(ArrayBufferDetached, buffer, []);
const transferArrayBuffer = (buffer) => reflectApply(ArrayBufferTransferToFixedLength, buffer, []);
const arrayBufferByteLength = (buffer) => reflectApply(ArrayBufferByteLength, buffer, []);

// ArrayBufferViews, by their slots and not by what the page says about them.
const viewKind = (v) => {
  if (!isObject(v)) return undefined;
  try {
    reflectApply(TypedArrayBufferGetter, v, []);
    return 'typed';
  } catch (e) { /* not a typed array */ }
  try {
    reflectApply(DataViewBufferGetter, v, []);
    return 'dataview';
  } catch (e) { /* not a data view either */ }
  return undefined;
};
const viewBuffer = (v) => (viewKind(v) === 'typed' ? reflectApply(TypedArrayBufferGetter, v, []) : reflectApply(DataViewBufferGetter, v, []));
const viewByteOffset = (v) => (viewKind(v) === 'typed' ? reflectApply(TypedArrayByteOffsetGetter, v, []) : reflectApply(DataViewByteOffsetGetter, v, []));
const viewByteLength = (v) => (viewKind(v) === 'typed' ? reflectApply(TypedArrayByteLengthGetter, v, []) : reflectApply(DataViewByteLengthGetter, v, []));
const typedArrayName = (v) => reflectApply(TypedArrayToStringTag, v, []);
const typedArrayConstructors = {
  Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array, Int32Array, Uint32Array, Float32Array, Float64Array, BigInt64Array, BigUint64Array,
};
const cloneArrayBuffer = (buffer, byteOffset, byteLength) => reflectApply(ArrayBufferPrototypeSlice, buffer, [byteOffset, byteOffset + byteLength]);
const cloneAsUint8Array = (view) => new Uint8Array(cloneArrayBuffer(viewBuffer(view), viewByteOffset(view), viewByteLength(view)));
const copyDataBlockBytes = (destination, destinationStart, source, sourceStart, count) => {
  new Uint8Array(destination, destinationStart, count).set(new Uint8Array(source, sourceStart, count));
};

// ---- Read-into requests and the BYOB reader ----

const readableStreamAddReadIntoRequest = (stream, readIntoRequest) => {
  reflectApply(ArrayPrototypePush, byobReaderSlots.get(streamSlots.get(stream).reader).readIntoRequests, [readIntoRequest]);
};
const readableStreamFulfillReadIntoRequest = (stream, chunk, done) => {
  const reader = streamSlots.get(stream).reader;
  const readIntoRequest = reflectApply(ArrayPrototypeShift, byobReaderSlots.get(reader).readIntoRequests, []);
  if (done) readIntoRequest.closeSteps(chunk);
  else readIntoRequest.chunkSteps(chunk);
};
const readableStreamGetNumReadIntoRequests = (stream) => byobReaderSlots.get(streamSlots.get(stream).reader).readIntoRequests.length;
const readableStreamHasBYOBReader = (stream) => {
  const reader = streamSlots.get(stream).reader;
  return reader !== undefined && isReadableStreamBYOBReader(reader);
};

const readableStreamBYOBReaderErrorReadIntoRequests = (reader, e) => {
  const readIntoRequests = byobReaderSlots.get(reader).readIntoRequests;
  byobReaderSlots.get(reader).readIntoRequests = [];
  for (const readIntoRequest of readIntoRequests) readIntoRequest.errorSteps(e);
};

const readableStreamBYOBReaderRead = (reader, view, min, readIntoRequest) => {
  const stream = readerGenericSlots(reader).stream;
  const slots = streamSlots.get(stream);
  slots.disturbed = true;
  if (slots.state === 'errored') readIntoRequest.errorSteps(slots.storedError);
  else readableByteStreamControllerPullInto(slots.controller, view, min, readIntoRequest);
};

const readableStreamBYOBReaderRelease = (reader) => {
  readableStreamReaderGenericRelease(reader);
  readableStreamBYOBReaderErrorReadIntoRequests(reader, new TypeError('The reader was released'));
};

const setUpReadableStreamBYOBReader = (reader, stream) => {
  if (isReadableStreamLocked(stream)) throw new TypeError('This stream has already been locked for exclusive reading by another reader');
  if (streamSlots.get(stream).controller.kind !== 'byte') throw new TypeError('Cannot construct a ReadableStreamBYOBReader for a stream not constructed with a byte source');
  readableStreamReaderGenericInitialize(reader, stream);
  byobReaderSlots.set(reader, { readIntoRequests: [] });
};

const acquireReadableStreamBYOBReader = (stream) => {
  const reader = objectCreate(ReadableStreamBYOBReader.prototype);
  setUpReadableStreamBYOBReader(reader, stream);
  return reader;
};

class ReadableStreamBYOBReader {
  constructor(stream) {
    if (arguments.length < 1) throw new TypeError('Failed to construct \'ReadableStreamBYOBReader\': 1 argument required, but only 0 present.');
    if (!isReadableStream(stream)) throw new TypeError('Failed to construct \'ReadableStreamBYOBReader\': parameter 1 is not of type \'ReadableStream\'.');
    setUpReadableStreamBYOBReader(this, stream);
  }

  get closed() {
    if (!isReadableStreamBYOBReader(this)) return promiseRejectedWith(new TypeError('Illegal invocation'));
    return readerGenericSlots(this).closedPromise.promise;
  }

  cancel(reason = undefined) {
    if (!isReadableStreamBYOBReader(this)) return promiseRejectedWith(new TypeError('Illegal invocation'));
    if (readerGenericSlots(this).stream === undefined) return promiseRejectedWith(new TypeError('This reader has been released and cannot be used to cancel its previous owner stream'));
    return readableStreamReaderGenericCancel(this, reason);
  }

  read(view, options = {}) {
    if (!isReadableStreamBYOBReader(this)) return promiseRejectedWith(new TypeError('Illegal invocation'));
    if (arguments.length < 1) return promiseRejectedWith(new TypeError('Failed to execute \'read\' on \'ReadableStreamBYOBReader\': 1 argument required, but only 0 present.'));
    let min;
    try {
      if (viewKind(view) === undefined) throw new TypeError('The view is not an ArrayBufferView');
      if (options === undefined || options === null) options = {};
      else if (!isObject(options)) throw new TypeError('The options are not an object');
      min = options.min;
      if (min === undefined) {
        min = 1;
      } else {
        min = Number(min);
        if (!isFinite(min) || min < 0 || min > 18446744073709551615) throw new TypeError('The min option is out of range');
        min = Math.trunc(min);
      }
    } catch (e) {
      return promiseRejectedWith(e);
    }
    if (viewByteLength(view) === 0) return promiseRejectedWith(new TypeError('view must have non-zero byteLength'));
    if (arrayBufferByteLength(viewBuffer(view)) === 0) return promiseRejectedWith(new TypeError('view\'s buffer must have non-zero byteLength'));
    if (isDetachedBuffer(viewBuffer(view))) return promiseRejectedWith(new TypeError('view\'s buffer has been detached'));
    if (min === 0) return promiseRejectedWith(new TypeError('options.min must be greater than 0'));
    if (viewKind(view) === 'typed') {
      if (min > reflectApply(TypedArrayLengthGetter, view, [])) return promiseRejectedWith(new RangeError('options.min must be less than or equal to view\'s length'));
    } else if (min > viewByteLength(view)) {
      return promiseRejectedWith(new RangeError('options.min must be less than or equal to view\'s byteLength'));
    }
    if (readerGenericSlots(this).stream === undefined) return promiseRejectedWith(new TypeError('This reader has been released and cannot be used to read from its previous owner stream'));
    const capability = newPromiseCapability();
    readableStreamBYOBReaderRead(this, view, min, {
      chunkSteps: (chunk) => capability.resolve({ value: chunk, done: false }),
      closeSteps: (chunk) => capability.resolve({ value: chunk, done: true }),
      errorSteps: (e) => capability.reject(e),
    });
    return capability.promise;
  }

  releaseLock() {
    if (!isReadableStreamBYOBReader(this)) throw new TypeError('Illegal invocation');
    if (readerGenericSlots(this).stream === undefined) return;
    readableStreamBYOBReaderRelease(this);
  }
}
idlify(ReadableStreamBYOBReader);

// ---- BYOB request ----

class ReadableStreamBYOBRequest {
  constructor() { illegalConstructor(); }

  get view() {
    if (!isReadableStreamBYOBRequest(this)) throw new TypeError('Illegal invocation');
    return byobRequestSlots.get(this).view;
  }

  respond(bytesWritten) {
    if (!isReadableStreamBYOBRequest(this)) throw new TypeError('Illegal invocation');
    if (arguments.length < 1) throw new TypeError('Failed to execute \'respond\' on \'ReadableStreamBYOBRequest\': 1 argument required, but only 0 present.');
    bytesWritten = enforceRangeUnsignedLongLong(bytesWritten);
    const slots = byobRequestSlots.get(this);
    if (slots.controller === undefined) throw new TypeError('This BYOB request has been invalidated');
    if (isDetachedBuffer(viewBuffer(slots.view))) throw new TypeError('The BYOB request\'s buffer has been detached and so cannot be used as a response');
    readableByteStreamControllerRespond(slots.controller, bytesWritten);
  }

  respondWithNewView(view) {
    if (!isReadableStreamBYOBRequest(this)) throw new TypeError('Illegal invocation');
    if (arguments.length < 1) throw new TypeError('Failed to execute \'respondWithNewView\' on \'ReadableStreamBYOBRequest\': 1 argument required, but only 0 present.');
    if (viewKind(view) === undefined) throw new TypeError('You can only respond with array buffer views');
    const slots = byobRequestSlots.get(this);
    if (slots.controller === undefined) throw new TypeError('This BYOB request has been invalidated');
    if (isDetachedBuffer(viewBuffer(view))) throw new TypeError('The given view\'s buffer has been detached and so cannot be used as a response');
    readableByteStreamControllerRespondWithNewView(slots.controller, view);
  }
}
idlify(ReadableStreamBYOBRequest);

const enforceRangeUnsignedLongLong = (value) => {
  const n = Number(value);
  if (!isFinite(n)) throw new TypeError('The value is not a finite number');
  const truncated = Math.trunc(n);
  if (truncated < 0 || truncated > 9007199254740991) throw new TypeError('The value is out of range');
  return truncated;
};

// ---- Byte stream controller ----

class ReadableByteStreamController {
  constructor() { illegalConstructor(); }

  get byobRequest() {
    if (!isReadableByteStreamController(this)) throw new TypeError('Illegal invocation');
    return readableByteStreamControllerGetBYOBRequest(byteControllerSlots.get(this));
  }

  get desiredSize() {
    if (!isReadableByteStreamController(this)) throw new TypeError('Illegal invocation');
    return readableByteStreamControllerGetDesiredSize(byteControllerSlots.get(this));
  }

  close() {
    if (!isReadableByteStreamController(this)) throw new TypeError('Illegal invocation');
    const slots = byteControllerSlots.get(this);
    if (slots.closeRequested) throw new TypeError('The stream has already been closed; do not close it again!');
    if (streamSlots.get(slots.stream).state !== 'readable') throw new TypeError('The stream is not in the readable state and cannot be closed');
    readableByteStreamControllerClose(slots);
  }

  enqueue(chunk) {
    if (!isReadableByteStreamController(this)) throw new TypeError('Illegal invocation');
    if (arguments.length < 1) throw new TypeError('Failed to execute \'enqueue\' on \'ReadableByteStreamController\': 1 argument required, but only 0 present.');
    if (viewKind(chunk) === undefined) throw new TypeError('chunk must be an array buffer view');
    if (viewByteLength(chunk) === 0) throw new TypeError('chunk must have non-zero byteLength');
    if (arrayBufferByteLength(viewBuffer(chunk)) === 0) throw new TypeError('chunk\'s buffer must have non-zero byteLength');
    const slots = byteControllerSlots.get(this);
    if (slots.closeRequested) throw new TypeError('stream is closed or draining');
    if (streamSlots.get(slots.stream).state !== 'readable') throw new TypeError('The stream is not in the readable state and cannot be enqueued to');
    readableByteStreamControllerEnqueue(slots, chunk);
  }

  error(e = undefined) {
    if (!isReadableByteStreamController(this)) throw new TypeError('Illegal invocation');
    readableByteStreamControllerError(byteControllerSlots.get(this), e);
  }
}
idlify(ReadableByteStreamController);

const readableByteStreamControllerCallPullIfNeeded = (slots) => {
  if (!readableByteStreamControllerShouldCallPull(slots)) return;
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
        readableByteStreamControllerCallPullIfNeeded(slots);
      }
    },
    (e) => readableByteStreamControllerError(slots, e));
};

const readableByteStreamControllerClearAlgorithms = (slots) => {
  slots.pullAlgorithm = undefined;
  slots.cancelAlgorithm = undefined;
};

const readableByteStreamControllerClearPendingPullIntos = (slots) => {
  readableByteStreamControllerInvalidateBYOBRequest(slots);
  slots.pendingPullIntos = [];
};

const readableByteStreamControllerClose = (slots) => {
  const stream = slots.stream;
  if (slots.closeRequested || streamSlots.get(stream).state !== 'readable') return;
  if (slots.queueTotalSize > 0) {
    slots.closeRequested = true;
    return;
  }
  if (slots.pendingPullIntos.length > 0) {
    const firstPendingPullInto = slots.pendingPullIntos[0];
    if (firstPendingPullInto.bytesFilled % firstPendingPullInto.elementSize !== 0) {
      const e = new TypeError('Insufficient bytes to fill elements in the given buffer');
      readableByteStreamControllerError(slots, e);
      throw e;
    }
  }
  readableByteStreamControllerClearAlgorithms(slots);
  readableStreamClose(stream);
};

const readableByteStreamControllerCommitPullIntoDescriptor = (stream, pullIntoDescriptor) => {
  let done = false;
  if (streamSlots.get(stream).state === 'closed') done = true;
  const filledView = readableByteStreamControllerConvertPullIntoDescriptor(pullIntoDescriptor);
  if (pullIntoDescriptor.readerType === 'default') readableStreamFulfillReadRequest(stream, filledView, done);
  else readableStreamFulfillReadIntoRequest(stream, filledView, done);
};

const readableByteStreamControllerConvertPullIntoDescriptor = (pullIntoDescriptor) => {
  const { bytesFilled, elementSize } = pullIntoDescriptor;
  const buffer = transferArrayBuffer(pullIntoDescriptor.buffer);
  return new pullIntoDescriptor.viewConstructor(buffer, pullIntoDescriptor.byteOffset, bytesFilled / elementSize);
};

const readableByteStreamControllerEnqueueChunkToQueue = (slots, buffer, byteOffset, byteLength) => {
  reflectApply(ArrayPrototypePush, slots.queue, [{ buffer, byteOffset, byteLength }]);
  slots.queueTotalSize += byteLength;
};

const readableByteStreamControllerEnqueueClonedChunkToQueue = (slots, buffer, byteOffset, byteLength) => {
  let clonedChunk;
  try {
    clonedChunk = cloneArrayBuffer(buffer, byteOffset, byteLength);
  } catch (cloneE) {
    readableByteStreamControllerError(slots, cloneE);
    throw cloneE;
  }
  readableByteStreamControllerEnqueueChunkToQueue(slots, clonedChunk, 0, byteLength);
};

const readableByteStreamControllerEnqueueDetachedPullIntoToQueue = (slots, pullIntoDescriptor) => {
  if (pullIntoDescriptor.bytesFilled > 0) {
    readableByteStreamControllerEnqueueClonedChunkToQueue(slots, pullIntoDescriptor.buffer, pullIntoDescriptor.byteOffset, pullIntoDescriptor.bytesFilled);
  }
  readableByteStreamControllerShiftPendingPullInto(slots);
};

const readableByteStreamControllerError = (slots, e) => {
  const stream = slots.stream;
  if (streamSlots.get(stream).state !== 'readable') return;
  readableByteStreamControllerClearPendingPullIntos(slots);
  resetQueue(slots);
  readableByteStreamControllerClearAlgorithms(slots);
  readableStreamError(stream, e);
};

const readableByteStreamControllerFillHeadPullIntoDescriptor = (slots, size, pullIntoDescriptor) => {
  readableByteStreamControllerInvalidateBYOBRequest(slots);
  pullIntoDescriptor.bytesFilled += size;
};

const readableByteStreamControllerFillPullIntoDescriptorFromQueue = (slots, pullIntoDescriptor) => {
  const maxBytesToCopy = Math.min(slots.queueTotalSize, pullIntoDescriptor.byteLength - pullIntoDescriptor.bytesFilled);
  const maxBytesFilled = pullIntoDescriptor.bytesFilled + maxBytesToCopy;
  let totalBytesToCopyRemaining = maxBytesToCopy;
  let ready = false;
  const remainderBytes = maxBytesFilled % pullIntoDescriptor.elementSize;
  const maxAlignedBytes = maxBytesFilled - remainderBytes;
  if (maxAlignedBytes >= pullIntoDescriptor.minimumFill) {
    totalBytesToCopyRemaining = maxAlignedBytes - pullIntoDescriptor.bytesFilled;
    ready = true;
  }
  const queue = slots.queue;
  while (totalBytesToCopyRemaining > 0) {
    const headOfQueue = queue[0];
    const bytesToCopy = Math.min(totalBytesToCopyRemaining, headOfQueue.byteLength);
    const destStart = pullIntoDescriptor.byteOffset + pullIntoDescriptor.bytesFilled;
    copyDataBlockBytes(pullIntoDescriptor.buffer, destStart, headOfQueue.buffer, headOfQueue.byteOffset, bytesToCopy);
    if (headOfQueue.byteLength === bytesToCopy) {
      reflectApply(ArrayPrototypeShift, queue, []);
    } else {
      headOfQueue.byteOffset += bytesToCopy;
      headOfQueue.byteLength -= bytesToCopy;
    }
    slots.queueTotalSize -= bytesToCopy;
    readableByteStreamControllerFillHeadPullIntoDescriptor(slots, bytesToCopy, pullIntoDescriptor);
    totalBytesToCopyRemaining -= bytesToCopy;
  }
  return ready;
};

const readableByteStreamControllerFillReadRequestFromQueue = (slots, readRequest) => {
  const entry = reflectApply(ArrayPrototypeShift, slots.queue, []);
  slots.queueTotalSize -= entry.byteLength;
  readableByteStreamControllerHandleQueueDrain(slots);
  const view = new Uint8Array(entry.buffer, entry.byteOffset, entry.byteLength);
  readRequest.chunkSteps(view);
};

const readableByteStreamControllerGetBYOBRequest = (slots) => {
  if (slots.byobRequest === null && slots.pendingPullIntos.length > 0) {
    const firstDescriptor = slots.pendingPullIntos[0];
    const view = new Uint8Array(firstDescriptor.buffer, firstDescriptor.byteOffset + firstDescriptor.bytesFilled, firstDescriptor.byteLength - firstDescriptor.bytesFilled);
    const byobRequest = objectCreate(ReadableStreamBYOBRequest.prototype);
    byobRequestSlots.set(byobRequest, { controller: slots, view });
    slots.byobRequest = byobRequest;
  }
  return slots.byobRequest;
};

const readableByteStreamControllerGetDesiredSize = (slots) => {
  const state = streamSlots.get(slots.stream).state;
  if (state === 'errored') return null;
  if (state === 'closed') return 0;
  return slots.strategyHWM - slots.queueTotalSize;
};

const readableByteStreamControllerHandleQueueDrain = (slots) => {
  if (slots.queueTotalSize === 0 && slots.closeRequested) {
    readableByteStreamControllerClearAlgorithms(slots);
    readableStreamClose(slots.stream);
  } else {
    readableByteStreamControllerCallPullIfNeeded(slots);
  }
};

const readableByteStreamControllerInvalidateBYOBRequest = (slots) => {
  if (slots.byobRequest === null) return;
  const request = byobRequestSlots.get(slots.byobRequest);
  request.controller = undefined;
  request.view = null;
  slots.byobRequest = null;
};

const readableByteStreamControllerProcessPullIntoDescriptorsUsingQueue = (slots) => {
  const filledPullIntos = [];
  while (slots.pendingPullIntos.length > 0) {
    if (slots.queueTotalSize === 0) break;
    const pullIntoDescriptor = slots.pendingPullIntos[0];
    if (readableByteStreamControllerFillPullIntoDescriptorFromQueue(slots, pullIntoDescriptor)) {
      readableByteStreamControllerShiftPendingPullInto(slots);
      reflectApply(ArrayPrototypePush, filledPullIntos, [pullIntoDescriptor]);
    }
  }
  for (const pullIntoDescriptor of filledPullIntos) readableByteStreamControllerCommitPullIntoDescriptor(slots.stream, pullIntoDescriptor);
};

const readableByteStreamControllerProcessReadRequestsUsingQueue = (slots) => {
  const reader = streamSlots.get(slots.stream).reader;
  const readRequests = defaultReaderSlots.get(reader).readRequests;
  while (readRequests.length > 0) {
    if (slots.queueTotalSize === 0) return;
    const readRequest = reflectApply(ArrayPrototypeShift, readRequests, []);
    readableByteStreamControllerFillReadRequestFromQueue(slots, readRequest);
  }
};

const readableByteStreamControllerPullInto = (slots, view, min, readIntoRequest) => {
  const stream = slots.stream;
  let elementSize = 1;
  let ViewConstructor = DataView;
  if (viewKind(view) === 'typed') {
    ViewConstructor = typedArrayConstructors[typedArrayName(view)];
    elementSize = ViewConstructor.BYTES_PER_ELEMENT;
  }
  const minimumFill = min * elementSize;
  const byteOffset = viewByteOffset(view);
  const byteLength = viewByteLength(view);
  let buffer;
  try {
    buffer = transferArrayBuffer(viewBuffer(view));
  } catch (e) {
    readIntoRequest.errorSteps(e);
    return;
  }
  const pullIntoDescriptor = {
    buffer, bufferByteLength: arrayBufferByteLength(buffer), byteOffset, byteLength, bytesFilled: 0, minimumFill, elementSize,
    viewConstructor: ViewConstructor, readerType: 'byob',
  };
  if (slots.pendingPullIntos.length > 0) {
    reflectApply(ArrayPrototypePush, slots.pendingPullIntos, [pullIntoDescriptor]);
    readableStreamAddReadIntoRequest(stream, readIntoRequest);
    return;
  }
  if (streamSlots.get(stream).state === 'closed') {
    const emptyView = new ViewConstructor(pullIntoDescriptor.buffer, pullIntoDescriptor.byteOffset, 0);
    readIntoRequest.closeSteps(emptyView);
    return;
  }
  if (slots.queueTotalSize > 0) {
    if (readableByteStreamControllerFillPullIntoDescriptorFromQueue(slots, pullIntoDescriptor)) {
      const filledView = readableByteStreamControllerConvertPullIntoDescriptor(pullIntoDescriptor);
      readableByteStreamControllerHandleQueueDrain(slots);
      readIntoRequest.chunkSteps(filledView);
      return;
    }
    if (slots.closeRequested) {
      const e = new TypeError('Insufficient bytes to fill elements in the given buffer');
      readableByteStreamControllerError(slots, e);
      readIntoRequest.errorSteps(e);
      return;
    }
  }
  reflectApply(ArrayPrototypePush, slots.pendingPullIntos, [pullIntoDescriptor]);
  readableStreamAddReadIntoRequest(stream, readIntoRequest);
  readableByteStreamControllerCallPullIfNeeded(slots);
};

const readableByteStreamControllerRespondInClosedState = (slots, firstDescriptor) => {
  if (firstDescriptor.readerType === 'none') readableByteStreamControllerShiftPendingPullInto(slots);
  const stream = slots.stream;
  if (readableStreamHasBYOBReader(stream)) {
    while (readableStreamGetNumReadIntoRequests(stream) > 0) {
      const pullIntoDescriptor = readableByteStreamControllerShiftPendingPullInto(slots);
      readableByteStreamControllerCommitPullIntoDescriptor(stream, pullIntoDescriptor);
    }
  }
};

const readableByteStreamControllerRespondInReadableState = (slots, bytesWritten, pullIntoDescriptor) => {
  readableByteStreamControllerFillHeadPullIntoDescriptor(slots, bytesWritten, pullIntoDescriptor);
  if (pullIntoDescriptor.readerType === 'none') {
    readableByteStreamControllerEnqueueDetachedPullIntoToQueue(slots, pullIntoDescriptor);
    readableByteStreamControllerProcessPullIntoDescriptorsUsingQueue(slots);
    return;
  }
  if (pullIntoDescriptor.bytesFilled < pullIntoDescriptor.minimumFill) return;
  readableByteStreamControllerShiftPendingPullInto(slots);
  const remainderSize = pullIntoDescriptor.bytesFilled % pullIntoDescriptor.elementSize;
  if (remainderSize > 0) {
    const end = pullIntoDescriptor.byteOffset + pullIntoDescriptor.bytesFilled;
    readableByteStreamControllerEnqueueClonedChunkToQueue(slots, pullIntoDescriptor.buffer, end - remainderSize, remainderSize);
  }
  pullIntoDescriptor.bytesFilled -= remainderSize;
  readableByteStreamControllerCommitPullIntoDescriptor(slots.stream, pullIntoDescriptor);
  readableByteStreamControllerProcessPullIntoDescriptorsUsingQueue(slots);
};

const readableByteStreamControllerRespondInternal = (slots, bytesWritten) => {
  const firstDescriptor = slots.pendingPullIntos[0];
  readableByteStreamControllerInvalidateBYOBRequest(slots);
  const state = streamSlots.get(slots.stream).state;
  if (state === 'closed') readableByteStreamControllerRespondInClosedState(slots, firstDescriptor);
  else readableByteStreamControllerRespondInReadableState(slots, bytesWritten, firstDescriptor);
  readableByteStreamControllerCallPullIfNeeded(slots);
};

const readableByteStreamControllerRespond = (slots, bytesWritten) => {
  const firstDescriptor = slots.pendingPullIntos[0];
  const state = streamSlots.get(slots.stream).state;
  if (state === 'closed') {
    if (bytesWritten !== 0) throw new TypeError('bytesWritten must be 0 when calling respond() on a closed stream');
  } else {
    if (bytesWritten === 0) throw new TypeError('bytesWritten must be greater than 0 when calling respond() on a readable stream');
    if (firstDescriptor.bytesFilled + bytesWritten > firstDescriptor.byteLength) throw new RangeError('bytesWritten out of range');
  }
  firstDescriptor.buffer = transferArrayBuffer(firstDescriptor.buffer);
  readableByteStreamControllerRespondInternal(slots, bytesWritten);
};

const readableByteStreamControllerRespondWithNewView = (slots, view) => {
  const firstDescriptor = slots.pendingPullIntos[0];
  const state = streamSlots.get(slots.stream).state;
  if (state === 'closed') {
    if (viewByteLength(view) !== 0) throw new TypeError('The view\'s length must be 0 when calling respondWithNewView() on a closed stream');
  } else if (viewByteLength(view) === 0) {
    throw new TypeError('The view\'s length must be greater than 0 when calling respondWithNewView() on a readable stream');
  }
  if (firstDescriptor.byteOffset + firstDescriptor.bytesFilled !== viewByteOffset(view)) throw new RangeError('The region specified by view does not match byobRequest');
  if (firstDescriptor.bufferByteLength !== arrayBufferByteLength(viewBuffer(view))) throw new RangeError('The buffer of view has different capacity than byobRequest');
  if (firstDescriptor.bytesFilled + viewByteLength(view) > firstDescriptor.byteLength) throw new RangeError('The region specified by view is larger than byobRequest');
  const viewByteLengthValue = viewByteLength(view);
  firstDescriptor.buffer = transferArrayBuffer(viewBuffer(view));
  readableByteStreamControllerRespondInternal(slots, viewByteLengthValue);
};

const readableByteStreamControllerShiftPendingPullInto = (slots) => {
  const descriptor = reflectApply(ArrayPrototypeShift, slots.pendingPullIntos, []);
  readableByteStreamControllerInvalidateBYOBRequest(slots);
  return descriptor;
};

const readableByteStreamControllerShouldCallPull = (slots) => {
  const stream = slots.stream;
  if (streamSlots.get(stream).state !== 'readable') return false;
  if (slots.closeRequested) return false;
  if (!slots.started) return false;
  if (readableStreamHasDefaultReader(stream) && readableStreamGetNumReadRequests(stream) > 0) return true;
  if (readableStreamHasBYOBReader(stream) && readableStreamGetNumReadIntoRequests(stream) > 0) return true;
  const desiredSize = readableByteStreamControllerGetDesiredSize(slots);
  return desiredSize > 0;
};

const readableByteStreamControllerEnqueue = (slots, chunk) => {
  const stream = slots.stream;
  if (slots.closeRequested || streamSlots.get(stream).state !== 'readable') return;
  const buffer = viewBuffer(chunk);
  const byteOffset = viewByteOffset(chunk);
  const byteLength = viewByteLength(chunk);
  if (isDetachedBuffer(buffer)) throw new TypeError('chunk\'s buffer is detached and so cannot be enqueued');
  const transferredBuffer = transferArrayBuffer(buffer);
  if (slots.pendingPullIntos.length > 0) {
    const firstPendingPullInto = slots.pendingPullIntos[0];
    if (isDetachedBuffer(firstPendingPullInto.buffer)) throw new TypeError('The BYOB request\'s buffer has been detached and so cannot be filled with an enqueued chunk');
    readableByteStreamControllerInvalidateBYOBRequest(slots);
    firstPendingPullInto.buffer = transferArrayBuffer(firstPendingPullInto.buffer);
    if (firstPendingPullInto.readerType === 'none') readableByteStreamControllerEnqueueDetachedPullIntoToQueue(slots, firstPendingPullInto);
  }
  if (readableStreamHasDefaultReader(stream)) {
    readableByteStreamControllerProcessReadRequestsUsingQueue(slots);
    if (readableStreamGetNumReadRequests(stream) === 0) {
      readableByteStreamControllerEnqueueChunkToQueue(slots, transferredBuffer, byteOffset, byteLength);
    } else {
      if (slots.pendingPullIntos.length > 0) readableByteStreamControllerShiftPendingPullInto(slots);
      const transferredView = new Uint8Array(transferredBuffer, byteOffset, byteLength);
      readableStreamFulfillReadRequest(stream, transferredView, false);
    }
  } else if (readableStreamHasBYOBReader(stream)) {
    readableByteStreamControllerEnqueueChunkToQueue(slots, transferredBuffer, byteOffset, byteLength);
    readableByteStreamControllerProcessPullIntoDescriptorsUsingQueue(slots);
  } else {
    readableByteStreamControllerEnqueueChunkToQueue(slots, transferredBuffer, byteOffset, byteLength);
  }
  readableByteStreamControllerCallPullIfNeeded(slots);
};

const setUpReadableByteStreamController = (stream, controller, startAlgorithm, pullAlgorithm, cancelAlgorithm, highWaterMark, autoAllocateChunkSize) => {
  const slots = {
    kind: 'byte', stream, pullAgain: false, pulling: false, byobRequest: null, queue: [], queueTotalSize: 0, closeRequested: false, started: false,
    strategyHWM: highWaterMark, pullAlgorithm, cancelAlgorithm, autoAllocateChunkSize, pendingPullIntos: [],
  };
  slots.cancelSteps = (reason) => {
    readableByteStreamControllerClearPendingPullIntos(slots);
    resetQueue(slots);
    const result = slots.cancelAlgorithm(reason);
    readableByteStreamControllerClearAlgorithms(slots);
    return result;
  };
  slots.pullSteps = (readRequest) => {
    const theStream = slots.stream;
    if (slots.queueTotalSize > 0) {
      readableByteStreamControllerFillReadRequestFromQueue(slots, readRequest);
      return;
    }
    const size = slots.autoAllocateChunkSize;
    if (size !== undefined) {
      let buffer;
      try {
        buffer = new ArrayBuffer(size);
      } catch (bufferE) {
        readRequest.errorSteps(bufferE);
        return;
      }
      reflectApply(ArrayPrototypePush, slots.pendingPullIntos, [{
        buffer, bufferByteLength: size, byteOffset: 0, byteLength: size, bytesFilled: 0, minimumFill: 1, elementSize: 1, viewConstructor: Uint8Array, readerType: 'default',
      }]);
    }
    readableStreamAddReadRequest(theStream, readRequest);
    readableByteStreamControllerCallPullIfNeeded(slots);
  };
  slots.releaseSteps = () => {
    if (slots.pendingPullIntos.length > 0) {
      const firstPendingPullInto = slots.pendingPullIntos[0];
      firstPendingPullInto.readerType = 'none';
      slots.pendingPullIntos = [firstPendingPullInto];
    }
  };
  byteControllerSlots.set(controller, slots);
  streamSlots.get(stream).controller = slots;

  const startResult = startAlgorithm();
  uponPromise(
    promiseResolvedWith(startResult),
    () => {
      slots.started = true;
      readableByteStreamControllerCallPullIfNeeded(slots);
    },
    (r) => readableByteStreamControllerError(slots, r));
};

const setUpReadableByteStreamControllerFromUnderlyingSource = (stream, underlyingSource, sourceDict, highWaterMark) => {
  const controller = objectCreate(ReadableByteStreamController.prototype);
  const startAlgorithm = sourceDict.start !== undefined ? () => reflectApply(sourceDict.start, underlyingSource, [controller]) : () => undefined;
  const pullAlgorithm = sourceDict.pull !== undefined ? () => promiseCall(sourceDict.pull, underlyingSource, [controller]) : () => promiseResolvedWith(undefined);
  const cancelAlgorithm = sourceDict.cancel !== undefined ? (reason) => promiseCall(sourceDict.cancel, underlyingSource, [reason]) : () => promiseResolvedWith(undefined);
  const autoAllocateChunkSize = sourceDict.autoAllocateChunkSize;
  if (autoAllocateChunkSize === 0) throw new TypeError('autoAllocateChunkSize must be greater than 0');
  reflectApply(WeakMapSet, readableStreamControllers, [stream, controller]);
  setUpReadableByteStreamController(stream, controller, startAlgorithm, pullAlgorithm, cancelAlgorithm, highWaterMark, autoAllocateChunkSize);
};

const createReadableByteStream = (startAlgorithm, pullAlgorithm, cancelAlgorithm) => {
  const stream = objectCreate(ReadableStream.prototype);
  streamSlots.set(stream, { state: 'readable', reader: undefined, storedError: undefined, disturbed: false, controller: undefined });
  const controller = objectCreate(ReadableByteStreamController.prototype);
  reflectApply(WeakMapSet, readableStreamControllers, [stream, controller]);
  setUpReadableByteStreamController(stream, controller, startAlgorithm, pullAlgorithm, cancelAlgorithm, 0, undefined);
  return stream;
};

// ---- tee of a byte stream ----

const readableByteStreamTee = (stream) => {
  let reader = acquireReadableStreamDefaultReader(stream);
  let reading = false;
  let readAgainForBranch1 = false;
  let readAgainForBranch2 = false;
  let canceled1 = false;
  let canceled2 = false;
  let reason1, reason2;
  let branch1, branch2;
  const cancelPromise = newPromiseCapability();
  const slotsOfBranch = (branch) => byteControllerSlots.get(reflectApply(WeakMapGet, readableStreamControllers, [branch]));

  const forwardReaderError = (thisReader) => {
    uponRejection(readerGenericSlots(thisReader).closedPromise.promise, (r) => {
      if (thisReader !== reader) return;
      readableByteStreamControllerError(slotsOfBranch(branch1), r);
      readableByteStreamControllerError(slotsOfBranch(branch2), r);
      if (!canceled1 || !canceled2) cancelPromise.resolve(undefined);
    });
  };

  const pullWithDefaultReader = () => {
    if (isReadableStreamBYOBReader(reader)) {
      readableStreamBYOBReaderRelease(reader);
      reader = acquireReadableStreamDefaultReader(stream);
      forwardReaderError(reader);
    }
    const readRequest = {
      chunkSteps: (chunk) => {
        queueMicrotask(() => {
          readAgainForBranch1 = false;
          readAgainForBranch2 = false;
          const chunk1 = chunk;
          let chunk2 = chunk;
          if (!canceled1 && !canceled2) {
            try {
              chunk2 = cloneAsUint8Array(chunk);
            } catch (cloneE) {
              readableByteStreamControllerError(slotsOfBranch(branch1), cloneE);
              readableByteStreamControllerError(slotsOfBranch(branch2), cloneE);
              cancelPromise.resolve(readableStreamCancel(stream, cloneE));
              return;
            }
          }
          if (!canceled1) readableByteStreamControllerEnqueue(slotsOfBranch(branch1), chunk1);
          if (!canceled2) readableByteStreamControllerEnqueue(slotsOfBranch(branch2), chunk2);
          reading = false;
          if (readAgainForBranch1) pull1Algorithm();
          else if (readAgainForBranch2) pull2Algorithm();
        });
      },
      closeSteps: () => {
        reading = false;
        if (!canceled1) readableByteStreamControllerClose(slotsOfBranch(branch1));
        if (!canceled2) readableByteStreamControllerClose(slotsOfBranch(branch2));
        if (slotsOfBranch(branch1).pendingPullIntos.length > 0) readableByteStreamControllerRespond(slotsOfBranch(branch1), 0);
        if (slotsOfBranch(branch2).pendingPullIntos.length > 0) readableByteStreamControllerRespond(slotsOfBranch(branch2), 0);
        if (!canceled1 || !canceled2) cancelPromise.resolve(undefined);
      },
      errorSteps: () => { reading = false; },
    };
    readableStreamDefaultReaderRead(reader, readRequest);
  };

  const pullWithBYOBReader = (view, forBranch2) => {
    if (isReadableStreamDefaultReader(reader)) {
      readableStreamDefaultReaderRelease(reader);
      reader = acquireReadableStreamBYOBReader(stream);
      forwardReaderError(reader);
    }
    const byobBranch = forBranch2 ? branch2 : branch1;
    const otherBranch = forBranch2 ? branch1 : branch2;
    const readIntoRequest = {
      chunkSteps: (chunk) => {
        queueMicrotask(() => {
          readAgainForBranch1 = false;
          readAgainForBranch2 = false;
          const byobCanceled = forBranch2 ? canceled2 : canceled1;
          const otherCanceled = forBranch2 ? canceled1 : canceled2;
          if (!otherCanceled) {
            let clonedChunk;
            try {
              clonedChunk = cloneAsUint8Array(chunk);
            } catch (cloneE) {
              readableByteStreamControllerError(slotsOfBranch(byobBranch), cloneE);
              readableByteStreamControllerError(slotsOfBranch(otherBranch), cloneE);
              cancelPromise.resolve(readableStreamCancel(stream, cloneE));
              return;
            }
            if (!byobCanceled) readableByteStreamControllerRespondWithNewView(slotsOfBranch(byobBranch), chunk);
            readableByteStreamControllerEnqueue(slotsOfBranch(otherBranch), clonedChunk);
          } else if (!byobCanceled) {
            readableByteStreamControllerRespondWithNewView(slotsOfBranch(byobBranch), chunk);
          }
          reading = false;
          if (readAgainForBranch1) pull1Algorithm();
          else if (readAgainForBranch2) pull2Algorithm();
        });
      },
      closeSteps: (chunk) => {
        reading = false;
        const byobCanceled = forBranch2 ? canceled2 : canceled1;
        const otherCanceled = forBranch2 ? canceled1 : canceled2;
        if (!byobCanceled) readableByteStreamControllerClose(slotsOfBranch(byobBranch));
        if (!otherCanceled) readableByteStreamControllerClose(slotsOfBranch(otherBranch));
        if (chunk !== undefined) {
          if (!byobCanceled) readableByteStreamControllerRespondWithNewView(slotsOfBranch(byobBranch), chunk);
          if (!otherCanceled && slotsOfBranch(otherBranch).pendingPullIntos.length > 0) readableByteStreamControllerRespond(slotsOfBranch(otherBranch), 0);
        }
        if (!byobCanceled || !otherCanceled) cancelPromise.resolve(undefined);
      },
      errorSteps: () => { reading = false; },
    };
    readableStreamBYOBReaderRead(reader, view, 1, readIntoRequest);
  };

  const pull1Algorithm = () => {
    if (reading) {
      readAgainForBranch1 = true;
      return promiseResolvedWith(undefined);
    }
    reading = true;
    const byobRequest = readableByteStreamControllerGetBYOBRequest(slotsOfBranch(branch1));
    if (byobRequest === null) pullWithDefaultReader();
    else pullWithBYOBReader(byobRequestSlots.get(byobRequest).view, false);
    return promiseResolvedWith(undefined);
  };
  const pull2Algorithm = () => {
    if (reading) {
      readAgainForBranch2 = true;
      return promiseResolvedWith(undefined);
    }
    reading = true;
    const byobRequest = readableByteStreamControllerGetBYOBRequest(slotsOfBranch(branch2));
    if (byobRequest === null) pullWithDefaultReader();
    else pullWithBYOBReader(byobRequestSlots.get(byobRequest).view, true);
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
  branch1 = createReadableByteStream(startAlgorithm, pull1Algorithm, cancel1Algorithm);
  branch2 = createReadableByteStream(startAlgorithm, pull2Algorithm, cancel2Algorithm);
  forwardReaderError(reader);
  return [branch1, branch2];
};
