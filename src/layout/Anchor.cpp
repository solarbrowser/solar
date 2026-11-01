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
#include "solar/css/Cssom.h"
#include "solar/css/Style.h"

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
    if (&box == &env.element) {
      reached = true;
      return;  // (nor the element's own boxes)
    }
    if (box.kind == Box::Kind::Block && !(box.anonymous && box.pseudo.empty()) && box.style && box.style->display != Display::None && std::find(box.style->anchorNames.begin(), box.style->anchorNames.end(), name) != box.style->anchorNames.end()) {
      // It is one in the containing block of the element, and, when it is positioned itself, one that comes before it in the tree (those are placed first).
      bool inside = holder == env.lc.tree.root.get();
      for (const Box* p = box.parent; p && !inside; p = p->parent) inside = p == holder;
      // (Names are scoped to the tree: an anchor of a shadow tree is not one of the page, nor of another shadow tree.)
      bool sameTree = false;
      dom::Node* anchorNode = box.node;  // (a pseudo-element's box is of its element's tree)
      for (const Box* owner = &box; !anchorNode && owner; owner = owner->parent) anchorNode = owner->node;
      if (anchorNode && env.element.node) {
        dom::Node* anchorTree = anchorNode->Root();
        dom::Node* queryTree = env.element.node->Root();
        // The names of a tree are for the queries of its own and of the trees under it; the part of a shadow tree the outer tree names with ::part is the outer tree's.
        const auto above = [](dom::Node* outer, dom::Node* inner) {
          for (dom::Node* r = inner; r && r != outer;) {
            if (!r->IsFragment() || !static_cast<dom::DocumentFragment*>(r)->isShadowRoot) return false;
            dom::Node* host = static_cast<dom::DocumentFragment*>(r)->host;
            r = host ? host->Root() : nullptr;
            if (r == outer) return true;
          }
          return false;
        };
        sameTree = anchorTree == queryTree || above(anchorTree, queryTree);
        if (!sameTree && box.element() && box.element()->FindAttribute("", "part")) sameTree = above(queryTree, anchorTree);
      }
      // (anchor-scope: a name an element scopes is seen from inside it only, by queries inside it; an element with display: contents has no box, and scopes all the same.)
      bool scoped = true;
      if (anchorNode && env.element.node) {
        const auto scopesOf = [&](const dom::Node* n) {
          std::vector<std::pair<const dom::Node*, std::vector<std::string>>> chain;
          for (const dom::Node* p = n; p; ) {
            if (p->IsElement()) {
              const dom::Element* e = static_cast<const dom::Element*>(p);
              const auto boxes = env.lc.tree.boxesOf.find(p);
              std::shared_ptr<const BoxStyle> style = boxes != env.lc.tree.boxesOf.end() && !boxes->second.empty() ? boxes->second.front()->style : nullptr;
              if (!style) style = ReadStyle(env.lc.ctx, const_cast<dom::Element*>(e));
              if (style && !style->anchorScope.empty()) chain.push_back({p, style->anchorScope});
            }
            const dom::Node* parent = dom::ParentOf(p);
            if (!parent && p->IsFragment() && static_cast<const dom::DocumentFragment*>(p)->isShadowRoot) parent = static_cast<const dom::DocumentFragment*>(p)->host;
            p = parent;
          }
          return chain;
        };
        const auto within = [](const dom::Node* inner, const dom::Node* outer) {
          for (const dom::Node* p = inner; p;) {
            if (p == outer) return true;
            const dom::Node* parent = dom::ParentOf(p);
            if (!parent && p->IsFragment() && static_cast<const dom::DocumentFragment*>(p)->isShadowRoot) parent = static_cast<const dom::DocumentFragment*>(p)->host;
            p = parent;
          }
          return false;
        };
        // (`all` is of the names of the tree it is written in: one that the outer tree gives a part of a shadow tree does not take the names the shadow tree has.)
        const auto covers = [&](const dom::Node* scopeNode, const std::vector<std::string>& names) {
          const bool all = std::find(names.begin(), names.end(), "all") != names.end() && !(scopeNode->IsElement() && static_cast<const dom::Element*>(scopeNode)->FindAttribute("", "part"));
          return all || std::find(names.begin(), names.end(), name) != names.end();
        };
        for (const auto& [scopeNode, names] : scopesOf(anchorNode)) {
          if (covers(scopeNode, names) && !within(env.element.node, scopeNode)) scoped = false;
        }
        for (const auto& [scopeNode, names] : scopesOf(env.element.node)) {
          if (covers(scopeNode, names) && !within(anchorNode, scopeNode)) scoped = false;
        }
      }
      if (inside && sameTree && scoped && (reached == false || !box.style->IsOutOfFlow())) found = &box;
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
    // 0% is the start of the axis in the writing mode of the containing block.
    const Box* holder = ContainingBox(env);
    const int edge = StartEdge(holder->Mode(), holder->style->direction, horizontal);
    position = edge == 0 ? start + along : start + length - along;
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

namespace {

// ---- position-try-fallbacks ----

Align AlignFromText(std::string v) {
  Align a;
  if (v.rfind("safe ", 0) == 0) { a.safe = true; v = v.substr(5); }
  else if (v.rfind("unsafe ", 0) == 0) { a.unsafe = true; v = v.substr(7); }
  static const std::pair<const char*, Align::Kind> table[] = {
      {"auto", Align::Kind::Auto}, {"normal", Align::Kind::Normal}, {"stretch", Align::Kind::Stretch}, {"baseline", Align::Kind::Baseline}, {"start", Align::Kind::Start},
      {"end", Align::Kind::End}, {"flex-start", Align::Kind::FlexStart}, {"flex-end", Align::Kind::FlexEnd}, {"self-start", Align::Kind::SelfStart}, {"self-end", Align::Kind::SelfEnd},
      {"center", Align::Kind::Center}, {"left", Align::Kind::Left}, {"right", Align::Kind::Right}, {"anchor-center", Align::Kind::AnchorCenter}};
  a.kind = Align::Kind::Normal;
  for (const auto& [name, kind] : table) {
    if (v == name) a.kind = kind;
  }
  return a;
}

// anchor() inside a text with the sides it names turned by `swap`: left and right (horizontal), top and bottom, and a percentage as the rest of 100%.
std::string FlipAnchors(const std::string& text, bool horizontal, bool vertical, bool transpose) {
  std::string out;
  size_t i = 0;
  while (i < text.size()) {
    const bool afterWord = i > 0 && (std::isalnum(static_cast<unsigned char>(text[i - 1])) || text[i - 1] == '-');
    if (transpose && !afterWord && text.compare(i, 12, "anchor-size(") == 0) {
      // Across the diagonal the width of the anchor is its height, and the inline size its block size.
      size_t closeSize = 0;
      std::vector<std::string> sizeParts;
      if (FunctionArguments(text, i + 11, closeSize, sizeParts)) {
        std::string head;
        for (const std::string& word : Words(sizeParts[0])) {
          std::string w = word;
          const std::string l = Lower(word);
          if (l == "width") w = "height"; else if (l == "height") w = "width";
          else if (l == "block") w = "inline"; else if (l == "inline") w = "block";
          else if (l == "self-block") w = "self-inline"; else if (l == "self-inline") w = "self-block";
          head += (head.empty() ? "" : " ") + w;
        }
        out += "anchor-size(" + head;
        if (sizeParts.size() > 1) out += ", " + FlipAnchors(sizeParts[1], horizontal, vertical, transpose);
        out += ")";
        i = closeSize + 1;
        continue;
      }
    }
    if (text.compare(i, 7, "anchor(") != 0 || afterWord) {
      out += text[i++];
      continue;
    }
    size_t close = 0;
    std::vector<std::string> parts;
    if (!FunctionArguments(text, i + 6, close, parts)) {
      out += text[i++];
      continue;
    }
    std::string head;
    for (const std::string& word : Words(parts[0])) {
      std::string w = word;
      const std::string l = Lower(word);
      if (l == "left" || l == "right") w = transpose ? (l == "left" ? "top" : "bottom") : (horizontal ? (l == "left" ? "right" : "left") : l);
      else if (l == "top" || l == "bottom") w = transpose ? (l == "top" ? "left" : "right") : (vertical ? (l == "top" ? "bottom" : "top") : l);
      else if ((l == "start" || l == "end" || l == "self-start" || l == "self-end") && (horizontal || vertical) && !transpose) w = l.find("start") != std::string::npos ? (l.rfind("self-", 0) == 0 ? "self-end" : "end") : (l.rfind("self-", 0) == 0 ? "self-start" : "start");
      else if ((l.rfind("calc(", 0) == 0 || l.rfind("min(", 0) == 0 || l.rfind("max(", 0) == 0) && l.find('%') != std::string::npos && l.find("px") == std::string::npos && (horizontal || vertical) && !transpose) {
        // A calculation of percentages: the rest of 100%.
        double total = 0;
        if (EvaluateCalc(l, 100, total)) {
          char buffer[32];
          std::snprintf(buffer, sizeof buffer, "%g%%", 100 - total);
          w = buffer;
        }
      } else if (!l.empty() && l.back() == '%' && (horizontal || vertical) && !transpose) {
        char* end = nullptr;
        const double p = std::strtod(l.c_str(), &end);
        if (end && *end == '%') {
          char buffer[32];
          std::snprintf(buffer, sizeof buffer, "%g%%", 100 - p);
          w = buffer;
        }
      }
      head += (head.empty() ? "" : " ") + w;
    }
    out += "anchor(" + head;
    if (parts.size() > 1) out += ", " + FlipAnchors(parts[1], horizontal, vertical, transpose);
    out += ")";
    i = close + 1;
  }
  return out;
}

void SwapSides(BoxStyle& st, int a, int b, bool flipHorizontalText, bool flipVerticalText, bool transpose) {
  std::swap(st.inset[a], st.inset[b]);
  std::swap(st.margin[a], st.margin[b]);
  for (int side : {a, b}) {
    if (st.inset[side].IsAnchor()) st.inset[side].calc = FlipAnchors(st.inset[side].calc, flipHorizontalText, flipVerticalText, transpose);
  }
}

Align::Kind FlippedAlignment(Align::Kind k) {
  switch (k) {
    case Align::Kind::Start: return Align::Kind::End;
    case Align::Kind::End: return Align::Kind::Start;
    case Align::Kind::SelfStart: return Align::Kind::SelfEnd;
    case Align::Kind::SelfEnd: return Align::Kind::SelfStart;
    case Align::Kind::FlexStart: return Align::Kind::FlexEnd;
    case Align::Kind::FlexEnd: return Align::Kind::FlexStart;
    case Align::Kind::Left: return Align::Kind::Right;
    case Align::Kind::Right: return Align::Kind::Left;
    default: return k;
  }
}

// The words of a position-area turned: along an axis (left and right, top and bottom, and the logical ones), or across the diagonal.
std::string FlipAreaWords(const std::string& area, int mode /* 0 horizontal, 1 vertical, 2 block, 3 inline, 4 transpose */) {
  std::vector<std::string> words = Words(area);
  const auto swapIn = [](std::string w, const char* a, const char* b) {
    const size_t pa = w.find(a), pb = w.find(b);
    if (pa != std::string::npos && (pb == std::string::npos || pa < pb)) w.replace(pa, std::string(a).size(), b);
    else if (pb != std::string::npos) w.replace(pb, std::string(b).size(), a);
    return w;
  };
  for (std::string& w : words) {
    const std::string l = Lower(w);
    if (l == "none" || l == "center" || l == "span-all") continue;
    const bool isBlock = l.find("block") != std::string::npos, isInline = l.find("inline") != std::string::npos;
    const bool physicalH = l.find("left") != std::string::npos || l.find("right") != std::string::npos || l.find("x-") != std::string::npos;
    const bool physicalV = l.find("top") != std::string::npos || l.find("bottom") != std::string::npos || l.find("y-") != std::string::npos;
    if (mode == 4) {
      if (physicalH || physicalV) {
        std::string r = l;
        const auto replaceAll = [&](const char* a, const char* b) { for (size_t p = r.find(a); p != std::string::npos; p = r.find(a, p + std::string(b).size())) r.replace(p, std::string(a).size(), b); };
        if (physicalH) { replaceAll("left", "@top"); replaceAll("right", "@bottom"); replaceAll("x-start", "@y-start"); replaceAll("x-end", "@y-end"); }
        else { replaceAll("top", "@left"); replaceAll("bottom", "@right"); replaceAll("y-start", "@x-start"); replaceAll("y-end", "@x-end"); }
        r.erase(std::remove(r.begin(), r.end(), '@'), r.end());
        w = r;
      } else if (isBlock) w = l.substr(0, l.find("block")) + "inline" + l.substr(l.find("block") + 5);
      else if (isInline) w = l.substr(0, l.find("inline")) + "block" + l.substr(l.find("inline") + 6);
      continue;
    }
    const bool applies = (mode == 0 && physicalH) || (mode == 1 && physicalV) || (mode == 2 && isBlock) || (mode == 3 && isInline) || (!physicalH && !physicalV && !isBlock && !isInline);
    if (!applies) continue;
    std::string r = l;
    if (physicalH && mode == 0) { r = swapIn(r, "left", "right"); r = swapIn(r, "x-start", "x-end"); }
    else if (physicalV && mode == 1) { r = swapIn(r, "top", "bottom"); r = swapIn(r, "y-start", "y-end"); }
    else if ((isBlock && mode == 2) || (isInline && mode == 3)) r = swapIn(r, "-start", "-end");
    else r = swapIn(r, "start", "end");  // an ambiguous start or end: of the axis the tactic turns, which is the position of the word
    w = r;
  }
  std::string joined;
  for (const std::string& w : words) joined += (joined.empty() ? "" : " ") + w;
  return joined;
}

// The rules @position-try of the tree the element is in and of the document, by name: the last of a name.
const css::CssRule* FindTryRule(const Env& env, const std::string& name) {
  const css::CssRule* found = nullptr;
  const std::function<void(const std::vector<css::CssRule*>&)> walk = [&](const std::vector<css::CssRule*>& rules) {
    for (const css::CssRule* rule : rules) {
      switch (rule->kind) {
        case css::RuleKind::PositionTry:
          if (rule->name == name) found = rule;
          break;
        case css::RuleKind::Media: case css::RuleKind::Supports: case css::RuleKind::LayerBlock:
          walk(rule->rules);
          break;
        case css::RuleKind::Import:
          if (rule->importedSheet) walk(rule->importedSheet->rules);
          break;
        default: break;
      }
    }
  };
  dom::Node* node = env.element.node;
  if (!node) return nullptr;
  dom::Node* root = node->Root();
  for (const css::CssStyleSheet* sheet : css::SheetsOfTreeRoot(root)) if (!sheet->disabled) walk(sheet->rules);
  if (!found && node->nodeDocument && node->nodeDocument != root) {
    for (const css::CssStyleSheet* sheet : css::SheetsOfTreeRoot(node->nodeDocument)) if (!sheet->disabled) walk(sheet->rules);
  }
  return found;
}

void ApplyRuleDeclarations(Env& env, BoxStyle& st, const css::CssRule& rule) {
  if (!rule.style) return;
  const WritingMode mode = env.element.Mode();
  const bool rtl = env.element.style->direction == Direction::Rtl;
  const bool vertical = IsVertical(mode);
  for (const css::DeclarationEntry& e : rule.style->items) {
    const std::string& n = e.name;
    const std::string& v = e.value;
    if (v.empty() || v == "revert" || v == "revert-layer") continue;
    // initial and unset are the property's initial value (none of these properties is inherited); inherit is not told apart here.
    const bool reset = v == "initial" || v == "unset";
    if (v == "inherit") continue;
    int side = -1;
    bool inset = false;
    if (n == "top") { side = 0; inset = true; } else if (n == "right") { side = 1; inset = true; } else if (n == "bottom") { side = 2; inset = true; } else if (n == "left") { side = 3; inset = true; }
    else if (n == "inset-block-start") { side = PhysicalSideOf(mode, 0); inset = true; } else if (n == "inset-block-end") { side = PhysicalSideOf(mode, 2); inset = true; }
    else if (n == "inset-inline-start") { side = PhysicalSideOf(mode, rtl ? 1 : 3); inset = true; } else if (n == "inset-inline-end") { side = PhysicalSideOf(mode, rtl ? 3 : 1); inset = true; }
    else if (n == "margin-top") side = 0; else if (n == "margin-right") side = 1; else if (n == "margin-bottom") side = 2; else if (n == "margin-left") side = 3;
    else if (n == "margin-block-start") side = PhysicalSideOf(mode, 0); else if (n == "margin-block-end") side = PhysicalSideOf(mode, 2);
    else if (n == "margin-inline-start") side = PhysicalSideOf(mode, rtl ? 1 : 3); else if (n == "margin-inline-end") side = PhysicalSideOf(mode, rtl ? 3 : 1);
    if (side >= 0) {
      (inset ? st.inset[side] : st.margin[side]) = reset ? (inset ? Length() : Length::Px(0)) : ParseLengthText(v);
      continue;
    }
    const auto sized = [&](bool none, const char* blockName, const char* inlineName, Length& physicalWidth, Length& physicalHeight, const char* w, const char* h) {
      const Length value = reset ? (none ? Length(Length::Kind::None) : Length()) : ParseLengthText(v);
      if (n == w) physicalWidth = value;
      else if (n == h) physicalHeight = value;
      else if (n == blockName) (vertical ? physicalWidth : physicalHeight) = value;
      else if (n == inlineName) (vertical ? physicalHeight : physicalWidth) = value;
    };
    sized(false, "block-size", "inline-size", st.width, st.height, "width", "height");
    sized(false, "min-block-size", "min-inline-size", st.minWidth, st.minHeight, "min-width", "min-height");
    sized(true, "max-block-size", "max-inline-size", st.maxWidth, st.maxHeight, "max-width", "max-height");
    if (n == "justify-self") st.justifySelf = reset ? Align() : AlignFromText(v);
    else if (n == "align-self") st.alignSelf = reset ? Align() : AlignFromText(v);
    else if (n == "position-anchor") st.positionAnchor = reset ? "normal" : v;
    else if (n == "position-area") st.positionArea = reset ? "none" : v;
  }
}

void ApplyTactic(Env& env, BoxStyle& st, const std::string& tactic) {
  const Box* holder = ContainingBox(env);
  const WritingMode mode = holder->Mode();
  const Direction direction = holder->style->direction;
  const bool horizontalBlock = IsVertical(mode);  // the block axis is the horizontal one
  const int bs = PhysicalSideOf(mode, 0), be = PhysicalSideOf(mode, 2);
  const int ls = PhysicalSideOf(mode, 3), le = PhysicalSideOf(mode, 1);
  const int is = direction == Direction::Rtl ? le : ls, ie = direction == Direction::Rtl ? ls : le;
  const bool cbHorizontal = !IsVertical(mode);
  Align& horizontalSelf = cbHorizontal ? st.justifySelf : st.alignSelf;
  Align& verticalSelf = cbHorizontal ? st.alignSelf : st.justifySelf;
  const auto flipAxis = [&](bool horizontal) {
    if (horizontal) SwapSides(st, 1, 3, true, false, false);
    else SwapSides(st, 0, 2, false, true, false);
    Align& self = horizontal ? horizontalSelf : verticalSelf;
    self.kind = FlippedAlignment(self.kind);
    if (st.positionArea != "none") st.positionArea = FlipAreaWords(st.positionArea, horizontal ? 0 : 1);
  };
  if (tactic == "flip-x") flipAxis(true);
  else if (tactic == "flip-y") flipAxis(false);
  else if (tactic == "flip-block") flipAxis(horizontalBlock);
  else if (tactic == "flip-inline") flipAxis(!horizontalBlock);
  else if (tactic == "flip-start") {
    // The start of the block axis and of the inline axis change places (the diagonal), and with them the width and the height.
    SwapSides(st, bs, is, false, false, true);
    SwapSides(st, be, ie, false, false, true);
    std::swap(st.width, st.height);
    std::swap(st.minWidth, st.minHeight);
    std::swap(st.maxWidth, st.maxHeight);
    for (Length* l : {&st.width, &st.height, &st.minWidth, &st.minHeight, &st.maxWidth, &st.maxHeight}) {
      if (l->IsAnchor()) l->calc = FlipAnchors(l->calc, false, false, true);
    }
    // (left and right are start and end of the lines, by the direction.)
    for (Align* self : {&st.justifySelf, &st.alignSelf}) {
      if (self->kind == Align::Kind::Left) self->kind = direction == Direction::Rtl ? Align::Kind::End : Align::Kind::Start;
      else if (self->kind == Align::Kind::Right) self->kind = direction == Direction::Rtl ? Align::Kind::Start : Align::Kind::End;
    }
    std::swap(st.justifySelf, st.alignSelf);
    if (st.positionArea != "none") st.positionArea = FlipAreaWords(st.positionArea, 4);
  }
}

}  // namespace

std::vector<PositionTryOption> PositionTryOptions(const BoxStyle& style) {
  std::vector<PositionTryOption> options;
  const std::string& text = style.Physical().positionTryFallbacks;
  if (text.empty() || text == "none") return options;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t comma = text.find(',', start);
    const std::string part = Trim(text.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
    PositionTryOption option;
    bool areaWords = false;
    for (const std::string& word : Words(part)) {
      if (word.rfind("--", 0) == 0) option.rule = word;
      else if (word.rfind("flip-", 0) == 0) option.tactics.push_back(Lower(word));
      else areaWords = true;
    }
    if (areaWords) option.area = part;
    options.push_back(option);
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return options;
}

namespace {

// ---- position-area ----

struct Regions {
  int hs = 0, he = 2, vs = 0, ve = 2;  // the first and last of the three regions on each axis
};

// One word of a position-area: which axis it is on and the regions it stands for.
struct AreaWord {
  enum class Axis { Horizontal, Vertical, Block, Inline, Ambiguous, Neutral } axis = Axis::Neutral;
  bool center = false, all = false, span = false, self = false, end = false;
  int physical = -1;  // left (0) or right (2), top (0) or bottom (2), for the physical words
};

std::optional<AreaWord> ReadAreaWord(const std::string& word) {
  AreaWord w;
  if (word == "center") { w.center = true; return w; }
  if (word == "span-all") { w.all = true; return w; }
  std::string base = word;
  if (base.rfind("span-", 0) == 0) { w.span = true; base = base.substr(5); }
  if (base.rfind("self-", 0) == 0) { w.self = true; base = base.substr(5); }
  if (base == "left") { w.axis = AreaWord::Axis::Horizontal; w.physical = 0; }
  else if (base == "right") { w.axis = AreaWord::Axis::Horizontal; w.physical = 2; }
  else if (base == "top") { w.axis = AreaWord::Axis::Vertical; w.physical = 0; }
  else if (base == "bottom") { w.axis = AreaWord::Axis::Vertical; w.physical = 2; }
  else if (base == "x-start" || base == "x-end") { w.axis = AreaWord::Axis::Horizontal; w.end = base == "x-end"; }
  else if (base == "y-start" || base == "y-end") { w.axis = AreaWord::Axis::Vertical; w.end = base == "y-end"; }
  else if (base == "block-start" || base == "block-end") { w.axis = AreaWord::Axis::Block; w.end = base == "block-end"; }
  else if (base == "inline-start" || base == "inline-end") { w.axis = AreaWord::Axis::Inline; w.end = base == "inline-end"; }
  else if (base == "start" || base == "end") { w.axis = AreaWord::Axis::Ambiguous; w.end = base == "end"; }
  else return std::nullopt;
  return w;
}

// The regions of an axis a word takes: [first, last].
void RegionsOf(Env& env, const AreaWord& w, bool horizontalAxis, int& first, int& last) {
  if (w.center) { first = last = 1; return; }
  if (w.all) { first = 0; last = 2; return; }
  int edge = w.physical;  // 0 the first region of the axis, 2 the last
  if (edge < 0) {
    const Box* holder = ContainingBox(env);
    const WritingMode mode = w.self ? env.element.Mode() : holder->Mode();
    const Direction direction = w.self ? env.element.style->direction : holder->style->direction;
    edge = (StartEdge(mode, direction, horizontalAxis) ^ (w.end ? 1 : 0)) ? 2 : 0;
  }
  if (w.span) {
    first = edge == 0 ? 0 : 1;
    last = edge == 0 ? 1 : 2;
  } else {
    first = last = edge;
  }
}

std::optional<Regions> ReadArea(Env& env, const std::string& text) {
  std::vector<AreaWord> words;
  for (const std::string& piece : Words(text)) {
    const std::optional<AreaWord> w = ReadAreaWord(Lower(piece));
    if (!w) return std::nullopt;
    words.push_back(*w);
  }
  if (words.empty() || words.size() > 2) return std::nullopt;
  const auto axisOf = [&](const AreaWord& w, int position) -> int {  // 1 horizontal, 0 vertical, -1 neutral
    switch (w.axis) {
      case AreaWord::Axis::Horizontal: return 1;
      case AreaWord::Axis::Vertical: return 0;
      case AreaWord::Axis::Block: case AreaWord::Axis::Inline: case AreaWord::Axis::Ambiguous: {
        const Box* holder = ContainingBox(env);
        const WritingMode mode = w.self ? env.element.Mode() : holder->Mode();
        bool block = w.axis == AreaWord::Axis::Block;
        if (w.axis == AreaWord::Axis::Ambiguous) block = position == 0;  // the first is the block axis's, the second the inline axis's
        return block == IsVertical(mode) ? 1 : 0;
      }
      default: return -1;
    }
  };
  Regions r;
  if (words.size() == 1) {
    const AreaWord& w = words[0];
    if (w.axis == AreaWord::Axis::Neutral) {
      if (w.center) r.hs = r.he = r.vs = r.ve = 1;
      return r;
    }
    if (w.axis == AreaWord::Axis::Ambiguous) {
      // A single start or end is the same on both axes.
      for (int a = 0; a < 2; ++a) {
        const int axis = axisOf(w, a);
        int first, last;
        RegionsOf(env, w, axis == 1, first, last);
        (axis == 1 ? r.hs : r.vs) = first;
        (axis == 1 ? r.he : r.ve) = last;
      }
      return r;
    }
    const int axis = axisOf(w, 0);
    int first, last;
    RegionsOf(env, w, axis == 1, first, last);
    (axis == 1 ? r.hs : r.vs) = first;
    (axis == 1 ? r.he : r.ve) = last;
    return r;
  }
  int axis0 = axisOf(words[0], 0), axis1 = axisOf(words[1], 1);
  if (axis0 < 0 && axis1 < 0) { axis0 = IsVertical(ContainingBox(env)->Mode()) ? 1 : 0; axis1 = 1 - axis0; }  // the first is of the block axis, the second of the inline axis
  else if (axis0 < 0) axis0 = 1 - axis1;
  else if (axis1 < 0) axis1 = 1 - axis0;
  if (axis0 == axis1) return std::nullopt;
  const int axes[2] = {axis0, axis1};
  for (int i = 0; i < 2; ++i) {
    int first, last;
    RegionsOf(env, words[i], axes[i] == 1, first, last);
    (axes[i] == 1 ? r.hs : r.vs) = first;
    (axes[i] == 1 ? r.he : r.ve) = last;
  }
  return r;
}

// The alignment `normal` comes to along an axis, for the regions the area takes there.
Align::Kind AreaAlignment(int first, int last, bool horizontalAxis) {
  if (first == 0 && last == 2) return Align::Kind::AnchorCenter;
  if (first == 1 && last == 1) return Align::Kind::Center;
  if (last <= 1) return horizontalAxis ? Align::Kind::PhysRight : Align::Kind::PhysBottom;
  return horizontalAxis ? Align::Kind::PhysLeft : Align::Kind::PhysTop;
}

}  // namespace

bool NeedsAnchors(const BoxStyle& style) {
  const BoxStyle& physical = style.Physical();
  for (const Length& l : physical.inset) if (l.IsAnchor()) return true;
  for (const Length& l : physical.margin) if (l.IsAnchor()) return true;
  if (physical.positionArea != "none" || physical.positionTryFallbacks != "none") return true;
  if (physical.justifySelf.kind == Align::Kind::AnchorCenter || physical.alignSelf.kind == Align::Kind::AnchorCenter) return true;
  return physical.width.IsAnchor() || physical.height.IsAnchor() || physical.minWidth.IsAnchor() || physical.minHeight.IsAnchor() || physical.maxWidth.IsAnchor() || physical.maxHeight.IsAnchor();
}

AnchorResult ResolveAnchorStyle(LayoutContext& lc, const Box& element, double cbX, double cbY, double cbWidth, double cbHeight, const PositionTryOption* option) {
  AnchorResult result;
  Env env{lc, element, cbX, cbY, cbWidth, cbHeight};
  auto physical = std::make_shared<BoxStyle>(element.style->Physical());
  if (option) {
    if (!option->rule.empty()) {
      if (const css::CssRule* rule = FindTryRule(env, option->rule)) ApplyRuleDeclarations(env, *physical, *rule);
    }
    if (!option->area.empty()) physical->positionArea = option->area;
    for (const std::string& tactic : option->tactics) ApplyTactic(env, *physical, tactic);
  }
  // The default anchor, and the area around it.
  const Box* anchor = AnchorNamed(env, "");
  if (anchor) {
    const Rect r = AbsoluteBorderBox(*anchor);
    result.anchor = true;
    result.anchorX = r.x;
    result.anchorY = r.y;
    result.anchorWidth = r.width;
    result.anchorHeight = r.height;
    if (physical->positionArea != "none") {
      if (const std::optional<Regions> regions = ReadArea(env, physical->positionArea)) {
        // The lines between the regions: the edges of the containing block and of the anchor, the block taken as large as to hold the anchor.
        const double ax0 = r.x - cbX, ax1 = r.x + r.width - cbX, ay0 = r.y - cbY, ay1 = r.y + r.height - cbY;
        const double hx[4] = {std::min(0.0, ax0), ax0, ax1, std::max(cbWidth, ax1)};
        const double vy[4] = {std::min(0.0, ay0), ay0, ay1, std::max(cbHeight, ay1)};
        result.area = true;
        result.areaX = cbX + hx[regions->hs];
        result.areaWidth = hx[regions->he + 1] - hx[regions->hs];
        result.areaY = cbY + vy[regions->vs];
        result.areaHeight = vy[regions->ve + 1] - vy[regions->vs];
        // An inset that is auto is 0 in it, and the alignment that is normal is toward the anchor, or on it.
        for (Length& inset : physical->inset) {
          if (inset.IsAuto()) inset = Length::Px(0);
        }
        const Box* holder = ContainingBox(env);
        const bool cbHorizontal = !IsVertical(holder->Mode());
        Align& horizontalSelf = cbHorizontal ? physical->justifySelf : physical->alignSelf;
        Align& verticalSelf = cbHorizontal ? physical->alignSelf : physical->justifySelf;
        const auto isNormal = [](const Align& a) { return a.kind == Align::Kind::Normal || a.kind == Align::Kind::Auto; };
        if (isNormal(horizontalSelf)) horizontalSelf.kind = AreaAlignment(regions->hs, regions->he, true);
        if (isNormal(verticalSelf)) verticalSelf.kind = AreaAlignment(regions->vs, regions->ve, false);
      }
    }
  }
  // What is measured from the anchor is measured in the area, when there is one.
  const double basisW = result.area ? result.areaWidth : cbWidth, basisH = result.area ? result.areaHeight : cbHeight;
  if (result.area) {
    env.cx = result.areaX;
    env.cy = result.areaY;
    env.cw = result.areaWidth;
    env.ch = result.areaHeight;
  }
  for (int side = 0; side < 4; ++side) Resolve(env, physical->inset[side], side, side % 2, side % 2 == 0 ? basisH : basisW, false);
  for (int side = 0; side < 4; ++side) Resolve(env, physical->margin[side], -1, side % 2, basisW, false);
  Resolve(env, physical->width, -1, 1, basisW, false);
  Resolve(env, physical->height, -1, 0, basisH, false);
  Resolve(env, physical->minWidth, -1, 1, basisW, false);
  Resolve(env, physical->minHeight, -1, 0, basisH, false);
  Resolve(env, physical->maxWidth, -1, 1, basisW, true);
  Resolve(env, physical->maxHeight, -1, 0, basisH, true);
  // Centered on the anchor, an inset that is auto is 0: the room is the containing block's.
  if (anchor) {
    const bool cbHorizontal = !IsVertical(ContainingBox(env)->Mode());
    const bool horizontalCenter = (cbHorizontal ? physical->justifySelf : physical->alignSelf).kind == Align::Kind::AnchorCenter;
    const bool verticalCenter = (cbHorizontal ? physical->alignSelf : physical->justifySelf).kind == Align::Kind::AnchorCenter;
    const bool horizontalArea = result.area, verticalArea = result.area;
    if (horizontalCenter || horizontalArea) {
      for (int side : {1, 3}) {
        if (physical->inset[side].IsAuto()) physical->inset[side] = Length::Px(0);
        if (physical->margin[side].IsAuto()) physical->margin[side] = Length::Px(0);
      }
    }
    if (verticalCenter || verticalArea) {
      for (int side : {0, 2}) {
        if (physical->inset[side].IsAuto()) physical->inset[side] = Length::Px(0);
        if (physical->margin[side].IsAuto()) physical->margin[side] = Length::Px(0);
      }
    }
  }
  if (!NeedsAnchors(*element.style)) return result;
  result.style = Logicalize(physical);
  return result;
}

}  // namespace solar::layout
