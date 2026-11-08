/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/encode/SkPngEncoder.h"
#include "solar/gui/FrameBuffer.h"
#include "solar/media/Player.h"
#include "solar/paint/SkiaPainter.h"
#include "Renderer.h"
#include "RemoteAudio.h"
#include "RemoteStorage.h"

namespace solar::gui {

using net::ipc::Writer;

// The compositor: the scroll of the page (a wheel step is a glide), the scrollbar, and the drawing of the display list the page made, at the scroll it has now, into a picture the window
// shows. It is on a thread of its own so that a script that runs long does not stop the page from scrolling. Returns when the program is to end.
void RunCompositor(Shared& shared, net::ipc::Channel& link) {
  solar::paint::SkiaPainter painter;
  // The pictures it draws into are memory the window can see (it only reads); one is the window's while it shows it. They are made again when the window changes its size.
  struct Slot {
    SharedMemory memory;
    int width = 0, height = 0, stride = 0;
    bool held = false;  // the window has it
    bool announced = false;
  };
  std::vector<Slot> slots(3);
  uint64_t frameSerial = 0;
  bool stalledForBuffer = false;
  Frame frame;
  // Scrolling goes toward where it was asked to, a part of the way each frame (it eases out), so that a wheel step is a glide and not a jump.
  double x = 0, y = 0, targetX = 0, targetY = 0;
  bool gliding = false;
  Clock::time_point lastGlide = Clock::now();
  Clock::time_point scrollActive = Clock::now() - std::chrono::seconds(10);
  // The scrollbar of the window floats over the page: it shows while the page is scrolled (and under the pointer), then fades; it can be dragged.
  bool barDrag = false, barHover = false;
  float lockedAtX = 0, lockedAtY = 0;  // (where the pointer was when it was locked: it goes back there)
  double barGrab = 0;
  struct Bar {
    double top = 0, height = 0, track = 0, maxY = 0;
  };
  const auto barOf = [&](Bar& bar) -> bool {
    const int h = shared.height.load();
    if (frame.maxY <= 0.5 || h <= 0) return false;
    bar.track = h;
    bar.height = std::max(32.0, double(h) * h / (h + frame.maxY));
    bar.maxY = frame.maxY;
    bar.top = y / frame.maxY * (h - bar.height);
    return true;
  };
  bool dirty = true;
  bool running = true;
  std::string shotPath;
  wire::Geometry known;  // (what the window last said it is)
  const auto publish = [&] {
    shared.scrollX = x;
    shared.scrollY = y;
    ++shared.scrollSerial;
    shared.gliding = gliding;
    shared.WakePage();
  };
  const auto clampTargets = [&] {
    targetX = std::clamp(targetX, 0.0, std::max(0.0, frame.maxX));
    targetY = std::clamp(targetY, 0.0, std::max(0.0, frame.maxY));
  };
  const auto scrollBy = [&](double dx, double dy, bool instant) {
    if (!gliding) {
      targetX = x;
      targetY = y;
    }
    targetX += dx;
    targetY += dy;
    clampTargets();
    scrollActive = Clock::now();
    shared.scrollInputNs = NowNs();
    if (instant) {
      x = targetX;
      y = targetY;
      gliding = false;
      publish();
    } else {
      if (!gliding) lastGlide = Clock::now();
      gliding = true;
      shared.gliding = true;
    }
    dirty = true;
  };
  const auto scrollTo = [&](double nx, double ny) {
    targetX = x = std::clamp(nx, 0.0, std::max(0.0, frame.maxX));
    targetY = y = std::clamp(ny, 0.0, std::max(0.0, frame.maxY));
    gliding = false;
    publish();
    dirty = true;
  };
  const auto applyGeometry = [&](const wire::Geometry& g) {
    known = g;
    shared.width = g.width;
    shared.height = g.height;
    shared.pixelWidth = g.pixelWidth;
    shared.pixelHeight = g.pixelHeight;
    shared.density = g.density;
    // (SOLAR_GUI_DENSITY=2: as on a high-density screen, for timing what that costs without one.)
    if (const char* forced = std::getenv("SOLAR_GUI_DENSITY")) {
      const float d = static_cast<float>(std::atof(forced));
      if (d >= 1) {
        shared.density = d;
        shared.pixelWidth = static_cast<int>(g.width * d);
        shared.pixelHeight = static_cast<int>(g.height * d);
      }
    }
  };
  applyGeometry(shared.initialGeometry);
  while (running && !shared.quit.load()) {
    const bool animating = solar::media::PlayingCount() > 0 || gliding || (std::chrono::duration<double>(Clock::now() - scrollActive).count() < 1.5 && frame.maxY > 0.5);
    // (It waits for the window, the page, or the test script to tell it something, or for a time to pass: a frame is due only while something moves.)
    if (!dirty && !animating) shared.WaitCompositor(std::chrono::milliseconds(50));
    else if (stalledForBuffer) shared.WaitCompositor(std::chrono::milliseconds(4));
    for (Shared::CompositorInput& input : shared.TakeCompositor()) {
      if (input.kind == Shared::CompositorInput::Kind::Released) {
        if (input.index < slots.size()) slots[input.index].held = false;
        continue;
      }
      if (input.kind == Shared::CompositorInput::Kind::Geometry) {
        applyGeometry(input.geometry);
        UiEvent forward;
        forward.event.type = SDL_EVENT_WINDOW_RESIZED;
        shared.PushEvent(std::move(forward));
        dirty = true;
        continue;
      }
      UiEvent& incoming = input.event;
      SDL_Event& event = incoming.event;
      if (event.type == SDL_EVENT_TEXT_INPUT) event.text.text = incoming.text.c_str();
      if (incoming.mods >= 0) shared.mods = incoming.mods;
      else if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) shared.mods = event.key.mod;
      switch (event.type) {
        case SDL_EVENT_MOUSE_WHEEL: {
          const double stepX = -event.wheel.x * 60, stepY = -event.wheel.y * 60;
          // A frame under the pointer that can be scrolled that way takes the wheel (the page of the frame scrolls it); at its end, the page has it.
          bool frameTakes = false;
          for (auto it = frame.regions.rbegin(); it != frame.regions.rend() && !frameTakes; ++it) {
            const double px = event.wheel.mouse_x + (it->fixed ? 0 : x), py = event.wheel.mouse_y + (it->fixed ? 0 : y);
            if (px < it->x || py < it->y || px >= it->x + it->width || py >= it->y + it->height) continue;
            const bool moreX = stepX > 0 ? it->curX < it->maxX : stepX < 0 && it->curX > 0;
            const bool moreY = stepY > 0 ? it->curY < it->maxY : stepY < 0 && it->curY > 0;
            frameTakes = moreX || moreY;
          }
          if (!frameTakes) scrollBy(stepX, stepY, false);
          // (The page hears of it too, for what it does with the wheel besides scrolling; it cannot hold the scroll back.)
          shared.PushEvent(incoming);
          break;
        }
        case SDL_EVENT_TEXT_INPUT:
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
          // (A key that pastes comes with the clipboard the window had.)
          shared.PushEvent(incoming);
          break;
        }
        case SDL_EVENT_MOUSE_MOTION: {
          const int ww = shared.width.load();
          if (g_pointerLocked.load()) {
            shared.PushEvent(incoming);
            break;
          }
          if (barDrag) {
            Bar bar;
            if (barOf(bar) && bar.track > bar.height) {
              const double ny = std::clamp((event.motion.y - barGrab) / (bar.track - bar.height), 0.0, 1.0) * bar.maxY;
              scrollTo(x, ny);
              scrollActive = Clock::now();
              shared.scrollInputNs = NowNs();
            }
            break;
          }
          const bool over = event.motion.x >= ww - 16;
          if (over != barHover) {
            barHover = over;
            scrollActive = Clock::now();
            dirty = true;
          }
          shared.PushEvent(incoming);
          break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
          const int ww = shared.width.load(), wh = shared.height.load();
          if (g_pointerLocked.load()) {
            shared.PushEvent(incoming);
            break;
          }
          Bar bar;
          if (event.button.button == SDL_BUTTON_LEFT && event.button.x >= ww - 16 && barOf(bar)) {
            if (event.button.y >= bar.top && event.button.y <= bar.top + bar.height) {
              barDrag = true;
              barGrab = event.button.y - bar.top;
            } else {
              scrollBy(0, event.button.y < bar.top ? -wh * 0.9 : wh * 0.9, false);
            }
            dirty = true;
            break;
          }
          shared.PushEvent(incoming);
          break;
        }
        case SDL_EVENT_MOUSE_BUTTON_UP: {
          if (g_pointerLocked.load()) {
            shared.PushEvent(incoming);
            break;
          }
          if (barDrag) {
            barDrag = false;
            dirty = true;
            break;
          }
          shared.PushEvent(incoming);
          break;
        }
        default: break;
      }
    }
    for (UiCommand& command : shared.TakeCommands()) {
      switch (command.kind) {
        case UiCommand::Kind::Title: link.SendRaw(static_cast<uint8_t>(wire::ToBrowser::Title), command.text); break;
        case UiCommand::Kind::ScrollBy: scrollBy(command.x, command.y, command.instant); break;
        case UiCommand::Kind::ScrollTo: scrollTo(command.x, command.y); break;
        case UiCommand::Kind::Forget: painter.Forget(); break;
        case UiCommand::Kind::Shot:
          shotPath = command.text;
          // (SOLAR_GUI_SHOTAT=y: the list the page made is drawn at that scroll, as the window does while the page is busy.)
          if (const char* at = std::getenv("SOLAR_GUI_SHOTAT")) y = targetY = std::atof(at);
          dirty = true;
          break;
        case UiCommand::Kind::Quit:
          link.SendRaw(static_cast<uint8_t>(wire::ToBrowser::Quit), "");
          running = false;
          break;
        case UiCommand::Kind::Clipboard: link.SendRaw(static_cast<uint8_t>(wire::ToBrowser::Clipboard), command.text); break;
        case UiCommand::Kind::Fullscreen: link.SendRaw(static_cast<uint8_t>(wire::ToBrowser::Fullscreen), std::string(1, command.instant ? 1 : 0)); break;
        case UiCommand::Kind::PointerLock: link.SendRaw(static_cast<uint8_t>(wire::ToBrowser::PointerLock), std::string(1, command.instant ? 1 : 0)); break;
      }
    }
    if (Frame next; shared.TakeFrame(next)) {
      frame = std::move(next);
      // (What can be scrolled to is what the page says it is now.)
      clampTargets();
      if (y > std::max(0.0, frame.maxY) || x > std::max(0.0, frame.maxX)) scrollTo(x, y);
      dirty = true;
    }
    if (gliding) {
      const auto now = Clock::now();
      const double dt = std::chrono::duration<double>(now - lastGlide).count();
      lastGlide = now;
      const double k = 1 - std::exp(-dt / 0.07);
      x += (targetX - x) * k;
      y += (targetY - y) * k;
      if (std::abs(targetX - x) < 0.5 && std::abs(targetY - y) < 0.5) {
        x = targetX;
        y = targetY;
        gliding = false;
      }
      publish();
      dirty = true;
    }
    if (!dirty && !animating) continue;
    dirty = false;

