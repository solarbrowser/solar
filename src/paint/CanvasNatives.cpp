/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// The natives CanvasRenderingContext2D (src/web-api/js/canvas.js) draws with. They are few and take numbers (an operation's number and its arguments), so that a call costs
// little: the context keeps its state in a CanvasSurface (src/paint/Canvas.cpp), which the realm holds by a number. A drawing is shown by the window a moment later: one task
// of the loop, for all that was drawn meanwhile, makes the picture the window reads and tells layout that the element changed.
#include <cmath>
#include <map>
#include <memory>
#include <set>

#include "CanvasSurface.h"
#include "include/codec/SkCodec.h"
#include "include/core/SkData.h"
#include "include/core/SkStream.h"
#include "include/effects/SkGradient.h"
#include "include/encode/SkJpegEncoder.h"
#include "include/encode/SkPngEncoder.h"
#include "include/encode/SkWebpEncoder.h"
#include "solar/css/Color.h"
#include "solar/css/Syntax.h"
#include "solar/dom/Damage.h"
#include "solar/dom/NodeBindingsInternal.h"
#include "solar/media/Player.h"
#include "solar/paint/Paint.h"
#include "solar/web/FetchHost.h"

namespace solar::paint {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::Value;

namespace {

char g_key;

struct Natives {
  Natives(Context& c, web::FetchHost& h) : ctx(&c), host(&h) {}
  Context* ctx;
  web::FetchHost* host;
  std::map<uint32_t, std::shared_ptr<CanvasSurface>> surfaces;
  std::map<uint32_t, sk_sp<SkImage>> images;
  std::map<uint32_t, std::vector<dom::Element*>> elements;
  std::set<uint32_t> queued;
  uint32_t next = 1;
  std::shared_ptr<bool> alive = std::make_shared<bool>(true);

