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

void Write(Writer& out, const Navigate& n) {
  out.U32(n.id);
  out.U8(static_cast<uint8_t>(n.kind));
  out.I32(n.entry);
  out.String(n.url);
  out.String(n.method);
  out.String(n.body);
  out.String(n.bodyType);
}

bool Read(Reader& in, Navigate& n) {
  n.id = in.U32();
  n.kind = in.EnumUpTo(Navigate::Kind::Traversal);
  n.entry = in.I32();
  n.url = in.String(size_t{1} << 21);
  n.method = in.String(16);
  n.body = in.String(kMaxNavigationBody);
  n.bodyType = in.String(1024);
  return in.ok() && (n.method == "GET" || n.method == "POST");
}

void Write(Writer& out, const NavHead& h) {
  out.U32(h.id);
  out.I32(h.status);
  out.String(h.contentType);
  out.String(h.finalUrl);
}

bool Read(Reader& in, NavHead& h) {
  h.id = in.U32();
  h.status = in.I32();
  h.contentType = in.String(1 << 16);
  h.finalUrl = in.String(size_t{1} << 21);
  return in.ok();
}

void Write(Writer& out, const StorageRequest& r) {
  out.U8(static_cast<uint8_t>(r.op));
  out.U32(r.id);
  out.U8(r.area);
  out.String(r.origin);
  out.String(r.file);
  out.U32(r.maxBytes);
  out.String(r.data);
}

bool Read(Reader& in, StorageRequest& r) {
  r.op = in.EnumUpTo(StorageRequest::Op::List);
  r.id = in.U32();
  r.area = in.U8();
  r.origin = in.String(1 << 12);
  r.file = in.String(64);
  r.maxBytes = in.U32();
  r.data = in.String(kMaxStorageFile);
  return in.ok() && r.area <= 2;
}

}  // namespace solar::gui::wire
