/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// WebGL (https://registry.khronos.org/webgl/specs/latest/): the context is an OpenGL ES 3 context of the machine's (or ANGLE's), made through EGL with no window, that draws into
// a framebuffer of its own (the drawing buffer). What the page has drawn is read from it a moment after (one task of the loop for all the drawing there was meanwhile) into the
// canvas's surface, which the window shows as it shows a 2D canvas. The API the script sees is src/web-api/js/webgl.js; this is the natives under it: __solarGlCreate, __solarGlOps,
// __solarGl and the rest. Most of the functions of the standard are the OpenGL ES function of the same name, called with the numbers the script gives (`Scalar`); the ones that
// take memory, strings or arrays have a handler each.
#include <dlfcn.h>
#include <unistd.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include "CanvasNatives.h"
#include "GlApi.h"
#include "solar/paint/Canvas.h"

// ANGLE's own tokens of eglext_angle.h (the header is not in the machine's).
#ifndef EGL_PLATFORM_ANGLE_ANGLE
#define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#define EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE 0x320D
#define EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE 0x3208
#define EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE 0x3450
#define EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE 0x3489
#define EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE 0x3209
#define EGL_PLATFORM_ANGLE_DEVICE_TYPE_SWIFTSHADER_ANGLE 0x3487
#endif

namespace solar::paint {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::Value;

// ---- The libraries ----

namespace {

void* OpenLibrary(const char* const* names) {
  // The ones that are shipped with the program (ANGLE's, in a directory beside it or beside it) come first; then the machine's.
  char self[4096];
  std::string directory;
  const ssize_t length = readlink("/proc/self/exe", self, sizeof self - 1);
  if (length > 0) {
    directory.assign(self, static_cast<size_t>(length));
    directory.resize(directory.rfind('/') + 1);
  }
  if (!directory.empty()) {
    for (const char* sub : {"angle/", ""}) {
      for (const char* const* name = names; *name; ++name) {
        if (void* handle = dlopen((directory + sub + *name).c_str(), RTLD_NOW | RTLD_LOCAL)) return handle;
      }
    }
  }
  for (const char* const* name = names; *name; ++name) {
    if (void* handle = dlopen(*name, RTLD_NOW | RTLD_LOCAL)) return handle;
  }
  return nullptr;
}

struct Egl {
  bool tried = false, ok = false, angle = false;
  void* eglLibrary = nullptr;
  void* glLibrary = nullptr;
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLConfig config = nullptr;
  PFNEGLGETPROCADDRESSPROC getProc = nullptr;
  GlApi api;
  std::string description;
};

Egl& TheEgl() {
  static Egl egl;
  return egl;
}

template <typename F>
F Proc(Egl& egl, const char* name) {
  if (egl.eglLibrary) {
    if (void* p = dlsym(egl.eglLibrary, name)) return reinterpret_cast<F>(p);
  }
  return egl.getProc ? reinterpret_cast<F>(egl.getProc(name)) : nullptr;
}

bool InitDisplay(Egl& egl, EGLDisplay display) {
  EGLint major = 0, minor = 0;
  if (display == EGL_NO_DISPLAY || !Proc<EGLBoolean (*)(EGLDisplay, EGLint*, EGLint*)>(egl, "eglInitialize")(display, &major, &minor)) return false;
  Proc<EGLBoolean (*)(EGLenum)>(egl, "eglBindAPI")(EGL_OPENGL_ES_API);
  const EGLint attributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
  EGLConfig config = nullptr;
  EGLint count = 0;
  if (!Proc<EGLBoolean (*)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*)>(egl, "eglChooseConfig")(display, attributes, &config, 1, &count) || count < 1) return false;
  egl.display = display;
  egl.config = config;
  return true;
}

// Loads the libraries and finds a display, once. The display is ANGLE's (the backend asked for in SOLAR_ANGLE_BACKEND, or the platform's) when the library is ANGLE; else a device of
// the machine that EGL can use with no window (a GPU), else the default display.
bool EnsureEgl() {
  static std::mutex lock;
  std::lock_guard<std::mutex> guard(lock);
  Egl& egl = TheEgl();
  if (egl.tried) return egl.ok;
  egl.tried = true;
  setenv("EGL_LOG_LEVEL", "fatal", 0);
#if defined(__APPLE__)
  static const char* const eglNames[] = {"libEGL.dylib", nullptr};
  static const char* const glNames[] = {"libGLESv2.dylib", nullptr};
#elif defined(_WIN32)
  static const char* const eglNames[] = {"libEGL.dll", nullptr};
  static const char* const glNames[] = {"libGLESv2.dll", nullptr};
#else
  static const char* const eglNames[] = {"libEGL.so.1", "libEGL.so", nullptr};
  static const char* const glNames[] = {"libGLESv2.so.2", "libGLESv2.so", nullptr};
#endif
  egl.eglLibrary = OpenLibrary(eglNames);
  egl.glLibrary = OpenLibrary(glNames);
  if (!egl.eglLibrary || !egl.glLibrary) return false;
  egl.getProc = reinterpret_cast<PFNEGLGETPROCADDRESSPROC>(dlsym(egl.eglLibrary, "eglGetProcAddress"));
  const char* clientExtensions = Proc<const char* (*)(EGLDisplay, EGLint)>(egl, "eglQueryString")(EGL_NO_DISPLAY, EGL_EXTENSIONS);
  const std::string extensions = clientExtensions ? clientExtensions : "";
  egl.angle = extensions.find("EGL_ANGLE_platform_angle") != std::string::npos;
  auto getPlatformDisplay = Proc<PFNEGLGETPLATFORMDISPLAYEXTPROC>(egl, "eglGetPlatformDisplayEXT");
  if (egl.angle && getPlatformDisplay) {
    EGLint type = EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE;  // (Linux: Vulkan is there with SOLAR_ANGLE_BACKEND=vulkan)
#if defined(_WIN32)
    type = EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE;
#elif defined(__APPLE__)
    type = EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE;
#endif
    if (const char* wanted = std::getenv("SOLAR_ANGLE_BACKEND")) {
      const std::string w = wanted;
      if (w == "vulkan") type = EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE;
      else if (w == "gl") type = EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE;
      else if (w == "d3d11") type = EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE;
      else if (w == "metal") type = EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE;
    }
    EGLint attributes[] = {EGL_PLATFORM_ANGLE_TYPE_ANGLE, type, EGL_NONE};
    if (std::getenv("SOLAR_ANGLE_SWIFTSHADER")) {
      EGLint swift[] = {EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE, EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_DEVICE_TYPE_SWIFTSHADER_ANGLE, EGL_NONE};
      if (InitDisplay(egl, getPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, swift))) egl.ok = true;
    } else if (InitDisplay(egl, getPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, attributes))) {
      egl.ok = true;
    }
  }
  if (!egl.ok && extensions.find("EGL_EXT_device_enumeration") != std::string::npos && getPlatformDisplay) {
    auto queryDevices = Proc<PFNEGLQUERYDEVICESEXTPROC>(egl, "eglQueryDevicesEXT");
    EGLDeviceEXT devices[8];
    EGLint count = 0;
    if (queryDevices && queryDevices(8, devices, &count)) {
      for (EGLint i = 0; i < count && !egl.ok; ++i) egl.ok = InitDisplay(egl, getPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, devices[i], nullptr));
    }
  }
  if (!egl.ok) egl.ok = InitDisplay(egl, Proc<EGLDisplay (*)(EGLNativeDisplayType)>(egl, "eglGetDisplay")(EGL_DEFAULT_DISPLAY));
  if (!egl.ok) return false;
