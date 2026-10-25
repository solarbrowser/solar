/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// Ruby (https://www.w3.org/TR/css-ruby-1/): a base with annotations over it and under it, in columns as wide as the widest of what is in them, the whole of it
// an atomic inline whose baseline is that of the bases.
#include <algorithm>
#include <limits>

#include "Internal.h"

namespace solar::layout {

namespace {

struct Column {
  std::vector<Box*> members;  // the base first, then the annotations in order
  double width = 0;
};

std::vector<Column> ColumnsOf(Box& ruby) {
  std::vector<Column> columns;
  for (auto& c : ruby.children) {
    if (c->rubyRole == 1 || columns.empty()) columns.emplace_back();
    columns.back().members.push_back(c.get());
  }
  return columns;
}

// The border and the padding of a box, as they are before it is laid out.
void ResolveEdges(Box& b, double cb) {
  const BoxStyle& s = *b.style;
  b.border = {s.border[0], s.border[1], s.border[2], s.border[3]};
  b.padding = {std::max(0.0, s.padding[0].Resolve(cb)), std::max(0.0, s.padding[1].Resolve(cb)), std::max(0.0, s.padding[2].Resolve(cb)), std::max(0.0, s.padding[3].Resolve(cb))};
}

double OuterExtras(const Box& b) {
  const BoxStyle& s = *b.style;
  const auto margin = [&](int side) { return s.margin[side].IsAuto() ? 0.0 : s.margin[side].Resolve(0); };
  return b.border.Horizontal() + b.padding.Horizontal() + margin(1) + margin(3);
}

void SizeColumns(LayoutContext& lc, std::vector<Column>& columns) {
  for (Column& column : columns) {
    column.width = 0;
    for (Box* b : column.members) {
      ResolveEdges(*b, 0);
      ComputeContentSizes(lc, *b);
      column.width = std::max(column.width, b->maxContent + OuterExtras(*b));
    }
  }
}

}  // namespace

void RubyContentSizes(LayoutContext& lc, Box& box, double& minContent, double& maxContent) {
  std::vector<Column> columns = ColumnsOf(box);
  SizeColumns(lc, columns);
  double sum = 0, widest = 0;
  for (const Column& c : columns) {
    sum += c.width;
    widest = std::max(widest, c.width);
  }
  // A line may break between the columns.
  minContent = widest;
  maxContent = sum;
}

void LayoutRuby(LayoutContext& lc, Box& box, double contentWidth, double& contentHeight) {
  (void)contentWidth;
  std::vector<Column> columns = ColumnsOf(box);
  SizeColumns(lc, columns);
  // Each member is laid out at the width of its column; what is over and under is as high as the highest of the columns.
  double maxOver = 0, maxUnder = 0, maxAscent = 0, maxDescent = 0;
  for (Column& column : columns) {
    double over = 0, under = 0;
    for (Box* b : column.members) {
      const BoxStyle& s = *b->style;
      lc.forceWidth = std::max(0.0, column.width - OuterExtras(*b));
      lc.forceHeight = std::numeric_limits<double>::quiet_NaN();
      lc.cbX = 0;
      lc.boxY = 0;
      LayoutBlockLevel(lc, *b, column.width, std::numeric_limits<double>::quiet_NaN(), true);
      const double outer = b->margin.top + b->height + b->margin.bottom;
      (void)s;
      if (b->rubyRole == 2) over += outer;
      else if (b->rubyRole == 3) under += outer;
      else {
        const double baseline = b->baseline >= 0 ? b->baseline : b->height;
        maxAscent = std::max(maxAscent, b->margin.top + baseline);
        maxDescent = std::max(maxDescent, b->height - baseline + b->margin.bottom);
      }
    }
    maxOver = std::max(maxOver, over);
    maxUnder = std::max(maxUnder, under);
  }
  // Place them: the bases on one baseline, the annotations stacked outward from them.
  double x = 0;
  const double left = box.ContentLeft(), top = box.ContentTop();
  for (Column& column : columns) {
    // (The box of the ruby is that of the bases: the annotations are outside it, above and below.)
    double overY = 0, underY = maxAscent + maxDescent;
    for (Box* b : column.members) {
      b->margin.left = OuterExtras(*b) > 0 && !b->style->margin[3].IsAuto() ? b->style->margin[3].Resolve(0) : 0;
      b->margin.right = !b->style->margin[1].IsAuto() ? b->style->margin[1].Resolve(0) : 0;
      b->x = left + x + b->margin.left;
      if (b->rubyRole == 2) {
        const double outer = b->margin.top + b->height + b->margin.bottom;
        overY -= outer;
        b->y = top + overY + b->margin.top;
      } else if (b->rubyRole == 3) {
        b->y = top + underY + b->margin.top;
        underY += b->margin.top + b->height + b->margin.bottom;
      } else {
        const double baseline = b->baseline >= 0 ? b->baseline : b->height;
        b->y = top + (maxAscent - b->margin.top - baseline) + b->margin.top;
      }
    }
    x += column.width;
    // (position: relative moves a member from where it was put.)
    for (Box* b : column.members) {
      b->x += b->shiftX;
      b->y += b->shiftY;
    }
  }
  contentHeight = maxAscent + maxDescent;
  // The annotations take the leading the bases have above and below their text first: only what is more makes the line taller.
  double leadingOver = 0, leadingUnder = 0;
  for (Column& column : columns) {
    const Box* base = column.members[0];
    if (base->lines.empty() || base->lines[0].items.empty()) continue;
    const Line& line = base->lines[0];
    double content = 0;
    for (const LineItem& item : line.items) if (item.kind == LineItem::Kind::Text) content = std::max(content, item.rect.height);
    if (content <= 0) continue;
    const double half = std::max(0.0, (line.rect.height - content) / 2);
    leadingOver = std::max(leadingOver, half);
    leadingUnder = std::max(leadingUnder, half);
    break;
  }
  box.rubyOver = std::max(0.0, maxOver - leadingOver);
  box.rubyUnder = std::max(0.0, maxUnder - leadingUnder);
  box.baseline = top + maxAscent;
  box.firstBaseline = box.baseline;
}

}  // namespace solar::layout
