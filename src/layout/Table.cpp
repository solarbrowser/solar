// Table layout (https://www.w3.org/TR/CSS22/tables.html): the grid of cells, column widths (automatic and fixed), row heights,
// vertical alignment of cells and collapsing borders.
#include <algorithm>
#include <cmath>
#include <functional>

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

bool IsRowGroup(Display d) { return d == Display::TableRowGroup || d == Display::TableHeaderGroup || d == Display::TableFooterGroup; }

int AttributeInt(const Box& box, const char* name, int fallback, int lo, int hi) {
  const dom::Element* element = box.element();
  if (!element) return fallback;
  const dom::Attr* a = element->FindAttribute(name);
  if (!a) return fallback;
  char* end = nullptr;
  const long v = std::strtol(a->value.c_str(), &end, 10);
  if (end == a->value.c_str()) return fallback;
  return static_cast<int>(std::max<long>(lo, std::min<long>(hi, v)));
}

struct Cell {
  Box* box = nullptr;
  Box* row = nullptr;
  int rowIndex = 0, colIndex = 0, rowSpan = 1, colSpan = 1;
  double minWidth = 0, maxWidth = 0;   // border-box (content sizes + padding + border)
  double fixedWidth = kNaN;            // border-box
  double percent = kNaN;
  double height = 0;                   // laid out, border box
  double baseline = -1;
};

struct Column {
  double min = 0, max = 0;
  double fixed = kNaN;     // border-box width asked for
  double percent = kNaN;
  double width = 0;
};

struct RowInfo {
  Box* box = nullptr;
  Box* group = nullptr;
  double height = 0;
  double requested = 0;
  double baseline = 0;
  double y = 0;
};

struct Model {
  Box& table;
  std::vector<Cell> cells;
  std::vector<RowInfo> rows;
  std::vector<Column> columns;
  std::vector<Box*> captions;
  std::vector<Box*> groups;      // row groups in order
  bool collapse;
  double spacingH, spacingV;
  int numCols = 0;

  explicit Model(Box& t) : table(t), collapse(t.style->borderCollapse), spacingH(collapse ? 0 : t.style->borderSpacingH), spacingV(collapse ? 0 : t.style->borderSpacingV) {}

  void Build() {
    // Header groups first, then bodies and rows, then footers.
    std::vector<Box*> ordered;
    std::vector<Box*> headers, footers, others;
    std::vector<std::pair<Box*, std::vector<Box*>>> columnGroups;
    for (auto& c : table.children) {
      const Display d = c->style->display;
      if (d == Display::TableCaption) captions.push_back(c.get());
      else if (d == Display::TableHeaderGroup) headers.push_back(c.get());
      else if (d == Display::TableFooterGroup) footers.push_back(c.get());
      else if (IsRowGroup(d) || d == Display::TableRow) others.push_back(c.get());
    }
    // (Only the first header and first footer are placed at the ends.)
    std::vector<Box*> body;
    if (!headers.empty()) body.push_back(headers[0]);
    for (size_t i = 1; i < headers.size(); ++i) others.insert(others.begin(), headers[i]);
    for (Box* o : others) body.push_back(o);
    if (!footers.empty()) {
      for (size_t i = 0; i + 1 < footers.size(); ++i) body.push_back(footers[i]);
      body.push_back(footers.back());
    }
    std::vector<std::vector<char>> occupied;
    const auto addRow = [&](Box* row, Box* group) {
      RowInfo info;
      info.box = row;
      info.group = group;
      rows.push_back(info);
      const int r = static_cast<int>(rows.size()) - 1;
      if (static_cast<int>(occupied.size()) <= r) occupied.resize(r + 1);
      int col = 0;
      for (auto& c : row->children) {
        if (c->style->display != Display::TableCell) continue;
        while (col < static_cast<int>(occupied[r].size()) && occupied[r][col]) ++col;
        Cell cell;
        cell.box = c.get();
        cell.row = row;
        cell.rowIndex = r;
        cell.colIndex = col;
        cell.colSpan = AttributeInt(*c, "colspan", 1, 1, 1000);
        cell.rowSpan = AttributeInt(*c, "rowspan", 1, 0, 65534);
        if (cell.rowSpan == 0) cell.rowSpan = 1;  // (to the end of the group: settled below)
        for (int i = 0; i < cell.rowSpan; ++i) {
          if (static_cast<int>(occupied.size()) <= r + i) occupied.resize(r + i + 1);
          if (static_cast<int>(occupied[r + i].size()) < col + cell.colSpan) occupied[r + i].resize(col + cell.colSpan, 0);
          for (int j = 0; j < cell.colSpan; ++j) occupied[r + i][col + j] = 1;
        }
        numCols = std::max(numCols, col + cell.colSpan);
        col += cell.colSpan;
        cells.push_back(cell);
      }
    };
    for (Box* b : body) {
      if (b->style->display == Display::TableRow) {
        addRow(b, nullptr);
      } else {
        groups.push_back(b);
        for (auto& r : b->children) if (r->style->display == Display::TableRow) addRow(r.get(), b);
      }
    }
    // Column elements give widths to columns that have no cells to say otherwise.
    int col = 0;
    columns.assign(numCols, Column());
    const std::function<void(Box&)> cols = [&](Box& parent) {
      for (auto& c : parent.children) {
        if (c->style->display == Display::TableColumnGroup) {
          const int before = col;
          cols(*c);
          if (col == before) col += AttributeInt(*c, "span", 1, 1, 1000);
        } else if (c->style->display == Display::TableColumn) {
          const int span = AttributeInt(*c, "span", 1, 1, 1000);
          const double w = ResolveSize(c->style->width, kNaN);
          for (int i = 0; i < span; ++i) {
            if (col + i >= static_cast<int>(columns.size())) columns.resize(col + i + 1);
            if (Known(w)) columns[col + i].fixed = w;
            else if (c->style->width.kind == Length::Kind::Percent) columns[col + i].percent = c->style->width.value;
          }
          col += span;
        }
      }
    };
    cols(table);
    numCols = std::max<int>(numCols, static_cast<int>(columns.size()));
    columns.resize(numCols);
  }
};

