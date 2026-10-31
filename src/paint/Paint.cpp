/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// Painting (https://www.w3.org/TR/CSS22/zindex.html, Appendix E): the boxes of layout in the order they are painted, as a display list.
//
// A stacking context is painted as the standard says: its own background and border, the contexts below it (negative z-index), the backgrounds of the
// block-level descendants, the floats, the inline content (text, and the backgrounds of inline boxes), the positioned descendants and contexts with
// z-index auto or 0, and the contexts above it. What a box does to what is painted after (a clip of its overflow, how far it is scrolled, its opacity
// and transform) is the display list's PushClip / PushLayer / PushTransform, kept in step with the boxes that are being painted.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "quanta/Embed.h"
#include "solar/css/Style.h"
#include "solar/dom/Range.h"
#include "solar/layout/Layout.h"
#include "solar/paint/Paint.h"
#include "solar/html/Frames.h"
#include "solar/html/Xml.h"

namespace solar::paint {

namespace {

std::atomic<bool> g_caretOn{true};  // the caret blinks: it is drawn in the frames that are made while this is set
Popup g_popup;  // the drop-down list that is open, if any (set and read on the thread that makes the frames)

using layout::Box;
using layout::Line;
using layout::LineItem;
using layout::Rect;
using layout::Tree;

float F(double v) { return static_cast<float>(v); }

// The numbers in a computed value of border-*-radius: one or two lengths or percentages.
bool ParseRadius(const std::string& text, double width, double height, double& x, double& y) {
  std::vector<std::pair<double, bool>> parts;
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && text[i] == ' ') ++i;
    if (i >= text.size()) break;
    char* end = nullptr;
    const double v = std::strtod(text.c_str() + i, &end);
    if (end == text.c_str() + i) return false;
    i = static_cast<size_t>(end - text.c_str());
    bool percent = false;
    if (i < text.size() && text[i] == '%') {
      percent = true;
      ++i;
    } else {
      while (i < text.size() && std::isalpha(static_cast<unsigned char>(text[i]))) ++i;  // px (computed lengths are in px)
    }
    parts.push_back({v, percent});
  }
  if (parts.empty()) return false;
  const auto resolve = [&](const std::pair<double, bool>& p, double basis) { return std::max(0.0, p.second ? basis * p.first / 100 : p.first); };
  x = resolve(parts[0], width);
  y = parts.size() > 1 ? resolve(parts[1], height) : (parts[0].second ? resolve(parts[0], height) : x);
  return true;
}


// ---- The values of the properties of images ----

// The items of a comma separated list, at the top level (not inside parentheses or quotes).
std::vector<std::string> SplitList(const std::string& text, char separator) {
  std::vector<std::string> out;
  std::string current;
  int depth = 0;
  char quote = 0;
  for (char c : text) {
    if (quote) {
      current += c;
      if (c == quote) quote = 0;
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
      current += c;
    } else if (c == '(') {
      ++depth;
      current += c;
    } else if (c == ')') {
      --depth;
      current += c;
    } else if (c == separator && depth == 0) {
      out.push_back(current);
      current.clear();
    } else {
      current += c;
    }
  }
  out.push_back(current);
  return out;
}