#define X(name)                                                                                    \
  {                                                                                                \
    void* symbol = dlsym(egl.glLibrary, "gl" #name);                                              \
    if (!symbol && egl.getProc) symbol = reinterpret_cast<void*>(egl.getProc("gl" #name));        \
    egl.api.name = reinterpret_cast<decltype(egl.api.name)>(symbol);                              \
  }
  SOLAR_GL_FUNCTIONS(X)
#undef X
  return true;
}

// ---- A context ----

struct GlContext {
  Egl* egl = nullptr;
  GlApi* gl = nullptr;
  EGLContext context = EGL_NO_CONTEXT;
  int version = 1;  // what the page asked for (1: WebGL 1, 2: WebGL 2; the context is ES 3 either way)
  int width = 0, height = 0;
  bool antialias = true, alpha = true, depth = true, stencil = false, premultiplied = true, preserve = false;
  int samples = 0;
  GLuint defaultFbo = 0, resolveFbo = 0, msaaColor = 0, resolveColor = 0, depthStencil = 0;
  GLuint drawFbo = 0, readFbo = 0;  // what is bound (0 is the default, which is defaultFbo)
  uint32_t surfaceId = 0;
  bool dirtySinceRead = true;  // something was drawn since the surface was filled
  std::map<uint32_t, GLsync> syncs;
  uint32_t nextSync = 1;
  std::string renderer, vendor, glVersion;
  std::vector<std::string> extensions;
};

thread_local EGLContext g_current = EGL_NO_CONTEXT;  // (a worker has its contexts on its own thread)

void MakeCurrent(GlContext& g) {
  if (g_current == g.context) return;
  Egl& egl = *g.egl;
  Proc<EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext)>(egl, "eglMakeCurrent")(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, g.context);
  g_current = g.context;
}

// The drawing buffer: what the page draws into, as large as the canvas. With antialiasing it is multisampled and resolved into the one that is read.
void BuildBuffers(GlContext& g) {
  GlApi& gl = *g.gl;
  if (g.defaultFbo) {
    gl.DeleteFramebuffers(1, &g.defaultFbo);
    if (g.resolveFbo) gl.DeleteFramebuffers(1, &g.resolveFbo);
    GLuint renderbuffers[3] = {g.msaaColor, g.resolveColor, g.depthStencil};
    gl.DeleteRenderbuffers(3, renderbuffers);
  }
  g.defaultFbo = g.resolveFbo = g.msaaColor = g.resolveColor = g.depthStencil = 0;
  GLint maxSamples = 0;
  gl.GetIntegerv(GL_MAX_SAMPLES, &maxSamples);
  g.samples = g.antialias ? std::min<GLint>(4, maxSamples) : 0;
  const GLenum colorFormat = g.alpha ? GL_RGBA8 : GL_RGB8;
  const int w = std::max(1, g.width), h = std::max(1, g.height);
  GLuint renderbuffers[3];
  gl.GenRenderbuffers(3, renderbuffers);
  g.msaaColor = renderbuffers[0];
  g.resolveColor = renderbuffers[1];
  g.depthStencil = renderbuffers[2];
  gl.GenFramebuffers(1, &g.defaultFbo);
  gl.BindRenderbuffer(GL_RENDERBUFFER, g.msaaColor);
  if (g.samples > 0) gl.RenderbufferStorageMultisample(GL_RENDERBUFFER, g.samples, colorFormat, w, h);
  else gl.RenderbufferStorage(GL_RENDERBUFFER, colorFormat, w, h);
  gl.BindFramebuffer(GL_FRAMEBUFFER, g.defaultFbo);
  gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g.msaaColor);
  if (g.depth || g.stencil) {
    const GLenum format = g.depth && g.stencil ? GL_DEPTH24_STENCIL8 : g.depth ? GL_DEPTH_COMPONENT24 : GL_STENCIL_INDEX8;
    const GLenum attachment = g.depth && g.stencil ? GL_DEPTH_STENCIL_ATTACHMENT : g.depth ? GL_DEPTH_ATTACHMENT : GL_STENCIL_ATTACHMENT;
    gl.BindRenderbuffer(GL_RENDERBUFFER, g.depthStencil);
    if (g.samples > 0) gl.RenderbufferStorageMultisample(GL_RENDERBUFFER, g.samples, format, w, h);
    else gl.RenderbufferStorage(GL_RENDERBUFFER, format, w, h);
    gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, attachment, GL_RENDERBUFFER, g.depthStencil);
  }
  if (g.samples > 0) {
    gl.GenFramebuffers(1, &g.resolveFbo);
    gl.BindRenderbuffer(GL_RENDERBUFFER, g.resolveColor);
    gl.RenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
    gl.BindFramebuffer(GL_FRAMEBUFFER, g.resolveFbo);
    gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g.resolveColor);
  }
  gl.BindRenderbuffer(GL_RENDERBUFFER, 0);
  // (What was bound is the drawing buffer again.)
  gl.BindFramebuffer(GL_FRAMEBUFFER, g.defaultFbo);
  g.drawFbo = g.readFbo = 0;
  gl.ClearColor(0, 0, 0, 0);
  gl.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

// The picture of the drawing buffer, read for the surface: resolved if it is multisampled, with its first row at the bottom as OpenGL has it.
std::vector<uint8_t> ReadDrawingBuffer(GlContext& g) {
  GlApi& gl = *g.gl;
  const int w = std::max(1, g.width), h = std::max(1, g.height);
  GLint previousRead = 0, previousDraw = 0;
  gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);
  gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDraw);
  GLuint source = g.defaultFbo;
  if (g.samples > 0) {
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, g.defaultFbo);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, g.resolveFbo);
    gl.Disable(GL_SCISSOR_TEST);
    gl.BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    source = g.resolveFbo;
  }
  gl.BindFramebuffer(GL_READ_FRAMEBUFFER, source);
  GLint packAlignment = 4;
  gl.GetIntegerv(GL_PACK_ALIGNMENT, &packAlignment);
  gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
  gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
  gl.ReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
  gl.PixelStorei(GL_PACK_ALIGNMENT, packAlignment);
  gl.BindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousRead));
  gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDraw));
  return pixels;
}

// What was shown is cleared (unless the page asked to keep it), as if by clear with the defaults and no mask, with the state of the page kept as it was.
void ClearAfterPresent(GlContext& g) {
  GlApi& gl = *g.gl;
  GLboolean colorMask[4];
  GLboolean depthMask;
  GLint stencilMask, drawBinding;
  GLfloat color[4], depthClear;
  GLint stencilClear;
  const GLboolean scissor = gl.IsEnabled(GL_SCISSOR_TEST);
  gl.GetBooleanv(GL_COLOR_WRITEMASK, colorMask);
  gl.GetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
  gl.GetIntegerv(GL_STENCIL_WRITEMASK, &stencilMask);
  gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawBinding);
  gl.GetFloatv(GL_COLOR_CLEAR_VALUE, color);
  gl.GetFloatv(GL_DEPTH_CLEAR_VALUE, &depthClear);
  gl.GetIntegerv(GL_STENCIL_CLEAR_VALUE, &stencilClear);
  gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, g.defaultFbo);
  gl.Disable(GL_SCISSOR_TEST);
  gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  gl.DepthMask(GL_TRUE);
  gl.StencilMask(0xFFFFFFFFu);
  gl.ClearColor(0, 0, 0, 0);
  gl.ClearDepthf(1);
  gl.ClearStencil(0);
  gl.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
  gl.ClearColor(color[0], color[1], color[2], color[3]);
  gl.ClearDepthf(depthClear);
  gl.ClearStencil(stencilClear);
  gl.ColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
  gl.DepthMask(depthMask);
  gl.StencilMask(static_cast<GLuint>(stencilMask));
  if (scissor) gl.Enable(GL_SCISSOR_TEST);
  gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawBinding));
}

// ---- What the natives hold ----

struct WebGlState {
  std::map<uint32_t, std::unique_ptr<GlContext>> contexts;
  uint32_t next = 1;
};

WebGlState& StateFor(Natives& n) {
  thread_local std::map<Natives*, std::unique_ptr<WebGlState>> states;
  auto& slot = states[&n];
  if (!slot) {
    slot = std::make_unique<WebGlState>();
    Natives* key = &n;
    n.onRelease.push_back([key] {
      // (The contexts of the realm go with it.)
      auto it = states.find(key);
      if (it == states.end()) return;
      for (auto& [id, context] : it->second->contexts) {
        MakeCurrent(*context);
        Egl& egl = *context->egl;
        Proc<EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext)>(egl, "eglMakeCurrent")(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        g_current = EGL_NO_CONTEXT;
        Proc<EGLBoolean (*)(EGLDisplay, EGLContext)>(egl, "eglDestroyContext")(egl.display, context->context);
      }
      states.erase(it);
    });
  }
  return *slot;
}

double Num(Context& ctx, qe::Args a, size_t i) { return i < a.size() ? qe::ToNumber(ctx, a[i]) : 0; }
uint32_t Uint(Context& ctx, qe::Args a, size_t i) { return static_cast<uint32_t>(static_cast<int64_t>(Num(ctx, a, i))); }
int Int(Context& ctx, qe::Args a, size_t i) { return static_cast<int>(static_cast<int64_t>(Num(ctx, a, i))); }
const void* Offset(Context& ctx, qe::Args a, size_t i) { return reinterpret_cast<const void*>(static_cast<uintptr_t>(Num(ctx, a, i))); }

template <typename T>
T Convert(double v) {
  static_assert(!std::is_pointer_v<T>, "a function with a pointer is not a scalar one");
  if constexpr (std::is_floating_point_v<T>) return static_cast<T>(v);
  else return static_cast<T>(static_cast<int64_t>(v));
}

Value Wrap(Context& ctx, GLuint v) { return qe::FromNumber(static_cast<double>(v)); }
Value Wrap(Context&, GLint v) { return qe::FromNumber(v); }
Value Wrap(Context&, GLboolean v) { return qe::FromNumber(v ? 1 : 0); }

using Handler = std::function<Value(GlContext&, Context&, qe::Args)>;

template <typename R, typename... A, size_t... I>
Value Invoke(GlContext& g, Context& ctx, qe::Args a, R(GL_APIENTRY* GlApi::*member)(A...), std::index_sequence<I...>) {
  auto fn = g.gl->*member;
  if (!fn) return qe::Undefined();
  if constexpr (std::is_void_v<R>) {
    fn(Convert<A>(Num(ctx, a, I))...);
    return qe::Undefined();
  } else {
    return Wrap(ctx, fn(Convert<A>(Num(ctx, a, I))...));
  }
}

