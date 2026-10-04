#pragma once

#include "quanta/Embed.h"

namespace solar::dom {
struct Document;
}

namespace solar::html {

// Gives a realm what the HTML parser and serializer add to the DOM: Element.innerHTML and outerHTML,
// insertAdjacentHTML and DOMParser. Call it after dom::InstallNodeApis.
void InstallHtmlApis(Quanta::Embed::Realm& realm);
void InstallHtmlApis(Quanta::Embed::Runtime& runtime);

// Defines HTMLElement's subinterfaces (HTMLDivElement and the rest) and tells the DOM which element gets which.
void DefineHtmlElementInterfaces(Quanta::Context& ctx);

// Makes the realm a page's: window and its names, document (the given one, or the document the realm has),
// and the window's own EventTarget methods and on<event> handlers. Call it once, after InstallHtmlApis.
void InstallWindow(Quanta::Embed::Runtime& runtime, dom::Document* document = nullptr);

}  // namespace solar::html
