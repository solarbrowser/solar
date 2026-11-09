/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/gui/GpuProcess.h"

// The GPU's process is shut in with what Linux has (namespaces, Landlock, seccomp) and draws on the machine's driver or ANGLE: elsewhere there is none yet (docs/porting.md), and the pages draw by
// themselves, on the CPU, as they do when it is gone.
#if !defined(__linux__)
namespace solar::gui {
struct GpuHost::Impl {};
int GpuMain(int) { return 1; }
std::unique_ptr<GpuHost> GpuHost::Start(std::string* why) {
  if (why) *why = "the GPU's process is only on Linux for now";
  return nullptr;
}
GpuHost::GpuHost() {}
GpuHost::~GpuHost() {}
int GpuHost::NewClient(int*) { return -1; }
const std::string& GpuHost::renderer() const {
  static const std::string none;
  return none;
}
}  // namespace solar::gui
#else

#include "solar/gui/Platform.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <sys/syscall.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "solar/gui/GpuRaster.h"
#include "solar/gui/FrameBuffer.h"
#include "solar/gui/GlWire.h"
#include "solar/gui/GpuWire.h"
#include "solar/gui/Sandbox.h"
#include "solar/net/Ipc.h"
#include "solar/paint/GlRuntime.h"
#include "solar/paint/ListCodec.h"
#include "solar/paint/SkiaPainter.h"

extern char** environ;

