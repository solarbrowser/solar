/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/gui/GpuSurface.h"

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

struct GpuSurface::Impl {
  SDL_Window* window = nullptr;
  SDL_GLContext context = nullptr;
  sk_sp<GrDirectContext> direct;
  sk_sp<SkSurface> surface;
  int width = 0, height = 0;
  std::string renderer;
};

GpuSurface::GpuSurface() : impl_(std::make_unique<Impl>()) {}

GpuSurface::~GpuSurface() {
  if (!impl_) return;
  impl_->surface.reset();
  if (impl_->direct) {
    impl_->direct->abandonContext();  // (the context goes with the window: nothing is to be asked of it after)
    impl_->direct.reset();
  }
  if (impl_->context) SDL_GL_DestroyContext(impl_->context);
}

void GpuSurface::PrepareWindowAttributes() {
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

std::unique_ptr<GpuSurface> GpuSurface::Create(SDL_Window* window) {
  if (!window) return nullptr;
  std::unique_ptr<GpuSurface> gpu(new GpuSurface());
  Impl& impl = *gpu->impl_;
  impl.window = window;
  impl.context = SDL_GL_CreateContext(window);
  if (!impl.context) return nullptr;
  if (!SDL_GL_MakeCurrent(window, impl.context)) return nullptr;
  SDL_GL_SetSwapInterval(1);
  const sk_sp<const GrGLInterface> interface = GrGLMakeAssembledInterface(nullptr, [](void*, const char* name) -> GrGLFuncPtr {
    return reinterpret_cast<GrGLFuncPtr>(SDL_GL_GetProcAddress(name));
  });
  if (!interface) return nullptr;
  GrContextOptions options;
  impl.direct = GrDirectContexts::MakeGL(interface, options);
  if (!impl.direct) return nullptr;
  impl.direct->setResourceCacheLimit(kResourceBudget);
  impl.renderer = "OpenGL";
  using GetString = const unsigned char* (*)(unsigned);
  if (auto getString = reinterpret_cast<GetString>(SDL_GL_GetProcAddress("glGetString"))) {
    if (const char* name = reinterpret_cast<const char*>(getString(0x1F01))) impl.renderer = name;  // GL_RENDERER
  }
  return gpu;
}

SkCanvas* GpuSurface::BeginFrame(int pixelWidth, int pixelHeight) {
  Impl& impl = *impl_;
  if (pixelWidth <= 0 || pixelHeight <= 0) return nullptr;
  if (!impl.surface || impl.width != pixelWidth || impl.height != pixelHeight) {
    GrGLFramebufferInfo framebuffer;
    framebuffer.fFBOID = 0;  // the window's own
    framebuffer.fFormat = kRgba8;
    int stencil = 8;
    SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &stencil);
    const GrBackendRenderTarget target = GrBackendRenderTargets::MakeGL(pixelWidth, pixelHeight, 0, stencil, framebuffer);
    impl.surface = SkSurfaces::WrapBackendRenderTarget(impl.direct.get(), target, kBottomLeft_GrSurfaceOrigin, kRGBA_8888_SkColorType, nullptr, nullptr);
    impl.width = pixelWidth;
    impl.height = pixelHeight;
    if (!impl.surface) return nullptr;
  }
  return impl.surface->getCanvas();
}

void GpuSurface::Present() {
  Impl& impl = *impl_;
  if (!impl.surface) return;
  skgpu::ganesh::FlushAndSubmit(impl.surface.get());
  // (SOLAR_GUI_TIMES: the frame is waited for, to be able to tell what it cost the GPU, which is not told by how long it took to hand it over.)
  static const bool wait = std::getenv("SOLAR_GUI_TIMES") != nullptr;
  if (wait) impl.direct->flushAndSubmit(GrSyncCpu::kYes);
  SDL_GL_SwapWindow(impl.window);
}

bool GpuSurface::ReadPixels(SkBitmap& out) {
  Impl& impl = *impl_;
  if (!impl.surface) return false;
  if (!out.tryAllocPixels(SkImageInfo::MakeN32Premul(impl.width, impl.height))) return false;
  return impl.surface->readPixels(out, 0, 0);
}

const char* GpuSurface::Renderer() const { return impl_->renderer.c_str(); }

size_t GpuSurface::ResourceBytes() const {
  size_t bytes = 0;
  int count = 0;
  if (impl_->direct) impl_->direct->getResourceCacheUsage(&count, &bytes);
  return bytes;
}

}  // namespace solar::gui
