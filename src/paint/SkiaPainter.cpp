/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// A display list drawn with Skia.
#include "solar/paint/SkiaPainter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkFontArguments.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkImage.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/effects/SkGradient.h"
#include "include/core/SkM44.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkRRect.h"
#include "include/core/SkTextBlob.h"
#include "include/core/SkTypeface.h"
#include "include/ports/SkFontMgr_empty.h"
#include "modules/svg/include/SkSVGDOM.h"
#include "modules/svg/include/SkSVGSVG.h"
#include <map>

namespace solar::paint {

namespace {

SkColor ToSk(const Color& c) { return SkColorSetARGB(c.a, c.r, c.g, c.b); }

SkRRect ToSk(const RoundedRect& r) {
  const SkRect rect = SkRect::MakeXYWH(r.rect.x, r.rect.y, r.rect.width, r.rect.height);
  if (!r.Rounded()) return SkRRect::MakeRect(rect);
  SkVector radii[4] = {{r.radii[0], r.radii[1]}, {r.radii[2], r.radii[3]}, {r.radii[4], r.radii[5]}, {r.radii[6], r.radii[7]}};
  SkRRect out;
  out.setRectRadii(rect, radii);
  return out;
}

// The shape made larger by `d` on every side (smaller where negative): its corners follow.
SkRRect Outset(const SkRRect& r, float d) {
  const SkRect rect = r.rect().makeOutset(d, d);
  if (r.isRect()) return SkRRect::MakeRect(rect);
  SkVector radii[4];
  for (int i = 0; i < 4; ++i) {
    const SkVector v = r.radii(static_cast<SkRRect::Corner>(i));
    radii[i] = {v.x() > 0 ? std::max(0.0f, v.x() + d) : 0.0f, v.y() > 0 ? std::max(0.0f, v.y() + d) : 0.0f};
  }
  SkRRect out;
  out.setRectRadii(rect, radii);
  return out;
}

sk_sp<SkMaskFilter> BlurOf(float blur) { return blur > 0 ? SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, blur / 2) : nullptr; }

}  // namespace

struct SkiaPainter::Impl {
  sk_sp<SkFontMgr> manager = SkFontMgr_New_Custom_Empty();
  struct Kept {
    std::shared_ptr<font::Face> face;  // (kept: the typeface reads from its file, and its address is not another face's)
    sk_sp<SkTypeface> typeface;
  };
  std::unordered_map<const font::Face*, Kept> typefaces;

  // A picture as the painter has it: decoded (a raster image) or parsed (an SVG, drawn at the size it is wanted at).
  struct Decoded {
    ImageRef source;  // (kept: it is what the cache is by)
    sk_sp<SkImage> raster;
    sk_sp<SkSVGDOM> svg;
    float width = 0, height = 0;  // its own size; 0 where it has none (an SVG with no width and height)
    float ratio = 0;
    std::map<std::pair<int, int>, sk_sp<SkImage>> rendered;  // an SVG at the sizes it was drawn at
    bool failed = false;
  };
  std::unordered_map<const ImageData*, Decoded> images;

