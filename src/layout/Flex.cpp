// Flexible box layout (https://www.w3.org/TR/css-flexbox-1/#layout-algorithm), for the horizontal writing mode.
#include <algorithm>
#include <cmath>

#include "Internal.h"

namespace solar::layout {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

bool Known(double v) { return !std::isnan(v); }

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

struct Item {
  Box* box = nullptr;
  int index = 0;
  // Axes: "main" is the flex container's main axis. All sizes below are content-box unless said.
  double padBorderMain = 0, padBorderCross = 0;
  double marginMainStart = 0, marginMainEnd = 0, marginCrossStart = 0, marginCrossEnd = 0;
  bool autoMainStart = false, autoMainEnd = false, autoCrossStart = false, autoCrossEnd = false;
  double baseSize = 0, hypothetical = 0;  // content-box
  double minMain = 0, maxMain = kInf;
  double mainSize = 0;
  double crossSize = 0;    // content-box, after layout
  double grow = 0, shrink = 1;
  bool frozen = false;
  bool crossAuto = true;   // the cross size property is auto
  double baselineOffset = 0;  // from the item's margin-box cross start
  double posMain = 0, posCross = 0;  // margin box start, in the line's coordinates
  double OuterMain() const { return mainSize + padBorderMain + marginMainStart + marginMainEnd; }
  double OuterHypothetical() const { return hypothetical + padBorderMain + marginMainStart + marginMainEnd; }
  double OuterCross() const { return crossSize + padBorderCross + marginCrossStart + marginCrossEnd; }
};

struct Line {
  std::vector<Item*> items;
  double crossSize = 0;  // outer
  double crossPos = 0;
  double mainUsed = 0;
};

struct Flex {
  LayoutContext& lc;
  Box& container;
  const BoxStyle& style;
  bool row, reversedMain, wrapReverse, wraps;
  double containerWidth;     // content box
  double containerHeight;    // content box, or NaN
  double mainAvailable;      // inner main size of the container, or NaN (indefinite)
  double crossAvailable;     // inner cross size, or NaN
  double gapMain = 0, gapCross = 0;
  std::vector<Item> items;
  std::vector<Line> lines;

  Flex(LayoutContext& l, Box& c, double w, double h) : lc(l), container(c), style(*c.style), containerWidth(w), containerHeight(h) {
    const FlexDirection d = style.flexDirection;
    row = d == FlexDirection::Row || d == FlexDirection::RowReverse;
    reversedMain = d == FlexDirection::RowReverse || d == FlexDirection::ColumnReverse;
    if (row && style.direction == Direction::Rtl) reversedMain = !reversedMain;
    wraps = style.flexWrap != FlexWrap::Nowrap;
    wrapReverse = style.flexWrap == FlexWrap::WrapReverse;
    mainAvailable = row ? w : h;
    crossAvailable = row ? h : w;
    const double colGap = ResolveSize(style.columnGap, containerWidth);
    const double rowGap = ResolveSize(style.rowGap, containerHeight);
    const double cg = Known(colGap) ? colGap : (style.columnGap.kind == Length::Kind::Percent ? 0 : 0);
    const double rg = Known(rowGap) ? rowGap : 0;
    gapMain = row ? cg : rg;
    gapCross = row ? rg : cg;
  }

  // ---- Per item ----
  void Edges(Item& it) {
    Box& b = *it.box;
    const BoxStyle& s = *b.style;
    const double cb = containerWidth;
    b.border = {s.border[0], s.border[1], s.border[2], s.border[3]};
    b.padding = {std::max(0.0, s.padding[0].Resolve(cb)), std::max(0.0, s.padding[1].Resolve(cb)), std::max(0.0, s.padding[2].Resolve(cb)), std::max(0.0, s.padding[3].Resolve(cb))};
    const auto margin = [&](int side, bool& isAuto) {
      isAuto = s.margin[side].IsAuto();
      return isAuto ? 0.0 : s.margin[side].Resolve(cb);
    };
    // top right bottom left = 0 1 2 3
    const int mainStartSide = row ? 3 : 0, mainEndSide = row ? 1 : 2, crossStartSide = row ? 0 : 3, crossEndSide = row ? 2 : 1;
    it.marginMainStart = margin(mainStartSide, it.autoMainStart);
    it.marginMainEnd = margin(mainEndSide, it.autoMainEnd);
    it.marginCrossStart = margin(crossStartSide, it.autoCrossStart);
    it.marginCrossEnd = margin(crossEndSide, it.autoCrossEnd);
    it.padBorderMain = row ? b.border.Horizontal() + b.padding.Horizontal() : b.border.Vertical() + b.padding.Vertical();
    it.padBorderCross = row ? b.border.Vertical() + b.padding.Vertical() : b.border.Horizontal() + b.padding.Horizontal();
    it.grow = s.flexGrow;
    it.shrink = s.flexShrink;
    const Length& crossLen = row ? s.height : s.width;
    it.crossAuto = !Known(ResolveSize(crossLen, row ? containerHeight : containerWidth));
    if (crossLen.IsKeyword()) it.crossAuto = true;
  }