    const int pixelW = shared.pixelWidth.load(), pixelH = shared.pixelHeight.load();
    if (pixelW <= 0 || pixelH <= 0) continue;
    // A picture of the right size that the window is not showing; one of another size that it does not have is let go of.
    Slot* slot = nullptr;
    size_t slotIndex = 0;
    for (size_t i = 0; i < slots.size(); ++i) {
      Slot& s = slots[i];
      if (s.held) continue;
      if (s.memory.data() && (s.width != pixelW || s.height != pixelH)) {
        s.memory.Reset();
        s.announced = false;
      }
      if (s.memory.data() && !slot) {
        slot = &s;
        slotIndex = i;
      }
    }
    if (!slot) {
      for (size_t i = 0; i < slots.size() && !slot; ++i) {
        if (slots[i].held) continue;
        Slot& s = slots[i];
        const size_t stride = static_cast<size_t>(pixelW) * 4;
        if (!s.memory.Create(stride * static_cast<size_t>(pixelH))) continue;
        s.width = pixelW;
        s.height = pixelH;
        s.stride = static_cast<int>(stride);
        slot = &s;
        slotIndex = i;
      }
    }
    if (!slot) {
      // (The window has all of them: this frame waits for one it lets go of.)
      stalledForBuffer = true;
      dirty = true;
      continue;
    }
    stalledForBuffer = false;
    const auto d0 = Clock::now();
    sk_sp<SkSurface> surface = SkSurfaces::WrapPixels(SkImageInfo::MakeN32Premul(pixelW, pixelH), slot->memory.data(), static_cast<size_t>(slot->stride));
    if (!surface) continue;
    SkCanvas* canvas = surface->getCanvas();
    canvas->resetMatrix();
    canvas->clear(SK_ColorWHITE);
    const float density = shared.density.load();
    canvas->scale(density, density);
    const auto rasterStart = Clock::now();
    if (frame.list) painter.Draw(*frame.list, *canvas, x, y);
    const auto rasterEnd = Clock::now();
    {
      Bar bar;
      const double idle = std::chrono::duration<double>(Clock::now() - scrollActive).count();
      const double alpha = barHover || barDrag ? 1.0 : std::clamp(1.0 - (idle - 1.0) / 0.4, 0.0, 1.0);
      const int ww = shared.width.load(), wh = shared.height.load();
      if (alpha > 0 && barOf(bar)) {
        const float width = barHover || barDrag ? 10.f : 6.f;
        SkPaint paint;
        paint.setAntiAlias(true);
        if (barHover || barDrag) {
          paint.setColor(SkColorSetARGB(40, 0, 0, 0));
          canvas->drawRect(SkRect::MakeXYWH(ww - width - 4, 0, width + 4, wh), paint);
        }
        paint.setColor(SkColorSetARGB(static_cast<U8CPU>((barDrag ? 170 : 120) * alpha), 0, 0, 0));
        canvas->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(ww - width - 2, bar.top + 2, width, bar.height - 4), width / 2, width / 2), paint);
        if (alpha < 1.0 || idle < 1.4) dirty = true;
      }
    }
    SkPixmap pixmap;
    surface->peekPixels(&pixmap);
    const auto uploadStart = Clock::now();
    // The window is told of the picture (the first time of the memory with it), and shows it.
    if (!slot->announced) {
      Writer out;
      wire::NewBuffer announce{static_cast<uint32_t>(slotIndex), slot->width, slot->height, slot->stride};
      wire::Write(out, announce);
      if (!link.SendRaw(static_cast<uint8_t>(wire::ToBrowser::NewBuffer), out.bytes(), slot->memory.fd())) running = false;
      slot->announced = true;
    }
    {
      Writer out;
      wire::Write(out, wire::Frame{static_cast<uint32_t>(slotIndex), ++frameSerial});
      slot->held = true;
      if (!link.SendRaw(static_cast<uint8_t>(wire::ToBrowser::Frame), out.bytes())) running = false;
    }
    const auto uploadEnd = Clock::now();
    if (std::getenv("SOLAR_GUI_TIMES")) {
      // (Where a frame goes, in ms: the page painted onto the surface by the CPU, the surface sent to the texture, and the present, which waits for vsync.)
      const auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
      static int count = 0;
      static double rasterSum = 0, rasterMax = 0, uploadSum = 0, presentSum = 0;
      const double raster = ms(rasterStart, rasterEnd);
      rasterSum += raster;
      rasterMax = std::max(rasterMax, raster);
      uploadSum += ms(uploadStart, uploadEnd);
      presentSum += ms(uploadEnd, Clock::now());
      if (++count % 30 == 0 || !shotPath.empty()) {
        const int n = count % 30 == 0 ? 30 : count % 30;
        std::fprintf(stderr, "window: draw %.1f ms (scroll %.0f); over %d frames, per frame: raster %.2f ms (worst %.2f), upload %.2f ms, present %.2f ms; %dx%d px\n", ms(d0, Clock::now()), y, n, rasterSum / n, rasterMax, uploadSum / n, presentSum / n, pixelW, pixelH);
        rasterSum = rasterMax = uploadSum = presentSum = 0;
      }
    }
    // SOLAR_GUI_SHOT=file.png: the frame is written to a file, and the program ends (for looking at what it paints without a screen).
    if (!shotPath.empty()) {
      SkDynamicMemoryWStream out;
      if (pixmap.addr() && SkPngEncoder::Encode(&out, pixmap, SkPngEncoder::Options())) {
        sk_sp<SkData> png = out.detachAsData();
        std::ofstream file(shotPath, std::ios::binary);
        file.write(static_cast<const char*>(png->data()), static_cast<std::streamsize>(png->size()));
      }
      running = false;
    }
  }
  link.SendRaw(static_cast<uint8_t>(wire::ToBrowser::Quit), "");
  shared.quit = true;
  shared.WakePage();
}

