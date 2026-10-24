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
#include "include/core/SkFontArguments.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkImage.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
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

  void DrawTile(SkCanvas& canvas, Decoded& decoded, const SkRect& destination, bool pixelated) {
    if (destination.width() <= 0 || destination.height() <= 0) return;
    const float scale = std::max(1.0f, canvas.getLocalToDevice().rc(0, 0));
    sk_sp<SkImage> image = At(decoded, static_cast<int>(std::ceil(destination.width() * scale)), static_cast<int>(std::ceil(destination.height() * scale)));
    if (!image) return;
    const SkSamplingOptions sampling = pixelated ? SkSamplingOptions(SkFilterMode::kNearest) : SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear);
    canvas.drawImageRect(image, destination, sampling);
  }

  void DrawLayer(SkCanvas& canvas, const ImageLayer& layer) {
    if (!layer.image || !layer.image->bytes) return;
    Decoded& decoded = DecodedOf(layer.image);
    if (decoded.failed) return;
    const RectF o = layer.origin;
    if (o.width <= 0 || o.height <= 0) return;
    // How large one picture is.
    float w = 0, h = 0;
    const float iw = decoded.width, ih = decoded.height, ratio = decoded.ratio;
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
        DrawTile(canvas, decoded, SkRect::MakeXYWH(startX + i * stepX, startY + j * stepY, w, h), layer.pixelated);
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

SkiaPainter::SkiaPainter() : impl_(std::make_unique<Impl>()) {}
SkiaPainter::~SkiaPainter() = default;

namespace {

// A side of a border: the part of the ring between the outer and the inner edge that is toward that side, cut along the diagonals of the corners.
void DrawBorderSide(SkCanvas& canvas, const DrawBorder& b, int side, const SkRRect& outer, const SkRRect& inner) {
  const Color color = b.colors[side];
  if (b.widths[side] <= 0 || b.styles[side] == LineStyle::None || b.styles[side] == LineStyle::Hidden || color.Transparent()) return;
  const SkRect o = outer.rect(), i = inner.rect();
  SkPathBuilder region;
  switch (side) {
    case 0: region.moveTo(o.left(), o.top()).lineTo(o.right(), o.top()).lineTo(i.right(), i.top()).lineTo(i.left(), i.top()).close(); break;
    case 1: region.moveTo(o.right(), o.top()).lineTo(o.right(), o.bottom()).lineTo(i.right(), i.bottom()).lineTo(i.right(), i.top()).close(); break;
    case 2: region.moveTo(o.right(), o.bottom()).lineTo(o.left(), o.bottom()).lineTo(i.left(), i.bottom()).lineTo(i.right(), i.bottom()).close(); break;
    default: region.moveTo(o.left(), o.bottom()).lineTo(o.left(), o.top()).lineTo(i.left(), i.top()).lineTo(i.left(), i.bottom()).close(); break;
  }
  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setColor(ToSk(color));
  canvas.save();
  canvas.clipPath(region.detach(), true);
  canvas.drawDRRect(outer, inner, paint);
  canvas.restore();
}

}  // namespace

void SkiaPainter::Draw(const DisplayList& list, SkCanvas& canvas) {
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
        if (border->widths[0] > 0 && border->styles[0] != LineStyle::None && border->styles[0] != LineStyle::Hidden && !border->colors[0].Transparent()) {
          SkPaint paint;
          paint.setAntiAlias(true);
          paint.setColor(ToSk(border->colors[0]));
          canvas.drawDRRect(outer, inner, paint);
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
      SkPaint paint;
      paint.setAntiAlias(true);
      paint.setColor(ToSk(glyphs->color));
      if (blob) canvas.drawTextBlob(blob, glyphs->x, glyphs->y, paint);
    } else if (const auto* clip = std::get_if<PushClip>(&item)) {
      canvas.save();
      if (clip->rect.Rounded()) canvas.clipRRect(ToSk(clip->rect), true);
      else canvas.clipRect(SkRect::MakeXYWH(clip->rect.rect.x, clip->rect.rect.y, clip->rect.rect.width, clip->rect.rect.height), true);
    } else if (std::holds_alternative<PopClip>(item) || std::holds_alternative<PopLayer>(item) || std::holds_alternative<PopTransform>(item)) {
      canvas.restore();
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
      const float* m = transform->matrix.m;
      canvas.concat(SkM44(m[0], m[4], m[8], m[12], m[1], m[5], m[9], m[13], m[2], m[6], m[10], m[14], m[3], m[7], m[11], m[15]));
    }
  }
}

}  // namespace solar::paint
