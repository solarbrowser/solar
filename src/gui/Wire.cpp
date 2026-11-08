/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/gui/Wire.h"

namespace solar::gui::wire {

using net::ipc::Reader;
using net::ipc::Writer;

void Write(Writer& out, const Geometry& g) {
  out.I32(g.width);
  out.I32(g.height);
  out.I32(g.pixelWidth);
  out.I32(g.pixelHeight);
  out.F32(g.density);
}

bool Read(Reader& in, Geometry& g) {
  g.width = in.I32();
  g.height = in.I32();
  g.pixelWidth = in.I32();
  g.pixelHeight = in.I32();
  g.density = in.F32();
  if (!in.ok()) return false;
  const auto sane = [](int32_t v) { return v >= 1 && v <= kMaxPixels; };
  return sane(g.width) && sane(g.height) && sane(g.pixelWidth) && sane(g.pixelHeight) && g.density >= 0.25f && g.density <= 16.f;
}

void Write(Writer& out, const Input& i) {
  out.U8(static_cast<uint8_t>(i.kind));
  out.U32(i.key);
  out.U32(i.mod);
  out.Bool(i.repeat);
  out.U8(i.button);
  out.F32(i.x);
  out.F32(i.y);
  out.F32(i.xrel);
  out.F32(i.yrel);
  out.String(i.text);
  out.String(i.clipboard);
}

bool Read(Reader& in, Input& i) {
  i.kind = in.EnumUpTo(Input::Kind::Wheel);
  i.key = in.U32();
  i.mod = static_cast<uint16_t>(in.U32());
  i.repeat = in.Bool();
  i.button = in.U8();
  i.x = in.F32();
  i.y = in.F32();
  i.xrel = in.F32();
  i.yrel = in.F32();
  i.text = in.String(size_t{1} << 20);
  i.clipboard = in.String(size_t{1} << 24);
  return in.ok();
}

void Write(Writer& out, const NewBuffer& b) {
  out.U32(b.index);
  out.I32(b.width);
  out.I32(b.height);
  out.I32(b.stride);
}

bool Read(Reader& in, NewBuffer& b) {
  b.index = in.U32();
  b.width = in.I32();
  b.height = in.I32();
  b.stride = in.I32();
  if (!in.ok()) return false;
  // (The stride holds a row, and the memory is what the sizes say: the window maps no more than that.)
  return b.index < 64 && b.width >= 1 && b.width <= kMaxPixels && b.height >= 1 && b.height <= kMaxPixels && b.stride >= b.width * 4 && b.stride <= kMaxPixels * 4;
}

void Write(Writer& out, const Frame& f) {
  out.U32(f.index);
  out.U64(f.serial);
}

bool Read(Reader& in, Frame& f) {
  f.index = in.U32();
  f.serial = in.U64();
  return in.ok() && f.index < 64;
}

}  // namespace solar::gui::wire
