#pragma once

#include "quanta/Embed.h"

namespace solar::web {

// Defines the Fetch API's globals in a realm: Headers, and as they are added the rest. A class is
// defined once per realm, so call this for each realm that is to have them.
void InstallFetchApis(Quanta::Embed::Realm& realm);
void InstallFetchApis(Quanta::Embed::Runtime& runtime);

}  // namespace solar::web
