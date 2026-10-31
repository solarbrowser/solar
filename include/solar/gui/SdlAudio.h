/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <memory>

#include "solar/media/Player.h"

namespace solar::gui {

// Makes the media elements play their sound through SDL's audio (the window program does this; the others play without sound).
void InstallSdlAudio();

}  // namespace solar::gui