  // What was drawn is shown a moment later: one task for all of it.
  void Touch(uint32_t id) {
    const auto found = surfaces.find(id);
    if (found == surfaces.end() || !found->second->dirty || queued.count(id)) return;
    queued.insert(id);
    host->config().loop->Post([this, id, alive = alive] {
      if (!*alive) return;
      queued.erase(id);
      const auto surface = surfaces.find(id);
      if (surface == surfaces.end()) return;
      surface->second->Publish();
      const auto attached = elements.find(id);
      if (attached == elements.end()) return;
      for (dom::Element* element : attached->second) {
        dom::NoteDamage(element, dom::kDamageAttributes);
        dom::NoteTreeChangeAt(element);
      }
    });
  }
  void Release() {
    *alive = false;
    for (auto& [id, list] : elements) for (dom::Element* element : list) DetachCanvas(element);
    elements.clear();
    surfaces.clear();
    images.clear();
  }
};

struct Guard {
  explicit Guard(std::shared_ptr<Natives> n) : natives(std::move(n)) {}
  ~Guard() { natives->Release(); }
  std::shared_ptr<Natives> natives;
};

Natives* NativesOf(Context& ctx) {
  web::FetchHost* host = web::HostOf(ctx);
  if (!host) return nullptr;
  if (auto found = host->Extension(&g_key)) return static_cast<Guard*>(found.get())->natives.get();
  auto guard = std::make_shared<Guard>(std::make_shared<Natives>(ctx, *host));
  host->SetExtension(&g_key, guard);
  return guard->natives.get();
}

CanvasSurface* SurfaceOf(Natives& n, Context& ctx, const Value& id) {
  const auto found = n.surfaces.find(qe::ToUint32(ctx, id));
  return found == n.surfaces.end() ? nullptr : found->second.get();
}

double Num(Context& ctx, qe::Args args, size_t i) { return i < args.size() ? qe::ToNumber(ctx, args[i]) : 0; }

Value Bytes(Context& ctx, const void* data, size_t size) {
  Value buffer = qe::NewArrayBuffer(ctx, size);
  if (size) {
    if (auto out = qe::MutableBytesOf(buffer)) std::memcpy(out->data(), data, size);
  }
  return buffer;
}

// A color of a style (a string of CSS, any spelling a stylesheet's color can have): false where it is not one. It is worked out as the style engine works out a color (a keyword,
// a hex color, rgb(), hsl(), color-mix() and the rest), to the rgb() or rgba() that is the computed value, which is then read.
bool ColorOf(const std::string& text, SkColor4f& out) {
  css::ComponentValue value;
  if (!css::ParseComponentValue(text, value)) return false;
  const std::optional<css::ComponentValue> specified = css::NormalizeColor(value);
  if (!specified) return false;
  const std::optional<css::ComponentValue> computed = css::ComputeColor(*specified, "rgb(0, 0, 0)", "light");
  if (!computed) return false;
  const paint::Color color = ParseColor(css::Serialize(*computed));
  out = SkColor4f{color.r / 255.f, color.g / 255.f, color.b / 255.f, color.a / 255.f};
  return true;
}

// ---- A small reader of the JSON the script sends for styles: objects of numbers, strings and arrays, which is all they have ----
struct Json {
  const std::string& s;
  size_t at = 0;
  void Skip() { while (at < s.size() && std::isspace(static_cast<unsigned char>(s[at]))) ++at; }
  std::string String() {
    std::string out;
    if (at < s.size() && s[at] == '"') ++at;
    while (at < s.size() && s[at] != '"') {
      if (s[at] == '\\' && at + 1 < s.size()) { ++at; char e = s[at]; out += e == 'n' ? '\n' : e == 't' ? '\t' : e; }
      else out += s[at];
      ++at;
    }
    ++at;
    return out;
  }
  double Number() { char* end; const double v = std::strtod(s.c_str() + at, &end); at = end - s.c_str(); return v; }
};

// Parses {"t":kind,"c":[...],"s":[[offset,"color"],...]} for a gradient and {"p":image,"r":repeat,"m":[...]} for a pattern.
sk_sp<SkShader> ShaderOf(Natives& n, const std::string& text) {
  Json j{text};
  std::map<std::string, std::vector<double>> numbers;
  std::vector<std::pair<double, std::string>> stops;
  std::string repeat = "repeat";
  j.Skip();
  if (j.at < text.size() && text[j.at] == '{') ++j.at;
  while (true) {
    j.Skip();
    if (j.at >= text.size() || text[j.at] == '}') break;
    if (text[j.at] == ',') { ++j.at; continue; }
    const std::string key = j.String();
    j.Skip();
    if (text[j.at] == ':') ++j.at;
    j.Skip();
    if (text[j.at] == '"') {
      const std::string value = j.String();
      if (key == "r") repeat = value;
    } else if (text[j.at] == '[') {
      ++j.at;
      std::vector<double> list;
      while (true) {
        j.Skip();
        if (j.at >= text.size() || text[j.at] == ']') { ++j.at; break; }
        if (text[j.at] == ',') { ++j.at; continue; }
        if (text[j.at] == '[') {
          // a stop: [offset, "color"]
          ++j.at;
          j.Skip();
          const double offset = j.Number();
          j.Skip();
          if (text[j.at] == ',') ++j.at;
          j.Skip();
          const std::string color = j.String();
          j.Skip();
          if (text[j.at] == ']') ++j.at;
          stops.emplace_back(offset, color);
        } else {
          list.push_back(j.Number());
        }
      }
      numbers[key] = std::move(list);
    } else {
      numbers[key] = {j.Number()};
    }
  }
  if (numbers.count("p")) {
    const auto image = n.images.find(static_cast<uint32_t>(numbers["p"][0]));
    if (image == n.images.end()) return nullptr;
    const bool x = repeat == "repeat" || repeat == "repeat-x", y = repeat == "repeat" || repeat == "repeat-y";
    SkMatrix matrix = SkMatrix::I();
    const auto m = numbers["m"];
    if (m.size() == 6) matrix = SkMatrix::MakeAll(static_cast<float>(m[0]), static_cast<float>(m[2]), static_cast<float>(m[4]), static_cast<float>(m[1]), static_cast<float>(m[3]), static_cast<float>(m[5]), 0, 0, 1);
    return image->second->makeShader(x ? SkTileMode::kRepeat : SkTileMode::kDecal, y ? SkTileMode::kRepeat : SkTileMode::kDecal, SkSamplingOptions(SkFilterMode::kLinear), &matrix);
  }
  const int kind = numbers.count("t") ? static_cast<int>(numbers["t"][0]) : 0;
  const std::vector<double>& c = numbers["c"];
  if (stops.empty()) return nullptr;
  std::vector<SkColor4f> colors;
  std::vector<float> positions;
  for (const auto& [offset, color] : stops) {
    SkColor4f parsed;
    if (!ColorOf(color, parsed)) parsed = SkColor4f{0, 0, 0, 1};
    colors.push_back(parsed);
    positions.push_back(static_cast<float>(std::clamp(offset, 0.0, 1.0)));
  }
  if (colors.size() == 1) {
    colors.push_back(colors[0]);
    positions = {0, 1};
  }
  SkGradient::Interpolation interpolation;
  interpolation.fInPremul = SkGradient::Interpolation::InPremul::kYes;
  const SkGradient gradient(SkGradient::Colors(SkSpan<const SkColor4f>(colors), SkSpan<const float>(positions), SkTileMode::kClamp), interpolation);
  if (kind == 0 && c.size() >= 4) {
    const SkPoint points[2] = {{static_cast<float>(c[0]), static_cast<float>(c[1])}, {static_cast<float>(c[2]), static_cast<float>(c[3])}};
    return SkShaders::LinearGradient(points, gradient);
  }
  if (kind == 1 && c.size() >= 6) {
    return SkShaders::TwoPointConicalGradient({static_cast<float>(c[0]), static_cast<float>(c[1])}, static_cast<float>(c[2]), {static_cast<float>(c[3]), static_cast<float>(c[4])}, static_cast<float>(c[5]), gradient);
  }
  if (kind == 2 && c.size() >= 3) {
    const float start = static_cast<float>(c[2] * 180 / 3.14159265358979323846);
    return SkShaders::SweepGradient({static_cast<float>(c[0]), static_cast<float>(c[1])}, start, start + 360, gradient);
  }
  return nullptr;
}

std::vector<double> Doubles(const Value& value) {
  std::vector<double> out;
  if (auto bytes = qe::BytesOf(value)) {
    out.resize(bytes->size() / sizeof(double));
    std::memcpy(out.data(), bytes->data(), out.size() * sizeof(double));
  }
  return out;
}

// ---- The natives ----

Value Create(Context& ctx, Value, qe::Args args, Value) {
  Natives* n = NativesOf(ctx);
  if (!n) return qe::FromNumber(-1);
  const uint32_t id = n->next++;
  n->surfaces[id] = std::make_shared<CanvasSurface>(static_cast<int>(Num(ctx, args, 0)), static_cast<int>(Num(ctx, args, 1)));
  return qe::FromNumber(id);
}

Value Resize(Context& ctx, Value, qe::Args args, Value) {
  Natives* n = NativesOf(ctx);
  if (!n || args.size() < 3) return qe::Undefined();
  if (CanvasSurface* surface = SurfaceOf(*n, ctx, args[0])) {
    surface->Reset(static_cast<int>(Num(ctx, args, 1)), static_cast<int>(Num(ctx, args, 2)));
    n->Touch(qe::ToUint32(ctx, args[0]));
  }
  return qe::Undefined();
}

Value Free(Context& ctx, Value, qe::Args args, Value) {
  Natives* n = NativesOf(ctx);
  if (!n || args.empty()) return qe::Undefined();
  const uint32_t id = qe::ToUint32(ctx, args[0]);
  const auto attached = n->elements.find(id);
  if (attached != n->elements.end()) {
    for (dom::Element* element : attached->second) DetachCanvas(element);
    n->elements.erase(attached);
  }
  n->surfaces.erase(id);
  return qe::Undefined();
}

// __solarCanvasAttach(id, element): the element shows the surface (id < 0: it shows none).
Value Attach(Context& ctx, Value, qe::Args args, Value) {
  Natives* n = NativesOf(ctx);
  if (!n || args.size() < 2) return qe::Undefined();
  dom::Element* element = dom::ThisElement(ctx, args[1]);
  if (!element) {
    ctx.clear_exception();
    return qe::Undefined();
  }
  if (qe::ToNumber(ctx, args[0]) < 0) {
    DetachCanvas(element);
    return qe::Undefined();
  }
  const uint32_t id = qe::ToUint32(ctx, args[0]);
  const auto found = n->surfaces.find(id);
  if (found == n->surfaces.end()) return qe::Undefined();
  AttachCanvas(element, found->second);
  n->elements[id].push_back(element);
  found->second->Publish();
  dom::NoteDamage(element, dom::kDamageAttributes);
  dom::NoteTreeChangeAt(element);
  return qe::Undefined();
}

// __solarCv(id, op, a, b, c, d, e, f, g, h)
Value Op(Context& ctx, Value, qe::Args args, Value) {
  Natives* n = NativesOf(ctx);
  if (!n || args.size() < 2) return qe::Undefined();
  CanvasSurface* s = SurfaceOf(*n, ctx, args[0]);
  if (!s) return qe::Undefined();
  const int op = static_cast<int>(Num(ctx, args, 1));
  double v[8];
  for (int i = 0; i < 8; ++i) v[i] = Num(ctx, args, 2 + i);
  switch (op) {
    case 0: s->Save(); break;
    case 1: s->Restore(); break;
    case 2: s->Transform(v[0], 0, 0, v[1], 0, 0); break;
    case 3: s->Transform(std::cos(v[0]), std::sin(v[0]), -std::sin(v[0]), std::cos(v[0]), 0, 0); break;
    case 4: s->Transform(1, 0, 0, 1, v[0], v[1]); break;
    case 5: s->Transform(v[0], v[1], v[2], v[3], v[4], v[5]); break;
    case 6: s->SetTransform(v[0], v[1], v[2], v[3], v[4], v[5]); break;
    case 7: s->SetTransform(1, 0, 0, 1, 0, 0); break;
    case 8: s->SetGlobalAlpha(v[0]); break;
    case 9: if (v[0] > 0 && std::isfinite(v[0])) s->SetLineWidth(v[0]); break;
    case 10: s->SetLineCap(static_cast<int>(v[0])); break;
    case 11: s->SetLineJoin(static_cast<int>(v[0])); break;
    case 12: if (v[0] > 0 && std::isfinite(v[0])) s->SetMiterLimit(v[0]); break;
    case 13: if (std::isfinite(v[0])) s->SetLineDashOffset(v[0]); break;
    case 14: if (v[0] >= 0 && std::isfinite(v[0])) s->SetShadowBlur(v[0]); break;
    case 15: if (std::isfinite(v[0])) s->SetShadowOffsetX(v[0]); break;
    case 16: if (std::isfinite(v[0])) s->SetShadowOffsetY(v[0]); break;
    case 17: s->SetSmoothing(v[0] != 0, static_cast<int>(v[1])); break;
    case 18: s->SetTextAlign(static_cast<int>(v[0])); break;
    case 19: s->SetTextBaseline(static_cast<int>(v[0])); break;
    case 20: s->SetDirection(static_cast<int>(v[0])); break;
    case 30: s->BeginPath(); break;
    case 31: s->ClosePath(); break;
    case 32: s->MoveTo(v[0], v[1]); break;
    case 33: s->LineTo(v[0], v[1]); break;
    case 34: s->BezierCurveTo(v[0], v[1], v[2], v[3], v[4], v[5]); break;
    case 35: s->QuadraticCurveTo(v[0], v[1], v[2], v[3]); break;
    case 36: s->Arc(v[0], v[1], v[2], v[3], v[4], v[5] != 0); break;
    case 37: s->ArcTo(v[0], v[1], v[2], v[3], v[4]); break;
    case 38: s->Ellipse(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7] != 0); break;
    case 39: s->Rect(v[0], v[1], v[2], v[3]); break;
    case 50: s->FillRect(v[0], v[1], v[2], v[3]); break;
    case 51: s->StrokeRect(v[0], v[1], v[2], v[3]); break;
    case 52: s->ClearRect(v[0], v[1], v[2], v[3]); break;
  }
  n->Touch(qe::ToUint32(ctx, args[0]));
  return qe::Undefined();
}

// __solarCvDash(id, numbers, offset): the line dash (an even number of numbers, or none).
Value Dash(Context& ctx, Value, qe::Args args, Value) {
  Natives* n = NativesOf(ctx);
  if (!n || args.size() < 2) return qe::Undefined();
  if (CanvasSurface* s = SurfaceOf(*n, ctx, args[0])) {
    std::vector<float> dash;
    for (double d : Doubles(args[1])) dash.push_back(static_cast<float>(d));
    s->SetLineDash(std::move(dash));
  }
  return qe::Undefined();
}

// __solarCvRoundRect(id, x, y, w, h, radii (8 numbers))
Value RoundRect(Context& ctx, Value, qe::Args args, Value) {
  Natives* n = NativesOf(ctx);
  if (!n || args.size() < 6) return qe::Undefined();
  if (CanvasSurface* s = SurfaceOf(*n, ctx, args[0])) {
    const std::vector<double> radii = Doubles(args[5]);
    if (radii.size() >= 8) s->RoundRect(Num(ctx, args, 1), Num(ctx, args, 2), Num(ctx, args, 3), Num(ctx, args, 4), radii.data());
  }
  return qe::Undefined();
}

// __solarCvStr(id, op, string, a, b, c)
Value Str(Context& ctx, Value, qe::Args args, Value) {
  Natives* n = NativesOf(ctx);
  if (!n || args.size() < 3) return qe::Undefined();
  CanvasSurface* s = SurfaceOf(*n, ctx, args[0]);
  if (!s) return qe::Undefined();
  const int op = static_cast<int>(Num(ctx, args, 1));
  const std::string text = qe::ToWtf8(ctx, args[2]);
  Value result = qe::Undefined();
  switch (op) {
    case 0: result = qe::FromBool(s->SetFont(text)); break;
    case 1: result = qe::FromBool(s->SetBlend(text)); break;
    case 2:
    case 3: {
      const double max = Num(ctx, args, 5);
      s->DrawText(text, Num(ctx, args, 3), Num(ctx, args, 4), std::isnan(max) ? std::nullopt : std::optional<double>(max), op == 3);
      break;
    }
    case 4: result = qe::FromWtf8(ctx, s->Measure(text)); break;
  }
  n->Touch(qe::ToUint32(ctx, args[0]));
  return result;
}

// __solarCvStyle(id, which, text): which 0 fill, 1 stroke, 2 shadow color; the text is a CSS color, or the JSON of a gradient or a pattern ("{...}"). True if it was taken.
Value Style(Context& ctx, Value, qe::Args args, Value) {
  Natives* n = NativesOf(ctx);
  if (!n || args.size() < 3) return qe::FromBool(false);
  CanvasSurface* s = SurfaceOf(*n, ctx, args[0]);
  if (!s) return qe::FromBool(false);
  const int which = static_cast<int>(Num(ctx, args, 1));
  const std::string text = qe::ToWtf8(ctx, args[2]);
  if (!text.empty() && text[0] == '{') {
    sk_sp<SkShader> shader = ShaderOf(*n, text);
    if (!shader) return qe::FromBool(false);
    if (which == 0) s->SetFillShader(std::move(shader));
    else if (which == 1) s->SetStrokeShader(std::move(shader));
    return qe::FromBool(which <= 1);
  }
  SkColor4f color;
  if (!ColorOf(text, color)) return qe::FromBool(false);
  if (which == 0) s->SetFillColor(color);
  else if (which == 1) s->SetStrokeColor(color);
  else s->SetShadowColor(color);
  return qe::FromBool(true);
}

// __solarCvPath(id, op, commands | undefined, evenOdd, x, y): op 0 fill, 1 stroke, 2 clip, 3 isPointInPath, 4 isPointInStroke. `commands` are a Path2D's (a Float64Array); with none,
// the context's own path.
Value PathCall(Context& ctx, Value, qe::Args args, Value) {
  Natives* n = NativesOf(ctx);
  if (!n || args.size() < 4) return qe::Undefined();
  CanvasSurface* s = SurfaceOf(*n, ctx, args[0]);
  if (!s) return qe::Undefined();
  const int op = static_cast<int>(Num(ctx, args, 1));
  std::optional<SkPath> external;
  if (!args[2].is_undefined() && !args[2].is_null()) {
    external = PathFromCommands(Doubles(args[2]));
  }
  const SkPath* path = external ? &*external : nullptr;
  const bool evenOdd = qe::ToBoolean(args[3]);
  Value result = qe::Undefined();
  switch (op) {
    case 0: s->Fill(path, evenOdd); break;
    case 1: s->Stroke(path); break;
    case 2: s->Clip(path, evenOdd); break;
    case 3: result = qe::FromBool(s->InPath(path, Num(ctx, args, 4), Num(ctx, args, 5), evenOdd)); break;
    case 4: result = qe::FromBool(s->InStroke(path, Num(ctx, args, 4), Num(ctx, args, 5))); break;
  }
  n->Touch(qe::ToUint32(ctx, args[0]));
  return result;
}

// __solarCvAddPath(id, commands, a, b, c, d, e, f): the commands of a Path2D are added to the context's path, as they are with the matrix a..f (the context's own as well).
Value AddPath(Context& ctx, Value, qe::Args args, Value) {
  Natives* n = NativesOf(ctx);
  if (!n || args.size() < 8) return qe::Undefined();
  CanvasSurface* s = SurfaceOf(*n, ctx, args[0]);
  if (!s) return qe::Undefined();
  const SkMatrix extra = SkMatrix::MakeAll(static_cast<float>(Num(ctx, args, 2)), static_cast<float>(Num(ctx, args, 4)), static_cast<float>(Num(ctx, args, 6)), static_cast<float>(Num(ctx, args, 3)),
                                           static_cast<float>(Num(ctx, args, 5)), static_cast<float>(Num(ctx, args, 7)), 0, 0, 1);
  SkMatrix m = s->Matrix();
  m.preConcat(extra);
  s->AddPathCommands(Doubles(args[1]), m);
  return qe::Undefined();
}

// __solarCvColor(text): [r, g, b, a] (0-255, and the alpha 0-1) of a CSS color, or undefined where it is not one.
Value ColorCall(Context& ctx, Value, qe::Args args, Value) {
  if (args.empty()) return qe::Undefined();
  SkColor4f color;
  if (!ColorOf(qe::ToWtf8(ctx, args[0]), color)) return qe::Undefined();
  Value list = qe::NewArray(ctx);
  qe::ArrayPush(ctx, list, qe::FromNumber(std::lround(color.fR * 255)));
  qe::ArrayPush(ctx, list, qe::FromNumber(std::lround(color.fG * 255)));
  qe::ArrayPush(ctx, list, qe::FromNumber(std::lround(color.fB * 255)));
  qe::ArrayPush(ctx, list, qe::FromNumber(std::round(color.fA * 255) / 255.0));
  return list;
}

// ---- Images ----

Value ImageResult(Natives& n, Context& ctx, sk_sp<SkImage> image) {
  if (!image) return qe::Undefined();
  const uint32_t id = n.next++;
  const int w = image->width(), h = image->height();
  n.images[id] = std::move(image);
  Value list = qe::NewArray(ctx);
  qe::ArrayPush(ctx, list, qe::FromNumber(id));
  qe::ArrayPush(ctx, list, qe::FromNumber(w));
  qe::ArrayPush(ctx, list, qe::FromNumber(h));
  return list;
}

// __solarImageDecode(bytes): [id, width, height] of the picture the file is, or undefined.
Value ImageDecode(Context& ctx, Value, qe::Args args, Value) {
  Natives* n = NativesOf(ctx);
  if (!n || args.empty()) return qe::Undefined();
  auto bytes = qe::BytesOf(args[0]);
  if (!bytes) return qe::Undefined();
  return ImageResult(*n, ctx, DecodeImage(bytes->data(), bytes->size()));
}

// __solarImageFromCanvas(canvas id): the surface as it is now, as an image.
Value ImageFromCanvas(Context& ctx, Value, qe::Args args, Value) {
  Natives* n = NativesOf(ctx);
  if (!n || args.empty()) return qe::Undefined();
  CanvasSurface* s = SurfaceOf(*n, ctx, args[0]);
  return s ? ImageResult(*n, ctx, s->Snapshot()) : qe::Undefined();
}

// __solarImageFromPixels(width, height, rgba bytes)
Value ImageFromPixels(Context& ctx, Value, qe::Args args, Value) {
  Natives* n = NativesOf(ctx);
  if (!n || args.size() < 3) return qe::Undefined();
  const int w = static_cast<int>(Num(ctx, args, 0)), h = static_cast<int>(Num(ctx, args, 1));
  auto bytes = qe::BytesOf(args[2]);
  if (w <= 0 || h <= 0 || !bytes || bytes->size() < static_cast<size_t>(w) * h * 4) return qe::Undefined();
  const SkImageInfo info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
  SkBitmap bitmap;
  if (!bitmap.tryAllocPixels(info)) return qe::Undefined();
  std::memcpy(bitmap.getPixels(), bytes->data(), static_cast<size_t>(w) * h * 4);
  bitmap.setImmutable();
  return ImageResult(*n, ctx, bitmap.asImage());
}

// __solarImageFromVideo(element): the picture a video shows now (a copy), or undefined if it shows none.
Value ImageFromVideo(Context& ctx, Value, qe::Args args, Value) {
  Natives* n = NativesOf(ctx);
  if (!n || args.empty()) return qe::Undefined();
  dom::Element* element = dom::ThisElement(ctx, args[0]);
  if (!element) {
    ctx.clear_exception();
    return qe::Undefined();
  }
  const std::shared_ptr<media::Player> player = media::PlayerOf(element);
  if (!player) return qe::Undefined();
  const std::shared_ptr<const media::VideoFrame> frame = player->Frame();
  if (!frame || frame->width <= 0 || frame->height <= 0) return qe::Undefined();
  const SkImageInfo info = SkImageInfo::Make(frame->width, frame->height, kBGRA_8888_SkColorType, kOpaque_SkAlphaType);
  SkBitmap bitmap;
  if (!bitmap.tryAllocPixels(info)) return qe::Undefined();
  std::memcpy(bitmap.getPixels(), frame->bgra.data(), frame->bgra.size());
  bitmap.setImmutable();
  return ImageResult(*n, ctx, bitmap.asImage());
}

Value ImageFree(Context& ctx, Value, qe::Args args, Value) {
  Natives* n = NativesOf(ctx);
  if (n && !args.empty()) n->images.erase(qe::ToUint32(ctx, args[0]));
  return qe::Undefined();
}

// __solarCvDrawImage(id, image, sx, sy, sw, sh, dx, dy, dw, dh)
Value DrawImageCall(Context& ctx, Value, qe::Args args, Value) {
  Natives* n = NativesOf(ctx);
  if (!n || args.size() < 10) return qe::Undefined();
  CanvasSurface* s = SurfaceOf(*n, ctx, args[0]);
  const auto image = n->images.find(qe::ToUint32(ctx, args[1]));
  if (!s || image == n->images.end()) return qe::Undefined();
  auto rect = [&](size_t i) { return SkRect::MakeXYWH(static_cast<float>(Num(ctx, args, i)), static_cast<float>(Num(ctx, args, i + 1)), static_cast<float>(Num(ctx, args, i + 2)), static_cast<float>(Num(ctx, args, i + 3))); };
  s->DrawImage(image->second, rect(2).makeSorted(), rect(6).makeSorted());
  n->Touch(qe::ToUint32(ctx, args[0]));
  return qe::Undefined();
}

// ---- Pixels and files ----

Value GetImageData(Context& ctx, Value, qe::Args args, Value) {
  Natives* n = NativesOf(ctx);
  if (!n || args.size() < 5) return qe::Undefined();
  CanvasSurface* s = SurfaceOf(*n, ctx, args[0]);
  if (!s) return qe::Undefined();
  const int w = static_cast<int>(Num(ctx, args, 3)), h = static_cast<int>(Num(ctx, args, 4));
  if (static_cast<int64_t>(w) * h > (int64_t{1} << 28)) return qe::Undefined();
  const std::vector<uint8_t> pixels = s->GetImageData(static_cast<int>(Num(ctx, args, 1)), static_cast<int>(Num(ctx, args, 2)), w, h);
  return Bytes(ctx, pixels.data(), pixels.size());
}

// __solarCvPutImageData(id, bytes, w, h, dx, dy, sx, sy, sw, sh)
Value PutImageData(Context& ctx, Value, qe::Args args, Value) {
  Natives* n = NativesOf(ctx);
  if (!n || args.size() < 10) return qe::Undefined();
  CanvasSurface* s = SurfaceOf(*n, ctx, args[0]);
  auto bytes = qe::BytesOf(args[1]);
  if (!s || !bytes) return qe::Undefined();
  const int w = static_cast<int>(Num(ctx, args, 2)), h = static_cast<int>(Num(ctx, args, 3));
  if (bytes->size() < static_cast<size_t>(w) * h * 4) return qe::Undefined();
  s->PutImageData(bytes->data(), w, h, static_cast<int>(Num(ctx, args, 4)), static_cast<int>(Num(ctx, args, 5)), static_cast<int>(Num(ctx, args, 6)), static_cast<int>(Num(ctx, args, 7)),
                  static_cast<int>(Num(ctx, args, 8)), static_cast<int>(Num(ctx, args, 9)));
  n->Touch(qe::ToUint32(ctx, args[0]));
  return qe::Undefined();
}

// __solarImageEncode(isImage, id, mime, quality): the file of a surface or an image, as PNG, JPEG or WebP; undefined for a type this cannot make.
Value Encode(Context& ctx, Value, qe::Args args, Value) {
  Natives* n = NativesOf(ctx);
  if (!n || args.size() < 4) return qe::Undefined();
  sk_sp<SkImage> image;
  const uint32_t id = qe::ToUint32(ctx, args[1]);
  if (qe::ToBoolean(args[0])) {
    const auto found = n->images.find(id);
    if (found != n->images.end()) image = found->second;
  } else if (CanvasSurface* s = SurfaceOf(*n, ctx, args[1])) {
    image = s->Snapshot();
  }
  if (!image) return qe::Undefined();
  const std::string mime = qe::ToWtf8(ctx, args[2]);
  const double quality = Num(ctx, args, 3);
  SkBitmap bitmap;
  if (!bitmap.tryAllocPixels(SkImageInfo::MakeN32Premul(image->width(), image->height()))) return qe::Undefined();
  SkPixmap pixmap;
  bitmap.peekPixels(&pixmap);
  if (!image->readPixels(nullptr, pixmap, 0, 0)) return qe::Undefined();
  SkDynamicMemoryWStream stream;
  bool ok = false;
  if (mime == "image/jpeg") {
    SkJpegEncoder::Options options;
    options.fQuality = quality >= 0 && quality <= 1 ? static_cast<int>(quality * 100) : 92;
    // (JPEG has no alpha: what is transparent is black, as it is in a browser.)
    ok = SkJpegEncoder::Encode(&stream, pixmap, options);
  } else if (mime == "image/webp") {
    SkWebpEncoder::Options options;
    options.fQuality = quality >= 0 && quality <= 1 ? static_cast<float>(quality * 100) : 80.f;
    ok = SkWebpEncoder::Encode(&stream, pixmap, options);
  } else {
    ok = SkPngEncoder::Encode(&stream, pixmap, SkPngEncoder::Options());
  }
  if (!ok) return qe::Undefined();
  sk_sp<SkData> data = stream.detachAsData();
  return Bytes(ctx, data->data(), data->size());
}

}  // namespace

}  // namespace solar::paint

