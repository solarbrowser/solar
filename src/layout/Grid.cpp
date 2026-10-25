/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// Grid layout (https://www.w3.org/TR/css-grid-1/#layout-algorithm), for the horizontal writing mode.
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <tuple>

#include "Internal.h"
#include "solar/css/Syntax.h"

namespace solar::layout {

namespace {

using solar::css::ComponentValue;
using solar::css::ComponentValues;
using T = solar::css::Token::Type;

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

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// ---- Track definitions ----

struct Fn {
  enum class K { Auto, Length, Percent, Fr, MinContent, MaxContent, FitContent, Calc } k = K::Auto;
  double value = 0;       // px, percent, fr
  std::string calc;       // a calc() with a percentage
  bool Flexible() const { return k == K::Fr; }
  bool Intrinsic() const { return k == K::Auto || k == K::MinContent || k == K::MaxContent || k == K::FitContent; }
};

struct TrackDef {
  Fn min, max;
};

struct Segment {
  std::vector<TrackDef> tracks;
  std::vector<std::vector<std::string>> names{1};  // names[i] are the names of the line before track i; the last, of the line after the last
};

Segment Concat(Segment a, const Segment& b) {
  for (const std::string& n : b.names[0]) a.names.back().push_back(n);
  for (const TrackDef& t : b.tracks) a.tracks.push_back(t);
  for (size_t i = 1; i < b.names.size(); ++i) a.names.push_back(b.names[i]);
  return a;
}

Fn ParseFn(const ComponentValue& v, bool& ok) {
  Fn f;
  if (v.IsToken(T::Percentage)) { f.k = Fn::K::Percent; f.value = v.token.number; return f; }
  if (v.IsToken(T::Dimension)) {
    const std::string unit = Lower(v.token.value);
    if (unit == "fr") { f.k = Fn::K::Fr; f.value = v.token.number; return f; }
    f.k = Fn::K::Length;
    f.value = v.token.number;  // (px by the time it is computed)
    return f;
  }
  if (v.IsToken(T::Number)) { f.k = Fn::K::Length; f.value = v.token.number; return f; }
  if (v.IsIdent()) {
    const std::string n = Lower(v.token.value);
    if (n == "auto") return f;
    if (n == "min-content") { f.k = Fn::K::MinContent; return f; }
    if (n == "max-content") { f.k = Fn::K::MaxContent; return f; }
  }
  if (v.kind == ComponentValue::Kind::Function) {
    const std::string n = Lower(v.name);
    if (n == "calc" || n == "min" || n == "max" || n == "clamp") {
      f.k = Fn::K::Calc;
      f.calc = solar::css::Serialize(v);
      return f;
    }
  }
  ok = false;
  return f;
}

TrackDef ParseTrack(const ComponentValue& v, bool& ok) {
  TrackDef t;
  if (v.kind == ComponentValue::Kind::Function) {
    const std::string n = Lower(v.name);
    if (n == "minmax") {
      std::vector<ComponentValues> args(1);
      for (const ComponentValue& c : v.children) {
        if (c.IsToken(T::Comma)) args.emplace_back();
        else args.back().push_back(c);
      }
      if (args.size() == 2) {
        const ComponentValues a = solar::css::Trimmed(args[0]), b = solar::css::Trimmed(args[1]);
        if (a.size() == 1 && b.size() == 1) {
          t.min = ParseFn(a[0], ok);
          t.max = ParseFn(b[0], ok);
          return t;
        }
      }
      ok = false;
      return t;
    }
    if (n == "fit-content") {
      const ComponentValues arg = solar::css::Trimmed(v.children);
      t.min = Fn();
      if (arg.size() == 1) {
        Fn limit = ParseFn(arg[0], ok);
        t.max = limit;
        t.max.k = Fn::K::FitContent;
        t.max.calc = limit.k == Fn::K::Percent ? "%" + std::to_string(limit.value) : limit.k == Fn::K::Calc ? limit.calc : "";
        if (limit.k == Fn::K::Percent) t.max.value = limit.value;
        return t;
      }
      ok = false;
      return t;
    }
  }
  const Fn f = ParseFn(v, ok);
  if (f.k == Fn::K::Fr) {
    t.min = Fn();
    t.max = f;
  } else if (f.k == Fn::K::Auto) {
    t.min = t.max = f;
  } else {
    t.min = t.max = f;
  }
  return t;
}

struct Template {
  Segment before, after;
  bool hasAuto = false;
  bool autoFit = false;
  Segment repeated;
};

void ParseSegmentInto(const ComponentValues& values, Segment& out, bool& ok, bool allowAuto, Template* tmpl) {
  Segment current;
  bool afterRepeat = false;
  Segment* target = &current;
  for (const ComponentValue& v : values) {
    if (v.IsWhitespace()) continue;
    if (v.kind == ComponentValue::Kind::Block && v.open == T::LeftBracket) {
      for (const ComponentValue& c : v.children) if (c.IsIdent()) target->names.back().push_back(c.token.value);
      continue;
    }
    if (v.kind == ComponentValue::Kind::Function && Lower(v.name) == "repeat") {
      // repeat( count , tracks )
      size_t comma = 0;
      while (comma < v.children.size() && !v.children[comma].IsToken(T::Comma)) ++comma;
      if (comma >= v.children.size()) { ok = false; return; }
      ComponentValues countValues(v.children.begin(), v.children.begin() + comma);
      ComponentValues body(v.children.begin() + comma + 1, v.children.end());
      countValues = solar::css::Trimmed(countValues);
      Segment inner;
      ParseSegmentInto(body, inner, ok, false, nullptr);
      if (!ok) return;
      if (countValues.size() == 1 && countValues[0].IsIdent()) {
        const std::string mode = Lower(countValues[0].token.value);
        if ((mode == "auto-fill" || mode == "auto-fit") && allowAuto && tmpl && !tmpl->hasAuto) {
          tmpl->hasAuto = true;
          tmpl->autoFit = mode == "auto-fit";
          tmpl->repeated = inner;
          tmpl->before = current;
          current = Segment();
          afterRepeat = true;
          target = &current;
          continue;
        }
        ok = false;
        return;
      }
      if (countValues.size() == 1 && countValues[0].IsToken(T::Number)) {
        const int count = std::max(1, static_cast<int>(countValues[0].token.number));
        for (int i = 0; i < count && i < 10000; ++i) *target = Concat(*target, inner);
        continue;
      }
      ok = false;
      return;
    }
    bool good = true;
    TrackDef t = ParseTrack(v, good);
    if (!good) { ok = false; return; }
    target->tracks.push_back(t);
    target->names.emplace_back();
  }
  (void)afterRepeat;
  if (tmpl && tmpl->hasAuto) tmpl->after = current;
  out = current;
}

// ---- Item placement ----

struct LineSpec {
  enum class K { Auto, Line, Span } k = K::Auto;
  int n = 0;
  std::string name;
};

LineSpec ParseLine(const std::string& text) {
  LineSpec spec;
  const ComponentValues values = solar::css::Trimmed(solar::css::ParseComponentValues(text));
  bool span = false;
  for (const ComponentValue& v : values) {
    if (v.IsWhitespace()) continue;
    if (v.IsIdent()) {
      const std::string n = v.token.value;
      if (Lower(n) == "auto") return spec;
      if (Lower(n) == "span") { span = true; continue; }
      spec.name = n;
    } else if (v.IsToken(T::Number)) {
      spec.n = static_cast<int>(v.token.number);
    }
  }
  if (span) {
    spec.k = LineSpec::K::Span;
    if (spec.n <= 0) spec.n = 1;
  } else if (spec.n != 0 || !spec.name.empty()) {
    spec.k = LineSpec::K::Line;
    if (spec.n == 0) spec.n = 1;
  }
  return spec;
}

struct GridItem {
  Box* box = nullptr;
  LineSpec start[2], end[2];  // [0] rows, [1] columns
  int pos[2] = {0, 0};        // start line (0-based in the grid with implicit tracks before the explicit ones)
  int span[2] = {1, 1};
  bool definite[2] = {false, false};
  bool placed = false;
  // metrics
  double padBorder[2] = {0, 0};  // per axis [rows, cols]
  double marginStart[2] = {0, 0}, marginEnd[2] = {0, 0};
  bool autoMarginStart[2] = {false, false}, autoMarginEnd[2] = {false, false};
  double minContribution = 0, minContentContribution = 0, maxContentContribution = 0;
  double minimumExtras[2] = {0, 0};  // the margins, borders and padding in the minimum contribution
  bool automaticMinimum[2] = {false, false};  // the minimum contribution is the content size suggestion (which a fixed maximum of the tracks limits)
  // A subgrid does not size the tracks it spans by itself: the items inside it do, as if they were items of the grid it is in.
  bool subgridAxis[2] = {false, false};
  int onlyAxis = -1;  // such an item of a subgrid takes part in the sizing of the one axis only
  bool lanesAuto = false;  // in grid lanes, an item with no lane of its own: it counts for the lanes it could be in
  bool hasColumns = false;  // (for the rows) its columns are known too
  // Baseline alignment in an axis: what the item lines up on (0 none, 1 first, 2 last), at which side of the axis (the minimum side) its baseline is read from, how far from
  // the margin edge of that side the baseline is, and how much room that group leaves it before the margin edge.
  int baselineKind[2] = {0, 0};
  bool baselineMin[2] = {true, true};
  double baselineDistance[2] = {0, 0}, shim[2] = {0, 0}, baselineGroup[2] = {0, 0};  // (baselineGroup: how far the baseline of the group is from the side it reads from)
};

// The way the frame of a writing mode runs, physically: x (along the lines) and y (from block start to block end), as (right, down).
struct PhysDir {
  int x, y;
};
PhysDir FrameXDir(WritingMode m) { return m == WritingMode::HorizontalTb ? PhysDir{1, 0} : m == WritingMode::SidewaysLr ? PhysDir{0, -1} : PhysDir{0, 1}; }
PhysDir FrameYDir(WritingMode m) {
  switch (m) {
    case WritingMode::HorizontalTb: return {0, 1};
    case WritingMode::VerticalRl: case WritingMode::SidewaysRl: return {-1, 0};
    default: return {1, 0};
  }
}

struct Track {
  TrackDef def;
  double base = 0, limit = 0;
  double position = 0;     // start, in the container's content box
  bool implicit = false;
};

struct Grid {
  LayoutContext& lc;
  Box& container;
  const BoxStyle& style;
  double contentWidth, heightBasis;
  double gap[2] = {0, 0};  // [0] row gap (between rows), [1] column gap
  std::vector<Track> tracks[2];
  int explicitCount[2] = {0, 0};
  int implicitBefore[2] = {0, 0};
  std::vector<std::vector<std::string>> lineNames[2];
  std::vector<GridItem> items;
  std::map<std::string, std::array<int, 4>> areas;  // name -> rowStart, rowEnd, colStart, colEnd (explicit line numbers, 1-based)
  Template templates[2];

  bool sizingTracks_ = false;

  Grid(LayoutContext& l, Box& c, double w, double h) : lc(l), container(c), style(*c.style), contentWidth(w), heightBasis(h) {}

  // ---- Explicit grid ----
  void ParseTemplates(double columnsAvailable, double rowsAvailable) {
    const std::string* texts[2] = {&style.gridTemplateRows, &style.gridTemplateColumns};
    const double available[2] = {rowsAvailable, columnsAvailable};
    const double gaps[2] = {gap[0], gap[1]};
    for (int axis = 0; axis < 2; ++axis) {
      Template& tmpl = templates[axis];
      tmpl = Template();
      Segment result;
      std::string text = *texts[axis];
      const std::string lowered = Lower(text);
      if (lowered == "none" || lowered.empty() || lowered.rfind("subgrid", 0) == 0 || lowered.rfind("masonry", 0) == 0) {
        tracks[axis].clear();
        lineNames[axis].assign(1, {});
        continue;
      }
      bool ok = true;
      Segment flat;
      ParseSegmentInto(solar::css::ParseComponentValues(text), flat, ok, true, &tmpl);
      if (!ok) { tracks[axis].clear(); lineNames[axis].assign(1, {}); continue; }
      Segment full;
      if (tmpl.hasAuto) {
        // How many times the repeated tracks fit.
        double fixed = 0;
        const auto trackSize = [&](const TrackDef& t) {
          const Fn& f = t.max.k == Fn::K::Length || t.max.k == Fn::K::Percent ? t.max : t.min;
          if (f.k == Fn::K::Length) return f.value;
          if (f.k == Fn::K::Percent && Known(available[axis])) return available[axis] * f.value / 100;
          return 0.0;
        };
        for (const TrackDef& t : tmpl.before.tracks) fixed += trackSize(t);
        for (const TrackDef& t : tmpl.after.tracks) fixed += trackSize(t);
        double repeatSize = 0;
        for (const TrackDef& t : tmpl.repeated.tracks) repeatSize += trackSize(t);
        const double count0 = static_cast<double>(tmpl.before.tracks.size() + tmpl.after.tracks.size());
        const auto bound = [&](const Length& l) {
          double v = ResolveSize(l, kNaN);
          if (Known(v) && style.boxSizing == BoxSizing::BorderBox) v = std::max(0.0, v - (axis == 1 ? container.border.Horizontal() + container.padding.Horizontal() : container.border.Vertical() + container.padding.Vertical()));
          return v;
        };
        // (A size within the minimum and maximum there are.)
        double room0 = available[axis];
        if (Known(room0)) {
          const double hi = bound(axis == 1 ? style.maxWidth : style.maxHeight), lo = bound(axis == 1 ? style.minWidth : style.minHeight);
          if (Known(hi)) room0 = std::min(room0, hi);
          if (Known(lo)) room0 = std::max(room0, lo);
        }
        int count = 1;
        if (!Known(available[axis]) && repeatSize + gaps[axis] * tmpl.repeated.tracks.size() > 0) {
          // Without a size to fill, the largest number that stays within a definite maximum, else the smallest that reaches a definite minimum.
          const double maxSize = bound(axis == 1 ? style.maxWidth : style.maxHeight), minSize = bound(axis == 1 ? style.minWidth : style.minHeight);
          const double perRepeat = repeatSize + gaps[axis] * tmpl.repeated.tracks.size();
          const double count0 = static_cast<double>(tmpl.before.tracks.size() + tmpl.after.tracks.size());
          if (Known(maxSize)) {
            const double room = maxSize - fixed - gaps[axis] * count0;
            count = std::max(1, static_cast<int>(std::floor((room + gaps[axis]) / perRepeat + 1e-9)));
          } else if (Known(minSize)) {
            const double room = minSize - fixed - gaps[axis] * count0;
            count = std::max(1, static_cast<int>(std::ceil((room + gaps[axis]) / perRepeat - 1e-9)));
          }
        }
        if (Known(room0) && repeatSize + gaps[axis] * tmpl.repeated.tracks.size() > 0) {
          const double perRepeat = repeatSize + gaps[axis] * tmpl.repeated.tracks.size();
          const double room = room0 - fixed - gaps[axis] * count0;
          count = std::max(1, static_cast<int>(std::floor((room + gaps[axis]) / perRepeat + 1e-9)));
          if (room < perRepeat - 1e-9 && room + gaps[axis] < perRepeat) count = 1;
        }
        full = tmpl.before;
        for (int i = 0; i < count; ++i) full = Concat(full, tmpl.repeated);
        full = Concat(full, tmpl.after);
      } else {
        full = flat;
      }
      tracks[axis].clear();
      for (const TrackDef& d : full.tracks) {
        Track t;
        t.def = d;
        tracks[axis].push_back(t);
      }
      lineNames[axis] = full.names;
      if (lineNames[axis].size() < tracks[axis].size() + 1) lineNames[axis].resize(tracks[axis].size() + 1);
    }
    // Areas.
    areas.clear();
    const std::string& areaText = style.gridTemplateAreas;
    if (Lower(areaText) != "none" && !areaText.empty()) {
      std::vector<std::vector<std::string>> rows;
      for (const ComponentValue& v : solar::css::ParseComponentValues(areaText)) {
        if (!v.IsToken(T::String)) continue;
        std::vector<std::string> cells;
        std::string word;
        for (char c : v.token.value + " ") {
          if (c == ' ' || c == '\t' || c == '\n') { if (!word.empty()) cells.push_back(word); word.clear(); }
          else word += c;
        }
        rows.push_back(cells);
      }
      for (size_t r = 0; r < rows.size(); ++r) {
        for (size_t c = 0; c < rows[r].size(); ++c) {
          const std::string& name = rows[r][c];
          if (name == ".") continue;
          auto found = areas.find(name);
          if (found == areas.end()) areas[name] = {static_cast<int>(r) + 1, static_cast<int>(r) + 2, static_cast<int>(c) + 1, static_cast<int>(c) + 2};
          else {
            found->second[1] = std::max(found->second[1], static_cast<int>(r) + 2);
            found->second[3] = std::max(found->second[3], static_cast<int>(c) + 2);
          }
        }
      }
      size_t columns = 0;
      for (const auto& r : rows) columns = std::max(columns, r.size());
      // Implicit line names.
      for (int axis = 0; axis < 2; ++axis) {
        const size_t need = (axis == 0 ? rows.size() : columns) + 1;
        if (lineNames[axis].size() < need) lineNames[axis].resize(need);
        // The rows and columns the areas ask for beyond the template are sized as the implicit ones are: by grid-auto-rows and grid-auto-columns.
        std::vector<TrackDef> pattern;
        {
          const std::string& autoText = axis == 0 ? style.gridAutoRows : style.gridAutoColumns;
          bool ok = true;
          Segment seg;
          if (Lower(autoText) != "auto" && !autoText.empty()) ParseSegmentInto(solar::css::ParseComponentValues(autoText), seg, ok, false, nullptr);
          if (ok) pattern = seg.tracks;
        }
        const size_t listed = tracks[axis].size();
        while (tracks[axis].size() + 1 < need) {
          Track t;
          t.implicit = false;
          t.def.min = t.def.max = Fn();
          if (!pattern.empty()) t.def = pattern[(tracks[axis].size() - listed) % pattern.size()];
          tracks[axis].push_back(t);
        }
        if (lineNames[axis].size() < tracks[axis].size() + 1) lineNames[axis].resize(tracks[axis].size() + 1);
      }
      for (const auto& [name, a] : areas) {
        lineNames[0][a[0] - 1].push_back(name + "-start");
        lineNames[0][a[1] - 1].push_back(name + "-end");
        lineNames[1][a[2] - 1].push_back(name + "-start");
        lineNames[1][a[3] - 1].push_back(name + "-end");
      }
    }
    explicitCount[0] = static_cast<int>(tracks[0].size());
    explicitCount[1] = static_cast<int>(tracks[1].size());
  }

