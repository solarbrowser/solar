/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "GpuRaster.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkImageGanesh.h"
#include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "include/gpu/ganesh/gl/GrGLTypes.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/gl/GrGLAssembleInterface.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLInterface.h"
#include "solar/paint/GlRuntime.h"
#include "solar/paint/SkiaPainter.h"

namespace solar::gui {

thread_local GpuRaster::Impl* g_drawing = nullptr;

namespace {

constexpr size_t kResourceBudget = static_cast<size_t>(128) << 20;

}  // namespace

struct GpuRaster::Impl {
  std::mutex mutex;
  paint::gl::RasterEgl egl;
  void* display() const { return egl.display; }
  sk_sp<GrDirectContext> direct;
  sk_sp<SkSurface> surface;
  int width = 0, height = 0;

  ~Impl() {
    surface.reset();
    if (direct) {
      direct->abandonContext();
      direct.reset();
    }
    paint::gl::DestroyRasterContext(egl);
  }
};

GpuRaster::GpuRaster() : impl_(std::make_unique<Impl>()) {}
GpuRaster::~GpuRaster() = default;

GpuRaster::Scope::Scope(GpuRaster& raster) : raster_(raster) {
  raster_.impl_->mutex.lock();
  const bool made = raster_.impl_->egl.makeCurrent(raster_.impl_->egl.display, raster_.impl_->egl.context);
  if (!made && std::getenv("SOLAR_GL_DEBUG")) { using EglGetError = int (*)(); auto err = reinterpret_cast<EglGetError>(raster_.impl_->egl.getProc("eglGetError")); std::fprintf(stderr, "gpu raster: the context could not be made current in this thread (EGL error 0x%x)\n", err ? err() : 0); }
}

GpuRaster::Scope::~Scope() {
  raster_.impl_->egl.makeCurrent(raster_.impl_->egl.display, nullptr);
  raster_.impl_->mutex.unlock();
}

std::unique_ptr<GpuRaster> GpuRaster::Create(std::string* why) {
  const auto fail = [&](const char* text) -> std::unique_ptr<GpuRaster> {
    if (why) *why = text;
    return nullptr;
  };
  auto raster = std::unique_ptr<GpuRaster>(new GpuRaster());
  Impl& impl = *raster->impl_;
  // The context is made on the EGL that WebGL's contexts are made on (ANGLE where there is one), and they are made to share textures with it.
  std::string reason;
  if (!paint::gl::CreateRasterContext(impl.egl, &reason)) {
    if (why) *why = reason;
    return nullptr;
  }
  // (No surface: the picture is drawn into a texture and read back.)
  if (!impl.egl.makeCurrent(impl.egl.display, impl.egl.context)) return fail("the context cannot be made current");
  const sk_sp<const GrGLInterface> interface = GrGLMakeAssembledInterface(&impl.egl, [](void* context, const char* name) -> GrGLFuncPtr {
    return reinterpret_cast<GrGLFuncPtr>(static_cast<paint::gl::RasterEgl*>(context)->getProc(name));
  });
  if (!interface) return fail("Skia cannot use the GL interface");
  GrContextOptions options;
  impl.direct = GrDirectContexts::MakeGL(interface, options);
  if (!impl.direct) return fail("Skia cannot make a GPU context");
  impl.direct->setResourceCacheLimit(kResourceBudget);
  using GetString = const unsigned char* (*)(unsigned);
  raster->renderer_ = "OpenGL ES";
  if (auto getString = reinterpret_cast<GetString>(impl.egl.getProc("glGetString"))) {
    if (const char* name = reinterpret_cast<const char*>(getString(0x1F01))) raster->renderer_ = name;  // GL_RENDERER
  }
  // (The context is let go of: whoever draws makes it current for itself.)
  impl.egl.makeCurrent(impl.egl.display, nullptr);
  return raster;
}

bool GpuRaster::Draw(paint::SkiaPainter& painter, const paint::DisplayList& list, int pixelWidth, int pixelHeight, float density, double scrollX, double scrollY, const ScrollbarShape& bar, uint8_t* out,
                     size_t stride) {
  Scope scope(*this);
  Impl& impl = *impl_;
  if (pixelWidth <= 0 || pixelHeight <= 0 || stride < static_cast<size_t>(pixelWidth) * 4) return false;
  if (!impl.surface || impl.width != pixelWidth || impl.height != pixelHeight) {
    impl.surface = SkSurfaces::RenderTarget(impl.direct.get(), skgpu::Budgeted::kNo, SkImageInfo::MakeN32Premul(pixelWidth, pixelHeight), 0, kTopLeft_GrSurfaceOrigin, nullptr);
    impl.width = pixelWidth;
    impl.height = pixelHeight;
    if (!impl.surface) {
      if (std::getenv("SOLAR_GL_DEBUG")) {
        using GetError = unsigned (*)();
        auto getError = reinterpret_cast<GetError>(impl.egl.getProc("glGetError"));
        std::fprintf(stderr, "gpu raster: no surface of %dx%d (GL error 0x%x, context lost %d)\n", pixelWidth, pixelHeight, getError ? getError() : 0, impl.direct->abandoned() ? 1 : 0);
      }
      return false;
    }
  }
  SkCanvas* canvas = impl.surface->getCanvas();
  canvas->resetMatrix();
  canvas->clear(SK_ColorWHITE);
  canvas->scale(density, density);
  const auto t0 = std::chrono::steady_clock::now();
  g_drawing = &impl;
  painter.Draw(list, *canvas, scrollX, scrollY);
  g_drawing = nullptr;
  DrawScrollbar(*canvas, bar);
  const auto t1 = std::chrono::steady_clock::now();
  // (SOLAR_GUI_TIMES: the drawing is waited for before the picture is read, to tell what the GPU took from what the reading did.)
  static const bool timing = std::getenv("SOLAR_GUI_TIMES") != nullptr;
  if (timing) impl.direct->flushAndSubmit(GrSyncCpu::kYes);
  const auto t2 = std::chrono::steady_clock::now();
  const bool ok = impl.surface->readPixels(SkImageInfo::MakeN32Premul(pixelWidth, pixelHeight), out, stride, 0, 0);
  const auto t3 = std::chrono::steady_clock::now();
  if (timing) {
    static int frames = 0;
    static double record = 0, gpu = 0, read = 0;
    const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    record += ms(t0, t1);
    gpu += ms(t1, t2);
    read += ms(t2, t3);
    if (++frames % 30 == 0) {
      std::fprintf(stderr, "gpu process: over 30 frames, per frame: record %.2f ms, GPU %.2f ms, read back %.2f ms\n", record / 30, gpu / 30, read / 30);
      record = gpu = read = 0;
    }
  }
  if (!ok && std::getenv("SOLAR_GL_DEBUG")) {
    using GetError = unsigned (*)();
    auto getError = reinterpret_cast<GetError>(impl.egl.getProc("glGetError"));
    std::fprintf(stderr, "gpu raster: the picture could not be read (GL error 0x%x, abandoned %d)\n", getError ? getError() : 0, impl.direct->abandoned() ? 1 : 0);
  }
  return ok;
}

void* GpuRaster::eglDisplay() const { return impl_->egl.display; }
void* GpuRaster::eglContext() const { return impl_->egl.context; }

sk_sp<SkImage> GpuRaster::TextureImage(uint32_t texture, int width, int height, bool premultiplied) {
  Impl* impl = g_drawing;
  if (!impl || !impl->direct || texture == 0 || width <= 0 || height <= 0) return nullptr;
  GrGLTextureInfo info;
  info.fTarget = 0x0DE1;  // GL_TEXTURE_2D
  info.fID = texture;
  info.fFormat = 0x8058;  // GL_RGBA8
  const GrBackendTexture backend = GrBackendTextures::MakeGL(width, height, skgpu::Mipmapped::kNo, info);
  return SkImages::BorrowTextureFrom(impl->direct.get(), backend, kBottomLeft_GrSurfaceOrigin, kRGBA_8888_SkColorType, premultiplied ? kPremul_SkAlphaType : kUnpremul_SkAlphaType, nullptr);
}

size_t GpuRaster::ResourceBytes() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  size_t bytes = 0;
  int count = 0;
  if (impl_->direct) impl_->direct->getResourceCacheUsage(&count, &bytes);
  return bytes;
}

void GpuRaster::Trim() {
  Scope scope(*this);
  if (impl_->direct) impl_->direct->freeGpuResources();
}

}  // namespace solar::gui