  // The content-box size a size property asks for in the item's main or cross axis, or NaN.
  double Specified(const Item& it, const Length& l, bool mainAxis) const {
    const double basis = (row == mainAxis) ? containerWidth : containerHeight;
    double v = ResolveSize(l, basis);
    if (!Known(v)) return kNaN;
    const double pb = mainAxis ? it.padBorderMain : it.padBorderCross;
    return it.box->style->boxSizing == BoxSizing::BorderBox ? std::max(0.0, v - pb) : v;
  }

  // Lays the item out at the given content sizes (NaN: its own), and returns the box.
  void LayoutItem(Item& it, double forceMain, double forceCross, bool fill = false) {
    Box& b = *it.box;
    lc.forceWidth = row ? forceMain : forceCross;
    lc.forceHeight = row ? forceCross : forceMain;
    lc.cbX = 0;
    lc.boxY = 0;
    (void)fill;
    LayoutBlockLevel(lc, b, containerWidth, containerHeight, true);
  }

  // The item's content size in its main axis for max-content (and the cross size to lay out at, for a column).
  double ContentMain(Item& it, double crossForColumn) {
    Box& b = *it.box;
    if (row) {
      ComputeContentSizes(lc, b);
      return b.maxContent;
    }
    LayoutItem(it, kNaN, crossForColumn);
    return b.height - it.padBorderMain;
  }

  double MinContentMain(Item& it, double crossForColumn) {
    Box& b = *it.box;
    if (row) {
      ComputeContentSizes(lc, b);
      return b.minContent;
    }
    LayoutItem(it, kNaN, crossForColumn);
    return b.height - it.padBorderMain;
  }

  // The width a column item gets in the cross axis before the main size is known.
  double ProvisionalCross(Item& it) {
    const BoxStyle& s = *it.box->style;
    const double specified = Specified(it, s.width, false);
    if (Known(specified)) return specified;
    const double room = std::max(0.0, containerWidth - it.marginCrossStart - it.marginCrossEnd - it.padBorderCross);
    const Align::Kind a = (s.alignSelf.kind == Align::Kind::Auto) ? style.alignItems.kind : s.alignSelf.kind;
    if ((a == Align::Kind::Normal || a == Align::Kind::Stretch) && !it.autoCrossStart && !it.autoCrossEnd) return room;
    ComputeContentSizes(lc, *it.box);
    return std::min(std::max(it.box->minContent, room), it.box->maxContent);
  }