// A function of the standard that is the OpenGL ES one with the numbers it is given.
template <typename R, typename... A>
Handler Scalar(R(GL_APIENTRY* GlApi::*member)(A...)) {
  return [member](GlContext& g, Context& ctx, qe::Args a) { return Invoke(g, ctx, a, member, std::index_sequence_for<A...>{}); };
}

std::string LowerFirst(std::string name) {
  if (!name.empty()) name[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[0])));
  return name;
}

Value StringValue(Context& ctx, const std::string& s) { return qe::FromWtf8(ctx, s); }

Value NumberArray(Context& ctx, const double* values, size_t n) {
  Value list = qe::NewArray(ctx);
  for (size_t i = 0; i < n; ++i) qe::ArrayPush(ctx, list, qe::FromNumber(values[i]));
  return list;
}

std::optional<std::span<const uint8_t>> InBytes(qe::Args a, size_t i) { return i < a.size() ? qe::BytesOf(a[i]) : std::nullopt; }

// A view of the script's, for a function that writes into it.
std::optional<std::span<uint8_t>> OutBytes(qe::Args a, size_t i) { return i < a.size() ? qe::MutableBytesOf(a[i]) : std::nullopt; }

// The framebuffer a script means by none is the drawing buffer.
GLuint Framebuffer(GlContext& g, GLuint name) { return name == 0 ? g.defaultFbo : name; }

}  // namespace

