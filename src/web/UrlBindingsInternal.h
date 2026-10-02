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

  url::UrlSearchParams& Params();
  void Visit(Quanta::Visitor& visitor);
};

struct JsUrl : Quanta::DOMObject {
  std::unique_ptr<url::UrlObject> url;
  // Created on first access so that url.searchParams is the same object every time.
  JsUrlSearchParams* searchParams = nullptr;

  void Visit(Quanta::Visitor& visitor) { visitor.Mark(searchParams); }
};

inline url::UrlSearchParams& JsUrlSearchParams::Params() { return owner ? owner->url->SearchParams() : *own; }
inline void JsUrlSearchParams::Visit(Quanta::Visitor& visitor) { visitor.Mark(owner); }

void DefineUrlClass(Quanta::Embed::Runtime& runtime);
void DefineUrlSearchParamsClass(Quanta::Embed::Runtime& runtime);
Quanta::Object* UrlSearchParamsPrototype();

// Throws a TypeError when fewer than `count` arguments were passed, as Web IDL requires.
bool RequireArguments(Quanta::Context& ctx, Quanta::Embed::Args args, size_t count, const char* what);

}  // namespace solar::web