  void BaseSizes() {
    for (Item& it : items) {
      Box& b = *it.box;
      const BoxStyle& s = *b.style;
      const double crossForColumn = row ? kNaN : ProvisionalCross(it);
      // flex-basis
      double base = kNaN;
      if (!s.flexBasisContent) {
        const Length& basis = s.flexBasis.IsAuto() ? (row ? s.width : s.height) : s.flexBasis;
        base = Specified(it, basis, true);
        if (basis.kind == Length::Kind::MinContent || basis.kind == Length::Kind::MaxContent || basis.kind == Length::Kind::FitContent) {
          base = row ? (ComputeContentSizes(lc, b), basis.kind == Length::Kind::MinContent ? b.minContent : b.maxContent) : kNaN;
        }
      }
      if (!Known(base)) {
        if (b.replaced && !row && b.naturalHeight >= 0) base = b.naturalHeight;
        else base = ContentMain(it, crossForColumn);
      }
      it.baseSize = std::max(0.0, base);
      // min and max
      const Length& maxLen = row ? s.maxWidth : s.maxHeight;
      const Length& minLen = row ? s.minWidth : s.minHeight;
      const double maxV = Specified(it, maxLen, true);
      it.maxMain = Known(maxV) ? maxV : kInf;
      double minV = Specified(it, minLen, true);
      if (!Known(minV)) {
        const bool scroller = (row ? s.overflowX : s.overflowY) != Overflow::Visible && (row ? s.overflowX : s.overflowY) != Overflow::Clip;
        if (minLen.IsAuto() && !scroller) {
          // The content-based minimum: the smaller of what the size property says and the content.
          double content = MinContentMain(it, crossForColumn);
          if (b.replaced && row && b.naturalWidth >= 0) content = b.naturalWidth;
          const double specifiedSize = Specified(it, row ? s.width : s.height, true);
          double suggestion = Known(specifiedSize) ? std::min(specifiedSize, content) : content;
          minV = std::min(suggestion, it.maxMain);
        } else {
          minV = 0;
        }
      }
      it.minMain = minV;
      it.hypothetical = std::max(it.minMain, std::min(it.maxMain, it.baseSize));
      it.mainSize = it.hypothetical;
    }
  }

  void CollectLines() {
    lines.clear();
    lines.emplace_back();
    double used = 0;
    const bool limited = wraps && Known(mainAvailable);
    for (Item& it : items) {
      Line* line = &lines.back();
      const double outer = it.OuterHypothetical();
      if (limited && !line->items.empty() && used + gapMain + outer > mainAvailable + 1e-6) {
        lines.emplace_back();
        line = &lines.back();
        used = 0;
      }
      used += (line->items.empty() ? 0 : gapMain) + outer;
      line->items.push_back(&it);
    }
  }

  // 9.7 Resolving Flexible Lengths.
  void ResolveFlexible(Line& line) {
    double available = mainAvailable;
    if (!Known(available)) {
      // Indefinite: the items are as big as they want.
      for (Item* it : line.items) it->mainSize = it->hypothetical;
      return;
    }
    double gaps = gapMain * (line.items.size() > 1 ? line.items.size() - 1 : 0);
    double hypotheticalSum = gaps;
    for (Item* it : line.items) hypotheticalSum += it->OuterHypothetical();
    const bool growing = hypotheticalSum < available;
    for (Item* it : line.items) {
      it->frozen = false;
      it->mainSize = it->baseSize;
      const double factor = growing ? it->grow : it->shrink;
      if (factor == 0 || (growing && it->baseSize > it->hypothetical) || (!growing && it->baseSize < it->hypothetical)) {
        it->frozen = true;
        it->mainSize = it->hypothetical;
      }
    }
    for (int guard = 0; guard < 1000; ++guard) {
      double frozenSum = gaps, unfrozenBase = 0, flexSum = 0;
      for (Item* it : line.items) {
        const double extras = it->padBorderMain + it->marginMainStart + it->marginMainEnd;
        if (it->frozen) frozenSum += it->mainSize + extras;
        else {
          unfrozenBase += it->baseSize + extras;
          flexSum += growing ? it->grow : it->shrink;
        }
      }
      bool anyUnfrozen = false;
      for (Item* it : line.items) if (!it->frozen) anyUnfrozen = true;
      if (!anyUnfrozen) break;
      double remaining = available - frozenSum - unfrozenBase;
      // If the sum of factors is less than one the free space is scaled down.
      const double initialFree = available - gaps - [&] { double t = 0; for (Item* it : line.items) t += it->frozen ? it->mainSize + it->padBorderMain + it->marginMainStart + it->marginMainEnd : it->baseSize + it->padBorderMain + it->marginMainStart + it->marginMainEnd; return t; }();
      (void)initialFree;
      if (flexSum < 1) {
        const double scaled = remaining * flexSum;
        if (std::fabs(scaled) < std::fabs(remaining)) remaining = scaled;
      }
      if (growing) {
        for (Item* it : line.items) {
          if (it->frozen) continue;
          it->mainSize = it->baseSize + (flexSum > 0 ? remaining * (it->grow / flexSum) : 0);
        }
      } else {
        double scaledSum = 0;
        for (Item* it : line.items) if (!it->frozen) scaledSum += it->shrink * it->baseSize;
        for (Item* it : line.items) {
          if (it->frozen) continue;
          const double ratio = scaledSum > 0 ? (it->shrink * it->baseSize) / scaledSum : 0;
          it->mainSize = it->baseSize + remaining * ratio;
        }
      }
      // Fix violations.
      double totalViolation = 0;
      std::vector<std::pair<Item*, double>> clamped;
      for (Item* it : line.items) {
        if (it->frozen) continue;
        const double c = std::max(it->minMain, std::min(it->maxMain, it->mainSize));
        const double v = c - it->mainSize;
        totalViolation += v;
        clamped.push_back({it, v});
        it->mainSize = std::max(0.0, c);
      }
      for (auto& [it, v] : clamped) {
        if (totalViolation == 0 || (totalViolation > 0 && v > 0) || (totalViolation < 0 && v < 0)) it->frozen = true;
      }
      bool done = true;
      for (Item* it : line.items) if (!it->frozen) done = false;
      if (done) break;
    }
  }

