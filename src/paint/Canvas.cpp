/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <regex>

#include "CanvasSurface.h"
#include "include/core/SkData.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkPathUtils.h"
#include "include/core/SkRRect.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/effects/SkImageFilters.h"
#include "include/ports/SkFontMgr_empty.h"
#include "solar/font/Database.h"

namespace solar::paint {

namespace {

constexpr double kPi = 3.14159265358979323846;

SkPathFillType FillTypeOf(bool evenOdd) { return evenOdd ? SkPathFillType::kEvenOdd : SkPathFillType::kWinding; }

std::optional<SkBlendMode> BlendOf(const std::string& name) {
  static const std::map<std::string, SkBlendMode> modes = {
      {"source-over", SkBlendMode::kSrcOver}, {"source-in", SkBlendMode::kSrcIn}, {"source-out", SkBlendMode::kSrcOut}, {"source-atop", SkBlendMode::kSrcATop},
      {"destination-over", SkBlendMode::kDstOver}, {"destination-in", SkBlendMode::kDstIn}, {"destination-out", SkBlendMode::kDstOut}, {"destination-atop", SkBlendMode::kDstATop},
      {"lighter", SkBlendMode::kPlus}, {"copy", SkBlendMode::kSrc}, {"xor", SkBlendMode::kXor}, {"multiply", SkBlendMode::kMultiply}, {"screen", SkBlendMode::kScreen},
      {"overlay", SkBlendMode::kOverlay}, {"darken", SkBlendMode::kDarken}, {"lighten", SkBlendMode::kLighten}, {"color-dodge", SkBlendMode::kColorDodge},
      {"color-burn", SkBlendMode::kColorBurn}, {"hard-light", SkBlendMode::kHardLight}, {"soft-light", SkBlendMode::kSoftLight}, {"difference", SkBlendMode::kDifference},
      {"exclusion", SkBlendMode::kExclusion}, {"hue", SkBlendMode::kHue}, {"saturation", SkBlendMode::kSaturation}, {"color", SkBlendMode::kColor}, {"luminosity", SkBlendMode::kLuminosity}};
  const auto found = modes.find(name);
  if (found == modes.end()) return std::nullopt;
  return found->second;
}

// Splits a font shorthand into what the canvas needs of it: the style, the weight, the size in pixels and the families.
struct FontSpec {
  double size = 10, weight = 400;
  font::SlantStyle style = font::SlantStyle::Normal;
  std::vector<std::string> families;
};

bool ParseFont(const std::string& text, FontSpec& out) {
  std::vector<std::string> tokens;
  std::string current;
  char quote = 0;
  for (char c : text) {
    if (quote) {
      current += c;
      if (c == quote) quote = 0;
    } else if (c == '"' || c == '\'') {
      quote = c;
      current += c;
    } else if (std::isspace(static_cast<unsigned char>(c))) {
      if (!current.empty()) tokens.push_back(std::move(current)), current.clear();
    } else {
      current += c;
    }
  }
  if (!current.empty()) tokens.push_back(std::move(current));
  static const std::regex sizePattern(R"(^([0-9]*\.?[0-9]+)(px|pt|pc|in|cm|mm|q|em|rem|ex|ch|%)?(?:/[^ ]+)?$)", std::regex::icase);
  static const std::map<std::string, double> keywords = {{"xx-small", 9}, {"x-small", 10}, {"small", 13}, {"medium", 16}, {"large", 18}, {"x-large", 24}, {"xx-large", 32}, {"smaller", 13}, {"larger", 19}};
  FontSpec spec;
  size_t i = 0;
  bool found = false;
  for (; i < tokens.size(); ++i) {
    std::string lower = tokens[i];
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::smatch m;
    if (std::regex_match(lower, m, sizePattern)) {
      const double v = std::atof(m[1].str().c_str());
      const std::string unit = m[2].str();
      double px = v;
      if (unit == "pt") px = v * 4 / 3;
      else if (unit == "pc") px = v * 16;
      else if (unit == "in") px = v * 96;
      else if (unit == "cm") px = v * 96 / 2.54;
      else if (unit == "mm") px = v * 96 / 25.4;
      else if (unit == "q") px = v * 96 / 101.6;
      else if (unit == "em" || unit == "rem") px = v * 16;
      else if (unit == "ex" || unit == "ch") px = v * 8;
      else if (unit == "%") px = v * 16 / 100;
      spec.size = px;
      found = true;
      ++i;
      break;
    }
    if (const auto k = keywords.find(lower); k != keywords.end()) {
      spec.size = k->second;
      found = true;
      ++i;
      break;
    }
    if (lower == "italic") spec.style = font::SlantStyle::Italic;
    else if (lower == "oblique") spec.style = font::SlantStyle::Oblique;
    else if (lower == "bold") spec.weight = 700;
    else if (lower == "bolder") spec.weight = 700;
    else if (lower == "lighter") spec.weight = 300;
    else if (lower == "normal" || lower == "small-caps") {}
    else if (lower.find_first_not_of("0123456789") == std::string::npos && !lower.empty()) spec.weight = std::atof(lower.c_str());
    else if (lower == "condensed" || lower == "expanded" || lower == "semi-condensed" || lower == "semi-expanded" || lower == "ultra-condensed" || lower == "extra-condensed" ||
             lower == "extra-expanded" || lower == "ultra-expanded") {}
    else return false;
  }
  if (!found || i >= tokens.size()) return false;
  std::string families;
  for (; i < tokens.size(); ++i) families += (families.empty() ? "" : " ") + tokens[i];
  std::string family;
  quote = 0;
  for (char c : families + ",") {
    if (quote) {
      if (c == quote) quote = 0;
      else family += c;
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == ',') {
      while (!family.empty() && std::isspace(static_cast<unsigned char>(family.back()))) family.pop_back();
      size_t from = 0;
      while (from < family.size() && std::isspace(static_cast<unsigned char>(family[from]))) ++from;
      if (from < family.size()) spec.families.push_back(family.substr(from));
      family.clear();
    } else {
      family += c;
    }
  }
  if (spec.families.empty()) return false;
  out = std::move(spec);
  return true;
}

struct TypefaceCache {
  sk_sp<SkFontMgr> manager = SkFontMgr_New_Custom_Empty();
  std::map<const font::Face*, std::pair<std::shared_ptr<font::Face>, sk_sp<SkTypeface>>> faces;
};
TypefaceCache& Typefaces() {
  static TypefaceCache cache;
  return cache;
}

SkPoint Mapped(const SkMatrix& m, double x, double y) { return m.mapPoint(SkPoint::Make(static_cast<float>(x), static_cast<float>(y))); }

// The sweep of an arc (the angles in radians), as the specification has it: a full circle at most, in the direction asked.
double SweepOf(double start, double end, bool ccw) {
  double sweep = end - start;
  if (!ccw) {
    if (sweep >= 2 * kPi) return 2 * kPi;
    if (sweep < 0) {
      sweep = std::fmod(sweep, 2 * kPi);
      if (sweep < 0) sweep += 2 * kPi;
    }
  } else {
    if (sweep <= -2 * kPi) return -2 * kPi;
    if (sweep > 0) {
      sweep = std::fmod(sweep, 2 * kPi);
      if (sweep > 0) sweep -= 2 * kPi;
    }
  }
  return sweep;
}

// An elliptical arc, as cubic curves of at most a quarter turn, the points of them mapped by the matrix; the first is joined to what is there by a line.
void AppendEllipse(SkPathBuilder& path, const SkMatrix& m, double cx, double cy, double rx, double ry, double rotation, double start, double end, bool ccw) {
  const double sweep = SweepOf(start, end, ccw);
  const double cr = std::cos(rotation), sr = std::sin(rotation);
  auto point = [&](double angle) {
    const double x = rx * std::cos(angle), y = ry * std::sin(angle);
    return Mapped(m, cx + x * cr - y * sr, cy + x * sr + y * cr);
  };
  auto tangent = [&](double angle) {  // the derivative wrt angle, mapped as a vector
    const double x = -rx * std::sin(angle), y = ry * std::cos(angle);
    const SkPoint a = Mapped(m, 0, 0), b = Mapped(m, x * cr - y * sr, x * sr + y * cr);
    return SkPoint::Make(b.fX - a.fX, b.fY - a.fY);
  };
  const SkPoint first = point(start);
  if (path.isEmpty()) path.moveTo(first);
  else path.lineTo(first);
  const int pieces = std::max(1, static_cast<int>(std::ceil(std::fabs(sweep) / (kPi / 2) - 1e-9)));
  const double step = sweep / pieces;
  const double k = 4.0 / 3.0 * std::tan(step / 4);
  double angle = start;
  for (int i = 0; i < pieces; ++i) {
    const double next = angle + step;
    const SkPoint p0 = point(angle), p3 = point(next);
    const SkPoint t0 = tangent(angle), t3 = tangent(next);
    path.cubicTo(p0.fX + static_cast<float>(k) * t0.fX, p0.fY + static_cast<float>(k) * t0.fY, p3.fX - static_cast<float>(k) * t3.fX, p3.fY - static_cast<float>(k) * t3.fY, p3.fX, p3.fY);
    angle = next;
  }
}

}  // namespace

sk_sp<SkTypeface> TypefaceFor(const std::shared_ptr<font::Face>& face) {
  TypefaceCache& cache = Typefaces();
  const auto found = cache.faces.find(face.get());
  if (found != cache.faces.end()) return found->second.second;
  sk_sp<SkTypeface> typeface;
  const std::string_view bytes = face->FileData();
  if (!bytes.empty()) {
    sk_sp<SkData> data = SkData::MakeWithoutCopy(bytes.data(), bytes.size());
    typeface = cache.manager->makeFromData(data, static_cast<int>(face->Index()));
    if (typeface && !face->Variations().empty()) {
      std::vector<SkFontArguments::VariationPosition::Coordinate> coordinates;
      for (const font::Face::Axis& axis : face->Variations()) {
        if (axis.tag.size() != 4) continue;
        coordinates.push_back({SkSetFourByteTag(axis.tag[0], axis.tag[1], axis.tag[2], axis.tag[3]), static_cast<float>(axis.value)});
      }
      SkFontArguments arguments;
      arguments.setVariationDesignPosition({coordinates.data(), static_cast<int>(coordinates.size())});
      if (sk_sp<SkTypeface> varied = typeface->makeClone(arguments)) typeface = varied;
    }
  }
  cache.faces[face.get()] = {face, typeface};
  return typeface;
}

sk_sp<SkImage> DecodeImage(const void* data, size_t size) {
  sk_sp<SkData> bytes = SkData::MakeWithCopy(data, size);
  sk_sp<SkImage> image = SkImages::DeferredFromEncodedData(bytes);
  if (!image) return nullptr;
  // (Decoded now, so that what is drawn is the pixels and not a promise of them.)
  return image->makeRasterImage(nullptr);
}

// ---- The surface ----

CanvasSurface::CanvasSurface(int width, int height) { Reset(width, height); }

void CanvasSurface::Reset(int width, int height) {
  width_ = std::max(0, width);
  height_ = std::max(0, height);
  surface_ = (width_ > 0 && height_ > 0) ? SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width_, height_)) : nullptr;
  canvas_ = surface_ ? surface_->getCanvas() : nullptr;
  if (!canvas_) {
    // (A canvas with no size has nothing to draw on: a one pixel surface takes what is drawn.)
    surface_ = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(1, 1));
    canvas_ = surface_->getCanvas();
  }
  canvas_->clear(SK_ColorTRANSPARENT);
  state_ = State();
  stack_.clear();
  path_ = PathWriter();
  SetFont("10px sans-serif");
  Mark();
}

