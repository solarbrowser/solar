/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "quanta/Embed.h"

// The media elements are written in script (src/web-api/js/media.js); the playing is the program's (src/media: FFmpeg), and these are the natives the script holds a
// player by: __solarMediaCanPlay, __solarMediaOpen, __solarMediaControl, __solarMediaState, __solarMediaEvents and __solarMediaAttach.
namespace solar::webapi {

// Defines them in a window's realm (one that has a FetchHost: the resource is fetched with its client).
void InstallMediaNatives(Quanta::Embed::Realm& realm);

}  // namespace solar::webapi