  int laneAxis = -1;  // grid lanes: the axis of the lanes
  std::vector<GridItem> virtualItems;  // the items inside the subgrids, for the sizing of the axis the subgrid shares

  // The items that size an axis: those that are not subgrids in it, and the items inside the subgrids that are.
  std::vector<GridItem*> SizingItems(int axis) {
    std::vector<GridItem*> out;
    for (GridItem& it : items) if (!it.subgridAxis[axis] && !(it.lanesAuto && axis == laneAxis)) out.push_back(&it);
    for (GridItem& it : virtualItems) if (it.onlyAxis == axis) out.push_back(&it);
    return out;
  }

  // Whether a box is a subgrid in the axis (its template says so and it is a grid container).
  bool IsSubgridIn(const Box& b, int axis) const {
    if (!IsGridDisplay(b.style->display)) return false;
    return Lower(axis == 0 ? b.style->gridTemplateRows : b.style->gridTemplateColumns).rfind("subgrid", 0) == 0;
  }

  // The items inside a subgrid `box` that spans `count` tracks of `axis` from `from`: made items of this grid. `startExtra` and `endExtra` are what is added at its
  // two ends by the subgrids around it (its own margin, border and padding are added here).
  void ExpandSubgrid(GridItem& root, Box& box, int axis, int from, int count, double startExtra, double endExtra, double columnWidth) {
    const BoxStyle& bs = *box.style;
    const int starts[2] = {0, 3}, ends[2] = {2, 1};
    const auto edge = [&](int side) {
      const double border = bs.border[side];
      const double padding = std::max(0.0, bs.padding[side].Resolve(columnWidth));
      const double margin = bs.margin[side].IsAuto() ? 0 : bs.margin[side].Resolve(columnWidth);
      return border + padding + margin;
    };
    const double S = startExtra + edge(starts[axis]), E = endExtra + edge(ends[axis]);
    // Its own grid, only for where its items are in the lines it spans.
    Grid inner(lc, box, kNaN, kNaN);
    box.subgrid[axis] = SubgridTracks();
    box.subgrid[axis].active = true;
    box.subgrid[axis].sizes.assign(static_cast<size_t>(count), 0.0);
    for (int i = from; i <= from + count; ++i) box.subgrid[axis].names.push_back(static_cast<size_t>(i) < lineNames[axis].size() ? lineNames[axis][i] : std::vector<std::string>());
    box.subgrid[1 - axis] = SubgridTracks();
    // The other axis too, when it is shared as well: the items inside are placed in its lines.
    const int other = 1 - axis;
    if (root.subgridAxis[other] && root.box == &box && IsSubgridIn(box, other)) {
      box.subgrid[other].active = true;
      box.subgrid[other].sizes.assign(static_cast<size_t>(root.span[other]), 0.0);
      for (int i = root.pos[other]; i <= root.pos[other] + root.span[other]; ++i) box.subgrid[other].names.push_back(static_cast<size_t>(i) < lineNames[other].size() ? lineNames[other][i] : std::vector<std::string>());
    }
    std::vector<Box*> boxes;
    for (auto& c : box.children) if (!c->style->IsOutOfFlow()) boxes.push_back(c.get());
    std::stable_sort(boxes.begin(), boxes.end(), [](const Box* a, const Box* b) { return a->style->order < b->style->order; });
    inner.items.resize(boxes.size());
    for (size_t i = 0; i < boxes.size(); ++i) inner.items[i].box = boxes[i];
    inner.gap[0] = inner.gap[1] = 0;
    inner.ParseTemplates(kNaN, kNaN);
    inner.ApplySubgrid();
    inner.Place();
    for (GridItem& si : inner.items) {
      const double itemStart = si.pos[axis] == 0 ? S : 0;
      const double itemEnd = si.pos[axis] + si.span[axis] == count ? E : 0;
      if (IsSubgridIn(*si.box, axis)) {
        ExpandSubgrid(root, *si.box, axis, from + si.pos[axis], si.span[axis], itemStart, itemEnd, columnWidth);
        continue;
      }
      GridItem v;
      v.box = si.box;
      v.onlyAxis = axis;
      v.pos[axis] = from + si.pos[axis];
      v.span[axis] = si.span[axis];
      ItemEdges(v, columnWidth);
      v.marginStart[axis] += itemStart + (si.pos[axis] > 0 ? inner.subgridGapDelta[axis] : 0);
      v.marginEnd[axis] += itemEnd + (si.pos[axis] + si.span[axis] < count ? inner.subgridGapDelta[axis] : 0);
      // The columns of such an item, for the heights it asks for: those of the subgrid when it shares them, and else the width of the subgrid.
      if (axis == 0 && root.box == &box && inner.subgridded[1]) {
        v.pos[1] = root.pos[1] + si.pos[1];
        v.span[1] = si.span[1];
        v.hasColumns = true;
      }
      virtualItems.push_back(v);
    }
  }

  // A subgrid in an axis has the tracks of the grid it is in, for the lines it spans: sizes and names come from there, less its own edges at the two ends.
  bool subgridded[2] = {false, false};
  void ApplySubgrid() {
    const std::string* texts[2] = {&style.gridTemplateRows, &style.gridTemplateColumns};
    for (int axis = 0; axis < 2; ++axis) {
      const SubgridTracks& from = container.subgrid[axis];
      if (!from.active || from.sizes.empty() || Lower(*texts[axis]).rfind("subgrid", 0) != 0) continue;
      subgridded[axis] = true;
      // Its own margin, border and padding at the start and the end of the axis belong to the first and the last track.
      const Edges& m = container.margin;
      const double startExtra = axis == 0 ? m.top + container.border.top + container.padding.top : m.left + container.border.left + container.padding.left;
      const double endExtra = axis == 0 ? m.bottom + container.border.bottom + container.padding.bottom : m.right + container.border.right + container.padding.right;
      tracks[axis].clear();
      for (size_t i = 0; i < from.sizes.size(); ++i) {
        Track t;
        double size = from.sizes[i];
        if (i == 0) size -= startExtra;
        if (i + 1 == from.sizes.size()) size -= endExtra;
        size = std::max(0.0, size);
        t.def.min = t.def.max = Fn();
        t.def.min.k = t.def.max.k = Fn::K::Length;
        t.def.min.value = t.def.max.value = size;
        tracks[axis].push_back(t);
      }
      lineNames[axis] = from.names;
      lineNames[axis].resize(tracks[axis].size() + 1);
      // The names it gives lines itself, one list for each line from the start.
      const solar::css::ComponentValues values = solar::css::ParseComponentValues(*texts[axis]);
      // (repeat() in it repeats its names: a number of times, or as many as the lines it has room for.)
      std::vector<std::vector<std::string>> written;
      const auto namesOf = [&](const ComponentValue& block) {
        std::vector<std::string> list;
        for (const ComponentValue& name : block.children) if (name.IsIdent()) list.push_back(name.token.value);
        return list;
      };
      size_t loose = 0;
      for (const ComponentValue& v : values) if (v.IsBlock(T::LeftBracket)) ++loose;
      for (const ComponentValue& v : values) {
        if (v.IsBlock(T::LeftBracket)) {
          written.push_back(namesOf(v));
        } else if (v.kind == ComponentValue::Kind::Function && Lower(v.name) == "repeat") {
          std::string count;
          std::vector<std::vector<std::string>> body;
          bool afterComma = false;
          for (const ComponentValue& c : v.children) {
            if (c.IsToken(T::Comma)) afterComma = true;
            else if (!afterComma && !c.IsWhitespace()) count = c.IsIdent() ? Lower(c.token.value) : std::to_string(static_cast<int>(c.token.number));
            else if (afterComma && c.IsBlock(T::LeftBracket)) body.push_back(namesOf(c));
          }
          if (body.empty()) continue;
          size_t reps = 1;
          if (count == "auto-fill") {
            const size_t room = lineNames[axis].size() > loose ? lineNames[axis].size() - loose : 0;
            reps = room / body.size();
          } else {
            reps = static_cast<size_t>(std::max(0, std::atoi(count.c_str())));
          }
          for (size_t r = 0; r < reps; ++r) for (const auto& b : body) written.push_back(b);
        }
      }
      for (size_t line = 0; line < written.size() && line < lineNames[axis].size(); ++line) {
        for (const std::string& n : written[line]) lineNames[axis][line].push_back(n);
      }
      // Its areas name its lines too.
      for (const auto& [name, a] : areas) {
        const int first = axis == 0 ? a[0] : a[2], last = axis == 0 ? a[1] : a[3];
        if (first >= 1 && static_cast<size_t>(first - 1) < lineNames[axis].size()) lineNames[axis][first - 1].push_back(name + "-start");
        if (last >= 1 && static_cast<size_t>(last - 1) < lineNames[axis].size()) lineNames[axis][last - 1].push_back(name + "-end");
      }
      explicitCount[axis] = static_cast<int>(tracks[axis].size());
      // The gap between its tracks is the one of the grid above; what its own gap is more or less than that is shared by the two items beside a line, as margin.
      const double own = axis == 0 ? ResolveSize(style.rowGap, kNaN) : ResolveSize(style.columnGap, kNaN);
      gap[axis] = from.gap;
      subgridGapDelta[axis] = Known(own) ? (own - from.gap) / 2 : 0;
    }
  }
  bool gapGiven[2] = {false, false};
  double subgridGapDelta[2] = {0, 0};

  // The 1-based line number of the nth line with a name, or 0.
  int FindNamedLine(int axis, const std::string& name, int n, bool isEnd) const {
    const auto search = [&](const std::string& wanted) {
      int seen = 0;
      const int count = n > 0 ? n : -n;
      if (n > 0) {
        for (size_t i = 0; i < lineNames[axis].size(); ++i) {
          for (const std::string& x : lineNames[axis][i]) if (x == wanted && ++seen == count) return static_cast<int>(i) + 1;
        }
      } else {
        for (size_t i = lineNames[axis].size(); i-- > 0;) {
          for (const std::string& x : lineNames[axis][i]) if (x == wanted && ++seen == count) return static_cast<int>(i) + 1;
        }
      }
      return 0;
    };
    int found = search(name);
    if (!found) found = search(name + (isEnd ? "-end" : "-start"));
    return found;
  }

  // ---- Resolving positions ----
  void ResolveLines(GridItem& it) {
    const BoxStyle& s = *it.box->style;
    const std::string* starts[2] = {&s.gridRowStart, &s.gridColumnStart};
    const std::string* ends[2] = {&s.gridRowEnd, &s.gridColumnEnd};
    for (int axis = 0; axis < 2; ++axis) {
      LineSpec a = ParseLine(*starts[axis]), b = ParseLine(*ends[axis]);
      it.start[axis] = a;
      it.end[axis] = b;
      it.definite[axis] = false;
      it.span[axis] = 1;
      const int explicitLines = explicitCount[axis] + 1;
      const auto lineNumber = [&](const LineSpec& spec, bool isEnd, int& out) {
        // 1-based line numbers, negative counting from the end of the explicit grid.
        if (!spec.name.empty()) {
          const int found = FindNamedLine(axis, spec.name, spec.n, isEnd);
          if (found) { out = found; return true; }
          // Not there: every implicit line has the name.
          out = spec.n > 0 ? explicitLines + spec.n : 1 + spec.n;
          return true;
        }
        out = spec.n > 0 ? spec.n : explicitLines + 1 + spec.n;
        return true;
      };
      const auto spanOf = [&](const LineSpec& spec) { return std::max(1, spec.n); };
      int sLine = 0, eLine = 0;
      if (a.k == LineSpec::K::Span && b.k == LineSpec::K::Span) b = LineSpec();  // both spans: the end is auto
      if (a.k == LineSpec::K::Line && b.k == LineSpec::K::Line) {
        lineNumber(a, false, sLine);
        lineNumber(b, true, eLine);
        if (sLine > eLine) std::swap(sLine, eLine);
        if (sLine == eLine) eLine = sLine + 1;
        it.pos[axis] = sLine - 1;
        it.span[axis] = eLine - sLine;
        it.definite[axis] = true;
      } else if (a.k == LineSpec::K::Line) {
        lineNumber(a, false, sLine);
        it.pos[axis] = sLine - 1;
        it.span[axis] = b.k == LineSpec::K::Span ? spanOf(b) : 1;
        if (b.k == LineSpec::K::Span && !b.name.empty()) {
          // span to the nth named line after the start
          int hit = 0, seen = 0;
          for (size_t i = sLine; i < lineNames[axis].size() && !hit; ++i) {
            for (const std::string& x : lineNames[axis][i]) if (x == b.name && ++seen == std::max(1, b.n)) hit = static_cast<int>(i) + 1;
          }
          if (hit) it.span[axis] = hit - sLine;
        }
        it.definite[axis] = true;
      } else if (b.k == LineSpec::K::Line) {
        lineNumber(b, true, eLine);
        it.span[axis] = a.k == LineSpec::K::Span ? spanOf(a) : 1;
        it.pos[axis] = eLine - 1 - it.span[axis];
        it.definite[axis] = true;
      } else {
        it.span[axis] = a.k == LineSpec::K::Span ? spanOf(a) : (b.k == LineSpec::K::Span ? spanOf(b) : 1);
        it.definite[axis] = false;
      }
      // An area named in grid-row-start and the like with only a name is covered by FindNamedLine (name-start / name-end).
    }
  }

