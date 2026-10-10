// Multiple columns (https://www.w3.org/TR/css-multicol-1/) with the breaking of css-break: the contents of a multicol container are laid out as
// if in one column as wide as the columns are and as tall as they need, and then cut into the columns, each piece taking its place in its column.
#include <algorithm>
#include <cmath>
#include <limits>

#include "Internal.h"

namespace solar::layout {

namespace {

constexpr double kEps = 1e-4;

struct Candidate {
  double fit;    // the bottom of what comes before: the column holding it must be tall enough for this
  double start;  // the top of what comes after: where the next column begins
  bool forced = false;
  bool allowed = true;
};

bool InFlowBlock(const Box& b) {
  return b.kind == Box::Kind::Block && !b.outsideMarker && !b.style->IsOutOfFlow() && !b.style->IsFloating() && !b.inlineLevel;
}

// Where a column may end in the box (tall coordinates: its border box is at (x, y) in the one column).
void Gather(const Box& b, double y, bool avoidInside, std::vector<Candidate>& out) {
  const bool avoid = avoidInside || b.style->breakInside == BreakKind::Avoid;
  if (b.hasInlineContent) {
    const size_t n = b.lines.size();
    for (size_t k = 1; k < n; ++k) {
      Candidate c;
      c.fit = y + b.lines[k - 1].rect.y + b.lines[k - 1].rect.height;
      c.start = y + b.lines[k].rect.y;
      c.allowed = !avoid && static_cast<int>(k) >= b.style->orphans && static_cast<int>(n - k) >= b.style->widows;
      out.push_back(c);
    }
    return;
  }
  const Box* previous = nullptr;
  for (const auto& childPtr : b.children) {
    const Box& child = *childPtr;
    if (!InFlowBlock(child) || child.collapsedThrough) continue;
    const double top = y + child.y;
    if (previous) {
      Candidate c;
      c.fit = y + previous->y + previous->height;
      c.start = top;
      const BreakKind after = previous->style->breakAfter, before = child.style->breakBefore;
      c.forced = after == BreakKind::Column || after == BreakKind::Always || before == BreakKind::Column || before == BreakKind::Always;
      c.allowed = !avoid && after != BreakKind::Avoid && before != BreakKind::Avoid;
      out.push_back(c);
    }
    Gather(child, top, avoid, out);
    previous = &child;
  }
}

// One segment of columns: the starts of its columns in tall coordinates.
struct Fill {
  std::vector<double> starts;
  double tall = 0;
  double used = 0;  // the tallest column
};

Fill Cut(const std::vector<Candidate>& candidates, double tall, double height) {
  Fill fill;
  fill.tall = tall;
  double s = 0;
  fill.starts.push_back(0);
  for (int guard = 0; guard < 10000; ++guard) {
    const Candidate* forced = nullptr;
    const Candidate* best = nullptr;
    const Candidate* first = nullptr;
    for (const Candidate& c : candidates) {
      if (c.start <= s + kEps) continue;
      if (!first && c.allowed) first = &c;
      if (c.fit - s > height + kEps) continue;
      if (c.forced && !forced) forced = &c;
      if (c.allowed || c.forced) best = &c;
    }
    const Candidate* chosen = nullptr;
    if (forced) chosen = forced;
    else if (tall - s <= height + kEps) break;
    else if (best) chosen = best;
    else if (first) chosen = first;  // something that does not fit a column: it overflows
    if (!chosen) break;
    s = chosen->start;
    fill.starts.push_back(s);
  }
  // The tallest column.
  for (size_t i = 0; i < fill.starts.size(); ++i) {
    double end = i + 1 < fill.starts.size() ? tall : tall;
    if (i + 1 < fill.starts.size()) {
      // up to what fits before the next column starts
      end = fill.starts[i + 1];
      for (const Candidate& c : candidates) {
        if (std::fabs(c.start - fill.starts[i + 1]) < kEps) {
          end = c.fit;
          break;
        }
      }
    }
    fill.used = std::max(fill.used, end - fill.starts[i]);
  }
  return fill;
}

struct Geometry {
  int count;
  double width;  // of a column
  double gap;
  bool rtl;
  double Left(size_t column) const {
    const double step = width + gap;
    return rtl ? (static_cast<double>(count) - 1 - static_cast<double>(column)) * step : static_cast<double>(column) * step;
  }
};

struct Placer {
  const Fill& fill;
  const Geometry& geometry;
  double yOffset;  // where the segment is in the container's content box