void CanvasSurface::Save() {
  canvas_->save();
  stack_.push_back(state_);
}

void CanvasSurface::Restore() {
  if (stack_.empty()) return;
  canvas_->restore();
  state_ = std::move(stack_.back());
  stack_.pop_back();
}

void CanvasSurface::Transform(double a, double b, double c, double d, double e, double f) {
  if (!std::isfinite(a + b + c + d + e + f)) return;
  // (The matrix of the canvas, in rows: a c e / b d f.)
  const SkScalar values[9] = {static_cast<float>(a), static_cast<float>(c), static_cast<float>(e), static_cast<float>(b), static_cast<float>(d), static_cast<float>(f), 0, 0, 1};
  canvas_->concat(SkMatrix::MakeAll(values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7], values[8]));
}

void CanvasSurface::SetTransform(double a, double b, double c, double d, double e, double f) {
  if (!std::isfinite(a + b + c + d + e + f)) return;
  canvas_->setMatrix(SkMatrix::MakeAll(static_cast<float>(a), static_cast<float>(c), static_cast<float>(e), static_cast<float>(b), static_cast<float>(d), static_cast<float>(f), 0, 0, 1));
}

void CanvasSurface::SetGlobalAlpha(double alpha) {
  if (alpha >= 0 && alpha <= 1) state_.alpha = static_cast<float>(alpha);
}

