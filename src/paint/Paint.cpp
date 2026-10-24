// Painting (https://www.w3.org/TR/CSS22/zindex.html, Appendix E): the boxes of layout in the order they are painted, as a display list.
//
// A stacking context is painted as the standard says: its own background and border, the contexts below it (negative z-index), the backgrounds of the
// block-level descendants, the floats, the inline content (text, and the backgrounds of inline boxes), the positioned descendants and contexts with
// z-index auto or 0, and the contexts above it. What a box does to what is painted after (a clip of its overflow, how far it is scrolled, its opacity
// and transform) is the display list's PushClip / PushLayer / PushTransform, kept in step with the boxes that are being painted.
#include <algorithm>
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
#include "solar/layout/Layout.h"
#include "solar/paint/Paint.h"

namespace solar::paint {

namespace {

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
};

struct Decoration {
  bool underline = false, overline = false, lineThrough = false;
  Color color;
};

class Painter {
 public:
  Painter(Quanta::Context& ctx, Tree& tree) : ctx_(ctx), tree_(tree) {}

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
    // Everything is moved by how far the viewport is scrolled.
    PushTransformOf(-scrollX_, -scrollY_);
    PaintContext(*html);
    EnsureClips({});
    list_->items.push_back(PopTransform{});
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

  void PushTransformOf(double dx, double dy) {
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
    auto found = props_.find(&box);
    if (found != props_.end()) return found->second;
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
    return props_.emplace(&box, props).first->second;
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
    const PaintProps& props = PropsOf(box, borderBox);
    const layout::BoxStyle& physical = box.style->Physical();
    if (&box != canvasFrom_) {  // (the background of the root element, or of the body, is the canvas's)
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
    layout::Edges widths = box.border;
    if (!leftEdge) widths.left = 0;
    if (!rightEdge) widths.right = 0;
    if (widths.top > 0 || widths.right > 0 || widths.bottom > 0 || widths.left > 0) PaintBorder(box, props, physical, borderBox, widths);
    if (leftEdge && rightEdge) PaintImageOf(box, borderBox, props);
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
    if (bytes && !bytes->empty()) {
      auto data = std::make_shared<ImageData>();
      data->bytes = bytes;
      data->url = absolute;
      ref = data;
    }
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
      if (url.empty()) continue;  // (gradients and the like are not yet)
      ImageLayer layer;
      layer.image = ImageFor(url);
      if (!layer.image) continue;
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

  // The picture of an img (and the like): in its content box.
  void PaintImageOf(const Box& box, const RectF& borderBox, const PaintProps& props) {
    if (std::getenv("SOLAR_PAINT_DEBUG") && box.element() && box.element()->IsHtml("img")) std::fprintf(stderr, "paint img box replaced=%d kind=%d url=[%s] size %gx%g\n", box.replaced, (int)box.kind, box.imageUrl.c_str(), box.width, box.height);
    if (!box.replaced || box.kind != Box::Kind::Block || box.imageUrl.empty()) return;
    const ImageRef image = ImageFor(box.imageUrl);
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

  void PaintText(const Box& container, const LineItem& item, const Rect& origin) {
    const Box* box = item.box;
    if (!box || !box->style || box->style->visibility != layout::Visibility::Visible || !item.text.face) return;
    const layout::TextPiece& piece = item.text;
    if (piece.glyphs.empty()) return;
    const double size = piece.fontSize;
    const font::Metrics& metrics = piece.face->metrics();
    const double ascent = std::round(metrics.ascent * size);
    const double x = origin.x + item.rect.x;
    const double baseline = origin.y + item.rect.y + ascent;
    const Color color = ParseColor(box->style->color);
    if (!color.Transparent()) {
      // (Each stretch of glyphs is of its own face: the ids are of that font.)
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
    EnsureClipsFor(owner);
    boxes(owner);
    for (const Line& line : owner.lines) {
      for (const LineItem& item : line.items) {
        if (item.kind == LineItem::Kind::Text) {
          PaintText(owner, item, origin);
        } else if (item.kind == LineItem::Kind::Atomic && item.box) {
          PaintStackingChild(*item.box);
          EnsureClipsFor(owner);
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
    if (scrollPushed_.erase(&box)) list_->items.push_back(PopTransform{});
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
    if (parent.kind == Box::Kind::Block && parent.hasInlineContent && !parent.lines.empty()) lists.inlineOwners.push_back(&parent);
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
    bool layer = false, transformed = false, fixedUndo = false;
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
      transformed = true;
    }
    if (box.style->position == layout::Position::Fixed && (scrollX_ != 0 || scrollY_ != 0) && !transformed) {
      PushTransformOf(scrollX_, scrollY_);  // (a fixed box stays where it is in the viewport)
      fixedUndo = true;
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
    if (fixedUndo) list_->items.push_back(PopTransform{});
    if (transformed) list_->items.push_back(PopTransform{});
    if (layer) list_->items.push_back(PopLayer{});
  }

  void PaintContext(const Box& root) {
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
      inside(*o);
      PaintLines(*o);
    }
    for (const auto& z : lists.zero) PaintStackingChild(*z.second);
    for (const auto& p : lists.positive) PaintStackingChild(*p.second);
  }

  Quanta::Context& ctx_;
  Tree& tree_;
  DisplayList* list_ = nullptr;
  double scrollX_ = 0, scrollY_ = 0;
  const Box* canvasFrom_ = nullptr;
  std::vector<Frame> frames_;
  std::vector<const Box*> clipStack_;
  std::unordered_map<std::string, ImageRef> images_;
  std::unordered_map<const Box*, PaintProps> props_;
  std::unordered_map<const Box*, Decoration> decorations_;
  std::unordered_map<const Box*, std::vector<const Box*>> clipChains_;
  std::set<const Box*> scrollPushed_;
};

}  // namespace

DisplayList BuildDisplayList(Quanta::Context& ctx, layout::Tree& tree) {
  Painter painter(ctx, tree);
  return painter.Run();
}

}  // namespace solar::paint