std::string Trim(const std::string& text) {
  size_t a = 0, b = text.size();
  while (a < b && std::isspace(static_cast<unsigned char>(text[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(text[b - 1]))) --b;
  return text.substr(a, b - a);
}

// The words of a value: split at white space at the top level.
std::vector<std::string> SplitWords(const std::string& text) {
  std::vector<std::string> out;
  std::string current;
  int depth = 0;
  char quote = 0;
  for (char c : text) {
    if (quote) {
      current += c;
      if (c == quote) quote = 0;
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
      current += c;
    } else if (c == '(') {
      ++depth;
      current += c;
    } else if (c == ')') {
      --depth;
      current += c;
    } else if (std::isspace(static_cast<unsigned char>(c)) && depth == 0) {
      if (!current.empty()) out.push_back(current);
      current.clear();
    } else {
      current += c;
    }
  }
  if (!current.empty()) out.push_back(current);
  return out;
}

// A length or a percentage, or the sum of them in calc() (what a computed position or size comes to): `auto` is the automatic one.
bool ParseLength(const std::string& given, Length& out) {
  const std::string text = Trim(given);
  out = Length{};
  if (text == "auto") {
    out.automatic = true;
    return true;
  }
  const auto add = [&](const std::string& t, double sign) {
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (end == t.c_str()) return false;
    const std::string unit = end;
    if (unit == "%") out.percent += static_cast<float>(sign * v);
    else if (unit.empty() || unit == "px") out.px += static_cast<float>(sign * v);
    else return false;
    return true;
  };
  if (text.rfind("calc(", 0) == 0 && text.back() == ')') {
    double sign = 1;
    bool any = false;
    for (const std::string& w : SplitWords(Trim(text.substr(5, text.size() - 6)))) {
      if (w == "+") { sign = 1; continue; }
      if (w == "-") { sign = -1; continue; }
      if (!add(w, sign)) return false;
      any = true;
      sign = 1;
    }
    return any;
  }
  return add(text, 1);
}


// ---- Gradients ----

// An angle in degrees: 90deg, 0.25turn, 1.5rad, 100grad.
bool ParseAngle(const std::string& text, float& degrees) {
  char* end = nullptr;
  const double v = std::strtod(text.c_str(), &end);
  if (end == text.c_str()) return false;
  const std::string unit = end;
  if (unit == "deg" || unit.empty()) degrees = static_cast<float>(v);
  else if (unit == "grad") degrees = static_cast<float>(v * 0.9);
  else if (unit == "rad") degrees = static_cast<float>(v * 180 / M_PI);
  else if (unit == "turn") degrees = static_cast<float>(v * 360);
  else return false;
  return true;
}

bool IsColorWord(const std::string& w) {
  if (w.empty()) return false;
  const char c = w[0];
  return w.find('(') != std::string::npos || c == '#' || std::isalpha(static_cast<unsigned char>(c));
}

// The gradient a computed value of an image is (linear-, radial- and conic-gradient, and their repeating forms); null if it is not one.
GradientRef ParseGradient(const std::string& given) {
  const std::string text = Trim(given);
  const size_t open = text.find('(');
  if (open == std::string::npos || text.back() != ')') return nullptr;
  std::string name = text.substr(0, open);
  auto g = std::make_shared<Gradient>();
  if (name.rfind("repeating-", 0) == 0) {
    g->repeating = true;
    name = name.substr(10);
  }
  if (name == "-webkit-linear-gradient") name = "linear-gradient";
  if (name == "linear-gradient") g->kind = Gradient::Kind::Linear;
  else if (name == "radial-gradient") g->kind = Gradient::Kind::Radial;
  else if (name == "conic-gradient") g->kind = Gradient::Kind::Conic;
  else return nullptr;
  std::vector<std::string> args = SplitList(text.substr(open + 1, text.size() - open - 2), ',');
  size_t first = 0;
  // The first argument, when it is not a color stop, says where it goes.
  if (!args.empty()) {
    const std::vector<std::string> words = SplitWords(args[0]);
    bool config = false;
    if (g->kind == Gradient::Kind::Linear) {
      float angle = 0;
      if (!words.empty() && words[0] == "to") {
        config = true;
        g->toCorner = true;
        for (size_t i = 1; i < words.size(); ++i) {
          if (words[i] == "right") g->cornerX = 1;
          else if (words[i] == "left") g->cornerX = -1;
          else if (words[i] == "bottom") g->cornerY = 1;
          else if (words[i] == "top") g->cornerY = -1;
        }
        if (g->cornerX == 0 || g->cornerY == 0) {
          // A side: the angle of it.
          g->toCorner = false;
          g->angle = g->cornerY == -1 ? 0 : g->cornerY == 1 ? 180 : g->cornerX == 1 ? 90 : 270;
        }
      } else if (words.size() == 1 && ParseAngle(words[0], angle)) {
        config = true;
        g->angle = angle;
      }
    } else if (g->kind == Gradient::Kind::Radial) {
      for (const std::string& w : words) {
        if (w == "circle" || w == "ellipse" || w == "closest-side" || w == "farthest-side" || w == "closest-corner" || w == "farthest-corner" || w == "at") config = true;
      }
      if (!config && !words.empty() && (std::isdigit(static_cast<unsigned char>(words[0][0])) || words[0][0] == '.')) config = true;
      if (config) {
        size_t i = 0;
        std::vector<Length> lengths;
        for (; i < words.size() && words[i] != "at"; ++i) {
          const std::string& w = words[i];
          if (w == "circle") g->circle = true;
          else if (w == "ellipse") g->circle = false;
          else if (w == "closest-side") g->extent = Gradient::Extent::ClosestSide;
          else if (w == "farthest-side") g->extent = Gradient::Extent::FarthestSide;
          else if (w == "closest-corner") g->extent = Gradient::Extent::ClosestCorner;
          else if (w == "farthest-corner") g->extent = Gradient::Extent::FarthestCorner;
          else {
            Length l;
            if (ParseLength(w, l)) lengths.push_back(l);
          }
        }
        if (!lengths.empty()) {
          g->extent = Gradient::Extent::Lengths;
          g->radiusX = lengths[0];
          g->radiusY = lengths.size() > 1 ? lengths[1] : lengths[0];
          if (lengths.size() == 1) g->circle = true;
        }
        if (i < words.size() && words[i] == "at") {
          std::vector<std::string> position(words.begin() + static_cast<std::ptrdiff_t>(i) + 1, words.end());
          if (position.size() == 1) position.push_back("50%");
          const auto keyword = [](const std::string& w) { return w == "left" || w == "top" ? "0%" : w == "right" || w == "bottom" ? "100%" : w == "center" ? "50%" : w.c_str(); };
          if (position.size() >= 2) {
            ParseLength(keyword(position[0]), g->centerX);
            ParseLength(keyword(position[1]), g->centerY);
          }
        }
      }
    } else {
      // conic: from <angle> at <position>
      for (size_t i = 0; i < words.size(); ++i) {
        if (words[i] == "from" || words[i] == "at") config = true;
      }
      if (config) {
        for (size_t i = 0; i < words.size(); ++i) {
          if (words[i] == "from" && i + 1 < words.size()) ParseAngle(words[i + 1], g->angle);
          if (words[i] == "at" && i + 1 < words.size()) {
            std::vector<std::string> position(words.begin() + static_cast<std::ptrdiff_t>(i) + 1, words.end());
            if (position.size() == 1) position.push_back("50%");
            const auto keyword = [](const std::string& w) { return w == "left" || w == "top" ? "0%" : w == "right" || w == "bottom" ? "100%" : w == "center" ? "50%" : w.c_str(); };
            ParseLength(keyword(position[0]), g->centerX);
            ParseLength(keyword(position[1]), g->centerY);
            break;
          }
        }
        if (g->kind == Gradient::Kind::Conic && words[0] != "from" && words[0] != "at") config = false;
      }
      if (g->kind == Gradient::Kind::Conic && !config) g->angle = 0;
    }
    if (config) first = 1;
    else if (g->kind == Gradient::Kind::Conic) g->angle = 0;
  }
  // The color stops: a color and one or two positions. (A position alone is an interpolation hint, which is not kept.)
  for (size_t i = first; i < args.size(); ++i) {
    const std::vector<std::string> words = SplitWords(args[i]);
    if (words.empty()) continue;
    size_t colorAt = IsColorWord(words[0]) ? 0 : words.size() - 1;
    if (!IsColorWord(words[colorAt])) continue;
    ColorStop stop;
    stop.color = ParseColor(words[colorAt]);
    std::vector<std::string> positions;
    for (size_t k = 0; k < words.size(); ++k) {
      if (k != colorAt) positions.push_back(words[k]);
    }
    for (const std::string& p : positions) {
      ColorStop copy = stop;
      float angle = 0;
      Length l;
      if (g->kind == Gradient::Kind::Conic && ParseAngle(p, angle) && p.find('%') == std::string::npos) {
        copy.position = Length{0, angle / 3.6f, false};
        copy.positioned = true;
      } else if (ParseLength(p, l)) {
        copy.position = l;
        copy.positioned = true;
      }
      g->stops.push_back(copy);
    }
    if (positions.empty()) g->stops.push_back(stop);
  }
  if (g->stops.size() < 2) return nullptr;
  return g;
}

// The address in a url("...") (or url(...)); empty if it is not one.
std::string UrlOf(const std::string& item) {
  const std::string t = Trim(item);
  if (t.rfind("url(", 0) != 0 || t.back() != ')') return "";
  std::string inner = Trim(t.substr(4, t.size() - 5));
  if (inner.size() >= 2 && (inner.front() == '"' || inner.front() == '\'') && inner.back() == inner.front()) {
    std::string out;
    for (size_t i = 1; i + 1 < inner.size(); ++i) {
      if (inner[i] == '\\' && i + 2 < inner.size()) ++i;
      out += inner[i];
    }
    return out;
  }
  return inner;
}

Repeat RepeatOf(const std::string& word) {
  if (word == "no-repeat") return Repeat::NoRepeat;
  if (word == "space") return Repeat::Space;
  if (word == "round") return Repeat::Round;
  return Repeat::Repeat;
}

// The shadows of a computed box-shadow or text-shadow: each is a color, two to four lengths and perhaps `inset`, the first drawn on top.
std::vector<Shadow> ParseShadows(const std::string& text) {
  std::vector<Shadow> out;
  const std::string t = Trim(text);
  if (t.empty() || t == "none") return out;
  for (const std::string& item : SplitList(t, ',')) {
    Shadow shadow;
    std::vector<float> numbers;
    bool colored = false;
    for (const std::string& word : SplitWords(item)) {
      if (word == "inset") {
        shadow.inset = true;
        continue;
      }
      const char c = word.empty() ? 0 : word[0];
      if (std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+' || c == '.') {
        numbers.push_back(static_cast<float>(std::atof(word.c_str())));
      } else {
        shadow.color = ParseColor(word);
        colored = true;
      }
    }
    if (numbers.size() < 2) continue;
    (void)colored;
    shadow.dx = numbers[0];
    shadow.dy = numbers[1];
    if (numbers.size() > 2) shadow.blur = std::max(0.0f, numbers[2]);
    if (numbers.size() > 3) shadow.spread = numbers[3];
    out.push_back(shadow);
  }
  return out;
}

LineStyle LineStyleOf(layout::BorderStyle s) {
  switch (s) {
    case layout::BorderStyle::None: return LineStyle::None;
    case layout::BorderStyle::Hidden: return LineStyle::Hidden;
    case layout::BorderStyle::Dotted: return LineStyle::Dotted;
    case layout::BorderStyle::Dashed: return LineStyle::Dashed;
    case layout::BorderStyle::Double: return LineStyle::Double;
    case layout::BorderStyle::Groove: return LineStyle::Groove;
    case layout::BorderStyle::Ridge: return LineStyle::Ridge;
    case layout::BorderStyle::Inset: return LineStyle::Inset;
    case layout::BorderStyle::Outset: return LineStyle::Outset;
    default: return LineStyle::Solid;
  }
}

// What a box needs of its style to be painted, that layout does not keep.
struct PaintProps {
  Color borderColors[4];
  double radii[8] = {0, 0, 0, 0, 0, 0, 0, 0};  // top left, top right, bottom right, bottom left: x then y
  bool hasRadius = false;
  std::vector<Shadow> boxShadows;
  LineStyle outlineStyle = LineStyle::None;
  double outlineWidth = 0, outlineOffset = 0;
  Color outlineColor;
};

using PropsMap = std::unordered_map<const Box*, PaintProps>;

struct Decoration {
  bool underline = false, overline = false, lineThrough = false;
  Color color;
};

// The same picture is the same ImageData from one display list to the next: the painter decodes it once.
ImageRef KeptImage(const std::string& key, const std::shared_ptr<const std::string>& bytes) {
  static std::unordered_map<std::string, ImageRef> kept;
  const auto found = kept.find(key);
  if (found != kept.end() && (found->second->bytes == bytes || *found->second->bytes == *bytes)) return found->second;
  if (kept.size() > 600) kept.clear();
  auto data = std::make_shared<ImageData>();
  data->bytes = bytes;
  data->url = key;
  kept[key] = data;
  return data;
}

class Painter {
 public:
  Painter(Quanta::Context& ctx, Tree& tree, double overscan) : ctx_(ctx), tree_(tree), overscan_(overscan) {
    if (!tree.paintCache || tree.paintCacheFor != tree.builtFor) {
      tree.paintCache = std::make_shared<PropsMap>();
      tree.paintCacheFor = tree.builtFor;
    }
    props_ = static_cast<PropsMap*>(tree.paintCache.get());
  }

  DisplayList Run() {
    DisplayList out;
    list_ = &out;
    if (!tree_.root || tree_.root->children.empty()) return out;
    Box* html = tree_.root->children.front().get();
    const dom::Document* document = html->node ? html->node->nodeDocument : nullptr;
    scrollX_ = scrollY_ = 0;
    if (document) {
      const auto found = tree_.scroll.find(document);
      if (found != tree_.scroll.end()) {
        scrollX_ = found->second.first;
        scrollY_ = found->second.second;
      }
    }
    // The canvas: white, then the background of the root element, or of the body when the root has none.
    const layout::Rect extent = layout::ScrollableOverflow(tree_, *tree_.root, html->element());
    out.width = F(std::max(tree_.viewportWidth, extent.Right()));
    out.height = F(std::max(tree_.viewportHeight, extent.Bottom()));
    Fill(RectF{0, 0, out.width, out.height}, Color{255, 255, 255, 255});
    Color canvas = BackgroundOf(*html);
    const Box* body = nullptr;
    for (const auto& child : html->children) {
      if (child->element() && child->element()->IsHtml("body")) body = child.get();
    }
    if (canvas.Transparent() && body) {
      canvas = BackgroundOf(*body);
      canvasFrom_ = body;
    } else {
      canvasFrom_ = html;
    }
    if (!canvas.Transparent()) Fill(RectF{0, 0, out.width, out.height}, canvas);
    // Everything is in the coordinates of the document: whoever draws moves it by how far the viewport is scrolled (as `out` says).
    out.scrollX = F(scrollX_);
    out.scrollY = F(scrollY_);
    out.roomY = F(overscan_);
    ++transformDepth_;
    PaintContext(*html);
    EnsureClips({});
    --transformDepth_;
    if (g_popup.select) PaintPopup();
    return out;
  }

 private:
  struct Frame {
    size_t base = 0;  // the clips below this are applied outside the frame
  };

  // ---- Items ----

  void Fill(const RectF& rect, const Color& color) {
    if (color.Transparent() || rect.width <= 0 || rect.height <= 0) return;
    FillRoundedRect item;
    item.rect.rect = rect;
    item.color = color;
    list_->items.push_back(item);
  }

  // Whether something that takes the document rectangle is out of the viewport (with room for shadows, outlines and the like) and so need not be made: only
  // where nothing but the scroll of the viewport moves the coordinates.
  bool Culled(double x, double y, double width, double height) const {
    if (transformDepth_ != 1 || std::getenv("SOLAR_PAINT_NO_CULL")) return false;
    const double room = 300;
    const double roomY = room + overscan_;
    return x > scrollX_ + tree_.viewportWidth + room || x + width < scrollX_ - room || y > scrollY_ + tree_.viewportHeight + roomY || y + height < scrollY_ - roomY;
  }

  void PushTransformOf(double dx, double dy) {
    ++transformDepth_;
    PushTransform item;
    item.matrix.m[12] = F(dx);
    item.matrix.m[13] = F(dy);
    list_->items.push_back(item);
  }

  // ---- What boxes look like ----

  dom::Element* ElementOf(const Box& box, std::string& pseudo) const {
    pseudo.clear();
    if (box.element()) return box.element();
    if (!box.pseudo.empty() && box.parent) {
      pseudo = box.pseudo;
      return box.parent->element();
    }
    return nullptr;
  }

  const PaintProps& PropsOf(const Box& box, const RectF& borderBox) {
    auto found = props_->find(&box);
    if (found != props_->end()) return found->second;
    PaintProps props;
    std::string pseudo;
    dom::Element* element = ElementOf(box, pseudo);
    if (element) {
      css::StyleReader reader(ctx_, element, pseudo);
      static const char* const colors[4] = {"border-top-color", "border-right-color", "border-bottom-color", "border-left-color"};
      for (int i = 0; i < 4; ++i) props.borderColors[i] = ParseColor(reader.Get(colors[i]));
      static const char* const radii[4] = {"border-top-left-radius", "border-top-right-radius", "border-bottom-right-radius", "border-bottom-left-radius"};
      for (int i = 0; i < 4; ++i) {
        double rx = 0, ry = 0;
        if (ParseRadius(reader.Get(radii[i]), borderBox.width, borderBox.height, rx, ry)) {
          props.radii[2 * i] = rx;
          props.radii[2 * i + 1] = ry;
          if (rx > 0 || ry > 0) props.hasRadius = true;
        }
      }
      props.boxShadows = ParseShadows(reader.Get("box-shadow"));
      const std::string outline = reader.Get("outline-style");
      if (!outline.empty() && outline != "none" && outline != "hidden") {
        props.outlineStyle = outline == "dotted" ? LineStyle::Dotted : outline == "dashed" ? LineStyle::Dashed : outline == "double" ? LineStyle::Double : LineStyle::Solid;
        props.outlineWidth = std::atof(reader.Get("outline-width").c_str());
        props.outlineOffset = std::atof(reader.Get("outline-offset").c_str());
        props.outlineColor = ParseColor(reader.Get("outline-color"));
      }
      if (props.hasRadius) {
        // Radii that would overlap are scaled down together (https://www.w3.org/TR/css-backgrounds-3/#corner-overlap).
        double f = 1;
        const auto limit = [&](double side, double a, double b) {
          if (a + b > side && a + b > 0) f = std::min(f, side / (a + b));
        };
        limit(borderBox.width, props.radii[0], props.radii[2]);
        limit(borderBox.width, props.radii[6], props.radii[4]);
        limit(borderBox.height, props.radii[1], props.radii[7]);
        limit(borderBox.height, props.radii[3], props.radii[5]);
        if (f < 1) {
          for (double& r : props.radii) r *= f;
        }
      }
    }
    return props_->emplace(&box, props).first->second;
  }

  Color BackgroundOf(const Box& box) {
    if (!box.style) return Color{};
    return ParseColor(box.style->backgroundColor);
  }

  static RectF ToRectF(const Rect& r) { return RectF{F(r.x), F(r.y), F(r.width), F(r.height)}; }

  // The rounded rectangle of a box's border box, its padding box, or its content box.
  RoundedRect Rounded(const Box& box, const RectF& rect, const PaintProps& props, int edge /*0 border, 1 padding*/, const layout::Edges* insets) const {
    RoundedRect out;
    out.rect = rect;
    if (!props.hasRadius) return out;
    for (int i = 0; i < 8; ++i) out.radii[i] = F(props.radii[i]);
    if (edge == 1 && insets) {
      // The padding edge: the radii are smaller by the widths of the borders at each corner.
      const double top = insets->top, right = insets->right, bottom = insets->bottom, left = insets->left;
      const auto shrink = [](double r, double w) { return F(std::max(0.0, r - w)); };
      out.radii[0] = shrink(props.radii[0], left);
      out.radii[1] = shrink(props.radii[1], top);
      out.radii[2] = shrink(props.radii[2], right);
      out.radii[3] = shrink(props.radii[3], top);
      out.radii[4] = shrink(props.radii[4], right);
      out.radii[5] = shrink(props.radii[5], bottom);
      out.radii[6] = shrink(props.radii[6], left);
      out.radii[7] = shrink(props.radii[7], bottom);
    }
    (void)box;
    return out;
  }

  // The background and border of a rectangle of a box (the whole border box of a block, one fragment of an inline box).
  void PaintDecoration(const Box& box, const Rect& absolute, bool leftEdge, bool rightEdge) {
    if (!box.style || box.style->visibility != layout::Visibility::Visible) return;
    const RectF borderBox = ToRectF(absolute);
    if (borderBox.width <= 0 && borderBox.height <= 0) return;
    if (Culled(borderBox.x, borderBox.y, borderBox.width, borderBox.height)) return;
    const PaintProps& props = PropsOf(box, borderBox);
    const layout::BoxStyle& physical = box.style->Physical();
    // Outer shadows are under the background; the first one is the top one.
    for (size_t i = props.boxShadows.size(); i-- > 0;) {
      if (props.boxShadows[i].inset || props.boxShadows[i].color.Transparent()) continue;
      DrawBoxShadow item;
      item.shape = Rounded(box, borderBox, props, 0, nullptr);
      item.shadow = props.boxShadows[i];
      list_->items.push_back(std::move(item));
    }
    // A control no style touched has the look of the platform's: its own frame is drawn instead of the border of its style.
    const bool native = leftEdge && rightEdge && NativeFrame(box, props, borderBox);
    if (&box != canvasFrom_ && !native) {  // (the background of the root element, or of the body, is the canvas's)
      const Color color = BackgroundOf(box);
      if (!color.Transparent()) {
        FillRoundedRect item;
        item.rect = Rounded(box, borderBox, props, 0, nullptr);
        item.color = color;
        list_->items.push_back(item);
      }
    }
    for (ImageLayer& layer : LayersOf(box, borderBox, props, false)) {
      DrawImageLayer item;
      item.layer = std::move(layer);
      list_->items.push_back(std::move(item));
    }
    // Inner shadows are over the background, under the border.
    for (size_t i = props.boxShadows.size(); i-- > 0;) {
      if (!props.boxShadows[i].inset || props.boxShadows[i].color.Transparent()) continue;
      layout::Edges noEdges;
      DrawBoxShadow item;
      item.shape = Rounded(box, borderBox, props, 0, &noEdges);
      item.shadow = props.boxShadows[i];
      list_->items.push_back(std::move(item));
    }
    if (props.outlineStyle != LineStyle::None && props.outlineWidth > 0 && !props.outlineColor.Transparent() && leftEdge && rightEdge) {
      DrawOutline outline;
      outline.shape = Rounded(box, borderBox, props, 0, nullptr);
      outline.offset = F(props.outlineOffset);
      outline.width = F(props.outlineWidth);
      outline.style = props.outlineStyle;
      outline.color = props.outlineColor;
      outlines_.push_back(outline);
    }
    layout::Edges widths = box.border;
    if (!leftEdge) widths.left = 0;
    if (!rightEdge) widths.right = 0;
    if (!native && (widths.top > 0 || widths.right > 0 || widths.bottom > 0 || widths.left > 0)) PaintBorder(box, props, physical, borderBox, widths);
    if (leftEdge && rightEdge) PaintFocusRing(box, props, borderBox);
    if (leftEdge && rightEdge) {
      PaintImageOf(box, borderBox, props);
      PaintVideoOf(box, borderBox, props);
    }
    if (leftEdge && rightEdge && box.formControl) PaintControl(box, borderBox);
  }


  // ---- Form controls ----
  // What a control shows is not in the nodes of the document but in the state the page's script keeps (what was typed, what is checked), so it is
  // asked for. The control's own box (border, background) is already drawn; this is what is in it.

  static Color HexColor(const std::string& text) {
    if (text.size() != 7 || text[0] != '#') return Rgb(0, 0, 0);
    const auto byte = [&](size_t at) { return static_cast<int>(std::strtol(text.substr(at, 2).c_str(), nullptr, 16)); };
    return Rgb(byte(1), byte(3), byte(5));
  }

  static Color Rgb(int r, int g, int b, int a = 255) { return Color{static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b), static_cast<uint8_t>(a)}; }

  static RoundedRect Round(const RectF& rect, float radius) {
    RoundedRect out;
    out.rect = rect;
    for (float& r : out.radii) r = radius;
    return out;
  }

  void FillRound(const RectF& rect, float radius, const Color& color) {
    FillRoundedRect item;
    item.rect = Round(rect, radius);
    item.color = color;
    list_->items.push_back(item);
  }

  // A rectangle turned by `degrees` about its own center.
  void FillTurned(const RectF& rect, float degrees, const Color& color) {
    const float a = degrees * 3.14159265f / 180, c = std::cos(a), s = std::sin(a);
    const float cx = rect.x + rect.width / 2, cy = rect.y + rect.height / 2;
    PushTransform push;
    push.matrix.m[0] = c; push.matrix.m[1] = s; push.matrix.m[4] = -s; push.matrix.m[5] = c;
    push.matrix.m[12] = cx - c * cx + s * cy;
    push.matrix.m[13] = cy - s * cx - c * cy;
    list_->items.push_back(push);
    Fill(rect, color);
    list_->items.push_back(PopTransform{});
  }

  // The bytes of a UTF-8 string that come before `units` UTF-16 units of it.
  static size_t BytesBefore(const std::string& text, size_t units) {
    size_t i = 0;
    while (i < text.size() && units > 0) {
      const unsigned char c = static_cast<unsigned char>(text[i]);
      const size_t length = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
      const size_t u = length == 4 ? 2 : 1;
      if (u > units) break;
      units -= u;
      i += length;
    }
    return i;
  }

  // A line of text at `x` with its baseline at `baseline`; returns its width.
  double DrawLine(const std::shared_ptr<font::Face>& face, double size, const std::string& text, double x, double baseline, const Color& color) {
    if (!face || text.empty()) return 0;
    const std::vector<font::Glyph> glyphs = face->Shape(text);
    DrawGlyphs run;
    run.face = face;
    run.size = F(size);
    run.color = color;
    run.x = F(x);
    run.y = F(baseline);
    double pen = 0;
    for (const font::Glyph& g : glyphs) {
      run.glyphs.push_back(g.id);
      run.offsets.push_back(F(pen + g.xOffset * size));
      run.offsets.push_back(F(-g.yOffset * size));
      pen += g.advance * size;
    }
    list_->items.push_back(std::move(run));
    return pen;
  }

  void PaintPopup() {
    PopupGeometry g;
    if (!PopupGeometryOf(ctx_, tree_, g_popup.select, g_popup.top, g)) return;
    html::ControlView view;
    if (!html::ReadControl(g_popup.select, view)) return;
    const auto boxes = tree_.boxesOf.find(g_popup.select);
    if (boxes == tree_.boxesOf.end() || boxes->second.empty()) return;
    const Box& box = *boxes->second.front();
    const std::shared_ptr<font::Face> face = layout::FaceOf(ctx_, tree_, box);
    if (!face) return;
    const double size = box.style->fontSize;
    const double ascent = std::round(face->metrics().ascent * size);
    const RectF frame{F(g.x), F(g.y), F(g.width), F(g.height)};
    FillRound(RectF{frame.x - 1, frame.y - 1, frame.width + 2, frame.height + 2}, 2, Rgb(118, 118, 118));
    FillRound(frame, 1, Rgb(255, 255, 255));
    for (size_t row = 0; row < g.rows; ++row) {
      const size_t i = g_popup.top + row;
      if (i >= view.options.size()) break;
      const html::ControlView::Option& option = view.options[i];
      const double y = g.y + static_cast<double>(row) * g.rowHeight;
      Color ink = option.disabled || option.group ? Rgb(110, 110, 110) : Rgb(0, 0, 0);
      if (i == g_popup.hover && !option.disabled && !option.group) {
        Fill(RectF{frame.x, F(y), frame.width, F(g.rowHeight)}, Rgb(0, 117, 255));
        ink = Rgb(255, 255, 255);
      } else if (option.selected) {
        Fill(RectF{frame.x, F(y), frame.width, F(g.rowHeight)}, Rgb(224, 224, 224));
      }
      DrawLine(face, size, option.label, g.x + (option.indented ? 20 : 4), y + 1 + ascent, ink);
    }
  }

  // Whether a control looks as the platform draws it (nothing of its border or background was written by a style), and if so draws its frame.
  bool NativeFrame(const Box& box, const PaintProps& props, const RectF& borderBox) {
    dom::Element* element = box.element();
    if (!element || !box.style) return false;
    if (!element->IsHtml("input") && !element->IsHtml("select") && !element->IsHtml("textarea") && !element->IsHtml("button")) return false;
    html::ControlView view;
    if (!html::ReadControl(element, view)) return false;
    const layout::BoxStyle& physical = box.style->Physical();
    if (!BackgroundOf(box).Transparent()) return false;
    const auto all = [&](layout::BorderStyle style) { return physical.borderStyle[0] == style && physical.borderStyle[1] == style && physical.borderStyle[2] == style && physical.borderStyle[3] == style; };
    const bool buttonLike = view.kind == "button" || (view.kind == "input" && (view.type == "submit" || view.type == "reset" || view.type == "button" || view.type == "file"));
    const bool fieldLike = view.kind == "input" && view.type != "checkbox" && view.type != "radio" && view.type != "range" && view.type != "color" && !buttonLike;
    const Color ink = ParseColor(box.style->color);
    const auto inkBorder = [&] { return props.borderColors[0].r == ink.r && props.borderColors[0].g == ink.g && props.borderColors[0].b == ink.b && props.borderColors[0].a == ink.a; };
    const bool disabled = view.disabled;
    if (buttonLike) {
      if (!all(layout::BorderStyle::Outset)) return false;
      FillRound(borderBox, 3, disabled ? Rgb(118, 118, 118, 77) : Rgb(118, 118, 118));
      FillRound(RectF{borderBox.x + 1, borderBox.y + 1, std::max(0.0f, borderBox.width - 2), std::max(0.0f, borderBox.height - 2)}, 2,
                disabled ? Rgb(239, 239, 239, 77) : Rgb(239, 239, 239));
      return true;
    }
    if (fieldLike && all(layout::BorderStyle::Inset)) {
      FillRound(borderBox, 2, disabled ? Rgb(118, 118, 118, 77) : Rgb(118, 118, 118));
      FillRound(RectF{borderBox.x + 1, borderBox.y + 1, std::max(0.0f, borderBox.width - 2), std::max(0.0f, borderBox.height - 2)}, 1,
                disabled ? Rgb(239, 239, 239, 77) : Rgb(255, 255, 255));
      return true;
    }
    if ((view.kind == "textarea" || view.kind == "select" || (view.kind == "input" && view.type == "color")) && all(layout::BorderStyle::Solid) && inkBorder()) {
      FillRound(borderBox, 2, disabled ? Rgb(118, 118, 118, 77) : Rgb(118, 118, 118));
      FillRound(RectF{borderBox.x + 1, borderBox.y + 1, std::max(0.0f, borderBox.width - 2), std::max(0.0f, borderBox.height - 2)}, 1,
                disabled ? Rgb(239, 239, 239, 77) : Rgb(255, 255, 255));
      return true;
    }
    return false;
  }

  // The ring round the element that has the focus: for a text field always, for anything else when a key put the focus there (a style may draw its own).
  void PaintFocusRing(const Box& box, const PaintProps& props, const RectF& borderBox) {
    dom::Element* element = box.element();
    if (!element || props.outlineStyle != LineStyle::None) return;
    dom::Document* document = element->nodeDocument;
    if (!document || document->FocusedInView() != element) return;
    bool textEntry = element->IsHtml("textarea");
    if (element->IsHtml("input")) {
      html::ControlView view;
      if (html::ReadControl(element, view)) textEntry = view.type != "checkbox" && view.type != "radio" && view.type != "range" && view.type != "color" && view.type != "submit" && view.type != "reset" && view.type != "button" && view.type != "file";
    }
    if (!textEntry && !document->focusByKeyboard) return;
    DrawOutline ring;
    ring.shape = Rounded(box, borderBox, props, 0, nullptr);
    ring.offset = 0;
    ring.width = 2;
    ring.style = LineStyle::Solid;
    ring.color = Rgb(16, 16, 16);
    outlines_.push_back(ring);
  }

  void PaintControl(const Box& box, const RectF& borderBox) {
    dom::Element* element = box.element();
    if (!element || !box.style || box.style->visibility != layout::Visibility::Visible) return;
    if (!element->IsHtml("input") && !element->IsHtml("select") && !element->IsHtml("textarea")) return;
    if (Culled(borderBox.x, borderBox.y, borderBox.width, borderBox.height)) return;
    html::ControlView view;
    if (!html::ReadControl(element, view)) return;
    const RectF content{borderBox.x + F(box.border.left + box.padding.left), borderBox.y + F(box.border.top + box.padding.top),
                        std::max(0.0f, borderBox.width - F(box.border.Horizontal() + box.padding.Horizontal())),
                        std::max(0.0f, borderBox.height - F(box.border.Vertical() + box.padding.Vertical()))};
    const Color accent = view.disabled ? Rgb(16, 16, 16, 77) : Rgb(0, 117, 255);
    const Color ink = ParseColor(box.style->color);
    (void)ink;
    if (view.kind == "input" && (view.type == "checkbox" || view.type == "radio")) {
      const float side = std::min(content.width, content.height);
      const RectF square{content.x + (content.width - side) / 2, content.y + (content.height - side) / 2, side, side};
      const bool radio = view.type == "radio";
      const Color edge = view.disabled ? Rgb(118, 118, 118, 77) : Rgb(118, 118, 118);
      if (view.checked || (view.indeterminate && !radio)) {
        FillRound(square, radio ? side / 2 : 2.5f, accent);
        if (radio) {
          FillRound(RectF{square.x + side * 0.15f, square.y + side * 0.15f, side * 0.7f, side * 0.7f}, side * 0.35f, Rgb(255, 255, 255));
          FillRound(RectF{square.x + side * 0.3f, square.y + side * 0.3f, side * 0.4f, side * 0.4f}, side * 0.2f, accent);
        } else if (view.indeterminate) {
          Fill(RectF{square.x + side * 0.22f, square.y + side * 0.43f, side * 0.56f, side * 0.14f}, Rgb(255, 255, 255));
        } else {
          FillTurned(RectF{square.x + side * 0.30f, square.y + side * 0.42f, side * 0.14f, side * 0.30f}, -45, Rgb(255, 255, 255));
          FillTurned(RectF{square.x + side * 0.57f, square.y + side * 0.24f, side * 0.14f, side * 0.56f}, 45, Rgb(255, 255, 255));
        }
      } else {
        FillRound(square, radio ? side / 2 : 2.5f, edge);
        FillRound(RectF{square.x + 1, square.y + 1, side - 2, side - 2}, radio ? (side - 2) / 2 : 1.5f, view.disabled ? Rgb(239, 239, 239) : Rgb(255, 255, 255));
      }
      return;
    }
    const std::shared_ptr<font::Face> face = layout::FaceOf(ctx_, tree_, box);
    if (!face) return;
    const double size = box.style->fontSize;
    const double ascent = std::round(face->metrics().ascent * size), descent = std::round(face->metrics().descent * size);
    const Color text = view.disabled ? Rgb(110, 110, 110) : ParseColor(box.style->color);
    PushClip clip;
    clip.rect.rect = content;
    list_->items.push_back(clip);
    const auto baselineIn = [&](double top, double height) { return top + (height - (ascent + descent)) / 2 + ascent; };
    const bool focusedNow = element->nodeDocument && element->nodeDocument->FocusedInView() == element;
    if (view.kind == "select" && (view.rows > 1 || view.multiple)) {
      // A list: the options in rows, the first one shown at the top; what is selected is lit (strongly where the list has the focus).
      const double rowHeight = std::max(1.0, ascent + descent);
      for (size_t i = view.top; i < view.options.size(); ++i) {
        const double y = content.y + static_cast<double>(i - view.top) * rowHeight;
        if (y >= content.y + content.height) break;
        const html::ControlView::Option& option = view.options[i];
        Color ink2 = option.disabled || view.disabled ? Rgb(110, 110, 110) : text;
        if (option.selected) {
          Fill(RectF{content.x, F(y), content.width, F(rowHeight)}, focusedNow ? Rgb(0, 117, 255) : Rgb(200, 200, 200));
          if (focusedNow) ink2 = Rgb(255, 255, 255);
        }
        DrawLine(face, size, option.label, content.x + (option.indented ? 20 : 2), y + ascent, ink2);
      }
      list_->items.push_back(PopClip{});
      return;
    }
    if (view.kind == "select") {
      DrawLine(face, size, view.label, content.x, baselineIn(content.y, content.height), text);
      list_->items.push_back(PopClip{});
      // The arrow of a drop-down: a chevron at the right.
      const float cx = borderBox.x + borderBox.width - F(box.border.right) - 9, cy = borderBox.y + borderBox.height / 2 - 1;
      FillTurned(RectF{cx - 2.9f, cy - 0.5f, 4.4f, 1.4f}, 45, text);
      FillTurned(RectF{cx - 0.6f, cy - 0.5f, 4.4f, 1.4f}, -45, text);
      return;
    }
    if (view.kind == "input" && (view.type == "submit" || view.type == "reset" || view.type == "button")) {
      const double width = face->Measure(view.label) * size;
      DrawLine(face, size, view.label, content.x + (content.width - width) / 2, baselineIn(content.y, content.height), text);
      list_->items.push_back(PopClip{});
      return;
    }
    if (view.kind == "input" && (view.type == "file")) {
      DrawLine(face, size, "Choose File  No file chosen", content.x, baselineIn(content.y, content.height), text);
      list_->items.push_back(PopClip{});
      return;
    }
    if (view.kind == "input" && (view.type == "range" || view.type == "color")) {
      list_->items.push_back(PopClip{});
      if (view.type == "color") {
        FillRound(RectF{content.x + 2, content.y + 2, content.width - 4, content.height - 4}, 2, HexColor(view.value));
      } else {
        const float mid = content.y + content.height / 2;
        FillRound(RectF{content.x, mid - 2, content.width, 4}, 2, Rgb(176, 176, 176));
        const float at = content.x + F(std::clamp(view.fraction, 0.0, 1.0)) * (content.width - 14);
        FillRound(RectF{at, mid - 7, 14, 14}, 7, accent);
      }
      return;
    }
    // A text field (and a text area, whose text is its own boxes: only the caret and the placeholder are drawn here).
    const bool areaOwnsText = view.kind == "textarea";
    const bool focused = element->nodeDocument && element->nodeDocument->FocusedInView() == element;
    const double top = baselineIn(content.y, content.height);
    if (!areaOwnsText) {
      // (A caret in a field that is wider than the box keeps itself in view: the text moves left by what is past the right edge.)
      const std::string before = view.value.substr(0, BytesBefore(view.value, view.end));
      const double caretX = face->Measure(before) * size;
      const double shift = focused && caretX > content.width - 1 ? caretX - (content.width - 1) : 0;
      if (view.value.empty()) {
        DrawLine(face, size, view.placeholder, content.x, top, Rgb(117, 117, 117));
      } else {
        if (focused && view.start != view.end) {
          const size_t a = std::min(view.start, view.end), b = std::max(view.start, view.end);
          const double x0 = face->Measure(view.value.substr(0, BytesBefore(view.value, a))) * size, x1 = face->Measure(view.value.substr(0, BytesBefore(view.value, b))) * size;
          Fill(RectF{content.x + F(x0 - shift), content.y, F(x1 - x0), content.height}, Rgb(179, 212, 252));
        }
        DrawLine(face, size, view.value, content.x - shift, top, text);
      }
      if (focused && !view.disabled && !view.readOnly && g_caretOn) Fill(RectF{content.x + F(caretX - shift), F(top - ascent), 1, F(ascent + descent)}, text);
    } else if (view.value.empty()) {
      if (!view.placeholder.empty()) DrawLine(face, size, view.placeholder, content.x, content.y + ascent, Rgb(117, 117, 117));
      if (focused && !view.disabled && !view.readOnly && g_caretOn) Fill(RectF{content.x, content.y, 1, F(ascent + descent)}, text);
    } else {
      // The text of an area: its lines wrapped to the width (the scrollbar's room left out), the part with the caret in view when the area has the focus.
      const double rowHeight = std::max(1.0, ascent + descent);
      const std::vector<layout::TextLine> lines = layout::WrapText(*face, size, view.value, std::max(1.0, content.width - 15.0));
      const size_t caretByte = BytesBefore(view.value, view.end);
      size_t caretLine = 0;
      for (size_t i = 0; i < lines.size(); ++i) {
        if (caretByte >= lines[i].begin) caretLine = i;
      }
      const size_t visible = std::max<size_t>(1, static_cast<size_t>(content.height / rowHeight));
      size_t first = 0;
      if (focused && caretLine >= visible) first = caretLine - visible + 1;
      const size_t from = std::min(view.start, view.end), to = std::max(view.start, view.end);
      const size_t byteFrom = BytesBefore(view.value, from), byteTo = BytesBefore(view.value, to);
      for (size_t i = first; i < lines.size() && i - first < visible + 1; ++i) {
        const double y = content.y + static_cast<double>(i - first) * rowHeight;
        const std::string line = view.value.substr(lines[i].begin, lines[i].end - lines[i].begin);
        if (focused && byteFrom != byteTo && byteTo > lines[i].begin && byteFrom <= lines[i].end) {
          const size_t a = std::max(byteFrom, lines[i].begin), b = std::min(byteTo, lines[i].end);
          const double x0 = face->Measure(view.value.substr(lines[i].begin, a - lines[i].begin)) * size, x1 = face->Measure(view.value.substr(lines[i].begin, b - lines[i].begin)) * size;
          Fill(RectF{content.x + F(x0), F(y), F(std::max(x1 - x0, byteTo > lines[i].end ? 4.0 : 0.0)), F(rowHeight)}, Rgb(179, 212, 252));
        }
        DrawLine(face, size, line, content.x, y + ascent, text);
        if (focused && !view.disabled && !view.readOnly && g_caretOn && i == caretLine) {
          const double cx = face->Measure(view.value.substr(lines[i].begin, std::min(caretByte, lines[i].end) - lines[i].begin)) * size;
          Fill(RectF{content.x + F(cx), F(y), 1, F(rowHeight)}, text);
        }
      }
    }
    list_->items.push_back(PopClip{});
  }

  void PaintBorder(const Box& box, const PaintProps& props, const layout::BoxStyle& physical, const RectF& borderBox, const layout::Edges& widths) {
    DrawBorder border;
    border.outer = Rounded(box, borderBox, props, 0, nullptr);
    RectF inner{borderBox.x + F(widths.left), borderBox.y + F(widths.top), std::max(0.0f, borderBox.width - F(widths.left + widths.right)),
                std::max(0.0f, borderBox.height - F(widths.top + widths.bottom))};
    border.inner = Rounded(box, inner, props, 1, &widths);
    const double w[4] = {widths.top, widths.right, widths.bottom, widths.left};
    bool any = false;
    for (int i = 0; i < 4; ++i) {
      border.widths[i] = F(w[i]);
      border.styles[i] = w[i] > 0 ? LineStyleOf(physical.borderStyle[i]) : LineStyle::None;
      border.colors[i] = props.borderColors[i];
      if (border.styles[i] != LineStyle::None && border.styles[i] != LineStyle::Hidden && !border.colors[i].Transparent()) any = true;
    }
    if (any) list_->items.push_back(border);
  }

  // ---- Images ----

  ImageRef ImageFor(const std::string& url) {
    const auto found = images_.find(url);
    if (found != images_.end()) return found->second;
    ImageRef ref;
    std::string absolute;
    const std::shared_ptr<const std::string> bytes = layout::ImageDataOf(url, "", &absolute);
    if (bytes && !bytes->empty()) ref = KeptImage(absolute, bytes);
    images_[url] = ref;
    return ref;
  }

  // The rectangle a box keyword (border-box, padding-box, content-box) names, and its corners.
  RectF BoxArea(const std::string& keyword, const RectF& borderBox, const layout::Edges& border, const layout::Edges& padding, layout::Edges& insets) const {
    insets = layout::Edges();
    if (keyword == "padding-box" || keyword == "content-box") {
      insets = border;
      if (keyword == "content-box") {
        insets.top += padding.top;
        insets.right += padding.right;
        insets.bottom += padding.bottom;
        insets.left += padding.left;
      }
    }
    return RectF{borderBox.x + F(insets.left), borderBox.y + F(insets.top), std::max(0.0f, borderBox.width - F(insets.Horizontal())), std::max(0.0f, borderBox.height - F(insets.Vertical()))};
  }

  // The images of the background (or the mask) of a box, the lowest first.
  std::vector<ImageLayer> LayersOf(const Box& box, const RectF& borderBox, const PaintProps& props, bool mask) {
    std::vector<ImageLayer> out;
    std::string pseudo;
    dom::Element* element = ElementOf(box, pseudo);
    if (!element) return out;
    css::StyleReader reader(ctx_, element, pseudo);
    const std::string prefix = mask ? "mask-" : "background-";
    const std::string images = Trim(reader.Get(prefix + "image"));
    if (images.empty() || images == "none") return out;
    const std::vector<std::string> items = SplitList(images, ',');
    const std::vector<std::string> sizes = SplitList(reader.Get(prefix + "size"), ','), positions = SplitList(reader.Get(prefix + "position"), ',');
    const std::vector<std::string> repeats = SplitList(reader.Get(prefix + "repeat"), ','), origins = SplitList(reader.Get(prefix + "origin"), ','), clips = SplitList(reader.Get(prefix + "clip"), ',');
    const auto at = [&](const std::vector<std::string>& values, size_t i) { return values.empty() ? std::string() : Trim(values[i % values.size()]); };
    const std::string rendering = reader.Get("image-rendering");
    for (size_t k = items.size(); k-- > 0;) {
      const std::string url = UrlOf(items[k]);
      ImageLayer layer;
      if (!url.empty()) {
        layer.image = ImageFor(url);
        if (!layer.image) continue;
      } else {
        layer.gradient = ParseGradient(items[k]);
        if (!layer.gradient) continue;
      }
      layer.pixelated = rendering == "pixelated" || rendering == "crisp-edges";
      layout::Edges insets;
      std::string origin = at(origins, k);
      if (origin.empty()) origin = mask ? "border-box" : "padding-box";
      layer.origin = BoxArea(origin, borderBox, box.border, box.padding, insets);
      std::string clipWord = at(clips, k);
      if (clipWord.empty() || clipWord == "text" || clipWord == "no-clip") clipWord = "border-box";
      const RectF clipRect = BoxArea(clipWord, borderBox, box.border, box.padding, insets);
      layer.clip = Rounded(box, clipRect, props, clipWord == "border-box" ? 0 : 1, &insets);
      const std::vector<std::string> size = SplitWords(at(sizes, k));
      if (!size.empty() && size[0] == "cover") layer.size = ImageLayer::Size::Cover;
      else if (!size.empty() && size[0] == "contain") layer.size = ImageLayer::Size::Contain;
      else if (!size.empty()) {
        layer.size = ImageLayer::Size::Lengths;
        ParseLength(size[0], layer.width);
        if (size.size() > 1) ParseLength(size[1], layer.height);
      }
      std::vector<std::string> position = SplitWords(at(positions, k));
      const auto keyword = [](const std::string& w, bool horizontal) -> std::string {
        if (w == "left" || w == "top") return "0%";
        if (w == "center") return "50%";
        if (w == "right" || w == "bottom") return "100%";
        (void)horizontal;
        return w;
      };
      if (position.size() == 1) position.push_back("50%");
      if (position.size() == 2) {
        ParseLength(keyword(position[0], true), layer.x);
        ParseLength(keyword(position[1], false), layer.y);
      }
      std::vector<std::string> repeat = SplitWords(at(repeats, k));
      if (repeat.size() == 1) {
        if (repeat[0] == "repeat-x") repeat = {"repeat", "no-repeat"};
        else if (repeat[0] == "repeat-y") repeat = {"no-repeat", "repeat"};
        else repeat.push_back(repeat[0]);
      }
      if (repeat.size() >= 2) {
        layer.repeatX = RepeatOf(repeat[0]);
        layer.repeatY = RepeatOf(repeat[1]);
      }
      out.push_back(std::move(layer));
    }
    return out;
  }

  // The picture of a video: the player's current one, in the content box.
  void PaintVideoOf(const Box& box, const RectF& borderBox, const PaintProps& props) {
    if (!box.replaced || box.kind != Box::Kind::Block) return;
    dom::Element* element = box.element();
    if (!element || !element->IsHtml("video")) return;
    DrawVideo item;
    item.player = media::PlayerOf(element);
    if (!box.imageUrl.empty()) item.poster = ImageFor(box.imageUrl);  // (the poster attribute, made absolute by layout)
    if (!item.player && !item.poster) return;
    item.controls = element->FindAttribute("controls") != nullptr;
    layout::Edges insets;
    insets.top = box.border.top + box.padding.top;
    insets.right = box.border.right + box.padding.right;
    insets.bottom = box.border.bottom + box.padding.bottom;
    insets.left = box.border.left + box.padding.left;
    const RectF content{borderBox.x + F(insets.left), borderBox.y + F(insets.top), std::max(0.0f, borderBox.width - F(insets.Horizontal())), std::max(0.0f, borderBox.height - F(insets.Vertical()))};
    item.clip = Rounded(box, content, props, 1, &insets);
    std::string pseudo;
    css::StyleReader reader(ctx_, element, pseudo);
    const std::string fit = reader.Get("object-fit");
    item.fit = fit == "fill" ? ObjectFit::Fill : fit == "cover" ? ObjectFit::Cover : fit == "none" ? ObjectFit::None : fit == "scale-down" ? ObjectFit::ScaleDown : ObjectFit::Contain;
    const std::vector<std::string> position = SplitWords(reader.Get("object-position"));
    if (position.size() == 2) {
      ParseLength(position[0], item.x);
      ParseLength(position[1], item.y);
    }
    list_->items.push_back(std::move(item));
  }

  // The picture of an img (and the like): in its content box.
  void PaintImageOf(const Box& box, const RectF& borderBox, const PaintProps& props) {
    if (std::getenv("SOLAR_PAINT_DEBUG") && box.element() && box.element()->IsHtml("img")) std::fprintf(stderr, "paint img box replaced=%d kind=%d url=[%s] size %gx%g\n", box.replaced, (int)box.kind, box.imageUrl.c_str(), box.width, box.height);
    if (!box.replaced || box.kind != Box::Kind::Block) return;
    if (box.element() && box.element()->IsHtml("video")) return;  // (a video has its own, below)
    ImageRef image;
    if (box.imageUrl.empty()) {
      // An svg in the markup is drawn as the picture of its own source (with the color it is given, for currentColor).
      dom::Element* svg = box.element();
      if (!svg || svg->namespaceUri != dom::kSvgNamespace || svg->localName != "svg") return;
      std::optional<std::string> source = html::SerializeXml(svg, false);
      if (!source || source->compare(0, 4, "<svg") != 0) return;
      std::string pseudo;
      const std::string color = css::ComputedValue(ctx_, svg, "color", pseudo);
      source->insert(4, " color=\"" + color + "\"");
      image = KeptImage("inline-svg:" + std::to_string(std::hash<std::string>{}(*source)), std::make_shared<const std::string>(std::move(*source)));
    } else {
      image = ImageFor(box.imageUrl);
    }
    if (std::getenv("SOLAR_PAINT_DEBUG")) std::fprintf(stderr, "img %s -> %s\n", box.imageUrl.c_str(), image ? "bytes" : "none");
    if (!image) return;
    layout::Edges insets;
    insets.top = box.border.top + box.padding.top;
    insets.right = box.border.right + box.padding.right;
    insets.bottom = box.border.bottom + box.padding.bottom;
    insets.left = box.border.left + box.padding.left;
    const RectF content{borderBox.x + F(insets.left), borderBox.y + F(insets.top), std::max(0.0f, borderBox.width - F(insets.Horizontal())), std::max(0.0f, borderBox.height - F(insets.Vertical()))};
    DrawImage item;
    item.image = image;
    item.clip = Rounded(box, content, props, 1, &insets);
    std::string pseudo;
    if (dom::Element* element = ElementOf(box, pseudo)) {
      css::StyleReader reader(ctx_, element, pseudo);
      const std::string fit = reader.Get("object-fit");
      item.fit = fit == "contain" ? ObjectFit::Contain : fit == "cover" ? ObjectFit::Cover : fit == "none" ? ObjectFit::None : fit == "scale-down" ? ObjectFit::ScaleDown : ObjectFit::Fill;
      const std::vector<std::string> position = SplitWords(reader.Get("object-position"));
      if (position.size() == 2) {
        ParseLength(position[0], item.x);
        ParseLength(position[1], item.y);
      }
      const std::string rendering = reader.Get("image-rendering");
      item.pixelated = rendering == "pixelated" || rendering == "crisp-edges";
    }
    list_->items.push_back(std::move(item));
  }

  // ---- Text ----

  Decoration DecorationOf(const Box& textBox) {
    const auto cached = decorations_.find(&textBox);
    if (cached != decorations_.end()) return cached->second;
    Decoration out;
    for (const Box* b = textBox.parent; b; b = b->parent) {
      if (b->kind != Box::Kind::Text && b->style) {
        std::string pseudo;
        dom::Element* element = ElementOf(*b, pseudo);
        if (element) {
          const std::string line = css::ComputedValue(ctx_, element, "text-decoration-line", pseudo);
          if (!line.empty() && line != "none") {
            Color color = ParseColor(css::ComputedValue(ctx_, element, "text-decoration-color", pseudo));
            if (color.Transparent()) color = ParseColor(b->style->color);
            if (line.find("underline") != std::string::npos) out.underline = true;
            if (line.find("overline") != std::string::npos) out.overline = true;
            if (line.find("line-through") != std::string::npos) out.lineThrough = true;
            if (out.underline || out.overline || out.lineThrough) out.color = color;
          }
        }
        // (A decoration does not go past a box that is on its own: a float, a positioned or an atomic one.)
        if (b->kind == Box::Kind::Block && (b->inlineLevel || b->style->IsFloating() || b->style->IsOutOfFlow())) break;
      }
    }
    decorations_[&textBox] = out;
    return out;
  }

  // The text-shadow of a piece of text: that of the element it is in.
  const std::vector<Shadow>& TextShadowsOf(const Box& text) {
    auto found = textShadows_.find(&text);
    if (found != textShadows_.end()) return found->second;
    std::vector<Shadow> shadows;
    for (const Box* b = text.parent; b; b = b->parent) {
      std::string pseudo;
      if (dom::Element* element = ElementOf(*b, pseudo)) {
        shadows = ParseShadows(css::ComputedValue(ctx_, element, "text-shadow", pseudo));
        break;
      }
    }
    return textShadows_.emplace(&text, std::move(shadows)).first->second;
  }

  // The part of a piece of text the selection of the page covers is lit.
  void PaintSelection(const Box& box, const layout::TextPiece& piece, double x, double top, double height, double size) {
    if (!box.node || !box.node->IsText()) return;
    const dom::Document* document = box.node->nodeDocument;
    const dom::Range* range = document ? dom::SelectionRangeOf(const_cast<dom::Document*>(document)) : nullptr;
    if (!range || range->Collapsed() || !range->startNode || !range->endNode) return;
    dom::Node* node = const_cast<dom::Node*>(box.node);
    const std::string& data = static_cast<const dom::CharacterData*>(node)->Data();
    const uint32_t length = static_cast<uint32_t>(layout::UnitsBeforeByte(data, data.size()));
    // The units of the node's data that are inside the range.
    int64_t lo = range->startNode == node ? range->startOffset : (dom::ComparePoints(node, 0, range->startNode, range->startOffset) >= 0 ? 0 : static_cast<int64_t>(length) + 1);
    int64_t hi = range->endNode == node ? range->endOffset : (dom::ComparePoints(node, length, range->endNode, range->endOffset) <= 0 ? static_cast<int64_t>(length) : -1);
    if (lo > hi || lo > length) return;
    const size_t loByte = layout::BytesBeforeUnit(data, static_cast<size_t>(lo)), hiByte = layout::BytesBeforeUnit(data, static_cast<size_t>(std::max<int64_t>(hi, 0)));
    // Where the piece's processed text has those bytes of the data: the first processed byte that comes from at or after each.
    const auto processedAt = [&](size_t dataByte) {
      for (size_t p = piece.begin; p < piece.end && p < box.processedSource.size(); ++p) {
        if (box.textOffset + box.processedSource[p] >= dataByte) return p;
      }
      return piece.end;
    };
    const size_t from = processedAt(loByte), to = processedAt(hiByte);
    if (to <= from) return;
    // The pen position of a processed byte: the advances of the glyphs before it.
    const auto penAt = [&](size_t processed) {
      double pen = 0;
      for (const font::Glyph& g : piece.glyphs) {
        if (piece.begin + g.cluster >= processed) break;
        pen += g.advance * size;
      }
      return pen;
    };
    const double x0 = penAt(from), x1 = to >= piece.end ? penAt(piece.end + 1) : penAt(to);
    Fill(RectF{F(x + x0), F(top), F(std::max(0.0, x1 - x0)), F(height)}, Rgb(179, 212, 252));
  }

  void PaintText(const Box& container, const LineItem& item, const Rect& origin) {
    const Box* box = item.box;
    if (!box || !box->style || box->style->visibility != layout::Visibility::Visible || !item.text.face) return;
    const layout::TextPiece& piece = item.text;
    if (piece.glyphs.empty()) return;
    if (Culled(origin.x + item.rect.x, origin.y + item.rect.y, item.rect.width, item.rect.height)) return;
    const double size = piece.fontSize;
    const font::Metrics& metrics = piece.face->metrics();
    const double ascent = std::round(metrics.ascent * size);
    const double x = origin.x + item.rect.x;
    const double baseline = origin.y + item.rect.y + ascent;
    const Color color = ParseColor(box->style->color);
    PaintSelection(*box, piece, x, origin.y + item.rect.y, item.rect.height, size);
    if (!color.Transparent()) {
      // (Each stretch of glyphs is of its own face: the ids are of that font.)
      const std::vector<Shadow>& shadows = TextShadowsOf(*box);
      std::vector<layout::TextPiece::FaceRun> faces = piece.faceRuns;
      if (faces.empty()) faces.push_back({piece.face, 0, piece.glyphs.size()});
      double pen = 0;
      for (const layout::TextPiece::FaceRun& stretch : faces) {
        if (!stretch.face || stretch.end <= stretch.begin) {
          for (size_t i = stretch.begin; i < stretch.end; ++i) pen += piece.glyphs[i].advance * size;
          continue;
        }
        DrawGlyphs run;
        run.face = stretch.face;
        run.size = F(size);
        run.color = color;
        run.x = F(x);
        run.y = F(baseline);
        run.shadows = shadows;
        for (size_t i = stretch.begin; i < stretch.end; ++i) {
          const font::Glyph& g = piece.glyphs[i];
          run.glyphs.push_back(g.id);
          run.offsets.push_back(F(pen + g.xOffset * size));
          run.offsets.push_back(F(-g.yOffset * size));
          pen += g.advance * size;
        }
        list_->items.push_back(std::move(run));
      }
    }
    const Decoration decoration = DecorationOf(*box);
    if (decoration.underline || decoration.overline || decoration.lineThrough) {
      double width = 0;
      for (const font::Glyph& g : piece.glyphs) width += g.advance * size;
      const double thickness = std::max(1.0, std::round(metrics.underlineThickness * size));
      const auto line = [&](double center) { Fill(RectF{F(x), F(center - thickness / 2), F(width), F(thickness)}, decoration.color); };
      if (decoration.underline) line(baseline - metrics.underlinePosition * size);
      if (decoration.overline) line(baseline - ascent);
      if (decoration.lineThrough) line(baseline - metrics.strikeoutPosition * size);
    }
    (void)container;
  }

  // The inline content of a block container: the backgrounds and borders of its inline boxes, then its text and atomic inlines.
  void PaintLines(const Box& owner) {
    const Rect origin = layout::AbsoluteBorderBox(owner);
    // Inline boxes, in tree order, with each of their fragments.
    std::function<void(const Box&)> boxes = [&](const Box& parent) {
      for (const auto& child : parent.children) {
        if (child->kind == Box::Kind::Inline) {
          const size_t count = child->fragments.size();
          for (size_t i = 0; i < count; ++i) {
            const Rect& f = child->fragments[i];
            PaintDecoration(*child, Rect{origin.x + f.x, origin.y + f.y, f.width, f.height}, i == 0, i + 1 == count);
          }
          boxes(*child);
        }
      }
    };
    EnsureClipsForContentsOf(owner);
    boxes(owner);
    for (const Line& line : owner.lines) {
      for (const LineItem& item : line.items) {
        if (item.kind == LineItem::Kind::Text) {
          PaintText(owner, item, origin);
        } else if (item.kind == LineItem::Kind::Atomic && item.box) {
          PaintStackingChild(*item.box);
          EnsureClipsForContentsOf(owner);
        }
      }
    }
  }

  // ---- Clips and what is scrolled ----

  static bool ClipsOverflow(const Box& b) {
    if (b.kind != Box::Kind::Block || !b.style) return false;
    const layout::BoxStyle& p = b.style->Physical();
    return p.overflowX != layout::Overflow::Visible || p.overflowY != layout::Overflow::Visible || b.style->containPaint;
  }

  // Where a box that is positioned goes up to: its containing block, which is what clips it (the boxes between do not).
  const Box* Above(const Box& b) const {
    if (b.kind == Box::Kind::Block && b.style && b.style->position == layout::Position::Absolute) {
      for (const Box* p = b.parent; p; p = p->parent) {
        if (p->style && (p->style->position != layout::Position::Static || p->style->containsPositioned)) return p;
      }
      return nullptr;
    }
    if (b.kind == Box::Kind::Block && b.style && b.style->position == layout::Position::Fixed) {
      for (const Box* p = b.parent; p; p = p->parent) {
        if (p->style && p->style->containsPositioned) return p;
      }
      return nullptr;
    }
    return b.parent;
  }

  // The boxes that cut what is painted of `box`, outermost first.
  std::vector<const Box*> ClipChain(const Box& box) {
    const auto cached = clipChains_.find(&box);
    if (cached != clipChains_.end()) return cached->second;
    std::vector<const Box*> chain;
    for (const Box* up = Above(box); up; up = Above(*up)) {
      if (!up->parent) break;  // the initial containing block
      if (!ClipsOverflow(*up)) continue;
      const dom::Element* element = up->element();
      // (The overflow of the root element and of the body belongs to the viewport.)
      if (element && (element->IsHtml("html") || (element->IsHtml("body") && up->parent && up->parent->element() && up->parent->element()->IsHtml("html") && up == canvasFrom_))) continue;
      chain.push_back(up);
    }
    std::reverse(chain.begin(), chain.end());
    clipChains_[&box] = chain;
    return chain;
  }

  void EnsureClipsFor(const Box& box) { EnsureClips(ClipChain(box)); }

  // What cuts the contents of a box: what cuts the box, and the box itself when it clips its overflow.
  void EnsureClipsForContentsOf(const Box& box) {
    std::vector<const Box*> chain = ClipChain(box);
    const dom::Element* element = box.element();
    const bool viewport = element && (element->IsHtml("html") || (element->IsHtml("body") && box.parent && box.parent->element() && box.parent->element()->IsHtml("html") && &box == canvasFrom_));
    if (box.parent && ClipsOverflow(box) && !viewport) chain.push_back(&box);
    EnsureClips(chain);
  }

  void EnsureClips(const std::vector<const Box*>& chain) {
    const size_t base = frames_.empty() ? 0 : frames_.back().base;
    std::vector<const Box*> want;
    for (const Box* c : chain) {
      if (std::find(clipStack_.begin(), clipStack_.begin() + static_cast<std::ptrdiff_t>(base), c) == clipStack_.begin() + static_cast<std::ptrdiff_t>(base)) want.push_back(c);
    }
    size_t keep = 0;
    while (base + keep < clipStack_.size() && keep < want.size() && clipStack_[base + keep] == want[keep]) ++keep;
    while (clipStack_.size() > base + keep) {
      PopClipOf(*clipStack_.back());
      clipStack_.pop_back();
    }
    for (size_t i = keep; i < want.size(); ++i) {
      PushClipOf(*want[i]);
      clipStack_.push_back(want[i]);
    }
  }

  void PushClipOf(const Box& box) {
    const Rect absolute = layout::AbsoluteBorderBox(box);
    const RectF borderBox = ToRectF(absolute);
    const PaintProps& props = PropsOf(box, borderBox);
    const layout::Edges& b = box.border;
    RectF padding{borderBox.x + F(b.left), borderBox.y + F(b.top), std::max(0.0f, borderBox.width - F(b.Horizontal())), std::max(0.0f, borderBox.height - F(b.Vertical()))};
    PushClip clip;
    clip.rect = Rounded(box, padding, props, 1, &b);
    list_->items.push_back(clip);
    const auto scrolled = box.node ? tree_.scroll.find(box.node) : tree_.scroll.end();
    if (scrolled != tree_.scroll.end() && (scrolled->second.first != 0 || scrolled->second.second != 0)) {
      PushTransformOf(-scrolled->second.first, -scrolled->second.second);
      scrollPushed_.insert(&box);
    }
  }

  void PopClipOf(const Box& box) {
    if (scrollPushed_.erase(&box)) {
      list_->items.push_back(PopTransform{});
      --transformDepth_;
    }
    list_->items.push_back(PopClip{});
  }

  // ---- Stacking contexts ----

  static int ZIndexOf(const Box& b) {
    const std::string& z = b.style->zIndex;
    return z == "auto" || z.empty() ? 0 : std::atoi(z.c_str());
  }

  static bool Positioned(const Box& b) { return b.style && b.style->position != layout::Position::Static; }

  struct Lists {
    std::vector<const Box*> blocks, floats, inlineOwners;
    std::vector<std::pair<int, const Box*>> negative, zero, positive;
  };

  void Collect(const Box& parent, Lists& lists) {
    // (The text of a text area is drawn by the control from its value, not from the text it was made with.)
    const bool areaText = parent.formControl && parent.element() && parent.element()->IsHtml("textarea");
    if (parent.kind == Box::Kind::Block && parent.hasInlineContent && !parent.lines.empty() && !areaText) lists.inlineOwners.push_back(&parent);
    for (const auto& child : parent.children) {
      const Box& c = *child;
      if (!c.style) continue;
      if (c.kind == Box::Kind::Block) {
        if (c.inlineLevel) {
          // An atomic inline is painted with the line it is in; what is positioned in it is its own.
          continue;
        }
        if (Positioned(c) || c.style->stackingContext) {
          const int z = ZIndexOf(c);
          (z < 0 ? lists.negative : z > 0 ? lists.positive : lists.zero).push_back({z, &c});
          continue;
        }
        if (c.style->IsFloating()) {
          lists.floats.push_back(&c);
          continue;
        }
        if (c.outsideMarker && !c.hasInlineContent && c.children.empty()) continue;
        lists.blocks.push_back(&c);
        Collect(c, lists);
      } else if (c.kind == Box::Kind::Inline) {
        Collect(c, lists);  // (what is positioned or floating in an inline box)
      }
    }
  }

  // The effects of a box on what is painted of it, then the box as a stacking context (its own, or for the purpose of the one it is in).
  void PaintStackingChild(const Box& box) {
    if (!box.style || box.kind != Box::Kind::Block) return;
    EnsureClipsFor(box);
    const Rect absolute = layout::AbsoluteBorderBox(box);
    bool layer = false, transformed = false, fixedUndo = false, stickyUndo = false;
    if (box.style->opacity < 1) {
      PushLayer item;
      item.opacity = F(std::max(0.0, box.style->opacity));
      list_->items.push_back(item);
      layer = true;
    }
    layout::Matrix matrix;
    if (layout::TransformOf(box, matrix) && !matrix.IsIdentity()) {
      PushTransform item;
      // About the box's origin: it is the matrix of the box, in the coordinates of its own border box.
      layout::Matrix to, back;
      to.m[12] = absolute.x;
      to.m[13] = absolute.y;
      back.m[12] = -absolute.x;
      back.m[13] = -absolute.y;
      const layout::Matrix total = to * matrix * back;
      for (int i = 0; i < 16; ++i) item.matrix.m[i] = F(total.m[i]);
      list_->items.push_back(item);
      ++transformDepth_;
      transformed = true;
    }
    if (box.style->position == layout::Position::Fixed && (scrollX_ != 0 || scrollY_ != 0) && !transformed) {
      PushTransformOf(scrollX_, scrollY_);  // (a fixed box stays where it is in the viewport)
      std::get<PushTransform>(list_->items.back()).followsScroll = true;
      fixedUndo = true;
    }
    if (box.style->position == layout::Position::Sticky && !transformed && !fixedUndo) {
      paint::StickyInfo sticky;
      if (layout::StickyInfoOf(tree_, box, sticky)) {
        PushTransformOf(-box.stickyX, -box.stickyY);  // (the box is where it is held now: it is put where it is without that, and moved as the scroll asks)
        std::get<PushTransform>(list_->items.back()).sticky = sticky;
        stickyUndo = true;
      }
    }
    std::vector<ImageLayer> masks;
    {
      const RectF area = ToRectF(absolute);
      masks = LayersOf(box, area, PropsOf(box, area), true);
    }
    if (!masks.empty()) list_->items.push_back(PushMask{});
    frames_.push_back(Frame{clipStack_.size()});
    PaintContext(box);
    EnsureClips({});
    frames_.pop_back();
    if (!masks.empty()) {
      PopMask popped;
      popped.layers = std::move(masks);
      list_->items.push_back(std::move(popped));
    }
    if (stickyUndo) {
      list_->items.push_back(PopTransform{});
      --transformDepth_;
    }
    if (fixedUndo) {
      list_->items.push_back(PopTransform{});
      --transformDepth_;
    }
    if (transformed) {
      list_->items.push_back(PopTransform{});
      --transformDepth_;
    }
    if (layer) list_->items.push_back(PopLayer{});
  }

  void PaintContext(const Box& root) {
    const size_t outlinesFrom = outlines_.size();
    PaintContextContents(root);
    // Outlines are over everything of the context.
    if (outlines_.size() > outlinesFrom) {
      EnsureClipsFor(root);
      for (size_t i = outlinesFrom; i < outlines_.size(); ++i) list_->items.push_back(outlines_[i]);
      outlines_.resize(outlinesFrom);
    }
  }

  void PaintContextContents(const Box& root) {
    if (root.style && root.style->visibility == layout::Visibility::Visible) {
      EnsureClipsFor(root);
      PaintDecoration(root, layout::AbsoluteBorderBox(root), true, true);
    }
    Lists lists;
    Collect(root, lists);
    const auto inside = [&](const Box& b) { EnsureClipsFor(b); };
    const auto byZ = [](const std::pair<int, const Box*>& a, const std::pair<int, const Box*>& b) { return a.first < b.first; };
    std::stable_sort(lists.negative.begin(), lists.negative.end(), byZ);
    std::stable_sort(lists.positive.begin(), lists.positive.end(), byZ);
    for (const auto& n : lists.negative) PaintStackingChild(*n.second);
    for (const Box* b : lists.blocks) {
      inside(*b);
      PaintDecoration(*b, layout::AbsoluteBorderBox(*b), true, true);
    }
    for (const Box* f : lists.floats) PaintStackingChild(*f);
    for (const Box* o : lists.inlineOwners) {
      PaintLines(*o);
    }
    for (const auto& z : lists.zero) PaintStackingChild(*z.second);
    for (const auto& p : lists.positive) PaintStackingChild(*p.second);
  }

  Quanta::Context& ctx_;
  Tree& tree_;
  DisplayList* list_ = nullptr;
  double scrollX_ = 0, scrollY_ = 0;
  double overscan_ = 0;
  const Box* canvasFrom_ = nullptr;
  std::vector<Frame> frames_;
  std::vector<const Box*> clipStack_;
  std::vector<DrawOutline> outlines_;
  std::unordered_map<const Box*, std::vector<Shadow>> textShadows_;
  std::unordered_map<std::string, ImageRef> images_;
  PropsMap* props_ = nullptr;
  std::unordered_map<const Box*, Decoration> decorations_;
  std::unordered_map<const Box*, std::vector<const Box*>> clipChains_;
  std::set<const Box*> scrollPushed_;
  int transformDepth_ = 0;
};

}  // namespace

