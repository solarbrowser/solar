#pragma once

#include <cstdint>
#include <memory>
#include <variant>
#include <vector>

#include "solar/font/Face.h"

// What a page comes to when it is painted, as a list of drawing commands in the coordinates of the document: made from the boxes of layout
// (Paint.cpp), run by a canvas (Skia, or anything that can draw rectangles and glyphs). Nothing in it refers to the DOM or the boxes, so a canvas
// can run it on another thread than the one the page is on.
namespace solar::paint {

struct Color {
  uint8_t r = 0, g = 0, b = 0, a = 0;  // sRGB, not premultiplied
  bool Transparent() const { return a == 0; }
};

struct RectF {
  float x = 0, y = 0, width = 0, height = 0;
};

// A rectangle with rounded corners: the radii of top left, top right, bottom right and bottom left, each as x then y.
struct RoundedRect {
  RectF rect;
  float radii[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  bool Rounded() const {
    for (float r : radii) {
      if (r > 0) return true;
    }
    return false;
  }
};

struct Matrix4 {
  float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  // column-major, as CSS has it
};

enum class LineStyle : uint8_t { None, Hidden, Dotted, Dashed, Solid, Double, Groove, Ridge, Inset, Outset };

// The inside of a rounded rectangle, one color.
struct FillRoundedRect {
  RoundedRect rect;
  Color color;
};

// A border: the ring between `outer` and `inner`, each side (top, right, bottom, left) with its own width, style and color.
struct DrawBorder {
  RoundedRect outer, inner;
  float widths[4] = {0, 0, 0, 0};
  LineStyle styles[4] = {LineStyle::None, LineStyle::None, LineStyle::None, LineStyle::None};
  Color colors[4];
};

// Glyphs of one font along a line: `x`, `y` is where the first one starts (on the baseline), `offsets` are the places of the glyphs from it.
struct DrawGlyphs {
  std::shared_ptr<font::Face> face;
  float size = 16;
  Color color;
  float x = 0, y = 0;
  std::vector<uint32_t> glyphs;
  std::vector<float> offsets;  // along the line, then up (pairs: x, y), one for each glyph
};

// What drawn after is cut to a rectangle, until the matching PopClip.
struct PushClip {
  RoundedRect rect;
};
struct PopClip {};

// What is drawn after is drawn on its own and put in the page with this opacity, until the matching PopLayer.
struct PushLayer {
  float opacity = 1;
};
struct PopLayer {};

// What is drawn after is moved by the matrix, until the matching PopTransform.
struct PushTransform {
  Matrix4 matrix;
};
struct PopTransform {};

using Item = std::variant<FillRoundedRect, DrawBorder, DrawGlyphs, PushClip, PopClip, PushLayer, PopLayer, PushTransform, PopTransform>;

struct DisplayList {
  std::vector<Item> items;
  float width = 0, height = 0;  // the extent of the document (the scrollable overflow of the viewport)
};

}  // namespace solar::paint
