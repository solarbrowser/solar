
// ======================================================================================
// The globals
// ======================================================================================

for (const Class of [ReadableStream, ReadableStreamDefaultReader, ReadableStreamBYOBReader, ReadableStreamDefaultController, ReadableByteStreamController, ReadableStreamBYOBRequest, WritableStream, WritableStreamDefaultWriter,
                     WritableStreamDefaultController, TransformStream, TransformStreamDefaultController, TextDecoderStream, TextEncoderStream]) {
  exposeGlobal(Class);
}
})(globalThis);
