/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// position: sticky (https://www.w3.org/TR/css-position-3/#sticky-pos): a box that stays in view while its scroll container is scrolled, as far as
// its containing block lets it. Where it is depends on how far the container is scrolled, so this is done again when the scroll position changes.
#include <algorithm>
#include <cmath>

#include "solar/layout/Layout.h"
#include "solar/paint/Sticky.h"

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

// Where a sticky box is held by the viewport (not by a scroll container of its own), and how far the viewport is scrolled. False for one that is not
// held by the viewport.
bool InfoOf(const Tree& tree, const Box& box, paint::StickyInfo& info, double& scrollX, double& scrollY) {
  const BoxStyle& style = *box.style;
  if (box.kind != Box::Kind::Block || style.position != Position::Sticky || !box.parent || box.inlineLevel) return false;
  const BoxStyle& physical = style.Physical();
  for (const Box* p = BlockParent(box); p && p->parent; p = BlockParent(*p)) {
    if (p->style && ScrollsOverflow(*p->style)) return false;
  }
  scrollX = scrollY = 0;
  const auto found = tree.root->node ? tree.scroll.find(tree.root->node) : tree.scroll.end();
  if (found != tree.scroll.end()) {
    scrollX = found->second.first;
    scrollY = found->second.second;
  }
  info = paint::StickyInfo();
  info.valid = true;
  info.portWidth = tree.viewportWidth;
  info.portHeight = tree.viewportHeight;
  // Where the box is without the shift it had.
  const Rect abs = AbsoluteBorderBox(box);
  info.x = abs.x - box.stickyX;
  info.y = abs.y - box.stickyY;
  info.width = abs.width;
  info.height = abs.height;
  info.marginTop = box.margin.top;
  info.marginRight = box.margin.right;
  info.marginBottom = box.margin.bottom;
  info.marginLeft = box.margin.left;
  // The containing block's content box.
  const Box* container = BlockParent(box);
  Rect content = container ? AbsoluteBorderBox(*container) : Rect{0, 0, tree.viewportWidth, tree.viewportHeight};
  if (container) {
    content.x += container->border.left + container->padding.left;
    content.y += container->border.top + container->padding.top;
    // A scrollbar takes no room from the block a sticky box is held in.
    content.width = container->ContentWidth() + container->scrollbar.Horizontal();
    content.height = container->ContentHeight() + container->scrollbar.Vertical();
  }
  info.containerX = content.x;
  info.containerY = content.y;
  info.containerWidth = content.width;
  info.containerHeight = content.height;
  const auto inset = [&](int side, double basis, double& out) {
    const Length& l = physical.inset[side];
    if (l.IsAuto()) return false;
    out = l.Resolve(basis);
    return true;
  };
  info.hasTop = inset(0, info.portHeight, info.top);
  info.hasRight = inset(1, info.portWidth, info.right);
  info.hasBottom = inset(2, info.portHeight, info.bottom);
  info.hasLeft = inset(3, info.portWidth, info.left);
  info.rtl = physical.direction == Direction::Rtl;
  return true;
}

void Apply(Tree& tree, Box& box) {
  const BoxStyle& style = *box.style;
  if (box.kind == Box::Kind::Block && style.position == Position::Sticky && box.parent && !box.inlineLevel) {
    const BoxStyle& physical = style.Physical();
    paint::StickyInfo info;
    double viewportScrollX = 0, viewportScrollY = 0;
    double dx = 0, dy = 0;
    if (InfoOf(tree, box, info, viewportScrollX, viewportScrollY)) {
      paint::ComputeSticky(info, viewportScrollX, viewportScrollY, dx, dy);
    } else {
      // Held by a scroll container of its own.
      const Box* scroller = nullptr;
      for (const Box* p = BlockParent(box); p && p->parent; p = BlockParent(*p)) {
        if (p->style && ScrollsOverflow(*p->style)) {
          scroller = p;
          break;
        }
      }
      if (scroller) {
        const Rect scrollerBox = AbsoluteBorderBox(*scroller);
        double scrollX = 0, scrollY = 0;
        const auto found = scroller->node ? tree.scroll.find(scroller->node) : tree.scroll.end();
        if (found != tree.scroll.end()) {
          scrollX = found->second.first;
          scrollY = found->second.second;
        }
        paint::StickyInfo own;
        own.valid = true;
        const double portX = scrollerBox.x + scroller->border.left + scroller->scrollbar.left;
        const double portY = scrollerBox.y + scroller->border.top + scroller->scrollbar.top;
        own.portWidth = scroller->width - scroller->border.Horizontal() - scroller->scrollbar.Horizontal();
        own.portHeight = scroller->height - scroller->border.Vertical() - scroller->scrollbar.Vertical();
        Rect abs = AbsoluteBorderBox(box);
        own.x = abs.x - box.stickyX;
        own.y = abs.y - box.stickyY;
        own.width = abs.width;
        own.height = abs.height;
        own.marginTop = box.margin.top;
        own.marginRight = box.margin.right;
        own.marginBottom = box.margin.bottom;
        own.marginLeft = box.margin.left;
        const Box* container = BlockParent(box);
        Rect content = container ? AbsoluteBorderBox(*container) : Rect{0, 0, tree.viewportWidth, tree.viewportHeight};
        if (container) {
          content.x += container->border.left + container->padding.left;
          content.y += container->border.top + container->padding.top;
          content.width = container->ContentWidth() + container->scrollbar.Horizontal();
          content.height = container->ContentHeight() + container->scrollbar.Vertical();
        }
        own.containerX = content.x;
        own.containerY = content.y;
        own.containerWidth = content.width;
        own.containerHeight = content.height;
        const auto inset = [&](int side, double basis, double& out) {
          const Length& l = physical.inset[side];
          if (l.IsAuto()) return false;
          out = l.Resolve(basis);
          return true;
        };
        own.hasTop = inset(0, own.portHeight, own.top);
        own.hasRight = inset(1, own.portWidth, own.right);
        own.hasBottom = inset(2, own.portHeight, own.bottom);
        own.hasLeft = inset(3, own.portWidth, own.left);
        own.rtl = physical.direction == Direction::Rtl;
        // (The port of a scroll container is at its own place in the document, and what is scrolled is in its own coordinates: the numbers are moved to the port.)
        paint::StickyInfo moved = own;
        moved.x -= portX;
        moved.y -= portY;
        moved.containerX -= portX;
        moved.containerY -= portY;
        paint::ComputeSticky(moved, scrollX, scrollY, dx, dy);
      }
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

bool StickyInfoOf(const Tree& tree, const Box& box, paint::StickyInfo& info) {
  double scrollX = 0, scrollY = 0;
  return tree.root && InfoOf(tree, box, info, scrollX, scrollY);
}

}  // namespace solar::layout