namespace solar::gui {

namespace {

// What the GPU's process does for one page: it keeps the display list the page sent (and what that has), and the memory the pictures go in, and draws when it is asked to.
void Serve(GpuRaster& raster, int fd, uint64_t token, const std::vector<std::string>& fontDirectories) {
  net::ipc::Channel channel(fd);
  paint::ListDecoder decoder;
  // A canvas that is a WebGL context's is drawn from the texture the context published (the contexts of this page, by its token: another page's are not to be found).
  decoder.SetGlResolver([token](uint32_t context) -> sk_sp<SkImage> {
    paint::gl::Context::Snapshot snapshot;
    if (!paint::gl::LookupSnapshot(token, context, snapshot)) {
      if (std::getenv("SOLAR_GL_DEBUG")) std::fprintf(stderr, "gpu: no texture published for context %u of the page\n", context);
      return nullptr;
    }
    sk_sp<SkImage> image = GpuRaster::TextureImage(snapshot.texture, snapshot.width, snapshot.height, snapshot.premultiplied);
    if (!image && std::getenv("SOLAR_GL_DEBUG")) std::fprintf(stderr, "gpu: Skia cannot draw from texture %u (%dx%d)\n", snapshot.texture, snapshot.width, snapshot.height);
    return image;
  });
  decoder.AllowFontDirectories(fontDirectories);
  auto painter = std::make_unique<paint::SkiaPainter>();
  struct Slot {
    SharedMemory memory;
    gpu::Buffer shape;
  };
  std::map<uint32_t, Slot> buffers;
  uint8_t type = 0;
  std::string bytes;
  int received = -1;
  bool good = true;
  while (good && channel.ReceiveRaw(type, bytes, &received)) {
    const int descriptor = received;
    received = -1;
    if (type >= static_cast<uint8_t>(paint::ListMessage::DefineFace) && type <= static_cast<uint8_t>(paint::ListMessage::DefineGlCanvas)) {
      if (descriptor >= 0) ::close(descriptor);
      // (A message that is not good ends the page's connection: a page that sends one is not to be trusted with a GPU.)
      good = decoder.Apply(static_cast<paint::ListMessage>(type), bytes);
      if (!good) std::fprintf(stderr, "gpu process: a message of a page was not good (type %d, %zu bytes): the page is let go of\n", type, bytes.size());
      continue;
    }
    switch (static_cast<gpu::ToGpu>(type)) {
      case gpu::ToGpu::Buffer: {
        net::ipc::Reader in(bytes);
        gpu::Buffer shape;
        if (!gpu::Read(in, shape)) {
          if (descriptor >= 0) ::close(descriptor);
          good = false;
          break;
        }
        SharedMemory memory;
        if (!memory.Adopt(descriptor, static_cast<size_t>(shape.stride) * static_cast<size_t>(shape.height), true)) {
          good = false;
          break;
        }
        buffers[shape.index] = Slot{std::move(memory), shape};
        break;
      }
      case gpu::ToGpu::Draw: {
        if (descriptor >= 0) ::close(descriptor);
        net::ipc::Reader in(bytes);
        gpu::Draw draw;
        if (!gpu::Read(in, draw)) {
          good = false;
          break;
        }
        const auto slot = buffers.find(draw.index);
        const std::shared_ptr<const paint::DisplayList> list = decoder.list();
        bool drawn = false;
        if (slot != buffers.end() && list && draw.pixelWidth <= slot->second.shape.width && draw.pixelHeight <= slot->second.shape.height) {
          ScrollbarShape bar;
          bar.visible = draw.barVisible;
          bar.emphasized = draw.barEmphasized;
          bar.dragging = draw.barDragging;
          bar.alpha = draw.barAlpha;
          bar.top = draw.barTop;
          bar.height = draw.barHeight;
          bar.windowWidth = draw.windowWidth;
          bar.windowHeight = draw.windowHeight;
          // (The textures of the page's WebGL are not changed or deleted while they are drawn from.)
          std::lock_guard<std::recursive_mutex> shared(paint::gl::SharedLock());
          paint::gl::RunOnGlThread([&] {
            drawn = raster.Draw(*painter, *list, draw.pixelWidth, draw.pixelHeight, draw.density, draw.scrollX, draw.scrollY, bar, slot->second.memory.data(), static_cast<size_t>(slot->second.shape.stride));
          });
        }
        if (!drawn && std::getenv("SOLAR_GL_DEBUG")) std::fprintf(stderr, "gpu: a draw failed: buffer %s, list %s, asked %dx%d, buffer is %dx%d\n", slot != buffers.end() ? "there" : "missing", list ? "there" : "missing", draw.pixelWidth, draw.pixelHeight, slot != buffers.end() ? slot->second.shape.width : 0, slot != buffers.end() ? slot->second.shape.height : 0);
        net::ipc::Writer out;
        out.U32(draw.index);
        out.U64(draw.serial);
        channel.SendRaw(static_cast<uint8_t>(drawn ? gpu::FromGpu::Drawn : gpu::FromGpu::Failed), out.bytes());
        break;
      }
      default:
        if (descriptor >= 0) ::close(descriptor);
        break;
    }
  }
  // (What the page had on the GPU goes with its painter, with the context to hand.)
  paint::gl::RunOnGlThread([&] {
    GpuRaster::Scope scope(raster);
    painter.reset();
  });
  buffers.clear();
}

// What the GPU's process does for one page's WebGL: the contexts the page asks for, the calls its script makes (in batches), and the pictures of what it drew, made in memory the page gave.
void ServeGl(int fd, uint64_t token) {
  net::ipc::Channel channel(fd);
  struct Held {
    std::unique_ptr<paint::gl::Context> context;
    SharedMemory memory;
    int width = 0, height = 0;
  };
  std::map<uint32_t, Held> contexts;
  uint32_t next = 1;
  constexpr size_t kMostContexts = 16;
  const std::vector<std::string>& names = paint::gl::OperationNames();
  uint8_t type = 0;
  std::string bytes;
  int received = -1;
  bool good = true;
  const auto answer = [&](uint64_t request, bool ok, const net::ipc::Writer& payload) {
    net::ipc::Writer out;
    out.U64(request);
    out.Bool(ok);
    std::string message = out.Take();
    message += payload.bytes();
    channel.SendRaw(static_cast<uint8_t>(glwire::FromGl::Answer), message);
  };
  while (good && channel.ReceiveRaw(type, bytes, &received)) {
    const int descriptor = received;
    received = -1;
    net::ipc::Reader in(bytes);
    const auto kind = static_cast<glwire::ToGl>(type);
    if (kind != glwire::ToGl::Buffer && descriptor >= 0) ::close(descriptor);
    // (The GPU is used from one thread: the drawing of the pages and WebGL's contexts are on one EGL, which does not take being used from the several threads of the connections.)
    paint::gl::RunOnGlThread([&] {
    switch (kind) {
      case glwire::ToGl::Create: {
        const uint64_t request = in.U64();
        const int version = in.I32();
        const std::string attributes = in.String(1 << 12);
        const int width = in.I32(), height = in.I32();
        if (!in.ok() || width < 1 || height < 1 || width > glwire::kMostSide || height > glwire::kMostSide) {
          good = false;
          break;
        }
        net::ipc::Writer out;
        std::unique_ptr<paint::gl::Context> context;
        if (contexts.size() < kMostContexts) context = paint::gl::Context::Create(version, attributes, width, height);
        if (!context) {
          answer(request, false, out);
          break;
        }
        const uint32_t id = next++;
        contexts[id].context = std::move(context);
        contexts[id].width = width;
        contexts[id].height = height;
        {
          std::lock_guard<std::recursive_mutex> shared(paint::gl::SharedLock());
          paint::gl::RegisterShared(token, id, contexts[id].context.get());
        }
        out.U32(id);
        answer(request, true, out);
        break;
      }
      case glwire::ToGl::Info: {
        const uint64_t request = in.U64();
        const uint32_t id = in.U32();
        const auto found = contexts.find(id);
        net::ipc::Writer out;
        if (!in.ok() || found == contexts.end()) {
          answer(request, false, out);
          break;
        }
        out.String(found->second.context->Info());
        answer(request, true, out);
        break;
      }
      case glwire::ToGl::Resize: {
        const uint32_t id = in.U32();
        const int width = in.I32(), height = in.I32();
        const auto found = contexts.find(id);
        if (!in.ok() || width < 1 || height < 1 || width > glwire::kMostSide || height > glwire::kMostSide) {
          good = false;
          break;
        }
        if (found != contexts.end()) {
          std::lock_guard<std::recursive_mutex> shared(paint::gl::SharedLock());
          found->second.context->Resize(width, height);
          found->second.width = width;
          found->second.height = height;
          found->second.memory.Reset();  // (the page sends the memory for the new size)
        }
        break;
      }
      case glwire::ToGl::Buffer: {
        const uint32_t id = in.U32();
        const int width = in.I32(), height = in.I32();
        const auto found = contexts.find(id);
        if (!in.ok() || width < 1 || height < 1 || width > glwire::kMostSide || height > glwire::kMostSide) {
          if (descriptor >= 0) ::close(descriptor);
          good = false;
          break;
        }
        SharedMemory memory;
        if (found == contexts.end()) {
          if (descriptor >= 0) ::close(descriptor);
          break;
        }
        if (memory.Adopt(descriptor, static_cast<size_t>(width) * height * 4, true)) found->second.memory = std::move(memory);
        break;
      }
      case glwire::ToGl::Calls: {
        const uint32_t count = in.U32();
        if (!in.ok() || count > (1u << 20)) {
          good = false;
          break;
        }
        for (uint32_t i = 0; i < count && good; ++i) {
          glwire::Call call;
          if (!glwire::ReadCall(in, call, names.size(), names)) {
            std::fprintf(stderr, "gpu process: a call of a page's WebGL was not good (call %u of %u, operation %u %s): the page is let go of\n", i, count, call.operation, call.operation < names.size() ? names[call.operation].c_str() : "?");
            good = false;
            break;
          }
          const auto found = contexts.find(call.context);
          paint::gl::Result result;
          std::string outBytes;
          if (found != contexts.end()) {
            result = found->second.context->Call(call.operation, call.args);
            const int output = glwire::OutputArgument(names[call.operation]);
            if (output >= 0 && static_cast<size_t>(output) < call.args.size() && call.args[output].writable) {
              outBytes.assign(reinterpret_cast<const char*>(call.args[output].data), call.args[output].size);
            }
          }
          if (call.request != 0) {
            net::ipc::Writer out;
            glwire::WriteResult(out, result, outBytes);
            answer(call.request, found != contexts.end(), out);
          }
        }
        break;
      }
      case glwire::ToGl::Present: {
        const uint64_t request = in.U64();
        const uint32_t id = in.U32();
        const auto found = contexts.find(id);
        net::ipc::Writer out;
        if (!in.ok() || found == contexts.end()) {
          answer(request, false, out);
          break;
        }
        const std::vector<uint8_t> pixels = found->second.context->Present();
        if (!found->second.memory.data() || pixels.size() > found->second.memory.size() || pixels.size() != static_cast<size_t>(found->second.width) * found->second.height * 4) {
          answer(request, false, out);
          break;
        }
        std::memcpy(found->second.memory.data(), pixels.data(), pixels.size());
        out.I32(found->second.width);
        out.I32(found->second.height);
        answer(request, true, out);
        break;
      }
      case glwire::ToGl::Publish: {
        const uint64_t request = in.U64();
        const uint32_t id = in.U32();
        const auto found = contexts.find(id);
        net::ipc::Writer out;
        paint::gl::Context::Snapshot snapshot;
        bool ok = false;
        if (in.ok() && found != contexts.end()) {
          std::lock_guard<std::recursive_mutex> shared(paint::gl::SharedLock());
          ok = found->second.context->Publish(snapshot);
          if (std::getenv("SOLAR_GL_DEBUG")) std::fprintf(stderr, "gpu: publish context %u: %s (texture %u, %dx%d, shares %d)\n", id, ok ? "done" : "not done", snapshot.texture, snapshot.width, snapshot.height, found->second.context->shares() ? 1 : 0);
        }
        answer(request, ok, out);
        break;
      }
      case glwire::ToGl::Destroy: {
        const uint32_t id = in.U32();
        if (in.ok() && contexts.count(id)) {
          std::lock_guard<std::recursive_mutex> shared(paint::gl::SharedLock());
          paint::gl::UnregisterShared(token, id);
          contexts.erase(id);
        }
        break;
      }
      default: break;
    }
    });
  }
  // (The page is gone: its contexts are let go of, where nothing is drawing from them.)
  paint::gl::RunOnGlThread([&] {
    std::lock_guard<std::recursive_mutex> shared(paint::gl::SharedLock());
    for (auto& [id, held] : contexts) paint::gl::UnregisterShared(token, id);
    contexts.clear();
  });
}

}  // namespace

// What the GPU's process may read and write once it is shut in: the drivers, their libraries and configuration, the devices, the places a driver keeps its compiled shaders, the fonts.
SandboxPolicy GpuPolicy() {
  SandboxPolicy policy;
  for (const char* path : {"/usr", "/lib", "/lib64", "/etc/ld.so.cache", "/etc/egl", "/etc/glvnd", "/etc/nvidia", "/etc/vulkan", "/etc/OpenCL", "/sys/devices", "/sys/kernel/mm", "/sys/class/drm",
                           "/sys/dev", "/proc/driver", "/proc/meminfo", "/proc/cpuinfo", "/proc/self", "/proc/sys/vm", "/dev/urandom", "/dev/null"}) {
    policy.readable.push_back(path);
  }
  policy.writable.push_back("/dev/dri");
  policy.writable.push_back("/dev/char");  // (a driver keeps links to its devices there)
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator("/dev", error)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("nvidia", 0) == 0) policy.writable.push_back(entry.path().string());
  }
  if (const char* home = std::getenv("HOME")) {
    const std::string user = home;
    for (const char* sub : {"/.fonts", "/.local/share/fonts"}) policy.readable.push_back(user + sub);
    for (const char* sub : {"/.cache/nvidia", "/.nv", "/.cache/mesa_shader_cache", "/.cache/mesa_shader_cache_db", "/.cache/AMD"}) policy.writable.push_back(user + sub);
  }
  if (const char* cache = std::getenv("XDG_CACHE_HOME")) {
    for (const char* sub : {"/nvidia", "/mesa_shader_cache"}) policy.writable.push_back(std::string(cache) + sub);
  }
  // (The system calls a driver makes besides the page's: asking who the process is, its scheduling priorities, a link and a directory for its caches.)
  for (int number : {SYS_getresuid, SYS_getresgid, SYS_sched_get_priority_max, SYS_sched_get_priority_min, SYS_symlink, SYS_chmod}) policy.extraSystemCalls.push_back(number);
  policy.anyIoctl = true;
  policy.unixSockets = true;
  // (The NVIDIA driver runs this when its device links are not as it likes; as shut in as the process is, it can do nothing the process cannot.)
  if (std::filesystem::exists("/usr/bin/nvidia-modprobe", error)) {
    policy.executable.push_back("/usr/bin/nvidia-modprobe");
    for (const char* loader : {"/usr/lib/ld-linux-x86-64.so.2", "/usr/lib64/ld-linux-x86-64.so.2", "/lib64/ld-linux-x86-64.so.2"}) policy.executable.push_back(loader);
  }
  return policy;
}