double PaddingBorderH(const Box& b) { return b.border.Horizontal() + b.padding.Horizontal(); }

// ---- Collapsed borders (https://www.w3.org/TR/CSS22/tables.html#collapsing-borders) ----

struct EdgeBorder {
  double width = 0;
  BorderStyle style = BorderStyle::None;
  int rank = 0;  // source: cell 5, row 4, row group 3, column 2, column group 1, table 0
};

int StyleRank(BorderStyle s) {
  switch (s) {
    case BorderStyle::Double: return 7;
    case BorderStyle::Solid: return 6;
    case BorderStyle::Dashed: return 5;
    case BorderStyle::Dotted: return 4;
    case BorderStyle::Ridge: return 3;
    case BorderStyle::Outset: return 2;
    case BorderStyle::Groove: return 1;
    case BorderStyle::Inset: return 0;
    default: return -1;
  }
}

EdgeBorder Better(const EdgeBorder& a, const EdgeBorder& b) {
  if (a.style == BorderStyle::Hidden) return a;
  if (b.style == BorderStyle::Hidden) return b;
  const bool aNone = a.style == BorderStyle::None, bNone = b.style == BorderStyle::None;
  if (aNone) return b;
  if (bNone) return a;
  if (a.width != b.width) return a.width > b.width ? a : b;
  if (StyleRank(a.style) != StyleRank(b.style)) return StyleRank(a.style) > StyleRank(b.style) ? a : b;
  return a.rank >= b.rank ? a : b;
}

EdgeBorder EdgeOf(const BoxStyle& s, int side, int rank) {
  EdgeBorder e;
  e.style = s.borderStyleRaw[side];
  e.width = (e.style == BorderStyle::None || e.style == BorderStyle::Hidden) ? 0 : s.borderWidthRaw[side];
  e.rank = rank;
  return e;
}

