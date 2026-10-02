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

void InstallUrlApis(Quanta::Embed::Runtime& runtime) {
  DefineUrlSearchParamsClass(runtime);
  DefineUrlClass(runtime);
}

}  // namespace solar::web
