/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "quanta/Embed.h"
#include "solar/net/Ipc.h"

namespace solar::gui {

// The one native of the browser's interface page (and of no other page): __solarUiSend(text), which asks the browser for something (a command: its verb, and what it is for after a tab).
// The browser tells the page its state by calling globalThis.__solarUiState(state).
void InstallUiNatives(Quanta::Embed::Realm& realm, net::ipc::Channel& link);

}  // namespace solar::gui
