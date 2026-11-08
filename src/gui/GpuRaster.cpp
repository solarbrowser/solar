/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "GpuRaster.h"

#include <dlfcn.h>

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

// ---- EGL, by name (the libraries are opened when the context is made: a machine with none does without) ----

using EGLDisplayT = void*;
using EGLContextT = void*;
using EGLConfigT = void*;
using EGLSurfaceT = void*;
using EGLDeviceT = void*;
constexpr int kEglNone = 0x3038;
constexpr int kEglExtensions = 0x3055;
constexpr int kEglRenderableType = 0x3040, kEglOpenGlEs3Bit = 0x0040, kEglSurfaceType = 0x3033, kEglRedSize = 0x3024, kEglGreenSize = 0x3023, kEglBlueSize = 0x3022, kEglAlphaSize = 0x3021;
constexpr int kEglContextClientVersion = 0x3098;
constexpr int kEglOpenGlEsApi = 0x30A0;
constexpr int kEglPlatformDevice = 0x313F;

struct Egl {
  void* library = nullptr;
  void* (*getProc)(const char*) = nullptr;
  unsigned (*initialize)(EGLDisplayT, int*, int*) = nullptr;
  unsigned (*bindApi)(unsigned) = nullptr;
  unsigned (*chooseConfig)(EGLDisplayT, const int*, EGLConfigT*, int, int*) = nullptr;
  EGLContextT (*createContext)(EGLDisplayT, EGLConfigT, EGLContextT, const int*) = nullptr;
  unsigned (*makeCurrent)(EGLDisplayT, EGLSurfaceT, EGLSurfaceT, EGLContextT) = nullptr;
  unsigned (*destroyContext)(EGLDisplayT, EGLContextT) = nullptr;
  const char* (*queryString)(EGLDisplayT, int) = nullptr;
  EGLDisplayT (*getPlatformDisplay)(unsigned, void*, const int*) = nullptr;
  unsigned (*queryDevices)(int, EGLDeviceT*, int*) = nullptr;
  EGLDisplayT (*getDisplay)(void*) = nullptr;
};

bool LoadEgl(Egl& egl) {
  for (const char* name : {"libEGL.so.1", "libEGL.so"}) {
    if ((egl.library = dlopen(name, RTLD_NOW | RTLD_LOCAL))) break;
  }
  if (!egl.library) return false;
  const auto sym = [&](const char* name) { return dlsym(egl.library, name); };
  egl.getProc = reinterpret_cast<void* (*)(const char*)>(sym("eglGetProcAddress"));
  egl.initialize = reinterpret_cast<decltype(egl.initialize)>(sym("eglInitialize"));
  egl.bindApi = reinterpret_cast<decltype(egl.bindApi)>(sym("eglBindAPI"));
  egl.chooseConfig = reinterpret_cast<decltype(egl.chooseConfig)>(sym("eglChooseConfig"));
  egl.createContext = reinterpret_cast<decltype(egl.createContext)>(sym("eglCreateContext"));
  egl.makeCurrent = reinterpret_cast<decltype(egl.makeCurrent)>(sym("eglMakeCurrent"));
  egl.destroyContext = reinterpret_cast<decltype(egl.destroyContext)>(sym("eglDestroyContext"));
  egl.queryString = reinterpret_cast<decltype(egl.queryString)>(sym("eglQueryString"));
  egl.getDisplay = reinterpret_cast<decltype(egl.getDisplay)>(sym("eglGetDisplay"));
  // (By name in the library first, as WebGL does: the extension's functions are exported by it.)
  egl.getPlatformDisplay = reinterpret_cast<decltype(egl.getPlatformDisplay)>(sym("eglGetPlatformDisplayEXT") ? sym("eglGetPlatformDisplayEXT") : (egl.getProc ? egl.getProc("eglGetPlatformDisplayEXT") : nullptr));
  egl.queryDevices = reinterpret_cast<decltype(egl.queryDevices)>(sym("eglQueryDevicesEXT") ? sym("eglQueryDevicesEXT") : (egl.getProc ? egl.getProc("eglQueryDevicesEXT") : nullptr));
  return egl.getProc && egl.initialize && egl.bindApi && egl.chooseConfig && egl.createContext && egl.makeCurrent && egl.queryString && egl.getDisplay;
}

constexpr size_t kResourceBudget = static_cast<size_t>(128) << 20;

}  // namespace

struct GpuRaster::Impl {
  std::mutex mutex;
  Egl egl;
  EGLDisplayT display = nullptr;
  EGLContextT context = nullptr;
  sk_sp<GrDirectContext> direct;
  sk_sp<SkSurface> surface;
  int width = 0, height = 0;