  // Lays the items out at their main sizes, which gives the cross sizes.
  void CrossSizes() {
    for (Item& it : items) {
      Box& b = *it.box;
      // A column item with no width of its own gets what align-self says; a row item's height is its content's.
      double forceCross = kNaN;
      if (!row && it.crossAuto) {
        const Align::Kind a = (b.style->alignSelf.kind == Align::Kind::Auto) ? style.alignItems.kind : b.style->alignSelf.kind;
        if ((a == Align::Kind::Normal || a == Align::Kind::Stretch) && !it.autoCrossStart && !it.autoCrossEnd) {
          forceCross = std::max(0.0, containerWidth - it.marginCrossStart - it.marginCrossEnd - it.padBorderCross);
        }
      }
      LayoutItem(it, it.mainSize, forceCross);
      it.crossSize = row ? b.height - it.padBorderCross : b.width - it.padBorderCross;
    }
  }

  Align::Kind AlignOf(const Item& it) const {
    Align::Kind a = it.box->style->alignSelf.kind;
    if (a == Align::Kind::Auto) a = style.alignItems.kind;
    if (a == Align::Kind::Normal) a = Align::Kind::Stretch;
    if (a == Align::Kind::Start || a == Align::Kind::SelfStart) a = wrapReverse ? Align::Kind::FlexEnd : Align::Kind::FlexStart;
    if (a == Align::Kind::End || a == Align::Kind::SelfEnd) a = wrapReverse ? Align::Kind::FlexStart : Align::Kind::FlexEnd;
    return a;
  }