UiEvent ToUiEvent(const wire::Input& in) {
  UiEvent out;
  SDL_Event& event = out.event;
  switch (in.kind) {
    case wire::Input::Kind::KeyDown:
    case wire::Input::Kind::KeyUp:
      event.type = in.kind == wire::Input::Kind::KeyDown ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
      event.key.key = in.key;
      event.key.mod = in.mod;
      event.key.repeat = in.repeat;
      event.key.down = in.kind == wire::Input::Kind::KeyDown;
      break;
    case wire::Input::Kind::Text:
      event.type = SDL_EVENT_TEXT_INPUT;
      out.text = in.text;
      break;
    case wire::Input::Kind::Motion:
      event.type = SDL_EVENT_MOUSE_MOTION;
      event.motion.x = in.x;
      event.motion.y = in.y;
      event.motion.xrel = in.xrel;
      event.motion.yrel = in.yrel;
      break;
    case wire::Input::Kind::ButtonDown:
    case wire::Input::Kind::ButtonUp:
      event.type = in.kind == wire::Input::Kind::ButtonDown ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
      event.button.button = in.button;
      event.button.down = in.kind == wire::Input::Kind::ButtonDown;
      event.button.clicks = 1;
      event.button.x = in.x;
      event.button.y = in.y;
      break;
    case wire::Input::Kind::Wheel:
      event.type = SDL_EVENT_MOUSE_WHEEL;
      event.wheel.x = in.x;
      event.wheel.y = in.y;
      event.wheel.mouse_x = in.xrel;
      event.wheel.mouse_y = in.yrel;
      break;
  }
  out.clipboard = in.clipboard;
  out.mods = in.mod;
  return out;
}