  Decoded& DecodedOf(const ImageRef& source) {
    auto found = images.find(source.get());
    if (found != images.end()) return found->second;
    Decoded decoded;
    decoded.source = source;
    const std::string& bytes = *source->bytes;
    const std::string head = bytes.substr(0, 4096);
    std::string lowered;
    for (char c : head) lowered += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lowered.find("<svg") != std::string::npos) {
      SkMemoryStream stream(bytes.data(), bytes.size(), false);
      decoded.svg = SkSVGDOM::Builder().setFontManager(manager).make(stream);
      if (decoded.svg) {
        const SkSize size = decoded.svg->containerSize();
        decoded.width = size.width();
        decoded.height = size.height();
        if (decoded.width <= 0 || decoded.height <= 0) {
          decoded.width = decoded.height = 0;
          const std::optional<SkSVGViewBoxType>& box = decoded.svg->getRoot()->getViewBox();
          if (box && box->width() > 0 && box->height() > 0) decoded.ratio = box->width() / box->height();
        } else {
          decoded.ratio = decoded.width / decoded.height;
        }
      } else {
        decoded.failed = true;
      }
    } else {
      decoded.raster = SkImages::DeferredFromEncodedData(SkData::MakeWithoutCopy(bytes.data(), bytes.size()));
      if (decoded.raster) {
        decoded.width = static_cast<float>(decoded.raster->width());
        decoded.height = static_cast<float>(decoded.raster->height());
        decoded.ratio = decoded.height > 0 ? decoded.width / decoded.height : 0;
      } else {
        decoded.failed = true;
      }
    }
    if (std::getenv("SOLAR_PAINT_DEBUG")) std::fprintf(stderr, "decode %s: %s (%zu bytes, %gx%g)\n", source->url.c_str(), decoded.failed ? "FAILED" : decoded.svg ? "svg" : "raster", bytes.size(), decoded.width, decoded.height);
    return images.emplace(source.get(), std::move(decoded)).first->second;
  }

  // The picture drawn at a size in pixels: the image itself, or an SVG made into one.
  sk_sp<SkImage> At(Decoded& decoded, int width, int height) {
    if (decoded.raster) return decoded.raster;
    if (!decoded.svg || width <= 0 || height <= 0 || width > 8192 || height > 8192) return nullptr;
    if (decoded.rendered.size() > 6 && !decoded.rendered.count({width, height})) decoded.rendered.clear();
    auto& slot = decoded.rendered[{width, height}];
    if (!slot) {
      sk_sp<SkSurface> surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
      if (!surface) return nullptr;
      surface->getCanvas()->clear(SK_ColorTRANSPARENT);
      decoded.svg->setContainerSize(SkSize::Make(static_cast<float>(width), static_cast<float>(height)));
      decoded.svg->render(surface->getCanvas());
      slot = surface->makeImageSnapshot();
    }
    return slot;
  }


  // ---- Gradients ----

  sk_sp<SkShader> GradientShader(const Gradient& g, const SkRect& t) {
    const float w = t.width(), h = t.height();
    const float cx = t.left() + g.centerX.Of(w), cy = t.top() + g.centerY.Of(h);
    // The geometry first: how long the line is that the stops are along (a percent of a turn for a conic one).
    SkPoint start{0, 0}, end{0, 0};
    float length = 1, rx = 1, ry = 1;
    float startAngle = 0;
    if (g.kind == Gradient::Kind::Linear) {
      float dx, dy;
      if (g.toCorner) {
        const float n = std::hypot(h, w);
        dx = g.cornerX * h / n;
        dy = g.cornerY * w / n;
      } else {
        const float a = g.angle * static_cast<float>(M_PI) / 180;
        dx = std::sin(a);
        dy = -std::cos(a);
      }
      length = std::fabs(w * dx) + std::fabs(h * dy);
      const float mx = t.centerX(), my = t.centerY();
      start = {mx - dx * length / 2, my - dy * length / 2};
      end = {mx + dx * length / 2, my + dy * length / 2};
    } else if (g.kind == Gradient::Kind::Radial) {
      const float left = cx - t.left(), right = t.right() - cx, top = cy - t.top(), bottom = t.bottom() - cy;
      const float nearX = std::min(std::fabs(left), std::fabs(right)), farX = std::max(std::fabs(left), std::fabs(right));
      const float nearY = std::min(std::fabs(top), std::fabs(bottom)), farY = std::max(std::fabs(top), std::fabs(bottom));
      switch (g.extent) {
        case Gradient::Extent::Lengths:
          rx = g.radiusX.Of(w);
          ry = g.circle ? rx : g.radiusY.Of(h);
          break;
        case Gradient::Extent::ClosestSide:
          if (g.circle) rx = ry = std::min(nearX, nearY); else { rx = nearX; ry = nearY; }
          break;
        case Gradient::Extent::FarthestSide:
          if (g.circle) rx = ry = std::max(farX, farY); else { rx = farX; ry = farY; }
          break;
        case Gradient::Extent::ClosestCorner:
          if (g.circle) rx = ry = std::hypot(nearX, nearY); else { rx = nearX * 1.41421356f; ry = nearY * 1.41421356f; }
          break;
        case Gradient::Extent::FarthestCorner:
          if (g.circle) rx = ry = std::hypot(farX, farY); else { rx = farX * 1.41421356f; ry = farY * 1.41421356f; }
          break;
      }
      rx = std::max(rx, 0.001f);
      ry = std::max(ry, 0.001f);
      length = rx;
    } else {
      length = 100;  // (positions are percents of the turn)
      startAngle = g.angle - 90;
    }
    // The stops, as fractions of the line: those with no position are spread between the ones around them, and none goes back.
    const size_t n = g.stops.size();
    std::vector<float> pos(n, std::nanf(""));
    for (size_t i = 0; i < n; ++i) {
      if (g.stops[i].positioned) pos[i] = g.kind == Gradient::Kind::Conic ? g.stops[i].position.percent / 100 : g.stops[i].position.px / length + g.stops[i].position.percent / 100;
    }
    if (std::isnan(pos.front())) pos.front() = 0;
    if (std::isnan(pos.back())) pos.back() = std::max(1.0f, pos[n - 2] == pos[n - 2] ? pos[n - 2] : 1.0f);
    for (size_t i = 1; i < n; ++i) {
      if (!std::isnan(pos[i])) continue;
      size_t j = i;
      while (std::isnan(pos[j])) ++j;
      for (size_t k = i; k < j; ++k) pos[k] = pos[i - 1] + (pos[j] - pos[i - 1]) * static_cast<float>(k - i + 1) / static_cast<float>(j - i + 1);
      i = j;
    }
    for (size_t i = 1; i < n; ++i) pos[i] = std::max(pos[i], pos[i - 1]);
    std::vector<SkColor4f> colors;
    for (const ColorStop& stop : g.stops) colors.push_back(SkColor4f::FromColor(ToSk(stop.color)));
    SkGradient::Interpolation interpolation;
    interpolation.fInPremul = SkGradient::Interpolation::InPremul::kYes;
    float t0 = 0, t1 = 1;
    SkTileMode mode = SkTileMode::kClamp;
    if (g.repeating) {
      t0 = pos.front();
      t1 = pos.back();
      if (t1 - t0 < 1e-5f) return SkShaders::Color(colors.back(), nullptr);
      for (float& p : pos) p = (p - t0) / (t1 - t0);
      mode = SkTileMode::kRepeat;
    }
    const SkGradient gradient(SkGradient::Colors(colors, pos, mode), interpolation);
    if (g.kind == Gradient::Kind::Linear) {
      const SkPoint points[2] = {{start.x() + (end.x() - start.x()) * t0, start.y() + (end.y() - start.y()) * t0}, {start.x() + (end.x() - start.x()) * t1, start.y() + (end.y() - start.y()) * t1}};
      return SkShaders::LinearGradient(points, gradient);
    }
    if (g.kind == Gradient::Kind::Radial) {
      SkMatrix scale = SkMatrix::Translate(cx, cy);
      scale.preScale(1, ry / rx);
      scale.preTranslate(-cx, -cy);
      if (g.repeating && t0 > 0) return SkShaders::TwoPointConicalGradient({cx, cy}, rx * t0, {cx, cy}, rx * t1, gradient, &scale);
      return SkShaders::RadialGradient({cx, cy}, rx * t1, gradient, &scale);
    }
    return SkShaders::SweepGradient({cx, cy}, startAngle, startAngle + 360, gradient);
  }

  void DrawTile(SkCanvas& canvas, Decoded& decoded, const SkRect& destination, bool pixelated) {
    if (destination.width() <= 0 || destination.height() <= 0) return;
    const float scale = std::max(1.0f, canvas.getLocalToDevice().rc(0, 0));
    sk_sp<SkImage> image = At(decoded, static_cast<int>(std::ceil(destination.width() * scale)), static_cast<int>(std::ceil(destination.height() * scale)));
    if (!image) return;
    const SkSamplingOptions sampling = pixelated ? SkSamplingOptions(SkFilterMode::kNearest) : SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear);
    canvas.drawImageRect(image, destination, sampling);
  }

  void DrawLayer(SkCanvas& canvas, const ImageLayer& layer) {
    Decoded* decoded = nullptr;
    if (layer.image && layer.image->bytes) {
      decoded = &DecodedOf(layer.image);
      if (decoded->failed) return;
    } else if (!layer.gradient) {
      return;
    }
    const RectF o = layer.origin;
    if (o.width <= 0 || o.height <= 0) return;
    // How large one picture is.
    float w = 0, h = 0;
    const float iw = decoded ? decoded->width : 0, ih = decoded ? decoded->height : 0, ratio = decoded ? decoded->ratio : 0;
    switch (layer.size) {
      case ImageLayer::Size::Cover:
      case ImageLayer::Size::Contain: {
        const float rr = ratio > 0 ? ratio : (o.height > 0 ? o.width / o.height : 1);
        const bool wider = o.width / o.height > rr;  // the area is wider than the picture
        const bool fitWidth = (layer.size == ImageLayer::Size::Contain) ? !wider : wider;
        if (fitWidth) { w = o.width; h = w / rr; } else { h = o.height; w = h * rr; }
        break;
      }
      case ImageLayer::Size::Lengths: {
        const bool autoW = layer.width.automatic, autoH = layer.height.automatic;
        w = autoW ? 0 : layer.width.Of(o.width);
        h = autoH ? 0 : layer.height.Of(o.height);
        if (autoW && autoH) { w = iw; h = ih; }
        else if (autoW) w = ratio > 0 ? h * ratio : iw;
        else if (autoH) h = ratio > 0 ? w / ratio : ih;
        break;
      }
      default:
        w = iw;
        h = ih;
        if (w <= 0 && h <= 0) { w = o.width; h = ratio > 0 ? w / ratio : o.height; }
        else if (w <= 0) w = ratio > 0 ? h * ratio : o.width;
        else if (h <= 0) h = ratio > 0 ? w / ratio : o.height;
        break;
    }
    if (!decoded) {  // (a gradient has no size of its own: it is as large as the area)
      if (w <= 0) w = o.width;
      if (h <= 0) h = o.height;
    }
    if (w <= 0 || h <= 0) return;
    // `round` makes the picture fit a whole number of times, `space` spreads the whole ones.
    float gapX = 0, gapY = 0;
    if (layer.repeatX == Repeat::Round) w = o.width / std::max(1.0f, std::round(o.width / w));
    if (layer.repeatY == Repeat::Round) h = o.height / std::max(1.0f, std::round(o.height / h));
    float x = layer.x.Of(o.width - w), y = layer.y.Of(o.height - h);
    Repeat repeatX = layer.repeatX, repeatY = layer.repeatY;
    if (repeatX == Repeat::Space) {
      const int n = static_cast<int>(std::floor(o.width / w));
      if (n >= 2) { gapX = (o.width - n * w) / (n - 1); x = 0; } else repeatX = Repeat::NoRepeat;
    }
    if (repeatY == Repeat::Space) {
      const int n = static_cast<int>(std::floor(o.height / h));
      if (n >= 2) { gapY = (o.height - n * h) / (n - 1); y = 0; } else repeatY = Repeat::NoRepeat;
    }
    const SkRRect clip = ToSk(layer.clip);
    const SkRect bounds = clip.getBounds();
    const float stepX = w + gapX, stepY = h + gapY;
    // The tiles that are in the area.
    int firstX = 0, lastX = 0, firstY = 0, lastY = 0;
    const float startX = o.x + x, startY = o.y + y;
    if (repeatX != Repeat::NoRepeat) {
      firstX = static_cast<int>(std::floor((bounds.left() - startX) / stepX));
      lastX = static_cast<int>(std::ceil((bounds.right() - startX) / stepX));
    }
    if (repeatY != Repeat::NoRepeat) {
      firstY = static_cast<int>(std::floor((bounds.top() - startY) / stepY));
      lastY = static_cast<int>(std::ceil((bounds.bottom() - startY) / stepY));
    }
    if (static_cast<long>(lastX - firstX + 1) * (lastY - firstY + 1) > 20000) return;
    canvas.save();
    if (clip.isRect()) canvas.clipRect(bounds, true);
    else canvas.clipRRect(clip, true);
    for (int j = firstY; j <= lastY; ++j) {
      for (int i = firstX; i <= lastX; ++i) {
        const SkRect tile = SkRect::MakeXYWH(startX + i * stepX, startY + j * stepY, w, h);
        if (decoded) {
          DrawTile(canvas, *decoded, tile, layer.pixelated);
        } else {
          SkPaint paint;
          paint.setAntiAlias(false);
          paint.setShader(GradientShader(*layer.gradient, tile));
          canvas.drawRect(tile, paint);
        }
      }
    }
    canvas.restore();
  }

  void DrawElementImage(SkCanvas& canvas, const DrawImage& item) {
    if (!item.image || !item.image->bytes) return;
    Decoded& decoded = DecodedOf(item.image);
    if (decoded.failed) return;
    const SkRRect clip = ToSk(item.clip);
    const SkRect box = clip.getBounds();
    if (box.width() <= 0 || box.height() <= 0) return;
    float iw = decoded.width, ih = decoded.height;
    if (iw <= 0 || ih <= 0) {
      iw = box.width();
      ih = decoded.ratio > 0 ? iw / decoded.ratio : box.height();
    }
    float w = box.width(), h = box.height();
    const float contain = std::min(box.width() / iw, box.height() / ih), cover = std::max(box.width() / iw, box.height() / ih);
    switch (item.fit) {
      case ObjectFit::Contain: w = iw * contain; h = ih * contain; break;
      case ObjectFit::Cover: w = iw * cover; h = ih * cover; break;
      case ObjectFit::None: w = iw; h = ih; break;
      case ObjectFit::ScaleDown: w = iw * std::min(1.0f, contain); h = ih * std::min(1.0f, contain); break;
      default: break;
    }
    const float x = box.left() + item.x.Of(box.width() - w), y = box.top() + item.y.Of(box.height() - h);
    canvas.save();
    if (clip.isRect()) canvas.clipRect(box, true);
    else canvas.clipRRect(clip, true);
    DrawTile(canvas, decoded, SkRect::MakeXYWH(x, y, w, h), item.pixelated);
    canvas.restore();
  }

  sk_sp<SkTypeface> TypefaceOf(const std::shared_ptr<font::Face>& face) {
    const auto found = typefaces.find(face.get());
    if (found != typefaces.end()) return found->second.typeface;
    sk_sp<SkTypeface> typeface;
    const std::string_view bytes = face->FileData();
    if (!bytes.empty()) {
      sk_sp<SkData> data = SkData::MakeWithoutCopy(bytes.data(), bytes.size());
      typeface = manager->makeFromData(data, static_cast<int>(face->Index()));
      if (typeface && !face->Variations().empty()) {
        std::vector<SkFontArguments::VariationPosition::Coordinate> coordinates;
        for (const font::Face::Axis& axis : face->Variations()) {
          if (axis.tag.size() != 4) continue;
          const SkFourByteTag tag = SkSetFourByteTag(axis.tag[0], axis.tag[1], axis.tag[2], axis.tag[3]);
          coordinates.push_back({tag, static_cast<float>(axis.value)});
        }
        SkFontArguments arguments;
        arguments.setVariationDesignPosition({coordinates.data(), static_cast<int>(coordinates.size())});
        if (sk_sp<SkTypeface> varied = typeface->makeClone(arguments)) typeface = varied;
      }
    }
    typefaces[face.get()] = {face, typeface};
    return typeface;
  }
};