  void Run() {
    for (Item& it : items) Edges(it);
    BaseSizes();
    CollectLines();
    for (Line& line : lines) ResolveFlexible(line);
    CrossSizes();

    // ---- Line cross sizes ----
    const bool singleLineFixed = !wraps && Known(crossAvailable);
    for (Line& line : lines) {
      double maxAbove = 0, maxBelow = 0, maxOuter = 0;
      bool anyBaseline = false;
      for (Item* it : line.items) {
        const Align::Kind a = AlignOf(*it);
        const double outer = it->OuterCross();
        if (a == Align::Kind::Baseline && row && !it->autoCrossStart && !it->autoCrossEnd) {
          const double base = it->box->firstBaseline >= 0 ? it->box->firstBaseline : it->box->height;
          const double above = it->marginCrossStart + base;
          maxAbove = std::max(maxAbove, above);
          maxBelow = std::max(maxBelow, outer - above);
          anyBaseline = true;
          it->baselineOffset = above;
        }
        maxOuter = std::max(maxOuter, outer);
      }
      line.crossSize = std::max(maxOuter, anyBaseline ? maxAbove + maxBelow : 0.0);
      if (anyBaseline) {
        for (Item* it : line.items) if (AlignOf(*it) == Align::Kind::Baseline && row && !it->autoCrossStart && !it->autoCrossEnd) it->baselineOffset = maxAbove - it->baselineOffset;
      }
      if (singleLineFixed) line.crossSize = crossAvailable;
    }
    if (singleLineFixed) {
      // clamp by the container's min and max
    }
    // The container's cross size (content box).
    double crossTotal = 0;
    for (size_t i = 0; i < lines.size(); ++i) crossTotal += lines[i].crossSize + (i ? gapCross : 0);
    double containerCross = Known(crossAvailable) ? crossAvailable : crossTotal;
    if (!Known(crossAvailable)) {
      // auto: clamped by min and max
      const BoxStyle& s = style;
      const double minC = ResolveSize(row ? s.minHeight : s.minWidth, row ? containerHeight : containerWidth);
      const double maxC = ResolveSize(row ? s.maxHeight : s.maxWidth, row ? containerHeight : containerWidth);
      if (Known(maxC)) containerCross = std::min(containerCross, maxC);
      if (Known(minC)) containerCross = std::max(containerCross, minC);
    }
    // align-content: lines in the cross space.
    {
      const double free = containerCross - crossTotal;
      Align::Kind ac = style.alignContent.kind;
      if (ac == Align::Kind::Normal) ac = Align::Kind::Stretch;
      if (!wraps) ac = Align::Kind::Stretch;
      if (ac == Align::Kind::Start) ac = wrapReverse ? Align::Kind::FlexEnd : Align::Kind::FlexStart;
      if (ac == Align::Kind::End) ac = wrapReverse ? Align::Kind::FlexStart : Align::Kind::FlexEnd;
      double offset = 0, between = 0;
      const double n = static_cast<double>(lines.size());
      if (free < 0 && (ac == Align::Kind::SpaceBetween)) ac = Align::Kind::FlexStart;
      if (free < 0 && (ac == Align::Kind::SpaceAround || ac == Align::Kind::SpaceEvenly)) ac = Align::Kind::Center;
      if (free < 0 && ac == Align::Kind::Stretch) ac = Align::Kind::FlexStart;
      switch (ac) {
        case Align::Kind::Stretch:
          if (wraps || singleLineFixed || true) for (Line& l : lines) l.crossSize += free / n;
          break;
        case Align::Kind::FlexEnd: offset = free; break;
        case Align::Kind::Center: offset = free / 2; break;
        case Align::Kind::SpaceBetween: between = n > 1 ? free / (n - 1) : 0; break;
        case Align::Kind::SpaceAround: between = free / n; offset = between / 2; break;
        case Align::Kind::SpaceEvenly: between = free / (n + 1); offset = between; break;
        default: break;
      }
      double pos = offset;
      for (Line& l : lines) {
        l.crossPos = pos;
        pos += l.crossSize + gapCross + between;
      }
    }
    // Stretch.
    for (Line& line : lines) {
      for (Item* it : line.items) {
        if (AlignOf(*it) != Align::Kind::Stretch || !it->crossAuto || it->autoCrossStart || it->autoCrossEnd) continue;
        double target = line.crossSize - it->marginCrossStart - it->marginCrossEnd - it->padBorderCross;
        const BoxStyle& s = *it->box->style;
        const double maxV = Specified(*it, row ? s.maxHeight : s.maxWidth, false);
        if (Known(maxV)) target = std::min(target, maxV);
        target = std::max(target, it->crossSize);
        if (std::fabs(target - it->crossSize) > 1e-6) {
          LayoutItem(*it, it->mainSize, std::max(0.0, target));
          it->crossSize = target;
        }
      }
    }
    // ---- Main axis ----
    const double mainSize = Known(mainAvailable) ? mainAvailable : [&] {
      double widest = 0;
      for (Line& l : lines) {
        double u = gapMain * (l.items.size() > 1 ? l.items.size() - 1 : 0);
        for (Item* it : l.items) u += it->OuterMain();
        widest = std::max(widest, u);
      }
      return widest;
    }();
    for (Line& line : lines) {
      double used = gapMain * (line.items.size() > 1 ? line.items.size() - 1 : 0);
      int autoMargins = 0;
      for (Item* it : line.items) {
        used += it->OuterMain();
        autoMargins += (it->autoMainStart ? 1 : 0) + (it->autoMainEnd ? 1 : 0);
      }
      double free = mainSize - used;
      if (autoMargins > 0 && free > 0) {
        const double share = free / autoMargins;
        for (Item* it : line.items) {
          if (it->autoMainStart) it->marginMainStart = share;
          if (it->autoMainEnd) it->marginMainEnd = share;
        }
        free = 0;
      }
      Align::Kind jc = style.justifyContent.kind;
      if (jc == Align::Kind::Normal || jc == Align::Kind::Stretch) jc = Align::Kind::FlexStart;
      if (jc == Align::Kind::Start) jc = reversedMain ? Align::Kind::FlexEnd : Align::Kind::FlexStart;
      if (jc == Align::Kind::End) jc = reversedMain ? Align::Kind::FlexStart : Align::Kind::FlexEnd;
      if (jc == Align::Kind::Left) jc = (row ? (reversedMain != (style.direction == Direction::Rtl) ? Align::Kind::FlexEnd : Align::Kind::FlexStart) : Align::Kind::FlexStart);
      if (jc == Align::Kind::Right) jc = (row ? (reversedMain != (style.direction == Direction::Rtl) ? Align::Kind::FlexStart : Align::Kind::FlexEnd) : Align::Kind::FlexStart);
      const double n = static_cast<double>(line.items.size());
      if (free < 0 && jc == Align::Kind::SpaceBetween) jc = Align::Kind::FlexStart;
      if (free < 0 && (jc == Align::Kind::SpaceAround || jc == Align::Kind::SpaceEvenly)) jc = Align::Kind::Center;
      if (free < 0 && style.justifyContent.safe && (jc == Align::Kind::Center || jc == Align::Kind::FlexEnd)) jc = Align::Kind::FlexStart;
      double offset = 0, between = 0;
      switch (jc) {
        case Align::Kind::FlexEnd: offset = free; break;
        case Align::Kind::Center: offset = free / 2; break;
        case Align::Kind::SpaceBetween: between = n > 1 ? free / (n - 1) : 0; break;
        case Align::Kind::SpaceAround: between = free / n; offset = between / 2; break;
        case Align::Kind::SpaceEvenly: between = free / (n + 1); offset = between; break;
        default: break;
      }
      double pos = offset;
      for (Item* it : line.items) {
        it->posMain = pos;
        pos += it->OuterMain() + gapMain + between;
      }
      line.mainUsed = mainSize;
    }
    // ---- Cross alignment and final placement ----
    const double originX = container.ContentLeft(), originY = container.ContentTop();
    double firstBaseline = -1;
    for (size_t li = 0; li < lines.size(); ++li) {
      Line& line = lines[li];
      for (Item* it : line.items) {
        Align::Kind a = AlignOf(*it);
        const double outer = it->OuterCross();
        double freeCross = line.crossSize - outer;
        double cross = 0;
        if (it->autoCrossStart || it->autoCrossEnd) {
          if (freeCross > 0) {
            if (it->autoCrossStart && it->autoCrossEnd) { it->marginCrossStart += freeCross / 2; it->marginCrossEnd += freeCross / 2; cross = 0; }
            else if (it->autoCrossStart) it->marginCrossStart += freeCross;
            else it->marginCrossEnd += freeCross;
          }
        } else {
          switch (a) {
            case Align::Kind::FlexEnd: cross = freeCross; break;
            case Align::Kind::Center: cross = freeCross / 2; break;
            case Align::Kind::Baseline: cross = row ? it->baselineOffset : 0; break;
            case Align::Kind::LastBaseline: cross = freeCross; break;
            default: break;
          }
          if (freeCross < 0 && it->box->style->alignSelf.safe && (a == Align::Kind::Center || a == Align::Kind::FlexEnd)) cross = 0;
        }
        it->posCross = cross;
        // Physical coordinates of the margin box's start.
        double mainPhys = reversedMain ? mainSize - (it->posMain + it->OuterMain()) : it->posMain;
        const double lineCross = line.crossPos;
        double crossPhys = lineCross + it->posCross;
        const double crossSpace = containerCross;
        if (wrapReverse) crossPhys = crossSpace - (lineCross + it->posCross + outer);
        Box& b = *it->box;
        // The border box: after the start margin.
        double borderMain = mainPhys + it->marginMainStart;
        double borderCross = crossPhys + it->marginCrossStart;
        if (reversedMain) borderMain = mainPhys + it->marginMainEnd;
        if (wrapReverse) borderCross = crossPhys + it->marginCrossEnd;
        // margins for the geometry APIs are physical
        if (row) {
          b.margin.left = reversedMain ? it->marginMainEnd : it->marginMainStart;
          b.margin.right = reversedMain ? it->marginMainStart : it->marginMainEnd;
          b.margin.top = wrapReverse ? it->marginCrossEnd : it->marginCrossStart;
          b.margin.bottom = wrapReverse ? it->marginCrossStart : it->marginCrossEnd;
          b.x = originX + borderMain;
          b.y = originY + borderCross;
        } else {
          b.margin.top = reversedMain ? it->marginMainEnd : it->marginMainStart;
          b.margin.bottom = reversedMain ? it->marginMainStart : it->marginMainEnd;
          b.margin.left = wrapReverse ? it->marginCrossEnd : it->marginCrossStart;
          b.margin.right = wrapReverse ? it->marginCrossStart : it->marginCrossEnd;
          b.x = originX + borderCross;
          b.y = originY + borderMain;
        }
        b.x += b.shiftX;
        b.y += b.shiftY;
        if (firstBaseline < 0 && li == 0 && b.firstBaseline >= 0) firstBaseline = b.y - b.shiftY - originY + b.firstBaseline;
      }
    }
    contentHeightOut = row ? containerCross : mainSize;
    container.firstBaseline = firstBaseline >= 0 ? firstBaseline + originY : -1;
    container.baseline = container.firstBaseline;
  }
  double contentHeightOut = 0;
};

}  // namespace

