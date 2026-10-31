/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <string>

#include "quanta/Embed.h"

// The Cache API (src/web-api/js/cache.js) keeps what it caches here, in the program and not in a realm, so that a page and a service worker of one origin see the same
// caches; each cache is also a file, written when the script asks (__solarCacheFlush), and read again the next time the origin is used. The same file of a few kinds
// of keys is what a service worker's registrations are kept in (__solarKvRead, __solarKvWrite).
namespace solar::webapi {

// Defines in a realm __solarCacheNames, __solarCacheOpen, __solarCacheHas, __solarCacheDelete, __solarCacheMetas, __solarCacheBody, __solarCachePut, __solarCacheRemove,
// __solarCacheFlush, __solarKvRead and __solarKvWrite.
void InstallCacheStoreNatives(Quanta::Embed::Realm& realm);

// Whether the origin has a service worker registered whose scope begins the address (the program does not start fetching what the page will ask for before the page's
// worker is there to be asked).
bool HasServiceWorkerFor(const std::string& origin, const std::string& url);

}  // namespace solar::webapi
