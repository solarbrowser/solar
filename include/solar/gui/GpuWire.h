/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <cstdint>
#include <string>

#include "solar/net/Ipc.h"

// What a page's process (a renderer) and the GPU's process say to each other, and what the browser says to the GPU's process. The GPU's process draws the display lists the pages make (what the
// compositor of a renderer draws on the CPU otherwise) with the GPU and puts the pictures in memory the page's process made, which the window shows; it is not trusted by the browser to be
// more than that, and the pages are not trusted by it: what is read is checked.
namespace solar::gui::gpu {

// Renderer to GPU. The messages that carry a display list and what it refers to are those of solar/paint/ListCodec.h (1 to 6, with their bytes).
enum class ToGpu : uint8_t {
  Buffer = 16,  // memory the pictures are made in (sent with its descriptor): index, size
  Draw = 17,    // make the picture of the list that was last sent, in a buffer
};
// GPU to renderer.
enum class FromGpu : uint8_t { Drawn = 32, Failed = 33 };

// Browser to GPU, and back.
enum class Control : uint8_t {
  NewClient = 1,    // a page's connection (sent with its descriptor; the bytes are the page's token)
  NewGlClient = 4,  // a page's connection for WebGL (sent with its descriptor, and the same token: what is of the page is found by it)
  Ready = 2,      // (from the GPU) it can draw: the name of the GPU
  Unavailable = 3,  // (from the GPU) it cannot: why
};

struct Buffer {
  uint32_t index = 0;
  int32_t width = 0, height = 0, stride = 0;
};

struct Draw {
  uint32_t index = 0;
  uint64_t serial = 0;
  int32_t pixelWidth = 0, pixelHeight = 0;
  float density = 1;
  double scrollX = 0, scrollY = 0;
  bool barVisible = false, barEmphasized = false, barDragging = false;
  float barAlpha = 1, barTop = 0, barHeight = 0;
  int32_t windowWidth = 0, windowHeight = 0;
};

void Write(net::ipc::Writer& out, const Buffer& buffer);
bool Read(net::ipc::Reader& in, Buffer& buffer);
void Write(net::ipc::Writer& out, const Draw& draw);
bool Read(net::ipc::Reader& in, Draw& draw);

}  // namespace solar::gui::gpu