DisplayList BuildDisplayList(Quanta::Context& ctx, layout::Tree& tree, double overscan) {
  // What is painted is the tree as it was committed (see dom::CommittedViewScope).
  dom::CommittedViewScope view;
  layout::LayoutFreeze freeze;
  Painter painter(ctx, tree, overscan);
  return painter.Run();
}

void SetCaretVisible(bool visible) { g_caretOn = visible; }

void SetPopup(const Popup& popup) { g_popup = popup; }

bool TextPointAt(layout::Tree& tree, double x, double y, dom::Node*& node, uint32_t& offset, dom::Element** block) {
  using layout::Box;
  using layout::LineItem;
  // The piece of text nearest to the point: first by how far it is from the point up or down, then sideways.
  double bestDy = 1e18, bestDx = 1e18;
  const Box* bestOwner = nullptr;
  const LineItem* best = nullptr;
  Rect bestOrigin;
  std::function<void(const Box&)> walk = [&](const Box& box) {
    if (box.kind == Box::Kind::Block && box.hasInlineContent && !box.lines.empty() && !(box.formControl && box.element() && box.element()->IsHtml("textarea"))) {
      const Rect origin = layout::AbsoluteBorderBox(box);
      for (const layout::Line& line : box.lines) {
        for (const LineItem& item : line.items) {
          if (item.kind != LineItem::Kind::Text || !item.box || !item.box->node || !item.box->node->IsText() || item.text.glyphs.empty()) continue;
          if (item.box->style && item.box->style->visibility != layout::Visibility::Visible) continue;
          const double top = origin.y + item.rect.y, left = origin.x + item.rect.x;
          const double dy = y < top ? top - y : y > top + item.rect.height ? y - (top + item.rect.height) : 0;
          const double dx = x < left ? left - x : x > left + item.rect.width ? x - (left + item.rect.width) : 0;
          if (dy < bestDy || (dy == bestDy && dx < bestDx)) {
            bestDy = dy;
            bestDx = dx;
            best = &item;
            bestOwner = &box;
            bestOrigin = origin;
          }
        }
      }
    }
    for (const auto& child : box.children) {
      if (child->kind == Box::Kind::Block || child->kind == Box::Kind::Inline) walk(*child);
    }
  };
  if (!tree.root) return false;
  walk(*tree.root);
  if (!best) return false;
  const layout::TextPiece& piece = best->text;
  const Box& text = *best->box;
  const double size = piece.fontSize;
  const double want = x - (bestOrigin.x + best->rect.x);
  // The boundary between the glyphs nearest to the point (the end of the piece included).
  size_t at = piece.begin;
  double pen = 0, bestDistance = std::fabs(want);
  for (const font::Glyph& g : piece.glyphs) {
    pen += g.advance * size;
    if (std::fabs(want - pen) < bestDistance) {
      bestDistance = std::fabs(want - pen);
      at = piece.end;
      // (The boundary after this glyph is where the next one's cluster begins.)
      const size_t index = static_cast<size_t>(&g - piece.glyphs.data());
      if (index + 1 < piece.glyphs.size()) at = piece.begin + piece.glyphs[index + 1].cluster;
    }
  }
  // The processed byte back to the node's data, and that to UTF-16 units.
  size_t dataByte = text.textOffset + text.text.size();
  if (at < text.processedSource.size()) dataByte = text.textOffset + text.processedSource[at];
  const std::string& data = static_cast<const dom::CharacterData*>(text.node)->Data();
  node = text.node;
  offset = static_cast<uint32_t>(layout::UnitsBeforeByte(data, std::min(dataByte, data.size())));
  if (block) *block = bestOwner ? bestOwner->element() : nullptr;
  return true;
}

