#pragma once

#include "quanta/Embed.h"

namespace solar::css {

// Gives the DOM what selectors add to it: querySelector and querySelectorAll on elements, documents and
// fragments, and matches, closest and webkitMatchesSelector on elements, and the CSS namespace's escape().
// Call it after dom::InstallNodeApis.
void InstallSelectorApis(Quanta::Embed::Realm& realm);
void InstallSelectorApis(Quanta::Embed::Runtime& runtime);

}  // namespace solar::css
