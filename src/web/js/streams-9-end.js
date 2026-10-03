
// ======================================================================================
// The globals
// ======================================================================================

for (const Class of [ReadableStream, ReadableStreamDefaultReader, ReadableStreamBYOBReader, ReadableStreamDefaultController, ReadableByteStreamController, ReadableStreamBYOBRequest, WritableStream, WritableStreamDefaultWriter,
                     WritableStreamDefaultController, TransformStream, TransformStreamDefaultController]) {
  exposeGlobal(Class);
}
})(globalThis);
