/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "solar/media/AudioGraph.h"

namespace solar::media::audio {

// Decodes a whole sound file (any format FFmpeg reads: ogg, mp3, wav, flac, aac ...) and resamples it to `rate`, as decodeAudioData does. False if it is not sound that can be decoded,
// or the machine has no FFmpeg; the channels (at most 8) are in `out`.
bool DecodeAudio(const uint8_t* bytes, size_t size, double rate, Buffer& out);

}  // namespace solar::media::audio