// The handlers, by the name the script calls them with.
const std::vector<std::pair<std::string, Handler>>& Operations() {
  static const std::vector<std::pair<std::string, Handler>> operations = [] {
    std::vector<std::pair<std::string, Handler>> list;
    auto add = [&](const std::string& name, Handler handler) { list.emplace_back(name, std::move(handler)); };
#define S(Name) add(LowerFirst(#Name), Scalar(&GlApi::Name))
    S(ActiveTexture); S(AttachShader); S(BeginQuery); S(BeginTransformFeedback); S(BindBuffer); S(BindBufferBase); S(BindBufferRange); S(BindRenderbuffer); S(BindSampler);
    S(BindTexture); S(BindTransformFeedback); S(BindVertexArray); S(BlendColor); S(BlendEquation); S(BlendEquationSeparate); S(BlendFunc); S(BlendFuncSeparate);
    S(BlitFramebuffer); S(CheckFramebufferStatus); S(Clear); S(ClearBufferfi); S(ClearColor); S(ClearDepthf); S(ClearStencil); S(ColorMask); S(CompileShader);
    S(CopyBufferSubData); S(CopyTexImage2D); S(CopyTexSubImage2D); S(CopyTexSubImage3D); S(CreateProgram); S(CreateShader); S(CullFace); S(DeleteProgram); S(DeleteShader);
    S(DepthFunc); S(DepthMask); S(DepthRangef); S(DetachShader); S(Disable); S(DisableVertexAttribArray); S(DrawArrays); S(DrawArraysInstanced); S(Enable);
    S(EnableVertexAttribArray); S(EndQuery); S(EndTransformFeedback); S(Finish); S(Flush); S(FramebufferRenderbuffer); S(FramebufferTexture2D); S(FramebufferTextureLayer);
    S(FrontFace); S(GenerateMipmap); S(GetError); S(Hint); S(IsBuffer); S(IsEnabled); S(IsFramebuffer); S(IsProgram); S(IsQuery); S(IsRenderbuffer); S(IsSampler); S(IsShader);
    S(IsTexture); S(IsTransformFeedback); S(IsVertexArray); S(LineWidth); S(LinkProgram); S(PauseTransformFeedback); S(PixelStorei); S(PolygonOffset); S(RenderbufferStorage);
    S(RenderbufferStorageMultisample); S(ResumeTransformFeedback); S(SampleCoverage); S(SamplerParameterf); S(SamplerParameteri); S(Scissor); S(StencilFunc);
    S(StencilFuncSeparate); S(StencilMask); S(StencilMaskSeparate); S(StencilOp); S(StencilOpSeparate); S(TexParameterf); S(TexParameteri); S(TexStorage2D); S(TexStorage3D);
    S(Uniform1f); S(Uniform2f); S(Uniform3f); S(Uniform4f); S(Uniform1i); S(Uniform2i); S(Uniform3i); S(Uniform4i); S(Uniform1ui); S(Uniform2ui); S(Uniform3ui); S(Uniform4ui);
    S(UniformBlockBinding); S(UseProgram); S(ValidateProgram); S(VertexAttrib1f); S(VertexAttrib2f); S(VertexAttrib3f); S(VertexAttrib4f); S(VertexAttribDivisor);
    S(VertexAttribI4i); S(VertexAttribI4ui); S(Viewport); S(UnmapBuffer);
#undef S

    // ---- objects ----
    auto generate = [&](const char* name, void(GL_APIENTRY* GlApi::*gen)(GLsizei, GLuint*)) {
      add(name, [gen](GlContext& g, Context&, qe::Args) {
        GLuint id = 0;
        if (g.gl->*gen) (g.gl->*gen)(1, &id);
        return qe::FromNumber(id);
      });
    };
    auto destroy = [&](const char* name, void(GL_APIENTRY* GlApi::*del)(GLsizei, const GLuint*)) {
      add(name, [del](GlContext& g, Context& ctx, qe::Args a) {
        const GLuint id = Uint(ctx, a, 0);
        if (g.gl->*del) (g.gl->*del)(1, &id);
        return qe::Undefined();
      });
    };
    generate("createBuffer", &GlApi::GenBuffers); generate("createTexture", &GlApi::GenTextures); generate("createFramebuffer", &GlApi::GenFramebuffers);
    generate("createRenderbuffer", &GlApi::GenRenderbuffers); generate("createQuery", &GlApi::GenQueries); generate("createSampler", &GlApi::GenSamplers);
    generate("createTransformFeedback", &GlApi::GenTransformFeedbacks); generate("createVertexArray", &GlApi::GenVertexArrays);
    destroy("deleteBuffer", &GlApi::DeleteBuffers); destroy("deleteTexture", &GlApi::DeleteTextures); destroy("deleteFramebuffer", &GlApi::DeleteFramebuffers);
    destroy("deleteRenderbuffer", &GlApi::DeleteRenderbuffers); destroy("deleteQuery", &GlApi::DeleteQueries); destroy("deleteSampler", &GlApi::DeleteSamplers);
    destroy("deleteTransformFeedback", &GlApi::DeleteTransformFeedbacks); destroy("deleteVertexArray", &GlApi::DeleteVertexArrays);

    // ---- the drawing buffer is what "none" is for a framebuffer ----
    add("bindFramebuffer", [](GlContext& g, Context& ctx, qe::Args a) {
      const GLenum target = Uint(ctx, a, 0);
      const GLuint name = Uint(ctx, a, 1);
      g.gl->BindFramebuffer(target, Framebuffer(g, name));
      if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) g.drawFbo = name;
      if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) g.readFbo = name;
      return qe::Undefined();
    });
    add("readBuffer", [](GlContext& g, Context& ctx, qe::Args a) {
      GLenum mode = Uint(ctx, a, 0);
      if (g.readFbo == 0 && mode == GL_BACK) mode = GL_COLOR_ATTACHMENT0;
      g.gl->ReadBuffer(mode);
      return qe::Undefined();
    });
    add("drawBuffers", [](GlContext& g, Context& ctx, qe::Args a) {
      auto bytes = InBytes(a, 0);
      if (!bytes) return qe::Undefined();
      std::vector<GLenum> modes(bytes->size() / 4);
      std::memcpy(modes.data(), bytes->data(), modes.size() * 4);
      if (g.drawFbo == 0) for (GLenum& m : modes) if (m == GL_BACK) m = GL_COLOR_ATTACHMENT0;
      g.gl->DrawBuffers(static_cast<GLsizei>(modes.size()), modes.data());
      return qe::Undefined();
    });
    // The numbers the script cannot know: the name that "none" stands for.
    add("defaultFramebuffer", [](GlContext& g, Context&, qe::Args) { return qe::FromNumber(g.defaultFbo); });

    // ---- buffers ----
    add("bufferData", [](GlContext& g, Context& ctx, qe::Args a) {
      const GLenum target = Uint(ctx, a, 0), usage = Uint(ctx, a, 2);
      if (a.size() > 1 && qe::IsObject(a[1])) {
        auto bytes = InBytes(a, 1);
        if (bytes) g.gl->BufferData(target, static_cast<GLsizeiptr>(bytes->size()), bytes->data(), usage);
      } else {
        g.gl->BufferData(target, static_cast<GLsizeiptr>(Num(ctx, a, 1)), nullptr, usage);
      }
      return qe::Undefined();
    });
    add("bufferSubData", [](GlContext& g, Context& ctx, qe::Args a) {
      if (auto bytes = InBytes(a, 2)) g.gl->BufferSubData(Uint(ctx, a, 0), static_cast<GLintptr>(Num(ctx, a, 1)), static_cast<GLsizeiptr>(bytes->size()), bytes->data());
      return qe::Undefined();
    });
    add("getBufferSubData", [](GlContext& g, Context& ctx, qe::Args a) {
      auto out = OutBytes(a, 2);
      if (!out || !g.gl->MapBufferRange) return qe::Undefined();
      void* mapped = g.gl->MapBufferRange(Uint(ctx, a, 0), static_cast<GLintptr>(Num(ctx, a, 1)), static_cast<GLsizeiptr>(out->size()), GL_MAP_READ_BIT);
      if (mapped) {
        std::memcpy(out->data(), mapped, out->size());
        g.gl->UnmapBuffer(Uint(ctx, a, 0));
      }
      return qe::Undefined();
    });
    add("vertexAttribPointer", [](GlContext& g, Context& ctx, qe::Args a) {
      g.gl->VertexAttribPointer(Uint(ctx, a, 0), Int(ctx, a, 1), Uint(ctx, a, 2), Num(ctx, a, 3) != 0, Int(ctx, a, 4), Offset(ctx, a, 5));
      return qe::Undefined();
    });
    add("vertexAttribIPointer", [](GlContext& g, Context& ctx, qe::Args a) {
      g.gl->VertexAttribIPointer(Uint(ctx, a, 0), Int(ctx, a, 1), Uint(ctx, a, 2), Int(ctx, a, 3), Offset(ctx, a, 4));
      return qe::Undefined();
    });
    add("drawElements", [](GlContext& g, Context& ctx, qe::Args a) {
      g.gl->DrawElements(Uint(ctx, a, 0), Int(ctx, a, 1), Uint(ctx, a, 2), Offset(ctx, a, 3));
      return qe::Undefined();
    });
    add("drawElementsInstanced", [](GlContext& g, Context& ctx, qe::Args a) {
      g.gl->DrawElementsInstanced(Uint(ctx, a, 0), Int(ctx, a, 1), Uint(ctx, a, 2), Offset(ctx, a, 3), Int(ctx, a, 4));
      return qe::Undefined();
    });
    add("drawRangeElements", [](GlContext& g, Context& ctx, qe::Args a) {
      g.gl->DrawRangeElements(Uint(ctx, a, 0), Uint(ctx, a, 1), Uint(ctx, a, 2), Int(ctx, a, 3), Uint(ctx, a, 4), Offset(ctx, a, 5));
      return qe::Undefined();
    });

    // ---- textures: the bytes, or (when a buffer is bound as the unpack buffer) an offset into it ----
    auto pixels = [](qe::Args a, size_t i, Context& ctx) -> const void* {
      if (i >= a.size() || a[i].is_null() || a[i].is_undefined()) return nullptr;
      if (qe::IsObject(a[i])) {
        auto bytes = qe::BytesOf(a[i]);
        return bytes ? bytes->data() : nullptr;
      }
      return Offset(ctx, a, i);
    };
    add("texImage2D", [pixels](GlContext& g, Context& ctx, qe::Args a) {
      g.gl->TexImage2D(Uint(ctx, a, 0), Int(ctx, a, 1), Int(ctx, a, 2), Int(ctx, a, 3), Int(ctx, a, 4), Int(ctx, a, 5), Uint(ctx, a, 6), Uint(ctx, a, 7), pixels(a, 8, ctx));
      return qe::Undefined();
    });
    add("texSubImage2D", [pixels](GlContext& g, Context& ctx, qe::Args a) {
      g.gl->TexSubImage2D(Uint(ctx, a, 0), Int(ctx, a, 1), Int(ctx, a, 2), Int(ctx, a, 3), Int(ctx, a, 4), Int(ctx, a, 5), Uint(ctx, a, 6), Uint(ctx, a, 7), pixels(a, 8, ctx));
      return qe::Undefined();
    });
    add("texImage3D", [pixels](GlContext& g, Context& ctx, qe::Args a) {
      g.gl->TexImage3D(Uint(ctx, a, 0), Int(ctx, a, 1), Int(ctx, a, 2), Int(ctx, a, 3), Int(ctx, a, 4), Int(ctx, a, 5), Int(ctx, a, 6), Uint(ctx, a, 7), Uint(ctx, a, 8), pixels(a, 9, ctx));
      return qe::Undefined();
    });
    add("texSubImage3D", [pixels](GlContext& g, Context& ctx, qe::Args a) {
      g.gl->TexSubImage3D(Uint(ctx, a, 0), Int(ctx, a, 1), Int(ctx, a, 2), Int(ctx, a, 3), Int(ctx, a, 4), Int(ctx, a, 5), Int(ctx, a, 6), Int(ctx, a, 7), Uint(ctx, a, 8), Uint(ctx, a, 9), pixels(a, 10, ctx));
      return qe::Undefined();
    });
    add("compressedTexImage2D", [](GlContext& g, Context& ctx, qe::Args a) {
      auto bytes = InBytes(a, 6);
      g.gl->CompressedTexImage2D(Uint(ctx, a, 0), Int(ctx, a, 1), Uint(ctx, a, 2), Int(ctx, a, 3), Int(ctx, a, 4), Int(ctx, a, 5), bytes ? static_cast<GLsizei>(bytes->size()) : 0, bytes ? bytes->data() : nullptr);
      return qe::Undefined();
    });
    add("compressedTexSubImage2D", [](GlContext& g, Context& ctx, qe::Args a) {
      auto bytes = InBytes(a, 7);
      g.gl->CompressedTexSubImage2D(Uint(ctx, a, 0), Int(ctx, a, 1), Int(ctx, a, 2), Int(ctx, a, 3), Int(ctx, a, 4), Int(ctx, a, 5), Uint(ctx, a, 6), bytes ? static_cast<GLsizei>(bytes->size()) : 0, bytes ? bytes->data() : nullptr);
      return qe::Undefined();
    });
    add("compressedTexImage3D", [](GlContext& g, Context& ctx, qe::Args a) {
      auto bytes = InBytes(a, 7);
      g.gl->CompressedTexImage3D(Uint(ctx, a, 0), Int(ctx, a, 1), Uint(ctx, a, 2), Int(ctx, a, 3), Int(ctx, a, 4), Int(ctx, a, 5), Int(ctx, a, 6), bytes ? static_cast<GLsizei>(bytes->size()) : 0, bytes ? bytes->data() : nullptr);
      return qe::Undefined();
    });
    add("compressedTexSubImage3D", [](GlContext& g, Context& ctx, qe::Args a) {
      auto bytes = InBytes(a, 9);
      g.gl->CompressedTexSubImage3D(Uint(ctx, a, 0), Int(ctx, a, 1), Int(ctx, a, 2), Int(ctx, a, 3), Int(ctx, a, 4), Int(ctx, a, 5), Int(ctx, a, 6), Int(ctx, a, 7), Uint(ctx, a, 8), bytes ? static_cast<GLsizei>(bytes->size()) : 0, bytes ? bytes->data() : nullptr);
      return qe::Undefined();
    });
    add("readPixels", [](GlContext& g, Context& ctx, qe::Args a) {
      // From the drawing buffer, a multisampled one is resolved first.
      GLuint resolved = 0;
      GLint previousRead = 0;
      g.gl->GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);
      if (g.readFbo == 0 && g.samples > 0) {
        GLint previousDraw = 0;
        g.gl->GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDraw);
        const int w = std::max(1, g.width), h = std::max(1, g.height);
        g.gl->BindFramebuffer(GL_READ_FRAMEBUFFER, g.defaultFbo);
        g.gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER, g.resolveFbo);
        g.gl->Disable(GL_SCISSOR_TEST);
        g.gl->BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        g.gl->BindFramebuffer(GL_READ_FRAMEBUFFER, g.resolveFbo);
        g.gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDraw));
        resolved = g.resolveFbo;
      }
      void* destination = nullptr;
      if (a.size() > 6 && qe::IsObject(a[6])) {
        if (auto out = OutBytes(a, 6)) destination = out->data();
      } else {
        destination = const_cast<void*>(Offset(ctx, a, 6));
      }
      g.gl->ReadPixels(Int(ctx, a, 0), Int(ctx, a, 1), Int(ctx, a, 2), Int(ctx, a, 3), Uint(ctx, a, 4), Uint(ctx, a, 5), destination);
      if (resolved) g.gl->BindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousRead));
      return qe::Undefined();
    });

    // ---- uniforms and vertex attributes given as arrays ----
    auto vectorf = [&](const std::string& name, void(GL_APIENTRY* GlApi::*fn)(GLint, GLsizei, const GLfloat*), size_t components) {
      add(name, [fn, components](GlContext& g, Context& ctx, qe::Args a) {
        if (auto bytes = InBytes(a, 1)) (g.gl->*fn)(Int(ctx, a, 0), static_cast<GLsizei>(bytes->size() / (4 * components)), reinterpret_cast<const GLfloat*>(bytes->data()));
        return qe::Undefined();
      });
    };
    vectorf("uniform1fv", &GlApi::Uniform1fv, 1); vectorf("uniform2fv", &GlApi::Uniform2fv, 2); vectorf("uniform3fv", &GlApi::Uniform3fv, 3); vectorf("uniform4fv", &GlApi::Uniform4fv, 4);
    auto vectori = [&](const std::string& name, void(GL_APIENTRY* GlApi::*fn)(GLint, GLsizei, const GLint*), size_t components) {
      add(name, [fn, components](GlContext& g, Context& ctx, qe::Args a) {
        if (auto bytes = InBytes(a, 1)) (g.gl->*fn)(Int(ctx, a, 0), static_cast<GLsizei>(bytes->size() / (4 * components)), reinterpret_cast<const GLint*>(bytes->data()));
        return qe::Undefined();
      });
    };
    vectori("uniform1iv", &GlApi::Uniform1iv, 1); vectori("uniform2iv", &GlApi::Uniform2iv, 2); vectori("uniform3iv", &GlApi::Uniform3iv, 3); vectori("uniform4iv", &GlApi::Uniform4iv, 4);
    auto vectoru = [&](const std::string& name, void(GL_APIENTRY* GlApi::*fn)(GLint, GLsizei, const GLuint*), size_t components) {
      add(name, [fn, components](GlContext& g, Context& ctx, qe::Args a) {
        if (auto bytes = InBytes(a, 1)) (g.gl->*fn)(Int(ctx, a, 0), static_cast<GLsizei>(bytes->size() / (4 * components)), reinterpret_cast<const GLuint*>(bytes->data()));
        return qe::Undefined();
      });
    };
    vectoru("uniform1uiv", &GlApi::Uniform1uiv, 1); vectoru("uniform2uiv", &GlApi::Uniform2uiv, 2); vectoru("uniform3uiv", &GlApi::Uniform3uiv, 3); vectoru("uniform4uiv", &GlApi::Uniform4uiv, 4);
    auto matrix = [&](const std::string& name, void(GL_APIENTRY* GlApi::*fn)(GLint, GLsizei, GLboolean, const GLfloat*), size_t size) {
      add(name, [fn, size](GlContext& g, Context& ctx, qe::Args a) {
        if (auto bytes = InBytes(a, 2)) (g.gl->*fn)(Int(ctx, a, 0), static_cast<GLsizei>(bytes->size() / (4 * size)), Num(ctx, a, 1) != 0, reinterpret_cast<const GLfloat*>(bytes->data()));
        return qe::Undefined();
      });
    };
    matrix("uniformMatrix2fv", &GlApi::UniformMatrix2fv, 4); matrix("uniformMatrix3fv", &GlApi::UniformMatrix3fv, 9); matrix("uniformMatrix4fv", &GlApi::UniformMatrix4fv, 16);
    matrix("uniformMatrix2x3fv", &GlApi::UniformMatrix2x3fv, 6); matrix("uniformMatrix3x2fv", &GlApi::UniformMatrix3x2fv, 6); matrix("uniformMatrix2x4fv", &GlApi::UniformMatrix2x4fv, 8);
    matrix("uniformMatrix4x2fv", &GlApi::UniformMatrix4x2fv, 8); matrix("uniformMatrix3x4fv", &GlApi::UniformMatrix3x4fv, 12); matrix("uniformMatrix4x3fv", &GlApi::UniformMatrix4x3fv, 12);
    auto attributef = [&](const std::string& name, void(GL_APIENTRY* GlApi::*fn)(GLuint, const GLfloat*)) {
      add(name, [fn](GlContext& g, Context& ctx, qe::Args a) {
        if (auto bytes = InBytes(a, 1)) (g.gl->*fn)(Uint(ctx, a, 0), reinterpret_cast<const GLfloat*>(bytes->data()));
        return qe::Undefined();
      });
    };
    attributef("vertexAttrib1fv", &GlApi::VertexAttrib1fv); attributef("vertexAttrib2fv", &GlApi::VertexAttrib2fv); attributef("vertexAttrib3fv", &GlApi::VertexAttrib3fv);
    attributef("vertexAttrib4fv", &GlApi::VertexAttrib4fv);
    add("vertexAttribI4iv", [](GlContext& g, Context& ctx, qe::Args a) {
      if (auto bytes = InBytes(a, 1)) g.gl->VertexAttribI4iv(Uint(ctx, a, 0), reinterpret_cast<const GLint*>(bytes->data()));
      return qe::Undefined();
    });
    add("vertexAttribI4uiv", [](GlContext& g, Context& ctx, qe::Args a) {
      if (auto bytes = InBytes(a, 1)) g.gl->VertexAttribI4uiv(Uint(ctx, a, 0), reinterpret_cast<const GLuint*>(bytes->data()));
      return qe::Undefined();
    });
    add("clearBufferfv", [](GlContext& g, Context& ctx, qe::Args a) {
      if (auto bytes = InBytes(a, 2)) g.gl->ClearBufferfv(Uint(ctx, a, 0), Int(ctx, a, 1), reinterpret_cast<const GLfloat*>(bytes->data()));
      return qe::Undefined();
    });
    add("clearBufferiv", [](GlContext& g, Context& ctx, qe::Args a) {
      if (auto bytes = InBytes(a, 2)) g.gl->ClearBufferiv(Uint(ctx, a, 0), Int(ctx, a, 1), reinterpret_cast<const GLint*>(bytes->data()));
      return qe::Undefined();
    });
    add("clearBufferuiv", [](GlContext& g, Context& ctx, qe::Args a) {
      if (auto bytes = InBytes(a, 2)) g.gl->ClearBufferuiv(Uint(ctx, a, 0), Int(ctx, a, 1), reinterpret_cast<const GLuint*>(bytes->data()));
      return qe::Undefined();
    });
    add("invalidateFramebuffer", [](GlContext& g, Context& ctx, qe::Args a) {
      if (auto bytes = InBytes(a, 1)) g.gl->InvalidateFramebuffer(Uint(ctx, a, 0), static_cast<GLsizei>(bytes->size() / 4), reinterpret_cast<const GLenum*>(bytes->data()));
      return qe::Undefined();
    });
    add("invalidateSubFramebuffer", [](GlContext& g, Context& ctx, qe::Args a) {
      if (auto bytes = InBytes(a, 1)) g.gl->InvalidateSubFramebuffer(Uint(ctx, a, 0), static_cast<GLsizei>(bytes->size() / 4), reinterpret_cast<const GLenum*>(bytes->data()), Int(ctx, a, 2), Int(ctx, a, 3), Int(ctx, a, 4), Int(ctx, a, 5));
      return qe::Undefined();
    });

    // ---- shaders and programs ----
    add("shaderSource", [](GlContext& g, Context& ctx, qe::Args a) {
      const std::string source = qe::ToWtf8(ctx, a[1]);
      const char* text = source.c_str();
      const GLint length = static_cast<GLint>(source.size());
      g.gl->ShaderSource(Uint(ctx, a, 0), 1, &text, &length);
      return qe::Undefined();
    });
    add("bindAttribLocation", [](GlContext& g, Context& ctx, qe::Args a) {
      g.gl->BindAttribLocation(Uint(ctx, a, 0), Uint(ctx, a, 1), qe::ToWtf8(ctx, a[2]).c_str());
      return qe::Undefined();
    });
    add("getAttribLocation", [](GlContext& g, Context& ctx, qe::Args a) { return qe::FromNumber(g.gl->GetAttribLocation(Uint(ctx, a, 0), qe::ToWtf8(ctx, a[1]).c_str())); });
    add("getUniformLocation", [](GlContext& g, Context& ctx, qe::Args a) { return qe::FromNumber(g.gl->GetUniformLocation(Uint(ctx, a, 0), qe::ToWtf8(ctx, a[1]).c_str())); });
    add("getUniformBlockIndex", [](GlContext& g, Context& ctx, qe::Args a) { return qe::FromNumber(static_cast<double>(g.gl->GetUniformBlockIndex(Uint(ctx, a, 0), qe::ToWtf8(ctx, a[1]).c_str()))); });
    add("getFragDataLocation", [](GlContext& g, Context& ctx, qe::Args a) { return qe::FromNumber(g.gl->GetFragDataLocation(Uint(ctx, a, 0), qe::ToWtf8(ctx, a[1]).c_str())); });
    add("getShaderiv", [](GlContext& g, Context& ctx, qe::Args a) {
      GLint v = 0;
      g.gl->GetShaderiv(Uint(ctx, a, 0), Uint(ctx, a, 1), &v);
      return qe::FromNumber(v);
    });
    add("getProgramiv", [](GlContext& g, Context& ctx, qe::Args a) {
      GLint v = 0;
      g.gl->GetProgramiv(Uint(ctx, a, 0), Uint(ctx, a, 1), &v);
      return qe::FromNumber(v);
    });
    add("getShaderInfoLog", [](GlContext& g, Context& ctx, qe::Args a) {
      GLint length = 0;
      g.gl->GetShaderiv(Uint(ctx, a, 0), GL_INFO_LOG_LENGTH, &length);
      std::string log(static_cast<size_t>(std::max(length, 1)), '\0');
      GLsizei written = 0;
      g.gl->GetShaderInfoLog(Uint(ctx, a, 0), static_cast<GLsizei>(log.size()), &written, log.data());
      log.resize(static_cast<size_t>(written));
      return StringValue(ctx, log);
    });
    add("getProgramInfoLog", [](GlContext& g, Context& ctx, qe::Args a) {
      GLint length = 0;
      g.gl->GetProgramiv(Uint(ctx, a, 0), GL_INFO_LOG_LENGTH, &length);
      std::string log(static_cast<size_t>(std::max(length, 1)), '\0');
      GLsizei written = 0;
      g.gl->GetProgramInfoLog(Uint(ctx, a, 0), static_cast<GLsizei>(log.size()), &written, log.data());
      log.resize(static_cast<size_t>(written));
      return StringValue(ctx, log);
    });
    add("getShaderSource", [](GlContext& g, Context& ctx, qe::Args a) {
      GLint length = 0;
      g.gl->GetShaderiv(Uint(ctx, a, 0), GL_SHADER_SOURCE_LENGTH, &length);
      std::string text(static_cast<size_t>(std::max(length, 1)), '\0');
      GLsizei written = 0;
      g.gl->GetShaderSource(Uint(ctx, a, 0), static_cast<GLsizei>(text.size()), &written, text.data());
      text.resize(static_cast<size_t>(written));
      return StringValue(ctx, text);
    });
    auto active = [&](const char* name, bool attribute) {
      add(name, [attribute](GlContext& g, Context& ctx, qe::Args a) {
        const GLuint program = Uint(ctx, a, 0), index = Uint(ctx, a, 1);
        GLint maxLength = 0;
        g.gl->GetProgramiv(program, attribute ? GL_ACTIVE_ATTRIBUTE_MAX_LENGTH : GL_ACTIVE_UNIFORM_MAX_LENGTH, &maxLength);
        std::string text(static_cast<size_t>(std::max(maxLength, 1)), '\0');
        GLsizei length = 0;
        GLint size = 0;
        GLenum type = 0;
        if (attribute) g.gl->GetActiveAttrib(program, index, static_cast<GLsizei>(text.size()), &length, &size, &type, text.data());
        else g.gl->GetActiveUniform(program, index, static_cast<GLsizei>(text.size()), &length, &size, &type, text.data());
        text.resize(static_cast<size_t>(length));
        std::string json = "{\"name\":\"";
        for (char c : text) { if (c == '"' || c == '\\') json += '\\'; json += c; }
        return StringValue(ctx, json + "\",\"size\":" + std::to_string(size) + ",\"type\":" + std::to_string(type) + "}");
      });
    };
    active("getActiveAttrib", true);
    active("getActiveUniform", false);
    add("getAttachedShaders", [](GlContext& g, Context& ctx, qe::Args a) {
      GLuint shaders[16];
      GLsizei count = 0;
      g.gl->GetAttachedShaders(Uint(ctx, a, 0), 16, &count, shaders);
      double values[16];
      for (int i = 0; i < count; ++i) values[i] = shaders[i];
      return NumberArray(ctx, values, static_cast<size_t>(count));
    });
    add("getShaderPrecisionFormat", [](GlContext& g, Context& ctx, qe::Args a) {
      GLint range[2] = {0, 0}, precision = 0;
      g.gl->GetShaderPrecisionFormat(Uint(ctx, a, 0), Uint(ctx, a, 1), range, &precision);
      const double values[3] = {static_cast<double>(range[0]), static_cast<double>(range[1]), static_cast<double>(precision)};
      return NumberArray(ctx, values, 3);
    });
    add("getUniform", [](GlContext& g, Context& ctx, qe::Args a) {
      // (program, location, kind 0 float / 1 int / 2 unsigned, count)
      const GLuint program = Uint(ctx, a, 0);
      const GLint location = Int(ctx, a, 1);
      const int kind = Int(ctx, a, 2);
      const int count = std::clamp(Int(ctx, a, 3), 1, 16);
      double values[16] = {0};
      if (kind == 0) { GLfloat v[16] = {0}; g.gl->GetUniformfv(program, location, v); for (int i = 0; i < count; ++i) values[i] = v[i]; }
      else if (kind == 1) { GLint v[16] = {0}; g.gl->GetUniformiv(program, location, v); for (int i = 0; i < count; ++i) values[i] = v[i]; }
      else { GLuint v[16] = {0}; g.gl->GetUniformuiv(program, location, v); for (int i = 0; i < count; ++i) values[i] = v[i]; }
      return NumberArray(ctx, values, static_cast<size_t>(count));
    });
    add("transformFeedbackVaryings", [](GlContext& g, Context& ctx, qe::Args a) {
      // The names come as one string, separated by commas.
      const std::string joined = qe::ToWtf8(ctx, a[1]);
      std::vector<std::string> names;
      size_t at = 0;
      while (at <= joined.size()) {
        size_t end = joined.find(',', at);
        if (end == std::string::npos) end = joined.size();
        if (end > at) names.push_back(joined.substr(at, end - at));
        at = end + 1;
      }
      std::vector<const char*> pointers;
      for (const std::string& n : names) pointers.push_back(n.c_str());
      g.gl->TransformFeedbackVaryings(Uint(ctx, a, 0), static_cast<GLsizei>(pointers.size()), pointers.data(), Uint(ctx, a, 2));
      return qe::Undefined();
    });
    add("getTransformFeedbackVarying", [](GlContext& g, Context& ctx, qe::Args a) {
      char name[256];
      GLsizei length = 0, size = 0;
      GLenum type = 0;
      g.gl->GetTransformFeedbackVarying(Uint(ctx, a, 0), Uint(ctx, a, 1), sizeof name, &length, &size, &type, name);
      return StringValue(ctx, "{\"name\":\"" + std::string(name, static_cast<size_t>(length)) + "\",\"size\":" + std::to_string(size) + ",\"type\":" + std::to_string(type) + "}");
    });
    add("getUniformIndices", [](GlContext& g, Context& ctx, qe::Args a) {
      const std::string joined = qe::ToWtf8(ctx, a[1]);
      std::vector<std::string> names;
      size_t at = 0;
      while (at <= joined.size()) {
        size_t end = joined.find(',', at);
        if (end == std::string::npos) end = joined.size();
        names.push_back(joined.substr(at, end - at));
        at = end + 1;
      }
      std::vector<const char*> pointers;
      for (const std::string& n : names) pointers.push_back(n.c_str());
      std::vector<GLuint> indices(names.size());
      g.gl->GetUniformIndices(Uint(ctx, a, 0), static_cast<GLsizei>(pointers.size()), pointers.data(), indices.data());
      std::vector<double> values(indices.begin(), indices.end());
      return NumberArray(ctx, values.data(), values.size());
    });
    add("getActiveUniforms", [](GlContext& g, Context& ctx, qe::Args a) {
      auto bytes = InBytes(a, 1);
      if (!bytes) return qe::Undefined();
      const size_t count = bytes->size() / 4;
      std::vector<GLint> out(count);
      g.gl->GetActiveUniformsiv(Uint(ctx, a, 0), static_cast<GLsizei>(count), reinterpret_cast<const GLuint*>(bytes->data()), Uint(ctx, a, 2), out.data());
      std::vector<double> values(out.begin(), out.end());
      return NumberArray(ctx, values.data(), values.size());
    });
    add("getActiveUniformBlockParameter", [](GlContext& g, Context& ctx, qe::Args a) {
      const GLuint program = Uint(ctx, a, 0), index = Uint(ctx, a, 1);
      const GLenum pname = Uint(ctx, a, 2);
      if (pname == GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES) {
        GLint count = 0;
        g.gl->GetActiveUniformBlockiv(program, index, GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS, &count);
        std::vector<GLint> indices(static_cast<size_t>(std::max(count, 0)));
        if (count > 0) g.gl->GetActiveUniformBlockiv(program, index, pname, indices.data());
        std::vector<double> values(indices.begin(), indices.end());
        return NumberArray(ctx, values.data(), values.size());
      }
      GLint v = 0;
      g.gl->GetActiveUniformBlockiv(program, index, pname, &v);
      return qe::FromNumber(v);
    });
    add("getActiveUniformBlockName", [](GlContext& g, Context& ctx, qe::Args a) {
      GLint length = 0;
      g.gl->GetActiveUniformBlockiv(Uint(ctx, a, 0), Uint(ctx, a, 1), GL_UNIFORM_BLOCK_NAME_LENGTH, &length);
      std::string name(static_cast<size_t>(std::max(length, 1)), '\0');
      GLsizei written = 0;
      g.gl->GetActiveUniformBlockName(Uint(ctx, a, 0), Uint(ctx, a, 1), static_cast<GLsizei>(name.size()), &written, name.data());
      name.resize(static_cast<size_t>(written));
      return StringValue(ctx, name);
    });

    // ---- getters ----
    add("getIntegerv", [](GlContext& g, Context& ctx, qe::Args a) {
      GLint v[16] = {0};
      const int count = std::clamp(Int(ctx, a, 1), 1, 16);
      g.gl->GetIntegerv(Uint(ctx, a, 0), v);
      double d[16];
      for (int i = 0; i < count; ++i) d[i] = v[i];
      return NumberArray(ctx, d, static_cast<size_t>(count));
    });
    add("getInteger64v", [](GlContext& g, Context& ctx, qe::Args a) {
      GLint64 v = 0;
      g.gl->GetInteger64v(Uint(ctx, a, 0), &v);
      return qe::FromNumber(static_cast<double>(v));
    });
    add("getIntegeri_v", [](GlContext& g, Context& ctx, qe::Args a) {
      GLint v[4] = {0};
      const int count = std::clamp(Int(ctx, a, 2), 1, 4);
      g.gl->GetIntegeri_v(Uint(ctx, a, 0), Uint(ctx, a, 1), v);
      double d[4];
      for (int i = 0; i < count; ++i) d[i] = v[i];
      return NumberArray(ctx, d, static_cast<size_t>(count));
    });
    add("getFloatv", [](GlContext& g, Context& ctx, qe::Args a) {
      GLfloat v[16] = {0};
      const int count = std::clamp(Int(ctx, a, 1), 1, 16);
      g.gl->GetFloatv(Uint(ctx, a, 0), v);
      double d[16];
      for (int i = 0; i < count; ++i) d[i] = v[i];
      return NumberArray(ctx, d, static_cast<size_t>(count));
    });
    add("getBooleanv", [](GlContext& g, Context& ctx, qe::Args a) {
      GLboolean v[16] = {0};
      const int count = std::clamp(Int(ctx, a, 1), 1, 16);
      g.gl->GetBooleanv(Uint(ctx, a, 0), v);
      double d[16];
      for (int i = 0; i < count; ++i) d[i] = v[i];
      return NumberArray(ctx, d, static_cast<size_t>(count));
    });
    add("getString", [](GlContext& g, Context& ctx, qe::Args a) {
      const GLubyte* s = g.gl->GetString(Uint(ctx, a, 0));
      return StringValue(ctx, s ? reinterpret_cast<const char*>(s) : "");
    });
    add("getBufferParameter", [](GlContext& g, Context& ctx, qe::Args a) {
      GLint v = 0;
      g.gl->GetBufferParameteriv(Uint(ctx, a, 0), Uint(ctx, a, 1), &v);
      return qe::FromNumber(v);
    });
    add("getFramebufferAttachmentParameter", [](GlContext& g, Context& ctx, qe::Args a) {
      GLint v = 0;
      GLenum attachment = Uint(ctx, a, 1);
      const GLenum target = Uint(ctx, a, 0);
      const bool onDefault = (target == GL_READ_FRAMEBUFFER ? g.readFbo : g.drawFbo) == 0;
      if (onDefault) {
        if (attachment == GL_BACK) attachment = GL_COLOR_ATTACHMENT0;
        else if (attachment == GL_DEPTH) attachment = GL_DEPTH_ATTACHMENT;
        else if (attachment == GL_STENCIL) attachment = GL_STENCIL_ATTACHMENT;
      }
      g.gl->GetFramebufferAttachmentParameteriv(target, attachment, Uint(ctx, a, 2), &v);
      return qe::FromNumber(v);
    });
    add("getRenderbufferParameter", [](GlContext& g, Context& ctx, qe::Args a) {
      GLint v = 0;
      g.gl->GetRenderbufferParameteriv(Uint(ctx, a, 0), Uint(ctx, a, 1), &v);
      return qe::FromNumber(v);
    });
    add("getTexParameter", [](GlContext& g, Context& ctx, qe::Args a) {
      GLint v = 0;
      g.gl->GetTexParameteriv(Uint(ctx, a, 0), Uint(ctx, a, 1), &v);
      return qe::FromNumber(v);
    });
    add("getSamplerParameter", [](GlContext& g, Context& ctx, qe::Args a) {
      GLfloat v = 0;
      g.gl->GetSamplerParameterfv(Uint(ctx, a, 0), Uint(ctx, a, 1), &v);
      return qe::FromNumber(v);
    });
    add("getVertexAttrib", [](GlContext& g, Context& ctx, qe::Args a) {
      const GLuint index = Uint(ctx, a, 0);
      const GLenum pname = Uint(ctx, a, 1);
      if (pname == GL_CURRENT_VERTEX_ATTRIB) {
        GLfloat v[4] = {0};
        g.gl->GetVertexAttribfv(index, pname, v);
        const double d[4] = {v[0], v[1], v[2], v[3]};
        return NumberArray(ctx, d, 4);
      }
      GLint v = 0;
      g.gl->GetVertexAttribiv(index, pname, &v);
      return qe::FromNumber(v);
    });
    add("getVertexAttribOffset", [](GlContext& g, Context& ctx, qe::Args a) {
      void* p = nullptr;
      g.gl->GetVertexAttribPointerv(Uint(ctx, a, 0), GL_VERTEX_ATTRIB_ARRAY_POINTER, &p);
      return qe::FromNumber(static_cast<double>(reinterpret_cast<uintptr_t>(p)));
    });
    add("getQuery", [](GlContext& g, Context& ctx, qe::Args a) {
      GLint v = 0;
      g.gl->GetQueryiv(Uint(ctx, a, 0), Uint(ctx, a, 1), &v);
      return qe::FromNumber(v);
    });
    add("getQueryParameter", [](GlContext& g, Context& ctx, qe::Args a) {
      GLuint v = 0;
      g.gl->GetQueryObjectuiv(Uint(ctx, a, 0), Uint(ctx, a, 1), &v);
      return qe::FromNumber(static_cast<double>(v));
    });
    add("getInternalformatParameter", [](GlContext& g, Context& ctx, qe::Args a) {
      GLint count = 0;
      g.gl->GetInternalformativ(Uint(ctx, a, 0), Uint(ctx, a, 1), GL_NUM_SAMPLE_COUNTS, 1, &count);
      std::vector<GLint> v(static_cast<size_t>(std::max(count, 0)));
      if (count > 0) g.gl->GetInternalformativ(Uint(ctx, a, 0), Uint(ctx, a, 1), GL_SAMPLES, count, v.data());
      std::vector<double> d(v.begin(), v.end());
      return NumberArray(ctx, d.data(), d.size());
    });

    // ---- sync objects ----
    add("fenceSync", [](GlContext& g, Context& ctx, qe::Args a) {
      GLsync sync = g.gl->FenceSync(Uint(ctx, a, 0), Uint(ctx, a, 1));
      if (!sync) return qe::FromNumber(0);
      const uint32_t id = g.nextSync++;
      g.syncs[id] = sync;
      return qe::FromNumber(id);
    });
    add("clientWaitSync", [](GlContext& g, Context& ctx, qe::Args a) {
      const auto found = g.syncs.find(Uint(ctx, a, 0));
      if (found == g.syncs.end()) return qe::FromNumber(GL_WAIT_FAILED);
      return qe::FromNumber(static_cast<double>(g.gl->ClientWaitSync(found->second, Uint(ctx, a, 1), static_cast<GLuint64>(Num(ctx, a, 2)))));
    });
    add("waitSync", [](GlContext& g, Context& ctx, qe::Args a) {
      const auto found = g.syncs.find(Uint(ctx, a, 0));
      if (found != g.syncs.end()) g.gl->WaitSync(found->second, 0, GL_TIMEOUT_IGNORED);
      return qe::Undefined();
    });
    add("deleteSync", [](GlContext& g, Context& ctx, qe::Args a) {
      const auto found = g.syncs.find(Uint(ctx, a, 0));
      if (found != g.syncs.end()) {
        g.gl->DeleteSync(found->second);
        g.syncs.erase(found);
      }
      return qe::Undefined();
    });
    add("isSync", [](GlContext& g, Context& ctx, qe::Args a) { return qe::FromNumber(g.syncs.count(Uint(ctx, a, 0)) ? 1 : 0); });
    add("getSyncParameter", [](GlContext& g, Context& ctx, qe::Args a) {
      const auto found = g.syncs.find(Uint(ctx, a, 0));
      if (found == g.syncs.end()) return qe::FromNumber(0);
      GLint v = 0;
      GLsizei length = 0;
      g.gl->GetSynciv(found->second, Uint(ctx, a, 1), 1, &length, &v);
      return qe::FromNumber(v);
    });
    return list;
  }();
  return operations;
}

