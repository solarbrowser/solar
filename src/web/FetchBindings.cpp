#include "solar/web/FetchBindings.h"

#include "solar/web/FetchBindingsInternal.h"

namespace solar::web {

namespace {

// Realm and Runtime both offer GetContext and Evaluate, which is all setup needs.
template <typename Host>
void Install(Host& host) {
  DefineHeadersClass(host.GetContext());
  DefineResponseClass(host.GetContext());
  DefineRequestClass(host.GetContext());
  DefineFetchFunction(host.GetContext());
  // Web IDL makes @@iterator the same function object as entries, and the embedding surface has no
  // symbol-keyed definitions, so it is installed from script.
  host.Evaluate("Headers.prototype[Symbol.iterator] = Headers.prototype.entries;");

  // fetch was defined as a class, the one thing the embedding surface can bind as a global. It becomes
  // the plain function it should be: no prototype, not constructible, a length of 1.
  host.Evaluate(R"JS(
    (function () {
      const native = fetch;
      const call = Function.prototype.call;
      const fetchFunction = ({ fetch(input, init) { return call.call(native, undefined, ...arguments); } }).fetch;
      Object.defineProperty(globalThis, "fetch", { value: fetchFunction, writable: true, enumerable: true, configurable: true });
    })();
  )JS");
}

}  // namespace

void InstallFetchApis(Quanta::Embed::Realm& realm) { Install(realm); }
void InstallFetchApis(Quanta::Embed::Runtime& runtime) { Install(runtime); }

}  // namespace solar::web