bool PopupGeometryOf(Quanta::Context& ctx, layout::Tree& tree, dom::Element* select, size_t top, PopupGeometry& out) {
  const auto boxes = tree.boxesOf.find(select);
  if (boxes == tree.boxesOf.end() || boxes->second.empty()) return false;
  const Box& box = *boxes->second.front();
  if (!box.style) return false;
  html::ControlView view;
  if (!html::ReadControl(select, view) || view.options.empty()) return false;
  const std::shared_ptr<font::Face> face = layout::FaceOf(ctx, tree, box);
  if (!face) return false;
  const double size = box.style->fontSize;
  const double ascent = std::round(face->metrics().ascent * size), descent = std::round(face->metrics().descent * size);
  const layout::Rect where = layout::AbsoluteBorderBox(box);
  out.rowHeight = ascent + descent + 2;
  double widest = 0;
  for (const html::ControlView::Option& option : view.options) widest = std::max(widest, face->Measure(option.label) * size + (option.indented ? 20 : 4) + 8);
  out.width = std::max(where.width, widest);
  out.rows = std::min<size_t>(20, view.options.size() - std::min(top, view.options.size() - 1));
  out.height = static_cast<double>(out.rows) * out.rowHeight;
  out.x = where.x;
  out.y = where.y + where.height;
  // Not enough room under it in the window: over it, if there is room there.
  double scrollY = 0;
  if (const dom::Document* document = select->nodeDocument) {
    const auto found = tree.scroll.find(document);
    if (found != tree.scroll.end()) scrollY = found->second.second;
  }
  if (out.y + out.height > scrollY + tree.viewportHeight && where.y - out.height >= scrollY) out.y = where.y - out.height;
  return true;
}

}  // namespace solar::paint
