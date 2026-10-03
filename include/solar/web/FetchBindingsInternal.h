#pragma once

#include <memory>

#include "quanta/Embed.h"
#include "solar/net/FetchHeaders.h"

namespace solar::web {

// A Headers object: a header list with the guard it was made with.
struct JsHeaders : Quanta::DOMObject {
  net::FetchHeaders headers;
  // Held for the reason the URL objects hold theirs: what a Headers can outlive its realm with.
  Quanta::Object* iteratorPrototype = nullptr;

  void Visit(Quanta::Visitor& visitor) { visitor.Mark(iteratorPrototype); }
};

void DefineHeadersClass(Quanta::Context& ctx);

// A Headers in the realm the calling native belongs to, empty and with `guard`.
JsHeaders* AllocateHeaders(Quanta::Context& ctx, net::HeadersGuard guard);

}  // namespace solar::web
