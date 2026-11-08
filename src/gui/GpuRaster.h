/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "Scrollbar.h"
#include "solar/paint/DisplayList.h"

namespace solar::paint {
class SkiaPainter;
}

namespace solar::gui {

// Draws display lists on the GPU with no window: a context of the machine's GPU (OpenGL ES through EGL, with no surface), Skia on it, and the picture read back into memory the caller
// has. This is what the GPU's process does for the pages.
class GpuRaster {
 public:
  // Nothing where there is no GPU (or Skia cannot use it); `why` says.
  static std::unique_ptr<GpuRaster> Create(std::string* why = nullptr);
  ~GpuRaster();
  GpuRaster(const GpuRaster&) = delete;
  GpuRaster& operator=(const GpuRaster&) = delete;

  // The name of the GPU, as the driver gives it.
  const std::string& renderer() const { return renderer_; }

  // Draws the list as the viewport is scrolled by `scrollX`, `scrollY`, `pixelWidth` x `pixelHeight` pixels at `density` pixels to the point, with the scrollbar over it, into `out`
  // (rows of `stride` bytes, B G R A, premultiplied). `painter` is the page's (it keeps what it makes of its fonts and pictures). False where it could not be done.
  bool Draw(paint::SkiaPainter& painter, const paint::DisplayList& list, int pixelWidth, int pixelHeight, float density, double scrollX, double scrollY, const ScrollbarShape& bar, uint8_t* out,
            size_t stride);

  // The context for the thread that holds this (one at a time: it is made current for it, and let go of after): what is done with Skia's objects is done in one, and so is their end.
  class Scope {
   public:
    explicit Scope(GpuRaster& raster);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

   private:
    GpuRaster& raster_;
  };

  // What the GPU holds, in bytes.
  size_t ResourceBytes() const;
  // Lets go of what a page left behind (its pictures as textures).
  void Trim();

  struct Impl;

 private:
  GpuRaster();
  std::unique_ptr<Impl> impl_;
  std::string renderer_;
};

}  // namespace solar::gui
