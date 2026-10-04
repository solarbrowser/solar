#pragma once

#include "quanta/Embed.h"

namespace solar::html {

// Gives a realm what the HTML parser and serializer add to the DOM: Element.innerHTML and outerHTML,
// insertAdjacentHTML and DOMParser. Call it after dom::InstallNodeApis.
void InstallHtmlApis(Quanta::Embed::Realm& realm);
void InstallHtmlApis(Quanta::Embed::Runtime& runtime);

}  // namespace solar::html
