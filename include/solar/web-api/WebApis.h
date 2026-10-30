/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <string>

// The Web APIs that are more than a binding of what is below them: IndexedDB, and the rest that are written here as they are added. Each is a
// file in src/web-api (and its header here, where it has natives); the ones written in script are in src/web-api/js, compiled into the program.
namespace solar::webapi {

// The script that defines the APIs that are written in script, to be run in a window's realm after the DOM and the HTML APIs are there.
std::string Script();
// The script that makes the global scope of a worker (src/web-api/worker-js), run in the realm of each.
std::string WorkerScopeScript();

}  // namespace solar::webapi
