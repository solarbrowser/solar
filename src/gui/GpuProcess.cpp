/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "GpuProcess.h"

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
#include <thread>
#include <vector>

#include "GpuRaster.h"
#include "solar/gui/FrameBuffer.h"
#include "solar/gui/GpuWire.h"
#include "solar/gui/Sandbox.h"
#include "solar/net/Ipc.h"
#include "solar/paint/ListCodec.h"
#include "solar/paint/SkiaPainter.h"

extern char** environ;

namespace solar::gui {

namespace {

// What the GPU's process does for one page: it keeps the display list the page sent (and what that has), and the memory the pictures go in, and draws when it is asked to.
void Serve(GpuRaster& raster, int fd, const std::vector<std::string>& fontDirectories) {
  net::ipc::Channel channel(fd);
  paint::ListDecoder decoder;
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
    if (type >= static_cast<uint8_t>(paint::ListMessage::DefineFace) && type <= static_cast<uint8_t>(paint::ListMessage::SetList)) {
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
          drawn = raster.Draw(*painter, *list, draw.pixelWidth, draw.pixelHeight, draw.density, draw.scrollX, draw.scrollY, bar, slot->second.memory.data(), static_cast<size_t>(slot->second.shape.stride));
        }
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
  GpuRaster::Scope scope(raster);
  painter.reset();
  buffers.clear();
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
  ::prctl(PR_SET_PDEATHSIG, SIGKILL);
  ::signal(SIGPIPE, SIG_IGN);
  net::ipc::Channel control(controlFd);
  std::string why;
  std::unique_ptr<GpuRaster> raster = GpuRaster::Create(&why);
  if (!raster) {
    control.SendRaw(static_cast<uint8_t>(gpu::Control::Unavailable), why);
    return 1;
  }
  // It is shut in before it is told what pages to draw (and is shown to draw when it is: a driver that needs what the policy does not give is found out now, and the pages draw by themselves).
  const SandboxMode mode = SandboxModeFromEnvironment();
  if (mode != SandboxMode::Off) {
    {
      paint::SkiaPainter warm;
      paint::DisplayList empty;
      std::vector<uint8_t> pixels(64 * 64 * 4);
      raster->Draw(warm, empty, 64, 64, 1.f, 0, 0, ScrollbarShape{}, pixels.data(), 64 * 4);
    }
    const SandboxResult result = EnterSandbox(GpuPolicy(), mode);
    if (!result.problems.empty()) std::fprintf(stderr, "gpu sandbox: %s\n", result.problems.c_str());
    if (!result.seccomp) {
      control.SendRaw(static_cast<uint8_t>(gpu::Control::Unavailable), "the GPU's process could not be shut in");
      return 1;
    }
    paint::SkiaPainter check;
    paint::DisplayList list;
    std::vector<uint8_t> pixels(64 * 64 * 4);
    if (!raster->Draw(check, list, 64, 64, 1.f, 0, 0, ScrollbarShape{}, pixels.data(), 64 * 4)) {
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
    if (static_cast<gpu::Control>(type) == gpu::Control::NewClient && fd >= 0) {
      clients.emplace_back([&raster, fd, &fonts] { Serve(*raster, fd, fonts); });
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
  const ssize_t length = ::readlink("/proc/self/exe", self, sizeof(self) - 1);
  if (length <= 0) return fail("cannot find the program");
  self[length] = 0;
  const auto pair = net::ipc::Channel::MakePair();
  if (pair.first < 0) return fail("no connection");
  const int high = ::fcntl(pair.second, F_DUPFD_CLOEXEC, 10);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, high, 3);
  posix_spawn_file_actions_addclosefrom_np(&actions, 4);
  char arg0[] = "solar", arg1[] = "--gpu";
  char* arguments[] = {arg0, arg1, nullptr};
  pid_t child = -1;
  const int status = ::posix_spawn(&child, self, &actions, nullptr, arguments, environ);
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

int GpuHost::NewClient() {
  const auto pair = net::ipc::Channel::MakePair();
  if (pair.first < 0) return -1;
  // (The GPU's end goes to the GPU's process; the page gets the other.)
  if (!impl_->control->SendRaw(static_cast<uint8_t>(gpu::Control::NewClient), "", pair.first)) {
    ::close(pair.first);
    ::close(pair.second);
    return -1;
  }
  ::close(pair.first);
  return pair.second;
}

}  // namespace solar::gui
