
// ======================================================================================
// Bodies, Blob.stream() and the text streams
// ======================================================================================

// What the host gives, taken once and taken off the global object.
const bodyState = global.__solarBodyState;
const bodyTake = global.__solarBodyTake;
const bodyError = global.__solarBodyError;
const bodyWatch = global.__solarBodyWatch;
const bodyFilled = global.__solarBodyFilled;
const bodyFailed = global.__solarBodyFailed;
const blobChunk = global.__solarBlobChunk;
const registerBody = global.__solarRegisterBody;
for (const name of ['__solarBodyState', '__solarBodyTake', '__solarBodyError', '__solarBodyWatch', '__solarBodyFilled', '__solarBodyFailed', '__solarBlobChunk', '__solarRegisterBody']) {
  delete global[name];
}

const TransformStreamReadable = getOwnPropertyDescriptor(TransformStream.prototype, 'readable').get;
const TransformStreamWritable = getOwnPropertyDescriptor(TransformStream.prototype, 'writable').get;
const BODY_WAIT = 0;
const BODY_DATA = 1;
const BODY_DONE = 2;

// A byte stream over the bytes of a body, which may still be arriving.
const makeBodyStream = (owner, used) => {
  let offset = 0;
  const stream = new ReadableStream({
    type: 'bytes',
    pull(controller) {
      const closeStream = () => {
        controller.close();
        const request = controller.byobRequest;
        if (request !== null) request.respond(0);
      };
      const step = () => {
        const state = bodyState(owner, offset);
        if (state === BODY_DATA) {
          const chunk = bodyTake(owner, offset);
          offset += chunk.length;
          controller.enqueue(chunk);
        } else if (state === BODY_DONE) {
          closeStream();
        } else if (state === BODY_WAIT) {
          return new Promise((resolve) => {
            bodyWatch(owner, () => {
              try {
                const again = step();
                if (again) again.then(resolve);
                else resolve();
              } catch (e) {
                controller.error(e);
                resolve();
              }
            });
          });
        } else {
          controller.error(bodyError(owner));
        }
        return undefined;
      };
      return step();
    },
  });
  if (used) {
    // The bytes were read before anyone asked for the stream: it is one that has been read to the end.
    acquireReadableStreamDefaultReader(stream);
    readableStreamCancel(stream, undefined);
  }
  return stream;
};

// All the bytes of `stream`, handed to the host as a Uint8Array when it ends, or the reason it failed.
const readStreamForBody = (owner, stream) => {
  const reader = acquireReadableStreamDefaultReader(stream);
  const chunks = [];
  let total = 0;
  const readRequest = {
    chunkSteps: (chunk) => {
      if (viewKind(chunk) !== 'typed' || typedArrayName(chunk) !== 'Uint8Array') {
        bodyFailed(owner, new TypeError('Received a chunk of the body that is not a Uint8Array'));
        return;
      }
      reflectApply(ArrayPrototypePush, chunks, [chunk]);
      total += viewByteLength(chunk);
      // Not from here: a stream with many chunks queued would otherwise be as deep as it is long.
      queueMicrotaskIntrinsic(() => readableStreamDefaultReaderRead(reader, readRequest));
    },
    closeSteps: () => {
      const bytes = new Uint8Array(total);
      let at = 0;
      for (const chunk of chunks) {
        const length = viewByteLength(chunk);
        copyDataBlockBytes(bytes.buffer, at, viewBuffer(chunk), viewByteOffset(chunk), length);
        at += length;
      }
      bodyFilled(owner, bytes);
    },
    errorSteps: (e) => bodyFailed(owner, e),
  };
  readableStreamDefaultReaderRead(reader, readRequest);
};

const makeBlobStream = (blob) => {
  let offset = 0;
  return new ReadableStream({
    type: 'bytes',
    pull(controller) {
      const chunk = blobChunk(blob, offset);
      if (chunk === null) {
        controller.close();
        const request = controller.byobRequest;
        if (request !== null) request.respond(0);
        return;
      }
      offset += chunk.length;
      controller.enqueue(chunk);
    },
  });
};

