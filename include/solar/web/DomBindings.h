#pragma once

#include "quanta/Embed.h"

namespace solar::web {

// Defines DOMException, Event, EventTarget, AbortController and AbortSignal in a realm. A class is
// defined once per realm, so call this for each realm that is to have them, before InstallFetchApis.
void InstallDomApis(Quanta::Embed::Realm& realm);
void InstallDomApis(Quanta::Embed::Runtime& runtime);

}  // namespace solar::web
