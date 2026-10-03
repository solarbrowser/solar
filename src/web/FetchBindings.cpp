#include "solar/web/FetchBindings.h"

#include "solar/web/FetchBindingsInternal.h"

namespace solar::web {

namespace {

// Realm and Runtime both offer GetContext and Evaluate, which is all setup needs.
template <typename Host>
void Install(Host& host) {
  DefineHeadersClass(host.GetContext());
  // Web IDL makes @@iterator the same function object as entries, and the embedding surface has no
  // symbol-keyed definitions, so it is installed from script.
  host.Evaluate("Headers.prototype[Symbol.iterator] = Headers.prototype.entries;");
}

}  // namespace

void InstallFetchApis(Quanta::Embed::Realm& realm) { Install(realm); }
void InstallFetchApis(Quanta::Embed::Runtime& runtime) { Install(runtime); }

}  // namespace solar::web