bool CanvasSurface::SetBlend(const std::string& operation) {
  if (const auto mode = BlendOf(operation)) {
    state_.blend = *mode;
    return true;
  }
  return false;
}

void CanvasSurface::SetShadow(double blur, double dx, double dy, SkColor4f color) {
  state_.shadowBlur = static_cast<float>(blur);
  state_.shadowX = static_cast<float>(dx);
  state_.shadowY = static_cast<float>(dy);
  state_.shadowColor = color;
}

bool CanvasSurface::SetFont(const std::string& text) {
  FontSpec spec;
  if (!ParseFont(text, spec)) return false;
  font::Request request;
  request.families = spec.families;
  request.weight = spec.weight;
  request.style = spec.style;
  sk_sp<SkTypeface> typeface;
  if (std::shared_ptr<font::Face> face = font::Database::System().Match(request)) typeface = TypefaceFor(face);
  if (!typeface) {
    request.families = {"sans-serif"};
    if (std::shared_ptr<font::Face> face = font::Database::System().Match(request)) typeface = TypefaceFor(face);
  }
  state_.fontText = text;
  state_.fontSize = static_cast<float>(spec.size);
  state_.typeface = typeface;
  return true;
}

SkFont CanvasSurface::MakeFont() const {
  SkFont font(state_.typeface, state_.fontSize);
  font.setEdging(SkFont::Edging::kAntiAlias);
  font.setSubpixel(true);
  font.setHinting(SkFontHinting::kNone);
  return font;
}

