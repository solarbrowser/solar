#pragma once

#include <cstdint>
#include <memory>
#include <string>
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

// An image the page has, as the file says it (the painter decodes it, once): a picture, or an SVG.
struct ImageData {
  std::shared_ptr<const std::string> bytes;
  std::string url;
};
using ImageRef = std::shared_ptr<const ImageData>;

// A length: so many px and so many percent of some room (what a calc() of the two comes to), or automatic (the size of an image that is not given).
struct Length {
  float px = 0;
  float percent = 0;
  bool automatic = false;
  float Of(float room) const { return px + percent * room / 100; }
};

enum class Repeat : uint8_t { NoRepeat, Repeat, Space, Round };

// An image put in an area the way a background or a mask does (https://www.w3.org/TR/css-backgrounds-3/#backgrounds): it is as large as `size` says
// in the positioning area `origin`, put in it by `x` and `y`, repeated as asked, and shown where `clip` is.
struct ImageLayer {
  ImageRef image;
  RoundedRect clip;
  RectF origin;
  enum class Size : uint8_t { Auto, Cover, Contain, Lengths } size = Size::Auto;
  Length width{0, 0, true}, height{0, 0, true};
  Length x{0, 0, false}, y{0, 0, false};
  Repeat repeatX = Repeat::Repeat, repeatY = Repeat::Repeat;
  bool pixelated = false;  // image-rendering: pixelated or crisp-edges
};

struct DrawImageLayer {
  ImageLayer layer;
};

// An image of an element (img): in its content box as object-fit and object-position say.
enum class ObjectFit : uint8_t { Fill, Contain, Cover, None, ScaleDown };
struct DrawImage {
  ImageRef image;
  RoundedRect clip;  // the content box, whose corners are the element's rounded
  ObjectFit fit = ObjectFit::Fill;
  Length x{0, 50, false}, y{0, 50, false};
  bool pixelated = false;
};

// What is drawn between PushMask and PopMask is drawn on its own, then cut to the image of the mask (its alpha), and put in the page.
struct PushMask {};
struct PopMask {
  std::vector<ImageLayer> layers;  // added together: what any of them shows is shown
};

using Item = std::variant<FillRoundedRect, DrawBorder, DrawGlyphs, PushClip, PopClip, PushLayer, PopLayer, PushTransform, PopTransform, DrawImageLayer, DrawImage, PushMask, PopMask>;

struct DisplayList {
  std::vector<Item> items;
  float width = 0, height = 0;  // the extent of the document (the scrollable overflow of the viewport)
};

}  // namespace solar::paint