// ---- The natives ----

namespace {

// Operations that draw into the drawing buffer: what was drawn is shown a moment later.
bool Draws(const std::string& name) {
  static const std::set<std::string> names = {"clear", "clearBufferfi", "clearBufferfv", "clearBufferiv", "clearBufferuiv", "drawArrays", "drawArraysInstanced", "drawElements",
                                              "drawElementsInstanced", "drawRangeElements", "blitFramebuffer"};
  return names.count(name) > 0;
}

// __solarGlOps(): the names of the operations, whose numbers are their places.
Value OpsCall(Context& ctx, Value, qe::Args, Value) {
  Value list = qe::NewArray(ctx);
  for (const auto& [name, handler] : Operations()) qe::ArrayPush(ctx, list, qe::FromWtf8(ctx, name));
  return list;
}

// __solarGlCreate(version, attributes (JSON text), width, height): [context, surface] of a new context and the surface that shows it, or undefined where there is none.
Value CreateCall(Context& ctx, Value, qe::Args a, Value) {
  Natives* n = NativesOf(ctx);
  if (!n || !EnsureEgl()) return qe::Undefined();
  Egl& egl = TheEgl();
  auto g = std::make_unique<GlContext>();
  g->egl = &egl;
  g->gl = &egl.api;
  g->version = Int(ctx, a, 0) == 2 ? 2 : 1;
  const std::string attributes = a.size() > 1 ? qe::ToWtf8(ctx, a[1]) : "";
  auto flag = [&](const char* key, bool fallback) {
    const size_t at = attributes.find(std::string("\"") + key + "\":");
    if (at == std::string::npos) return fallback;
    return attributes.compare(at + std::strlen(key) + 3, 4, "true") == 0;
  };
  g->antialias = flag("antialias", true);
  g->alpha = flag("alpha", true);
  g->depth = flag("depth", true);
  g->stencil = flag("stencil", false);
  g->premultiplied = flag("premultipliedAlpha", true);
  g->preserve = flag("preserveDrawingBuffer", false);
  g->width = std::max(1, Int(ctx, a, 2));
  g->height = std::max(1, Int(ctx, a, 3));
  const EGLint contextAttributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
  g->context = Proc<EGLContext (*)(EGLDisplay, EGLConfig, EGLContext, const EGLint*)>(egl, "eglCreateContext")(egl.display, egl.config, EGL_NO_CONTEXT, contextAttributes);
  if (g->context == EGL_NO_CONTEXT) return qe::Undefined();
  MakeCurrent(*g);
  GlApi& gl = *g->gl;
  auto text = [&](GLenum name) {
    const GLubyte* s = gl.GetString(name);
    return std::string(s ? reinterpret_cast<const char*>(s) : "");
  };
  g->renderer = text(GL_RENDERER);
  g->vendor = text(GL_VENDOR);
  g->glVersion = text(GL_VERSION);
  GLint count = 0;
  gl.GetIntegerv(GL_NUM_EXTENSIONS, &count);
  for (GLint i = 0; i < count; ++i) {
    const GLubyte* e = gl.GetStringi(GL_EXTENSIONS, static_cast<GLuint>(i));
    if (e) g->extensions.emplace_back(reinterpret_cast<const char*>(e));
  }
  BuildBuffers(*g);
  gl.Viewport(0, 0, g->width, g->height);
  gl.Disable(GL_SCISSOR_TEST);
  const uint32_t surface = n->next++;
  n->surfaces[surface] = std::make_shared<CanvasSurface>(g->width, g->height);
  g->surfaceId = surface;
  WebGlState& state = StateFor(*n);
  const uint32_t id = state.next++;
  GlContext* raw = g.get();
  state.contexts[id] = std::move(g);
  // The drawing buffer is copied into the surface just before the surface is published.
  n->beforePublish[surface] = [raw, n, surface] {
    raw->dirtySinceRead = false;
    MakeCurrent(*raw);
    const std::vector<uint8_t> pixels = ReadDrawingBuffer(*raw);
    const SkImageInfo info = SkImageInfo::Make(std::max(1, raw->width), std::max(1, raw->height), kRGBA_8888_SkColorType, raw->premultiplied ? kPremul_SkAlphaType : kUnpremul_SkAlphaType);
    SkBitmap bitmap;
    if (!bitmap.tryAllocPixels(info)) return;
    std::memcpy(bitmap.getPixels(), pixels.data(), pixels.size());
    bitmap.setImmutable();
    const auto found = n->surfaces.find(surface);
    if (found != n->surfaces.end()) found->second->SetContents(bitmap.asImage(), true);
    if (!raw->preserve) ClearAfterPresent(*raw);
  };
  Value list = qe::NewArray(ctx);
  qe::ArrayPush(ctx, list, qe::FromNumber(id));
  qe::ArrayPush(ctx, list, qe::FromNumber(surface));
  return list;
}

GlContext* ContextOf(Natives& n, Context& ctx, const Value& id) {
  WebGlState& state = StateFor(n);
  const auto found = state.contexts.find(qe::ToUint32(ctx, id));
  return found == state.contexts.end() ? nullptr : found->second.get();
}

// __solarGlInfo(context): JSON text of what the context is: the renderer, the version, the extensions it has, its limits, and the name the drawing buffer's framebuffer has.
Value InfoCall(Context& ctx, Value, qe::Args a, Value) {
  Natives* n = NativesOf(ctx);
  if (!n || a.empty()) return qe::Undefined();
  GlContext* g = ContextOf(*n, ctx, a[0]);
  if (!g) return qe::Undefined();
  MakeCurrent(*g);
  auto escape = [](const std::string& s) {
    std::string out;
    for (char c : s) { if (c == '"' || c == '\\') out += '\\'; out += c; }
    return out;
  };
  std::string json = "{\"renderer\":\"" + escape(g->renderer) + "\",\"vendor\":\"" + escape(g->vendor) + "\",\"version\":\"" + escape(g->glVersion) + "\",\"defaultFramebuffer\":" +
                     std::to_string(g->defaultFbo) + ",\"samples\":" + std::to_string(g->samples) + ",\"extensions\":[";
  for (size_t i = 0; i < g->extensions.size(); ++i) json += (i ? ",\"" : "\"") + escape(g->extensions[i]) + "\"";
  return qe::FromWtf8(ctx, json + "]}");
}

// __solarGlResize(context, width, height)
Value ResizeCall(Context& ctx, Value, qe::Args a, Value) {
  Natives* n = NativesOf(ctx);
  if (!n || a.size() < 3) return qe::Undefined();
  GlContext* g = ContextOf(*n, ctx, a[0]);
  if (!g) return qe::Undefined();
  MakeCurrent(*g);
  GLint viewport[4];
  g->gl->GetIntegerv(GL_VIEWPORT, viewport);
  g->width = std::max(1, Int(ctx, a, 1));
  g->height = std::max(1, Int(ctx, a, 2));
  BuildBuffers(*g);
  g->gl->Viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
  const auto surface = n->surfaces.find(g->surfaceId);
  if (surface != n->surfaces.end()) surface->second->Reset(g->width, g->height);
  return qe::Undefined();
}

// __solarGlPresent(context): what has been drawn goes into the surface now (for toDataURL and for a canvas drawn from this one) instead of at the next turn of the loop.
Value PresentCall(Context& ctx, Value, qe::Args a, Value) {
  Natives* n = NativesOf(ctx);
  if (!n || a.empty()) return qe::Undefined();
  GlContext* g = ContextOf(*n, ctx, a[0]);
  if (!g) return qe::Undefined();
  const auto hook = n->beforePublish.find(g->surfaceId);
  if (hook != n->beforePublish.end() && g->dirtySinceRead) {
    hook->second();
    g->dirtySinceRead = false;
  }
  return qe::Undefined();
}

Value DestroyCall(Context& ctx, Value, qe::Args a, Value) {
  Natives* n = NativesOf(ctx);
  if (!n || a.empty()) return qe::Undefined();
  WebGlState& state = StateFor(*n);
  const auto found = state.contexts.find(qe::ToUint32(ctx, a[0]));
  if (found == state.contexts.end()) return qe::Undefined();
  GlContext& g = *found->second;
  MakeCurrent(g);
  Egl& egl = *g.egl;
  Proc<EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext)>(egl, "eglMakeCurrent")(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  g_current = EGL_NO_CONTEXT;
  Proc<EGLBoolean (*)(EGLDisplay, EGLContext)>(egl, "eglDestroyContext")(egl.display, g.context);
  n->beforePublish.erase(g.surfaceId);
  state.contexts.erase(found);
  return qe::Undefined();
}

// __solarGl(context, operation, ...): one call of the standard's. The numbers of the operations are the places of their names in __solarGlOps().
Value CallCall(Context& ctx, Value, qe::Args a, Value) {
  Natives* n = NativesOf(ctx);
  if (!n || a.size() < 2) return qe::Undefined();
  GlContext* g = ContextOf(*n, ctx, a[0]);
  const auto& operations = Operations();
  const size_t op = static_cast<size_t>(Num(ctx, a, 1));
  if (!g || op >= operations.size()) return qe::Undefined();
  MakeCurrent(*g);
  Value result = operations[op].second(*g, ctx, a.subspan(2));
  if (Draws(operations[op].first)) {
    const auto surface = n->surfaces.find(g->surfaceId);
    if (surface != n->surfaces.end()) {
      surface->second->dirty = true;
      g->dirtySinceRead = true;
      n->Touch(g->surfaceId);
    }
  }
  return result;
}

}  // namespace

}  // namespace solar::paint

namespace solar::webapi {

void InstallWebGlNatives(Quanta::Embed::Realm& realm) {
  namespace p = solar::paint;
  Quanta::Context& ctx = realm.GetContext();
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarGlOps", p::OpsCall, 0);
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarGlCreate", p::CreateCall, 4);
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarGlInfo", p::InfoCall, 1);
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarGlResize", p::ResizeCall, 3);
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarGlPresent", p::PresentCall, 1);
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarGlDestroy", p::DestroyCall, 1);
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarGl", p::CallCall, 2);
}

}  // namespace solar::webapi