SkSamplingOptions CanvasSurface::Sampling() const {
  if (!state_.smooth) return SkSamplingOptions(SkFilterMode::kNearest);
  if (state_.quality >= 2) return SkSamplingOptions(SkCubicResampler::Mitchell());
  return SkSamplingOptions(SkFilterMode::kLinear, state_.quality >= 1 ? SkMipmapMode::kLinear : SkMipmapMode::kNone);
}

sk_sp<SkImageFilter> CanvasSurface::ShadowFilter(sk_sp<SkImageFilter> input) const {
  // The shadow is drawn under what is drawn (the canvas's blur is a standard deviation of half the value).
  const float sigma = state_.shadowBlur / 2;
  return SkImageFilters::DropShadow(state_.shadowX, state_.shadowY, sigma, sigma, state_.shadowColor, nullptr, std::move(input));
}

SkPaint CanvasSurface::MakePaint(bool stroke, bool) const {
  SkPaint paint;
  paint.setAntiAlias(true);
  const SkColor4f color = stroke ? state_.strokeColor : state_.fillColor;
  const sk_sp<SkShader>& shader = stroke ? state_.strokeShader : state_.fillShader;
  if (shader) {
    paint.setShader(shader);
    paint.setAlphaf(state_.alpha);
  } else {
    paint.setColor4f({color.fR, color.fG, color.fB, color.fA * state_.alpha});
  }
  paint.setBlendMode(state_.blend);
  if (stroke) {
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(state_.lineWidth);
    paint.setStrokeCap(state_.cap == 1 ? SkPaint::kRound_Cap : state_.cap == 2 ? SkPaint::kSquare_Cap : SkPaint::kButt_Cap);
    paint.setStrokeJoin(state_.join == 1 ? SkPaint::kRound_Join : state_.join == 2 ? SkPaint::kBevel_Join : SkPaint::kMiter_Join);
    paint.setStrokeMiter(state_.miter);
    if (state_.dash.size() >= 2) paint.setPathEffect(SkDashPathEffect::Make(SkSpan<const float>(state_.dash.data(), state_.dash.size()), state_.dashOffset));
  }
  sk_sp<SkImageFilter> filter = state_.filter;
  if (HasShadow()) filter = ShadowFilter(std::move(filter));
  if (filter) paint.setImageFilter(std::move(filter));
  return paint;
}

void CanvasSurface::MapPoint(double x, double y, SkPoint* out) const { *out = Mapped(canvas_->getTotalMatrix(), x, y); }

// ---- Path ----

void PathWriter::MoveTo(double x, double y) {
  if (!std::isfinite(x + y)) return;
  path.moveTo(Mapped(m, x, y));
}

void PathWriter::LineTo(double x, double y) {
  if (!std::isfinite(x + y)) return;
  const SkPoint p = Mapped(m, x, y);
  if (path.isEmpty()) path.moveTo(p);
  else path.lineTo(p);
}

void PathWriter::BezierCurveTo(double x1, double y1, double x2, double y2, double x, double y) {
  if (!std::isfinite(x1 + y1 + x2 + y2 + x + y)) return;
  if (path.isEmpty()) path.moveTo(Mapped(m, x1, y1));
  path.cubicTo(Mapped(m, x1, y1), Mapped(m, x2, y2), Mapped(m, x, y));
}

void PathWriter::QuadraticCurveTo(double x1, double y1, double x, double y) {
  if (!std::isfinite(x1 + y1 + x + y)) return;
  if (path.isEmpty()) path.moveTo(Mapped(m, x1, y1));
  path.quadTo(Mapped(m, x1, y1), Mapped(m, x, y));
}

void PathWriter::Ellipse(double x, double y, double rx, double ry, double rotation, double start, double end, bool ccw) {
  if (!std::isfinite(x + y + rx + ry + rotation + start + end) || rx < 0 || ry < 0) return;
  if (rx == 0 && ry == 0) {
    LineTo(x, y);
    return;
  }
  AppendEllipse(path, m, x, y, rx, ry, rotation, start, end, ccw);
}