  // ---- Auto placement ----
  std::vector<std::vector<char>> occupied;  // [row][column] with offsets applied
  int rows = 0, cols = 0;                   // size of the occupancy grid (implicit grid)

  bool Free(int r, int c, int rs, int cs) {
    for (int i = r; i < r + rs; ++i) {
      for (int j = c; j < c + cs; ++j) {
        if (i < rows && j < cols && occupied[i][j]) return false;
      }
    }
    return true;
  }
  void Grow(int r, int c) {
    rows = std::max(rows, r);
    cols = std::max(cols, c);
    occupied.resize(rows);
    for (auto& row : occupied) row.resize(cols, 0);
  }
  void Occupy(int r, int c, int rs, int cs) {
    Grow(r + rs, c + cs);
    for (int i = r; i < r + rs; ++i) for (int j = c; j < c + cs; ++j) occupied[i][j] = 1;
  }

  void Place() {
    for (GridItem& it : items) ResolveLines(it);
    // Negative positions put implicit tracks before the explicit ones.
    int minPos[2] = {0, 0};
    for (const GridItem& it : items) for (int a = 0; a < 2; ++a) if (it.definite[a]) minPos[a] = std::min(minPos[a], it.pos[a]);
    implicitBefore[0] = -minPos[0];
    implicitBefore[1] = -minPos[1];
    for (GridItem& it : items) for (int a = 0; a < 2; ++a) if (it.definite[a]) it.pos[a] += implicitBefore[a];
    const int explicitRows = explicitCount[0] + implicitBefore[0], explicitCols = explicitCount[1] + implicitBefore[1];
    rows = cols = 0;
    Grow(explicitRows, explicitCols);
    const std::string flow = Lower(style.gridAutoFlow);
    const bool columnFlow = flow.find("column") != std::string::npos;
    const bool dense = flow.find("dense") != std::string::npos;
    const int major = columnFlow ? 1 : 0, minor = columnFlow ? 0 : 1;
    // Step 1: both axes definite.
    for (GridItem& it : items) {
      if (it.definite[0] && it.definite[1]) {
        it.placed = true;
        Occupy(it.pos[0], it.pos[1], it.span[0], it.span[1]);
      }
    }
    // Step 2: definite in the major axis.
    std::map<int, int> rowCursor;  // per major line: where the last item of this step ended in the minor axis
    for (GridItem& it : items) {
      if (it.placed || !it.definite[major]) continue;
      int start = dense ? 0 : rowCursor[it.pos[major]];
      int p[2];
      p[major] = it.pos[major];
      for (;; ++start) {
        p[minor] = start;
        int r = p[0], c = p[1];
        if (Free(r, c, it.span[0], it.span[1])) break;
        if (start > 100000) break;
      }
      it.pos[minor] = start;
      it.placed = true;
      Occupy(it.pos[0], it.pos[1], it.span[0], it.span[1]);
      rowCursor[it.pos[major]] = start + it.span[minor];
    }
    // Step 3: the implicit minor axis is as wide as the explicit grid and the items in it need.
    int minorSize = columnFlow ? rows : cols;
    for (const GridItem& it : items) if (it.placed || it.definite[minor]) minorSize = std::max(minorSize, (it.definite[minor] ? it.pos[minor] : it.pos[minor]) + it.span[minor]);
    for (const GridItem& it : items) minorSize = std::max(minorSize, it.span[minor]);
    // Step 4: the rest.
    int cursorMajor = 0, cursorMinor = 0;
    for (GridItem& it : items) {
      if (it.placed) continue;
      if (dense) { cursorMajor = 0; cursorMinor = 0; }
      if (it.definite[minor]) {
        if (!dense) {
          if (it.pos[minor] < cursorMinor) ++cursorMajor;
          cursorMinor = it.pos[minor];
        }
        int m = dense ? 0 : cursorMajor;
        for (;; ++m) {
          int p[2];
          p[major] = m;
          p[minor] = it.pos[minor];
          if (Free(p[0], p[1], it.span[0], it.span[1])) break;
          if (m > 100000) break;
        }
        it.pos[major] = m;
        cursorMajor = m;
      } else {
        for (;;) {
          bool found = false;
          for (; cursorMinor + it.span[minor] <= minorSize; ++cursorMinor) {
            int p[2];
            p[major] = cursorMajor;
            p[minor] = cursorMinor;
            if (Free(p[0], p[1], it.span[0], it.span[1])) { found = true; break; }
          }
          if (found) break;
          ++cursorMajor;
          cursorMinor = 0;
          if (cursorMajor > 100000) break;
        }
        it.pos[major] = cursorMajor;
        it.pos[minor] = cursorMinor;
      }
      it.placed = true;
      Occupy(it.pos[0], it.pos[1], it.span[0], it.span[1]);
    }
    // The tracks: implicit ones before, the explicit, implicit after.
    for (int a = 0; a < 2; ++a) {
      int needed = a == 0 ? rows : cols;
      for (const GridItem& it : items) needed = std::max(needed, it.pos[a] + it.span[a]);
      BuildTracks(a, needed);
    }
  }

  // The tracks of an axis: the implicit ones before the explicit, the explicit, and the implicit ones after, up to `needed` in all.
  void BuildTracks(int a, int needed) {
    std::vector<Track> all;
    const std::string& autoText = a == 0 ? style.gridAutoRows : style.gridAutoColumns;
    std::vector<TrackDef> pattern;
    {
      bool ok = true;
      Segment seg;
      if (Lower(autoText) != "auto" && !autoText.empty()) ParseSegmentInto(solar::css::ParseComponentValues(autoText), seg, ok, false, nullptr);
      if (ok) pattern = seg.tracks;
      if (pattern.empty()) pattern.push_back(TrackDef());
    }
    const int total = std::max(needed, explicitCount[a] + implicitBefore[a]);
    for (int i = 0; i < implicitBefore[a]; ++i) {
      Track t;
      t.implicit = true;
      t.def = pattern[(pattern.size() - 1 - (i % pattern.size()))];
      all.push_back(t);
    }
    // (the implicit tracks before count backwards from the explicit start; the order is reversed below)
    std::reverse(all.begin(), all.end());
    for (const Track& t : tracks[a]) all.push_back(t);
    int implicitIndex = 0;
    while (static_cast<int>(all.size()) < total) {
      Track t;
      t.implicit = true;
      t.def = pattern[implicitIndex % pattern.size()];
      ++implicitIndex;
      all.push_back(t);
    }
    tracks[a] = all;
  }

  // ---- Items ----
  void ItemEdges(GridItem& it, double cb) {
    Box& b = *it.box;
    const BoxStyle& s = *b.style;
    b.border = {s.border[0], s.border[1], s.border[2], s.border[3]};
    b.padding = {std::max(0.0, s.padding[0].Resolve(cb)), std::max(0.0, s.padding[1].Resolve(cb)), std::max(0.0, s.padding[2].Resolve(cb)), std::max(0.0, s.padding[3].Resolve(cb))};
    Edges margin, autoMargin, border = b.border, padding = b.padding;
    const double* marginSides[4] = {&margin.top, &margin.right, &margin.bottom, &margin.left};
    double* autoSides[4] = {&autoMargin.top, &autoMargin.right, &autoMargin.bottom, &autoMargin.left};
    for (int i = 0; i < 4; ++i) {
      *autoSides[i] = s.margin[i].IsAuto() ? 1 : 0;
      *const_cast<double*>(marginSides[i]) = s.margin[i].IsAuto() ? 0 : s.margin[i].Resolve(cb);
    }
    // The edges are those of the item's own frame: the grid's are those of its frame.
    if (b.Mode() != container.Mode()) {
      const auto across = [&](const Edges& e) { return EdgesToFrame(container.Mode(), EdgesToPhysical(b.Mode(), e)); };
      margin = across(margin);
      autoMargin = across(autoMargin);
      border = across(border);
      padding = across(padding);
    }
    it.padBorder[0] = border.Vertical() + padding.Vertical();
    it.padBorder[1] = border.Horizontal() + padding.Horizontal();
    it.autoMarginStart[0] = autoMargin.top != 0;
    it.autoMarginEnd[0] = autoMargin.bottom != 0;
    it.autoMarginStart[1] = autoMargin.left != 0;
    it.autoMarginEnd[1] = autoMargin.right != 0;
    it.marginStart[0] = margin.top;
    it.marginEnd[0] = margin.bottom;
    it.marginStart[1] = margin.left;
    it.marginEnd[1] = margin.right;
  }

  // Whether the alignment of the item in an axis (its own, or that of the container's items) is safe.
  bool SafeAlign(const GridItem& it, int axis) const {
    const BoxStyle& s = *it.box->style;
    const Align& self = axis == 0 ? s.alignSelf : s.justifySelf;
    const Align& items = axis == 0 ? style.alignItems : style.justifyItems;
    return (self.kind == Align::Kind::Auto ? items : self).safe;
  }

  Align::Kind SelfAlign(const GridItem& it, int axis) const {
    const BoxStyle& s = *it.box->style;
    // axis 0: rows (align-*), axis 1: columns (justify-*)
    Align self = axis == 0 ? s.alignSelf : s.justifySelf;
    Align items = axis == 0 ? style.alignItems : style.justifyItems;
    Align::Kind k = self.kind == Align::Kind::Auto ? items.kind : self.kind;
    if (k == Align::Kind::Auto) k = Align::Kind::Normal;
    return k;
  }

  // ---- Baseline alignment (css-align-3 §9, css-grid-1 §6.6) ----
  // An item lines up on a baseline in an axis when it asks for it, spans one track there, and has no auto margins in it.
  int BaselineKindOf(const GridItem& it, int axis) const {
    if (it.span[axis] != 1 || it.autoMarginStart[axis] || it.autoMarginEnd[axis] || it.subgridAxis[axis] || it.onlyAxis >= 0) return 0;
    // An item whose size is a percentage of a track that is sized by its contents would size the track by what the track gives it: it does not line up on a baseline.
    const BoxStyle& s = *it.box->style;
    if ((axis == 0 ? s.height : s.width).IsPercentage() && static_cast<size_t>(it.pos[axis]) < tracks[axis].size()) {
      const TrackDef& def = tracks[axis][it.pos[axis]].def;
      const auto definite = [](const Fn& f) { return f.k == Fn::K::Length || f.k == Fn::K::Percent; };
      if (!definite(def.min) || !definite(def.max)) return 0;
    }
    const Align::Kind k = SelfAlign(it, axis);
    return k == Align::Kind::Baseline ? 1 : k == Align::Kind::LastBaseline ? 2 : 0;
  }

  // Where the baseline of each item that has one in the axis is, and the room the items of a group have to leave to line up on it. The group is the
  // items in one track that read their baseline from the same side.
  void ComputeBaselines(int axis, const std::function<double(const GridItem&)>& areaWidthOf, const std::function<double(const GridItem&)>& sizeAlong) {
    const WritingMode mode = container.Mode();
    const PhysDir along = axis == 1 ? FrameXDir(mode) : FrameYDir(mode);
    using Key = std::tuple<int, bool, int>;
    std::map<Key, double> widest;
    std::map<Key, const GridItem*> reference;  // the item of a group whose baseline is a real one: it has the writing mode that the others' synthesized baselines go by
    std::vector<GridItem*> members;
    std::vector<bool> blockAlongOf;
    for (GridItem& it : items) {
      it.baselineKind[axis] = BaselineKindOf(it, axis);
      it.shim[axis] = 0;
      if (!it.baselineKind[axis]) continue;
      Box& b = *it.box;
      const bool orthogonal = Orthogonal(mode, b.Mode());
      const bool blockAlong = (axis == 0) != orthogonal;  // the block axis of the item is along the axis
      // Which side of the axis the start of the item (its block start or its inline start, whichever is along it) is at.
      PhysDir start;
      if (blockAlong) start = FrameYDir(b.Mode());
      else start = b.style->direction == Direction::Ltr ? FrameXDir(b.Mode()) : PhysDir{-FrameXDir(b.Mode()).x, -FrameXDir(b.Mode()).y};
      const bool startAtMin = start.x * along.x + start.y * along.y > 0;
      const bool first = it.baselineKind[axis] == 1;
      it.baselineMin[axis] = first ? startAtMin : !startAtMin;
      const Key key{it.pos[axis], it.baselineMin[axis], it.baselineKind[axis]};
      if (blockAlong) {
        // (Laid out for its baselines, in its own frame.)
        if (areaWidthOf) LayoutItemAt(it, areaWidthOf(it), kNaN);
        if (!reference.count(key)) reference[key] = &it;
      }
      members.push_back(&it);
      blockAlongOf.push_back(blockAlong);
    }
    for (size_t m = 0; m < members.size(); ++m) {
      GridItem& it = *members[m];
      Box& b = *it.box;
      const bool first = it.baselineKind[axis] == 1;
      const bool atMin = it.baselineMin[axis];
      const Key key{it.pos[axis], atMin, it.baselineKind[axis]};
      // How far the baseline is from the margin edge at the side of the group.
      double distance;
      if (blockAlongOf[m]) {
        const double blockSize = b.ownHeight;
        distance = first ? (b.ownFirstBaseline >= 0 ? b.ownFirstBaseline : blockSize) : (b.ownLastBaseline >= 0 ? blockSize - b.ownLastBaseline : 0);
        distance += atMin ? it.marginStart[axis] : it.marginEnd[axis];
      } else {
        // Synthesized: the border box's edge that is under the text of the writing mode the others in the group go by.
        distance = atMin ? it.marginStart[axis] : it.marginEnd[axis];
        // (A group with no baseline of its own goes by the writing mode of the grid.)
        const auto ref = reference.find(key);
        const WritingMode refMode = ref != reference.end() ? ref->second->box->Mode() : mode;
        const PhysDir under = refMode == WritingMode::HorizontalTb ? PhysDir{0, 1} : refMode == WritingMode::SidewaysLr ? PhysDir{1, 0} : PhysDir{-1, 0};
        const int toward = under.x * along.x + under.y * along.y;
        if (toward != 0 && (toward < 0) != atMin) distance += std::max(0.0, sizeAlong(it));
      }
      it.baselineDistance[axis] = distance;
      widest[key] = std::max(widest[key], distance);
    }
    for (GridItem* it : members) {
      it->baselineGroup[axis] = widest[Key{it->pos[axis], it->baselineMin[axis], it->baselineKind[axis]}];
      it->shim[axis] = it->baselineGroup[axis] - it->baselineDistance[axis];
    }
  }

  // An item in another writing mode than the grid's has its inline size along the rows and its block size along the columns.
  bool Orth(const GridItem& it) const { return Orthogonal(container.Mode(), it.box->Mode()); }
  // The size, minimum and maximum of the item along an axis of the grid (the properties are those of the frame of the item).
  const Length& SizeLen(const GridItem& it, int axis) const { const BoxStyle& s = *it.box->style; return (axis == 0) != Orth(it) ? s.height : s.width; }
  const Length& MinLen(const GridItem& it, int axis) const { const BoxStyle& s = *it.box->style; return (axis == 0) != Orth(it) ? s.minHeight : s.minWidth; }
  const Length& MaxLen(const GridItem& it, int axis) const { const BoxStyle& s = *it.box->style; return (axis == 0) != Orth(it) ? s.maxHeight : s.maxWidth; }