namespace solar::webapi {

void InstallCanvasNatives(Quanta::Embed::Realm& realm) {
  namespace p = solar::paint;
  Quanta::Context& ctx = realm.GetContext();
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarCanvasCreate", p::Create, 2);
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarCanvasResize", p::Resize, 3);
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarCanvasFree", p::Free, 1);
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarCanvasAttach", p::Attach, 2);
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarCv", p::Op, 10);
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarCvDash", p::Dash, 2);
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarCvRoundRect", p::RoundRect, 6);
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarCvStr", p::Str, 6);
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarCvStyle", p::Style, 3);
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarCvColor", p::ColorCall, 1);
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarCvPath", p::PathCall, 6);
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarCvAddPath", p::AddPath, 8);
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarCvDrawImage", p::DrawImageCall, 10);
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarCvGetImageData", p::GetImageData, 5);
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarCvPutImageData", p::PutImageData, 10);
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarImageDecode", p::ImageDecode, 1);
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarImageFromCanvas", p::ImageFromCanvas, 1);
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarImageFromPixels", p::ImageFromPixels, 3);
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarImageFromVideo", p::ImageFromVideo, 1);
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarImageFree", p::ImageFree, 1);
  Quanta::Embed::DefineGlobalFunction(ctx, "__solarImageEncode", p::Encode, 4);
}

}  // namespace solar::webapi
