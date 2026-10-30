/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "quanta/Embed.h"

// WebSocket is written in script (src/web-api/js/websocket.js) over the connections the network layer makes (src/net/WebSocket.cpp); these are the natives
// it holds them by.
namespace solar::webapi {

// Defines in a realm (a window's or a worker's, one that has a FetchHost) __solarSocketOpen, __solarSocketSend and __solarSocketClose.
void InstallWebSocketNatives(Quanta::Embed::Realm& realm);

}  // namespace solar::webapi
