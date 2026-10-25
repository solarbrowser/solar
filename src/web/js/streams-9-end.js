/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */


// ======================================================================================
// The globals
// ======================================================================================

for (const Class of [ReadableStream, ReadableStreamDefaultReader, ReadableStreamBYOBReader, ReadableStreamDefaultController, ReadableByteStreamController, ReadableStreamBYOBRequest, WritableStream, WritableStreamDefaultWriter,
                     WritableStreamDefaultController, TransformStream, TransformStreamDefaultController, TextDecoderStream, TextEncoderStream]) {
  exposeGlobal(Class);
}
})(globalThis);
