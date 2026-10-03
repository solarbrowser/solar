#include "solar/web/FetchBindings.h"

#include "solar/web/FetchBindingsInternal.h"

namespace solar::web {

namespace {

// Realm and Runtime both offer GetContext and Evaluate, which is all setup needs.
template <typename Host>
void Install(Host& host) {
  DefineEncodingClasses(host.GetContext());
  DefineBlobClasses(host.GetContext());
  DefineFormDataClass(host.GetContext());
  DefineHeadersClass(host.GetContext());
  DefineResponseClass(host.GetContext());
  DefineRequestClass(host.GetContext());
  DefineFetchFunction(host.GetContext());
  // Web IDL makes @@iterator the same function object as entries, and the embedding surface has no
  // symbol-keyed definitions, so it is installed from script.
  host.Evaluate("Headers.prototype[Symbol.iterator] = Headers.prototype.entries; FormData.prototype[Symbol.iterator] = FormData.prototype.entries; Object.setPrototypeOf(File, Blob);");

  // encodeInto returns a dictionary, a plain object, which a native cannot make; the native does the
  // work and this puts its answer in the shape the standard gives.
  host.Evaluate(R"JS(
    (function () {
      const native = __solarEncodeInto;
      delete globalThis.__solarEncodeInto;
      const check = TextEncoder.prototype.encode;
      const encodeInto = ({ encodeInto(source, destination) {
        if (!(this instanceof TextEncoder)) throw new TypeError("Illegal invocation");
        if (arguments.length < 2) throw new TypeError("Failed to execute 'encodeInto' on 'TextEncoder': 2 arguments required, but only " + arguments.length + " present.");
        if (Object.prototype.toString.call(destination) !== "[object Uint8Array]") throw new TypeError("Failed to execute 'encodeInto' on 'TextEncoder': parameter 2 is not of type 'Uint8Array'.");
        const result = native(source, destination);
        return { read: result[0], written: result[1] };
      } }).encodeInto;
      Object.defineProperty(TextEncoder.prototype, "encodeInto", { value: encodeInto, writable: true, enumerable: true, configurable: true });
    })();
  )JS");

}

}  // namespace

void InstallFetchApis(Quanta::Embed::Realm& realm) { Install(realm); }
void InstallFetchApis(Quanta::Embed::Runtime& runtime) { Install(runtime); }

}  // namespace solar::web
