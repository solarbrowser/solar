/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/gui/GpuClient.h"

#include <chrono>

#include "solar/gui/GpuWire.h"

namespace solar::gui {

GpuClient::GpuClient(int fd) : channel_(fd) {
  reader_ = std::thread([this] {
    uint8_t type = 0;
    std::string bytes;
    while (channel_.ReceiveRaw(type, bytes)) {
      net::ipc::Reader in(bytes);
      const uint32_t index = in.U32();
      const uint64_t serial = in.U64();
      if (!in.ok()) continue;
      (void)index;
      std::lock_guard<std::mutex> lock(mutex_);
      answered_ = serial;
      answeredOk_ = static_cast<gpu::FromGpu>(type) == gpu::FromGpu::Drawn;
      changed_.notify_all();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    broken_ = true;
    changed_.notify_all();
  });
}

GpuClient::~GpuClient() {
  channel_.Shutdown();
  reader_.join();
}

bool GpuClient::ok() const { return !broken_; }

bool GpuClient::Draw(const std::shared_ptr<const paint::DisplayList>& list, const SharedMemory& memory, uint32_t index, uint64_t generation, int width, int height, int stride, float density,
                     double scrollX, double scrollY, const ScrollbarShape& bar) {
  if (!list || broken_) return false;
  if (list != sent_) {
    for (const paint::EncodedMessage& message : encoder_.Encode(*list)) {
      if (!channel_.SendRaw(static_cast<uint8_t>(message.type), message.bytes)) return false;
    }
    sent_ = list;
  }
  for (const paint::EncodedMessage& message : encoder_.Dynamic()) {
    if (!channel_.SendRaw(static_cast<uint8_t>(message.type), message.bytes)) return false;
  }
  const auto known = announced_.find(index);
  if (known == announced_.end() || known->second != generation) {
    net::ipc::Writer out;
    gpu::Write(out, gpu::Buffer{index, width, height, stride});
    if (!channel_.SendRaw(static_cast<uint8_t>(gpu::ToGpu::Buffer), out.bytes(), memory.fd())) return false;
    announced_[index] = generation;
  }
  gpu::Draw draw;
  draw.index = index;
  draw.serial = ++serial_;
  draw.pixelWidth = width;
  draw.pixelHeight = height;
  draw.density = density;
  draw.scrollX = scrollX;
  draw.scrollY = scrollY;
  draw.barVisible = bar.visible;
  draw.barEmphasized = bar.emphasized;
  draw.barDragging = bar.dragging;
  draw.barAlpha = bar.alpha;
  draw.barTop = bar.top;
  draw.barHeight = bar.height;
  draw.windowWidth = bar.windowWidth;
  draw.windowHeight = bar.windowHeight;
  net::ipc::Writer out;
  gpu::Write(out, draw);
  if (!channel_.SendRaw(static_cast<uint8_t>(gpu::ToGpu::Draw), out.bytes())) return false;
  std::unique_lock<std::mutex> lock(mutex_);
  // (A picture is waited for for a few seconds: a GPU's process that does not answer is not waited for again.)
  const bool arrived = changed_.wait_for(lock, std::chrono::seconds(4), [&] { return broken_ || answered_ >= draw.serial; });
  return arrived && !broken_ && answered_ == draw.serial && answeredOk_;
}

}  // namespace solar::gui
