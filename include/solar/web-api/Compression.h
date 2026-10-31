/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "quanta/Embed.h"

// CompressionStream and DecompressionStream are written in script (src/web-api/js/compression.js) over TransformStream; zlib is reached by these natives.
namespace solar::webapi {

// Defines in a realm __solarZNew(format, compress) (a number, or -1), __solarZWrite(id, bytes) and __solarZFinish(id) (an ArrayBuffer, or undefined where the data
// was not valid), and __solarZFree(id).
void InstallCompressionNatives(Quanta::Embed::Realm& realm);

}  // namespace solar::webapi
