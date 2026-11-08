/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/gui/Window.h"

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "solar/gui/FrameBuffer.h"
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
};

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
  float lockedAtX = 0, lockedAtY = 0;
  bool running = true;

  const auto present = [&] {
    if (!texture) return;
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);
  };
  const auto sendGeometry = [&] {
    int w = 0, h = 0, pw = 0, ph = 0;
    SDL_GetWindowSize(window, &w, &h);
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    Writer out;
    wire::Write(out, wire::Geometry{w, h, pw, ph, SDL_GetWindowPixelDensity(window)});
    session.Broadcast(wire::ToRenderer::Resize, out.bytes());
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
        if (shown) {
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
        if (shown) SDL_SetWindowTitle(window, message.bytes.substr(0, 4096).c_str());
        break;
      case wire::ToBrowser::Clipboard:
        if (shown) SDL_SetClipboardText(message.bytes.c_str());
        break;
      case wire::ToBrowser::Fullscreen:
        if (shown) SDL_SetWindowFullscreen(window, !message.bytes.empty() && message.bytes[0] != 0);
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
      case wire::ToBrowser::Quit:
        if (shown) running = false;
        break;
      default: break;
    }
  };

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
        case SDL_EVENT_USER: {
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
        Writer out;
        wire::Write(out, input);
        session.Send(session.current(), wire::ToRenderer::Input, out.bytes());
      }
    } while (running && SDL_PollEvent(&event));
  }
  if (audioTimer) SDL_RemoveTimer(audioTimer);
  audio.clear();
  if (script.joinable()) script.join();
  // The pages are told that the window ends (one that is gone does not mind), and their end is waited for.
  const int code = session.Close();
  if (texture) SDL_DestroyTexture(texture);
  return code;
}

}  // namespace solar::gui