  // Whether the item's size in an axis is a keyword for the size of its contents (min-content, max-content, fit-content): it is not stretched.
  bool ContentSized(const GridItem& it, int axis) const {
    const Length::Kind k = SizeLen(it, axis).kind;
    return k == Length::Kind::MinContent || k == Length::Kind::MaxContent || k == Length::Kind::FitContent;
  }

  // The size property of the item in an axis resolved to a content-box length, or NaN.
  double SpecifiedSize(const GridItem& it, int axis, double basis) const {
    const BoxStyle& s = *it.box->style;
    const Length& l = SizeLen(it, axis);
    // (A percentage is of the grid area, which is not known while the tracks are sized: it is as auto.)
    if (sizingTracks_ && l.IsPercentage() && !it.box->replaced) return kNaN;
    double v = ResolveSize(l, basis);
    if (!Known(v)) return kNaN;
    return s.boxSizing == BoxSizing::BorderBox ? std::max(0.0, v - it.padBorder[axis]) : v;
  }

  // The contributions of an item to the tracks it spans in the column axis.
  void ColumnContributions(GridItem& it, double columnBasis) {
    Box& b = *it.box;
    const BoxStyle& s = *b.style;
    if (style.containSizeInline) {  // size containment: the items count for nothing in the sizes of the tracks
      it.minContribution = it.minContentContribution = it.maxContentContribution = 0;
      return;
    }
    struct Sizing {
      bool& flag;
      explicit Sizing(bool& f) : flag(f) { flag = true; }
      ~Sizing() { flag = false; }
    } sizing(sizingTracks_);
    ComputeContentSizes(lc, b);
    const bool orth = Orth(it);
    double mn = b.minContent, mx = b.maxContent;
    // (The percentage width of a replaced box is of the area, which is not known: its contribution is that of its natural size.)
    const bool compressible = SizeLen(it, 1).IsPercentage() && b.replaced && !style.gridLanes;
    const double specified = compressible ? kNaN : SpecifiedSize(it, 1, columnBasis);
    if (orth && !Known(specified)) {
      // The columns are the block axis of the item: its size there is what it comes to when laid out as wide (along the rows) as it would be.
      mn = mx = MeasuredAcross(it, columnBasis);
    }
    if (Known(specified)) mn = mx = specified;
    else if (compressible) mn = 0;  // (a replaced box with a percentage width can be squeezed to nothing)
    const Length& maxLen = MaxLen(it, 1);
    const double maxV = maxLen.IsPercentage() ? kNaN : ResolveSize(maxLen, columnBasis);
    if (Known(maxV)) { const double m = s.boxSizing == BoxSizing::BorderBox ? std::max(0.0, maxV - it.padBorder[1]) : maxV; mn = std::min(mn, m); mx = std::min(mx, m); }
    const double minV = MinLen(it, 1).IsPercentage() ? kNaN : ResolveSize(MinLen(it, 1), columnBasis);
    if (Known(minV)) { const double m = s.boxSizing == BoxSizing::BorderBox ? std::max(0.0, minV - it.padBorder[1]) : minV; mn = std::max(mn, m); mx = std::max(mx, m); }
    const double extras = it.padBorder[1] + it.marginStart[1] + it.marginEnd[1];
    it.minContentContribution = mn + extras;
    it.maxContentContribution = mx + extras;
    // The automatic minimum size.
    double minimum;
    const bool scroller = (orth ? s.overflowY : s.overflowX) != Overflow::Visible && (orth ? s.overflowY : s.overflowX) != Overflow::Clip;
    // An item with a preferred size of its own is as small as that size lets it be (its minimum contribution is its min-content one).
    if (Known(specified)) minimum = mn;
    else if (Known(minV)) minimum = std::max(0.0, minV);
    else if (compressible) minimum = 0;  // (a replaced box with a percentage width is compressible)
    // (the content size suggestion is the min-content contribution of the box, which its own width gives)
    else if (MinLen(it, 1).IsAuto() && !scroller) { minimum = Known(specified) ? std::min(specified, mn) : (orth ? mn : b.minContent); it.automaticMinimum[1] = !Known(specified) && !ContentSized(it, 1); it.minimumExtras[1] = extras; }
    else minimum = 0;
    it.minContribution = minimum + extras;
  }

  static constexpr double g_noRoom = -1;
  // The size across the columns of an item in another writing mode than the grid's, which is its block size: it is laid out as long (along the rows) as its
  // size says, or as its contents would have it within the height there is.
  double MeasuredAcross(GridItem& it, double columnBasis) {
    Box& b = *it.box;
    double inlineSize = SpecifiedSize(it, 0, heightBasis);
    // Rows of fixed sizes give the room it has along them.
    double fixedRoom = g_noRoom;
    {
      double sum = gap[0] * (it.span[0] - 1);
      bool fixed = it.pos[0] >= 0 && static_cast<size_t>(it.pos[0] + it.span[0]) <= tracks[0].size();
      for (int i = it.pos[0]; fixed && i < it.pos[0] + it.span[0]; ++i) {
        const TrackDef& d = tracks[0][i].def;
        if (d.min.k == Fn::K::Length && d.max.k == Fn::K::Length) sum += d.max.value;
        else fixed = false;
      }
      if (fixed) fixedRoom = sum;
    }
    if (!Known(inlineSize) && fixedRoom != g_noRoom && !ContentSized(it, 0) && (SelfAlign(it, 0) == Align::Kind::Normal || SelfAlign(it, 0) == Align::Kind::Stretch)) {
      inlineSize = std::max(0.0, fixedRoom - it.marginStart[0] - it.marginEnd[0] - it.padBorder[0]);
    }
    if (!Known(inlineSize)) {
      double room = Known(heightBasis) ? std::max(0.0, heightBasis - it.marginStart[0] - it.marginEnd[0] - it.padBorder[0]) : kInf;
      inlineSize = std::min(std::max(b.minContent, room), b.maxContent);
    }
    lc.forceWidth = kNaN;
    lc.forceHeight = inlineSize;
    lc.cbX = 0;
    lc.boxY = 0;
    LayoutBlockLevel(lc, b, columnBasis, heightBasis, true);
    return std::max(0.0, b.width - it.padBorder[1]);
  }

  // The contributions to rows: the item laid out at the width its columns give it.
  void RowContributions(GridItem& it, double areaWidth) {
    Box& b = *it.box;
    const BoxStyle& s = *b.style;
    if (style.containSizeBlock) {
      it.minContribution = it.minContentContribution = it.maxContentContribution = 0;
      return;
    }
    const double extras = it.padBorder[0] + it.marginStart[0] + it.marginEnd[0];
    struct Sizing {
      bool& flag;
      explicit Sizing(bool& f) : flag(f) { flag = true; }
      ~Sizing() { flag = false; }
    } sizing(sizingTracks_);
    double height;
    const bool orth = Orth(it);
    const double specified = SpecifiedSize(it, 0, heightBasis);
    if (orth) {
      // The rows are the inline axis of the item: the contributions are the sizes of its contents.
      ComputeContentSizes(lc, b);
      height = b.maxContent;
      double smallest = b.minContent;
      if (Known(specified)) height = smallest = specified;
      it.minContentContribution = smallest + extras;
      it.maxContentContribution = height + extras;
      double minimum;
      const double minV = MinLen(it, 0).IsPercentage() ? kNaN : ResolveSize(MinLen(it, 0), heightBasis);
      const bool scroller = s.overflowX != Overflow::Visible && s.overflowX != Overflow::Clip;
      if (Known(specified)) minimum = Known(minV) ? std::max(specified, minV) : specified;
      else if (Known(minV)) minimum = minV;
      else if (MinLen(it, 0).IsAuto() && !scroller) { minimum = smallest; it.automaticMinimum[0] = !ContentSized(it, 0); it.minimumExtras[0] = extras; }
      else minimum = 0;
      it.minContribution = minimum + extras;
      const double maxV = MaxLen(it, 0).IsPercentage() ? kNaN : ResolveSize(MaxLen(it, 0), heightBasis);
      if (Known(maxV)) { it.minContentContribution = std::min(it.minContentContribution, maxV + extras); it.maxContentContribution = std::min(it.maxContentContribution, maxV + extras); }
      return;
    }
    if (Known(specified)) {
      height = specified;
    } else {
      LayoutItemAt(it, areaWidth, kNaN);
      height = b.height - it.padBorder[0];
    }
    it.minContentContribution = it.maxContentContribution = height + extras;
    double minimum;
    const double minV = s.minHeight.IsPercentage() ? kNaN : ResolveSize(s.minHeight, heightBasis);
    const bool scroller = s.overflowY != Overflow::Visible && s.overflowY != Overflow::Clip;
    if (Known(specified)) minimum = Known(minV) ? std::max(specified, minV) : specified;  // (a preferred size of its own: as small as that)
    else if (Known(minV)) minimum = minV;
    else if (s.minHeight.IsAuto() && !scroller) { minimum = Known(specified) ? std::min(specified, height) : height; it.automaticMinimum[0] = !Known(specified) && !ContentSized(it, 0); it.minimumExtras[0] = extras; }
    else minimum = 0;
    it.minContribution = minimum + extras;
    const double maxV = s.maxHeight.IsPercentage() ? kNaN : ResolveSize(s.maxHeight, heightBasis);
    if (Known(maxV)) { it.minContentContribution = std::min(it.minContentContribution, maxV + extras); it.maxContentContribution = std::min(it.maxContentContribution, maxV + extras); }
  }

  // Lays the item out in a grid area of the given width (the content width its alignment gives it), to measure it.
  void LayoutItemAt(GridItem& it, double areaWidth, double forceHeight) {
    Box& b = *it.box;
    const BoxStyle& s = *b.style;
    const Align::Kind jk = SelfAlign(it, 1);
    double forceWidth = kNaN;
    const double specified = SpecifiedSize(it, 1, areaWidth);
    if (!Known(specified) && !ContentSized(it, 1) && !it.autoMarginStart[1] && !it.autoMarginEnd[1] && (jk == Align::Kind::Normal || jk == Align::Kind::Stretch) && !b.replaced) {
      forceWidth = std::max(0.0, areaWidth - it.marginStart[1] - it.marginEnd[1] - it.padBorder[1]);
    }
    lc.forceWidth = forceWidth;
    lc.forceHeight = forceHeight;
    lc.cbX = 0;
    lc.boxY = 0;
    (void)s;
    LayoutBlockLevel(lc, b, areaWidth, sizingTracks_ && !Orthogonal(container.Mode(), b.Mode()) ? kNaN : heightBasis, true);
  }

  // ---- Track sizing (css-grid-1 §12) ----
  double Resolve(const Fn& f, double basis) const {
    switch (f.k) {
      case Fn::K::Length: return f.value;
      case Fn::K::Percent: return Known(basis) ? basis * f.value / 100 : kNaN;
      case Fn::K::Calc: {
        if (!Known(basis)) return kNaN;
        double v = 0;
        return EvaluateCalc(f.calc, basis, v) ? v : kNaN;
      }
      default: return kNaN;
    }
  }