void SkiaPainter::Forget() {
  impl_->images.clear();
  impl_->typefaces.clear();
}

SkiaPainter::SkiaPainter() : impl_(std::make_unique<Impl>()) {}
SkiaPainter::~SkiaPainter() = default;

namespace {

// The rounded rectangle moved in on each side by its own amount (negative: out); a corner radius shrinks with the sides it touches.
SkRRect InsetEach(const SkRRect& r, float top, float right, float bottom, float left) {
  const SkRect rect = r.rect();
  SkRect moved = SkRect::MakeLTRB(rect.left() + left, rect.top() + top, rect.right() - right, rect.bottom() - bottom);
  if (moved.right() < moved.left()) moved.fLeft = moved.fRight = (moved.left() + moved.right()) / 2;
  if (moved.bottom() < moved.top()) moved.fTop = moved.fBottom = (moved.top() + moved.bottom()) / 2;
  const auto shrink = [](SkVector v, float dx, float dy) { return SkVector{std::max(0.f, v.fX - dx), std::max(0.f, v.fY - dy)}; };
  SkVector radii[4] = {shrink(r.radii(SkRRect::kUpperLeft_Corner), left, top), shrink(r.radii(SkRRect::kUpperRight_Corner), right, top),
                       shrink(r.radii(SkRRect::kLowerRight_Corner), right, bottom), shrink(r.radii(SkRRect::kLowerLeft_Corner), left, bottom)};
  SkRRect out;
  out.setRectRadii(moved, radii);
  return out;
}

SkColor Shade(const Color& c, bool dark) {
  if (!dark) return ToSk(c);
  return SkColorSetARGB(c.a, c.r / 3, c.g / 3, c.b / 3);
}

// A side of a border: the part of the ring between the outer and the inner edge that is toward that side, cut along the diagonals of the corners.
void DrawBorderSide(SkCanvas& canvas, const DrawBorder& b, int side, const SkRRect& outer, const SkRRect& inner) {
  const Color color = b.colors[side];
  const LineStyle style = b.styles[side];
  const float w = b.widths[side];
  if (w <= 0 || style == LineStyle::None || style == LineStyle::Hidden || color.Transparent()) return;
  const SkRect o = outer.rect(), i = inner.rect();
  SkPathBuilder region;
  switch (side) {
    case 0: region.moveTo(o.left(), o.top()).lineTo(o.right(), o.top()).lineTo(i.right(), i.top()).lineTo(i.left(), i.top()).close(); break;
    case 1: region.moveTo(o.right(), o.top()).lineTo(o.right(), o.bottom()).lineTo(i.right(), i.bottom()).lineTo(i.right(), i.top()).close(); break;
    case 2: region.moveTo(o.right(), o.bottom()).lineTo(o.left(), o.bottom()).lineTo(i.left(), i.bottom()).lineTo(i.right(), i.bottom()).close(); break;
    default: region.moveTo(o.left(), o.bottom()).lineTo(o.left(), o.top()).lineTo(i.left(), i.top()).lineTo(i.left(), i.bottom()).close(); break;
  }
  const float tw = b.widths[0], rw = b.widths[1], bw = b.widths[2], lw = b.widths[3];
  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setColor(ToSk(color));
  canvas.save();
  canvas.clipPath(region.detach(), true);
  const auto ring = [&](const SkRRect& a, const SkRRect& c, SkColor fill) {
    paint.setColor(fill);
    canvas.drawDRRect(a, c, paint);
  };
  const bool topLeft = side == 0 || side == 3;
  switch (style) {
    case LineStyle::Dotted:
    case LineStyle::Dashed: {
      const SkRRect mid = InsetEach(outer, tw / 2, rw / 2, bw / 2, lw / 2);
      SkPathBuilder pathBuilder;
      pathBuilder.addRRect(mid);
      const SkPath path = pathBuilder.detach();
      const bool dots = style == LineStyle::Dotted;
      const float on = dots ? 0.001f : std::max(2 * w, 3.f), off = dots ? w : std::max(w, 2.f);
      const float dash[2] = {on, dots ? w : off};
      paint.setStyle(SkPaint::kStroke_Style);
      paint.setStrokeWidth(w);
      if (dots) paint.setStrokeCap(SkPaint::kRound_Cap);
      // Whole dashes along the side: the interval is stretched to fit when the side is straight.
      const float length = side % 2 == 0 ? o.width() : o.height();
      const float period = dash[0] + dash[1];
      const float count = std::max(1.f, std::round(length / period));
      const float scale = length / (count * period);
      const float scaled[2] = {dash[0] * scale, dash[1] * scale};
      paint.setPathEffect(SkDashPathEffect::Make(scaled, 0));
      canvas.drawPath(path, paint);
      break;
    }
    case LineStyle::Double: {
      if (w < 3) {
        ring(outer, inner, ToSk(color));
        break;
      }
      const float t = w / 3;
      ring(outer, InsetEach(outer, tw > 0 ? t : 0, rw > 0 ? t : 0, bw > 0 ? t : 0, lw > 0 ? t : 0), ToSk(color));
      ring(InsetEach(inner, tw > 0 ? -t : 0, rw > 0 ? -t : 0, bw > 0 ? -t : 0, lw > 0 ? -t : 0), inner, ToSk(color));
      break;
    }
    case LineStyle::Groove:
    case LineStyle::Ridge:
    case LineStyle::Inset:
    case LineStyle::Outset: {
      const bool inward = style == LineStyle::Groove || style == LineStyle::Inset;
      if (style == LineStyle::Inset || style == LineStyle::Outset) {
        ring(outer, inner, Shade(color, topLeft == inward));
      } else {
        const SkRRect mid = InsetEach(outer, tw / 2, rw / 2, bw / 2, lw / 2);
        ring(outer, mid, Shade(color, topLeft == inward));
        ring(mid, inner, Shade(color, topLeft != inward));
      }
      break;
    }
    default: ring(outer, inner, ToSk(color)); break;
  }
  canvas.restore();
}

}  // namespace

