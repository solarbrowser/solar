/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <memory>

#include "FfmpegApi.h"
#include "solar/media/Mse.h"

namespace solar::media {

// What one init segment says about its tracks (a packet is decoded with the parameters of the init segment it came after).
struct MseCodec {
  AVCodecParameters* vpar = nullptr;
  AVCodecParameters* apar = nullptr;
  AVRational vtb{1, 1000}, atb{1, 1000};
  double sar = 1;
  ~MseCodec();
};

// A packet as kept: its times are in seconds, with the buffer's timestamp offset already in them (and in the packet's own, which the decoder gives the pictures).
struct MsePacket {
  std::shared_ptr<AVPacket> packet;
  double dts = 0, pts = 0, duration = 0;
  bool key = false;
  std::shared_ptr<MseCodec> codec;
  size_t bytes = 0;
};

}  // namespace solar::media