  // Sizes the tracks of an axis. `available` is the content box size in the axis, NaN if it is not known; `useMax` picks max-content for
  // indefinite sizes (otherwise min-content). The contributions of the items are those last computed for the axis.
  void SizeTracks(int axis, double available, bool useMax) {
    std::vector<Track>& ts = tracks[axis];
    const double g = gap[axis];
    const int n = static_cast<int>(ts.size());
    if (n == 0) return;
    // Initialise.
    for (Track& t : ts) {
      const double minV = Resolve(t.def.min, available);
      const double maxV = Resolve(t.def.max, available);
      t.base = Known(minV) ? minV : 0;
      if (t.def.max.k == Fn::K::Fr) t.limit = t.base;  // a flexible track's limit is its base until it is flexed
      else t.limit = Known(maxV) ? maxV : kInf;
      if (t.def.max.k == Fn::K::FitContent) t.limit = kInf;
      if (t.limit < t.base) t.limit = t.base;
    }
    // Items by span, those that cross no flexible track first for the intrinsic sizes.
    std::vector<GridItem*> order = SizingItems(axis);
    std::stable_sort(order.begin(), order.end(), [&](const GridItem* a, const GridItem* b) { return a->span[axis] < b->span[axis]; });
    const auto spansFlexible = [&](const GridItem& it) {
      for (int i = it.pos[axis]; i < it.pos[axis] + it.span[axis]; ++i) if (ts[i].def.max.k == Fn::K::Fr) return true;
      return false;
    };
    // Distributes `extra` over the tracks given, equally, never past their limits when `limited`.
    const auto distribute = [&](double extra, std::vector<int>& which, bool toBase, bool limited) {
      if (extra <= 0 || which.empty()) return;
      std::vector<double> share(which.size(), 0);
      double remaining = extra;
      std::vector<bool> frozen(which.size(), false);
      for (int guard = 0; guard < 100 && remaining > 1e-9; ++guard) {
        int free = 0;
        for (size_t i = 0; i < which.size(); ++i) if (!frozen[i]) ++free;
        if (!free) break;
        const double part = remaining / free;
        double used = 0;
        for (size_t i = 0; i < which.size(); ++i) {
          if (frozen[i]) continue;
          Track& t = ts[which[i]];
          double room = kInf;
          if (limited && toBase) room = t.limit - (t.base + share[i]);
          double give = std::min(part, std::max(0.0, room));
          if (give < part) frozen[i] = true;
          share[i] += give;
          used += give;
        }
        remaining -= used;
        if (used < 1e-12) break;
      }
      // If the space did not all fit under limits, the rest goes over them.
      if (remaining > 1e-9 && !which.empty()) {
        for (size_t i = 0; i < which.size(); ++i) share[i] += remaining / which.size();
      }
      for (size_t i = 0; i < which.size(); ++i) {
        Track& t = ts[which[i]];
        if (toBase) {
          t.base += share[i];
          if (t.limit < t.base) t.limit = t.base;
        } else {
          t.limit = (std::isinf(t.limit) ? t.base : t.limit) + share[i];
        }
      }
    };
    // 12.5: intrinsic minimums, then maximums, by increasing span.
    std::vector<int> levels;
    for (GridItem* it : order) if (levels.empty() || levels.back() != it->span[axis]) levels.push_back(it->span[axis]);
    // (Each span in turn: the minimums, then the maximums, before the next span: what the items of one span make of the limits is seen by the ones that follow.)
    for (const int level : levels) for (int phase = 0; phase < 4; ++phase) {
      for (GridItem* it : order) {
        if (it->span[axis] != level) continue;
        // (An item in one track counts for it whatever the track; one that spans several and crosses a flexible one is dealt with at the end.)
        if (it->span[axis] > 1 && spansFlexible(*it) && phase != 3) continue;
        if (phase == 3 && !(it->span[axis] > 1 && spansFlexible(*it))) continue;
        std::vector<int> intrinsicMin, intrinsicMax;
        double spanned = g * (it->span[axis] - 1);
        for (int i = it->pos[axis]; i < it->pos[axis] + it->span[axis]; ++i) {
          spanned += ts[i].base;
          const Fn& mn = ts[i].def.min;
          const Fn& mx = ts[i].def.max;
          if (mn.Intrinsic()) intrinsicMin.push_back(i);
          if (mx.Intrinsic()) intrinsicMax.push_back(i);
        }
        if (phase == 0) {
          // tracks with an auto min: the item's minimum contribution; min-content: min-content; max-content: max-content
          for (int pass = 0; pass < 3; ++pass) {
            std::vector<int> which;
            double contribution = 0;
            for (int i : intrinsicMin) {
              const Fn::K k = ts[i].def.min.k;
              if (pass == 0 && k == Fn::K::Auto) {
                which.push_back(i);
                contribution = it->minContribution;
                // (The content size suggestion of an item in tracks that all have a fixed maximum does not go past the area they make.)
                if (it->automaticMinimum[axis]) {
                  double area = g * (it->span[axis] - 1);
                  bool fixed = true;
                  for (int j = it->pos[axis]; j < it->pos[axis] + it->span[axis]; ++j) {
                    const Fn::K mk = ts[j].def.max.k;
                    if ((mk != Fn::K::Length && mk != Fn::K::Percent) || std::isinf(ts[j].limit)) fixed = false;
                    area += ts[j].limit;
                  }
                  if (fixed) contribution = std::min(contribution, std::max(area, it->minimumExtras[axis]));
                }
              }
              else if (pass == 1 && k == Fn::K::MinContent) { which.push_back(i); contribution = it->minContentContribution; }
              else if (pass == 2 && k == Fn::K::MaxContent) { which.push_back(i); contribution = it->maxContentContribution; }
            }
            if (which.empty()) continue;
            double current = g * (it->span[axis] - 1);
            for (int i = it->pos[axis]; i < it->pos[axis] + it->span[axis]; ++i) current += ts[i].base;
            distribute(contribution - current, which, true, true);
          }
        } else if (phase == 2) {
          // maximums: min-content max → min-content; max-content max (and auto) → max-content
          std::vector<int> which;
          for (int i : intrinsicMax) which.push_back(i);
          if (which.empty()) continue;
          if (it->span[axis] == 1) {
            // One track: its growth limit is the largest of what the items in it want (from nothing, not from its base).
            Track& t = ts[it->pos[axis]];
            const double want = t.def.max.k == Fn::K::MinContent ? it->minContentContribution : it->maxContentContribution;
            t.limit = std::isinf(t.limit) ? want : std::max(t.limit, want);
            if (t.limit < t.base) t.limit = t.base;
            continue;
          }
          double current = g * (it->span[axis] - 1);
          for (int i = it->pos[axis]; i < it->pos[axis] + it->span[axis]; ++i) current += std::isinf(ts[i].limit) ? ts[i].base : ts[i].limit;
          const bool anyMin = std::any_of(which.begin(), which.end(), [&](int i) { return ts[i].def.max.k == Fn::K::MinContent; });
          const double contribution = anyMin ? it->minContentContribution : it->maxContentContribution;
          const double extra = contribution - current;
          if (extra > 0) {
            // tracks with an infinite limit take the base as their limit first
            for (int i : which) if (std::isinf(ts[i].limit)) ts[i].limit = ts[i].base;
            current = g * (it->span[axis] - 1);
            for (int i = it->pos[axis]; i < it->pos[axis] + it->span[axis]; ++i) current += ts[i].limit;
            distribute(contribution - current, which, false, false);
          }
        } else if (phase == 3) {
          // Items crossing flexible tracks: what they need beyond the tracks they span goes to the flexible ones.
          std::vector<int> flexible;
          double current = g * (it->span[axis] - 1);
          for (int i = it->pos[axis]; i < it->pos[axis] + it->span[axis]; ++i) {
            current += ts[i].base;
            if (ts[i].def.max.k == Fn::K::Fr) flexible.push_back(i);
          }
          (void)current;
          (void)flexible;
        }
      }
      if (phase == 1) {
        // 12.5 step 3: a track whose limit is still infinite takes its base.
      }
    }
    for (Track& t : ts) {
      if (std::isinf(t.limit) && t.def.max.k != Fn::K::Fr) t.limit = t.base;
      if (t.def.max.k == Fn::K::FitContent) {
        const double cap = t.def.max.calc.empty() ? t.def.max.value : t.def.max.value;
        double limitValue = kNaN;
        if (t.def.max.calc.empty()) limitValue = t.def.max.value;
        else if (t.def.max.calc[0] == '%') { if (Known(available)) limitValue = available * t.def.max.value / 100; }
        else {
          Fn expression = t.def.max;
          expression.k = Fn::K::Calc;
          limitValue = Resolve(expression, available);
        }
        if (Known(limitValue)) { t.limit = std::max(t.base, std::min(t.limit, std::max(limitValue, t.base))); (void)cap; }
      }
      if (t.limit < t.base) t.limit = t.base;
    }
    // Maximize: with a definite size the free space grows the tracks up to their limits.
    const auto sumTracks = [&]() {
      double sum = g * (n - 1);
      for (Track& t : ts) sum += t.base;
      return sum;
    };
    if (Known(available)) {
      double free = available - sumTracks();
      if (free > 0) {
        std::vector<int> which;
        for (int i = 0; i < n; ++i) which.push_back(i);
        // equally, up to the limits
        for (int guard = 0; guard < 50 && free > 1e-9; ++guard) {
          int open = 0;
          for (int i : which) if (ts[i].base < ts[i].limit - 1e-9) ++open;
          if (!open) break;
          const double part = free / open;
          double used = 0;
          for (int i : which) {
            if (ts[i].base >= ts[i].limit - 1e-9) continue;
            const double give = std::min(part, ts[i].limit - ts[i].base);
            ts[i].base += give;
            used += give;
          }
          free -= used;
          if (used < 1e-12) break;
        }
      }
    } else if (useMax) {
      for (Track& t : ts) if (t.def.max.k != Fn::K::Fr) t.base = std::max(t.base, std::isinf(t.limit) ? t.base : t.limit);
    }
    // Flexible tracks.
    double flexSum = 0;
    for (const Track& t : ts) if (t.def.max.k == Fn::K::Fr) flexSum += t.def.max.value;
    if (flexSum > 0) {
      double frSize = 0;
      if (Known(available)) {
        // The leftover space after the inflexible tracks, shared by the factors; tracks that would be below their base stop flexing.
        std::vector<bool> inflexible(n, false);
        for (int guard = 0; guard < n + 2; ++guard) {
          double leftover = available - g * (n - 1);
          double factors = 0;
          for (int i = 0; i < n; ++i) {
            if (ts[i].def.max.k == Fn::K::Fr && !inflexible[i]) factors += ts[i].def.max.value;
            else leftover -= ts[i].base;
          }
          if (factors < 1) factors = 1;
          frSize = std::max(0.0, leftover) / factors;
          bool changed = false;
          for (int i = 0; i < n; ++i) {
            if (ts[i].def.max.k == Fn::K::Fr && !inflexible[i] && frSize * ts[i].def.max.value < ts[i].base) { inflexible[i] = true; changed = true; }
          }
          if (!changed) break;
        }
      } else {
        // Indefinite: each flexible track is as big as its items want; the fr is the largest of base/factor.
        for (GridItem* itp : SizingItems(axis)) {
          GridItem& it = *itp;
          double factors = 0, other = g * (it.span[axis] - 1);
          for (int i = it.pos[axis]; i < it.pos[axis] + it.span[axis]; ++i) {
            if (ts[i].def.max.k == Fn::K::Fr) factors += ts[i].def.max.value;
            else other += ts[i].base;
          }
          if (factors > 0) frSize = std::max(frSize, (std::max(useMax ? it.maxContentContribution : it.minContentContribution, 0.0) - other) / std::max(factors, 1.0));
        }
        for (const Track& t : ts) if (t.def.max.k == Fn::K::Fr && t.def.max.value > 0) frSize = std::max(frSize, t.base / std::max(t.def.max.value, 1.0 > t.def.max.value ? t.def.max.value : 1.0));
      }
      for (Track& t : ts) {
        if (t.def.max.k != Fn::K::Fr) continue;
        const double flexFactor = t.def.max.value;
        t.base = std::max(t.base, frSize * flexFactor);
        t.limit = t.base;
      }
    }
    // Stretch the auto tracks to fill the container (align/justify-content normal or stretch).
    const Align::Kind content = axis == 1 ? style.justifyContent.kind : style.alignContent.kind;
    if (Known(available) && (content == Align::Kind::Normal || content == Align::Kind::Stretch)) {
      double free = available - sumTracks();
      if (free > 1e-9) {
        std::vector<int> autos;
        for (int i = 0; i < n; ++i) if (ts[i].def.max.k == Fn::K::Auto) autos.push_back(i);
        for (int i : autos) ts[i].base += free / autos.size();
      }
    }
  }

  double TrackSum(int axis) const {
    double sum = gap[axis] * (std::max<int>(0, static_cast<int>(tracks[axis].size()) - 1));
    for (const Track& t : tracks[axis]) sum += t.base;
    return sum;
  }

  std::pair<double, double> ContentOffset(int axis, double available) const {
    // A subgridded axis has its tracks from the grid above: nothing of its own to distribute.
    if (subgridded[axis]) return std::pair<double, double>{0, 0};
    const double used = TrackSum(axis);
    const double free = Known(available) ? available - used : 0;
    Align a = axis == 1 ? style.justifyContent : style.alignContent;
    Align::Kind k = a.kind;
    double offset = 0, between = 0;
    const double n = static_cast<double>(tracks[axis].size());
    if (k == Align::Kind::SpaceBetween && free < 0) k = Align::Kind::Start;
    if ((k == Align::Kind::SpaceAround || k == Align::Kind::SpaceEvenly) && free < 0) k = Align::Kind::Center;
    if (a.safe && free < 0 && (k == Align::Kind::Center || k == Align::Kind::End || k == Align::Kind::FlexEnd)) k = Align::Kind::Start;
    switch (k) {
      case Align::Kind::End: case Align::Kind::FlexEnd: offset = free; break;
      case Align::Kind::Center: offset = free / 2; break;
      case Align::Kind::Right: offset = axis == 1 ? (style.direction == Direction::Rtl ? 0 : free) : 0; break;
      case Align::Kind::Left: offset = axis == 1 && style.direction == Direction::Rtl ? free : 0; break;
      case Align::Kind::SpaceBetween: between = n > 1 ? free / (n - 1) : 0; break;
      case Align::Kind::SpaceAround: between = free / n; offset = between / 2; break;
      case Align::Kind::SpaceEvenly: between = free / (n + 1); offset = between; break;
      default: break;
    }
    return std::pair<double, double>{offset, between};
  }