registerBody({
  make: makeBodyStream,
  readAll: readStreamForBody,
  tee: (stream) => readableStreamTee(stream, false),
  proxy: (stream) => {
    const transform = new TransformStream();
    const writable = reflectApply(TransformStreamWritable, transform, []);
    const promise = readableStreamPipeTo(stream, writable, false, false, false, undefined);
    uponPromise(promise, undefined, undefined);
    return reflectApply(TransformStreamReadable, transform, []);
  },
  isStream: (value) => isObject(value) && isReadableStream(value),
  isUnusable: (stream) => isReadableStreamLocked(stream) || streamSlots.get(stream).disturbed,
  isDisturbed: (stream) => streamSlots.get(stream).disturbed,
  blobStream: makeBlobStream,
});

// ---- TextDecoderStream and TextEncoderStream ----

const decoderPrototype = TextDecoder.prototype;
const decoderDecode = decoderPrototype.decode;
const decoderEncodingGetter = getOwnPropertyDescriptor(decoderPrototype, 'encoding').get;
const decoderFatalGetter = getOwnPropertyDescriptor(decoderPrototype, 'fatal').get;
const decoderIgnoreBOMGetter = getOwnPropertyDescriptor(decoderPrototype, 'ignoreBOM').get;
const encoderEncode = TextEncoder.prototype.encode;
const isArrayBuffer = (v) => {
  try {
    reflectApply(ArrayBufferByteLength, v, []);
    return true;
  } catch (e) {
    return false;
  }
};
const textStreamSlots = slotsOf(new WeakMap());

class TextDecoderStream {
  constructor(label = 'utf-8', options = {}) {
    const decoder = new TextDecoder(label, options);
    const transform = new TransformStream({
      transform(chunk, controller) {
        if (viewKind(chunk) === undefined && !isArrayBuffer(chunk)) throw new TypeError('The chunk is not a BufferSource');
        const text = reflectApply(decoderDecode, decoder, [chunk, { stream: true }]);
        if (text.length !== 0) controller.enqueue(text);
      },
      flush(controller) {
        const text = reflectApply(decoderDecode, decoder, []);
        if (text.length !== 0) controller.enqueue(text);
      },
    });
    textStreamSlots.set(this, { decoder, transform });
  }

  get encoding() {
    return reflectApply(decoderEncodingGetter, textStreamSlots.get(this).decoder, []);
  }

  get fatal() {
    return reflectApply(decoderFatalGetter, textStreamSlots.get(this).decoder, []);
  }

  get ignoreBOM() {
    return reflectApply(decoderIgnoreBOMGetter, textStreamSlots.get(this).decoder, []);
  }

  get readable() {
    return reflectApply(TransformStreamReadable, textStreamSlots.get(this).transform, []);
  }

  get writable() {
    return reflectApply(TransformStreamWritable, textStreamSlots.get(this).transform, []);
  }
}

class TextEncoderStream {
  constructor() {
    const encoder = new TextEncoder();
    let pendingHighSurrogate = null;
    const transform = new TransformStream({
      transform(chunk, controller) {
        let input = `${chunk}`;
        if (pendingHighSurrogate !== null) {
          input = pendingHighSurrogate + input;
          pendingHighSurrogate = null;
        }
        if (input.length === 0) return;
        const last = input.charCodeAt(input.length - 1);
        if (last >= 0xD800 && last <= 0xDBFF) {
          pendingHighSurrogate = input.charAt(input.length - 1);
          input = input.slice(0, -1);
        }
        const bytes = reflectApply(encoderEncode, encoder, [input]);
        if (bytes.length !== 0) controller.enqueue(bytes);
      },
      flush(controller) {
        if (pendingHighSurrogate !== null) controller.enqueue(new Uint8Array([0xEF, 0xBF, 0xBD]));
      },
    });
    textStreamSlots.set(this, { transform });
  }

  get encoding() {
    if (!textStreamSlots.has(this)) throw new TypeError('Illegal invocation');
    return 'utf-8';
  }

  get readable() {
    return reflectApply(TransformStreamReadable, textStreamSlots.get(this).transform, []);
  }

  get writable() {
    return reflectApply(TransformStreamWritable, textStreamSlots.get(this).transform, []);
  }
}
idlify(TextDecoderStream);
idlify(TextEncoderStream);