  ~Impl() {
    surface.reset();
    if (direct) {
      direct->abandonContext();
      direct.reset();
    }
    if (context && egl.destroyContext) egl.destroyContext(display, context);
    if (egl.library) dlclose(egl.library);
  }

  bool TryDisplay(EGLDisplayT candidate) {
    int major = 0, minor = 0;
    if (!candidate || !egl.initialize(candidate, &major, &minor)) return false;
    egl.bindApi(kEglOpenGlEsApi);
    const int attributes[] = {kEglRenderableType, kEglOpenGlEs3Bit, kEglSurfaceType, 0x0001 /* EGL_PBUFFER_BIT: the device that GPU has is found by it (as WebGL finds it), though no surface is made */, kEglRedSize, 8, kEglGreenSize, 8, kEglBlueSize, 8, kEglAlphaSize, 8, kEglNone};
    EGLConfigT config = nullptr;
    int count = 0;
    if (!egl.chooseConfig(candidate, attributes, &config, 1, &count) || count < 1) return false;
    const int contextAttributes[] = {kEglContextClientVersion, 3, kEglNone};
    EGLContextT made = egl.createContext(candidate, config, nullptr, contextAttributes);
    if (!made) return false;
    // (No surface: the picture is drawn into a texture and read back.)
    if (!egl.makeCurrent(candidate, nullptr, nullptr, made)) {
      egl.destroyContext(candidate, made);
      return false;
    }
    display = candidate;
    context = made;
    return true;
  }
};

GpuRaster::GpuRaster() : impl_(std::make_unique<Impl>()) {}

GpuRaster::Scope::Scope(GpuRaster& raster) : raster_(raster) {
  raster_.impl_->mutex.lock();
  raster_.impl_->egl.makeCurrent(raster_.impl_->display, nullptr, nullptr, raster_.impl_->context);
}

GpuRaster::Scope::~Scope() {
  raster_.impl_->egl.makeCurrent(raster_.impl_->display, nullptr, nullptr, nullptr);
  raster_.impl_->mutex.unlock();
}
GpuRaster::~GpuRaster() = default;

std::unique_ptr<GpuRaster> GpuRaster::Create(std::string* why) {
  const auto fail = [&](const char* text) -> std::unique_ptr<GpuRaster> {
    if (why) *why = text;
    return nullptr;
  };
  auto raster = std::unique_ptr<GpuRaster>(new GpuRaster());
  Impl& impl = *raster->impl_;
  setenv("EGL_LOG_LEVEL", "fatal", 0);
  if (!LoadEgl(impl.egl)) return fail("no EGL");
  // A device of the machine that EGL can use with no window (a GPU), else the default display.
  const std::string extensions = impl.egl.queryString(nullptr, kEglExtensions) ? impl.egl.queryString(nullptr, kEglExtensions) : "";
  bool ok = false;
  if (impl.egl.queryDevices && impl.egl.getPlatformDisplay && extensions.find("EGL_EXT_device_enumeration") != std::string::npos) {
    EGLDeviceT devices[8];
    int count = 0;
    if (impl.egl.queryDevices(8, devices, &count)) {
      for (int i = 0; i < count && !ok; ++i) ok = impl.TryDisplay(impl.egl.getPlatformDisplay(kEglPlatformDevice, devices[i], nullptr));
    }
  }
  if (std::getenv("SOLAR_GL_DEBUG")) std::fprintf(stderr, "gpu raster: device enumeration %s, ok %d, extensions have it %d, query %p, getPlatform %p\n", impl.egl.queryDevices ? "there" : "missing", ok, extensions.find("EGL_EXT_device_enumeration") != std::string::npos, reinterpret_cast<void*>(impl.egl.queryDevices), reinterpret_cast<void*>(impl.egl.getPlatformDisplay));
  if (!ok) ok = impl.TryDisplay(impl.egl.getDisplay(nullptr));
  if (!ok) return fail("no EGL display with an OpenGL ES 3 context");
  const sk_sp<const GrGLInterface> interface = GrGLMakeAssembledInterface(&impl.egl, [](void* context, const char* name) -> GrGLFuncPtr {
    return reinterpret_cast<GrGLFuncPtr>(static_cast<Egl*>(context)->getProc(name));
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
  impl.egl.makeCurrent(impl.display, nullptr, nullptr, nullptr);
  // WebGL's contexts are made to share their textures with it.
  paint::gl::SetShare(impl.display, impl.context);
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
    if (!impl.surface) return false;
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
  return ok;
}

void* GpuRaster::eglDisplay() const { return impl_->display; }
void* GpuRaster::eglContext() const { return impl_->context; }

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