  void Positions(int axis, double offset) {
    double pos = offset;
    for (Track& t : tracks[axis]) {
      t.position = pos;
      pos += t.base + gap[axis];
    }
  }
};

}  // namespace

namespace {

// Whether the start of the box's own writing mode, in the physical axis that `axis` of the grid (0 the rows, 1 the columns) is along, is at the same end as the grid's start:
// self-start and self-end are about the box's own sides.
bool StartsWithContainer(const BoxStyle& container, const BoxStyle& item, int axis) {
  const auto vertical = [](WritingMode m) { return m != WritingMode::HorizontalTb; };
  const auto inlineAtMin = [](const BoxStyle& s) { return s.writingMode == WritingMode::SidewaysLr ? s.direction == Direction::Rtl : s.direction == Direction::Ltr; };
  // The start of the block axis of a vertical mode is at the right, but for vertical-lr and sideways-lr.
  const auto blockAtMin = [&](WritingMode m) { return !vertical(m) || m == WritingMode::VerticalLr || m == WritingMode::SidewaysLr; };
  const bool containerVertical = vertical(container.writingMode);
  // The physical axis (true: vertical) the axis of the grid is along, and which end the grid starts it at.
  const bool physicalVertical = axis == 1 ? containerVertical : !containerVertical;
  const bool containerAtMin = axis == 1 ? inlineAtMin(container) : blockAtMin(container.writingMode);
  bool itemAtMin;
  if (vertical(item.writingMode) == physicalVertical) itemAtMin = inlineAtMin(item);  // that axis is the inline axis of the item
  else itemAtMin = blockAtMin(item.writingMode);
  return itemAtMin == containerAtMin;
}

}  // namespace

// ---- Grid lanes (the items are packed into the lanes of one axis, each below the one before in the lane that is shortest) ----

void LayoutGridLanes(LayoutContext& lc, Box& container, double contentWidth, double heightBasis, double& contentHeight) {
  Grid grid(lc, container, contentWidth, heightBasis);
  const BoxStyle& s = *container.style;
  const std::string direction = Lower(s.gridLanesDirection);
  // (With the direction normal, the lanes are those of the axis that has tracks; the columns when both have or none.)
  const bool rowLanes = direction.rfind("row", 0) == 0 || (direction.rfind("normal", 0) == 0 && Lower(s.gridTemplateColumns) == "none" && Lower(s.gridTemplateRows) != "none");
  const bool trackReverse = direction.find("track-reverse") != std::string::npos, fillReverse = direction.find("fill-reverse") != std::string::npos;
  const int L = rowLanes ? 0 : 1, F = 1 - L;  // the axis of the lanes, and the one the items flow along
  grid.laneAxis = L;
  const double colGap = ResolveSize(s.columnGap, contentWidth), rowGap = ResolveSize(s.rowGap, heightBasis);
  grid.gap[1] = Known(colGap) ? colGap : 0;
  grid.gap[0] = Known(rowGap) ? rowGap : 0;
  grid.gapGiven[1] = Known(colGap);
  grid.gapGiven[0] = Known(rowGap);
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
  grid.items.resize(boxes.size());
  for (size_t i = 0; i < boxes.size(); ++i) {
    grid.items[i].box = boxes[i];
    boxes[i]->subgrid[0].active = boxes[i]->subgrid[1].active = false;
  }
  grid.ParseTemplates(contentWidth, heightBasis);
  // The axis the items flow along has no tracks.
  grid.tracks[F].clear();
  grid.explicitCount[F] = 0;
  grid.lineNames[F].assign(1, {});
  // Where the items are in the lanes (the ones that say).
  int before = 0;
  for (GridItem& it : grid.items) {
    grid.ResolveLines(it);
    it.definite[F] = false;
    it.span[F] = 1;
    it.pos[F] = 0;
    if (it.definite[L]) before = std::max(before, -it.pos[L]);
  }
  grid.implicitBefore[L] = before;
  grid.implicitBefore[F] = 0;
  int needed = std::max(1, grid.explicitCount[L] + before);
  for (GridItem& it : grid.items) {
    if (it.definite[L]) it.pos[L] += before;
    needed = std::max(needed, it.definite[L] ? it.pos[L] + it.span[L] : it.span[L]);
  }
  grid.BuildTracks(L, needed);
  const int lanes = static_cast<int>(grid.tracks[L].size());
  for (GridItem& it : grid.items) grid.ItemEdges(it, contentWidth);

  // ---- The sizes of the lanes: an item with no lane of its own counts for every place it could be in. ----
  // A subgrid in the lanes is not an item of them: the items in it are (and are, in every place it could be in, when it has no lane of its own).
  for (GridItem& it : grid.items) {
    if (!it.box->style || !grid.IsSubgridIn(*it.box, L)) continue;
    it.subgridAxis[L] = true;
    if (it.definite[L]) {
      grid.ExpandSubgrid(it, *it.box, L, it.pos[L], it.span[L], 0, 0, contentWidth);
    } else {
      it.lanesAuto = true;
      for (int w = 0; w + it.span[L] <= lanes; ++w) grid.ExpandSubgrid(it, *it.box, L, w, it.span[L], 0, 0, contentWidth);
    }
  }
  for (GridItem& it : grid.items) {
    if (it.definite[L] || it.subgridAxis[L]) continue;
    it.lanesAuto = true;
    for (int w = 0; w + it.span[L] <= lanes; ++w) {
      GridItem v = it;
      v.lanesAuto = false;
      v.onlyAxis = L;
      v.pos[L] = w;
      grid.virtualItems.push_back(v);
    }
  }
  const auto fitWidth = [&](GridItem& it) {
    // An item that flows along the inline axis is as wide as its contents, within the room there is.
    ComputeContentSizes(lc, *it.box);
    const double extras = it.padBorder[1] + it.marginStart[1] + it.marginEnd[1];
    const double mn = it.box->minContent, mx = it.box->maxContent;
    const double specified = grid.SpecifiedSize(it, 1, contentWidth);
    const double content = Known(specified) ? specified : std::min(mx, std::max(mn, contentWidth - extras));
    return std::max(0.0, content) + extras;
  };
  if (L == 1) {
    for (GridItem& it : grid.items) if (!it.lanesAuto && !it.subgridAxis[1]) grid.ColumnContributions(it, contentWidth);
    for (GridItem& v : grid.virtualItems) grid.ColumnContributions(v, contentWidth);
    grid.SizeTracks(1, contentWidth, false);
  } else {
    for (GridItem& it : grid.items) if (!it.lanesAuto && !it.subgridAxis[0]) grid.RowContributions(it, fitWidth(it));
    for (GridItem& v : grid.virtualItems) grid.RowContributions(v, fitWidth(v));
    grid.SizeTracks(0, heightBasis, true);
  }
  // The lanes where they are: distributed as the content is.
  double laneExtent;  // the room along the axis of the lanes
  {
    const double used = grid.TrackSum(L);
    laneExtent = Known(L == 1 ? contentWidth : heightBasis) ? (L == 1 ? contentWidth : heightBasis) : used;
    const auto [offset, between] = grid.ContentOffset(L, laneExtent);
    double pos = offset;
    for (Track& t : grid.tracks[L]) {
      t.position = pos;
      pos += t.base + grid.gap[L] + between;
    }
    if (trackReverse || (L == 1 && s.direction == Direction::Rtl && Known(contentWidth))) {
      const bool mirror = trackReverse != (L == 1 && s.direction == Direction::Rtl);
      if (mirror) for (Track& t : grid.tracks[L]) t.position = laneExtent - t.position - t.base;
    }
  }
  const auto areaOf = [&](const GridItem& it) {
    double size = grid.gap[L] * (it.span[L] - 1);
    for (int i = it.pos[L]; i < it.pos[L] + it.span[L]; ++i) size += grid.tracks[L][i].base;
    return size;
  };

  // ---- The items, each in the lane (or the lanes) whose end is nearest the start. ----
  std::vector<double> laneEnd(static_cast<size_t>(lanes), 0.0);
  const double tolerance = [&] {
    const std::string& t = s.flowTolerance;
    if (t == "infinite") return kInf;
    if (t == "normal" || t.empty()) return s.fontSize;
    const double v = ResolveSize(Length(), kNaN);
    (void)v;
    char* end = nullptr;
    const double number = std::strtod(t.c_str(), &end);
    if (end == t.c_str()) return s.fontSize;
    if (std::string(end) == "%") return (Known(L == 1 ? heightBasis : contentWidth) ? (L == 1 ? heightBasis : contentWidth) : 0.0) * number / 100;
    return number;
  }();
  int cursor = 0;
  const double originX = container.ContentLeft(), originY = container.ContentTop();
  struct Slot {
    GridItem* item;
    int lane, span;
    double marginStart;  // where its margin box begins along the lanes
    double outer;        // the margin box's size there
    double across;       // where its border box begins across the lanes
    double windowStart;  // where the room it has across the lanes begins
    double area;         // the room across the lanes
    double forcedAcross; // the size across the lanes it was laid out at, if it was given one
    double flowStart = 0, flowSize = 0;  // the border box, along the lanes
  };
  std::vector<Slot> slots;
  // Dense packing: an item goes to the earliest place along the lanes where it fits, in the holes the others leave.
  const bool dense = s.gridLanesDense || Lower(s.gridAutoFlow).find("dense") != std::string::npos;
  std::vector<std::vector<std::pair<double, double>>> occupied(static_cast<size_t>(lanes));
  // How large the item is along the lanes when it is in the lanes from `lane` on.
  const auto measureOuter = [&](GridItem& it, int lane) {
    const int saved = it.pos[L];
    it.pos[L] = lane;
    Box& b = *it.box;
    const double area = areaOf(it);
    double forced[2] = {kNaN, kNaN};
    const Align::Kind kind = grid.SelfAlign(it, L);
    const double specified = grid.SpecifiedSize(it, L, L == 0 ? heightBasis : contentWidth);
    const bool stretch = (kind == Align::Kind::Normal || kind == Align::Kind::Stretch) && !it.autoMarginStart[L] && !it.autoMarginEnd[L] && !b.replaced;
    if (!Known(specified) && !grid.ContentSized(it, L) && stretch) forced[L] = std::max(0.0, area - it.marginStart[L] - it.marginEnd[L] - it.padBorder[L]);
    lc.forceWidth = forced[1];
    lc.forceHeight = forced[0];
    lc.cbX = 0;
    lc.boxY = 0;
    LayoutBlockLevel(lc, b, L == 1 ? area : contentWidth, L == 0 ? area : heightBasis, true);
    it.pos[L] = saved;
    return (F == 0 ? b.height : b.width) + it.marginStart[F] + it.marginEnd[F];
  };
  const auto freeStart = [&](int lane, int span, double outer) {
    std::vector<double> candidates = {0.0};
    for (int l = lane; l < lane + span; ++l) for (const auto& iv : occupied[static_cast<size_t>(l)]) candidates.push_back(iv.second);
    std::sort(candidates.begin(), candidates.end());
    for (const double p : candidates) {
      bool fits = true;
      for (int l = lane; l < lane + span && fits; ++l) {
        for (const auto& iv : occupied[static_cast<size_t>(l)]) {
          if (p < iv.second - 1e-9 && p + outer + grid.gap[F] > iv.first + 1e-9) { fits = false; break; }
        }
      }
      if (fits) return p;
    }
    return candidates.back();
  };
  for (GridItem& it : grid.items) {
    int lane = it.pos[L];
    const int span = it.span[L];
    double denseStart = kNaN;
    if (dense) {
      const int windows = it.lanesAuto ? lanes - span + 1 : 1;
      std::vector<double> at(static_cast<size_t>(windows));
      double best = kInf;
      for (int w = 0; w < windows; ++w) {
        const int candidate = it.lanesAuto ? w : std::min(lane, std::max(0, lanes - span));
        at[static_cast<size_t>(w)] = freeStart(candidate, span, measureOuter(it, candidate));
        best = std::min(best, at[static_cast<size_t>(w)]);
      }
      int chosen = 0;
      for (int k = 0; k < windows; ++k) {
        const int w = it.lanesAuto ? (cursor + k < windows ? cursor + k : cursor + k - windows - std::max(0, cursor - windows)) : 0;
        if (w >= 0 && w < windows && at[static_cast<size_t>(w)] <= best + tolerance) {
          chosen = w;
          break;
        }
      }
      if (it.lanesAuto) {
        lane = chosen;
        cursor = lane + span >= lanes ? 0 : lane + span;
        it.pos[L] = lane;
      }
      denseStart = at[static_cast<size_t>(chosen)];
    } else if (it.lanesAuto) {
      const int windows = lanes - span + 1;
      const auto endOf = [&](int w) {
        double e = -kInf;
        for (int i = w; i < w + span; ++i) e = std::max(e, laneEnd[static_cast<size_t>(i)]);
        return e;
      };
      double best = kInf;
      for (int w = 0; w < windows; ++w) best = std::min(best, endOf(w));
      lane = 0;
      // (From the cursor on, and then from the start.)
      for (int k = 0; k < windows; ++k) {
        const int w = cursor + k < windows ? cursor + k : cursor + k - windows - std::max(0, cursor - windows);
        if (w >= 0 && w < windows && endOf(w) <= best + tolerance) {
          lane = w;
          break;
        }
      }
      cursor = lane + span >= lanes ? 0 : lane + span;
      it.pos[L] = lane;
    }
    lane = std::min(lane, std::max(0, lanes - span));
    double start = -kInf;
    for (int i = lane; i < lane + span; ++i) start = std::max(start, laneEnd[static_cast<size_t>(i)]);
    if (Known(denseStart)) start = denseStart;
    // Laid out in the room of its lanes.
    Box& b = *it.box;
    const BoxStyle& st = *b.style;
    const double area = areaOf(it);
    double forced[2] = {kNaN, kNaN};
    const Align::Kind kind = grid.SelfAlign(it, L);
    {
      const double specified = grid.SpecifiedSize(it, L, L == 0 ? heightBasis : contentWidth);
      const bool stretch = (kind == Align::Kind::Normal || kind == Align::Kind::Stretch) && !it.autoMarginStart[L] && !it.autoMarginEnd[L] && !b.replaced;
      if (!Known(specified) && !grid.ContentSized(it, L) && stretch) forced[L] = std::max(0.0, area - it.marginStart[L] - it.marginEnd[L] - it.padBorder[L]);
    }
    // A grid container that is a subgrid in the axis of the lanes takes the lanes it spans.
    if (IsGridDisplay(st.display) && grid.IsSubgridIn(b, L)) {
      SubgridTracks& sub = b.subgrid[L];
      sub = SubgridTracks();
      sub.active = true;
      for (int i = lane; i < lane + span; ++i) sub.sizes.push_back(grid.tracks[L][i].base);
      for (int i = lane; i <= lane + span; ++i) sub.names.push_back(static_cast<size_t>(i) < grid.lineNames[L].size() ? grid.lineNames[L][i] : std::vector<std::string>());
      sub.gap = grid.gap[L];
    }
    lc.forceWidth = forced[1];
    lc.forceHeight = forced[0];
    lc.cbX = 0;
    lc.boxY = 0;
    LayoutBlockLevel(lc, b, L == 1 ? area : contentWidth, L == 0 ? area : heightBasis, true);
    const double sizes[2] = {b.height, b.width};
    const double flowOuter = sizes[F] + it.marginStart[F] + it.marginEnd[F];
    // The place along the lanes (its margin box begins where the lanes end).
    double offset = it.marginStart[L];
    {
      const double free = area - (sizes[L] + it.marginStart[L] + it.marginEnd[L]);
      double ms = it.marginStart[L], me = it.marginEnd[L];
      if (it.autoMarginStart[L] || it.autoMarginEnd[L]) {
        if (free > 0) {
          if (it.autoMarginStart[L] && it.autoMarginEnd[L]) { ms += free / 2; me += free / 2; }
          else if (it.autoMarginStart[L]) ms += free;
          else me += free;
        }
        offset = ms;
        it.marginStart[L] = ms;
        it.marginEnd[L] = me;
      } else {
        Align::Kind k = kind;
        if (k == Align::Kind::SelfStart || k == Align::Kind::SelfEnd) {
          const bool same = StartsWithContainer(s, st, L);
          k = (k == Align::Kind::SelfStart) == same ? Align::Kind::Start : Align::Kind::End;
        }
        const bool rtl = s.direction == Direction::Rtl && L == 1;
        switch (k) {
          case Align::Kind::End: case Align::Kind::FlexEnd: case Align::Kind::SelfEnd: case Align::Kind::LastBaseline: offset += rtl ? 0 : free; break;
          case Align::Kind::Center: offset += free / 2; break;
          case Align::Kind::Right: offset += L == 1 ? free : 0; break;
          default:
            if (rtl && k != Align::Kind::Left) offset += free;
            break;
        }
        if (free < 0 && grid.SafeAlign(it, L)) offset = it.marginStart[L];
      }
    }
    b.margin.top = it.marginStart[0];
    b.margin.bottom = it.marginEnd[0];
    b.margin.left = it.marginStart[1];
    b.margin.right = it.marginEnd[1];
    const double laneStart = grid.tracks[L][lane].position;
    // (The lane that starts the window, or the one at its far end when the lanes run the other way.)
    double windowStart = laneStart;
    for (int i = lane; i < lane + span; ++i) windowStart = std::min(windowStart, grid.tracks[L][i].position);
    slots.push_back({&it, lane, span, start, flowOuter, windowStart + offset, windowStart, area, forced[L]});
    const double end = start + flowOuter + grid.gap[F];
    for (int i = lane; i < lane + span; ++i) {
      // (A negative margin does not take the end of the lane back.)
      laneEnd[static_cast<size_t>(i)] = std::max(laneEnd[static_cast<size_t>(i)], end);
      occupied[static_cast<size_t>(i)].push_back({start, end});
    }
  }
  double flowTotal = 0;
  for (double e : laneEnd) flowTotal = std::max(flowTotal, e);
  if (!slots.empty()) flowTotal = std::max(0.0, flowTotal - grid.gap[F]);
  // The size along the axis of the lanes is the lanes'; the other is as far as the items reach, but for a size it is given.
  const double flowSize = F == 0 ? (Known(heightBasis) ? heightBasis : flowTotal) : (Known(contentWidth) ? contentWidth : flowTotal);
  // Items that line up on a baseline in a lane do so with the others in it (the room they ask for is not added to the lane).
  {
    bool any = false;
    for (GridItem& it : grid.items) any = any || grid.BaselineKindOf(it, L);
    if (any) {
      grid.ComputeBaselines(L, nullptr, [&](const GridItem& it) { return L == 0 ? it.box->height : it.box->width; });
      for (Slot& slot : slots) {
        GridItem& it = *slot.item;
        if (!it.baselineKind[L]) continue;
        Box& b = *it.box;
        const double size = L == 0 ? b.height : b.width;
        slot.across = slot.windowStart + (it.baselineMin[L] ? it.marginStart[L] + it.shim[L] : slot.area - size - it.marginEnd[L] - it.shim[L]);
      }
    }
  }
  // Along the lanes an item has the room from where it begins to where the next item in its lanes does (or the end): where it is in that is up to its alignment.
  {
    std::vector<std::vector<size_t>> inLane(static_cast<size_t>(lanes));
    for (size_t i = 0; i < slots.size(); ++i) for (int l = slots[i].lane; l < slots[i].lane + slots[i].span; ++l) inLane[static_cast<size_t>(l)].push_back(i);
    for (auto& list : inLane) std::stable_sort(list.begin(), list.end(), [&](size_t a, size_t b) { return slots[a].marginStart < slots[b].marginStart; });
    for (size_t i = 0; i < slots.size(); ++i) {
      Slot& slot = slots[i];
      GridItem& it = *slot.item;
      Box& b = *it.box;
      double limit = flowSize;
      for (int l = slot.lane; l < slot.lane + slot.span; ++l) {
        const std::vector<size_t>& list = inLane[static_cast<size_t>(l)];
        for (size_t k = 0; k + 1 < list.size(); ++k) {
          if (list[k] == i) limit = std::min(limit, slots[list[k + 1]].marginStart - grid.gap[F]);
        }
      }
      double size = F == 0 ? b.height : b.width;
      double free = limit - slot.marginStart - slot.outer;
      Align::Kind kind = grid.SelfAlign(it, F);
      double shift = 0;
      if (kind == Align::Kind::Stretch && free > 0 && !b.replaced && !Known(grid.SpecifiedSize(it, F, F == 0 ? heightBasis : contentWidth))) {
        // It grows to the room (laid out again at the size).
        const double grown = size + free;
        // (The content size along the lanes is its size less its padding and border.)
        const double content = std::max(0.0, grown - it.padBorder[F]);
        lc.forceWidth = F == 1 ? content : slot.forcedAcross;
        lc.forceHeight = F == 0 ? content : slot.forcedAcross;
        lc.cbX = 0;
        lc.boxY = 0;
        LayoutBlockLevel(lc, b, L == 1 ? slot.area : contentWidth, L == 0 ? slot.area : heightBasis, true);
        b.margin.top = it.marginStart[0];
        b.margin.bottom = it.marginEnd[0];
        b.margin.left = it.marginStart[1];
        b.margin.right = it.marginEnd[1];
        size = F == 0 ? b.height : b.width;
        free = 0;
      } else if (kind == Align::Kind::End || kind == Align::Kind::FlexEnd || kind == Align::Kind::SelfEnd || kind == Align::Kind::LastBaseline) {
        shift = free;
      } else if (kind == Align::Kind::Center) {
        shift = free / 2;
      }
      if (free < 0) shift = 0;
      slot.flowSize = size;
      slot.flowStart = slot.marginStart + it.marginStart[F] + shift;
      if (L == 1) {
        b.x = originX + slot.across + b.shiftX;
        b.y = originY + slot.flowStart + b.shiftY;
      } else {
        b.y = originY + slot.across + b.shiftY;
        b.x = originX + slot.flowStart + b.shiftX;
      }
    }
  }
  if (fillReverse) {
    // The items begin at the far end and run back.
    for (Slot& slot : slots) {
      Box& b = *slot.item->box;
      const double mirrored = flowSize - (slot.flowStart + slot.flowSize);
      const double delta = mirrored - slot.flowStart;
      if (F == 0) b.y += delta;
      else b.x += delta;
    }
  }
  contentHeight = L == 1 ? flowSize : (Known(heightBasis) ? heightBasis : grid.TrackSum(0));
  auto lines = std::make_shared<GridLines>();
  for (int a = 0; a < 2; ++a) {
    const double offset = a == 0 ? container.padding.top : container.padding.left;
    if (a == L) {
      for (const Track& t : grid.tracks[a]) {
        lines->start[a].push_back(t.position + offset);
        lines->end[a].push_back(t.position + t.base + offset);
      }
    } else {
      lines->start[a].push_back(offset);
      lines->end[a].push_back(offset + (a == 0 ? contentHeight : contentWidth));
    }
    lines->explicitCount[a] = a == L ? grid.explicitCount[a] : 0;
    lines->implicitBefore[a] = a == L ? grid.implicitBefore[a] : 0;
    lines->names[a] = grid.lineNames[a];
  }
  container.gridLines = lines;
  // The baselines of the container: the first is that of the first item of the first lane that has any, the last that of the last item of the last lane.
  container.firstBaseline = container.baseline = -1;
  Slot* first = nullptr;
  Slot* last = nullptr;
  for (Slot& slot : slots) {
    if (!first || slot.lane < first->lane || (slot.lane == first->lane && slot.marginStart < first->marginStart)) first = &slot;
    const int lastLane = slot.lane + slot.span - 1;
    const int bestLane = last ? last->lane + last->span - 1 : -1;
    if (!last || lastLane > bestLane || (lastLane == bestLane && slot.marginStart + slot.outer > last->marginStart + last->outer)) last = &slot;
  }
  if (first) {
    Box* b = first->item->box;
    container.firstBaseline = b->y - b->shiftY - originY + (b->firstBaseline >= 0 ? b->firstBaseline : b->height) + originY;
  }
  if (last) {
    Box* b = last->item->box;
    container.baseline = b->y - b->shiftY - originY + (b->baseline >= 0 ? b->baseline : b->height) + originY;
  }
}

void LayoutGrid(LayoutContext& lc, Box& container, double contentWidth, double heightBasis, double& contentHeight) {
  if (container.style->gridLanes) {
    LayoutGridLanes(lc, container, contentWidth, heightBasis, contentHeight);
    return;
  }
  Grid grid(lc, container, contentWidth, heightBasis);
  const BoxStyle& s = *container.style;
  const double colGap = ResolveSize(s.columnGap, contentWidth), rowGap = ResolveSize(s.rowGap, heightBasis);
  grid.gap[1] = Known(colGap) ? colGap : 0;
  grid.gap[0] = Known(rowGap) ? rowGap : 0;
  grid.gapGiven[1] = Known(colGap);
  grid.gapGiven[0] = Known(rowGap);
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
  grid.items.resize(boxes.size());
  for (size_t i = 0; i < boxes.size(); ++i) {
    grid.items[i].box = boxes[i];
    // (The tracks of a subgrid are known when its grid has sized them: until then it has none.)
    boxes[i]->subgrid[0].active = boxes[i]->subgrid[1].active = false;
  }
  grid.ParseTemplates(contentWidth, heightBasis);
  grid.ApplySubgrid();
  grid.Place();
  for (GridItem& it : grid.items) grid.ItemEdges(it, contentWidth);
  // In a subgridded axis the items beside a line have the share of the gap of the subgrid that the grid above does not have.
  for (GridItem& it : grid.items) {
    for (int a = 0; a < 2; ++a) {
      if (!grid.subgridded[a] || grid.subgridGapDelta[a] == 0) continue;
      if (it.pos[a] > 0) it.marginStart[a] += grid.subgridGapDelta[a];
      if (it.pos[a] + it.span[a] < static_cast<int>(grid.tracks[a].size())) it.marginEnd[a] += grid.subgridGapDelta[a];
    }
  }
  // The items in a subgrid size the tracks it spans.
  for (GridItem& it : grid.items) {
    for (int a = 0; a < 2; ++a) {
      if (!grid.IsSubgridIn(*it.box, a)) continue;
      it.subgridAxis[a] = true;
    }
  }
  for (size_t i = 0; i < grid.items.size(); ++i) {
    for (int a = 0; a < 2; ++a) {
      if (!grid.items[i].subgridAxis[a]) continue;
      // (the other axis has to be known to the items inside, for placing them)
      grid.ExpandSubgrid(grid.items[i], *grid.items[i].box, a, grid.items[i].pos[a], grid.items[i].span[a], 0, 0, contentWidth);
    }
  }

  // Columns.
  for (GridItem& it : grid.items) if (!it.subgridAxis[1]) grid.ColumnContributions(it, contentWidth);
  for (GridItem& v : grid.virtualItems) if (v.onlyAxis == 1) grid.ColumnContributions(v, contentWidth);
  grid.ComputeBaselines(1, [&](const GridItem&) { return contentWidth; }, [](const GridItem& it) { return it.maxContentContribution - it.marginStart[1] - it.marginEnd[1]; });
  for (GridItem& it : grid.items) {
    it.minContribution += it.shim[1];
    it.minContentContribution += it.shim[1];
    it.maxContentContribution += it.shim[1];
  }
  grid.SizeTracks(1, contentWidth, false);
  // The widths of the item areas, for the rows.
  double spread[2] = {0, 0};  // what content distribution adds to the gap between two tracks: an area that spans tracks has it too
  const auto areaSize = [&](const GridItem& it, int axis) {
    double size = (grid.gap[axis] + spread[axis]) * (it.span[axis] - 1);
    for (int i = it.pos[axis]; i < it.pos[axis] + it.span[axis]; ++i) size += grid.tracks[axis][i].base;
    return size;
  };
  // Rows.
  for (GridItem& it : grid.items) if (!it.subgridAxis[0]) grid.RowContributions(it, areaSize(it, 1));
  for (GridItem& v : grid.virtualItems) {
    if (v.onlyAxis != 0) continue;
    grid.RowContributions(v, v.hasColumns ? areaSize(v, 1) : contentWidth);
  }
  grid.ComputeBaselines(0, [&](const GridItem& it) { return areaSize(it, 1); }, [](const GridItem& it) { return it.box->height; });
  for (GridItem& it : grid.items) {
    it.minContribution += it.shim[0];
    it.minContentContribution += it.shim[0];
    it.maxContentContribution += it.shim[0];
  }
  const double rowsAvailable = heightBasis;
  // Rows that are percentages of a height that is not known are auto ones for the height of the grid, and are worked out against that height after.
  double intrinsicRows = kNaN;
  if (!Known(rowsAvailable) && !grid.subgridded[0]) {
    bool percentages = false;
    for (const Track& t : grid.tracks[0]) percentages = percentages || t.def.min.k == Fn::K::Percent || t.def.max.k == Fn::K::Percent || !t.def.min.calc.empty() || !t.def.max.calc.empty();
    if (percentages) {
      std::vector<TrackDef> defs;
      for (Track& t : grid.tracks[0]) {
        defs.push_back(t.def);
        for (Fn* f : {&t.def.min, &t.def.max}) if (f->k == Fn::K::Percent || !f->calc.empty()) *f = Fn();
      }
      grid.SizeTracks(0, rowsAvailable, true);
      intrinsicRows = grid.TrackSum(0);
      for (size_t i = 0; i < defs.size(); ++i) grid.tracks[0][i].def = defs[i];
    }
  }
  grid.SizeTracks(0, Known(intrinsicRows) ? intrinsicRows : rowsAvailable, true);  // (a height that is auto is the max-content one)
  // align/justify-content positions.
  const auto contentOffset = [&](int axis, double available) { return grid.ContentOffset(axis, available); };
  {
    const auto [offset, between] = contentOffset(1, contentWidth);
    double pos = offset;
    spread[1] = between;
    for (Track& t : grid.tracks[1]) { t.position = pos; pos += t.base + grid.gap[1] + between; }
    // Right to left: the columns start at the right edge.
    if (s.direction == Direction::Rtl && Known(contentWidth)) {
      for (Track& t : grid.tracks[1]) t.position = contentWidth - t.position - t.base;
    }
  }
  const double usedRows = grid.TrackSum(0);
  const double rowContainer = Known(heightBasis) ? heightBasis : Known(intrinsicRows) ? intrinsicRows : usedRows;
  // A gap that is a percentage of a height that is not known was nothing for the sizes of the rows: it is of the height they came to.
  if (!Known(heightBasis) && s.rowGap.IsPercentage() && !grid.subgridded[0]) grid.gap[0] = ResolveSize(s.rowGap, rowContainer);
  {
    const auto [offset, between] = contentOffset(0, rowContainer);
    double pos = offset;
    spread[0] = between;
    for (Track& t : grid.tracks[0]) { t.position = pos; pos += t.base + grid.gap[0] + between; }
  }
  contentHeight = rowContainer;

  // Place the items.
  const double originX = container.ContentLeft(), originY = container.ContentTop();
  double firstBaseline = -1, lastBaseline = -1;
  int firstColumn = std::numeric_limits<int>::max(), lastColumn = -1;
  // The item laid out in its grid area.
  const auto layoutAtArea = [&](GridItem& it) {
    Box& b = *it.box;
    const BoxStyle& st = *b.style;
    const double areaW = areaSize(it, 1), areaH = areaSize(it, 0);
    double forced[2] = {kNaN, kNaN};
    const Align::Kind kinds[2] = {grid.SelfAlign(it, 0), grid.SelfAlign(it, 1)};
    const double area[2] = {areaH, areaW};
    for (int a = 0; a < 2; ++a) {
      const double specified = grid.SpecifiedSize(it, a, a == 0 ? heightBasis : contentWidth);
      const bool stretch = (kinds[a] == Align::Kind::Stretch || (kinds[a] == Align::Kind::Normal && !b.replaced)) && !it.autoMarginStart[a] && !it.autoMarginEnd[a];
      if (!Known(specified) && !grid.ContentSized(it, a) && stretch) {
        forced[a] = std::max(0.0, area[a] - it.marginStart[a] - it.marginEnd[a] - it.padBorder[a]);
        // The size it is stretched to stays within its minimum and maximum, which are of the area.
        const BoxStyle& bs = *b.style;
        const double minSize = ResolveSize(grid.MinLen(it, a), area[a]), maxSize = ResolveSize(grid.MaxLen(it, a), area[a]);
        const bool borderBox = bs.boxSizing == BoxSizing::BorderBox;
        if (Known(maxSize)) forced[a] = std::min(forced[a], std::max(0.0, borderBox ? maxSize - it.padBorder[a] : maxSize));
        if (Known(minSize)) forced[a] = std::max(forced[a], std::max(0.0, borderBox ? minSize - it.padBorder[a] : minSize));
      }
    }
    // A grid container that is a subgrid takes the tracks it spans.
    if (IsGridDisplay(st.display)) {
      const std::string* texts[2] = {&st.gridTemplateRows, &st.gridTemplateColumns};
      for (int a = 0; a < 2; ++a) {
        if (Lower(*texts[a]).rfind("subgrid", 0) != 0) continue;
        SubgridTracks& sub = b.subgrid[a];
        sub = SubgridTracks();
        sub.active = true;
        for (int i = it.pos[a]; i < it.pos[a] + it.span[a]; ++i) sub.sizes.push_back(grid.tracks[a][i].base);
        for (int i = it.pos[a]; i <= it.pos[a] + it.span[a]; ++i) {
          sub.names.push_back(static_cast<size_t>(i) < grid.lineNames[a].size() ? grid.lineNames[a][i] : std::vector<std::string>());
        }
        sub.gap = grid.gap[a] + spread[a];
      }
    }
    grid.lc.forceWidth = forced[1];
    grid.lc.forceHeight = forced[0];
    grid.lc.cbX = 0;
    grid.lc.boxY = 0;
    LayoutBlockLevel(lc, b, areaW, areaH, true);
  };
  // What the items that line up on a baseline have to leave, now that the areas they are in are known: the sizes of the items count for the baselines that are at the far edge.
  {
    bool any = false;
    for (GridItem& it : grid.items) any = any || grid.BaselineKindOf(it, 0) || grid.BaselineKindOf(it, 1);
    if (any) {
      for (GridItem& it : grid.items) {
        if (grid.BaselineKindOf(it, 0) || grid.BaselineKindOf(it, 1)) layoutAtArea(it);
      }
      grid.ComputeBaselines(0, nullptr, [](const GridItem& it) { return it.box->height; });
      grid.ComputeBaselines(1, nullptr, [](const GridItem& it) { return it.box->width; });
    }
  }
  for (GridItem& it : grid.items) {
    Box& b = *it.box;
    const BoxStyle& st = *b.style;
    const double areaW = areaSize(it, 1), areaH = areaSize(it, 0);
    const double areaX = grid.tracks[1][it.pos[1]].position, areaY = grid.tracks[0][it.pos[0]].position;
    const Align::Kind kinds[2] = {grid.SelfAlign(it, 0), grid.SelfAlign(it, 1)};
    const double area[2] = {areaH, areaW};
    layoutAtArea(it);
    // Position by alignment.
    const double sizes[2] = {b.height, b.width};
    double offsets[2];
    for (int a = 0; a < 2; ++a) {
      const double outer = sizes[a] + it.marginStart[a] + it.marginEnd[a];
      double free = area[a] - outer;
      double start = it.marginStart[a];
      double ms = it.marginStart[a], me = it.marginEnd[a];
      if (it.autoMarginStart[a] || it.autoMarginEnd[a]) {
        if (free > 0) {
          if (it.autoMarginStart[a] && it.autoMarginEnd[a]) { ms += free / 2; me += free / 2; }
          else if (it.autoMarginStart[a]) ms += free;
          else me += free;
        }
        start = ms;
        it.marginStart[a] = ms;
        it.marginEnd[a] = me;
      } else {
        Align::Kind k = kinds[a];
        if (k == Align::Kind::SelfStart || k == Align::Kind::SelfEnd) {
          const bool same = StartsWithContainer(s, st, a);
          k = (k == Align::Kind::SelfStart) == same ? Align::Kind::Start : Align::Kind::End;
        }
        if (k == Align::Kind::Normal || k == Align::Kind::Stretch) k = b.replaced || Known(grid.SpecifiedSize(it, a, a == 0 ? heightBasis : contentWidth)) ? Align::Kind::Start : Align::Kind::Start;
        const bool rtl = s.direction == Direction::Rtl && a == 1;  // (the columns of the grid start at the right)
        switch (k) {
          case Align::Kind::End: case Align::Kind::FlexEnd: case Align::Kind::SelfEnd: start += rtl ? 0 : free; break;
          case Align::Kind::Center: start += free / 2; break;
          case Align::Kind::Right: start += a == 1 ? free : 0; break;
          case Align::Kind::Baseline: case Align::Kind::LastBaseline:
            // On a baseline: the room the group asks for before the side the baseline is read from. Without one (the item spans tracks, has auto margins) it is at the start, or at the end for the last baseline.
            if (it.baselineKind[a]) start = it.baselineMin[a] ? it.marginStart[a] + it.shim[a] : area[a] - sizes[a] - it.marginEnd[a] - it.shim[a];
            else if (k == Align::Kind::LastBaseline) start += free;
            break;
          case Align::Kind::Left: break;
          case Align::Kind::Start: case Align::Kind::SelfStart: case Align::Kind::FlexStart:
            if (rtl) start += free;
            break;
          default: break;
        }
        if (free < 0 && grid.SafeAlign(it, a)) start = it.marginStart[a];
      }
      offsets[a] = start;
    }
    b.margin.top = it.marginStart[0];
    b.margin.bottom = it.marginEnd[0];
    b.margin.left = it.marginStart[1];
    b.margin.right = it.marginEnd[1];
    b.x = originX + areaX + offsets[1] + b.shiftX;
    b.y = originY + areaY + offsets[0] + b.shiftY;
    // (The first item in grid order, which runs along the rows, is the one in the first column of the row; the last is the one in the last column.)
    // (An item without a baseline has the one its border box gives: its bottom edge.)
    if (it.pos[0] == 0 && it.pos[1] < firstColumn) {
      firstColumn = it.pos[1];
      firstBaseline = b.y - b.shiftY - originY + (b.firstBaseline >= 0 ? b.firstBaseline : b.height);
    }
    if (it.pos[0] + it.span[0] == static_cast<int>(grid.tracks[0].size()) && it.pos[1] >= lastColumn) {
      lastColumn = it.pos[1];
      lastBaseline = b.y - b.shiftY - originY + (b.baseline >= 0 ? b.baseline : b.height);
    }
  }
  // The baselines of the container: those of the items that line up in the first row (and in the last), where they line up; else those of the first item in the first row (the last in the last).
  {
    const int rows = static_cast<int>(grid.tracks[0].size());
    double first = -1, last = -1;
    for (GridItem& it : grid.items) {
      if (!it.baselineKind[0]) continue;
      const double rowStart = grid.tracks[0][it.pos[0]].position, rowSize = grid.tracks[0][it.pos[0]].base;
      const double at = it.baselineMin[0] ? rowStart + it.baselineGroup[0] : rowStart + rowSize - it.baselineGroup[0];
      if (it.baselineKind[0] == 1 && it.pos[0] == 0 && first < 0) first = at;
      if (it.baselineKind[0] == 2 && it.pos[0] == rows - 1 && last < 0) last = at;
    }
    if (first >= 0) firstBaseline = first;
    if (last >= 0) lastBaseline = last;
  }
  container.firstBaseline = firstBaseline >= 0 ? firstBaseline + originY : -1;
  container.baseline = lastBaseline >= 0 ? lastBaseline + originY : container.firstBaseline;
  auto lines = std::make_shared<GridLines>();
  for (int a = 0; a < 2; ++a) {
    // (positions relative to the container's padding box)
    const double offset = a == 0 ? container.border.top * 0 + container.padding.top : container.padding.left;
    for (const Track& t : grid.tracks[a]) {
      lines->start[a].push_back(t.position + offset);
      lines->end[a].push_back(t.position + t.base + offset);
    }
    lines->explicitCount[a] = grid.explicitCount[a];
    lines->implicitBefore[a] = grid.implicitBefore[a];
    lines->names[a] = grid.lineNames[a];
  }
  container.gridLines = lines;
}

// The area a positioned box's grid-placement properties name in the grid, as a rectangle of the grid container's padding box (in the
// container's own frame); auto lines are the padding edges.
bool GridAreaOf(const Box& grid, const BoxStyle& absStyle, double paddingWidth, double paddingHeight, Rect& out) {
  if (!grid.gridLines) return false;
  const GridLines& g = *grid.gridLines;
  const std::string* starts[2] = {&absStyle.gridRowStart, &absStyle.gridColumnStart};
  const std::string* ends[2] = {&absStyle.gridRowEnd, &absStyle.gridColumnEnd};
  double edges[2][2];  // [axis][start, end]
  for (int axis = 0; axis < 2; ++axis) {
    const LineSpec a = ParseLine(*starts[axis]), b = ParseLine(*ends[axis]);
    const int count = static_cast<int>(g.start[axis].size());
    const int explicitLines = g.explicitCount[axis] + 1;
    const auto lineIndex = [&](const LineSpec& spec, bool isEnd) -> int {
      // 0-based line index among all the lines (implicit included), or -1 for auto
      if (spec.k != LineSpec::K::Line) return -1;
      int found = 0;
      if (!spec.name.empty()) {
        const auto search = [&](const std::string& wanted) {
          int seen = 0;
          const int want = spec.n > 0 ? spec.n : -spec.n;
          if (spec.n > 0) {
            for (size_t i = 0; i < g.names[axis].size(); ++i) for (const std::string& x : g.names[axis][i]) if (x == wanted && ++seen == want) return static_cast<int>(i) + 1;
          } else {
            for (size_t i = g.names[axis].size(); i-- > 0;) for (const std::string& x : g.names[axis][i]) if (x == wanted && ++seen == want) return static_cast<int>(i) + 1;
          }
          return 0;
        };
        found = search(spec.name);
        if (!found) found = search(spec.name + (isEnd ? "-end" : "-start"));
        if (!found) found = spec.n > 0 ? explicitLines + spec.n : 1 + spec.n;
      } else {
        found = spec.n > 0 ? spec.n : explicitLines + 1 + spec.n;
      }
      return found - 1 + g.implicitBefore[axis];
    };
    int s = lineIndex(a, false), e = lineIndex(b, true);
    if (a.k == LineSpec::K::Span && e >= 0) s = e - std::max(1, a.n);
    if (b.k == LineSpec::K::Span && s >= 0) e = s + std::max(1, b.n);
    const double total = axis == 0 ? paddingHeight : paddingWidth;
    // A start line is where its track begins, an end line where the track before it ends (the gutter is not in the area); lines past the last are the edges.
    // (The tracks of the inline axis are mirrored in the frame for right to left; the positions are worked out from the start and mirrored back.)
    const bool mirrored = axis == 1 && grid.style->direction == Direction::Rtl;
    const auto trackStart = [&](int i) { return mirrored ? total - g.end[axis][i] : g.start[axis][i]; };
    const auto trackEnd = [&](int i) { return mirrored ? total - g.start[axis][i] : g.end[axis][i]; };
    const auto position = [&](int index, bool isEnd) {
      if (index < 0) return isEnd ? total : 0.0;
      if (count == 0) return total;
      if (isEnd) {
        if (index == 0) return trackStart(0);
        return index > count ? total : trackEnd(index - 1);
      }
      if (index < count) return trackStart(index);
      return index == count ? trackEnd(count - 1) : 0.0;
    };
    edges[axis][0] = position(s, false);
    edges[axis][1] = position(e, true);
    if (edges[axis][1] < edges[axis][0]) std::swap(edges[axis][0], edges[axis][1]);
    if (mirrored) {
      const double a0 = total - edges[axis][1], a1 = total - edges[axis][0];
      edges[axis][0] = a0;
      edges[axis][1] = a1;
    }
  }
  // rows are the block axis, columns the inline
  out = {edges[1][0], edges[0][0], edges[1][1] - edges[1][0], edges[0][1] - edges[0][0]};
  return true;
}

// The widths of grid lanes whose lanes are rows: the items run along them, one after another in the lane that is shortest.
static void GridLanesRowSizes(LayoutContext& lc, Box& container, double& minContent, double& maxContent) {
  const BoxStyle& s = *container.style;
  Grid grid(lc, container, 0, kNaN);
  const double colGap = ResolveSize(s.columnGap, 0);
  grid.gap[1] = Known(colGap) ? colGap : 0;
  grid.gap[0] = 0;
  std::vector<Box*> boxes;
  for (auto& c : container.children) if (!c->style->IsOutOfFlow()) boxes.push_back(c.get());
  std::stable_sort(boxes.begin(), boxes.end(), [](const Box* a, const Box* b) { return a->style->order < b->style->order; });
  grid.items.resize(boxes.size());
  for (size_t i = 0; i < boxes.size(); ++i) grid.items[i].box = boxes[i];
  grid.ParseTemplates(kNaN, kNaN);
  int lanes = std::max<int>(1, static_cast<int>(grid.tracks[0].size()));
  for (GridItem& it : grid.items) {
    grid.ResolveLines(it);
    it.span[0] = std::max(1, it.span[0]);
    lanes = std::max(lanes, it.definite[0] ? it.pos[0] + it.span[0] : it.span[0]);
  }
  for (GridItem& it : grid.items) grid.ItemEdges(it, 0);
  std::vector<double> end(static_cast<size_t>(lanes), 0.0);
  const double tolerance = s.flowTolerance == "infinite" ? kInf : (s.flowTolerance == "normal" || s.flowTolerance.empty()) ? s.fontSize : std::atof(s.flowTolerance.c_str());
  int cursor = 0;
  minContent = 0;
  for (GridItem& it : grid.items) {
    ComputeContentSizes(lc, *it.box);
    const double extras = it.padBorder[1] + it.marginStart[1] + it.marginEnd[1];
    const double specified = grid.SpecifiedSize(it, 1, 0);
    const double outerMax = (Known(specified) ? specified : it.box->maxContent) + extras;
    minContent = std::max(minContent, (Known(specified) ? specified : it.box->minContent) + extras);
    const int span = std::min(lanes, std::max(1, it.span[0]));
    int lane = it.definite[0] ? std::min(it.pos[0], lanes - span) : 0;
    if (!it.definite[0]) {
      const int windows = lanes - span + 1;
      const auto endOf = [&](int w) {
        double e = 0;
        for (int i = w; i < w + span; ++i) e = std::max(e, end[static_cast<size_t>(i)]);
        return e;
      };
      double best = kInf;
      for (int w = 0; w < windows; ++w) best = std::min(best, endOf(w));
      for (int k = 0; k < windows; ++k) {
        const int w = cursor + k < windows ? cursor + k : cursor + k - windows - std::max(0, cursor - windows);
        if (w >= 0 && w < windows && endOf(w) <= best + tolerance) { lane = w; break; }
      }
      cursor = lane + span >= lanes ? 0 : lane + span;
    }
    double start = 0;
    for (int i = lane; i < lane + span; ++i) start = std::max(start, end[static_cast<size_t>(i)]);
    for (int i = lane; i < lane + span; ++i) end[static_cast<size_t>(i)] = start + outerMax + grid.gap[1];
  }
  maxContent = 0;
  for (double e : end) maxContent = std::max(maxContent, e);
  if (!grid.items.empty()) maxContent = std::max(0.0, maxContent - grid.gap[1]);
  minContent = std::min(minContent, maxContent);
}

// The widths of grid lanes whose lanes are columns: those of the lanes, with the items that have no lane of their own counted for every one they could be in.
static void GridLanesColumnSizes(LayoutContext& lc, Box& container, double& minContent, double& maxContent) {
  const BoxStyle& s = *container.style;
  for (int pass = 0; pass < 2; ++pass) {
    Grid grid(lc, container, 0, kNaN);
    grid.laneAxis = 1;
    const double colGap = ResolveSize(s.columnGap, 0);
    grid.gap[1] = Known(colGap) ? colGap : 0;
    grid.gap[0] = 0;
    std::vector<Box*> boxes;
    for (auto& c : container.children) if (!c->style->IsOutOfFlow()) boxes.push_back(c.get());
    grid.items.resize(boxes.size());
    for (size_t i = 0; i < boxes.size(); ++i) grid.items[i].box = boxes[i];
    grid.ParseTemplates(kNaN, kNaN);
    grid.tracks[0].clear();
    grid.explicitCount[0] = 0;
    int before = 0;
    for (GridItem& it : grid.items) {
      grid.ResolveLines(it);
      it.definite[0] = false;
      it.span[0] = 1;
      it.pos[0] = 0;
      if (it.definite[1]) before = std::max(before, -it.pos[1]);
    }
    grid.implicitBefore[1] = before;
    int needed = std::max(1, grid.explicitCount[1] + before);
    for (GridItem& it : grid.items) {
      if (it.definite[1]) it.pos[1] += before;
      needed = std::max(needed, it.definite[1] ? it.pos[1] + it.span[1] : it.span[1]);
    }
    grid.BuildTracks(1, needed);
    const int lanes = static_cast<int>(grid.tracks[1].size());
    for (GridItem& it : grid.items) grid.ItemEdges(it, 0);
    for (GridItem& it : grid.items) {
      if (it.definite[1]) continue;
      it.lanesAuto = true;
      for (int w = 0; w + it.span[1] <= lanes; ++w) {
        GridItem v = it;
        v.lanesAuto = false;
        v.onlyAxis = 1;
        v.pos[1] = w;
        grid.virtualItems.push_back(v);
      }
    }
    for (GridItem& it : grid.items) if (!it.lanesAuto) grid.ColumnContributions(it, kNaN);
    for (GridItem& v : grid.virtualItems) grid.ColumnContributions(v, kNaN);
    grid.SizeTracks(1, kNaN, pass == 1);
    (pass == 0 ? minContent : maxContent) = grid.TrackSum(1);
  }
}

void GridContentSizes(LayoutContext& lc, Box& container, double& minContent, double& maxContent) {
  const BoxStyle& s = *container.style;
  if (s.gridLanes) {
    const std::string direction = Lower(s.gridLanesDirection);
    if (direction.rfind("row", 0) == 0 || (direction.rfind("normal", 0) == 0 && Lower(s.gridTemplateColumns) == "none" && Lower(s.gridTemplateRows) != "none")) {
      GridLanesRowSizes(lc, container, minContent, maxContent);
    } else {
      GridLanesColumnSizes(lc, container, minContent, maxContent);
    }
    return;
  }
  for (int pass = 0; pass < 2; ++pass) {
    Grid grid(lc, container, 0, kNaN);
    const double colGap = ResolveSize(s.columnGap, 0);
    grid.gap[1] = Known(colGap) ? colGap : 0;
    grid.gap[0] = 0;
    std::vector<Box*> boxes;
    for (auto& c : container.children) if (!c->style->IsOutOfFlow()) boxes.push_back(c.get());
    grid.items.resize(boxes.size());
    for (size_t i = 0; i < boxes.size(); ++i) grid.items[i].box = boxes[i];
    grid.ParseTemplates(kNaN, kNaN);
    grid.Place();
    for (GridItem& it : grid.items) grid.ItemEdges(it, 0);
    for (GridItem& it : grid.items) grid.ColumnContributions(it, kNaN);
    grid.ComputeBaselines(1, [](const GridItem&) { return kNaN; }, [](const GridItem& it) { return it.maxContentContribution - it.marginStart[1] - it.marginEnd[1]; });
    for (GridItem& it : grid.items) {
      it.minContribution += it.shim[1];
      it.minContentContribution += it.shim[1];
      it.maxContentContribution += it.shim[1];
    }
    // (Columns that are percentages of a width that is not known are auto ones.)
    if (!grid.subgridded[1]) {
      for (Track& t : grid.tracks[1]) {
        for (Fn* f : {&t.def.min, &t.def.max}) if (f->k == Fn::K::Percent || !f->calc.empty()) *f = Fn();
      }
    }
    grid.SizeTracks(1, kNaN, pass == 1);
    (pass == 0 ? minContent : maxContent) = grid.TrackSum(1);
  }
}

}  // namespace solar::layout
