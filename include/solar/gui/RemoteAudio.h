/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <cstdint>

#include "solar/net/Ipc.h"

namespace solar::gui {

// The sound of the page's media is played by the window: the page's process is not let near the sound devices. A stream here sends its samples to the window; the window says now and then
// how much it has of them not yet played, and the page goes by that (and by the time since) for how much it has queued.
void InstallRemoteAudio(net::ipc::Channel& link);
// What the window said of a stream (from any thread).
void DeliverAudioQueued(uint32_t id, uint64_t frames);

}  // namespace solar::gui
