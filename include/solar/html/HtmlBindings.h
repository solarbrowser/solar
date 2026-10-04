#pragma once

#include "quanta/Embed.h"

namespace solar::dom {
struct Document;
struct Node;
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
void InstallWindow(Quanta::Embed::Realm& realm, dom::Document* document = nullptr);

// What the frames of a window are made of: the natives its script reads them with, and the members of iframe elements.
void DefineFrameNatives(Quanta::Context& ctx);
void DefineIframeMembers(Quanta::Context& ctx, Quanta::Object* prototype);
void DefineScriptMembers(Quanta::Context& ctx, Quanta::Object* prototype);
// focus(), blur(), tabIndex and activeElement; and what the tree does when the focused node leaves it.
void DefineFocusMembers(Quanta::Context& ctx);
// document.open(), write(), writeln() and close().
void DefineDocumentWriting(Quanta::Context& ctx);
void FocusAfterRemove(dom::Node* node);

}  // namespace solar::html
