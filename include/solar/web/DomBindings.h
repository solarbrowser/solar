/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "quanta/Embed.h"

namespace solar::web {

// Defines DOMException, Event, EventTarget, AbortController and AbortSignal in a realm. A class is
// defined once per realm, so call this for each realm that is to have them, before InstallFetchApis.
void InstallDomApis(Quanta::Embed::Realm& realm);
void InstallDomApis(Quanta::Embed::Runtime& runtime);

}  // namespace solar::web
