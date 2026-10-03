#include "solar/web/UrlBindings.h"

#include "UrlBindingsInternal.h"

namespace solar::web {

bool RequireArguments(Quanta::Context& ctx, Quanta::Embed::Args args, size_t count, const char* what) {
  if (args.size() >= count) return true;
  Quanta::Embed::ThrowTypeError(ctx, std::string(what) + ": " + std::to_string(count) +
                                         (count == 1 ? " argument required, but only " : " arguments required, but only ") +
                                         std::to_string(args.size()) + " present.");
  return false;
}

namespace {

// Realm and Runtime both offer GetContext and Evaluate, which is all setup needs.
template <typename Host>
void Install(Host& host) {
  Quanta::Context& ctx = host.GetContext();
  DefineUrlSearchParamsClass(ctx);
  DefineUrlClass(ctx);

  // Web IDL makes @@iterator the same function object as entries, and the embedding surface
  // has no symbol-keyed definitions, so it is installed from script.
  host.Evaluate("URLSearchParams.prototype[Symbol.iterator] = URLSearchParams.prototype.entries;");
}

}  // namespace

void InstallUrlApis(Quanta::Embed::Realm& realm) { Install(realm); }
void InstallUrlApis(Quanta::Embed::Runtime& runtime) { Install(runtime); }

}  // namespace solar::web
