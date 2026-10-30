/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <string>

#include "quanta/Embed.h"

// IndexedDB is written in script (src/web-api/js/indexeddb.js) and keeps its databases in memory; these are what it keeps them on disk with, so that
// what a page stored is there the next time it is opened: one file for each database of an origin, written whole when it changed.
namespace solar::webapi {

// Defines in a window's realm the natives the script keeps its files with (__solarIdbRead, __solarIdbWrite, __solarIdbDelete, __solarIdbList).
void InstallIndexedDbNatives(Quanta::Embed::Realm& realm);

// Where the files are (SOLAR_IDB_DIR names it; otherwise under the user's data directory), for tests and tools.
std::string IndexedDbDirectory();

}  // namespace solar::webapi