int GpuMain(int controlFd) {
#if defined(__linux__)
  ::prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
  ::signal(SIGPIPE, SIG_IGN);
  net::ipc::Channel control(controlFd);
  // (The namespaces cannot be entered once the process has threads, and the GPU's thread and the driver's make some.)
  const SandboxMode mode = SandboxModeFromEnvironment();
  std::string namespaceProblems;
  bool namespacesDone = false;
  if (mode != SandboxMode::Off) namespacesDone = EnterNamespaces(namespaceProblems);
  std::string why;
  std::unique_ptr<GpuRaster> raster;
  paint::gl::RunOnGlThread([&] { raster = GpuRaster::Create(&why); });
  if (!raster) {
    control.SendRaw(static_cast<uint8_t>(gpu::Control::Unavailable), why);
    return 1;
  }
  // It is shut in before it is told what pages to draw (and is shown to draw when it is: a driver that needs what the policy does not give is found out now, and the pages draw by themselves).
  if (mode != SandboxMode::Off) {
    // (All of it that touches the GPU is on its thread.)
    paint::gl::RunOnGlThread([&] {
      paint::SkiaPainter warm;
      paint::DisplayList empty;
      std::vector<uint8_t> pixels(64 * 64 * 4);
      raster->Draw(warm, empty, 64, 64, 1.f, 0, 0, ScrollbarShape{}, pixels.data(), 64 * 4);
      // (WebGL's libraries and driver are loaded before the process is shut in, with a context made and drawn from.)
      if (paint::gl::Available()) {
        if (auto context = paint::gl::Context::Create(2, "{}", 16, 16)) context->Present();
      }
    });
    SandboxPolicy policy = GpuPolicy();
    policy.namespacesDone = namespacesDone;
    SandboxResult result = EnterSandbox(policy, mode);
    result.problems += namespaceProblems;
    if (!result.problems.empty()) std::fprintf(stderr, "gpu sandbox: %s\n", result.problems.c_str());
    if (!result.seccomp) {
      control.SendRaw(static_cast<uint8_t>(gpu::Control::Unavailable), "the GPU's process could not be shut in");
      return 1;
    }
    bool works = false;
    paint::gl::RunOnGlThread([&] {
      if (paint::gl::Available()) {
        if (auto context = paint::gl::Context::Create(2, "{}", 16, 16)) context->Present();
      }
      paint::SkiaPainter check;
      paint::DisplayList list;
      std::vector<uint8_t> pixels(64 * 64 * 4);
      works = raster->Draw(check, list, 64, 64, 1.f, 0, 0, ScrollbarShape{}, pixels.data(), 64 * 4);
    });
    if (!works) {
      control.SendRaw(static_cast<uint8_t>(gpu::Control::Unavailable), "the GPU does not draw once the process is shut in");
      return 1;
    }
  }
  control.SendRaw(static_cast<uint8_t>(gpu::Control::Ready), raster->renderer());
  // The pages' fonts are opened here from files in the places the system keeps its fonts.
  std::vector<std::string> fonts = {"/usr/share/fonts", "/usr/local/share/fonts"};
  if (const char* home = std::getenv("HOME")) {
    fonts.push_back(std::string(home) + "/.fonts");
    fonts.push_back(std::string(home) + "/.local/share/fonts");
  }
  std::vector<std::thread> clients;
  uint8_t type = 0;
  std::string bytes;
  int fd = -1;
  while (control.ReceiveRaw(type, bytes, &fd)) {
    net::ipc::Reader token(bytes);
    const uint64_t pageToken = token.U64();
    if (static_cast<gpu::Control>(type) == gpu::Control::NewClient && fd >= 0) {
      clients.emplace_back([&raster, fd, pageToken, &fonts] { Serve(*raster, fd, pageToken, fonts); });
    } else if (static_cast<gpu::Control>(type) == gpu::Control::NewGlClient && fd >= 0) {
      clients.emplace_back([fd, pageToken] { ServeGl(fd, pageToken); });
    } else if (fd >= 0) {
      ::close(fd);
    }
    fd = -1;
  }
  // The browser is gone.
  for (std::thread& client : clients) client.detach();
  return 0;
}