void PathWriter::ArcTo(double x1, double y1, double x2, double y2, double r) {
  if (!std::isfinite(x1 + y1 + x2 + y2 + r) || r < 0) return;
  if (path.isEmpty()) path.moveTo(Mapped(m, x1, y1));
  SkMatrix inverse;
  if (!m.invert(&inverse)) return;
  const std::optional<SkPoint> last = path.getLastPt();
  const SkPoint p0 = last ? inverse.mapPoint(*last) : SkPoint::Make(static_cast<float>(x1), static_cast<float>(y1));
  const SkPoint p1 = SkPoint::Make(static_cast<float>(x1), static_cast<float>(y1)), p2 = SkPoint::Make(static_cast<float>(x2), static_cast<float>(y2));
  const double cross = (p1.fX - p0.fX) * (p2.fY - p1.fY) - (p1.fY - p0.fY) * (p2.fX - p1.fX);
  if (p0 == p1 || p1 == p2 || r == 0 || cross == 0) {
    path.lineTo(Mapped(m, x1, y1));
    return;
  }
  // The arc is worked out in user space (the radius is in it), then mapped.
  SkPathBuilder local;
  local.moveTo(p0);
  local.arcTo(p1, p2, static_cast<float>(r));
  path.addPath(local.snapshot(&m), SkPath::kExtend_AddPathMode);
}

void PathWriter::Rect(double x, double y, double w, double h) {
  if (!std::isfinite(x + y + w + h)) return;
  path.moveTo(Mapped(m, x, y));
  path.lineTo(Mapped(m, x + w, y));
  path.lineTo(Mapped(m, x + w, y + h));
  path.lineTo(Mapped(m, x, y + h));
  path.close();
  path.moveTo(Mapped(m, x, y));
}

void PathWriter::RoundRect(double x, double y, double w, double h, const double radii[8]) {
  if (!std::isfinite(x + y + w + h)) return;
  // The corners are radii (x then y) of the upper left, upper right, lower right and lower left; a box with a negative size is its mirror.
  if (w < 0) { x += w; w = -w; }
  if (h < 0) { y += h; h = -h; }
  double r[8];
  for (int i = 0; i < 8; ++i) r[i] = std::max(0.0, radii[i]);
  // (Radii that are too large for the sides are scaled down together.)
  double scale = 1;
  const double sides[4][2] = {{r[0] + r[2], w}, {r[4] + r[6], w}, {r[1] + r[7], h}, {r[3] + r[5], h}};
  for (const auto& side : sides) if (side[0] > side[1] && side[0] > 0) scale = std::min(scale, side[1] / side[0]);
  for (double& v : r) v *= scale;
  SkPathBuilder local;
  local.moveTo(static_cast<float>(x + r[0]), static_cast<float>(y));
  local.lineTo(static_cast<float>(x + w - r[2]), static_cast<float>(y));
  AppendEllipse(local, SkMatrix::I(), x + w - r[2], y + r[3], r[2], r[3], 0, -kPi / 2, 0, false);
  local.lineTo(static_cast<float>(x + w), static_cast<float>(y + h - r[5]));
  AppendEllipse(local, SkMatrix::I(), x + w - r[4], y + h - r[5], r[4], r[5], 0, 0, kPi / 2, false);
  local.lineTo(static_cast<float>(x + r[6]), static_cast<float>(y + h));
  AppendEllipse(local, SkMatrix::I(), x + r[6], y + h - r[7], r[6], r[7], 0, kPi / 2, kPi, false);
  local.lineTo(static_cast<float>(x), static_cast<float>(y + r[1]));
  AppendEllipse(local, SkMatrix::I(), x + r[0], y + r[1], r[0], r[1], 0, kPi, 3 * kPi / 2, false);
  local.close();
  path.addPath(local.snapshot(&m), SkPath::kAppend_AddPathMode);
  path.moveTo(Mapped(m, x, y));
}

