// Block layout (https://www.w3.org/TR/CSS22/visudet.html, https://www.w3.org/TR/CSS22/box.html#collapsing-margins), floats
// (https://www.w3.org/TR/CSS22/visuren.html#floats) and absolute positioning (https://www.w3.org/TR/CSS22/visuren.html#absolute-positioning).
#include <algorithm>
#include <cmath>

#include "Internal.h"

namespace solar::layout {

namespace {

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

bool IsFloat(const Box& b) { return b.style->IsFloating() && !b.style->IsOutOfFlow(); }
bool IsPositionedOutOfFlow(const Box& b) { return b.style->IsOutOfFlow(); }

bool FloatsLeft(const BoxStyle& s) {
  const bool rtl = s.direction == Direction::Rtl;
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
  const bool isLeft = FloatsLeft(*box.style);
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
  if (s.containSizeInline) {  // its contents count for nothing
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
      const bool rtl = cs.direction == Direction::Rtl;
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
    lc.cbX = px + shiftX;
    lc.boxY = estimateTop;
    lc.availOverride = availOverride;
    LayoutBlockLevel(lc, child, contentWidth, heightBasis);
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
      LayoutBlockLevel(lc, child, contentWidth, heightBasis);
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
      } else if (rest != 0 && !autoLeft && !autoRight && s.direction == Direction::Rtl) {
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
        else if (s.direction == Direction::Rtl) { ml = rest; mr = 0; }
        else { ml = 0; mr = rest; }
      } else if (autoLeft) {
        ml = rest;
      } else if (autoRight) {
        mr = rest;
      } else {
        if (s.direction == Direction::Rtl) ml += rest;
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
  const bool establishes = s.CreatesBlockFormattingContext() || IsRootBox(box) || box.forceBfc;
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
  else usedHeight = s.containSizeBlock ? (s.containIntrinsicHeightSet ? s.containIntrinsicHeight.value : 0) : contentHeight;
  if (!Known(forceHeight)) {
    const double maxH = ResolveSize(s.maxHeight, cbHeight);
    if (Known(maxH)) usedHeight = std::min(usedHeight, borderBox ? std::max(0.0, maxH - extrasV) : maxH);
    usedHeight = std::max(usedHeight, minH);
  }
  usedHeight = std::max(0.0, usedHeight);
  box.height = usedHeight + extrasV;

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
    if (s.direction == Direction::Rtl && Known(left) && Known(right)) box.shiftX = -right;
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
};

bool IsPositioned(const Box& b) { return b.style->position != Position::Static; }

CbRect ContainingBlockOf(LayoutContext& lc, const Box& box) {
  const bool fixed = box.style->position == Position::Fixed;
  for (const Box* p = box.parent; p; p = p->parent) {
    if (p->parent == nullptr) break;  // the initial containing block
    if (!(fixed ? p->style->containsPositioned : (IsPositioned(*p) || p->style->containsPositioned))) continue;
    if (p->kind == Box::Kind::Block) {
      const Rect r = AbsoluteBorderBox(*p);
      return {r.x + p->border.left, r.y + p->border.top, r.width - p->border.Horizontal(), r.height - p->border.Vertical()};
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
  return {0, 0, lc.viewportWidth, lc.viewportHeight};
}

void PlaceOne(LayoutContext& lc, Box& a) {
  const BoxStyle& s = *a.style;
  const WritingMode mode = a.Mode();
  const CbRect cbPhys = ContainingBlockOf(lc, a);
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
