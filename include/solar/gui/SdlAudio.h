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

// A stream of SDL's audio, for whoever plays the sound of a page (the browser: the pages' processes are not let near the sound devices, they send the samples).
std::unique_ptr<media::AudioOutput> MakeSdlAudioOutput();

}  // namespace solar::gui
