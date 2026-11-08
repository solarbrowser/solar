/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <cstdint>
#include <string>

#include "solar/net/Ipc.h"

// What the window (the browser process: it has the screen, the keyboard and the pointer) and the page (the renderer process: it is not trusted) say to each other. The window sends what
// the user does and how large the window is; the page sends the pictures it has drawn (in memory both can see: the window only reads it), and what it asks of the window (a title, the
// clipboard, full screen, the pointer held). Every message is checked when it is read.
namespace solar::gui::wire {

enum class ToRenderer : uint8_t { Resize = 1, Input, BufferReleased, Shutdown };
enum class ToBrowser : uint8_t { NewBuffer = 32, Frame, Title, Clipboard, Fullscreen, PointerLock, Quit };

// How large the window is: in points, in pixels, and the pixels a point comes to.
struct Geometry {
  int32_t width = 0, height = 0, pixelWidth = 0, pixelHeight = 0;
  float density = 1;
};
void Write(net::ipc::Writer& out, const Geometry& geometry);
// False where it is not a window (sizes below 1 or above what a screen has).
bool Read(net::ipc::Reader& in, Geometry& geometry);

// What the user did, as the page needs to know it (keys are SDL's key codes; modifiers are SDL's mod bits).
struct Input {
  enum class Kind : uint8_t { KeyDown, KeyUp, Text, Motion, ButtonDown, ButtonUp, Wheel } kind = Kind::Motion;
  uint32_t key = 0;
  uint16_t mod = 0;
  bool repeat = false;
  uint8_t button = 0;
  float x = 0, y = 0, xrel = 0, yrel = 0;  // the pointer, and for a wheel the steps (x, y) and the place (xrel, yrel)
  std::string text;       // a text input
  std::string clipboard;  // what the clipboard held when a key that pastes was pressed
};
void Write(net::ipc::Writer& out, const Input& input);
bool Read(net::ipc::Reader& in, Input& input);

// A picture of the page: the memory (sent along as a descriptor with the message) is `stride` * `height` bytes of 32-bit pixels (B, G, R, A in memory), and is the `index`th of the ones in use.
struct NewBuffer {
  uint32_t index = 0;
  int32_t width = 0, height = 0, stride = 0;
};
void Write(net::ipc::Writer& out, const NewBuffer& buffer);
bool Read(net::ipc::Reader& in, NewBuffer& buffer);

// The page has drawn into the buffer: it is not touched until the window says it is let go of.
struct Frame {
  uint32_t index = 0;
  uint64_t serial = 0;
};
void Write(net::ipc::Writer& out, const Frame& frame);
bool Read(net::ipc::Reader& in, Frame& frame);

// Largest sizes a buffer or a window may have (a renderer is not asked for more memory than a screen has).
constexpr int32_t kMaxPixels = 16384;

}  // namespace solar::gui::wire
