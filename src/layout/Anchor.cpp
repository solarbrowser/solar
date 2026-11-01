/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// Anchor positioning (https://drafts.csswg.org/css-anchor-position-1/): the anchor() and anchor-size() functions of the insets, sizes and margins of an
// absolutely positioned box, worked out against the box that has the anchor-name they name.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>
#include <functional>
#include <vector>

#include "Internal.h"

namespace solar::layout {

namespace {

struct Env {
  LayoutContext& lc;
  const Box& element;
  double cx, cy, cw, ch;  // the containing block's padding box, in the page
};

std::string Trim(const std::string& text) {
  const size_t b = text.find_first_not_of(" \t\n");
  if (b == std::string::npos) return "";
  return text.substr(b, text.find_last_not_of(" \t\n") - b + 1);
}

std::string Lower(std::string text) {
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

// The boxes that have a name of an anchor, before the element in the order of the tree.
const Box* ContainingBox(const Env& env);

const Box* FindAnchor(Env& env, const std::string& name) {
  const Box* holder = ContainingBox(env);
  const Box* found = nullptr;
  bool reached = false;
  const std::function<void(const Box&)> visit = [&](const Box& box) {
    if (reached) return;
    if (&box == &env.element) {
      reached = true;
      return;
    }
    if (box.kind == Box::Kind::Block && box.style && box.style->display != Display::None && std::find(box.style->anchorNames.begin(), box.style->anchorNames.end(), name) != box.style->anchorNames.end()) {
      // (An element is not its own anchor, nor a box in it: those come after it in the tree.) It is one that is in the containing block of the element.
      bool inside = holder == env.lc.tree.root.get();
      for (const Box* p = box.parent; p && !inside; p = p->parent) inside = p == holder;
      if (inside) found = &box;
    }
    for (const auto& child : box.children) visit(*child);
  };
  visit(*env.lc.tree.root);
  return found;
}

// Which edge of an axis its start is, in a writing mode: 0 the left or the top, 1 the right or the bottom.
int StartEdge(WritingMode mode, Direction direction, bool horizontalAxis) {
  const bool vertical = IsVertical(mode);
  if (horizontalAxis) {
    if (!vertical) return direction == Direction::Rtl ? 1 : 0;  // the inline axis
    return mode == WritingMode::VerticalRl || mode == WritingMode::SidewaysRl ? 1 : 0;  // the block axis
  }
  if (!vertical) return 0;  // the block axis
  if (mode == WritingMode::SidewaysLr) return direction == Direction::Rtl ? 0 : 1;  // the inline axis, upward
  return direction == Direction::Rtl ? 1 : 0;
}

const Box* ContainingBox(const Env& env) {
  for (const Box* p = env.element.parent; p; p = p->parent) {
    if (p->parent == nullptr) break;
    if (p->kind == Box::Kind::Block && (p->style->position != Position::Static || p->style->containsPositioned)) return p;
  }
  return env.lc.tree.root.get();
}

// The anchor a query names, or the default anchor of the element.
const Box* AnchorNamed(Env& env, const std::string& name) {
  std::string wanted = name;
  if (wanted.empty()) {
    const std::string& given = env.element.style->positionAnchor;
    if (given.rfind("--", 0) != 0) return nullptr;
    wanted = given;
  }
  return FindAnchor(env, wanted);
}

// The arguments of the function written at `text[open]` (the parenthesis), split at its top-level commas; `close` is where the parenthesis ends.
bool FunctionArguments(const std::string& text, size_t open, size_t& close, std::vector<std::string>& parts) {
  int depth = 0;
  std::string current;
  for (size_t i = open; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '(') {
      if (depth++ == 0) continue;
    } else if (c == ')') {
      if (--depth == 0) {
        parts.push_back(Trim(current));
        close = i;
        return true;
      }
    } else if (c == ',' && depth == 1) {
      parts.push_back(Trim(current));
      current.clear();
      continue;
    }
    current += c;
  }
  return false;
}

std::vector<std::string> Words(const std::string& text) {
  std::vector<std::string> words;
  std::string current;
  int depth = 0;
  for (char c : text) {
    if (c == '(') ++depth;
    if (c == ')') --depth;
    if ((c == ' ' || c == '\t') && depth == 0) {
      if (!current.empty()) words.push_back(current);
      current.clear();
    } else {
      current += c;
    }
  }
  if (!current.empty()) words.push_back(current);
  return words;
}

std::optional<std::string> Substitute(Env& env, const std::string& text, int side, int axis, double basis);

// anchor(<name>? <side>, fallback?) as the px it comes to.
std::optional<double> AnchorFunction(Env& env, const std::vector<std::string>& parts, int side) {
  if (side < 0 || parts.empty()) return std::nullopt;
  std::string name, keyword;
  for (const std::string& word : Words(parts[0])) {
    if (word.rfind("--", 0) == 0) name = word;
    else keyword = Lower(word);
  }
  if (keyword.empty()) return std::nullopt;
  const Box* anchor = AnchorNamed(env, name);
  if (!anchor) return std::nullopt;
  const bool horizontal = side == 1 || side == 3;
  const bool farSide = side == 1 || side == 2;  // right, bottom
  if ((keyword == "left" || keyword == "right") && !horizontal) return std::nullopt;
  if ((keyword == "top" || keyword == "bottom") && horizontal) return std::nullopt;
  const Rect r = AbsoluteBorderBox(*anchor);
  const double start = horizontal ? r.x - env.cx : r.y - env.cy;
  const double length = horizontal ? r.width : r.height;
  const double cbSize = horizontal ? env.cw : env.ch;
  double position;
  if (keyword == "left" || keyword == "top") position = start;
  else if (keyword == "right" || keyword == "bottom") position = start + length;
  else if (keyword == "center") position = start + length / 2;
  else if (keyword == "inside") position = farSide ? start + length : start;
  else if (keyword == "outside") position = farSide ? start : start + length;
  else if (keyword == "start" || keyword == "end" || keyword == "self-start" || keyword == "self-end") {
    const bool self = keyword.rfind("self-", 0) == 0;
    const bool end = keyword.size() >= 3 && keyword.compare(keyword.size() - 3, 3, "end") == 0;
    const Box* holder = ContainingBox(env);
    const WritingMode mode = self ? env.element.Mode() : holder->Mode();
    const Direction direction = self ? env.element.style->direction : holder->style->direction;
    const int edge = StartEdge(mode, direction, horizontal) ^ (end ? 1 : 0);
    position = edge == 0 ? start : start + length;
  } else {
    // A percentage (or a calculation with one) of the way from the left or the top to the right or the bottom.
    const std::optional<std::string> amount = Substitute(env, keyword, side, horizontal ? 1 : 0, length);
    if (!amount) return std::nullopt;
    const Length given = ParseLengthText(*amount);
    const double along = given.Resolve(length, std::nan(""));
    if (std::isnan(along)) return std::nullopt;
    position = start + along;
  }
  return farSide ? cbSize - position : position;
}

// anchor-size(<name>? <size>, fallback?) in px.
std::optional<double> AnchorSizeFunction(Env& env, const std::vector<std::string>& parts, int axisHorizontal) {
  std::string name, keyword;
  if (!parts.empty()) {
    for (const std::string& word : Words(parts[0])) {
      if (word.rfind("--", 0) == 0) name = word;
      else keyword = Lower(word);
    }
  }
  const Box* anchor = AnchorNamed(env, name);
  if (!anchor) return std::nullopt;
  const Rect r = AbsoluteBorderBox(*anchor);
  bool width;
  if (keyword.empty()) {
    if (axisHorizontal < 0) return std::nullopt;
    width = axisHorizontal == 1;  // the size of the property's own axis
  } else if (keyword == "width") width = true;
  else if (keyword == "height") width = false;
  else {
    const bool self = keyword.rfind("self-", 0) == 0;
    const bool block = keyword.find("block") != std::string::npos;
    const WritingMode mode = self ? env.element.Mode() : ContainingBox(env)->Mode();
    const bool inlineIsWidth = !IsVertical(mode);
    width = block ? !inlineIsWidth : inlineIsWidth;
  }
  return width ? r.width : r.height;
}

// The text with every anchor() and anchor-size() in it as the length it is, or nothing if one has no value and no fallback. `side` is the inset it is the value of (0 top, 1 right,
// 2 bottom, 3 left), or -1 for what is not an inset; `basis` what a percentage is of.
std::optional<std::string> Substitute(Env& env, const std::string& text, int side, int axis, double basis) {
  std::string out;
  size_t i = 0;
  while (i < text.size()) {
    const bool boundary = i == 0 || !(std::isalnum(static_cast<unsigned char>(text[i - 1])) || text[i - 1] == '-' || text[i - 1] == '_');
    const bool isSize = boundary && text.compare(i, 12, "anchor-size(") == 0;
    const bool isAnchor = boundary && !isSize && text.compare(i, 7, "anchor(") == 0;
    if (!isSize && !isAnchor) {
      out += text[i++];
      continue;
    }
    const size_t open = i + (isSize ? 11 : 6);
    size_t close = 0;
    std::vector<std::string> parts;
    if (!FunctionArguments(text, open, close, parts)) return std::nullopt;
    std::optional<double> value = isSize ? AnchorSizeFunction(env, parts, axis) : AnchorFunction(env, parts, side);
    std::string piece;
    if (value) {
      char buffer[64];
      std::snprintf(buffer, sizeof buffer, "%.6gpx", *value);
      piece = buffer;
    } else if (parts.size() >= 2) {
      const std::optional<std::string> fallback = Substitute(env, parts[1], side, axis, basis);
      if (!fallback) return std::nullopt;
      piece = *fallback;
    } else {
      return std::nullopt;
    }
    out += "(" + piece + ")";
    i = close + 1;
  }
  // A lone value needs no parentheses.
  const std::string trimmed = Trim(out);
  if (trimmed.size() > 2 && trimmed.front() == '(' && trimmed.back() == ')' && trimmed.find('(', 1) == std::string::npos) return trimmed.substr(1, trimmed.size() - 2);
  return trimmed;
}

// What an anchor value comes to: a length, or auto where it has no value.
void Resolve(Env& env, Length& value, int side, int axis, double basis, bool noneWhenInvalid) {
  if (!value.IsAnchor()) return;
  const std::optional<std::string> text = Substitute(env, value.calc, side, axis, basis);
  Length result;
  if (!text) {
    if (noneWhenInvalid) result.kind = Length::Kind::None;
    value = result;
    return;
  }
  result = ParseLengthText(*text);
  if (result.kind == Length::Kind::Anchor || result.IsAuto()) {
    // (A calculation the length reader cannot work out: with the percentage of the basis, here.)
    double v = 0;
    if (EvaluateCalc(*text, basis, v)) result = Length::Px(v);
    else result = Length();
  }
  value = result;
}

}  // namespace

bool NeedsAnchors(const BoxStyle& style) {
  const BoxStyle& physical = style.Physical();
  for (const Length& l : physical.inset) if (l.IsAnchor()) return true;
  for (const Length& l : physical.margin) if (l.IsAnchor()) return true;
  return physical.width.IsAnchor() || physical.height.IsAnchor() || physical.minWidth.IsAnchor() || physical.minHeight.IsAnchor() || physical.maxWidth.IsAnchor() || physical.maxHeight.IsAnchor();
}

std::shared_ptr<const BoxStyle> ResolveAnchorStyle(LayoutContext& lc, const Box& element, double cbX, double cbY, double cbWidth, double cbHeight) {
  if (!NeedsAnchors(*element.style)) return nullptr;
  Env env{lc, element, cbX, cbY, cbWidth, cbHeight};
  auto physical = std::make_shared<BoxStyle>(element.style->Physical());
  for (int side = 0; side < 4; ++side) Resolve(env, physical->inset[side], side, side % 2, side % 2 == 0 ? cbHeight : cbWidth, false);
  for (int side = 0; side < 4; ++side) Resolve(env, physical->margin[side], -1, side % 2, cbWidth, false);
  Resolve(env, physical->width, -1, 1, cbWidth, false);
  Resolve(env, physical->height, -1, 0, cbHeight, false);
  Resolve(env, physical->minWidth, -1, 1, cbWidth, false);
  Resolve(env, physical->minHeight, -1, 0, cbHeight, false);
  Resolve(env, physical->maxWidth, -1, 1, cbWidth, true);
  Resolve(env, physical->maxHeight, -1, 0, cbHeight, true);
  return Logicalize(physical);
}

}  // namespace solar::layout