  size_t ColumnOf(double y) const {
    size_t column = 0;
    for (size_t i = 0; i < fill.starts.size(); ++i) {
      if (fill.starts[i] <= y + kEps) column = i;
    }
    return column;
  }

  // The pieces of a rectangle [top, bottom] x [x, x + width] (tall coordinates) in the columns, in the container's content box.
  std::vector<Rect> Pieces(double x, double top, double width, double bottom) const {
    std::vector<Rect> pieces;
    const size_t first = ColumnOf(top);
    const size_t last = ColumnOf(std::max(top, bottom - 2 * kEps));
    for (size_t i = first; i <= last; ++i) {
      const double segTop = std::max(top, fill.starts[i]);
      const double segBottom = i + 1 < fill.starts.size() ? std::min(bottom, fill.starts[i + 1]) : bottom;
      pieces.push_back({geometry.Left(i) + x, yOffset + segTop - fill.starts[i], width, std::max(0.0, segBottom - segTop)});
    }
    if (pieces.empty()) pieces.push_back({geometry.Left(first) + x, yOffset + top - fill.starts[first], width, std::max(0.0, bottom - top)});
    return pieces;
  }

  static Rect Union(const std::vector<Rect>& rects) {
    double l = rects[0].x, t = rects[0].y, r = rects[0].Right(), b = rects[0].Bottom();
    for (const Rect& rect : rects) {
      l = std::min(l, rect.x);
      t = std::min(t, rect.y);
      r = std::max(r, rect.Right());
      b = std::max(b, rect.Bottom());
    }
    return {l, t, r - l, b - t};
  }

  // The shift a point at `top` (tall) gets going to its column, in the container's content box.
  void Shift(double top, double& dx, double& dy) const {
    const size_t column = ColumnOf(top);
    dx = geometry.Left(column);
    dy = yOffset - fill.starts[column];
  }

  // Moves the box (whose border box is at (x, y) in the tall column) and what is in it to the columns; returns where it came to, in the container's content box.
  Rect Move(Box& b, double x, double y) const {
    const std::vector<Rect> pieces = Pieces(x, y, b.width, y + b.height);
    const Rect box = Union(pieces);
    // Inline content: the lines go to the columns they fall in.
    if (b.hasInlineContent) {
      for (Line& line : b.lines) {
        double dx, dy;
        Shift(y + line.rect.y, dx, dy);
        const double ox = x + dx - box.x, oy = y + dy - box.y;  // (tall, in the box) -> (final, in the box)
        line.rect.x += ox;
        line.rect.y += oy;
        for (LineItem& item : line.items) {
          item.rect.x += ox;
          item.rect.y += oy;
          item.text.x += ox;
          if (item.kind == LineItem::Kind::Atomic && item.box) {
            item.box->x += ox;
            item.box->y += oy;
          }
        }
      }
      MoveInlineFragments(b, x, y, box);
    }
    for (auto& childPtr : b.children) {
      Box& child = *childPtr;
      if (child.kind != Box::Kind::Block || child.outsideMarker) continue;
      if (child.style->IsOutOfFlow() && !child.style->IsFloating()) continue;
      if (child.inlineLevel) continue;  // an atomic inline went with its line
      const Rect moved = Move(child, x + child.x, y + child.y);
      child.x = moved.x - box.x;
      child.y = moved.y - box.y;
    }
    b.fragments.clear();
    if (pieces.size() > 1) {
      for (const Rect& piece : pieces) b.fragments.push_back({piece.x - box.x, piece.y - box.y, piece.width, piece.height});
    }
    b.width = box.width;
    b.height = box.height;
    return box;
  }