// Path2D commands: a flat list of numbers, a verb then its arguments (0 move x y; 1 line x y; 2 cubic x1 y1 x2 y2 x y; 3 quad x1 y1 x y; 4 close; 5 ellipse x y rx ry rot start end ccw;
// 6 arcTo x1 y1 x2 y2 r; 7 rect x y w h; 8 roundRect x y w h and 8 radii; 9 matrix a b c d e f), written through the same calls the context's path has.
void PathWriter::AddCommands(const std::vector<double>& c) {
  const SkMatrix base = m;
  for (size_t i = 0; i < c.size();) {
    const int verb = static_cast<int>(c[i++]);
    auto need = [&](size_t n) { return i + n <= c.size(); };
    switch (verb) {
      case 0: if (!need(2)) return; MoveTo(c[i], c[i + 1]); i += 2; break;
      case 1: if (!need(2)) return; LineTo(c[i], c[i + 1]); i += 2; break;
      case 2: if (!need(6)) return; BezierCurveTo(c[i], c[i + 1], c[i + 2], c[i + 3], c[i + 4], c[i + 5]); i += 6; break;
      case 3: if (!need(4)) return; QuadraticCurveTo(c[i], c[i + 1], c[i + 2], c[i + 3]); i += 4; break;
      case 4: Close(); break;
      case 5: if (!need(8)) return; Ellipse(c[i], c[i + 1], c[i + 2], c[i + 3], c[i + 4], c[i + 5], c[i + 6], c[i + 7] != 0); i += 8; break;
      case 6: if (!need(5)) return; ArcTo(c[i], c[i + 1], c[i + 2], c[i + 3], c[i + 4]); i += 5; break;
      case 7: if (!need(4)) return; Rect(c[i], c[i + 1], c[i + 2], c[i + 3]); i += 4; break;
      case 8: if (!need(12)) return; RoundRect(c[i], c[i + 1], c[i + 2], c[i + 3], &c[i + 4]); i += 12; break;
      case 9: {
        // The commands after this one are through the matrix a b c d e f (a Path2D that was added to another with one).
        if (!need(6)) return;
        const SkMatrix extra = SkMatrix::MakeAll(static_cast<float>(c[i]), static_cast<float>(c[i + 2]), static_cast<float>(c[i + 4]), static_cast<float>(c[i + 1]), static_cast<float>(c[i + 3]), static_cast<float>(c[i + 5]), 0, 0, 1);
        m = base;
        m.preConcat(extra);
        i += 6;
        break;
      }
      default: return;
    }
  }
}

SkPath PathFromCommands(const std::vector<double>& commands) {
  PathWriter writer;
  writer.AddCommands(commands);
  return writer.path.detach();
}

void CanvasSurface::MoveTo(double x, double y) { path_.m = canvas_->getTotalMatrix(); path_.MoveTo(x, y); }
void CanvasSurface::LineTo(double x, double y) { path_.m = canvas_->getTotalMatrix(); path_.LineTo(x, y); }
void CanvasSurface::BezierCurveTo(double x1, double y1, double x2, double y2, double x, double y) { path_.m = canvas_->getTotalMatrix(); path_.BezierCurveTo(x1, y1, x2, y2, x, y); }
void CanvasSurface::QuadraticCurveTo(double x1, double y1, double x, double y) { path_.m = canvas_->getTotalMatrix(); path_.QuadraticCurveTo(x1, y1, x, y); }
void CanvasSurface::Ellipse(double x, double y, double rx, double ry, double rotation, double start, double end, bool ccw) { path_.m = canvas_->getTotalMatrix(); path_.Ellipse(x, y, rx, ry, rotation, start, end, ccw); }
void CanvasSurface::ArcTo(double x1, double y1, double x2, double y2, double r) { path_.m = canvas_->getTotalMatrix(); path_.ArcTo(x1, y1, x2, y2, r); }
void CanvasSurface::Rect(double x, double y, double w, double h) { path_.m = canvas_->getTotalMatrix(); path_.Rect(x, y, w, h); }
void CanvasSurface::RoundRect(double x, double y, double w, double h, const double radii[8]) { path_.m = canvas_->getTotalMatrix(); path_.RoundRect(x, y, w, h, radii); }

// A Path2D added to the current path: its commands, through the matrix of now and `extra` (what the page gave addPath).
void CanvasSurface::AddPathCommands(const std::vector<double>& commands, const SkMatrix& matrix) {
  PathWriter writer;
  writer.m = matrix;
  writer.AddCommands(commands);
  path_.path.addPath(writer.path.detach(), SkPath::kAppend_AddPathMode);
}

// The path to draw: the current one (in canvas space) taken back to the space of the matrix of now, or a Path2D's, which is in that space already.
SkPath CanvasSurface::UserPath(const SkPath* path, SkPathFillType fill) {
  if (path) {
    SkPath copy = *path;
    copy.setFillType(fill);
    return copy;
  }
  SkMatrix inverse;
  if (!canvas_->getTotalMatrix().invert(&inverse)) return SkPath();
  SkPathBuilder copy = path_.path;
  copy.setFillType(fill);
  SkPath device = copy.snapshot();
  return device.makeTransform(inverse);
}

void CanvasSurface::Fill(const SkPath* path, bool evenOdd) {
  SkPath user = UserPath(path, FillTypeOf(evenOdd));
  if (user.isEmpty()) return;
  canvas_->drawPath(user, MakePaint(false));
  Mark();
}

