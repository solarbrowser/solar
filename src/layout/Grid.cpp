// Grid layout (https://www.w3.org/TR/css-grid-1/#layout-algorithm), for the horizontal writing mode.
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>

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
        t.max.calc = limit.k == Fn::K::Percent ? "%" + std::to_string(limit.value) : "";
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
};

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
        int count = 1;
        if (Known(available[axis]) && repeatSize + gaps[axis] * tmpl.repeated.tracks.size() > 0) {
          const double perRepeat = repeatSize + gaps[axis] * tmpl.repeated.tracks.size();
          const double room = available[axis] - fixed - gaps[axis] * count0;
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
        while (tracks[axis].size() + 1 < need) {
          Track t;
          t.implicit = false;
          t.def.min = t.def.max = Fn();
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
      int needed = a == 0 ? rows : cols;
      for (const GridItem& it : items) needed = std::max(needed, it.pos[a] + it.span[a]);
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
  }

  // ---- Items ----
  void ItemEdges(GridItem& it, double cb) {
    Box& b = *it.box;
    const BoxStyle& s = *b.style;
    b.border = {s.border[0], s.border[1], s.border[2], s.border[3]};
    b.padding = {std::max(0.0, s.padding[0].Resolve(cb)), std::max(0.0, s.padding[1].Resolve(cb)), std::max(0.0, s.padding[2].Resolve(cb)), std::max(0.0, s.padding[3].Resolve(cb))};
    it.padBorder[0] = b.border.Vertical() + b.padding.Vertical();
    it.padBorder[1] = b.border.Horizontal() + b.padding.Horizontal();
    const int starts[2] = {0, 3}, ends[2] = {2, 1};  // top/bottom, left/right
    for (int a = 0; a < 2; ++a) {
      it.autoMarginStart[a] = s.margin[starts[a]].IsAuto();
      it.autoMarginEnd[a] = s.margin[ends[a]].IsAuto();
      it.marginStart[a] = it.autoMarginStart[a] ? 0 : s.margin[starts[a]].Resolve(cb);
      it.marginEnd[a] = it.autoMarginEnd[a] ? 0 : s.margin[ends[a]].Resolve(cb);
    }
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

  // The size property of the item in an axis resolved to a content-box length, or NaN.
  double SpecifiedSize(const GridItem& it, int axis, double basis) const {
    const BoxStyle& s = *it.box->style;
    const Length& l = axis == 0 ? s.height : s.width;
    double v = ResolveSize(l, basis);
    if (!Known(v)) return kNaN;
    return s.boxSizing == BoxSizing::BorderBox ? std::max(0.0, v - it.padBorder[axis]) : v;
  }

  // The contributions of an item to the tracks it spans in the column axis.
  void ColumnContributions(GridItem& it, double columnBasis) {
    Box& b = *it.box;
    const BoxStyle& s = *b.style;
    ComputeContentSizes(lc, b);
    double mn = b.minContent, mx = b.maxContent;
    const double specified = SpecifiedSize(it, 1, columnBasis);
    if (Known(specified)) mn = mx = specified;
    else if (s.width.IsPercentage()) mn = 0;
    const double maxW = SpecifiedSize(it, 1, columnBasis);
    (void)maxW;
    const Length& maxLen = s.maxWidth;
    const double maxV = ResolveSize(maxLen, columnBasis);
    if (Known(maxV)) { const double m = s.boxSizing == BoxSizing::BorderBox ? std::max(0.0, maxV - it.padBorder[1]) : maxV; mn = std::min(mn, m); mx = std::min(mx, m); }
    const double minV = ResolveSize(s.minWidth, columnBasis);
    if (Known(minV)) { const double m = s.boxSizing == BoxSizing::BorderBox ? std::max(0.0, minV - it.padBorder[1]) : minV; mn = std::max(mn, m); mx = std::max(mx, m); }
    const double extras = it.padBorder[1] + it.marginStart[1] + it.marginEnd[1];
    it.minContentContribution = mn + extras;
    it.maxContentContribution = mx + extras;
    // The automatic minimum size.
    double minimum;
    const bool scroller = s.overflowX != Overflow::Visible && s.overflowX != Overflow::Clip;
    if (Known(minV)) minimum = std::max(0.0, minV);
    else if (s.minWidth.IsAuto() && !scroller) minimum = Known(specified) ? std::min(specified, b.minContent) : b.minContent;
    else minimum = 0;
    it.minContribution = minimum + extras;
  }

  // The contributions to rows: the item laid out at the width its columns give it.
  void RowContributions(GridItem& it, double areaWidth) {
    Box& b = *it.box;
    const BoxStyle& s = *b.style;
    const double extras = it.padBorder[0] + it.marginStart[0] + it.marginEnd[0];
    double height;
    const double specified = SpecifiedSize(it, 0, heightBasis);
    if (Known(specified)) {
      height = specified;
    } else {
      LayoutItemAt(it, areaWidth, kNaN);
      height = b.height - it.padBorder[0];
    }
    it.minContentContribution = it.maxContentContribution = height + extras;
    double minimum;
    const double minV = ResolveSize(s.minHeight, heightBasis);
    const bool scroller = s.overflowY != Overflow::Visible && s.overflowY != Overflow::Clip;
    if (Known(minV)) minimum = minV;
    else if (s.minHeight.IsAuto() && !scroller) minimum = Known(specified) ? std::min(specified, height) : height;
    else minimum = 0;
    it.minContribution = minimum + extras;
    const double maxV = ResolveSize(s.maxHeight, heightBasis);
    if (Known(maxV)) { it.minContentContribution = std::min(it.minContentContribution, maxV + extras); it.maxContentContribution = std::min(it.maxContentContribution, maxV + extras); }
  }

  // Lays the item out in a grid area of the given width (the content width its alignment gives it), to measure it.
  void LayoutItemAt(GridItem& it, double areaWidth, double forceHeight) {
    Box& b = *it.box;
    const BoxStyle& s = *b.style;
    const Align::Kind jk = SelfAlign(it, 1);
    double forceWidth = kNaN;
    const double specified = SpecifiedSize(it, 1, areaWidth);
    if (!Known(specified) && !it.autoMarginStart[1] && !it.autoMarginEnd[1] && (jk == Align::Kind::Normal || jk == Align::Kind::Stretch) && !b.replaced) {
      forceWidth = std::max(0.0, areaWidth - it.marginStart[1] - it.marginEnd[1] - it.padBorder[1]);
    }
    lc.forceWidth = forceWidth;
    lc.forceHeight = forceHeight;
    lc.cbX = 0;
    lc.boxY = 0;
    (void)s;
    LayoutBlockLevel(lc, b, areaWidth, heightBasis, true);
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
    std::vector<GridItem*> order;
    for (GridItem& it : items) order.push_back(&it);
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
    for (int phase = 0; phase < 4; ++phase) {
      for (GridItem* it : order) {
        if (spansFlexible(*it) && phase != 3) continue;
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
              if (pass == 0 && k == Fn::K::Auto) { which.push_back(i); contribution = it->minContribution; }
              else if (pass == 1 && k == Fn::K::MinContent) { which.push_back(i); contribution = it->minContentContribution; }
              else if (pass == 2 && k == Fn::K::MaxContent) { which.push_back(i); contribution = it->maxContentContribution; }
            }
            if (which.empty()) continue;
            double current = g * (it->span[axis] - 1);
            for (int i = it->pos[axis]; i < it->pos[axis] + it->span[axis]; ++i) current += ts[i].base;
            distribute(contribution - current, which, true, false);
          }
        } else if (phase == 2) {
          // maximums: min-content max → min-content; max-content max (and auto) → max-content
          std::vector<int> which;
          for (int i : intrinsicMax) which.push_back(i);
          if (which.empty()) continue;
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
          // items crossing flexible tracks add to those tracks' bases by their contributions (fr is then found from them)
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
        else if (Known(available)) limitValue = available * t.def.max.value / 100;
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
        for (GridItem& it : items) {
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

  void Positions(int axis, double offset) {
    double pos = offset;
    for (Track& t : tracks[axis]) {
      t.position = pos;
      pos += t.base + gap[axis];
    }
  }
};

}  // namespace

void LayoutGrid(LayoutContext& lc, Box& container, double contentWidth, double heightBasis, double& contentHeight) {
  Grid grid(lc, container, contentWidth, heightBasis);
  const BoxStyle& s = *container.style;
  const double colGap = ResolveSize(s.columnGap, contentWidth), rowGap = ResolveSize(s.rowGap, heightBasis);
  grid.gap[1] = Known(colGap) ? colGap : 0;
  grid.gap[0] = Known(rowGap) ? rowGap : 0;
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
  for (size_t i = 0; i < boxes.size(); ++i) grid.items[i].box = boxes[i];
  grid.ParseTemplates(contentWidth, heightBasis);
  grid.Place();
  for (GridItem& it : grid.items) grid.ItemEdges(it, contentWidth);

  // Columns.
  for (GridItem& it : grid.items) grid.ColumnContributions(it, contentWidth);
  grid.SizeTracks(1, contentWidth, false);
  // The widths of the item areas, for the rows.
  const auto areaSize = [&](const GridItem& it, int axis) {
    double size = grid.gap[axis] * (it.span[axis] - 1);
    for (int i = it.pos[axis]; i < it.pos[axis] + it.span[axis]; ++i) size += grid.tracks[axis][i].base;
    return size;
  };
  // Rows.
  for (GridItem& it : grid.items) grid.RowContributions(it, areaSize(it, 1));
  const double rowsAvailable = heightBasis;
  grid.SizeTracks(0, rowsAvailable, false);
  // align/justify-content positions.
  const auto contentOffset = [&](int axis, double available) {
    const double used = grid.TrackSum(axis);
    const double free = Known(available) ? available - used : 0;
    Align a = axis == 1 ? s.justifyContent : s.alignContent;
    Align::Kind k = a.kind;
    double offset = 0, between = 0;
    const double n = static_cast<double>(grid.tracks[axis].size());
    if (k == Align::Kind::SpaceBetween && free < 0) k = Align::Kind::Start;
    if ((k == Align::Kind::SpaceAround || k == Align::Kind::SpaceEvenly) && free < 0) k = Align::Kind::Center;
    if (a.safe && free < 0 && (k == Align::Kind::Center || k == Align::Kind::End || k == Align::Kind::FlexEnd)) k = Align::Kind::Start;
    switch (k) {
      case Align::Kind::End: case Align::Kind::FlexEnd: offset = free; break;
      case Align::Kind::Center: offset = free / 2; break;
      case Align::Kind::Right: offset = axis == 1 ? free : 0; break;
      case Align::Kind::SpaceBetween: between = n > 1 ? free / (n - 1) : 0; break;
      case Align::Kind::SpaceAround: between = free / n; offset = between / 2; break;
      case Align::Kind::SpaceEvenly: between = free / (n + 1); offset = between; break;
      default: break;
    }
    return std::pair<double, double>{offset, between};
  };
  {
    const auto [offset, between] = contentOffset(1, contentWidth);
    double pos = offset;
    for (Track& t : grid.tracks[1]) { t.position = pos; pos += t.base + grid.gap[1] + between; }
  }
  const double usedRows = grid.TrackSum(0);
  const double rowContainer = Known(heightBasis) ? heightBasis : usedRows;
  {
    const auto [offset, between] = contentOffset(0, rowContainer);
    double pos = offset;
    for (Track& t : grid.tracks[0]) { t.position = pos; pos += t.base + grid.gap[0] + between; }
  }
  contentHeight = rowContainer;

  // Place the items.
  const double originX = container.ContentLeft(), originY = container.ContentTop();
  double firstBaseline = -1;
  // Baseline alignment: items in the same row that ask for it line up on the largest ascent.
  std::map<int, double> rowAscent;
  for (GridItem& it : grid.items) {
    if (grid.SelfAlign(it, 0) != Align::Kind::Baseline) continue;
    grid.LayoutItemAt(it, areaSize(it, 1), kNaN);
    const double base = it.box->firstBaseline >= 0 ? it.box->firstBaseline : it.box->height;
    rowAscent[it.pos[0]] = std::max(rowAscent[it.pos[0]], it.marginStart[0] + base);
  }
  for (GridItem& it : grid.items) {
    Box& b = *it.box;
    const BoxStyle& st = *b.style;
    const double areaW = areaSize(it, 1), areaH = areaSize(it, 0);
    const double areaX = grid.tracks[1][it.pos[1]].position, areaY = grid.tracks[0][it.pos[0]].position;
    double w[2];  // content sizes: [rows, columns]
    double forced[2] = {kNaN, kNaN};
    const Align::Kind kinds[2] = {grid.SelfAlign(it, 0), grid.SelfAlign(it, 1)};
    const double area[2] = {areaH, areaW};
    for (int a = 0; a < 2; ++a) {
      const double specified = grid.SpecifiedSize(it, a, a == 0 ? heightBasis : contentWidth);
      const bool stretch = (kinds[a] == Align::Kind::Normal || kinds[a] == Align::Kind::Stretch) && !it.autoMarginStart[a] && !it.autoMarginEnd[a] && !b.replaced;
      if (!Known(specified) && stretch) forced[a] = std::max(0.0, area[a] - it.marginStart[a] - it.marginEnd[a] - it.padBorder[a]);
      (void)w;
    }
    grid.lc.forceWidth = forced[1];
    grid.lc.forceHeight = forced[0];
    grid.lc.cbX = 0;
    grid.lc.boxY = 0;
    LayoutBlockLevel(lc, b, areaW, areaH, true);
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
        if (k == Align::Kind::Normal || k == Align::Kind::Stretch) k = b.replaced || Known(grid.SpecifiedSize(it, a, a == 0 ? heightBasis : contentWidth)) ? Align::Kind::Start : Align::Kind::Start;
        const bool rtl = st.direction == Direction::Rtl && a == 1;
        switch (k) {
          case Align::Kind::End: case Align::Kind::FlexEnd: case Align::Kind::SelfEnd: start += rtl ? 0 : free; break;
          case Align::Kind::Center: start += free / 2; break;
          case Align::Kind::Right: start += a == 1 ? free : 0; break;
          case Align::Kind::Baseline:
            if (a == 0) start = rowAscent[it.pos[0]] - (b.firstBaseline >= 0 ? b.firstBaseline : b.height);
            break;
          case Align::Kind::Start: case Align::Kind::SelfStart: case Align::Kind::Left: case Align::Kind::FlexStart:
            if (rtl) start += free;
            break;
          default: break;
        }
        if (free < 0 && (st.alignSelf.safe || st.justifySelf.safe)) start = it.marginStart[a];
      }
      offsets[a] = start;
    }
    b.margin.top = it.marginStart[0];
    b.margin.bottom = it.marginEnd[0];
    b.margin.left = it.marginStart[1];
    b.margin.right = it.marginEnd[1];
    b.x = originX + areaX + offsets[1] + b.shiftX;
    b.y = originY + areaY + offsets[0] + b.shiftY;
    if (firstBaseline < 0 && it.pos[0] == 0 && b.firstBaseline >= 0) firstBaseline = b.y - b.shiftY - originY + b.firstBaseline;
  }
  container.firstBaseline = firstBaseline >= 0 ? firstBaseline + originY : -1;
  container.baseline = container.firstBaseline;
}

void GridContentSizes(LayoutContext& lc, Box& container, double& minContent, double& maxContent) {
  const BoxStyle& s = *container.style;
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
    grid.SizeTracks(1, kNaN, pass == 1);
    (pass == 0 ? minContent : maxContent) = grid.TrackSum(1);
  }
}

}  // namespace solar::layout
