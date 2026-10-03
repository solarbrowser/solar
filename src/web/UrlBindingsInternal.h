#pragma once

#include <memory>
#include <optional>

#include "quanta/Embed.h"
#include "solar/url/UrlObject.h"
#include "solar/url/UrlSearchParams.h"

namespace solar::web {

struct JsUrl;

// Either a standalone list (`new URLSearchParams()`) or the query object of a URL, in which
// case it reads that URL's list and keeps the URL alive.
struct JsUrlSearchParams : Quanta::DOMObject {
  std::optional<url::UrlSearchParams> own;
  JsUrl* owner = nullptr;
  // Taken from the realm when the object is made. The realm frees what it made once nothing
  // holds it and does not trace what GetRealmData returns, so an object that can outlive its
  // realm holds the prototypes it will still need.
  Quanta::Object* iteratorPrototype = nullptr;

  url::UrlSearchParams& Params();
  void Visit(Quanta::Visitor& visitor);
};

struct JsUrl : Quanta::DOMObject {
  std::unique_ptr<url::UrlObject> url;
  // Created on first access so that url.searchParams is the same object every time.
  JsUrlSearchParams* searchParams = nullptr;
  // What the query object will get for prototypes, held for the same reason as above.
  Quanta::Object* queryPrototype = nullptr;
  Quanta::Object* queryIteratorPrototype = nullptr;

  void Visit(Quanta::Visitor& visitor) {
    visitor.Mark(searchParams);
    visitor.Mark(queryPrototype);
    visitor.Mark(queryIteratorPrototype);
  }
};

inline url::UrlSearchParams& JsUrlSearchParams::Params() { return owner ? owner->url->SearchParams() : *own; }
inline void JsUrlSearchParams::Visit(Quanta::Visitor& visitor) {
  visitor.Mark(owner);
  visitor.Mark(iteratorPrototype);
}

void DefineUrlClass(Quanta::Context& ctx);
void DefineUrlSearchParamsClass(Quanta::Context& ctx);

// URLSearchParams.prototype and the prototype of its iterators, in the realm the calling
// native belongs to.
Quanta::Object* UrlSearchParamsPrototype(Quanta::Context& ctx);
Quanta::Object* UrlSearchParamsIteratorPrototype(Quanta::Context& ctx);

JsUrlSearchParams* AllocateSearchParams(Quanta::Object* prototype, Quanta::Object* iteratorPrototype);

// Throws a TypeError when fewer than `count` arguments were passed, as Web IDL requires.
bool RequireArguments(Quanta::Context& ctx, Quanta::Embed::Args args, size_t count, const char* what);

}  // namespace solar::web