void LayoutFlex(LayoutContext& lc, Box& container, double contentWidth, double heightBasis, double& contentHeight) {
  Flex flex(lc, container, contentWidth, heightBasis);
  std::vector<Box*> boxes;
  for (auto& c : container.children) {
    if (c->style->IsOutOfFlow()) {
      c->staticX = container.ContentLeft();
      c->staticY = container.ContentTop();
      continue;
    }
    boxes.push_back(c.get());
  }
  std::stable_sort(boxes.begin(), boxes.end(), [](const Box* a, const Box* b) { return a->style->order < b->style->order; });
  flex.items.resize(boxes.size());
  for (size_t i = 0; i < boxes.size(); ++i) {
    flex.items[i].box = boxes[i];
    flex.items[i].index = static_cast<int>(i);
  }
  const bool saved = true;
  (void)saved;
  flex.Run();
  contentHeight = flex.contentHeightOut;
  // Column containers with an auto height are as tall as their items need; rows as their lines.
}

void FlexContentSizes(LayoutContext& lc, Box& container, double& minContent, double& maxContent) {
  const BoxStyle& s = *container.style;
  const bool row = s.flexDirection == FlexDirection::Row || s.flexDirection == FlexDirection::RowReverse;
  const double gap = std::max(0.0, ResolveSize(s.columnGap, 0));
  double minSum = 0, maxSum = 0, minMax = 0, maxMax = 0;
  int count = 0;
  for (auto& c : container.children) {
    if (c->style->IsOutOfFlow()) continue;
    ComputeContentSizes(lc, *c);
    const BoxStyle& cs = *c->style;
    c->border = {cs.border[0], cs.border[1], cs.border[2], cs.border[3]};
    c->padding = {std::max(0.0, cs.padding[0].Resolve(0)), std::max(0.0, cs.padding[1].Resolve(0)), std::max(0.0, cs.padding[2].Resolve(0)), std::max(0.0, cs.padding[3].Resolve(0))};
    const double extras = c->border.Horizontal() + c->padding.Horizontal() + (cs.margin[1].IsAuto() ? 0 : cs.margin[1].Resolve(0)) + (cs.margin[3].IsAuto() ? 0 : cs.margin[3].Resolve(0));
    double mn = c->minContent, mx = c->maxContent;
    const double fixed = ResolveSize(cs.width, kNaN);
    if (Known(fixed)) {
      mn = mx = cs.boxSizing == BoxSizing::BorderBox ? std::max(0.0, fixed - c->border.Horizontal() - c->padding.Horizontal()) : fixed;
    }
    // A flex item's min-content contribution does not go below its flex base size when it cannot shrink.
    minSum += mn + extras;
    maxSum += mx + extras;
    minMax = std::max(minMax, mn + extras);
    maxMax = std::max(maxMax, mx + extras);
    ++count;
  }
  const double gaps = gap * (count > 1 ? count - 1 : 0);
  if (row) {
    maxContent = maxSum + gaps;
    minContent = (s.flexWrap == FlexWrap::Nowrap) ? minSum + gaps : minMax;
  } else {
    minContent = minMax;
    maxContent = maxMax;
  }
}

}  // namespace solar::layout