void CanvasSurface::Stroke(const SkPath* path) {
  SkPath user = UserPath(path, SkPathFillType::kWinding);
  if (user.isEmpty()) return;
  canvas_->drawPath(user, MakePaint(true));
  Mark();
}

void CanvasSurface::Clip(const SkPath* path, bool evenOdd) {
  SkPath user = UserPath(path, FillTypeOf(evenOdd));
  canvas_->clipPath(user, SkClipOp::kIntersect, true);
}

bool CanvasSurface::InPath(const SkPath* path, double x, double y, bool evenOdd) {
  const SkPath user = UserPath(path, FillTypeOf(evenOdd));
  return user.contains(static_cast<float>(x), static_cast<float>(y));
}

bool CanvasSurface::InStroke(const SkPath* path, double x, double y) {
  const SkPath user = UserPath(path, SkPathFillType::kWinding);
  if (user.isEmpty()) return false;
  SkPaint paint = MakePaint(true);
  paint.setImageFilter(nullptr);
  SkPathBuilder outline;
  if (!skpathutils::FillPathWithPaint(user, paint, &outline)) return false;
  return outline.detach().contains(static_cast<float>(x), static_cast<float>(y));
}

void CanvasSurface::FillRect(double x, double y, double w, double h) {
  if (!std::isfinite(x + y + w + h) || w == 0 || h == 0) return;
  canvas_->drawRect(SkRect::MakeXYWH(static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h)).makeSorted(), MakePaint(false));
  Mark();
}

void CanvasSurface::StrokeRect(double x, double y, double w, double h) {
  if (!std::isfinite(x + y + w + h)) return;
  canvas_->drawRect(SkRect::MakeXYWH(static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h)).makeSorted(), MakePaint(true));
  Mark();
}

void CanvasSurface::ClearRect(double x, double y, double w, double h) {
  if (!std::isfinite(x + y + w + h)) return;
  SkPaint paint;
  paint.setBlendMode(SkBlendMode::kClear);
  paint.setAntiAlias(true);
  canvas_->drawRect(SkRect::MakeXYWH(static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h)).makeSorted(), paint);
  Mark();
}

// ---- Text ----

void CanvasSurface::DrawText(const std::string& text, double x, double y, std::optional<double> maxWidth, bool stroke) {
  if (text.empty() || !state_.typeface || !std::isfinite(x + y)) return;
  const SkFont font = MakeFont();
  float width = font.measureText(text.data(), text.size(), SkTextEncoding::kUTF8);
  float scale = 1;
  if (maxWidth && *maxWidth >= 0 && width > *maxWidth && width > 0) scale = static_cast<float>(*maxWidth / width);
  if (maxWidth && *maxWidth <= 0) return;
  float left = static_cast<float>(x);
  // Alignment: start and end follow the direction (left to right here); the position is then where the text begins.
  const int align = state_.textAlign;
  const bool rtl = state_.direction == 1;
  const bool endSide = align == 1 ? !rtl : align == 0 ? rtl : false;
  if (align == 3 || (align == 0 && rtl)) left -= width * scale;       // right
  else if (align == 4) left -= width * scale / 2;                      // center
  else if (align == 1 && !rtl) left -= width * scale;                  // end (left to right): ends at x
  (void)endSide;
  SkFontMetrics metrics;
  font.getMetrics(&metrics);
  float baseline = static_cast<float>(y);
  switch (state_.textBaseline) {
    case 1: baseline -= metrics.fAscent; break;                              // top
    case 2: baseline -= metrics.fAscent * 0.8f; break;                       // hanging
    case 3: baseline -= (metrics.fAscent + metrics.fDescent) / 2; break;    // middle
    case 4: baseline -= metrics.fDescent; break;                             // ideographic
    case 5: baseline -= metrics.fDescent; break;                             // bottom
    default: break;                                                          // alphabetic
  }
  canvas_->save();
  if (scale != 1) {
    canvas_->translate(left, baseline);
    canvas_->scale(scale, 1);
    canvas_->translate(-left, -baseline);
  }
  canvas_->drawSimpleText(text.data(), text.size(), SkTextEncoding::kUTF8, left, baseline, font, MakePaint(stroke));
  canvas_->restore();
  Mark();
}

