/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <cstdint>
#include <string>

#include "solar/net/Ipc.h"

namespace solar::gui {

// What the page stores is asked of the browser (the page's process has no disk): this process's storage is the browser's, asked one file at a time and waited for.
void InstallRemoteStorage(net::ipc::Channel& link);
// What the browser answered (from any thread).
void DeliverStorageReply(const std::string& bytes);

}  // namespace solar::gui
