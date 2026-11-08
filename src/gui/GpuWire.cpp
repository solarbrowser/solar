/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/gui/GpuWire.h"

#include <cmath>
#include <cstring>

#include "solar/gui/Wire.h"

namespace solar::gui::gpu {

using net::ipc::Reader;
using net::ipc::Writer;

void Write(Writer& out, const Buffer& b) {
  out.U32(b.index);
  out.I32(b.width);
  out.I32(b.height);
  out.I32(b.stride);
}

bool Read(Reader& in, Buffer& b) {
  b.index = in.U32();
  b.width = in.I32();
  b.height = in.I32();
  b.stride = in.I32();
  if (!in.ok()) return false;
  const int32_t most = wire::kMaxPixels;
  return b.index < 64 && b.width >= 1 && b.width <= most && b.height >= 1 && b.height <= most && b.stride >= b.width * 4 && b.stride <= most * 4;
}

namespace {
void WriteDouble(Writer& out, double v) {
  uint64_t bits;
  std::memcpy(&bits, &v, 8);
  out.U64(bits);
}
double ReadDouble(Reader& in) {
  const uint64_t bits = in.U64();
  double v;
  std::memcpy(&v, &bits, 8);
  return std::isfinite(v) ? v : 0.0;
}
}  // namespace

void Write(Writer& out, const Draw& d) {
  out.U32(d.index);
  out.U64(d.serial);
  out.I32(d.pixelWidth);
  out.I32(d.pixelHeight);
  out.F32(d.density);
  WriteDouble(out, d.scrollX);
  WriteDouble(out, d.scrollY);
  out.Bool(d.barVisible);
  out.Bool(d.barEmphasized);
  out.Bool(d.barDragging);
  out.F32(d.barAlpha);
  out.F32(d.barTop);
  out.F32(d.barHeight);
  out.I32(d.windowWidth);
  out.I32(d.windowHeight);
}

bool Read(Reader& in, Draw& d) {
  d.index = in.U32();
  d.serial = in.U64();
  d.pixelWidth = in.I32();
  d.pixelHeight = in.I32();
  d.density = in.F32();
  d.scrollX = ReadDouble(in);
  d.scrollY = ReadDouble(in);
  d.barVisible = in.Bool();
  d.barEmphasized = in.Bool();
  d.barDragging = in.Bool();
  d.barAlpha = in.F32();
  d.barTop = in.F32();
  d.barHeight = in.F32();
  d.windowWidth = in.I32();
  d.windowHeight = in.I32();
  if (!in.ok()) return false;
  const int32_t most = wire::kMaxPixels;
  return d.index < 64 && d.pixelWidth >= 1 && d.pixelWidth <= most && d.pixelHeight >= 1 && d.pixelHeight <= most && d.density >= 0.25f && d.density <= 16.f;
}

}  // namespace solar::gui::gpu