// ---- The browser's side ----

struct GpuHost::Impl {
  std::mutex clientMutex;
  pid_t child = -1;
  std::unique_ptr<net::ipc::Channel> control;
  std::string renderer;
};

GpuHost::GpuHost() : impl_(std::make_unique<Impl>()) {}

GpuHost::~GpuHost() {
  if (impl_->control) impl_->control->Shutdown();
  if (impl_->child > 0) {
    int status = 0;
    bool done = false;
    for (int i = 0; i < 200 && !done; ++i) {
      done = ::waitpid(impl_->child, &status, WNOHANG) == impl_->child;
      if (!done) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!done) {
      ::kill(impl_->child, SIGKILL);
      ::waitpid(impl_->child, &status, 0);
    }
  }
}

const std::string& GpuHost::renderer() const { return impl_->renderer; }

std::unique_ptr<GpuHost> GpuHost::Start(std::string* why) {
  const auto fail = [&](const std::string& text) -> std::unique_ptr<GpuHost> {
    if (why) *why = text;
    return nullptr;
  };
  if (const char* wanted = std::getenv("SOLAR_GUI_GPU")) {
    if (std::string(wanted) == "cpu") return fail("SOLAR_GUI_GPU=cpu");
  }
  char self[4096];
  const std::string selfPath = solar::gui::ExecutablePath();
  if (selfPath.empty()) return fail("cannot find the program");
  std::snprintf(self, sizeof(self), "%s", selfPath.c_str());
  const auto pair = net::ipc::Channel::MakePair();
  if (pair.first < 0) return fail("no connection");
  const int high = ::fcntl(pair.second, F_DUPFD_CLOEXEC, 10);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, high, 3);
  char arg0[] = "solar", arg1[] = "--gpu";
  char* arguments[] = {arg0, arg1, nullptr};
  pid_t child = -1;
  const int status = solar::gui::SpawnKeepingOnly(&child, self, &actions, 4, arguments);
  posix_spawn_file_actions_destroy(&actions);
  ::close(high);
  ::close(pair.second);
  if (status != 0) {
    ::close(pair.first);
    return fail("cannot start the GPU's process");
  }
  std::unique_ptr<GpuHost> host(new GpuHost());
  host->impl_->child = child;
  host->impl_->control = std::make_unique<net::ipc::Channel>(pair.first);
  // It says whether it can draw (a moment is given: a driver may be slow to start).
  uint8_t type = 0;
  std::string bytes;
  bool answered = false;
  std::thread waiter([&] { answered = host->impl_->control->ReceiveRaw(type, bytes); });
  for (int i = 0; i < 500 && !answered; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if (!answered) {
    host->impl_->control->Shutdown();
    waiter.join();
    return fail("the GPU's process did not answer");
  }
  waiter.join();
  if (static_cast<gpu::Control>(type) != gpu::Control::Ready) return fail(bytes);
  host->impl_->renderer = bytes;
  return host;
}

int GpuHost::NewClient(int* glFd) {
  std::lock_guard<std::mutex> lock(impl_->clientMutex);
  // (What names a page to the GPU's process: not to be guessed by another.)
  static std::random_device device;
  const uint64_t token = (static_cast<uint64_t>(device()) << 32) | device();
  net::ipc::Writer tokenBytes;
  tokenBytes.U64(token);
  if (glFd) {
    *glFd = -1;
    const auto gl = net::ipc::Channel::MakePair();
    if (gl.first >= 0) {
      if (impl_->control->SendRaw(static_cast<uint8_t>(gpu::Control::NewGlClient), tokenBytes.bytes(), gl.first)) *glFd = gl.second;
      else ::close(gl.second);
      ::close(gl.first);
    }
  }
  const auto pair = net::ipc::Channel::MakePair();
  if (pair.first < 0) return -1;
  // (The GPU's end goes to the GPU's process; the page gets the other.)
  if (!impl_->control->SendRaw(static_cast<uint8_t>(gpu::Control::NewClient), tokenBytes.bytes(), pair.first)) {
    ::close(pair.first);
    ::close(pair.second);
    return -1;
  }
  ::close(pair.first);
  return pair.second;
}

}  // namespace solar::gui

#endif  // __linux__
