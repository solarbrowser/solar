// The scrollable overflow of a box (https://www.w3.org/TR/css-overflow-3/#scrollable): the area that can be scrolled to, from the padding box and what
// sticks out of it, but not on the side where scrolling starts.
#include <algorithm>
#include <cmath>

#include "solar/layout/Layout.h"

namespace solar::layout {

namespace {

bool Clips(const BoxStyle& s) {
  const BoxStyle& p = s.Physical();
  return p.overflowX != Overflow::Visible || p.overflowY != Overflow::Visible;
}

bool EstablishesContainingBlock(const Box& b) {
  if (!b.style) return false;
  if (b.style->position != Position::Static) return true;
  if (b.style->containPaint || b.style->containLayout) return true;
  Matrix m;
  return b.kind == Box::Kind::Block && TransformOf(b, m);
}

// Whether an absolutely positioned box has the scroller, or a box in it, for its containing block.
bool ContainingBlockInside(const Box& scroller, const Box& box) {
  for (const Box* p = box.parent; p; p = p->parent) {
    if (p == &scroller) return scroller.parent == nullptr || EstablishesContainingBlock(scroller);
    if (p->kind == Box::Kind::Block && EstablishesContainingBlock(*p)) return true;
  }
  return false;
}

struct Collector {
  Tree& tree;
  const Box& scroller;
  Rect origin;  // the scroller's padding box in document coordinates
  bool any = false;
  double left = 0, top = 0, right = 0, bottom = 0;  // the union of what is contributed, in the padding box's coordinates
  // The in-flow bounds: the margin boxes of what is in the flow, where the flow put them (not where position: relative or a transform did).
  bool anyInflow = false;
  double inLeft = 0, inTop = 0, inRight = 0, inBottom = 0;

  void AddInflow(Rect r) {
    r.x -= origin.x;
    r.y -= origin.y;
    if (!anyInflow) {
      inLeft = r.x;
      inTop = r.y;
      inRight = r.Right();
      inBottom = r.Bottom();
      anyInflow = true;
    } else {
      inLeft = std::min(inLeft, r.x);
      inTop = std::min(inTop, r.y);
      inRight = std::max(inRight, r.Right());
      inBottom = std::max(inBottom, r.Bottom());
    }
  }

  void Add(Rect r, const Box* chainBox) {
    Matrix m;
    if (chainBox && TransformBelow(*chainBox, &scroller, m)) {
      double xs[4], ys[4];
      m.Map(r.x, r.y, xs[0], ys[0]);
      m.Map(r.x + r.width, r.y, xs[1], ys[1]);
      m.Map(r.x, r.y + r.height, xs[2], ys[2]);
      m.Map(r.x + r.width, r.y + r.height, xs[3], ys[3]);
      double l = xs[0], rr = xs[0], t = ys[0], b = ys[0];
      for (int i = 1; i < 4; ++i) {
        l = std::min(l, xs[i]);
        rr = std::max(rr, xs[i]);
        t = std::min(t, ys[i]);
        b = std::max(b, ys[i]);
      }
      r = {l, t, rr - l, b - t};
    }
    r.x -= origin.x;
    r.y -= origin.y;
    if (!any) {
      left = r.x;
      top = r.y;
      right = r.Right();
      bottom = r.Bottom();
      any = true;
    } else {
      left = std::min(left, r.x);
      top = std::min(top, r.y);
      right = std::max(right, r.Right());
      bottom = std::max(bottom, r.Bottom());
    }
  }

  // The lines of a block container hold text, which is what its inline content comes to.
  void Lines(const Box& b, bool inflowLevel) {
    if (b.lines.empty()) return;
    const Rect abs = AbsoluteBorderBox(b);
    for (const Line& line : b.lines) {
      for (const LineItem& item : line.items) {
        if (item.kind != LineItem::Kind::Text) continue;
        Add({abs.x + item.rect.x, abs.y + item.rect.y, item.rect.width, item.rect.height}, &b);
        if (inflowLevel) AddInflow({abs.x + item.rect.x, abs.y + item.rect.y, item.rect.width, item.rect.height});
      }
    }
    const auto fragments = [&](auto&& self, const Box& parent) -> void {
      for (const auto& c : parent.children) {
        if (c->kind != Box::Kind::Inline) continue;
        for (const Rect& f : c->fragments) Add({abs.x + f.x, abs.y + f.y, f.width, f.height}, &b);
        self(self, *c);
      }
    };
    fragments(fragments, b);
  }

