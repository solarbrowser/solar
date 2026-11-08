/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "Window.h"

#include <unistd.h>

#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "solar/gui/FrameBuffer.h"
#include "solar/gui/Wire.h"

namespace solar::gui {

using net::ipc::Writer;

namespace {

struct Message {
  uint8_t type = 0;
  std::string bytes;
  int fd = -1;
};

void Send(net::ipc::Channel& link, wire::ToRenderer type, const std::string& bytes) { link.SendRaw(static_cast<uint8_t>(type), bytes); }

void SendInput(net::ipc::Channel& link, const wire::Input& input) {
  Writer out;
  wire::Write(out, input);
  Send(link, wire::ToRenderer::Input, out.bytes());
}

}  // namespace

void SendGeometry(net::ipc::Channel& link, SDL_Window* window) {
  int w = 0, h = 0, pw = 0, ph = 0;
  SDL_GetWindowSize(window, &w, &h);
  SDL_GetWindowSizeInPixels(window, &pw, &ph);
  wire::Geometry geometry{w, h, pw, ph, SDL_GetWindowPixelDensity(window)};
  Writer out;
  wire::Write(out, geometry);
  Send(link, wire::ToRenderer::Resize, out.bytes());
}

void RunWindow(net::ipc::Channel& link, SDL_Window* window, SDL_Renderer* renderer, BrowserServices::Tab& tab) {
  // What the page says comes on a thread of its own and is handed to this one (the window's events and the page's messages are taken in one place).
  std::mutex mutex;
  std::deque<Message> inbox;
  std::thread reader([&] {
    Message message;
    while (link.ReceiveRaw(message.type, message.bytes, &message.fd)) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        inbox.push_back(std::move(message));
      }
      message = Message{};
      SDL_Event wake{};
      wake.type = SDL_EVENT_USER;
      SDL_PushEvent(&wake);
    }
    // (The page is gone: the window ends.)
    {
      std::lock_guard<std::mutex> lock(mutex);
      Message quit;
      quit.type = static_cast<uint8_t>(wire::ToBrowser::Quit);
      inbox.push_back(std::move(quit));
    }
    SDL_Event wake{};
    wake.type = SDL_EVENT_USER;
    SDL_PushEvent(&wake);
  });

  std::map<uint32_t, SharedMemory> buffers;  // the pictures the page can make, mapped to be read
  std::map<uint32_t, wire::NewBuffer> shapes;
  SDL_Texture* texture = nullptr;
  int textureW = 0, textureH = 0;
  float lockedAtX = 0, lockedAtY = 0;
  bool running = true;
  bool haveFrame = false;

  const auto present = [&] {
    if (!texture) return;
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);
  };

  const auto handle = [&](Message& message) {
    net::ipc::Reader in(message.bytes);
    // (A descriptor comes only with a buffer: the one that came with another message is closed.)
    int fd = message.fd;
    message.fd = -1;
    const auto type = static_cast<wire::ToBrowser>(message.type);
    if (type != wire::ToBrowser::NewBuffer && fd >= 0) {
      ::close(fd);
      fd = -1;
    }
    switch (type) {
      case wire::ToBrowser::NewBuffer: {
        wire::NewBuffer shape;
        if (!wire::Read(in, shape)) {
          if (fd >= 0) ::close(fd);
          break;
        }
        SharedMemory memory;
        if (!memory.Adopt(fd, static_cast<size_t>(shape.stride) * static_cast<size_t>(shape.height), false)) break;
        buffers[shape.index] = std::move(memory);
        shapes[shape.index] = shape;
        break;
      }
      case wire::ToBrowser::Frame: {
        wire::Frame frame;
        if (!wire::Read(in, frame)) break;
        auto buffer = buffers.find(frame.index);
        if (buffer == buffers.end()) break;
        const wire::NewBuffer& shape = shapes[frame.index];
        if (!texture || textureW != shape.width || textureH != shape.height) {
          if (texture) SDL_DestroyTexture(texture);
          texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, shape.width, shape.height);
          textureW = shape.width;
          textureH = shape.height;
        }
        if (texture) {
          SDL_UpdateTexture(texture, nullptr, buffer->second.data(), shape.stride);
          haveFrame = true;
          present();
        }
        // (It is copied: the page may draw into it again.)
        Writer out;
        out.U32(frame.index);
        Send(link, wire::ToRenderer::BufferReleased, out.bytes());
        break;
      }
      case wire::ToBrowser::Title: SDL_SetWindowTitle(window, message.bytes.substr(0, 4096).c_str()); break;
      case wire::ToBrowser::Clipboard: SDL_SetClipboardText(message.bytes.c_str()); break;
      case wire::ToBrowser::Fullscreen: SDL_SetWindowFullscreen(window, !message.bytes.empty() && message.bytes[0] != 0); break;
      case wire::ToBrowser::PointerLock:
        if (!message.bytes.empty() && message.bytes[0] != 0) {
          SDL_GetMouseState(&lockedAtX, &lockedAtY);
          SDL_SetWindowRelativeMouseMode(window, true);
        } else {
          SDL_SetWindowRelativeMouseMode(window, false);
          SDL_WarpMouseInWindow(window, lockedAtX, lockedAtY);
        }
        break;
      case wire::ToBrowser::Navigate: {
        wire::Navigate request;
        if (wire::Read(in, request)) tab.Navigate(std::move(request));
        break;
      }
      case wire::ToBrowser::NavCommitted: {
        const std::string address = in.String(size_t{1} << 21);
        if (in.ok()) tab.Committed(address);
        break;
      }
      case wire::ToBrowser::Quit: running = false; break;
      default: break;
    }
  };

  SendGeometry(link, window);
  while (running) {
    SDL_Event event;
    if (!SDL_WaitEvent(&event)) break;
    do {
      wire::Input input;
      input.mod = static_cast<uint16_t>(SDL_GetModState());
      bool forward = true;
      switch (event.type) {
        case SDL_EVENT_QUIT:
          Send(link, wire::ToRenderer::Shutdown, "");
          running = false;
          forward = false;
          break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: SendGeometry(link, window); forward = false; break;
        case SDL_EVENT_WINDOW_EXPOSED: present(); forward = false; break;
        case SDL_EVENT_USER: {
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
      if (forward) SendInput(link, input);
    } while (running && SDL_PollEvent(&event));
  }
  (void)haveFrame;
  // The page is told that the window ends (a page that is gone does not mind), and its end is waited for.
  Send(link, wire::ToRenderer::Shutdown, "");
  link.Shutdown();
  reader.join();
  if (texture) SDL_DestroyTexture(texture);
}

}  // namespace solar::gui
