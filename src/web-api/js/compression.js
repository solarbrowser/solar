// CompressionStream and DecompressionStream (https://compression.spec.whatwg.org/): a TransformStream over zlib (src/web-api/Compression.cpp), for gzip, deflate
// (zlib-wrapped) and deflate-raw.
(() => {
  "use strict";
  const make = globalThis.__solarZNew, write = globalThis.__solarZWrite, finish = globalThis.__solarZFinish, free = globalThis.__solarZFree;
  delete globalThis.__solarZNew; delete globalThis.__solarZWrite; delete globalThis.__solarZFinish; delete globalThis.__solarZFree;
  if (typeof make !== "function" || typeof TransformStream !== "function") return;

  const formats = ["gzip", "deflate", "deflate-raw"];
  const chunkBytes = (chunk) => {
    if (chunk instanceof ArrayBuffer) return new Uint8Array(chunk);
    if (ArrayBuffer.isView(chunk)) return new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength);
    throw new TypeError("The chunk is not an ArrayBuffer or ArrayBufferView.");
  };

  const define = (name, compress) => {
    const verb = compress ? "Compression" : "Decompression";
    const slots = new WeakMap();
    class Stream {
      constructor(format) {
        if (arguments.length === 0) throw new TypeError(`Failed to construct '${name}': 1 argument required, but only 0 present.`);
        format = String(format);
        if (!formats.includes(format)) throw new TypeError(`Failed to construct '${name}': Unsupported compression format: '${format}'`);
        const id = make(format, compress ? 1 : 0);
        if (id < 0) throw new TypeError(`Failed to construct '${name}': the format is not available.`);
        let done = false;
        const end = () => { if (!done) { done = true; free(id); } };
        const flush = (out, controller) => { if (out.byteLength) controller.enqueue(new Uint8Array(out)); };
        slots.set(this, new TransformStream({
          transform(chunk, controller) {
            const out = write(id, chunkBytes(chunk));
            if (out === undefined) { end(); throw new TypeError(compress ? "Compression failed." : "The compressed data is not valid."); }
            flush(out, controller);
          },
          flush(controller) {
            const out = finish(id);
            end();
            if (out === undefined) throw new TypeError(compress ? "Compression failed." : "Unexpected end of the compressed data.");
            flush(out, controller);
          },
          cancel: end,
        }));
      }
      get readable() { return slots.get(this).readable; }
      get writable() { return slots.get(this).writable; }
    }
    Object.defineProperty(Stream, "name", { value: name });
    Object.defineProperty(Stream.prototype, Symbol.toStringTag, { value: name, configurable: true });
    Object.defineProperty(globalThis, name, { value: Stream, writable: true, configurable: true, enumerable: false });
    void verb;
  };
  define("CompressionStream", true);
  define("DecompressionStream", false);
})();