void SkiaPainter::Draw(const DisplayList& list, SkCanvas& canvas) { Draw(list, canvas, list.scrollX, list.scrollY); }

void SkiaPainter::Draw(const DisplayList& list, SkCanvas& canvas, double scrollX, double scrollY) {
  if (impl_->images.size() > 700) impl_->images.clear();
  // The document is moved by how far the viewport is scrolled.
  canvas.save();
  canvas.translate(static_cast<float>(-scrollX), static_cast<float>(-scrollY));
  struct Restore {
    SkCanvas& canvas;
    ~Restore() { canvas.restore(); }
  } restore{canvas};
  for (const Item& item : list.items) {
    if (const auto* fill = std::get_if<FillRoundedRect>(&item)) {
      SkPaint paint;
      paint.setAntiAlias(true);
      paint.setColor(ToSk(fill->color));
      if (fill->rect.Rounded()) canvas.drawRRect(ToSk(fill->rect), paint);
      else canvas.drawRect(SkRect::MakeXYWH(fill->rect.rect.x, fill->rect.rect.y, fill->rect.rect.width, fill->rect.rect.height), paint);
    } else if (const auto* border = std::get_if<DrawBorder>(&item)) {
      const SkRRect outer = ToSk(border->outer), inner = ToSk(border->inner);
      // The same on every side: one ring.
      bool same = true;
      for (int i = 1; i < 4; ++i) {
        same = same && border->widths[i] == border->widths[0] && border->styles[i] == border->styles[0] && border->colors[i].r == border->colors[0].r &&
               border->colors[i].g == border->colors[0].g && border->colors[i].b == border->colors[0].b && border->colors[i].a == border->colors[0].a;
      }
      if (same) {
        if (border->widths[0] > 0 && border->styles[0] == LineStyle::Solid && !border->colors[0].Transparent()) {
          SkPaint paint;
          paint.setAntiAlias(true);
          paint.setColor(ToSk(border->colors[0]));
          canvas.drawDRRect(outer, inner, paint);
        } else {
          for (int side = 0; side < 4; ++side) DrawBorderSide(canvas, *border, side, outer, inner);
        }
      } else {
        for (int side = 0; side < 4; ++side) DrawBorderSide(canvas, *border, side, outer, inner);
      }
    } else if (const auto* glyphs = std::get_if<DrawGlyphs>(&item)) {
      if (glyphs->glyphs.empty() || !glyphs->face) continue;
      sk_sp<SkTypeface> typeface = impl_->TypefaceOf(glyphs->face);
      if (!typeface) continue;
      SkFont font(typeface, glyphs->size);
      font.setEdging(SkFont::Edging::kAntiAlias);
      font.setSubpixel(true);
      font.setHinting(SkFontHinting::kNone);
      SkTextBlobBuilder builder;
      const size_t count = glyphs->glyphs.size();
      const SkTextBlobBuilder::RunBuffer& run = builder.allocRunPos(font, static_cast<int>(count));
      for (size_t i = 0; i < count; ++i) {
        run.glyphs[i] = static_cast<SkGlyphID>(std::min<uint32_t>(glyphs->glyphs[i], 0xFFFF));
        run.pos[2 * i] = glyphs->offsets[2 * i];
        run.pos[2 * i + 1] = glyphs->offsets[2 * i + 1];
      }
      sk_sp<SkTextBlob> blob = builder.make();
      if (!blob) continue;
      // The first shadow is the top one: they are drawn from the last.
      for (size_t i = glyphs->shadows.size(); i-- > 0;) {
        const Shadow& shadow = glyphs->shadows[i];
        if (shadow.color.Transparent()) continue;
        SkPaint under;
        under.setAntiAlias(true);
        under.setColor(ToSk(shadow.color));
        under.setMaskFilter(BlurOf(shadow.blur));
        canvas.drawTextBlob(blob, glyphs->x + shadow.dx, glyphs->y + shadow.dy, under);
      }
      SkPaint paint;
      paint.setAntiAlias(true);
      paint.setColor(ToSk(glyphs->color));
      canvas.drawTextBlob(blob, glyphs->x, glyphs->y, paint);
    } else if (const auto* clip = std::get_if<PushClip>(&item)) {
      canvas.save();
      if (clip->rect.Rounded()) canvas.clipRRect(ToSk(clip->rect), true);
      else canvas.clipRect(SkRect::MakeXYWH(clip->rect.rect.x, clip->rect.rect.y, clip->rect.rect.width, clip->rect.rect.height), true);
    } else if (std::holds_alternative<PopClip>(item) || std::holds_alternative<PopLayer>(item) || std::holds_alternative<PopTransform>(item)) {
      canvas.restore();
    } else if (const auto* box = std::get_if<DrawBoxShadow>(&item)) {
      const SkRRect shape = ToSk(box->shape);
      const Shadow& shadow = box->shadow;
      SkPaint paint;
      paint.setAntiAlias(true);
      paint.setColor(ToSk(shadow.color));
      paint.setMaskFilter(BlurOf(shadow.blur));
      canvas.save();
      if (!shadow.inset) {
        // Outside the box only: what is under the box is its own.
        canvas.clipRRect(shape, SkClipOp::kDifference, true);
        SkRRect cast = Outset(shape, shadow.spread);
        cast.offset(shadow.dx, shadow.dy);
        canvas.drawRRect(cast, paint);
      } else {
        // Inside only: the ring between a far edge and the box moved and made smaller.
        canvas.clipRRect(shape, true);
        SkRRect hole = Outset(shape, -shadow.spread);
        hole.offset(shadow.dx, shadow.dy);
        const float reach = shadow.blur * 2 + std::fabs(shadow.dx) + std::fabs(shadow.dy) + std::fabs(shadow.spread) + 16;
        canvas.drawDRRect(SkRRect::MakeRect(shape.rect().makeOutset(reach, reach)), hole, paint);
      }
      canvas.restore();
    } else if (const auto* outline = std::get_if<DrawOutline>(&item)) {
      const SkRRect shape = ToSk(outline->shape);
      SkPaint paint;
      paint.setAntiAlias(true);
      paint.setColor(ToSk(outline->color));
      canvas.drawDRRect(Outset(shape, outline->offset + outline->width), Outset(shape, outline->offset), paint);
    } else if (const auto* layers = std::get_if<DrawImageLayer>(&item)) {
      impl_->DrawLayer(canvas, layers->layer);
    } else if (const auto* image = std::get_if<DrawImage>(&item)) {
      impl_->DrawElementImage(canvas, *image);
    } else if (std::holds_alternative<PushMask>(item)) {
      canvas.saveLayer(nullptr, nullptr);
    } else if (const auto* mask = std::get_if<PopMask>(&item)) {
      // The pictures of the mask are drawn on a layer that is put on what was drawn so as to keep only what they cover.
      SkPaint keep;
      keep.setBlendMode(SkBlendMode::kDstIn);
      canvas.saveLayer(SkCanvas::SaveLayerRec(nullptr, &keep));
      for (const ImageLayer& one : mask->layers) impl_->DrawLayer(canvas, one);
      canvas.restore();
      canvas.restore();
    } else if (const auto* layer = std::get_if<PushLayer>(&item)) {
      canvas.saveLayerAlphaf(nullptr, layer->opacity);
    } else if (const auto* transform = std::get_if<PushTransform>(&item)) {
      canvas.save();
      if (transform->followsScroll) {
        canvas.translate(static_cast<float>(scrollX), static_cast<float>(scrollY));
        continue;
      }
      const float* m = transform->matrix.m;
      if (transform->sticky.valid) {
        double dx = 0, dy = 0;
        ComputeSticky(transform->sticky, scrollX, scrollY, dx, dy);
        canvas.translate(static_cast<float>(dx), static_cast<float>(dy));
      }
      canvas.concat(SkM44(m[0], m[4], m[8], m[12], m[1], m[5], m[9], m[13], m[2], m[6], m[10], m[14], m[3], m[7], m[11], m[15]));
    }
  }
}

}  // namespace solar::paint
