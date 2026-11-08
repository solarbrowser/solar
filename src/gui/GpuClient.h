/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#include "Scrollbar.h"
#include "solar/gui/FrameBuffer.h"
#include "solar/net/Ipc.h"
#include "solar/paint/ListCodec.h"

namespace solar::gui {

// The page's side of its connection to the GPU's process: the display list is sent when it is a new one (with what it refers to), and each picture is asked for with where the page is scrolled to;
// the picture is made in memory this process made, and the GPU's process says when it is there.
class GpuClient {
 public:
  explicit GpuClient(int fd);
  ~GpuClient();
  GpuClient(const GpuClient&) = delete;
  GpuClient& operator=(const GpuClient&) = delete;

  bool ok() const;
  // Draws into `memory` (the buffer `index` of the page's pictures, made of `generation`: a new memory is announced to the GPU's process). False where the GPU's process did not (it is
  // gone, or says it could not, or did not answer in time): the page draws by itself then.
  bool Draw(const std::shared_ptr<const paint::DisplayList>& list, const SharedMemory& memory, uint32_t index, uint64_t generation, int width, int height, int stride, float density, double scrollX,
            double scrollY, const ScrollbarShape& bar);

 private:
  net::ipc::Channel channel_;
  std::thread reader_;
  std::mutex mutex_;
  std::condition_variable changed_;
  bool broken_ = false;
  uint64_t answered_ = 0;
  bool answeredOk_ = false;
  uint64_t serial_ = 0;
  paint::ListEncoder encoder_;
  std::shared_ptr<const paint::DisplayList> sent_;
  std::map<uint32_t, uint64_t> announced_;  // (a buffer, and the memory it was announced for)
};

}  // namespace solar::gui
