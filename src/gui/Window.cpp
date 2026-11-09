/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/gui/Window.h"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "solar/gui/Chrome.h"
#include "solar/gui/FrameBuffer.h"
#include "solar/gui/Settings.h"
#include "solar/gui/SdlAudio.h"
#include "solar/gui/Wire.h"

namespace solar::gui {

using net::ipc::Writer;

namespace {

// What is in the window's inbox: what a page's process said, or (with a type that is none of the wire's) that one is gone or is shown now.
struct Message {
  uint32_t host = 0;
  uint8_t type = 0;
  std::string bytes;
  int fd = -1;
};
constexpr uint8_t kGone = 0xFE, kShown = 0xFD;

// What the window knows of one page's process: the pictures it can make, mapped to be read.
struct View {
  std::map<uint32_t, SharedMemory> buffers;
  std::map<uint32_t, wire::NewBuffer> shapes;
  int64_t lastFrame = -1;  // the picture the page made last (shown again when its tab is)
};

// Where the window may be taken hold of to be moved, and its edges to be resized, when the system's bar is not there: the interface says which part of the strip of the tabs is empty.
struct HitState {
  std::mutex mutex;
  int dragLeft = 0, dragRight = 0, dragBottom = kTabStripHeight;
  bool borderless = false, maximized = false, fullscreen = false;
};
HitState g_hit;

SDL_HitTestResult HitTest(SDL_Window* window, const SDL_Point* point, void*) {
  std::lock_guard<std::mutex> lock(g_hit.mutex);
  if (!g_hit.borderless) return SDL_HITTEST_NORMAL;
  int w = 0, h = 0;
  SDL_GetWindowSize(window, &w, &h);
  if (!g_hit.maximized && !g_hit.fullscreen) {
    constexpr int kEdge = 6;
    const bool left = point->x < kEdge, right = point->x >= w - kEdge, top = point->y < kEdge, bottom = point->y >= h - kEdge;
    if (top && left) return SDL_HITTEST_RESIZE_TOPLEFT;
    if (top && right) return SDL_HITTEST_RESIZE_TOPRIGHT;
    if (bottom && left) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
    if (bottom && right) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    if (top) return SDL_HITTEST_RESIZE_TOP;
    if (bottom) return SDL_HITTEST_RESIZE_BOTTOM;
    if (left) return SDL_HITTEST_RESIZE_LEFT;
    if (right) return SDL_HITTEST_RESIZE_RIGHT;
  }
  if (!g_hit.fullscreen && point->y < g_hit.dragBottom && point->x >= g_hit.dragLeft && point->x < g_hit.dragRight) return SDL_HITTEST_DRAGGABLE;
  return SDL_HITTEST_NORMAL;
}

}  // namespace

int RunWindow(SDL_Window* window, SDL_Renderer* renderer, BrowserServices& services, Launcher launcher, bool allowFiles, const std::string& address) {
  std::mutex mutex;
  std::deque<Message> inbox;
  const auto push = [&](Message message) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      inbox.push_back(std::move(message));
    }
    SDL_Event wake{};
    wake.type = SDL_EVENT_USER;
    SDL_PushEvent(&wake);
  };
  Session::Events events;
  events.message = [&](uint32_t host, uint8_t type, std::string bytes, int fd) { push(Message{host, type, std::move(bytes), fd}); };
  events.gone = [&](uint32_t host, bool fatal) { push(Message{host, kGone, std::string(1, fatal ? 1 : 0), -1}); };
  events.shown = [&](uint32_t host) { push(Message{host, kShown, "", -1}); };
  Session session(services, std::move(launcher), std::move(events), allowFiles);

  std::map<uint32_t, View> views;
  // The sound of the pages: a stream of the sound device for each a page opened (a few at most), played as the page sends it; how much is not yet played is told to the page now and then.
  // (Only the page that is shown has sound.)
  struct Stream {
    std::unique_ptr<media::AudioOutput> output;
    int rate = 48000, channels = 2;
  };
  std::map<uint64_t, Stream> audio;
  SDL_TimerID audioTimer = 0;
  constexpr size_t kMostStreams = 16;
  const auto streamKey = [](uint32_t host, uint32_t id) { return (static_cast<uint64_t>(host) << 32) | id; };
  SDL_Texture* texture = nullptr;
  int textureW = 0, textureH = 0;
  // The interface (the bar) is a page of its own: its picture is above the page's. It is not shown while a page is full screen.
  SDL_Texture* barTexture = nullptr;
  int barTextureW = 0, barTextureH = 0;
  const bool frameLog = std::getenv("SOLAR_FRAME_LOG") != nullptr;  // (the time between the pictures of the page, for looking at how smooth it is)
  auto lastFrame = std::chrono::steady_clock::now();
  bool fullscreen = false, barFocused = false;
  // The interface may be taller than its bar for a while (a menu that comes down over the page): how many points more.
  int overlay = 0;
  // Where the tabs are: along the top (the bar is the top of the window, 88 points), or in a column at the side (the bar of the address is 48 points high at the top, and the column 240 wide at the left; the
  // picture of the interface is then the whole window, clear where the page is).
  const auto layoutOf = [](const std::string& name) { return name == "sidebar" ? 2 : name == "side" ? 1 : 0; };
  int layout = layoutOf(Settings::Instance().Get("tabs"));  // 0: the tabs along the top, 1: in a column with the bar at the top, 2: everything in the column
  bool sideTabs = layout != 0;  // (the interface has the whole window)
  const bool noBar = std::getenv("SOLAR_NO_CHROME") != nullptr;
  // (The window has no bar of the system's by default; the interface draws the buttons of the window then. SOLAR_SYSTEM_TITLE=1 gives it the system's.)
  const bool borderless = (SDL_GetWindowFlags(window) & SDL_WINDOW_BORDERLESS) != 0;
  std::map<uint32_t, std::string> titles;
  {
    std::lock_guard<std::mutex> lock(g_hit.mutex);
    g_hit.borderless = borderless;
  }
  if (borderless) SDL_SetWindowHitTest(window, HitTest, nullptr);
  // The mouse button that is down belongs to the one it went down on (the bar or the page) until it is up.
  int capture = 0;  // 0: none, 1: the bar, 2: the page
  float lockedAtX = 0, lockedAtY = 0;
  bool running = true;

  // The height of the bar in pixels (what it is in points, at the density of the window).
  const auto chromeVisible = [&] { return !fullscreen && !noBar; };
  const auto barPoints = [&] { return !chromeVisible() ? 0 : layout == 0 ? kChromeHeight : layout == 1 ? kToolbarHeight : 0; };
  const auto leftPoints = [&] { return !chromeVisible() ? 0 : layout == 1 ? kSideWidth : layout == 2 ? kSidebarWidth : 0; };
  const auto barPixels = [&] { return static_cast<int>(std::lround(barPoints() * SDL_GetWindowPixelDensity(window))); };
  const auto leftPixels = [&] { return static_cast<int>(std::lround(leftPoints() * SDL_GetWindowPixelDensity(window))); };
  const auto dark = [] { return Settings::Instance().Get("theme") == "dark"; };  // ("system" is light until the system's colours are read)
  // What the interface is told its size is: the bar (and what comes down from it), or the whole window with the tabs at the side.
  const auto chromeGeometry = [&] {
    int w = 0, h = 0, pw = 0, ph = 0;
    SDL_GetWindowSize(window, &w, &h);
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    const float density = SDL_GetWindowPixelDensity(window);
    Writer out;
    if (sideTabs) {
      wire::Write(out, wire::Geometry{w, std::max(1, h), pw, std::max(1, ph), density, dark()});
    } else {
      const int extra = barPoints() > 0 ? overlay : 0;
      wire::Write(out, wire::Geometry{w, kChromeHeight + extra, pw, static_cast<int>(std::lround((kChromeHeight + extra) * density)), density, dark()});
    }
    return out.bytes();
  };
  const auto chromeAddress = [&] { return std::string(layout == 2 ? kSidebarAddress : layout == 1 ? kSideTabsAddress : kChromeAddress); };
  const auto compose = [&] {
    if (dark()) SDL_SetRenderDrawColor(renderer, 21, 23, 28, 255);
    else SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderClear(renderer);
    int outW = 0, outH = 0;
    SDL_GetRenderOutputSize(renderer, &outW, &outH);
    const int bar = std::min(barPixels(), outH), left = std::min(leftPixels(), outW);
    const bool showChrome = chromeVisible();
    if (texture) {
      const SDL_FRect page{static_cast<float>(left), static_cast<float>(bar), static_cast<float>(outW - left), static_cast<float>(outH - bar)};
      SDL_RenderTexture(renderer, texture, nullptr, &page);
    }
    if (barTexture && showChrome) {
      // (The interface's picture is as large as the interface was told it is: laid over the window from its corner.)
      const float height = sideTabs ? static_cast<float>(outH) : barTextureW > 0 ? static_cast<float>(barTextureH) * static_cast<float>(outW) / static_cast<float>(barTextureW) : static_cast<float>(bar);
      const SDL_FRect top{0, 0, static_cast<float>(outW), height};
      SDL_RenderTexture(renderer, barTexture, nullptr, &top);
    }
  };
  const auto present = [&] {
    compose();
    SDL_RenderPresent(renderer);
  };
  const auto sendGeometry = [&] {
    int w = 0, h = 0, pw = 0, ph = 0;
    SDL_GetWindowSize(window, &w, &h);
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    const float density = SDL_GetWindowPixelDensity(window);
    const int bar = barPixels(), barPts = barPoints(), left = leftPixels(), leftPts = leftPoints();
    Writer out;
    wire::Write(out, wire::Geometry{std::max(1, w - leftPts), std::max(1, h - barPts), std::max(1, pw - left), std::max(1, ph - bar), density, dark()});
    session.Broadcast(wire::ToRenderer::Resize, out.bytes());
    session.SendChrome(wire::ToRenderer::Resize, chromeGeometry());
  };
  const auto stopAudioOf = [&](uint32_t host) {
    for (auto it = audio.begin(); it != audio.end();) it = (it->first >> 32) == host ? audio.erase(it) : std::next(it);
    if (audio.empty() && audioTimer) {
      SDL_RemoveTimer(audioTimer);
      audioTimer = 0;
    }
  };

  const auto handle = [&](Message& message) {
    if (message.type == kGone) {
      views.erase(message.host);
      titles.erase(message.host);
      stopAudioOf(message.host);
      if (!message.bytes.empty() && message.bytes[0] != 0) running = false;
      return;
    }
    if (message.type == kShown) {
      // (The sound of the page that was shown stops with it.)
      for (auto it = audio.begin(); it != audio.end();) it = (it->first >> 32) != message.host ? audio.erase(it) : std::next(it);
      if (audio.empty() && audioTimer) {
        SDL_RemoveTimer(audioTimer);
        audioTimer = 0;
      }
      // The tab that is shown has its page's last picture put up at once (a tab that has no page yet shows nothing), and its title.
      const auto view = views.find(message.host);
      if (message.host == 0 || view == views.end() || view->second.lastFrame < 0) {
        if (texture) SDL_DestroyTexture(texture);
        texture = nullptr;
      } else {
        const auto buffer = view->second.buffers.find(static_cast<uint32_t>(view->second.lastFrame));
        const auto shape = view->second.shapes.find(static_cast<uint32_t>(view->second.lastFrame));
        if (buffer != view->second.buffers.end() && shape != view->second.shapes.end()) {
          if (!texture || textureW != shape->second.width || textureH != shape->second.height) {
            if (texture) SDL_DestroyTexture(texture);
            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, shape->second.width, shape->second.height);
            textureW = shape->second.width;
            textureH = shape->second.height;
          }
          if (texture) SDL_UpdateTexture(texture, nullptr, buffer->second.data(), shape->second.stride);
        }
      }
      const auto title = titles.find(message.host);
      SDL_SetWindowTitle(window, title == titles.end() || title->second.empty() ? "Solar" : title->second.c_str());
      present();
      return;
    }
    // (A descriptor comes only with a buffer: the one that came with another message is closed.)
    int fd = message.fd;
    message.fd = -1;
    const auto type = static_cast<wire::ToBrowser>(message.type);
    if (type != wire::ToBrowser::NewBuffer && fd >= 0) {
      ::close(fd);
      fd = -1;
    }
    // What is the session's to answer: navigations, commits, the history, the page's storage.
    if (session.Handle(message.host, message.type, message.bytes)) return;
    const bool shown = session.current() == message.host;
    net::ipc::Reader in(message.bytes);
    switch (type) {
      case wire::ToBrowser::NewBuffer: {
        wire::NewBuffer shape;
        if (!wire::Read(in, shape)) {
          if (fd >= 0) ::close(fd);
          break;
        }
        SharedMemory memory;
        if (!memory.Adopt(fd, static_cast<size_t>(shape.stride) * static_cast<size_t>(shape.height), false)) break;
        View& view = views[message.host];
        view.buffers[shape.index] = std::move(memory);
        view.shapes[shape.index] = shape;
        break;
      }
      case wire::ToBrowser::Frame: {
        wire::Frame frame;
        if (!wire::Read(in, frame)) break;
        View& view = views[message.host];
        const auto buffer = view.buffers.find(frame.index);
        if (buffer == view.buffers.end()) break;
        view.lastFrame = frame.index;
        if (message.host == session.chrome()) {
          const wire::NewBuffer& shape = view.shapes[frame.index];
          if (!barTexture || barTextureW != shape.width || barTextureH != shape.height) {
            if (barTexture) SDL_DestroyTexture(barTexture);
            barTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, shape.width, shape.height);
            barTextureW = shape.width;
            barTextureH = shape.height;
            if (barTexture) SDL_SetTextureBlendMode(barTexture, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
          }
          if (barTexture) {
            SDL_UpdateTexture(barTexture, nullptr, buffer->second.data(), shape.stride);
            present();
          }
        } else if (shown) {
          if (frameLog) {
            const auto now = std::chrono::steady_clock::now();
            std::fprintf(stderr, "frame +%.1f ms at %.0f\n", std::chrono::duration<double, std::milli>(now - lastFrame).count(), std::chrono::duration<double, std::milli>(now.time_since_epoch()).count());
            lastFrame = now;
          }
          const wire::NewBuffer& shape = view.shapes[frame.index];
          if (!texture || textureW != shape.width || textureH != shape.height) {
            if (texture) SDL_DestroyTexture(texture);
            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, shape.width, shape.height);
            textureW = shape.width;
            textureH = shape.height;
          }
          if (texture) {
            SDL_UpdateTexture(texture, nullptr, buffer->second.data(), shape.stride);
            present();
          }
        }
        // (It is copied, or not wanted: the page may draw into it again.)
        Writer out;
        out.U32(frame.index);
        session.Send(message.host, wire::ToRenderer::BufferReleased, out.bytes());
        break;
      }
      // (What the page that is not shown asks of the window is not done: it is not the user's page yet.)
      case wire::ToBrowser::Title:
        titles[message.host] = message.bytes.substr(0, 4096);
        session.SetTitle(message.host, titles[message.host]);
        if (shown) SDL_SetWindowTitle(window, titles[message.host].c_str());
        break;
      case wire::ToBrowser::Clipboard:
        if (shown) SDL_SetClipboardText(message.bytes.c_str());
        break;
      case wire::ToBrowser::Fullscreen:
        if (shown) {
          fullscreen = !message.bytes.empty() && message.bytes[0] != 0;
          {
            std::lock_guard<std::mutex> lock(g_hit.mutex);
            g_hit.fullscreen = fullscreen;
          }
          SDL_SetWindowFullscreen(window, fullscreen);
          sendGeometry();
        }
        break;
      case wire::ToBrowser::PointerLock:
        if (!shown) break;
        if (!message.bytes.empty() && message.bytes[0] != 0) {
          SDL_GetMouseState(&lockedAtX, &lockedAtY);
          SDL_SetWindowRelativeMouseMode(window, true);
        } else {
          SDL_SetWindowRelativeMouseMode(window, false);
          SDL_WarpMouseInWindow(window, lockedAtX, lockedAtY);
        }
        break;
      case wire::ToBrowser::AudioOpen: {
        const uint32_t id = in.U32();
        const int rate = in.I32(), channels = in.I32();
        if (!shown || !in.ok() || audio.size() >= kMostStreams || audio.count(streamKey(message.host, id)) || rate < 8000 || rate > 192000 || channels < 1 || channels > 8) break;
        Stream stream;
        stream.output = MakeSdlAudioOutput();
        if (!stream.output->Start(rate, channels)) break;
        stream.rate = rate;
        stream.channels = channels;
        audio[streamKey(message.host, id)] = std::move(stream);
        if (!audioTimer) {
          // (Every so often the window tells the page how much sound it holds: a timer on another thread asks for it.)
          audioTimer = SDL_AddTimer(15, [](void*, SDL_TimerID, Uint32 interval) -> Uint32 {
            SDL_Event tick{};
            tick.type = SDL_EVENT_USER;
            tick.user.code = 1;
            SDL_PushEvent(&tick);
            return interval;
          }, nullptr);
        }
        break;
      }
      case wire::ToBrowser::AudioData: {
        const uint32_t id = in.U32();
        const std::string samples = in.String(size_t{4} << 20);
        const auto stream = audio.find(streamKey(message.host, id));
        if (!in.ok() || stream == audio.end()) break;
        const size_t frameBytes = sizeof(float) * static_cast<size_t>(stream->second.channels);
        const size_t frames = samples.size() / frameBytes;
        // (A page that sends sound faster than it is played is not let fill the memory: what is more than a few seconds ahead is dropped.)
        if (frames == 0 || stream->second.output->QueuedFrames() > static_cast<size_t>(stream->second.rate) * 5) break;
        std::vector<float> aligned(frames * static_cast<size_t>(stream->second.channels));
        std::memcpy(aligned.data(), samples.data(), aligned.size() * sizeof(float));
        stream->second.output->Queue(aligned.data(), frames);
        break;
      }
      case wire::ToBrowser::AudioPause: {
        const uint32_t id = in.U32();
        const bool paused = in.Bool();
        const auto stream = audio.find(streamKey(message.host, id));
        if (in.ok() && stream != audio.end()) stream->second.output->Pause(paused);
        break;
      }
      case wire::ToBrowser::AudioClear: {
        const uint32_t id = in.U32();
        const auto stream = audio.find(streamKey(message.host, id));
        if (in.ok() && stream != audio.end()) stream->second.output->Clear();
        break;
      }
      case wire::ToBrowser::AudioClose: {
        const uint32_t id = in.U32();
        if (!in.ok()) break;
        audio.erase(streamKey(message.host, id));
        if (audio.empty() && audioTimer) {
          SDL_RemoveTimer(audioTimer);
          audioTimer = 0;
        }
        break;
      }
      case wire::ToBrowser::UiCommand: {
        // What the interface asks (only its own process is heard).
        const bool fromBar = message.host == session.chrome(), fromSettings = !fromBar && session.IsSettings(message.host);
        if (!fromBar && !fromSettings) break;
        const std::string& text = message.bytes;
        const size_t tab = text.find('\t');
        const std::string verb = text.substr(0, tab), rest = tab == std::string::npos ? "" : text.substr(tab + 1);
        if (fromSettings) {
          // The settings page may read and change the settings, and do nothing else.
          if (verb == "ready") {
            session.Send(message.host, wire::ToRenderer::UiState, Settings::Instance().Json());
          } else if (verb == "set") {
            const size_t second = rest.find('\t');
            if (second != std::string::npos && Settings::Instance().Set(rest.substr(0, second), rest.substr(second + 1))) {
              session.Send(message.host, wire::ToRenderer::UiState, Settings::Instance().Json());
              // The tabs are moved to the other layout at once: another interface is started for it, and the pages are told how large they are now.
              // The colours too: the pages and the interface are told (they are given the size again, with the colours the user wants).
              if (rest.rfind("theme\t", 0) == 0) {
                sendGeometry();
                present();
              }
              const int wantLayout = layoutOf(Settings::Instance().Get("tabs"));
              if (wantLayout != layout) {
                layout = wantLayout;
                sideTabs = layout != 0;
                overlay = 0;
                barFocused = false;
                capture = 0;
                if (barTexture) SDL_DestroyTexture(barTexture);
                barTexture = nullptr;
                if (!noBar) session.OpenChrome(chromeGeometry(), chromeAddress());
                sendGeometry();
                present();
              } else {
                session.SendChrome(wire::ToRenderer::UiState, Settings::Instance().Json());
              }
            }
          }
          break;
        }
        if (verb == "go") {
          const std::string where = AddressFromInput(rest, Settings::Instance().SearchAddress());
          barFocused = false;
          if (!where.empty()) session.Go(where);
        } else if (verb == "back") session.Back();
        else if (verb == "forward") session.Forward();
        else if (verb == "reload") session.Reload();
        else if (verb == "focus") barFocused = rest == "1";
        else if (verb == "ready") {
          session.PushState();
          session.SendChrome(wire::ToRenderer::UiState, Settings::Instance().Json());
        } else if (verb == "settings") session.NewTab(kSettingsAddress);
        else if (verb == "overlay") {
          // The interface asks to be taller (a menu) or to be as it was: it takes the keys while it is, and the window tells the pages and it how large they are.
          const int wanted = std::clamp(std::atoi(rest.c_str()), 0, 600);
          if (wanted != overlay) {
            overlay = wanted;
            barFocused = wanted > 0;
            if (!sideTabs) sendGeometry();
          }
        }
        else if (verb == "newtab") {
          session.NewTab("");
          if (!noBar) {
            barFocused = true;
            session.SendChrome(wire::ToRenderer::UiState, "{\"focusAddress\":true}");
          }
        } else if (verb == "activate" || verb == "close") {
          const int number = std::atoi(rest.c_str());
          if (number > 0) {
            if (verb == "activate") session.Activate(static_cast<uint32_t>(number));
            else session.CloseTab(static_cast<uint32_t>(number));
            barFocused = false;
          }
        } else if (verb == "move") {
          // A tab is put at another place (by the interface, which has it dragged).
          const size_t second = rest.find('\t');
          const int number = std::atoi(rest.c_str()), place = second == std::string::npos ? -1 : std::atoi(rest.c_str() + second + 1);
          if (number > 0 && place >= 0) session.MoveTab(static_cast<uint32_t>(number), static_cast<size_t>(place));
        } else if (verb == "pin") {
          const size_t second = rest.find('\t');
          const int number = std::atoi(rest.c_str());
          if (number > 0 && second != std::string::npos) session.PinTab(static_cast<uint32_t>(number), rest[second + 1] == '1');
        } else if (verb == "min") SDL_MinimizeWindow(window);
        else if (verb == "max") {
          if (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) SDL_RestoreWindow(window);
          else SDL_MaximizeWindow(window);
        } else if (verb == "quit") running = false;
        else if (verb == "drag") {
          int left = 0, right = 0, bottom = kTabStripHeight;
          if (std::sscanf(rest.c_str(), "%d,%d,%d", &left, &right, &bottom) >= 2) {
            std::lock_guard<std::mutex> lock(g_hit.mutex);
            g_hit.dragLeft = left;
            g_hit.dragRight = right;
            g_hit.dragBottom = std::clamp(bottom, 0, 200);
          }
        }
        break;
      }
      case wire::ToBrowser::Quit:
        if (shown) running = false;
        break;
      default: break;
    }
  };

  if (!noBar) session.OpenChrome(chromeGeometry(), chromeAddress());
  session.SetWindowFlags(borderless, false);
  session.Open(address);
  sendGeometry();
  // (SOLAR_BROWSER_SCRIPT="wait:ms;go:address;back;forward": what the user would do with the history and the address bar, for trying the browser's side without a window to click in.)
  std::thread script;
  if (const char* steps = std::getenv("SOLAR_BROWSER_SCRIPT")) {
    script = std::thread([&session, window, steps = std::string(steps)] {
      size_t at = 0;
      while (at < steps.size()) {
        size_t end = steps.find(';', at);
        if (end == std::string::npos) end = steps.size();
        const std::string step = steps.substr(at, end - at);
        at = end + 1;
        if (step.rfind("wait:", 0) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(step.c_str() + 5)));
        else if (step.rfind("go:", 0) == 0) session.Go(step.substr(3));
        else if (step.rfind("size:", 0) == 0) {
          // (A window that is made another size, as the user does: the pages are told as they are.)
          int w = 0, h = 0;
          std::sscanf(step.c_str() + 5, "%dx%d", &w, &h);
          if (w > 0 && h > 0) SDL_SetWindowSize(window, w, h);
        }
        else if (step.rfind("shot:", 0) == 0) {
          SDL_Event ask{};
          ask.type = SDL_EVENT_USER;
          ask.user.code = 2;
          ask.user.data1 = SDL_strdup(step.c_str() + 5);
          SDL_PushEvent(&ask);
        }
        else if (step.rfind("rclick:", 0) == 0) {
          // The right button, at x,y.
          float x = 0, y = 0;
          std::sscanf(step.c_str() + 7, "%f,%f", &x, &y);
          SDL_Event motion{};
          motion.type = SDL_EVENT_MOUSE_MOTION;
          motion.motion.x = x;
          motion.motion.y = y;
          SDL_PushEvent(&motion);
          for (const bool down : {true, false}) {
            SDL_Event press{};
            press.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            press.button.button = SDL_BUTTON_RIGHT;
            press.button.down = down;
            press.button.x = x;
            press.button.y = y;
            SDL_PushEvent(&press);
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
          }
        }
        else if (step.rfind("click:", 0) == 0 || step.rfind("move:", 0) == 0) {
          // (A click or a move of the pointer, as the user's: x,y in points.)
          float x = 0, y = 0;
          std::sscanf(step.c_str() + (step[0] == 'c' ? 6 : 5), "%f,%f", &x, &y);
          SDL_Event motion{};
          motion.type = SDL_EVENT_MOUSE_MOTION;
          motion.motion.x = x;
          motion.motion.y = y;
          SDL_PushEvent(&motion);
          if (step[0] == 'c') {
            for (const Uint32 type : {static_cast<Uint32>(SDL_EVENT_MOUSE_BUTTON_DOWN), static_cast<Uint32>(SDL_EVENT_MOUSE_BUTTON_UP)}) {
              SDL_Event press{};
              press.type = type;
              press.button.button = SDL_BUTTON_LEFT;
              press.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
              press.button.x = x;
              press.button.y = y;
              SDL_PushEvent(&press);
              std::this_thread::sleep_for(std::chrono::milliseconds(40));
            }
          }
        } else if (step.rfind("wheel:", 0) == 0) {
          // wheel:count,every_ms,dy: that many notches of the wheel (dy of the wheel's), at the pointer.
          int count = 0, every = 16;
          float dy = -1;
          std::sscanf(step.c_str() + 6, "%d,%d,%f", &count, &every, &dy);
          std::fprintf(stderr, "wheel begins at %.0f\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count());
          for (int i = 0; i < count; ++i) {
            SDL_Event wheel{};
            wheel.type = SDL_EVENT_MOUSE_WHEEL;
            wheel.wheel.y = dy;
            wheel.wheel.mouse_x = 600;
            wheel.wheel.mouse_y = 400;
            SDL_PushEvent(&wheel);
            std::this_thread::sleep_for(std::chrono::milliseconds(every));
          }
        } else if (step.rfind("drag:", 0) == 0) {
          // drag:x1,y1,x2,y2: the button goes down at the first place, the pointer goes to the second in steps, and the button is let go.
          float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
          std::sscanf(step.c_str() + 5, "%f,%f,%f,%f", &x1, &y1, &x2, &y2);
          const auto motion = [](float x, float y) {
            SDL_Event move{};
            move.type = SDL_EVENT_MOUSE_MOTION;
            move.motion.x = x;
            move.motion.y = y;
            SDL_PushEvent(&move);
          };
          const auto button = [](bool down, float x, float y) {
            SDL_Event press{};
            press.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            press.button.button = SDL_BUTTON_LEFT;
            press.button.down = down;
            press.button.x = x;
            press.button.y = y;
            SDL_PushEvent(&press);
          };
          motion(x1, y1);
          std::this_thread::sleep_for(std::chrono::milliseconds(60));
          button(true, x1, y1);
          for (int i = 1; i <= 12; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            motion(x1 + (x2 - x1) * i / 12, y1 + (y2 - y1) * i / 12);
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(60));
          button(false, x2, y2);
        } else if (step.rfind("text:", 0) == 0) {
          SDL_Event typed{};
          typed.type = SDL_EVENT_TEXT_INPUT;
          typed.text.text = SDL_strdup(step.c_str() + 5);
          SDL_PushEvent(&typed);
        } else if (step.rfind("key:", 0) == 0) {
          // key:enter, key:escape, key:ctrl+l, key:alt+left ...
          std::string name = step.substr(4);
          SDL_Keymod mods = 0;
          for (const auto& [prefix, mod] : {std::pair<const char*, SDL_Keymod>{"ctrl+", SDL_KMOD_CTRL}, {"alt+", SDL_KMOD_ALT}, {"shift+", SDL_KMOD_SHIFT}}) {
            if (name.rfind(prefix, 0) == 0) {
              mods |= mod;
              name = name.substr(std::strlen(prefix));
            }
          }
          SDL_Keycode code = name == "enter" ? SDLK_RETURN : name == "escape" ? SDLK_ESCAPE : name == "left" ? SDLK_LEFT : name == "right" ? SDLK_RIGHT : name == "backspace" ? SDLK_BACKSPACE : name == "tab" ? SDLK_TAB : name.size() == 1 ? static_cast<SDL_Keycode>(name[0]) : SDLK_UNKNOWN;
          for (const bool down : {true, false}) {
            SDL_Event key{};
            key.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            key.key.key = code;
            key.key.mod = mods;
            key.key.down = down;
            SDL_PushEvent(&key);
          }
        }
        else if (step == "back") session.Back();
        else if (step == "forward") session.Forward();
        else if (step == "entries") {
          const std::vector<std::string> entries = session.Entries();
          std::fprintf(stderr, "history (at %d):", session.Index());
          for (const std::string& entry : entries) std::fprintf(stderr, " %s", entry.c_str());
          std::fprintf(stderr, "\n");
        }
      }
    });
  }
  while (running) {
    SDL_Event event;
    if (!SDL_WaitEvent(&event)) break;
    do {
      wire::Input input;
      input.mod = static_cast<uint16_t>(SDL_GetModState());
      bool forward = true;
      switch (event.type) {
        case SDL_EVENT_QUIT:
          running = false;
          forward = false;
          break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: sendGeometry(); forward = false; break;
        case SDL_EVENT_WINDOW_EXPOSED: present(); forward = false; break;
        case SDL_EVENT_WINDOW_MAXIMIZED:
        case SDL_EVENT_WINDOW_RESTORED: {
          const bool maximized = (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0;
          {
            std::lock_guard<std::mutex> lock(g_hit.mutex);
            g_hit.maximized = maximized;
          }
          session.SetWindowFlags(borderless, maximized);
          forward = false;
          break;
        }
        case SDL_EVENT_USER: {
          if (event.user.code == 2) {
            // (A picture of the window, for looking at: the script asked for it.)
            char* path = static_cast<char*>(event.user.data1);
            if (path) {
              compose();
              if (SDL_Surface* surface = SDL_RenderReadPixels(renderer, nullptr)) {
                SDL_SaveBMP(surface, path);
                SDL_DestroySurface(surface);
              }
              SDL_RenderPresent(renderer);
              std::fprintf(stderr, "window picture: %s\n", path);
              SDL_free(path);
            }
            forward = false;
            break;
          }
          if (event.user.code == 1) {
            // (The timer of the sound: the page is told how much each stream has not played.)
            for (const auto& [key, stream] : audio) {
              Writer out;
              out.U32(static_cast<uint32_t>(key & 0xFFFFFFFFu));
              out.U64(stream.output->QueuedFrames());
              session.Send(static_cast<uint32_t>(key >> 32), wire::ToRenderer::AudioQueued, out.bytes());
            }
            forward = false;
            break;
          }
          std::deque<Message> mine;
          {
            std::lock_guard<std::mutex> lock(mutex);
            mine.swap(inbox);
          }
          for (Message& message : mine) handle(message);
          forward = false;
          break;
        }
        case SDL_EVENT_MOUSE_WHEEL:
          input.kind = wire::Input::Kind::Wheel;
          input.x = event.wheel.x;
          input.y = event.wheel.y;
          input.xrel = event.wheel.mouse_x;
          input.yrel = event.wheel.mouse_y;
          break;
        case SDL_EVENT_TEXT_INPUT:
          input.kind = wire::Input::Kind::Text;
          input.text = event.text.text ? event.text.text : "";
          break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
          input.kind = event.type == SDL_EVENT_KEY_DOWN ? wire::Input::Kind::KeyDown : wire::Input::Kind::KeyUp;
          input.key = event.key.key;
          input.mod = event.key.mod;
          input.repeat = event.key.repeat;
          if (event.type == SDL_EVENT_KEY_DOWN && (((event.key.mod & SDL_KMOD_CTRL) && event.key.key == SDLK_V) || ((event.key.mod & SDL_KMOD_SHIFT) && event.key.key == SDLK_INSERT))) {
            char* held = SDL_GetClipboardText();
            input.clipboard = held ? held : "";
            SDL_free(held);
          }
          break;
        case SDL_EVENT_MOUSE_MOTION:
          input.kind = wire::Input::Kind::Motion;
          input.x = event.motion.x;
          input.y = event.motion.y;
          input.xrel = event.motion.xrel;
          input.yrel = event.motion.yrel;
          break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
          input.kind = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? wire::Input::Kind::ButtonDown : wire::Input::Kind::ButtonUp;
          input.button = event.button.button;
          input.x = event.button.x;
          input.y = event.button.y;
          break;
        default: forward = false; break;
      }
      if (forward) {
        uint32_t target = session.current();
        const float barPts = static_cast<float>(barPoints()), leftPts = static_cast<float>(leftPoints());
        const float barExtent = barPts > 0 ? barPts + (sideTabs ? 0.0f : static_cast<float>(overlay)) : 0;
        const bool mouse = input.kind == wire::Input::Kind::Motion || input.kind == wire::Input::Kind::ButtonDown || input.kind == wire::Input::Kind::ButtonUp || input.kind == wire::Input::Kind::Wheel;
        if (mouse) {
          // The bar is the top of the window and the page the rest (its y starts under the bar); a button that went down in one keeps what follows until it is up.
          const float y = input.kind == wire::Input::Kind::Wheel ? input.yrel : input.y;
          const float x = input.kind == wire::Input::Kind::Wheel ? input.xrel : input.x;
          // (With the tabs at the side the interface also has the column at the left, and the whole window while a menu is open.)
          const bool inBar = y < barExtent || x < leftPts || (sideTabs && overlay > 0);
          const int side = capture ? capture : (inBar ? 1 : 2);
          if (input.kind == wire::Input::Kind::ButtonDown) {
            capture = side;
            if (side == 2 && barFocused) {
              barFocused = false;
              session.SendChrome(wire::ToRenderer::UiState, "{\"blur\":true}");
            }
          } else if (input.kind == wire::Input::Kind::ButtonUp) {
            capture = 0;
          }
          if (side == 1) {
            target = session.chrome();
          } else if (input.kind == wire::Input::Kind::Wheel) {
            input.yrel -= barPts;
            input.xrel -= leftPts;
          } else {
            input.y -= barPts;
            input.x -= leftPts;
          }
        } else if (input.kind == wire::Input::Kind::KeyDown && !input.repeat) {
          // What the browser does with the keys before the page sees them.
          const bool ctrl = (input.mod & SDL_KMOD_CTRL) != 0, alt = (input.mod & SDL_KMOD_ALT) != 0;
          if ((ctrl && input.key == SDLK_L) || input.key == SDLK_F6) {
            if (!noBar && !fullscreen) {
              barFocused = true;
              session.SendChrome(wire::ToRenderer::UiState, "{\"focusAddress\":true}");
            }
            forward = false;
          } else if (ctrl && input.key == SDLK_T) {
            session.NewTab("");
            if (!noBar && !fullscreen) {
              barFocused = true;
              session.SendChrome(wire::ToRenderer::UiState, "{\"focusAddress\":true}");
            }
            forward = false;
          } else if (ctrl && input.key == SDLK_W) {
            session.CloseTab(session.activeTab());
            forward = false;
          } else if (ctrl && input.key == SDLK_TAB) {
            session.CycleTab((input.mod & SDL_KMOD_SHIFT) ? -1 : 1);
            barFocused = false;
            forward = false;
          } else if (alt && input.key == SDLK_LEFT) {
            session.Back();
            forward = false;
          } else if (alt && input.key == SDLK_RIGHT) {
            session.Forward();
            forward = false;
          } else if ((ctrl && input.key == SDLK_R) || input.key == SDLK_F5) {
            session.Reload();
            forward = false;
          }
        }
        if (!mouse && barFocused) target = session.chrome();
        if (forward) {
          Writer out;
          wire::Write(out, input);
          session.Send(target, wire::ToRenderer::Input, out.bytes());
        }
      }
    } while (running && SDL_PollEvent(&event));
  }
  if (audioTimer) SDL_RemoveTimer(audioTimer);
  audio.clear();
  if (script.joinable()) script.join();
  // The pages are told that the window ends (one that is gone does not mind), and their end is waited for.
  const int code = session.Close();
  if (texture) SDL_DestroyTexture(texture);
  if (barTexture) SDL_DestroyTexture(barTexture);
  return code;
}

}  // namespace solar::gui
