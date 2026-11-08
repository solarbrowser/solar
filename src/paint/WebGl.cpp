/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// WebGL (https://registry.khronos.org/webgl/specs/latest/): the natives under the API the script sees (src/web-api/js/webgl.js): __solarGlCreate, __solarGlOps, __solarGl and the
// rest. The contexts are made and run by a backend (solar/paint/GlRuntime.h: in this process, or in the GPU's, which the page's process is shut out of); what the page has drawn is read
// from the drawing buffer a moment after (one task of the loop for all the drawing there was meanwhile) into the canvas's surface, which the window shows as it shows a 2D canvas.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "CanvasNatives.h"
#include "solar/paint/Canvas.h"
#include "solar/paint/GlRuntime.h"

namespace solar::paint {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::Value;

namespace {

// What a realm has of WebGL: its contexts (by the numbers the backend gave) and the surfaces that show them.
struct Held {
  uint32_t backendId = 0;
  uint32_t surfaceId = 0;
  bool dirtySinceRead = true;
};
struct WebGlState {
  std::map<uint32_t, Held> contexts;
  uint32_t next = 1;
};

WebGlState& StateFor(Natives& n) {
  thread_local std::map<Natives*, std::unique_ptr<WebGlState>> states;
  auto& slot = states[&n];
  if (!slot) {
    slot = std::make_unique<WebGlState>();
    Natives* key = &n;
    n.onRelease.push_back([key] {
      // (The contexts of the realm go with it.)
      auto it = states.find(key);
      if (it == states.end()) return;
      for (auto& [id, held] : it->second->contexts) gl::CurrentBackend().Destroy(held.backendId);
      states.erase(it);
    });
  }
  return *slot;
}

double Num(Context& ctx, qe::Args a, size_t i) { return i < a.size() ? qe::ToNumber(ctx, a[i]) : 0; }
int Int(Context& ctx, qe::Args a, size_t i) { return static_cast<int>(static_cast<int64_t>(Num(ctx, a, i))); }

// The script's value as what a call is given.
gl::Arg ArgOf(Context& ctx, const Value& v) {
  gl::Arg arg;
  if (v.is_null()) {
    arg.kind = gl::Arg::Kind::Null;
  } else if (v.is_undefined()) {
    arg.kind = gl::Arg::Kind::Undefined;
  } else if (qe::IsObject(v)) {
    if (auto bytes = qe::MutableBytesOf(v)) {
      arg.kind = gl::Arg::Kind::Bytes;
      arg.data = bytes->data();
      arg.size = bytes->size();
      arg.writable = true;
    } else if (auto readable = qe::BytesOf(v)) {
      arg.kind = gl::Arg::Kind::Bytes;
      arg.data = const_cast<uint8_t*>(readable->data());
      arg.size = readable->size();
    } else {
      arg.kind = gl::Arg::Kind::Null;
    }
  } else if (v.is_string()) {
    arg.kind = gl::Arg::Kind::Text;
    arg.text = qe::ToWtf8(ctx, v);
  } else {
    arg.kind = gl::Arg::Kind::Number;
    arg.number = qe::ToNumber(ctx, v);
  }
  return arg;
}

Value ValueOf(Context& ctx, const gl::Result& r) {
  switch (r.kind) {
    case gl::Result::Kind::Number: return qe::FromNumber(r.number);
    case gl::Result::Kind::Text: return qe::FromWtf8(ctx, r.text);
    case gl::Result::Kind::Numbers: {
      Value list = qe::NewArray(ctx);
      for (double v : r.numbers) qe::ArrayPush(ctx, list, qe::FromNumber(v));
      return list;
    }
    case gl::Result::Kind::Undefined: break;
  }
  return qe::Undefined();
}

Held* HeldOf(Natives& n, Context& ctx, const Value& id) {
  WebGlState& state = StateFor(n);
  const auto found = state.contexts.find(qe::ToUint32(ctx, id));
  return found == state.contexts.end() ? nullptr : &found->second;
}

// __solarGlOps(): the names of the operations, whose numbers are their places.
Value OpsCall(Context& ctx, Value, qe::Args, Value) {
  Value list = qe::NewArray(ctx);
  for (const std::string& name : gl::OperationNames()) qe::ArrayPush(ctx, list, qe::FromWtf8(ctx, name));
  return list;
}

// What the drawing buffer has is copied into the surface just before the surface is published.
void Read(Natives* n, Held& held) {
  held.dirtySinceRead = false;
  gl::Backend& backend = gl::CurrentBackend();
  const auto t0 = std::chrono::steady_clock::now();
  int width = 0, height = 0;
  const std::vector<uint8_t> pixels = backend.Present(held.backendId, width, height);
  if (pixels.empty() || width <= 0 || height <= 0 || pixels.size() != static_cast<size_t>(width) * height * 4) return;
  const auto t1 = std::chrono::steady_clock::now();
  const SkImageInfo info = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, backend.Premultiplied(held.backendId) ? kPremul_SkAlphaType : kUnpremul_SkAlphaType);
  SkBitmap bitmap;
  if (!bitmap.tryAllocPixels(info)) return;
  std::memcpy(bitmap.getPixels(), pixels.data(), pixels.size());
  bitmap.setImmutable();
  const auto found = n->surfaces.find(held.surfaceId);
  if (found != n->surfaces.end()) found->second->SetContents(bitmap.asImage(), true);
  if (std::getenv("SOLAR_GL_DEBUG")) {
    static int frames = 0;
    static double read = 0, copy = 0;
    read += std::chrono::duration<double, std::milli>(t1 - t0).count();
    copy += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    if (++frames % 60 == 0) std::fprintf(stderr, "gl present: per frame read %.2f ms, copy %.2f ms\n", read / 60, copy / 60), read = copy = 0;
  }
}

// __solarGlCreate(version, attributes (JSON text), width, height): [context, surface] of a new context and the surface that shows it, or undefined where there is none.
Value CreateCall(Context& ctx, Value, qe::Args a, Value) {
  Natives* n = NativesOf(ctx);
  if (!n) return qe::Undefined();
  const std::string attributes = a.size() > 1 ? qe::ToWtf8(ctx, a[1]) : "";
  const int width = std::max(1, Int(ctx, a, 2)), height = std::max(1, Int(ctx, a, 3));
  const uint32_t backendId = gl::CurrentBackend().Create(Int(ctx, a, 0) == 2 ? 2 : 1, attributes, width, height);
  if (!backendId) return qe::Undefined();
  const uint32_t surface = n->next++;
  n->surfaces[surface] = std::make_shared<CanvasSurface>(width, height);
  WebGlState& state = StateFor(*n);
  const uint32_t id = state.next++;
  Held& held = state.contexts[id];
  held.backendId = backendId;
  held.surfaceId = surface;
  Held* raw = &held;
  (void)raw;
  n->beforePublish[surface] = [n, id, surface] {
    WebGlState& s = StateFor(*n);
    const auto found = s.contexts.find(id);
    if (found != s.contexts.end()) Read(n, found->second);
  };
  Value list = qe::NewArray(ctx);
  qe::ArrayPush(ctx, list, qe::FromNumber(id));
  qe::ArrayPush(ctx, list, qe::FromNumber(surface));
  return list;
}

// __solarGlInfo(context): JSON text of what the context is.
Value InfoCall(Context& ctx, Value, qe::Args a, Value) {
  Natives* n = NativesOf(ctx);
  if (!n || a.empty()) return qe::Undefined();
  Held* g = HeldOf(*n, ctx, a[0]);
  if (!g) return qe::Undefined();
  return qe::FromWtf8(ctx, gl::CurrentBackend().Info(g->backendId));
}

// __solarGlResize(context, width, height)
Value ResizeCall(Context& ctx, Value, qe::Args a, Value) {
  Natives* n = NativesOf(ctx);
  if (!n || a.size() < 3) return qe::Undefined();
  Held* g = HeldOf(*n, ctx, a[0]);
  if (!g) return qe::Undefined();
  const int width = std::max(1, Int(ctx, a, 1)), height = std::max(1, Int(ctx, a, 2));
  gl::CurrentBackend().Resize(g->backendId, width, height);
  const auto surface = n->surfaces.find(g->surfaceId);
  if (surface != n->surfaces.end()) surface->second->Reset(width, height);
  return qe::Undefined();
}

// __solarGlPresent(context): what has been drawn goes into the surface now (for toDataURL and for a canvas drawn from this one) instead of at the next turn of the loop.
Value PresentCall(Context& ctx, Value, qe::Args a, Value) {
  Natives* n = NativesOf(ctx);
  if (!n || a.empty()) return qe::Undefined();
  Held* g = HeldOf(*n, ctx, a[0]);
  if (!g) return qe::Undefined();
  if (g->dirtySinceRead) Read(n, *g);
  return qe::Undefined();
}

Value DestroyCall(Context& ctx, Value, qe::Args a, Value) {
  Natives* n = NativesOf(ctx);
  if (!n || a.empty()) return qe::Undefined();
  WebGlState& state = StateFor(*n);
  const auto found = state.contexts.find(qe::ToUint32(ctx, a[0]));
  if (found == state.contexts.end()) return qe::Undefined();
  gl::CurrentBackend().Destroy(found->second.backendId);
  n->beforePublish.erase(found->second.surfaceId);
  state.contexts.erase(found);
  return qe::Undefined();
}

// __solarGl(context, operation, ...): one call of the standard's. The numbers of the operations are the places of their names in __solarGlOps().
Value CallCall(Context& ctx, Value, qe::Args a, Value) {
  Natives* n = NativesOf(ctx);
  if (!n || a.size() < 2) return qe::Undefined();
  Held* g = HeldOf(*n, ctx, a[0]);
  const auto& names = gl::OperationNames();
  const size_t op = static_cast<size_t>(Num(ctx, a, 1));
  if (!g || op >= names.size()) return qe::Undefined();
  std::vector<gl::Arg> args;
  args.reserve(a.size() - 2);
  for (size_t i = 2; i < a.size(); ++i) args.push_back(ArgOf(ctx, a[i]));
  static const std::vector<bool> answers = [] {
    std::vector<bool> out;
    for (const std::string& name : gl::OperationNames()) out.push_back(gl::NeedsAnswer(name));
    return out;
  }();
  const gl::Result result = gl::CurrentBackend().Call(g->backendId, op, args, answers[op]);
  if (gl::Draws(names[op])) {
    const auto surface = n->surfaces.find(g->surfaceId);
    if (surface != n->surfaces.end()) {
      surface->second->dirty = true;
      g->dirtySinceRead = true;
      n->Touch(g->surfaceId);
    }
  }
  return ValueOf(ctx, result);
}

}  // namespace

}  // namespace solar::paint

namespace solar::webapi {

void InstallWebGlNatives(Quanta::Embed::Realm& realm) {
  namespace p = solar::paint;
  Quanta::Context& ctx = realm.GetContext();
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarGlOps", p::OpsCall, 0);
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarGlCreate", p::CreateCall, 4);
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarGlInfo", p::InfoCall, 1);
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarGlResize", p::ResizeCall, 3);
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarGlPresent", p::PresentCall, 1);
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarGlDestroy", p::DestroyCall, 1);
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarGl", p::CallCall, 2);
}

}  // namespace solar::webapi