  void Walk(const Box& b, bool inflowLevel = true) {
    Lines(b, inflowLevel);
    // The last box in the flow gives the margin of the box and of the empty ones after it that collapse with it.
    struct Pending {
      bool has = false;
      Rect flow;
      double leftMargin = 0, rightMargin = 0, top = 0, positive = 0, negative = 0;
    } pending;
    double trailingPositive = 0, trailingNegative = 0;
    const auto flush = [&](bool last) {
      if (!pending.has) return;
      double positive = pending.positive, negative = pending.negative;
      if (last) {
        positive = std::max(positive, trailingPositive);
        negative = std::min(negative, trailingNegative);
      }
      AddInflow({pending.flow.x - pending.leftMargin, pending.flow.y - pending.top, pending.flow.width + pending.leftMargin + pending.rightMargin,
                 pending.flow.height + pending.top + positive + negative});
      pending.has = false;
    };
    for (const auto& childPtr : b.children) {
      const Box& c = *childPtr;
      if (c.kind != Box::Kind::Block) continue;
      if (c.style->position == Position::Fixed) continue;
      if (c.style->position == Position::Absolute && !ContainingBlockInside(scroller, c)) continue;
      Rect r = AbsoluteBorderBox(c);
      // The margins on the sides where the container ends count for the items of a flex or grid container.
      const Display d = b.style->display;
      if (d == Display::Flex || d == Display::InlineFlex || d == Display::Grid || d == Display::InlineGrid) {
        const BoxStyle& bp = b.style->Physical();
        const bool vertical = IsVertical(bp.writingMode);
        const bool rtl = bp.direction == Direction::Rtl;
        const bool endRight = vertical ? bp.writingMode != WritingMode::VerticalRl : !rtl;
        const bool endLeft = vertical ? bp.writingMode == WritingMode::VerticalRl : rtl;
        const bool endBottom = vertical ? !rtl : true;
        const bool endTop = vertical ? rtl : false;
        if (endRight && c.margin.right > 0) r.width += c.margin.right;
        if (endLeft && c.margin.left > 0) { r.x -= c.margin.left; r.width += c.margin.left; }
        if (endBottom && c.margin.bottom > 0) r.height += c.margin.bottom;
        if (endTop && c.margin.top > 0) { r.y -= c.margin.top; r.height += c.margin.top; }
      }
      Add(r, c.parent);
      if (inflowLevel && c.style->position != Position::Absolute && c.style->position != Position::Fixed && !c.style->IsFloating()) {
        if (c.collapsedThrough) {
          // An empty box: its margins join those of the box before it.
          trailingPositive = std::max(trailingPositive, c.bottomPositive);
          trailingNegative = std::min(trailingNegative, c.bottomNegative);
        } else {
          flush(false);
          trailingPositive = trailingNegative = 0;
          pending.has = true;
          pending.flow = AbsoluteBorderBox(c);
          pending.flow.x -= c.shiftX + c.stickyX;
          pending.flow.y -= c.shiftY + c.stickyY;
          pending.leftMargin = c.margin.left;
          pending.rightMargin = c.margin.right;
          const bool collapsed = c.topPositive != 0 || c.topNegative != 0 || c.bottomPositive != 0 || c.bottomNegative != 0;
          pending.top = collapsed ? c.topPositive + c.topNegative : c.margin.top;
          pending.positive = collapsed ? c.bottomPositive : std::max(0.0, c.margin.bottom);
          pending.negative = collapsed ? c.bottomNegative : std::min(0.0, c.margin.bottom);
        }
      }
      // (Only what is directly in the container counts for the in-flow bounds, and what is directly in a box of no name that stands in for blocks.)
      if (!Clips(*c.style)) Walk(c, inflowLevel && c.anonymous);
    }
    flush(true);
  }
};

}  // namespace

Rect ScrollableOverflow(Tree& tree, const Box& scroller, dom::Element* rootElement) {
  const bool viewport = scroller.parent == nullptr;
  Rect padding;
  const Rect abs = AbsoluteBorderBox(scroller);
  if (viewport) {
    padding = {0, 0, tree.viewportWidth, tree.viewportHeight};
  } else {
    padding = {abs.x + scroller.border.left, abs.y + scroller.border.top, scroller.width - scroller.border.Horizontal(), scroller.height - scroller.border.Vertical()};
  }
  // The direction the scrolling starts from is that of the box (of the root element, for the viewport).
  const BoxStyle* styleBox = &scroller.style->Physical();
  Edges padEdges = viewport ? Edges() : scroller.padding;
  if (viewport && rootElement) {
    for (const Box* b : BoxesOf(tree, rootElement)) {
      if (b->kind == Box::Kind::Block) {
        styleBox = &b->style->Physical();
        // The writing mode and direction of the body are the viewport's, as the browsers have it.
        for (const auto& child : b->children) {
          if (child->node && child->node->IsElement() && static_cast<const dom::Element*>(child->node)->IsHtml("body") && child->kind == Box::Kind::Block) {
            styleBox = &child->style->Physical();
            break;
          }
        }
        break;
      }
    }
  }
  const bool vertical = IsVertical(styleBox->writingMode);
  const bool rtl = styleBox->direction == Direction::Rtl;
  const bool startRight = vertical ? (styleBox->writingMode == WritingMode::VerticalRl || styleBox->writingMode == WritingMode::SidewaysRl) : rtl;
  const bool startBottom = vertical ? (styleBox->writingMode == WritingMode::SidewaysLr ? !rtl : rtl) : false;

  Collector collector{tree, scroller, padding};
  collector.Walk(scroller);
  // The padding box, then what sticks out of it where scrolling can go; the padding at the end is added after the in-flow contents.
  double left = 0, right = padding.width, top = 0, bottom = padding.height;
  if (collector.any) {
    if (startRight) left = std::min(0.0, collector.left);
    else right = std::max(right, collector.right);
    if (startBottom) top = std::min(0.0, collector.top);
    else bottom = std::max(bottom, collector.bottom);
  }
  if (collector.anyInflow) {
    if (startRight) left = std::min(left, collector.inLeft - padEdges.left);
    else right = std::max(right, collector.inRight + padEdges.right);
    if (startBottom) top = std::min(top, collector.inTop - padEdges.top);
    else bottom = std::max(bottom, collector.inBottom + padEdges.bottom);
  }
  return {left, top, right - left, bottom - top};
}

}  // namespace solar::layout
