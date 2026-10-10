// Block layout (https://www.w3.org/TR/CSS22/visudet.html, https://www.w3.org/TR/CSS22/box.html#collapsing-margins), floats
// (https://www.w3.org/TR/CSS22/visuren.html#floats) and absolute positioning (https://www.w3.org/TR/CSS22/visuren.html#absolute-positioning).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>

#include "Internal.h"

namespace solar::layout {

void ShiftContents(Box& box, double dy) {
  for (auto& c : box.children) {
    if (c->kind == Box::Kind::Block && c->style->IsOutOfFlow() && !c->style->IsFloating()) continue;
    c->y += dy;
  }
  for (Line& line : box.lines) {
    line.rect.y += dy;
    for (LineItem& item : line.items) {
      item.rect.y += dy;
      if (item.kind == LineItem::Kind::Atomic && item.box) {}
    }
  }
  for (auto& c : box.children) {
    if (c->kind == Box::Kind::Inline || c->kind == Box::Kind::Text) {
      for (Rect& r : c->fragments) r.y += dy;
      // inline boxes' children too
      std::function<void(Box&)> deep = [&](Box& b) {
        for (auto& g : b.children) {
          if (g->kind == Box::Kind::Inline || g->kind == Box::Kind::Text) for (Rect& r : g->fragments) r.y += dy;
          if (g->kind == Box::Kind::Inline) deep(*g);
        }
      };
      deep(*c);
    }
  }
}


namespace {

// Physical directions of the axes of a writing mode: (1,0) is rightwards, (0,1) downwards.
struct DirP {
  int x, y;
};
DirP FrameXP(WritingMode m) { return m == WritingMode::HorizontalTb ? DirP{1, 0} : m == WritingMode::SidewaysLr ? DirP{0, -1} : DirP{0, 1}; }
DirP InlineStartDirP(WritingMode m, Direction d) {
  const DirP a = FrameXP(m);
  return d == Direction::Ltr ? a : DirP{-a.x, -a.y};
}
int DotP(DirP a, DirP b) { return a.x * b.x + a.y * b.y; }

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kEps = 0.0005;

// Margins that meet: the largest positive and the most negative; they collapse to their sum.
struct MarginSet {
  double positive = 0, negative = 0;
  void Add(double m) {
    if (m > 0) positive = std::max(positive, m);
    else negative = std::min(negative, m);
  }
  void Add(const MarginSet& o) {
    positive = std::max(positive, o.positive);
    negative = std::min(negative, o.negative);
  }
  double Sum() const { return positive + negative; }
};

bool Known(double v) { return !std::isnan(v); }

// A length for a width, or NaN if it is not a number of px (auto, an intrinsic keyword, a percentage of nothing).
double ResolveSize(const Length& l, double basis) {
  switch (l.kind) {
    case Length::Kind::Px: return l.value;
    case Length::Kind::Percent: return Known(basis) ? basis * l.value / 100 : kNaN;
    case Length::Kind::Calc: {
      if (!Known(basis)) return kNaN;
      double v = 0;
      return EvaluateCalc(l.calc, basis, v) ? v : kNaN;
    }
    default: return kNaN;
  }
}

void ResolveEdges(Box& box, double cbWidth) {
  const BoxStyle& s = *box.style;
  box.border = {s.border[0], s.border[1], s.border[2], s.border[3]};
  box.padding = {std::max(0.0, s.padding[0].Resolve(cbWidth)), std::max(0.0, s.padding[1].Resolve(cbWidth)), std::max(0.0, s.padding[2].Resolve(cbWidth)),
                 std::max(0.0, s.padding[3].Resolve(cbWidth))};
}

// The margin on a side, 0 when it is auto (the caller handles auto where it matters).
double MarginOf(const BoxStyle& s, int side, double cbWidth) { return s.margin[side].IsAuto() ? 0 : s.margin[side].Resolve(cbWidth); }

double ExtrasHorizontal(const Box& b) { return b.border.Horizontal() + b.padding.Horizontal(); }
double ExtrasVertical(const Box& b) { return b.border.Vertical() + b.padding.Vertical(); }

bool IsRootBox(const Box& box) { return box.parent && box.parent->parent == nullptr; }

// The direction of the box a box is in: what start and end mean for its margins, floats and clearance.
Direction CbDirection(const Box& b) {
  const Box* p = b.parent;
  while (p && p->kind != Box::Kind::Block) p = p->parent;
  return p ? p->style->direction : Direction::Ltr;
}

bool IsFloat(const Box& b) { return b.style->IsFloating() && !b.style->IsOutOfFlow(); }
bool IsPositionedOutOfFlow(const Box& b) { return b.style->IsOutOfFlow(); }

bool FloatsLeft(const BoxStyle& s, Direction cb) {
  const bool rtl = cb == Direction::Rtl;
  return s.floating == Float::Left || (s.floating == Float::InlineStart && !rtl) || (s.floating == Float::InlineEnd && rtl);
}

}  // namespace

// ---- Floats ----

void Bfc::Band(double y, double height, double& left, double& right) const {
  left = -std::numeric_limits<double>::infinity();
  right = std::numeric_limits<double>::infinity();
  for (const FloatRecord& f : floats) {
    const bool overlaps = height > 0 ? (f.top < y + height - kEps && f.bottom > y + kEps) : (f.top <= y + kEps && f.bottom > y + kEps);
    if (!overlaps || f.bottom <= f.top + kEps) continue;
    if (f.isLeft) left = std::max(left, f.right);
    else right = std::min(right, f.left);
  }
}

double Bfc::NextEdge(double y, double height) const {
  double next = std::numeric_limits<double>::infinity();
  for (const FloatRecord& f : floats) {
    const bool overlaps = height > 0 ? (f.top < y + height - kEps && f.bottom > y + kEps) : (f.top <= y + kEps && f.bottom > y + kEps);
    if (overlaps && f.bottom > f.top + kEps) next = std::min(next, f.bottom);
  }
  return std::isinf(next) ? y : next;
}

double Bfc::ClearY(bool left, bool right) const {
  double y = -std::numeric_limits<double>::infinity();
  for (const FloatRecord& f : floats) {
    if ((f.isLeft && left) || (!f.isLeft && right)) y = std::max(y, f.bottom);
  }
  return y;
}

FloatRecord PlaceFloat(LayoutContext& lc, Box& box, double cbLeft, double cbWidth, double minY) {
  ++lc.floatEvents;
  const double savedBoxY = lc.boxY, savedCbX = lc.cbX;
  lc.boxY = minY;
  lc.cbX = cbLeft;
  LayoutBlockLevel(lc, box, cbWidth, kNaN, true);
  lc.boxY = savedBoxY;
  lc.cbX = savedCbX;
  const double mw = box.margin.left + box.width + box.margin.right;
  const double mh = box.margin.top + box.height + box.margin.bottom;
  const bool isLeft = FloatsLeft(*box.style, CbDirection(box));
  double y = minY;
  for (const FloatRecord& f : lc.bfc->floats) y = std::max(y, f.top);
  double bandLeft = cbLeft, bandRight = cbLeft + cbWidth;
  for (int tries = 0; tries < 1000; ++tries) {
    double l, r;
    lc.bfc->Band(y, mh, l, r);
    bandLeft = std::max(cbLeft, l);
    bandRight = std::min(cbLeft + cbWidth, r);
    const bool intruded = bandLeft > cbLeft + kEps || bandRight < cbLeft + cbWidth - kEps;
    if (mw <= bandRight - bandLeft + kEps || !intruded) break;
    const double next = lc.bfc->NextEdge(y, mh);
    if (next <= y + kEps) break;
    y = next;
  }
  FloatRecord record;
  record.box = &box;
  record.isLeft = isLeft;
  record.top = y;
  record.bottom = y + mh;
  if (isLeft) {
    record.left = bandLeft;
    record.right = bandLeft + mw;
  } else {
    record.right = bandRight;
    record.left = bandRight - mw;
  }
  lc.bfc->floats.push_back(record);
  lc.bfc->lowest = std::max(lc.bfc->lowest, record.bottom);
  return record;
}


// ---- Replaced boxes ----

void ReplacedContentSize(const Box& box, double cbWidth, double cbHeight, double& w, double& h) {
  const BoxStyle& s = *box.style;
  const bool borderBox = s.boxSizing == BoxSizing::BorderBox;
  const double extrasH = ExtrasHorizontal(box), extrasV = ExtrasVertical(box);
  double cssW = ResolveSize(s.width, cbWidth), cssH = ResolveSize(s.height, cbHeight);
  if (Known(cssW) && borderBox) cssW = std::max(0.0, cssW - extrasH);
  if (Known(cssH) && borderBox) cssH = std::max(0.0, cssH - extrasV);
  if (!Known(cssW) && s.width.IsAuto() && box.attrWidth >= 0) cssW = box.attrWidth;
  if (!Known(cssH) && s.height.IsAuto() && box.attrHeight >= 0) cssH = box.attrHeight;
  double ratio = box.naturalRatio;
  if (ratio <= 0 && box.naturalWidth > 0 && box.naturalHeight > 0) ratio = box.naturalWidth / box.naturalHeight;
  if (ratio <= 0 && box.attrWidth > 0 && box.attrHeight > 0) ratio = box.attrWidth / box.attrHeight;
  if (s.aspectRatio > 0 && !s.aspectRatioAuto) ratio = s.aspectRatio;
  const bool widthAuto = !Known(cssW), heightAuto = !Known(cssH);
  w = cssW;
  h = cssH;
  if (widthAuto && heightAuto) {
    if (box.naturalWidth >= 0 && box.naturalHeight >= 0) { w = box.naturalWidth; h = box.naturalHeight; }
    else if (box.naturalWidth >= 0) { w = box.naturalWidth; h = ratio > 0 ? w / ratio : 150; }
    else if (box.naturalHeight >= 0) { h = box.naturalHeight; w = ratio > 0 ? h * ratio : 300; }
    else if (ratio > 0) { w = Known(cbWidth) ? std::min(cbWidth, 300.0) : 300; h = w / ratio; }
    else { w = 300; h = 150; }
    if (s.aspectRatio > 0 && !s.aspectRatioAuto && box.naturalWidth >= 0) h = w / ratio;
  } else if (widthAuto) {
    w = ratio > 0 ? h * ratio : (box.naturalWidth >= 0 ? box.naturalWidth : 300);
  } else if (heightAuto) {
    h = ratio > 0 ? w / ratio : (box.naturalHeight >= 0 ? box.naturalHeight : 150);
  }
  // min and max keep the ratio of what was automatic.
  const auto content = [&](const Length& l, double basis) {
    double v = ResolveSize(l, basis);
    if (Known(v) && borderBox) v = std::max(0.0, v - extrasH);
    return v;
  };
  const auto contentV = [&](const Length& l, double basis) {
    double v = ResolveSize(l, basis);
    if (Known(v) && borderBox) v = std::max(0.0, v - extrasV);
    return v;
  };
  const double maxW = content(s.maxWidth, cbWidth), minW = content(s.minWidth, cbWidth);
  const double maxH = contentV(s.maxHeight, cbHeight), minH = contentV(s.minHeight, cbHeight);
  for (int pass = 0; pass < 2; ++pass) {
    if (Known(maxW) && w > maxW) { w = maxW; if (heightAuto && ratio > 0) h = w / ratio; }
    if (Known(minW) && w < minW) { w = minW; if (heightAuto && ratio > 0) h = w / ratio; }
    if (Known(maxH) && h > maxH) { h = maxH; if (widthAuto && ratio > 0) w = h * ratio; }
    if (Known(minH) && h < minH) { h = minH; if (widthAuto && ratio > 0) w = h * ratio; }
  }
}

void ComputeContentSizes(LayoutContext& lc, Box& box) {
  if (box.minContent >= 0) return;
  box.minContent = box.maxContent = 0;
  const BoxStyle& s = *box.style;
  if (box.replaced) {
    ResolveEdges(box, 0);
    double w, h;
    ReplacedContentSize(box, kNaN, kNaN, w, h);
    box.minContent = box.maxContent = w;
    return;
  }
  if (s.containSizeInline && !IsGridDisplay(s.display)) {  // its contents count for nothing
    box.minContent = box.maxContent = s.containIntrinsicWidthSet ? s.containIntrinsicWidth.value : 0;
    return;
  }
  if (IsFlexDisplay(s.display)) {
    FlexContentSizes(lc, box, box.minContent, box.maxContent);
    return;
  }
  if (IsGridDisplay(s.display)) {
    GridContentSizes(lc, box, box.minContent, box.maxContent);
    return;
  }
  if (IsTableDisplay(s.display) && !box.replaced) {
    TableContentSizes(lc, box, box.minContent, box.maxContent);
    return;
  }
  if (box.hasInlineContent) {
    InlineContentSizes(lc, box, box.minContent, box.maxContent);
    return;
  }
  for (auto& child : box.children) {
    if (child->style->IsOutOfFlow() || child->outsideMarker) continue;
    ComputeContentSizes(lc, *child);
    const BoxStyle& cs = *child->style;
    ResolveEdges(*child, 0);
    double extras = ExtrasHorizontal(*child) + MarginOf(cs, 1, 0) + MarginOf(cs, 3, 0);
    double mn = child->minContent, mx = child->maxContent;
    const double fixed = ResolveSize(cs.width, kNaN);
    if (Known(fixed)) {
      const double w = cs.boxSizing == BoxSizing::BorderBox ? std::max(0.0, fixed - ExtrasHorizontal(*child)) : fixed;
      mn = mx = w;
    } else if (cs.width.kind == Length::Kind::Percent || cs.width.kind == Length::Kind::Calc) {
      mn = 0;  // a percentage of an unknown width contributes nothing to the minimum
    }
    const double maxW = ResolveSize(cs.maxWidth, kNaN);
    if (Known(maxW)) { mn = std::min(mn, maxW); mx = std::min(mx, maxW); }
    const double minW = ResolveSize(cs.minWidth, kNaN);
    if (Known(minW)) { mn = std::max(mn, minW); mx = std::max(mx, minW); }
    box.minContent = std::max(box.minContent, mn + extras);
    box.maxContent = std::max(box.maxContent, mx + extras);
  }
}

namespace {

double QuickMarginTop(const Box& child, double cbWidth) { return MarginOf(*child.style, 0, cbWidth); }

// The block children of a box, placed one under another with margins collapsing; floats are placed beside them.
// (px, py) is the box's content origin in the coordinates of the block formatting context.
void LayoutBlockChildren(LayoutContext& lc, Box& P, double contentWidth, double heightBasis, bool topOpen, bool bottomOpen, double px, double py, double& contentHeight) {
  double y = 0;
  MarginSet pending;
  bool atTop = true;
  MarginSet escapedTop;
  bool placedAny = false;
  double lastBaseline = -1;
  const double originX = P.ContentLeft(), originY = P.ContentTop();
  for (auto& childPtr : P.children) {
    Box& child = *childPtr;
    if (child.outsideMarker) continue;
    if (IsPositionedOutOfFlow(child)) {
      child.staticX = originX;
      child.staticY = originY + y + (atTop && topOpen ? 0 : pending.Sum());
      continue;
    }
    if (IsFloat(child)) {
      const FloatRecord rec = PlaceFloat(lc, child, px, contentWidth, py + y + pending.Sum());
      child.x = rec.left - px + child.margin.left + originX;
      child.y = rec.top - py + child.margin.top + originY;
      // A float is not in the flow, but a relative offset moves it.
      continue;
    }
    const BoxStyle& cs = *child.style;
    // Clearance: the child's top border edge goes below the floats it clears.
    double estimateTop = (atTop && topOpen) ? py : py + y + pending.Sum() + QuickMarginTop(child, contentWidth);
    bool cleared = false;
    if (cs.clear != Clear::None && !lc.bfc->floats.empty()) {
      const bool rtl = P.style->direction == Direction::Rtl;
      const bool left = cs.clear == Clear::Left || cs.clear == Clear::Both || (cs.clear == Clear::InlineStart && !rtl) || (cs.clear == Clear::InlineEnd && rtl);
      const bool right = cs.clear == Clear::Right || cs.clear == Clear::Both || (cs.clear == Clear::InlineStart && rtl) || (cs.clear == Clear::InlineEnd && !rtl);
      const double clearY = lc.bfc->ClearY(left, right);
      ++lc.floatEvents;
      if (clearY > estimateTop + kEps) {
        y = clearY - py;
        pending = MarginSet();
        estimateTop = clearY;
        cleared = true;
        atTop = false;
      }
    }
    // A box that is its own formatting context does not go under floats: beside them if it fits, else below.
    double availOverride = kNaN, shiftX = 0;
    const bool establishes = cs.CreatesBlockFormattingContext();
    if (establishes && !lc.bfc->floats.empty()) {
      for (int tries = 0; tries < 200; ++tries) {
        double l, r;
        lc.bfc->Band(estimateTop, 1, l, r);
        ++lc.floatEvents;
        const double bandLeft = std::max(px, l), bandRight = std::min(px + contentWidth, r);
        if (bandLeft <= px + kEps && bandRight >= px + contentWidth - kEps) break;
        const double room = bandRight - bandLeft;
        // A fixed width that does not fit goes below the next float's bottom.
        const double want = ResolveSize(cs.width, contentWidth);
        if (Known(want) && want + MarginOf(cs, 1, contentWidth) + MarginOf(cs, 3, contentWidth) > room + kEps) {
          const double next = lc.bfc->NextEdge(estimateTop, 1);
          if (next <= estimateTop + kEps) break;
          y += next - estimateTop;
          estimateTop = next;
          continue;
        }
        availOverride = room;
        shiftX = bandLeft - px;
        break;
      }
    }
    // Lay out at the estimated place; if the real place is another and floats mattered, lay out again there.
    const size_t floatCount = lc.bfc->floats.size();
    const uint64_t events = lc.floatEvents;
    // justify-self other than normal fits the box to its content and puts it where it says (unless a margin is auto).
    const Align::Kind justify = cs.justifySelf.kind;
    bool justified = justify != Align::Kind::Auto && justify != Align::Kind::Normal && justify != Align::Kind::Stretch && !cs.margin[1].IsAuto() && !cs.margin[3].IsAuto() &&
                     !Orthogonal(P.Mode(), child.Mode()) && !child.replaced && !establishes;
    if (Orthogonal(P.Mode(), child.Mode())) justified = justify != Align::Kind::Auto && justify != Align::Kind::Normal && justify != Align::Kind::Stretch && !cs.margin[1].IsAuto() && !cs.margin[3].IsAuto();
    if (justified && !Orthogonal(P.Mode(), child.Mode()) && child.replaced) justified = true;
    lc.cbX = px + shiftX;
    lc.boxY = estimateTop;
    lc.availOverride = availOverride;
    LayoutBlockLevel(lc, child, contentWidth, heightBasis, justified);
    MarginSet top{child.topPositive, child.topNegative};
    MarginSet bottom{child.bottomPositive, child.bottomNegative};
    const bool throughNow = child.collapsedThrough;
    double actualTop;
    if (throughNow) {
      actualTop = py + y + pending.Sum();
    } else if (cleared) {
      actualTop = py + y;
    } else {
      MarginSet withTop = pending;
      withTop.Add(top);
      actualTop = (atTop && topOpen) ? py : py + y + withTop.Sum();
    }
    if (std::fabs(actualTop - estimateTop) > kEps && lc.floatEvents != events) {
      lc.bfc->floats.resize(floatCount);
      lc.bfc->lowest = 0;
      for (const FloatRecord& f : lc.bfc->floats) lc.bfc->lowest = std::max(lc.bfc->lowest, f.bottom);
      lc.cbX = px + shiftX;
      lc.boxY = actualTop;
      lc.availOverride = availOverride;
      LayoutBlockLevel(lc, child, contentWidth, heightBasis, justified);
      top = {child.topPositive, child.topNegative};
      bottom = {child.bottomPositive, child.bottomNegative};
    }
    const double relativeX = child.shiftX, relativeY = child.shiftY;
    if (child.collapsedThrough) {
      pending.Add(top);
      pending.Add(bottom);
      child.x += originX + shiftX + relativeX;
      child.y = originY + y + pending.Sum() + relativeY;
      continue;
    }
    if (cleared) {
      child.y = originY + y + relativeY;
      y += child.height;
    } else {
      pending.Add(top);
      if (atTop && topOpen) {
        escapedTop = pending;
        child.y = originY + relativeY;
        y = child.height;
      } else {
        y += pending.Sum();
        child.y = originY + y + relativeY;
        y += child.height;
      }
    }
    atTop = false;
    placedAny = true;
    pending = bottom;
    if (child.baseline >= 0) lastBaseline = child.y - relativeY - originY + child.baseline;
    child.x += originX + shiftX + relativeX;
    if (justified) {
      // Aligned in the room the containing block has along its lines.
      const double free = contentWidth - shiftX - (child.margin.left + child.width + child.margin.right);
      const WritingMode pm = P.Mode();
      const DirP startToEnd = [&] {
        switch (justify) {
          case Align::Kind::SelfStart: case Align::Kind::SelfEnd: return InlineStartDirP(child.Mode(), cs.direction);
          case Align::Kind::Left: case Align::Kind::Right: return FrameXP(pm);
          default: return InlineStartDirP(pm, P.style->direction);
        }
      }();
      const bool wantStart = justify == Align::Kind::Start || justify == Align::Kind::SelfStart || justify == Align::Kind::Left || justify == Align::Kind::FlexStart ||
                             justify == Align::Kind::Baseline;
      const bool center = justify == Align::Kind::Center;
      const int d = DotP(startToEnd, FrameXP(pm));
      const bool low = d == 0 ? true : ((d > 0) == wantStart);
      double offset = 0;
      if (free < 0 && !cs.justifySelf.unsafe) {
        // does not fit: from its start
        offset = (DotP(InlineStartDirP(pm, P.style->direction), FrameXP(pm)) > 0) ? 0 : free;
      } else if (center) {
        offset = free / 2;
      } else if (!low) {
        offset = free;
      }
      child.x += offset;
    }
  }
  P.baseline = lastBaseline >= 0 ? lastBaseline + originY : -1;
  if (!placedAny && topOpen) {
    // Everything pending touches the box's top: it is the margin of the box's own, and of its bottom if that is open too.
    escapedTop = pending;
    if (bottomOpen) {
      P.bottomPositive = std::max(P.bottomPositive, pending.positive);
      P.bottomNegative = std::min(P.bottomNegative, pending.negative);
    }
    contentHeight = 0;
  } else if (bottomOpen) {
    P.bottomPositive = std::max(P.bottomPositive, pending.positive);
    P.bottomNegative = std::min(P.bottomNegative, pending.negative);
    contentHeight = placedAny ? y : 0;
  } else {
    contentHeight = y + pending.Sum();
  }
  // The margins that escaped the top join the box's own.
  P.topPositive = std::max(P.topPositive, escapedTop.positive);
  P.topNegative = std::min(P.topNegative, escapedTop.negative);
}

}  // namespace

void LayoutBlockLevel(LayoutContext& lc, Box& box, double cbWidth, double cbHeight, bool shrinkToFit) {
  const BoxStyle& s = *box.style;
  double forceWidth = lc.forceWidth, forceHeight = lc.forceHeight, availOverride = lc.availOverride;
  lc.forceWidth = lc.forceHeight = lc.availOverride = kNaN;
  const bool keepOwnFrame = lc.keepOwnFrame;
  lc.keepOwnFrame = false;
  // A box in another writing mode than the one it is in is laid out in its own: what it is given is turned round.
  if (!keepOwnFrame) {
    const Box* frameBox = box.parent;
    while (frameBox && frameBox->kind != Box::Kind::Block) frameBox = frameBox->parent;
    const WritingMode parentMode = frameBox ? frameBox->Mode() : WritingMode::HorizontalTb;
    if (Orthogonal(parentMode, s.writingMode)) {
      const double inlineSize = Known(cbHeight) ? cbHeight : (IsVertical(parentMode) ? lc.viewportWidth : lc.viewportHeight);
      cbHeight = cbWidth;
      cbWidth = inlineSize;
      std::swap(forceWidth, forceHeight);
      availOverride = kNaN;
    }
  }
  Bfc* const savedBfc = lc.bfc;
  const double savedCbX = lc.cbX, savedBoxY = lc.boxY;
  const double savedContainerX = lc.containerX, savedContainerY = lc.containerY;

  ResolveEdges(box, cbWidth);
  const double extrasH = ExtrasHorizontal(box);
  const double extrasV = ExtrasVertical(box);
  const bool borderBox = s.boxSizing == BoxSizing::BorderBox;
  const bool autoLeft = s.margin[3].IsAuto(), autoRight = s.margin[1].IsAuto();
  double ml = MarginOf(s, 3, cbWidth), mr = MarginOf(s, 1, cbWidth);
  box.margin.top = MarginOf(s, 0, cbWidth);
  box.margin.bottom = MarginOf(s, 2, cbWidth);
  box.minContent = box.maxContent = -1;
  const double availableWidth = Known(availOverride) ? availOverride : cbWidth;

  // ---- width ----
  const auto contentFromSpecified = [&](double specified) { return borderBox ? std::max(0.0, specified - extrasH) : std::max(0.0, specified); };
  double width = kNaN;
  bool widthAuto = true;
  if (Known(forceWidth)) {
    width = forceWidth;
    widthAuto = false;
  } else if (box.replaced) {
    double rw, rh;
    ReplacedContentSize(box, cbWidth, cbHeight, rw, rh);
    width = rw;
    widthAuto = false;
  } else if (s.width.kind == Length::Kind::MinContent || s.width.kind == Length::Kind::MaxContent || s.width.kind == Length::Kind::FitContent) {
    ComputeContentSizes(lc, box);
    if (s.width.kind == Length::Kind::MinContent) width = box.minContent;
    else if (s.width.kind == Length::Kind::MaxContent) width = box.maxContent;
    else width = std::min(std::max(box.minContent, availableWidth - ml - mr - extrasH), box.maxContent);
    widthAuto = false;
  } else {
    const double specified = ResolveSize(s.width, cbWidth);
    if (Known(specified)) {
      width = contentFromSpecified(specified);
      widthAuto = false;
    }
  }
  const auto clampWidth = [&](double w) {
    if (Known(forceWidth)) return w;
    const double maxW = ResolveSize(s.maxWidth, cbWidth);
    if (Known(maxW)) w = std::min(w, borderBox ? std::max(0.0, maxW - extrasH) : maxW);
    const double minW = ResolveSize(s.minWidth, cbWidth);
    if (Known(minW)) w = std::max(w, borderBox ? std::max(0.0, minW - extrasH) : minW);
    else if (s.minWidth.kind == Length::Kind::MinContent || s.minWidth.kind == Length::Kind::MaxContent) {
      ComputeContentSizes(lc, box);
      w = std::max(w, s.minWidth.kind == Length::Kind::MinContent ? box.minContent : box.maxContent);
    }
    return w;
  };
  const bool isTable = IsTableDisplay(s.display) && !box.replaced;
  if (isTable && !Known(forceWidth)) {
    // A table is as wide as its columns want, within what there is, and never narrower than they can be.
    ComputeContentSizes(lc, box);
    const double available = std::max(0.0, availableWidth - ml - mr - extrasH);
    if (widthAuto) width = std::min(std::max(box.minContent, available), box.maxContent);
    else width = std::max(width, box.minContent);
    widthAuto = false;
    // Collapsed borders make the table's own border half of the collapsed ones; the width is as given.
  }
  if (widthAuto) {
    if (shrinkToFit) {
      ComputeContentSizes(lc, box);
      const double available = std::max(0.0, availableWidth - ml - mr - extrasH);
      width = std::min(std::max(box.minContent, available), box.maxContent);
    } else {
      width = std::max(0.0, availableWidth - ml - mr - extrasH);
    }
    width = clampWidth(width);
    if (!shrinkToFit) {
      const double rest = availableWidth - width - extrasH - ml - mr;
      if (rest != 0 && (autoLeft || autoRight) && (width != availableWidth - ml - mr - extrasH)) {
        if (autoLeft && autoRight) { ml = rest / 2; mr = rest / 2; if (rest < 0) { ml = 0; mr = rest; } }
        else if (autoLeft) ml = rest;
        else mr = rest;
      } else if (rest != 0 && !autoLeft && !autoRight && CbDirection(box) == Direction::Rtl) {
        ml += rest;
      } else if (rest != 0 && !autoLeft && !autoRight) {
        mr += rest;
      }
    }
  } else {
    width = clampWidth(width);
    if (!Known(forceWidth) && !shrinkToFit) {
      const double rest = availableWidth - width - extrasH - ml - mr;
      if (autoLeft && autoRight) {
        if (rest >= 0) { ml = rest / 2; mr = rest / 2; }
        else if (CbDirection(box) == Direction::Rtl) { ml = rest; mr = 0; }
        else { ml = 0; mr = rest; }
      } else if (autoLeft) {
        ml = rest;
      } else if (autoRight) {
        mr = rest;
      } else {
        if (CbDirection(box) == Direction::Rtl) ml += rest;
        else mr += rest;
      }
    }
  }
  box.margin.left = ml;
  box.margin.right = mr;
  box.width = width + extrasH;
  // The offset of the border box in the parent's content box is the left margin (a parent adds its own origin).
  box.x = ml;

  // ---- height basis for the children ----
  double specifiedHeight = Known(forceHeight) ? forceHeight : ResolveSize(s.height, cbHeight);
  if (Known(specifiedHeight) && !Known(forceHeight)) specifiedHeight = borderBox ? std::max(0.0, specifiedHeight - extrasV) : specifiedHeight;
  // aspect-ratio gives a height from the width.
  double ratioHeight = kNaN;
  if (!box.replaced && s.aspectRatio > 0 && !Known(specifiedHeight) && !s.aspectRatioAuto) ratioHeight = width / s.aspectRatio;
  if (box.replaced && !Known(forceHeight)) {
    double rw, rh;
    ReplacedContentSize(box, cbWidth, cbHeight, rw, rh);
    // (when the width was forced the height follows the ratio)
    if (Known(forceWidth)) {
      double ratio = box.naturalRatio;
      if (ratio <= 0 && box.naturalWidth > 0 && box.naturalHeight > 0) ratio = box.naturalWidth / box.naturalHeight;
      if (ratio <= 0 && box.attrWidth > 0 && box.attrHeight > 0) ratio = box.attrWidth / box.attrHeight;
      if (s.aspectRatio > 0 && !s.aspectRatioAuto) ratio = s.aspectRatio;
      const bool heightFixed = !s.height.IsAuto() || box.attrHeight >= 0;
      if (!heightFixed && ratio > 0) rh = width / ratio;
    }
    specifiedHeight = rh;
  }
  const double heightForChildren = Known(specifiedHeight) ? specifiedHeight : (Known(ratioHeight) ? ratioHeight : kNaN);

  // ---- contents ----
  box.collapsedThrough = false;
  box.baseline = -1;
  double contentHeight = 0;
  // align-content other than normal puts the contents of a block container in the room it has, and makes it a formatting context.
  const bool plainBlock = !box.replaced && !IsFlexDisplay(s.display) && !IsGridDisplay(s.display) && !IsTableDisplay(s.display) && s.display != Display::TableCell;
  const bool contentAligned = plainBlock && s.alignContent.kind != Align::Kind::Normal && s.alignContent.kind != Align::Kind::Stretch && s.alignContent.kind != Align::Kind::Auto;
  const bool establishes = s.CreatesBlockFormattingContext() || IsRootBox(box) || box.forceBfc || contentAligned || s.display == Display::TableCell;
  const bool heightIsAuto = !Known(specifiedHeight) && !Known(ratioHeight);
  const double minH = [&] {
    const double v = ResolveSize(s.minHeight, cbHeight);
    return Known(v) ? (borderBox ? std::max(0.0, v - extrasV) : v) : 0.0;
  }();
  // A box's top margin is the first child's too if nothing is between them.
  const bool topOpen = !establishes && box.border.top == 0 && box.padding.top == 0;
  const bool bottomOpen = !establishes && box.border.bottom == 0 && box.padding.bottom == 0 && heightIsAuto && minH == 0;
  box.topPositive = std::max(0.0, box.margin.top);
  box.topNegative = std::min(0.0, box.margin.top);
  box.bottomPositive = std::max(0.0, box.margin.bottom);
  box.bottomNegative = std::min(0.0, box.margin.bottom);

  // The formatting context the contents are in, and where the content box is in it.
  Bfc ownBfc;
  double contentLeftBfc, contentTopBfc;
  if (establishes) {
    lc.bfc = &ownBfc;
    contentLeftBfc = 0;
    contentTopBfc = 0;
  } else {
    contentLeftBfc = lc.cbX + ml + box.border.left + box.padding.left;
    contentTopBfc = lc.boxY + box.border.top + box.padding.top;
  }
  lc.containerX = contentLeftBfc;
  lc.containerY = contentTopBfc;
  Bfc fallback;
  if (!lc.bfc) lc.bfc = &fallback;

  bool hasContent = false;
  if (box.replaced) {
    contentHeight = Known(specifiedHeight) ? specifiedHeight : 0;
    hasContent = true;
  } else if (IsFlexDisplay(s.display)) {
    lc.cbX = contentLeftBfc;
    LayoutFlex(lc, box, width, heightForChildren, contentHeight);
    for (auto& c : box.children) if (!c->style->IsOutOfFlow()) hasContent = true;
  } else if (IsTableDisplay(s.display) && !box.replaced) {
    lc.cbX = contentLeftBfc;
    LayoutTable(lc, box, width, heightForChildren, contentHeight);
    for (auto& c : box.children) if (!c->style->IsOutOfFlow()) hasContent = true;
  } else if (IsGridDisplay(s.display)) {
    lc.cbX = contentLeftBfc;
    LayoutGrid(lc, box, width, heightForChildren, contentHeight);
    for (auto& c : box.children) if (!c->style->IsOutOfFlow()) hasContent = true;
  } else if (box.hasInlineContent) {
    double baseline = -1;
    contentHeight = LayoutInlineContent(lc, box, width, baseline);
    box.baseline = baseline >= 0 ? box.ContentTop() + baseline : -1;
    hasContent = !box.lines.empty();
  } else {
    lc.cbX = contentLeftBfc;
    LayoutBlockChildren(lc, box, width, heightForChildren, topOpen, bottomOpen, contentLeftBfc, contentTopBfc, contentHeight);
    for (auto& c : box.children) {
      if (!c->style->IsOutOfFlow() && !c->style->IsFloating() && !c->collapsedThrough) hasContent = true;
    }
  }
  if (!IsFlexDisplay(s.display) && !IsGridDisplay(s.display)) {
    box.firstBaseline = -1;
    if (box.hasInlineContent && !box.lines.empty()) {
      box.firstBaseline = box.lines[0].rect.y + box.lines[0].baseline;
    } else if (!box.hasInlineContent) {
      for (auto& c : box.children) {
        if (c->style->IsOutOfFlow() || c->collapsedThrough || c->firstBaseline < 0) continue;
        box.firstBaseline = c->y + c->firstBaseline;
        break;
      }
    }
  }
  // A list item's marker sits outside, level with its first line.
  for (auto& c : box.children) {
    if (!c->outsideMarker) continue;
    lc.forceWidth = lc.forceHeight = kNaN;
    lc.cbX = 0;
    lc.boxY = 0;
    LayoutBlockLevel(lc, *c, width, kNaN, true);
    const double markerBaseline = c->firstBaseline >= 0 ? c->firstBaseline : c->height;
    const double itemBaseline = box.firstBaseline >= 0 ? box.firstBaseline - 0 : box.ContentTop() + markerBaseline;
    c->x = box.ContentLeft() - c->width;
    c->y = itemBaseline - markerBaseline;
    c->margin = Edges();
  }
  // A box that is its own formatting context holds the floats in it.
  if (establishes && heightIsAuto) contentHeight = std::max(contentHeight, ownBfc.lowest - contentTopBfc);
  double usedHeight;
  if (Known(specifiedHeight)) usedHeight = specifiedHeight;
  else if (Known(ratioHeight)) usedHeight = ratioHeight;
  else usedHeight = s.containSizeBlock && !IsGridDisplay(s.display) ? (s.containIntrinsicHeightSet ? s.containIntrinsicHeight.value : 0) : contentHeight;
  if (!Known(forceHeight)) {
    const double maxH = ResolveSize(s.maxHeight, cbHeight);
    if (Known(maxH)) usedHeight = std::min(usedHeight, borderBox ? std::max(0.0, maxH - extrasV) : maxH);
    usedHeight = std::max(usedHeight, minH);
  }
  usedHeight = std::max(0.0, usedHeight);
  box.height = usedHeight + extrasV;
  if (contentAligned || (s.display == Display::TableCell && false)) {
    const double free = usedHeight - contentHeight;
    double offset = 0;
    switch (s.alignContent.kind) {
      case Align::Kind::End: case Align::Kind::FlexEnd: case Align::Kind::LastBaseline: offset = free; break;
      case Align::Kind::Center: case Align::Kind::SpaceAround: case Align::Kind::SpaceEvenly: offset = free / 2; break;
      default: break;
    }
    if (free < 0 && !s.alignContent.unsafe) offset = 0;
    if (offset != 0) ShiftContents(box, offset);
  }

  // Empty: the margins collapse through it.
  box.collapsedThrough = !establishes && !box.replaced && heightIsAuto && usedHeight == 0 && box.border.Vertical() == 0 && box.padding.Vertical() == 0 && minH == 0 && !hasContent;
  if (box.collapsedThrough) {
    // Its top and bottom margins, and those that came out of it, are one set.
    const double positive = std::max(box.topPositive, box.bottomPositive), negative = std::min(box.topNegative, box.bottomNegative);
    box.topPositive = box.bottomPositive = positive;
    box.topNegative = box.bottomNegative = negative;
  }

  // position: relative moves the box and its contents, and nothing else.
  box.shiftX = box.shiftY = 0;
  if (s.position == Position::Relative) {
    const double left = ResolveSize(s.inset[3], cbWidth), right = ResolveSize(s.inset[1], cbWidth);
    const double top = ResolveSize(s.inset[0], cbHeight), bottom = ResolveSize(s.inset[2], cbHeight);
    if (Known(left)) box.shiftX = left;
    else if (Known(right)) box.shiftX = -right;
    if (CbDirection(box) == Direction::Rtl && Known(left) && Known(right)) box.shiftX = -right;
    if (Known(top)) box.shiftY = top;
    else if (Known(bottom)) box.shiftY = -bottom;
  }

  if (keepOwnFrame) {
    box.ownWidth = box.width;
    box.ownHeight = box.height;
  } else {
    AdaptToParentFrame(box);
  }
  lc.bfc = savedBfc;
  lc.cbX = savedCbX;
  lc.boxY = savedBoxY;
  lc.containerX = savedContainerX;
  lc.containerY = savedContainerY;
}

// ---- Positioned boxes ----

namespace {

struct CbRect {
  double x, y, width, height;
  const Box* box = nullptr;  // the box it is the padding box of, when it is one
};

bool IsPositioned(const Box& b) { return b.style->position != Position::Static; }

CbRect ContainingBlockOf(LayoutContext& lc, const Box& box) {
  const bool fixed = box.style->position == Position::Fixed;
  for (const Box* p = box.parent; p; p = p->parent) {
    if (p->parent == nullptr) break;  // the initial containing block
    if (!(fixed ? p->style->containsPositioned : (IsPositioned(*p) || p->style->containsPositioned))) continue;
    if (p->kind == Box::Kind::Block) {
      const Rect r = AbsoluteBorderBox(*p);
      return {r.x + p->border.left, r.y + p->border.top, r.width - p->border.Horizontal(), r.height - p->border.Vertical(), p};
    }
    // An inline box: the box around its fragments.
    const Box* container = p->parent;
    while (container && container->kind != Box::Kind::Block) container = container->parent;
    if (!container || p->fragments.empty()) continue;
    const Rect origin = AbsoluteBorderBox(*container);
    double l = 1e18, t = 1e18, r = -1e18, b = -1e18;
    for (const Rect& f : p->fragments) {
      l = std::min(l, f.x);
      t = std::min(t, f.y);
      r = std::max(r, f.Right());
      b = std::max(b, f.Bottom());
    }
    return {origin.x + l + p->border.left, origin.y + t + p->border.top, r - l - p->border.Horizontal(), b - t - p->border.Vertical()};
  }
  return {0, 0, lc.viewportWidth, lc.viewportHeight, nullptr};
}

// ---- Alignment of positioned boxes (css-align-3 §5.3, css-flexbox-1 §4.1, css-grid-1 §9) ----

// Physical directions of the axes of a writing mode: (1,0) is rightwards, (0,1) downwards.
struct Dir {
  int x, y;
};
Dir Negate(Dir d) { return {-d.x, -d.y}; }
int Dot(Dir a, Dir b) { return a.x * b.x + a.y * b.y; }
// The way x (the lines, left to right) and y (block-start to block-end) of a frame run.
Dir FrameX(WritingMode m) { return m == WritingMode::HorizontalTb ? Dir{1, 0} : m == WritingMode::SidewaysLr ? Dir{0, -1} : Dir{0, 1}; }
Dir FrameY(WritingMode m) {
  switch (m) {
    case WritingMode::HorizontalTb: return {0, 1};
    case WritingMode::VerticalRl: case WritingMode::SidewaysRl: return {-1, 0};
    default: return {1, 0};
  }
}
Dir InlineStartDir(WritingMode m, Direction d) { return d == Direction::Ltr ? FrameX(m) : Negate(FrameX(m)); }

enum class Side { None, Start, Center, End };

Side SideOf(Align::Kind kind, bool inlineAxis, Direction direction, Direction own = Direction::Ltr) {
  switch (kind) {
    case Align::Kind::Auto: case Align::Kind::Normal: case Align::Kind::Stretch: return Side::None;
    case Align::Kind::Start: return inlineAxis && direction == Direction::Rtl ? Side::End : Side::Start;
    case Align::Kind::SelfStart: return inlineAxis && own == Direction::Rtl ? Side::End : Side::Start;
    case Align::Kind::End: return inlineAxis && direction == Direction::Rtl ? Side::Start : Side::End;
    case Align::Kind::SelfEnd: return inlineAxis && own == Direction::Rtl ? Side::Start : Side::End;
    case Align::Kind::FlexStart: case Align::Kind::Baseline: case Align::Kind::SpaceBetween: return Side::Start;
    case Align::Kind::LastBaseline: return Side::End;
    case Align::Kind::FlexEnd: return Side::End;
    case Align::Kind::Center: case Align::Kind::SpaceAround: case Align::Kind::SpaceEvenly: return Side::Center;
    case Align::Kind::Left: return inlineAxis ? Side::Start : Side::None;
    case Align::Kind::Right: return inlineAxis ? Side::End : Side::None;
  }
  return Side::None;
}

// Where in its static-position rectangle (or inset-modified containing block) the box goes along one axis. A flex container's
// children are aligned as if each were the only item.
Side StaticSide(const Box& a, const Box* parent, bool inlineAxis, WritingMode cbMode, Direction cbDirection, bool orthogonalToCb) {
  if (!parent || (parent->Mode() != a.Mode() && !orthogonalToCb)) {
    if (!parent) return Side::None;
  }
  const BoxStyle& ps = *parent->style;
  const BoxStyle& s = *a.style;
  if (IsFlexDisplay(ps.display)) {
    const bool row = ps.flexDirection == FlexDirection::Row || ps.flexDirection == FlexDirection::RowReverse;
    bool reversed = ps.flexDirection == FlexDirection::RowReverse || ps.flexDirection == FlexDirection::ColumnReverse;
    if (row && ps.direction == Direction::Rtl) reversed = !reversed;
    if (inlineAxis == row) {  // the main axis
      const Align::Kind k = ps.justifyContent.kind;
      switch (k) {
        case Align::Kind::Normal: case Align::Kind::Stretch: case Align::Kind::FlexStart: case Align::Kind::SpaceBetween: return reversed ? Side::End : Side::Start;
        case Align::Kind::FlexEnd: return reversed ? Side::Start : Side::End;
        default: {
          const Side side = SideOf(k, inlineAxis, ps.direction);
          return side == Side::None ? Side::Start : side;
        }
      }
    }
    Align::Kind k = s.alignSelf.kind == Align::Kind::Auto ? ps.alignItems.kind : s.alignSelf.kind;
    // (The cross axis of a column is the inline axis, whose start is on the right in rtl.)
    const bool wrapReverse = (ps.flexWrap == FlexWrap::WrapReverse) != (!row && ps.direction == Direction::Rtl);
    if (k == Align::Kind::Normal || k == Align::Kind::Stretch || k == Align::Kind::Auto) k = Align::Kind::FlexStart;
    if (k == Align::Kind::FlexStart) return wrapReverse ? Side::End : Side::Start;
    if (k == Align::Kind::FlexEnd) return wrapReverse ? Side::Start : Side::End;
    const Side side = SideOf(k, inlineAxis, ps.direction);
    return side == Side::None ? Side::Start : side;
  }
  // (justify-self is about the lines of the containing block, which are this box's block axis when the two are orthogonal.)
  const bool cbInline = inlineAxis != orthogonalToCb;
  Align self = cbInline ? s.justifySelf : s.alignSelf;
  if (self.kind == Align::Kind::Auto && IsGridDisplay(ps.display)) self = cbInline ? ps.justifyItems : ps.alignItems;
  const Align::Kind kind = self.kind;
  if (kind == Align::Kind::Auto || kind == Align::Kind::Normal || kind == Align::Kind::Stretch) return Side::None;
  if (kind == Align::Kind::Center || kind == Align::Kind::SpaceAround || kind == Align::Kind::SpaceEvenly) return Side::Center;
  const WritingMode own = a.Mode();
  const Dir axis = inlineAxis ? FrameX(own) : FrameY(own);
  // Which end of the containing block's axis (or of the box's own) the box hugs, and which way that axis runs.
  Dir startToEnd = cbInline ? InlineStartDir(cbMode, cbDirection) : FrameY(cbMode);
  bool wantStart = true;
  switch (kind) {
    case Align::Kind::End: case Align::Kind::FlexEnd: case Align::Kind::LastBaseline: wantStart = false; break;
    case Align::Kind::SelfStart: startToEnd = inlineAxis ? InlineStartDir(own, s.direction) : FrameY(own); break;
    case Align::Kind::SelfEnd: startToEnd = inlineAxis ? InlineStartDir(own, s.direction) : FrameY(own); wantStart = false; break;
    case Align::Kind::Left: startToEnd = FrameX(cbMode); break;
    case Align::Kind::Right: startToEnd = FrameX(cbMode); wantStart = false; break;
    default: break;
  }
  const int d = Dot(startToEnd, axis);
  if (d == 0) return Side::Start;
  return ((d > 0) == wantStart) ? Side::Start : Side::End;
}

double Place(Side side, double start, double end, double outer, double cbSize, bool mirrored, bool safe, bool unsafe) {
  if (mirrored) {
    const Side flipped = side == Side::Start ? Side::End : side == Side::End ? Side::Start : side;
    return cbSize - Place(flipped, cbSize - end, cbSize - start, outer, cbSize, false, safe, unsafe) - outer;
  }
  const double free = (end - start) - outer;
  double pos = start;
  if (side == Side::Center) pos = start + free / 2;
  else if (side == Side::End) pos = start + free;
  if (free < 0 && safe) return start;
  if (unsafe) return pos;
  const double lo = std::min(start, 0.0), hi = std::max(end, cbSize);
  if (outer > hi - lo) return lo;
  return std::max(lo, std::min(pos, hi - outer));
}

void PlaceOne(LayoutContext& lc, Box& a) {
  const BoxStyle& s = *a.style;
  const WritingMode mode = a.Mode();
  CbRect cbPhys = ContainingBlockOf(lc, a);
  // Inside a grid container, the grid-placement properties name the area that is the containing block.
  if (cbPhys.box && IsGridDisplay(cbPhys.box->style->display) && cbPhys.box->gridLines) {
    const Box& g = *cbPhys.box;
    const Edges own = EdgesToFrame(g.Mode(), g.border);
    const double paddingW = g.ownWidth - own.left - own.right, paddingH = g.ownHeight - own.top - own.bottom;
    Rect area;
    if (GridAreaOf(g, a.style->Physical().writingMode == mode ? *a.style : *a.style, paddingW, paddingH, area)) {
      Rect physical;
      FrameToPhysical(g.Mode(), paddingW, paddingH, area.x, area.y, area.width, area.height, physical);
      cbPhys = {cbPhys.x + physical.x, cbPhys.y + physical.y, physical.width, physical.height, nullptr};
    }
  }
  // The containing block in the box's own frame: its inline size is the physical extent along the box's lines.
  const bool vertical = IsVertical(mode);
  const double cw = vertical ? cbPhys.height : cbPhys.width, ch = vertical ? cbPhys.width : cbPhys.height;
  // Trial layout, for the width the contents want and the box's edges.
  Bfc scratch;
  lc.bfc = &scratch;
  lc.cbX = 0;
  lc.boxY = 0;
  lc.forceWidth = lc.forceHeight = lc.availOverride = kNaN;
  const double left = ResolveSize(s.inset[3], cw), right = ResolveSize(s.inset[1], cw);
  const double top = ResolveSize(s.inset[0], ch), bottom = ResolveSize(s.inset[2], ch);
  lc.keepOwnFrame = true;
  LayoutBlockLevel(lc, a, cw, ch, true);
  if (getenv("SOLAR_DEBUG")) std::fprintf(stderr, "PlaceOne trial: width %g height %g inline %d children %zu min %g max %g cw %g lines %zu\n", a.width, a.height, (int)a.hasInlineContent, a.children.size(), a.minContent, a.maxContent, cw, a.lines.size());
  const double extrasH = ExtrasHorizontal(a), extrasV = ExtrasVertical(a);
  const bool autoMl = s.margin[3].IsAuto(), autoMr = s.margin[1].IsAuto(), autoMt = s.margin[0].IsAuto(), autoMb = s.margin[2].IsAuto();
  double ml = autoMl ? 0 : s.margin[3].Resolve(cw), mr = autoMr ? 0 : s.margin[1].Resolve(cw);
  double mt = autoMt ? 0 : s.margin[0].Resolve(cw), mb = autoMb ? 0 : s.margin[2].Resolve(cw);
  const bool borderBox = s.boxSizing == BoxSizing::BorderBox;

  // The static position: the box's parent's physical corner and where in that the box would be, in the containing block's frame.
  double staticLeft = 0, staticTop = 0;
  {
    const Box* p = a.parent;
    while (p && p->kind != Box::Kind::Block) p = p->parent;
    const Rect parentBox = p ? AbsoluteBorderBox(*p) : Rect{0, 0, 0, 0};
    const Rect physical = {parentBox.x + a.staticX - cbPhys.x, parentBox.y + a.staticY - cbPhys.y, 0, 0};
    double w2, h2;
    PhysicalToFrame(mode, cw, ch, physical, staticLeft, staticTop, w2, h2);
  }

  // The static-position rectangle: the content box of a flex or grid container parent, else a point.
  const Box* staticParent = a.parent;
  while (staticParent && staticParent->kind != Box::Kind::Block) staticParent = staticParent->parent;
  double spL = staticLeft, spR = staticLeft, spT = staticTop, spB = staticTop;
  if (staticParent && (IsFlexDisplay(staticParent->style->display) || IsGridDisplay(staticParent->style->display)) && staticParent->Mode() == mode) {
    const Rect pr = AbsoluteBorderBox(*staticParent);
    const Rect content = {pr.x + staticParent->border.left + staticParent->padding.left - cbPhys.x, pr.y + staticParent->border.top + staticParent->padding.top - cbPhys.y,
                          pr.width - staticParent->border.Horizontal() - staticParent->padding.Horizontal(), pr.height - staticParent->border.Vertical() - staticParent->padding.Vertical()};
    double rx, ry, rw, rh;
    PhysicalToFrame(mode, cw, ch, content, rx, ry, rw, rh);
    spL = rx; spR = rx + rw; spT = ry; spB = ry + rh;
  }
  const WritingMode cbMode = cbPhys.box ? cbPhys.box->Mode() : lc.tree.root->Mode();
  const bool orthogonalToCb = Orthogonal(cbMode, mode);
  const Direction cbDirection = cbPhys.box ? cbPhys.box->style->direction : lc.tree.root->style->direction;
  const Side inlineSide = StaticSide(a, staticParent, true, cbMode, cbDirection, orthogonalToCb), blockSide = StaticSide(a, staticParent, false, cbMode, cbDirection, orthogonalToCb);
  const Align& inlineSelf = orthogonalToCb ? s.alignSelf : s.justifySelf;
  const Align& blockSelf = orthogonalToCb ? s.justifySelf : s.alignSelf;

  // ---- inline axis ----
  double W = kNaN;
  if (s.width.kind == Length::Kind::Px || s.width.IsPercentage()) {
    const double w = ResolveSize(s.width, cw);
    if (Known(w)) W = borderBox ? std::max(0.0, w - extrasH) : w;
  } else if (a.replaced) {
    W = a.width - extrasH;
  }
  const bool widthAuto = !Known(W);
  double L = left, R = right;
  const auto shrink = [&](double avail) {
    ComputeContentSizes(lc, a);
    return std::min(std::max(a.minContent, std::max(0.0, avail)), a.maxContent);
  };
  const auto clampW = [&](double w) {
    const double maxW = ResolveSize(s.maxWidth, cw);
    if (Known(maxW)) w = std::min(w, borderBox ? std::max(0.0, maxW - extrasH) : maxW);
    const double minW = ResolveSize(s.minWidth, cw);
    if (Known(minW)) w = std::max(w, borderBox ? std::max(0.0, minW - extrasH) : minW);
    return w;
  };
  const auto solveH = [&]() {
    if (widthAuto) {
      if (!Known(L) && !Known(R)) {
        W = shrink(cw - staticLeft - ml - mr - extrasH);
        L = staticLeft;
      } else if (!Known(L)) {
        W = shrink(cw - R - ml - mr - extrasH);
      } else if (!Known(R)) {
        W = shrink(cw - L - ml - mr - extrasH);
      } else if (inlineSide != Side::None) {
        W = shrink(cw - L - R - ml - mr - extrasH);  // aligned, so not stretched
      } else {
        W = std::max(0.0, cw - L - R - ml - mr - extrasH);
      }
    }
    W = clampW(W);
    if (!Known(L) && !Known(R)) L = staticLeft;
    if (!Known(L)) L = cw - R - W - extrasH - ml - mr;
    else if (!Known(R)) R = cw - L - W - extrasH - ml - mr;
    else {
      const double rest = cw - L - R - W - extrasH - ml - mr;
      if (autoMl && autoMr) {
        if (rest >= 0) { ml += rest / 2; mr += rest / 2; }
        else { ml += 0; mr += rest; }
      } else if (autoMl) {
        ml += rest;
      } else if (autoMr) {
        mr += rest;
      }
      // (over-constrained: the right offset is ignored)
    }
  };
  solveH();
  if (inlineSide != Side::None) {
    // Aligned in the space between the insets (a missing one being the static rectangle's edge).
    const double i0 = Known(left) ? left : spL, i1 = Known(right) ? cw - right : spR;
    L = Place(inlineSide, i0, i1, W + extrasH + ml + mr, cw, Dot(orthogonalToCb ? FrameY(cbMode) : InlineStartDir(cbMode, cbDirection), FrameX(mode)) < 0, inlineSelf.safe, inlineSelf.unsafe);
  }

  // Lay out at the width, for the height the contents have.
  lc.forceWidth = W;
  lc.keepOwnFrame = true;
  LayoutBlockLevel(lc, a, cw, ch, false);
  // ---- block axis ----
  double H = kNaN;
  if (s.height.kind == Length::Kind::Px || s.height.IsPercentage()) {
    const double h = ResolveSize(s.height, ch);
    if (Known(h)) H = borderBox ? std::max(0.0, h - extrasV) : h;
  } else if (a.replaced) {
    H = a.height - extrasV;
  }
  const bool heightAuto = !Known(H);
  double T = top, B = bottom;
  const double contentH = a.height - extrasV;
  if (heightAuto) {
    if (!Known(T) && !Known(B)) { H = contentH; T = staticTop; }
    else if (!Known(T)) H = contentH;
    else if (!Known(B)) H = contentH;
    else if (blockSide != Side::None) H = contentH;  // aligned, so not stretched
    else H = std::max(0.0, ch - T - B - mt - mb - extrasV);
  }
  {
    const double maxH = ResolveSize(s.maxHeight, ch);
    if (Known(maxH)) H = std::min(H, borderBox ? std::max(0.0, maxH - extrasV) : maxH);
    const double minH = ResolveSize(s.minHeight, ch);
    if (Known(minH)) H = std::max(H, borderBox ? std::max(0.0, minH - extrasV) : minH);
  }
  if (!Known(T) && !Known(B)) T = staticTop;
  if (!Known(T)) T = ch - B - H - extrasV - mt - mb;
  else if (!Known(B)) B = ch - T - H - extrasV - mt - mb;
  else {
    const double rest = ch - T - B - H - extrasV - mt - mb;
    if (autoMt && autoMb) { mt += rest / 2; mb += rest / 2; }
    else if (autoMt) mt += rest;
    else if (autoMb) mb += rest;
  }
  if (blockSide != Side::None) {
    const double i0 = Known(top) ? top : spT, i1 = Known(bottom) ? ch - bottom : spB;
    T = Place(blockSide, i0, i1, H + extrasV + mt + mb, ch, Dot(orthogonalToCb ? InlineStartDir(cbMode, cbDirection) : FrameY(cbMode), FrameY(mode)) < 0, blockSelf.safe, blockSelf.unsafe);
  }
  lc.forceWidth = W;
  lc.forceHeight = H;
  lc.keepOwnFrame = true;
  LayoutBlockLevel(lc, a, cw, ch, false);
  a.margin.left = ml;
  a.margin.right = mr;
  a.margin.top = mt;
  a.margin.bottom = mb;
  // The border box in the containing block's own-frame coordinates, then physical, then relative to the parent.
  Rect inFrame = {L + ml, T + mt, a.width, a.height};
  Rect physical;
  FrameToPhysical(mode, cw, ch, inFrame.x, inFrame.y, inFrame.width, inFrame.height, physical);
  const Box* coordinateParent = a.parent;
  while (coordinateParent && coordinateParent->kind != Box::Kind::Block) coordinateParent = coordinateParent->parent;
  const Rect origin = coordinateParent ? AbsoluteBorderBox(*coordinateParent) : Rect{0, 0, 0, 0};
  // Make a's fields physical: its own frame's size becomes the physical one, and its contents are converted.
  a.ownWidth = a.width;
  a.ownHeight = a.height;
  ConvertPlaced(a, mode, cw, ch, inFrame);
  a.x = cbPhys.x + physical.x - origin.x;
  a.y = cbPhys.y + physical.y - origin.y;
}

void Walk(LayoutContext& lc, Box& box) {
  for (auto& c : box.children) {
    if (IsPositionedOutOfFlow(*c)) PlaceOne(lc, *c);
    Walk(lc, *c);
  }
}

}  // namespace

void PlacePositioned(LayoutContext& lc) {
  lc.bfc = nullptr;
  Walk(lc, *lc.tree.root);
}

void LayoutRoot(LayoutContext& lc) {
  Box& icb = *lc.tree.root;
  // The root element's writing mode is the viewport's.
  if (!icb.children.empty()) {
    auto style = std::make_shared<BoxStyle>(*icb.style);
    style->writingMode = icb.children[0]->style->writingMode;
    style->direction = icb.children[0]->style->direction;
    icb.style = style;
  }
  const bool vertical = IsVertical(icb.Mode());
  const double inlineSize = vertical ? lc.viewportHeight : lc.viewportWidth, blockSize = vertical ? lc.viewportWidth : lc.viewportHeight;
  icb.x = icb.y = 0;
  icb.width = inlineSize;
  double bottom = 0;
  Bfc rootBfc;
  lc.bfc = &rootBfc;
  for (auto& child : icb.children) {
    if (child->style->IsOutOfFlow()) continue;
    lc.cbX = 0;
    lc.boxY = 0;
    LayoutBlockLevel(lc, *child, inlineSize, blockSize);
    child->x = child->margin.left + child->shiftX;
    child->y = child->margin.top + child->shiftY;
    bottom = std::max(bottom, child->y + child->height + child->margin.bottom);
  }
  icb.ownWidth = inlineSize;
  icb.ownHeight = std::max(blockSize, bottom);
  icb.height = icb.ownHeight;
  lc.bfc = nullptr;
  ConvertTree(lc.tree);
  PlacePositioned(lc);
}

}  // namespace solar::layout
