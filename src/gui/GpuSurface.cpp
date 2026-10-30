/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/gui/GpuSurface.h"

#include "GpuSurfaceInternal.h"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <string>

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/gl/GrGLAssembleInterface.h"
#include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLInterface.h"
#include "include/gpu/ganesh/gl/GrGLTypes.h"

namespace solar::gui {

namespace {
constexpr unsigned kRgba8 = 0x8058;  // GL_RGBA8
// What Skia may keep on the GPU of what was drawn (pictures made into textures, glyphs).
constexpr size_t kResourceBudget = static_cast<size_t>(64) << 20;
}  // namespace

namespace {

class GlSurface final : public GpuSurface {
 public:
  ~GlSurface() override {
    surface_.reset();
    if (direct_) {
      direct_->abandonContext();  // (the context goes with the window: nothing is to be asked of it after)
      direct_.reset();
    }
    if (context_) SDL_GL_DestroyContext(context_);
  }

  bool Init(SDL_Window* window) {
    window_ = window;
    context_ = SDL_GL_CreateContext(window);
    if (!context_) return false;
    if (!SDL_GL_MakeCurrent(window, context_)) return false;
    SDL_GL_SetSwapInterval(1);
    const sk_sp<const GrGLInterface> interface = GrGLMakeAssembledInterface(nullptr, [](void*, const char* name) -> GrGLFuncPtr {
      return reinterpret_cast<GrGLFuncPtr>(SDL_GL_GetProcAddress(name));
    });
    if (!interface) return false;
    GrContextOptions options;
    direct_ = GrDirectContexts::MakeGL(interface, options);
    if (!direct_) return false;
    direct_->setResourceCacheLimit(kResourceBudget);
    renderer_ = "OpenGL";
    using GetString = const unsigned char* (*)(unsigned);
    if (auto getString = reinterpret_cast<GetString>(SDL_GL_GetProcAddress("glGetString"))) {
      if (const char* name = reinterpret_cast<const char*>(getString(0x1F01))) renderer_ = name;  // GL_RENDERER
    }
    return true;
  }

  SkCanvas* BeginFrame(int pixelWidth, int pixelHeight) override {
    if (pixelWidth <= 0 || pixelHeight <= 0) return nullptr;
    if (!surface_ || width_ != pixelWidth || height_ != pixelHeight) {
      GrGLFramebufferInfo framebuffer;
      framebuffer.fFBOID = 0;  // the window's own
      framebuffer.fFormat = kRgba8;
      int stencil = 8;
      SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &stencil);
      const GrBackendRenderTarget target = GrBackendRenderTargets::MakeGL(pixelWidth, pixelHeight, 0, stencil, framebuffer);
      surface_ = SkSurfaces::WrapBackendRenderTarget(direct_.get(), target, kBottomLeft_GrSurfaceOrigin, kRGBA_8888_SkColorType, nullptr, nullptr);
      width_ = pixelWidth;
      height_ = pixelHeight;
      if (!surface_) return nullptr;
    }
    return surface_->getCanvas();
  }

  void Present() override {
    if (!surface_) return;
    skgpu::ganesh::FlushAndSubmit(surface_.get());
    // (SOLAR_GUI_TIMES: the frame is waited for, to be able to tell what it cost the GPU, which is not told by how long it took to hand it over.)
    static const bool wait = std::getenv("SOLAR_GUI_TIMES") != nullptr;
    if (wait) direct_->flushAndSubmit(GrSyncCpu::kYes);
    SDL_GL_SwapWindow(window_);
  }

  bool ReadPixels(SkBitmap& out) override {
    if (!surface_) return false;
    if (!out.tryAllocPixels(SkImageInfo::MakeN32Premul(width_, height_))) return false;
    return surface_->readPixels(out, 0, 0);
  }

  const char* Renderer() const override { return renderer_.c_str(); }
  const char* Interface() const override { return "OpenGL"; }

  size_t ResourceBytes() const override {
    size_t bytes = 0;
    int count = 0;
    if (direct_) direct_->getResourceCacheUsage(&count, &bytes);
    return bytes;
  }

 private:
  SDL_Window* window_ = nullptr;
  SDL_GLContext context_ = nullptr;
  sk_sp<GrDirectContext> direct_;
  sk_sp<SkSurface> surface_;
  int width_ = 0, height_ = 0;
  std::string renderer_;
};

}  // namespace

std::unique_ptr<GpuSurface> MakeOpenGlSurface(SDL_Window* window) {
  if (!window) return nullptr;
  auto gpu = std::make_unique<GlSurface>();
  if (!gpu->Init(window)) return nullptr;
  return gpu;
}

std::vector<GpuBackend> GpuSurface::Backends() {
  if (const char* named = std::getenv("SOLAR_GUI_GPU")) {
    const std::string name = named;
    if (name == "vulkan") return {GpuBackend::Vulkan};
    if (name == "gl" || name == "opengl") return {GpuBackend::OpenGL};
    return {};
  }
  return {GpuBackend::Vulkan, GpuBackend::OpenGL};
}

unsigned long long GpuSurface::WindowFlags(GpuBackend backend) {
  return backend == GpuBackend::Vulkan ? SDL_WINDOW_VULKAN : SDL_WINDOW_OPENGL;
}

void GpuSurface::Prepare(GpuBackend backend) {
  if (backend != GpuBackend::OpenGL) return;
  // Core 3.3: what Skia needs is there on every desktop that has OpenGL at all.
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
  SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
  SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
}

std::unique_ptr<GpuSurface> GpuSurface::Create(GpuBackend backend, SDL_Window* window) {
  return backend == GpuBackend::Vulkan ? MakeVulkanSurface(window) : MakeOpenGlSurface(window);
}

}  // namespace solar::gui
