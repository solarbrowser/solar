#pragma once

#include "quanta/Embed.h"

namespace solar::web {

// Defines the URL and URLSearchParams globals in a realm. A class is defined once per realm,
// so call this for each realm that is to have them.
void InstallUrlApis(Quanta::Embed::Realm& realm);
void InstallUrlApis(Quanta::Embed::Runtime& runtime);

}  // namespace solar::web
