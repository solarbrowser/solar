/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "quanta/Embed.h"

// The files of the images that script makes (new Image(), img.src = ...) are fetched here: a picture is not the page's to read (it may be from any site), so this is not fetch():
// it takes no CORS decision, and what comes is given to the script's own decoding (src/web-api/js/image.js).
namespace solar::webapi {

// Defines in a realm __solarImageFetch(url, callback): the callback is called once, with the status, the content type and the bytes (undefined if the fetch failed).
void InstallImageLoadNatives(Quanta::Embed::Realm& realm);

}  // namespace solar::webapi
