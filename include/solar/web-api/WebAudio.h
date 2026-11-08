/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "quanta/Embed.h"

// Web Audio is written in script (src/web-api/js/webaudio.js) over the native engine (src/media/AudioGraph.cpp); the one native the script holds it by is __solarAudio(op, ...):
// a number that says what is done, then what it is done to (a context by its number, a node by its number) and the numbers of it.
namespace solar::webapi {

void InstallWebAudioNatives(Quanta::Embed::Realm& realm);

}  // namespace solar::webapi
