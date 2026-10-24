// A display list drawn with Skia.
#include "solar/paint/SkiaPainter.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontArguments.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkM44.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkRRect.h"
#include "include/core/SkTextBlob.h"
#include "include/core/SkTypeface.h"
#include "include/ports/SkFontMgr_empty.h"

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
