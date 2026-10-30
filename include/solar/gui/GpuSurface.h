/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <memory>

struct SDL_Window;
class SkCanvas;
class SkBitmap;

namespace solar::gui {

// The window's drawing surface on the GPU: the framebuffer of an OpenGL context of the window, which Skia draws on directly, so that
// what a frame costs is the list being recorded and not every pixel of it being filled by the CPU and sent to the GPU after.
// Windows that are to have one are made with SDL_WINDOW_OPENGL, after PrepareWindowAttributes.
class GpuSurface {
 public:
  // What the context is to have (set before the window is made).
  static void PrepareWindowAttributes();
  // The surface of the window, or nothing where the window has no OpenGL or Skia cannot use what it gives.
  static std::unique_ptr<GpuSurface> Create(SDL_Window* window);
  ~GpuSurface();
  GpuSurface(const GpuSurface&) = delete;
  GpuSurface& operator=(const GpuSurface&) = delete;

  // The canvas for a frame at the size of the window in pixels (the framebuffer, wrapped again when the size changed).
  SkCanvas* BeginFrame(int pixelWidth, int pixelHeight);
  // What was drawn is handed to the GPU and shown (waiting for the display, when it is asked to).
  void Present();
  // The frame as it is so far, as pixels (before Present: what is shown is not kept).
  bool ReadPixels(SkBitmap& out);
  // The name of the GPU, as the driver gives it.
  const char* Renderer() const;
  // What the GPU holds of what was drawn (pictures, glyphs), in bytes.
  size_t ResourceBytes() const;

 private:
  GpuSurface();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace solar::gui
