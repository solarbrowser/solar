/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <algorithm>

// position: sticky (https://www.w3.org/TR/css-position-3/#sticky-pos) of a box that is held by the viewport: where it is depends on how far the
// viewport is scrolled, so a list that is drawn at another scroll than the one it was made for moves it itself, from this. Layout works the same
// out for the boxes it has (ApplySticky), from the same numbers.
namespace solar::paint {

struct StickyInfo {
  bool valid = false;
  // The viewport (the room the page has) and the box where it is without any shift, in the coordinates of the document: the border box, the margins.
  double portWidth = 0, portHeight = 0;
  double x = 0, y = 0, width = 0, height = 0;
  double marginTop = 0, marginRight = 0, marginBottom = 0, marginLeft = 0;
  // The content box of the block that holds it.
  double containerX = 0, containerY = 0, containerWidth = 0, containerHeight = 0;
  // The offsets it keeps from the edges of the viewport.
  bool hasTop = false, hasRight = false, hasBottom = false, hasLeft = false;
  double top = 0, right = 0, bottom = 0, left = 0;
  bool rtl = false;
};

// How far the box is moved from where it is without a shift when the viewport is scrolled by `scrollX`, `scrollY`.
inline void ComputeSticky(const StickyInfo& s, double scrollX, double scrollY, double& dx, double& dy) {
  dx = dy = 0;
  const double marginLeft = s.x - s.marginLeft, marginTop = s.y - s.marginTop;
  const double marginRight = s.x + s.width + s.marginRight, marginBottom = s.y + s.height + s.marginBottom;
  const double viewLeft = scrollX, viewTop = scrollY, viewRight = viewLeft + s.portWidth, viewBottom = viewTop + s.portHeight;
  // The sticky view rectangle holds the border box; the containing block holds the margin box.
  double bt = s.y, bb = s.y + s.height, bl = s.x, br = s.x + s.width;
  if (s.hasTop && bt < viewTop + s.top) {
    const double room = std::max(0.0, s.containerY + s.containerHeight - (marginBottom + dy));
    const double shift = std::min(viewTop + s.top - bt, room);
    dy += shift;
    bt += shift;
    bb += shift;
  }
  if (s.hasBottom && bb > viewBottom - s.bottom) {
    const double room = std::max(0.0, (marginTop + dy) - s.containerY);
    dy -= std::min(bb - (viewBottom - s.bottom), room);
  }
  const auto horizontal = [&](bool useLeft) {
    if (useLeft) {
      if (s.hasLeft && bl < viewLeft + s.left) {
        const double room = std::max(0.0, s.containerX + s.containerWidth - (marginRight + dx));
        const double shift = std::min(viewLeft + s.left - bl, room);
        dx += shift;
        bl += shift;
        br += shift;
      }
    } else if (s.hasRight && br > viewRight - s.right) {
      const double room = std::max(0.0, (marginLeft + dx) - s.containerX);
      dx -= std::min(br - (viewRight - s.right), room);
    }
  };
  if (s.hasLeft && s.hasRight) {
    // Both: the one at the end of the line of text goes first and the other has what is left.
    horizontal(!s.rtl);
    horizontal(s.rtl);
  } else {
    horizontal(s.hasLeft);
  }
}

}  // namespace solar::paint
