#include "solar/web/FetchHost.h"

#include <algorithm>

#include "solar/web/FetchBindingsInternal.h"
#include "solar/web/FetchOperation.h"

namespace solar::web {

namespace qe = Quanta::Embed;

namespace {
char g_hostKey;
}

FetchHost::FetchHost(Quanta::Context& ctx, Config config) : config_(std::move(config)), ctx_(&ctx) {
  qe::SetRealmData(ctx, &g_hostKey, this);
  Quanta::Value json = qe::Get(ctx, qe::FromObject(ctx.get_global_object()), "JSON");
  jsonParse_ = qe::Persistent(ctx, qe::Get(ctx, json, "parse"));
  jsonStringify_ = qe::Persistent(ctx, qe::Get(ctx, json, "stringify"));
}

FetchHost::~FetchHost() {
  *alive_ = false;
  qe::SetRealmData(*ctx_, &g_hostKey, nullptr);
  // Cancelling calls back into the operations, which must find the host as it was.
  std::vector<std::unique_ptr<FetchOperation>> operations = std::move(operations_);
  for (auto& operation : operations) CancelOperation(*operation);
}

FetchHost* HostOf(Quanta::Context& ctx) { return static_cast<FetchHost*>(qe::GetRealmData(ctx, &g_hostKey)); }

void FetchHost::Adopt(std::unique_ptr<FetchOperation> operation) { operations_.push_back(std::move(operation)); }

bool FetchHost::Owns(const FetchOperation* operation) const {
  return std::any_of(operations_.begin(), operations_.end(), [operation](const std::unique_ptr<FetchOperation>& o) { return o.get() == operation; });
}

void FetchHost::Finish(FetchOperation* operation) {
  // The operation is on the stack, in a call from the network; it goes when the loop next runs.
  config_.loop->PostDelayed(std::chrono::milliseconds(0), [this, operation, alive = alive_] {
    if (!*alive) return;
    std::erase_if(operations_, [operation](const std::unique_ptr<FetchOperation>& o) { return o.get() == operation; });
  });
}

}  // namespace solar::web