std::string CanvasSurface::Measure(const std::string& text) {
  std::string json;
  const SkFont font = MakeFont();
  SkFontMetrics metrics;
  font.getMetrics(&metrics);
  SkRect bounds;
  const float width = font.measureText(text.data(), text.size(), SkTextEncoding::kUTF8, &bounds);
  float left = 0;
  if (state_.textAlign == 3 || (state_.textAlign == 0 && state_.direction == 1) || (state_.textAlign == 1 && state_.direction == 0)) left = width;
  else if (state_.textAlign == 4) left = width / 2;
  float baseline = 0;
  switch (state_.textBaseline) {
    case 1: baseline = -metrics.fAscent; break;
    case 2: baseline = -metrics.fAscent * 0.8f; break;
    case 3: baseline = -(metrics.fAscent + metrics.fDescent) / 2; break;
    case 4: case 5: baseline = -metrics.fDescent; break;
    default: break;
  }
  auto n = [](float v) { return std::to_string(v); };
  json = "{\"width\":" + n(width) + ",\"actualBoundingBoxLeft\":" + n(left - bounds.fLeft) + ",\"actualBoundingBoxRight\":" + n(bounds.fRight - left) +
         ",\"actualBoundingBoxAscent\":" + n(-bounds.fTop - baseline) + ",\"actualBoundingBoxDescent\":" + n(bounds.fBottom + baseline) +
         ",\"fontBoundingBoxAscent\":" + n(-metrics.fAscent - baseline) + ",\"fontBoundingBoxDescent\":" + n(metrics.fDescent + baseline) +
         ",\"emHeightAscent\":" + n(-metrics.fAscent - baseline) + ",\"emHeightDescent\":" + n(metrics.fDescent + baseline) + ",\"hangingBaseline\":" + n(-metrics.fAscent * 0.8f - baseline) +
         ",\"alphabeticBaseline\":" + n(-baseline) + ",\"ideographicBaseline\":" + n(metrics.fDescent - baseline) + "}";
  return json;
}

// ---- Images and pixels ----

void CanvasSurface::DrawImage(const sk_sp<SkImage>& image, SkRect source, SkRect destination) {
  if (!image || source.isEmpty() || destination.isEmpty()) return;
  SkPaint paint = MakePaint(false);
  paint.setShader(nullptr);
  paint.setAlphaf(state_.alpha);
  paint.setStyle(SkPaint::kFill_Style);
  canvas_->drawImageRect(image, source, destination, Sampling(), &paint, SkCanvas::kStrict_SrcRectConstraint);
  Mark();
}

std::vector<uint8_t> CanvasSurface::GetImageData(int x, int y, int w, int h) {
  std::vector<uint8_t> out(static_cast<size_t>(std::max(0, w)) * std::max(0, h) * 4, 0);
  if (w <= 0 || h <= 0 || !surface_) return out;
  const SkImageInfo info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
  surface_->readPixels(info, out.data(), static_cast<size_t>(w) * 4, x, y);
  return out;
}

void CanvasSurface::PutImageData(const uint8_t* rgba, int w, int h, int dx, int dy, int sx, int sy, int sw, int sh) {
  if (!surface_ || w <= 0 || h <= 0) return;
  sx = std::clamp(sx, 0, w);
  sy = std::clamp(sy, 0, h);
  sw = std::min(sw, w - sx);
  sh = std::min(sh, h - sy);
  if (sw <= 0 || sh <= 0) return;
  const SkImageInfo info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
  SkBitmap bitmap;
  bitmap.setInfo(info);
  bitmap.setPixels(const_cast<uint8_t*>(rgba));
  // (Written as they are: not blended with what was there, and not through the matrix or the clip.)
  SkPixmap pixmap(info, rgba, static_cast<size_t>(w) * 4);
  SkPixmap sub;
  if (!pixmap.extractSubset(&sub, SkIRect::MakeXYWH(sx, sy, sw, sh))) return;
  surface_->writePixels(sub, dx + sx, dy + sy);
  Mark();
}

void CanvasSurface::SetContents(const sk_sp<SkImage>& image, bool flip) {
  if (!canvas_ || !image) return;
  canvas_->save();
  canvas_->resetMatrix();
  canvas_->clear(SK_ColorTRANSPARENT);
  if (flip) {
    canvas_->translate(0, static_cast<float>(height_));
    canvas_->scale(1, -1);
  }
  SkPaint paint;
  paint.setBlendMode(SkBlendMode::kSrc);
  canvas_->drawImage(image, 0, 0, SkSamplingOptions(), &paint);
  canvas_->restore();
  Mark();
}

void CanvasSurface::Publish() {
  sk_sp<SkImage> image = surface_ ? surface_->makeImageSnapshot() : nullptr;
  {
    std::lock_guard lock(latestMutex_);
    latest_ = std::move(image);
  }
  dirty = false;
}

sk_sp<SkImage> CanvasSurface::Latest() const {
  std::lock_guard lock(latestMutex_);
  return latest_;
}

}  // namespace solar::paint