std::thread StartWindowLink(Shared& shared, net::ipc::Channel& link) {
  return std::thread([&shared, &link] {
    uint8_t type = 0;
    std::string bytes;
    while (link.ReceiveRaw(type, bytes)) {
      net::ipc::Reader in(bytes);
      switch (static_cast<wire::ToRenderer>(type)) {
        case wire::ToRenderer::Resize: {
          Shared::CompositorInput input;
          input.kind = Shared::CompositorInput::Kind::Geometry;
          if (wire::Read(in, input.geometry)) shared.PushCompositor(std::move(input));
          break;
        }
        case wire::ToRenderer::Input: {
          wire::Input read;
          if (!wire::Read(in, read)) break;
          Shared::CompositorInput input;
          input.event = ToUiEvent(read);
          shared.PushCompositor(std::move(input));
          break;
        }
        case wire::ToRenderer::BufferReleased: {
          Shared::CompositorInput input;
          input.kind = Shared::CompositorInput::Kind::Released;
          input.index = in.U32();
          if (in.ok()) shared.PushCompositor(std::move(input));
          break;
        }
        case wire::ToRenderer::NavHead:
        case wire::ToRenderer::NavBody:
        case wire::ToRenderer::NavEnd:
        case wire::ToRenderer::NavError:
        case wire::ToRenderer::NavRestart:
        case wire::ToRenderer::NavTransferred:
        case wire::ToRenderer::GoTo: shared.DeliverNavigation(static_cast<wire::ToRenderer>(type), bytes); break;
        case wire::ToRenderer::AudioQueued: {
          const uint32_t id = in.U32();
          const uint64_t frames = in.U64();
          if (in.ok()) DeliverAudioQueued(id, frames);
          break;
        }
        case wire::ToRenderer::StorageReply: DeliverStorageReply(bytes); break;
        case wire::ToRenderer::Shutdown: shared.quit = true; break;
        default: break;  // (A message that is none is not taken.)
      }
    }
    // The window is gone: the page has nothing to show to.
    shared.quit = true;
    shared.WakeWindow();
    shared.WakePage();
  });
}

}  // namespace solar::gui
