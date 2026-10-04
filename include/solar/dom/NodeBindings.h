#pragma once

#include "quanta/Embed.h"

namespace solar::dom {

// Defines Node, Element, Document, CharacterData and the rest of the document tree in a realm, with
// NodeList and HTMLCollection. A class is defined once per realm: call this after InstallDomApis
// (Node is an EventTarget) and before the scripts that use them.
void InstallNodeApis(Quanta::Embed::Realm& realm);
void InstallNodeApis(Quanta::Embed::Runtime& runtime);

}  // namespace solar::dom
