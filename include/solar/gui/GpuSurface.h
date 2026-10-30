/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <memory>
#include <vector>

struct SDL_Window;
class SkCanvas;
class SkBitmap;

namespace solar::gui {

// Which of the platform's graphics interfaces draws the window.
enum class GpuBackend { Vulkan, OpenGL };

// The window's drawing surface on the GPU, which Skia draws on directly, so that what a frame costs is the list being recorded and not
// every pixel of it being filled by the CPU and sent to the GPU after. Windows that are to have one are made with the flags the backend
// asks for (WindowFlags, after Prepare).
class GpuSurface {
 public:
  // The backends to try, best first: what the platform has (SOLAR_GUI_GPU=vulkan or gl names one alone; cpu none at all).
  static std::vector<GpuBackend> Backends();
  // What a window made for the backend is to have (SDL_WindowFlags), and the attributes it is to be made with.
  static unsigned long long WindowFlags(GpuBackend backend);
  static void Prepare(GpuBackend backend);
  // The surface of the window, or nothing where the window has none for the backend or Skia cannot use what it gives.
  static std::unique_ptr<GpuSurface> Create(GpuBackend backend, SDL_Window* window);

  virtual ~GpuSurface() = default;
  GpuSurface(const GpuSurface&) = delete;
  GpuSurface& operator=(const GpuSurface&) = delete;

  // The canvas for a frame at the size of the window in pixels (the framebuffer, wrapped again when the size changed).
  virtual SkCanvas* BeginFrame(int pixelWidth, int pixelHeight) = 0;
  // What was drawn is handed to the GPU and shown (waiting for the display, when it is asked to).
  virtual void Present() = 0;
  // The frame as it is so far, as pixels (before Present: what is shown is not kept).
  virtual bool ReadPixels(SkBitmap& out) = 0;
  // The name of the GPU, as the driver gives it, and of the interface that is used.
  virtual const char* Renderer() const = 0;
  virtual const char* Interface() const = 0;
  // What the GPU holds of what was drawn (pictures, glyphs), in bytes.
  virtual size_t ResourceBytes() const = 0;

 protected:
  GpuSurface() = default;
};

}  // namespace solar::gui
