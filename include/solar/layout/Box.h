#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "solar/dom/Node.h"
#include "solar/font/Face.h"
#include "solar/layout/Style.h"

// The box tree (https://www.w3.org/TR/CSS22/visuren.html): what the elements and text of a document come to as boxes, and where layout
// puts them. Layout reads the DOM only to build this, and works on the tree after.
namespace solar::layout {

struct Rect {
  double x = 0, y = 0, width = 0, height = 0;
  double Right() const { return x + width; }
  double Bottom() const { return y + height; }
};

struct Edges {
  double top = 0, right = 0, bottom = 0, left = 0;
  double Horizontal() const { return left + right; }
  double Vertical() const { return top + bottom; }
};

struct Box;

// A piece of text on a line, shaped.
struct TextPiece {
  Box* box = nullptr;           // the text box it is of
  size_t begin = 0, end = 0;    // byte range in the box's processed text
  double x = 0, width = 0;      // on the line
  double baselineShift = 0;     // from the line's baseline, up positive
  std::shared_ptr<font::Face> face;
  double fontSize = 16;
  std::vector<font::Glyph> glyphs;  // advances in em, clusters relative to `begin`
};

// Something placed on a line: text, or the edge of an inline box, or an atomic inline.
struct LineItem {
  enum class Kind { Text, Atomic, OpenBox, CloseBox } kind = Kind::Text;
  Box* box = nullptr;
  Rect rect;                     // relative to the block container's border box
  TextPiece text;
};

struct Line {
  Rect rect;           // relative to the block container's content box origin... (see Box::lines)
  double baseline = 0; // from the line's top
  std::vector<LineItem> items;
};

struct Box {
  enum class Kind : uint8_t { Block, Inline, Text, LineBreak };
  Kind kind = Kind::Block;
  bool anonymous = false;
  bool inlineLevel = false;   // takes part in its parent's inline formatting context (inline boxes, text, atomic inlines)
  bool replaced = false;
  dom::Node* node = nullptr;  // null for anonymous boxes
  std::shared_ptr<const BoxStyle> style;
  Box* parent = nullptr;
  std::vector<std::unique_ptr<Box>> children;

  std::string text;           // Text: the characters of the node
  std::string processed;      // Text: after white space and text-transform, as shaped
  std::string pseudo;         // "::before", "::after", "::marker" for generated content

  // Replaced boxes: the natural size, negative where there is none.
  double naturalWidth = -1, naturalHeight = -1;

  // ---- Layout ----
  // The border box, relative to the parent's border box (for an inline box: its first fragment, relative to the block container).
  double x = 0, y = 0, width = 0, height = 0;
  Edges margin, border, padding;
  // Inline boxes: where each piece of it fell, relative to the border box of the block container that holds the lines.
  std::vector<Rect> fragments;
  // Block containers with inline content: the lines, in the container's border box.
  std::vector<Line> lines;
  bool hasInlineContent = false;  // the children are all inline-level
  // Margin collapsing: the margins that go out through the top and bottom of the box as sums of the largest positive and most negative.
  double topPositive = 0, topNegative = 0, bottomPositive = 0, bottomNegative = 0;
  bool collapsedThrough = false;
  // Out of flow boxes: where the box would have been, in the border box of its parent (for position: absolute's auto offsets).
  double staticX = 0, staticY = 0;
  // position: relative shifts.
  double shiftX = 0, shiftY = 0;
  // The scrollable overflow, relative to the border box.
  Rect scrollableOverflow;
  // Baselines of the box (from its top): the last line's, for aligning inline-blocks, and the first's, for flex and grid items; negative if none.
  double baseline = -1;
  double firstBaseline = -1;
  // A flex or grid item is its own formatting context whatever its style.
  bool forceBfc = false;
  // Intrinsic widths, cached.
  double minContent = -1, maxContent = -1;

  Box* AddChild(std::unique_ptr<Box> child) {
    child->parent = this;
    children.push_back(std::move(child));
    return children.back().get();
  }
  bool IsBlockLevel() const { return !inlineLevel; }
  bool IsOutOfFlow() const { return style && (style->IsOutOfFlow() || style->IsFloating()); }
  dom::Element* element() const { return node && node->IsElement() ? static_cast<dom::Element*>(node) : nullptr; }
  double ContentLeft() const { return border.left + padding.left; }
  double ContentTop() const { return border.top + padding.top; }
  double ContentWidth() const { return width - border.Horizontal() - padding.Horizontal(); }
  double ContentHeight() const { return height - border.Vertical() - padding.Vertical(); }
};

// The layout of a document.
struct Tree {
  std::unique_ptr<Box> root;  // the initial containing block, anonymous, whose only child is the root element's box
  std::unordered_map<const dom::Node*, std::vector<Box*>> boxesOf;
  double viewportWidth = 800, viewportHeight = 600;
  uint64_t builtFor = 0;  // the versions of the tree and of style it was made from
  // How far elements (and the viewport, under the document) are scrolled; carried over when the tree is made again.
  std::unordered_map<const dom::Node*, std::pair<double, double>> scroll;
  std::vector<std::unique_ptr<Box>> detached;  // boxes the tree no longer holds, kept so that pointers stay good
};

}  // namespace solar::layout