void ShiftContents(Box& box, double dy) {
  for (auto& c : box.children) {
    if (c->style->IsOutOfFlow() && !c->style->IsFloating()) continue;
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

}  // namespace

void TableContentSizes(LayoutContext& lc, Box& table, double& minContent, double& maxContent) {
  Model model(table);
  model.Build();
  const bool collapse = model.collapse;
  (void)collapse;
  // Cells' edges.
  for (Cell& cell : model.cells) {
    Box& b = *cell.box;
    const BoxStyle& s = *b.style;
    b.border = {s.border[0], s.border[1], s.border[2], s.border[3]};
    b.padding = {std::max(0.0, s.padding[0].Resolve(0)), std::max(0.0, s.padding[1].Resolve(0)), std::max(0.0, s.padding[2].Resolve(0)), std::max(0.0, s.padding[3].Resolve(0))};
    ComputeContentSizes(lc, b);
    const double extras = PaddingBorderH(b);
    cell.minWidth = b.minContent + extras;
    cell.maxWidth = b.maxContent + extras;
    const double w = ResolveSize(s.width, kNaN);
    if (Known(w)) {
      const double bw = s.boxSizing == BoxSizing::BorderBox ? w : w + extras;
      cell.fixedWidth = bw;
      cell.maxWidth = std::max(cell.minWidth, bw);
    }
  }
  for (Cell& cell : model.cells) {
    if (cell.colSpan != 1) continue;
    Column& col = model.columns[cell.colIndex];
    col.min = std::max(col.min, cell.minWidth);
    col.max = std::max(col.max, cell.maxWidth);
    if (Known(cell.fixedWidth)) col.fixed = Known(col.fixed) ? std::max(col.fixed, cell.fixedWidth) : cell.fixedWidth;
  }
  for (Cell& cell : model.cells) {
    if (cell.colSpan == 1) continue;
    double have = 0, haveMax = 0;
    for (int i = 0; i < cell.colSpan; ++i) { have += model.columns[cell.colIndex + i].min; haveMax += model.columns[cell.colIndex + i].max; }
    const double spacing = model.spacingH * (cell.colSpan - 1);
    if (cell.minWidth - spacing > have) for (int i = 0; i < cell.colSpan; ++i) model.columns[cell.colIndex + i].min += (cell.minWidth - spacing - have) / cell.colSpan;
    if (cell.maxWidth - spacing > haveMax) for (int i = 0; i < cell.colSpan; ++i) model.columns[cell.colIndex + i].max += (cell.maxWidth - spacing - haveMax) / cell.colSpan;
  }
  double mn = model.spacingH * (model.numCols + 1), mx = mn;
  for (Column& c : model.columns) {
    if (Known(c.fixed)) { c.min = std::max(c.min, 0.0); c.max = std::max(c.min, Known(c.fixed) ? c.fixed : c.max); }
    mn += c.min;
    mx += std::max(c.min, c.max);
  }
  minContent = mn;
  maxContent = mx;
}

void LayoutTable(LayoutContext& lc, Box& table, double contentWidth, double heightBasis, double& contentHeight) {
  Model model(table);
  model.Build();
  const BoxStyle& ts = *table.style;
  const bool collapse = model.collapse;
  const double hs = model.spacingH, vs = model.spacingV;
  const int cols = model.numCols;
  const int nrows = static_cast<int>(model.rows.size());

  // ---- Borders ----
  if (collapse && cols > 0 && nrows > 0) {
    // Edge widths of the collapsed grid.
    std::vector<std::vector<EdgeBorder>> horizontal(nrows + 1, std::vector<EdgeBorder>(cols));  // above row r, column c
    std::vector<std::vector<EdgeBorder>> vertical(nrows, std::vector<EdgeBorder>(cols + 1));    // left of column c in row r
    for (Cell& cell : model.cells) {
      const BoxStyle& s = *cell.box->style;
      const int last = std::min(nrows, cell.rowIndex + cell.rowSpan);
      for (int c = cell.colIndex; c < cell.colIndex + cell.colSpan && c < cols; ++c) {
        horizontal[cell.rowIndex][c] = Better(horizontal[cell.rowIndex][c], EdgeOf(s, 0, 5));
        horizontal[last][c] = Better(horizontal[last][c], EdgeOf(s, 2, 5));
      }
      for (int r = cell.rowIndex; r < last; ++r) {
        vertical[r][cell.colIndex] = Better(vertical[r][cell.colIndex], EdgeOf(s, 3, 5));
        vertical[r][std::min(cols, cell.colIndex + cell.colSpan)] = Better(vertical[r][std::min(cols, cell.colIndex + cell.colSpan)], EdgeOf(s, 1, 5));
      }
    }
    // Rows and row groups contribute their borders along the rows.
    for (int r = 0; r < nrows; ++r) {
      const BoxStyle& rs = *model.rows[r].box->style;
      for (int c = 0; c < cols; ++c) {
        horizontal[r][c] = Better(horizontal[r][c], EdgeOf(rs, 0, 4));
        horizontal[r + 1][c] = Better(horizontal[r + 1][c], EdgeOf(rs, 2, 4));
      }
      vertical[r][0] = Better(vertical[r][0], EdgeOf(rs, 3, 4));
      vertical[r][cols] = Better(vertical[r][cols], EdgeOf(rs, 1, 4));
    }
    // The table's own borders at the outside.
    for (int c = 0; c < cols; ++c) {
      horizontal[0][c] = Better(horizontal[0][c], EdgeOf(ts, 0, 0));
      horizontal[nrows][c] = Better(horizontal[nrows][c], EdgeOf(ts, 2, 0));
    }
    for (int r = 0; r < nrows; ++r) {
      vertical[r][0] = Better(vertical[r][0], EdgeOf(ts, 3, 0));
      vertical[r][cols] = Better(vertical[r][cols], EdgeOf(ts, 1, 0));
    }
    for (Cell& cell : model.cells) {
      Box& b = *cell.box;
      const int last = std::min(nrows, cell.rowIndex + cell.rowSpan);
      double top = 0, bottom = 0, left = 0, right = 0;
      for (int c = cell.colIndex; c < cell.colIndex + cell.colSpan && c < cols; ++c) {
        top = std::max(top, horizontal[cell.rowIndex][c].width);
        bottom = std::max(bottom, horizontal[last][c].width);
      }
      for (int r = cell.rowIndex; r < last; ++r) {
        left = std::max(left, vertical[r][cell.colIndex].width);
        right = std::max(right, vertical[r][std::min(cols, cell.colIndex + cell.colSpan)].width);
      }
      b.border = {top / 2, right / 2, bottom / 2, left / 2};
    }
    // The table's border is half the widest of its outer collapsed edges.
    double top = 0, bottom = 0, left = 0, right = 0;
    for (int c = 0; c < cols; ++c) { top = std::max(top, horizontal[0][c].width); bottom = std::max(bottom, horizontal[nrows][c].width); }
    for (int r = 0; r < nrows; ++r) { left = std::max(left, vertical[r][0].width); right = std::max(right, vertical[r][cols].width); }
    table.border = {top / 2, right / 2, bottom / 2, left / 2};
  }
  for (Cell& cell : model.cells) {
    Box& b = *cell.box;
    const BoxStyle& s = *b.style;
    if (!collapse) b.border = {s.border[0], s.border[1], s.border[2], s.border[3]};
    b.padding = {std::max(0.0, s.padding[0].Resolve(contentWidth)), std::max(0.0, s.padding[1].Resolve(contentWidth)), std::max(0.0, s.padding[2].Resolve(contentWidth)),
                 std::max(0.0, s.padding[3].Resolve(contentWidth))};
    b.margin = Edges();
  }

  // ---- Column widths ----
  double fixedMin = 0;
  double minSum = 0, maxSum = 0;
  {
    double mn, mx;
    // (the cells' content sizes are cached on their boxes)
    for (Cell& cell : model.cells) {
      Box& b = *cell.box;
      b.minContent = b.maxContent = -1;
      ComputeContentSizes(lc, b);
      const double extras = PaddingBorderH(b);
      cell.minWidth = b.minContent + extras;
      cell.maxWidth = b.maxContent + extras;
      const double w = ResolveSize(b.style->width, contentWidth);
      if (Known(w)) {
        cell.fixedWidth = b.style->boxSizing == BoxSizing::BorderBox ? w : w + extras;
        cell.maxWidth = std::max(cell.minWidth, cell.fixedWidth);
      } else if (b.style->width.kind == Length::Kind::Percent) {
        cell.percent = b.style->width.value;
      }
    }
    (void)mn; (void)mx; (void)fixedMin;
  }
  const double available = std::max(0.0, contentWidth - hs * (cols + 1));
  if (ts.tableLayoutFixed && !ts.width.IsAuto()) {
    // The first row (and the column elements) decide.
    for (Cell& cell : model.cells) {
      if (cell.rowIndex != 0) continue;
      const double w = Known(cell.fixedWidth) ? cell.fixedWidth : kNaN;
      for (int i = 0; i < cell.colSpan; ++i) {
        Column& c = model.columns[cell.colIndex + i];
        if (!Known(c.fixed) && Known(w)) c.fixed = w / cell.colSpan - (i == 0 ? 0 : 0);
        if (!Known(c.fixed) && Known(cell.percent)) c.percent = cell.percent / cell.colSpan;
      }
    }
    double used = 0;
    int autos = 0;
    for (Column& c : model.columns) {
      if (Known(c.fixed)) { c.width = c.fixed; used += c.width; }
      else if (Known(c.percent)) { c.width = available * c.percent / 100; used += c.width; }
      else ++autos;
    }
    for (Column& c : model.columns) if (!Known(c.fixed) && !Known(c.percent)) c.width = autos ? std::max(0.0, available - used) / autos : 0;
    if (autos == 0 && used < available && cols > 0) for (Column& c : model.columns) c.width += (available - used) / cols;
  } else {
    for (Cell& cell : model.cells) {
      if (cell.colSpan != 1) continue;
      Column& col = model.columns[cell.colIndex];
      col.min = std::max(col.min, cell.minWidth);
      col.max = std::max(col.max, cell.maxWidth);
      if (Known(cell.fixedWidth)) col.fixed = Known(col.fixed) ? std::max(col.fixed, cell.fixedWidth) : cell.fixedWidth;
      if (Known(cell.percent)) col.percent = Known(col.percent) ? std::max(col.percent, cell.percent) : cell.percent;
    }
    for (Cell& cell : model.cells) {
      if (cell.colSpan == 1) continue;
      double have = 0, haveMax = 0;
      for (int i = 0; i < cell.colSpan; ++i) { have += model.columns[cell.colIndex + i].min; haveMax += model.columns[cell.colIndex + i].max; }
      const double spacing = hs * (cell.colSpan - 1);
      if (cell.minWidth - spacing > have) for (int i = 0; i < cell.colSpan; ++i) model.columns[cell.colIndex + i].min += (cell.minWidth - spacing - have) / cell.colSpan;
      if (cell.maxWidth - spacing > haveMax) for (int i = 0; i < cell.colSpan; ++i) model.columns[cell.colIndex + i].max += (cell.maxWidth - spacing - haveMax) / cell.colSpan;
    }
    for (Column& c : model.columns) {
      if (Known(c.fixed)) c.max = std::max(c.min, c.fixed);
      c.max = std::max(c.max, c.min);
      minSum += c.min;
      maxSum += c.max;
    }
    // Percent columns take their share first.
    double percentTotal = 0;
    for (Column& c : model.columns) if (Known(c.percent)) percentTotal += c.percent;
    if (percentTotal > 100) {
      for (Column& c : model.columns) if (Known(c.percent)) c.percent = c.percent * 100 / percentTotal;
      percentTotal = 100;
    }
    double usedByPercent = 0;
    for (Column& c : model.columns) {
      if (Known(c.percent)) {
        c.width = std::max(c.min, available * c.percent / 100);
        usedByPercent += c.width;
      }
    }
    // The rest share what is left, between their minimums and maximums.
    double restMin = 0, restMax = 0;
    for (Column& c : model.columns) if (!Known(c.percent)) { restMin += c.min; restMax += c.max; }
    const double room = std::max(0.0, available - usedByPercent);
    for (Column& c : model.columns) {
      if (Known(c.percent)) continue;
      if (room >= restMax) {
        // everything at its maximum, and the extra to the columns that may grow, in proportion
        double growable = 0;
        for (Column& d : model.columns) if (!Known(d.percent) && !Known(d.fixed)) growable += d.max;
        double extra = room - restMax;
        if (growable > 0 && !Known(c.fixed)) c.width = c.max + extra * (c.max / growable);
        else if (growable <= 0) {
          int open = 0;
          for (Column& d : model.columns) if (!Known(d.percent)) ++open;
          c.width = c.max + extra / std::max(1, open);
        } else c.width = c.max;
      } else if (room <= restMin) {
        c.width = c.min;
      } else {
        const double t = (room - restMin) / (restMax - restMin);
        c.width = c.min + (c.max - c.min) * t;
      }
    }
  }
  double sumWidth = 0;
  for (Column& c : model.columns) sumWidth += c.width;

  // ---- Cells ----
  std::vector<double> colX(cols + 1, 0);
  {
    double x = hs;
    for (int i = 0; i < cols; ++i) {
      colX[i] = x;
      x += model.columns[i].width + hs;
    }
    colX[cols] = x;
  }
  const double gridWidth = cols ? colX[cols] : hs * 2;
  for (Cell& cell : model.cells) {
    Box& b = *cell.box;
    double w = hs * (cell.colSpan - 1);
    for (int i = 0; i < cell.colSpan && cell.colIndex + i < cols; ++i) w += model.columns[cell.colIndex + i].width;
    lc.forceWidth = std::max(0.0, w - PaddingBorderH(b));
    lc.cbX = 0;
    lc.boxY = 0;
    const double specifiedHeight = ResolveSize(b.style->height, heightBasis);
    lc.forceHeight = kNaN;
    LayoutBlockLevel(lc, b, contentWidth, kNaN, true);
    cell.height = b.height;
    if (Known(specifiedHeight)) {
      const double extras = b.border.Vertical() + b.padding.Vertical();
      const double target = b.style->boxSizing == BoxSizing::BorderBox ? specifiedHeight : specifiedHeight + extras;
      cell.height = std::max(cell.height, target);
    }
    cell.baseline = b.firstBaseline >= 0 ? b.firstBaseline : -1;
  }
  // ---- Row heights ----
  for (int r = 0; r < nrows; ++r) {
    RowInfo& row = model.rows[r];
    const double rowHeight = ResolveSize(row.box->style->height, heightBasis);
    row.requested = Known(rowHeight) ? rowHeight : 0;
    row.height = row.requested;
    double above = 0;
    for (const Cell& cell : model.cells) {
      if (cell.rowIndex != r || cell.rowSpan != 1) continue;
      row.height = std::max(row.height, cell.height);
      if (cell.box->style->verticalAlign == VerticalAlign::Baseline && cell.baseline >= 0) above = std::max(above, cell.baseline);
    }
    row.baseline = above;
    // Baseline-aligned cells push the row taller when their baselines differ.
    double below = 0;
    for (const Cell& cell : model.cells) {
      if (cell.rowIndex != r || cell.rowSpan != 1) continue;
      if (cell.box->style->verticalAlign == VerticalAlign::Baseline && cell.baseline >= 0) below = std::max(below, cell.height - cell.baseline);
    }
    if (above > 0 || below > 0) row.height = std::max(row.height, above + below);
  }
  for (const Cell& cell : model.cells) {
    if (cell.rowSpan == 1) continue;
    const int last = std::min(nrows, cell.rowIndex + cell.rowSpan);
    double have = vs * (last - cell.rowIndex - 1);
    for (int r = cell.rowIndex; r < last; ++r) have += model.rows[r].height;
    if (cell.height > have) {
      const double extra = (cell.height - have) / (last - cell.rowIndex);
      for (int r = cell.rowIndex; r < last; ++r) model.rows[r].height += extra;
    }
  }
  // ---- Captions ----
  double captionTop = 0, captionBottom = 0;
  std::vector<Box*> top, bottom;
  for (Box* c : model.captions) (c->style->captionBottom ? bottom : top).push_back(c);
  const double outerWidth = gridWidth;
  const auto layoutCaption = [&](Box* c) {
    lc.forceWidth = lc.forceHeight = kNaN;
    lc.cbX = 0;
    lc.boxY = 0;
    LayoutBlockLevel(lc, *c, std::max(contentWidth, outerWidth), kNaN, false);
    return c->margin.top + c->height + c->margin.bottom;
  };
  for (Box* c : top) captionTop += layoutCaption(c);
  for (Box* c : bottom) captionBottom += layoutCaption(c);

  // Extra height from the table's own.
  double rowsTotal = vs;
  for (RowInfo& row : model.rows) rowsTotal += row.height + vs;
  if (nrows == 0) rowsTotal = vs * 2;
  const double specifiedHeight = ResolveSize(ts.height, heightBasis);
  if (Known(specifiedHeight) && nrows > 0) {
    const double extras = table.border.Vertical() + table.padding.Vertical();
    const double target = (ts.boxSizing == BoxSizing::BorderBox ? specifiedHeight - extras : specifiedHeight) - captionTop - captionBottom;
    if (target > rowsTotal) {
      const double extra = target - rowsTotal;
      double sum = 0;
      for (RowInfo& row : model.rows) sum += row.height;
      for (RowInfo& row : model.rows) row.height += sum > 0 ? extra * row.height / sum : extra / nrows;
      rowsTotal = target;
    }
  }
  // ---- Place ----
  const double originX = table.ContentLeft(), originY = table.ContentTop();
  double y = vs;
  for (RowInfo& row : model.rows) {
    row.y = y;
    y += row.height + vs;
  }
  double captionY = 0;
  for (Box* c : top) {
    c->x = originX + c->margin.left;
    c->y = originY + captionY + c->margin.top;
    captionY += c->margin.top + c->height + c->margin.bottom;
  }
  const double gridTop = captionY;
  for (int r = 0; r < nrows; ++r) {
    RowInfo& row = model.rows[r];
    Box& rb = *row.box;
    rb.x = originX + hs;
    rb.y = originY + gridTop + row.y;
    rb.width = cols ? colX[cols] - 2 * hs + (cols > 0 ? 0 : 0) : 0;
    rb.width = std::max(0.0, gridWidth - 2 * hs);
    rb.height = row.height;
    rb.margin = Edges();
    rb.border = Edges();
    rb.padding = Edges();
  }
  for (Cell& cell : model.cells) {
    Box& b = *cell.box;
    const int last = std::min(nrows, cell.rowIndex + cell.rowSpan);
    double h = vs * (last - cell.rowIndex - 1);
    for (int r = cell.rowIndex; r < last; ++r) h += model.rows[r].height;
    const RowInfo& row = model.rows[cell.rowIndex];
    double dy = 0;
    // What the cell holds, as high as it is, apart from the height it was asked to be.
    double bottomOfContent = b.ContentTop();
    for (const Line& line : b.lines) bottomOfContent = std::max(bottomOfContent, line.rect.Bottom());
    for (const auto& c : b.children) {
      if (c->style->IsOutOfFlow() || c->collapsedThrough) continue;
      bottomOfContent = std::max(bottomOfContent, c->y + c->height);
    }
    const double content = bottomOfContent - b.ContentTop();
    const double inner = h - b.border.Vertical() - b.padding.Vertical();
    switch (b.style->verticalAlign) {
      case VerticalAlign::Middle: dy = (inner - content) / 2; break;
      case VerticalAlign::Bottom: dy = inner - content; break;
      case VerticalAlign::Baseline:
        if (cell.baseline >= 0 && cell.rowSpan == 1) dy = row.baseline - cell.baseline;
        break;
      default: break;
    }
    // The cell fills its slot; what it holds moves for middle, bottom and baseline.
    b.height = h;
    if (dy > 0) ShiftContents(b, dy);
    b.x = colX[cell.colIndex] - hs;
    b.y = 0;
    b.width = 0;
    double w = hs * (cell.colSpan - 1);
    for (int i = 0; i < cell.colSpan && cell.colIndex + i < cols; ++i) w += model.columns[cell.colIndex + i].width;
    b.width = w;
    (void)row;
  }
  // The cells are the rows' children: x and y within the row.
  for (Cell& cell : model.cells) {
    Box& b = *cell.box;
    b.y = 0;
    if (cell.rowIndex != 0 || true) b.x = colX[cell.colIndex] - hs;
    // a cell that spans rows starts at its own row
  }
  // Row groups cover their rows.
  for (Box* g : model.groups) {
    double top = kInf, bottom = -kInf;
    for (auto& r : g->children) {
      if (r->style->display != Display::TableRow) continue;
      top = std::min(top, r->y);
      bottom = std::max(bottom, r->y + r->height);
    }
    if (top <= bottom) {
      g->x = originX + hs;
      g->y = top;
      g->width = std::max(0.0, gridWidth - 2 * hs);
      g->height = bottom - top;
      for (auto& r : g->children) {
        if (r->style->display != Display::TableRow) continue;
        r->x -= g->x;
        r->y -= g->y;
      }
    }
    g->margin = g->border = g->padding = Edges();
  }
  double gridBottom = gridTop + rowsTotal;
  double captionPos = gridBottom;
  for (Box* c : bottom) {
    c->x = originX + c->margin.left;
    c->y = originY + captionPos + c->margin.top;
    captionPos += c->margin.top + c->height + c->margin.bottom;
  }
  contentHeight = captionTop + rowsTotal + captionBottom;
  table.baseline = -1;
  table.firstBaseline = nrows > 0 && model.rows[0].baseline > 0 ? gridTop + model.rows[0].y + model.rows[0].baseline + originY : -1;
  table.baseline = table.firstBaseline;
  (void)sumWidth;
  (void)collapse;
}

}  // namespace solar::layout