  // The inline boxes and text of a container hold their pieces as rectangles in it: each goes with its line.
  void MoveInlineFragments(Box& container, double x, double y, const Rect& box) const {
    std::vector<std::pair<double, double>> lineSpans;
    for (const Line& line : container.lines) lineSpans.push_back({line.rect.y, line.rect.y + line.rect.height});
    (void)lineSpans;
    const auto shiftFragments = [&](auto&& self, Box& b) -> void {
      for (auto& childPtr : b.children) {
        Box& child = *childPtr;
        if (child.kind != Box::Kind::Inline && child.kind != Box::Kind::Text) continue;
        for (Rect& r : child.fragments) {
          double dx, dy;
          Shift(y + r.y + r.height / 2, dx, dy);
          r.x += x + dx - box.x;
          r.y += y + dy - box.y;
        }
        self(self, child);
      }
    };
    shiftFragments(shiftFragments, container);
  }
};

}  // namespace

void LayoutMulticol(LayoutContext& lc, Box& box, double contentWidth, double heightBasis, double& contentHeight) {
  const BoxStyle& s = *box.style;
  const bool rtl = s.direction == Direction::Rtl;
  const double gap = s.columnGap.IsAuto() ? s.fontSize : std::max(0.0, s.columnGap.Resolve(contentWidth));
  Geometry geometry{1, contentWidth, gap, rtl};
  // The used count and width of the columns (css-multicol 3.4).
  const double available = std::max(0.0, contentWidth);
  if (s.columnWidth >= 0 && s.columnCount == 0) {
    geometry.count = std::max(1, static_cast<int>(std::floor((available + gap) / std::max(s.columnWidth + gap, 1e-9))));
  } else if (s.columnWidth >= 0) {
    geometry.count = std::max(1, std::min(s.columnCount, static_cast<int>(std::floor((available + gap) / std::max(s.columnWidth + gap, 1e-9)))));
  } else {
    geometry.count = std::max(1, s.columnCount);
  }
  geometry.width = std::max(0.0, (available - (geometry.count - 1) * gap) / geometry.count);

  const double originX = box.ContentLeft(), originY = box.ContentTop();
  const bool definiteHeight = !std::isnan(heightBasis);
  double y = 0;  // in the content box

  // The children go in segments: a run of them in the columns, then a spanner across them, then a run again.
  struct Segment {
    size_t first, last;
    bool spanner;
  };
  std::vector<Segment> segments;
  if (box.hasInlineContent) {
    segments.push_back({0, box.children.size(), false});
  } else {
    size_t runStart = 0;
    for (size_t i = 0; i < box.children.size(); ++i) {
      const Box& child = *box.children[i];
      if (InFlowBlock(child) && child.style->columnSpanAll) {
        if (i > runStart) segments.push_back({runStart, i, false});
        segments.push_back({i, i + 1, true});
        runStart = i + 1;
      }
    }
    if (runStart < box.children.size() || segments.empty()) segments.push_back({runStart, box.children.size(), false});
  }
  const bool anySpanner = std::any_of(segments.begin(), segments.end(), [](const Segment& seg) { return seg.spanner; });

  for (size_t index = 0; index < segments.size(); ++index) {
    const Segment& seg = segments[index];
    if (seg.spanner) {
      Box& child = *box.children[seg.first];
      Bfc spannerBfc;
      Bfc* saved = lc.bfc;
      lc.bfc = &spannerBfc;
      lc.cbX = 0;
      lc.boxY = y;
      LayoutBlockLevel(lc, child, contentWidth, std::numeric_limits<double>::quiet_NaN());
      lc.bfc = saved;
      const double top = child.margin.top;
      child.x += originX;
      child.y = originY + y + top;
      y += top + child.height + child.margin.bottom;
      continue;
    }
    // Everything of the segment as one column.
    Bfc columnBfc;
    Bfc* saved = lc.bfc;
    lc.bfc = &columnBfc;
    lc.containerX = 0;
    lc.containerY = 0;
    double tall = 0;
    if (box.hasInlineContent) {
      double baseline = -1;
      tall = LayoutInlineContent(lc, box, geometry.width, baseline);
    } else {
      LayoutBlockFlow(lc, box, geometry.width, seg.first, seg.last, tall);
    }
    tall = std::max(tall, columnBfc.lowest);
    lc.bfc = saved;

    std::vector<Candidate> candidates;
    if (box.hasInlineContent) {
      Gather(box, -originY, false, candidates);
    } else {
      // The segment's children are the ones of a box of their own: the breaks between them and inside them.
      const Box* previous = nullptr;
      for (size_t i = seg.first; i < seg.last; ++i) {
        const Box& child = *box.children[i];
        if (!InFlowBlock(child) || child.collapsedThrough) continue;
        const double top = child.y - originY;
        if (previous) {
          Candidate c;
          c.fit = previous->y - originY + previous->height;
          c.start = top;
          const BreakKind after = previous->style->breakAfter, before = child.style->breakBefore;
          c.forced = after == BreakKind::Column || after == BreakKind::Always || before == BreakKind::Column || before == BreakKind::Always;
          c.allowed = after != BreakKind::Avoid && before != BreakKind::Avoid;
          candidates.push_back(c);
        }
        Gather(child, top, false, candidates);
        previous = &child;
      }
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.start < b.start; });

    // The height of the columns: given, or the least that holds the contents in the columns there are.
    Fill fill;
    const bool lastSegment = index + 1 == segments.size();
    double height;
    if (definiteHeight && !anySpanner && lastSegment) {
      height = std::max(0.0, heightBasis - y);
      Fill balanced = Cut(candidates, tall, height);
      if (!s.columnFillAuto && static_cast<int>(balanced.starts.size()) <= geometry.count) {
        // balanced within the height there is
        double lo = tall / geometry.count, hi = height;
        for (int i = 0; i < 40 && hi - lo > 1e-3; ++i) {
          const double mid = (lo + hi) / 2;
          if (static_cast<int>(Cut(candidates, tall, mid).starts.size()) <= geometry.count) hi = mid;
          else lo = mid;
        }
        fill = Cut(candidates, tall, hi);
        // (the container keeps its height; the columns are as tall as they need)
      } else {
        fill = balanced;
      }
      height = heightBasis - y;
    } else if (s.columnFillAuto && !definiteHeight) {
      fill = Cut(candidates, tall, tall + 1);
      height = fill.used;
    } else {
      double lo = tall / geometry.count, hi = std::max(tall, 0.0);
      if (static_cast<int>(Cut(candidates, tall, hi).starts.size()) > geometry.count) {
        fill = Cut(candidates, tall, hi);
      } else {
        for (int i = 0; i < 40 && hi - lo > 1e-3; ++i) {
          const double mid = (lo + hi) / 2;
          if (static_cast<int>(Cut(candidates, tall, mid).starts.size()) <= geometry.count) hi = mid;
          else lo = mid;
        }
        fill = Cut(candidates, tall, hi);
      }
      height = fill.used;
    }
    // Columns that overflow are further along the same row.
    Geometry placing = geometry;
    const Placer placer{fill, placing, y};
    if (box.hasInlineContent) {
      // The container's own lines.
      for (Line& line : box.lines) {
        double dx, dy;
        placer.Shift(line.rect.y - originY, dx, dy);
        const double ox = dx, oy = dy - fill.starts[0] * 0;
        line.rect.x += ox;
        line.rect.y += oy;
        for (LineItem& item : line.items) {
          item.rect.x += ox;
          item.rect.y += oy;
          item.text.x += ox;
          if (item.kind == LineItem::Kind::Atomic && item.box) {
            item.box->x += ox;
            item.box->y += oy;
          }
        }
      }
      const auto shiftFragments = [&](auto&& self, Box& b) -> void {
        for (auto& childPtr : b.children) {
          Box& child = *childPtr;
          if (child.kind != Box::Kind::Inline && child.kind != Box::Kind::Text) continue;
          for (Rect& r : child.fragments) {
            double dx, dy;
            placer.Shift(r.y - originY + r.height / 2, dx, dy);
            r.x += dx;
            r.y += dy;
          }
          self(self, child);
        }
      };
      shiftFragments(shiftFragments, box);
    } else {
      for (size_t i = seg.first; i < seg.last; ++i) {
        Box& child = *box.children[i];
        if (child.kind != Box::Kind::Block || child.outsideMarker) continue;
        if (child.style->IsOutOfFlow() && !child.style->IsFloating()) continue;
        const Rect moved = placer.Move(child, child.x - originX, child.y - originY);
        child.x = originX + moved.x;
        child.y = originY + moved.y;
      }
    }
    y += height;
  }
  contentHeight = y;
}

}  // namespace solar::layout
