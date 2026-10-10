// position: sticky (https://www.w3.org/TR/css-position-3/#sticky-pos): a box that stays in view while its scroll container is scrolled, as far as
// its containing block lets it. Where it is depends on how far the container is scrolled, so this is done again when the scroll position changes.
#include <algorithm>
#include <cmath>

#include "solar/layout/Layout.h"

namespace solar::layout {

namespace {

bool ScrollsOverflow(const BoxStyle& s) {
  const BoxStyle& p = s.Physical();
  const auto scrolls = [](Overflow o) { return o != Overflow::Visible && o != Overflow::Clip; };
  return scrolls(p.overflowX) || scrolls(p.overflowY);
}

const Box* BlockParent(const Box& box) {
  const Box* p = box.parent;
  while (p && p->kind != Box::Kind::Block) p = p->parent;
  return p;
}

void Apply(Tree& tree, Box& box) {
  const BoxStyle& style = *box.style;
  if (box.kind == Box::Kind::Block && style.position == Position::Sticky && box.parent && !box.inlineLevel) {
    const BoxStyle& physical = style.Physical();
    // The scroll container: the nearest box that scrolls its overflow, else the viewport.
    const Box* scroller = nullptr;
    for (const Box* p = BlockParent(box); p && p->parent; p = BlockParent(*p)) {
      if (p->style && ScrollsOverflow(*p->style)) {
        scroller = p;
        break;
      }
    }
    double portX, portY, portWidth, portHeight, scrollX = 0, scrollY = 0;
    if (scroller) {
      const Rect abs = AbsoluteBorderBox(*scroller);
      portX = abs.x + scroller->border.left;
      portY = abs.y + scroller->border.top;
      portWidth = scroller->width - scroller->border.Horizontal();
      portHeight = scroller->height - scroller->border.Vertical();
      const auto found = scroller->node ? tree.scroll.find(scroller->node) : tree.scroll.end();
      if (found != tree.scroll.end()) {
        scrollX = found->second.first;
        scrollY = found->second.second;
      }
    } else {
      portX = portY = 0;
      portWidth = tree.viewportWidth;
      portHeight = tree.viewportHeight;
      const auto found = tree.root->node ? tree.scroll.find(tree.root->node) : tree.scroll.end();
      if (found != tree.scroll.end()) {
        scrollX = found->second.first;
        scrollY = found->second.second;
      }
    }
    // Where the box is without the shift it had.
    Rect abs = AbsoluteBorderBox(box);
    abs.x -= box.stickyX;
    abs.y -= box.stickyY;
    const double marginLeft = abs.x - box.margin.left, marginTop = abs.y - box.margin.top;
    const double marginRight = abs.x + abs.width + box.margin.right, marginBottom = abs.y + abs.height + box.margin.bottom;
    // The containing block's content box.
    const Box* container = BlockParent(box);
    Rect content = container ? AbsoluteBorderBox(*container) : Rect{0, 0, tree.viewportWidth, tree.viewportHeight};
    if (container) {
      content.x += container->border.left + container->padding.left;
      content.y += container->border.top + container->padding.top;
      content.width = container->ContentWidth();
      content.height = container->ContentHeight();
    }
    // The part of the scroll container that is seen, and the sticky view rectangle inside it.
    const double viewLeft = portX + scrollX, viewTop = portY + scrollY, viewRight = viewLeft + portWidth, viewBottom = viewTop + portHeight;
    const auto inset = [&](int side, double basis, double& out) {
      const Length& l = physical.inset[side];
      if (l.IsAuto()) return false;
      out = l.Resolve(basis);
      return true;
    };
    double top, right, bottom, left;
    const bool hasTop = inset(0, portHeight, top), hasRight = inset(1, portWidth, right), hasBottom = inset(2, portHeight, bottom), hasLeft = inset(3, portWidth, left);
    double dx = 0, dy = 0;
    // The sticky view rectangle holds the border box; the containing block holds the margin box.
    double bt = abs.y, bb = abs.y + abs.height, bl = abs.x, br = abs.x + abs.width;
    if (hasTop && bt < viewTop + top) {
      const double room = std::max(0.0, content.y + content.height - (marginBottom + dy));
      const double shift = std::min(viewTop + top - bt, room);
      dy += shift;
      bt += shift;
      bb += shift;
    }
    if (hasBottom && bb > viewBottom - bottom) {
      const double room = std::max(0.0, (marginTop + dy) - content.y);
      dy -= std::min(bb - (viewBottom - bottom), room);
    }
    const bool rtl = physical.direction == Direction::Rtl;
    const auto horizontal = [&](bool useLeft) {
      if (useLeft) {
        if (hasLeft && bl < viewLeft + left) {
          const double room = std::max(0.0, content.x + content.width - (marginRight + dx));
          const double shift = std::min(viewLeft + left - bl, room);
          dx += shift;
          bl += shift;
          br += shift;
        }
      } else if (hasRight && br > viewRight - right) {
        const double room = std::max(0.0, (marginLeft + dx) - content.x);
        dx -= std::min(br - (viewRight - right), room);
      }
    };
    if (hasLeft && hasRight) {
      // Both: the one at the end of the line of text goes first and the other has what is left.
      horizontal(!rtl);
      horizontal(rtl);
    } else {
      horizontal(hasLeft);
    }
    box.x += dx - box.stickyX;
    box.y += dy - box.stickyY;
    box.stickyX = dx;
    box.stickyY = dy;
  }
  for (auto& child : box.children) Apply(tree, *child);
}

}  // namespace

void ApplySticky(Tree& tree) {
  if (tree.root) Apply(tree, *tree.root);
}

}  // namespace solar::layout
