/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"

namespace solar::gui {

// How the scrollbar of the window is drawn over the page: it floats at the right edge, thin until the pointer is over it (then wider, on a shade), and fades. Whoever draws the page draws
// it (the compositor, or the GPU's process for it).
struct ScrollbarShape {
  bool visible = false;
  float alpha = 1;
  bool emphasized = false;  // the pointer is over it, or it is dragged
  bool dragging = false;
  float top = 0, height = 0;
  int windowWidth = 0, windowHeight = 0;  // in points
};

inline void DrawScrollbar(SkCanvas& canvas, const ScrollbarShape& bar) {
  if (!bar.visible || bar.alpha <= 0) return;
  const float width = bar.emphasized ? 10.f : 6.f;
  SkPaint paint;
  paint.setAntiAlias(true);
  if (bar.emphasized) {
    paint.setColor(SkColorSetARGB(40, 0, 0, 0));
    canvas.drawRect(SkRect::MakeXYWH(bar.windowWidth - width - 4, 0, width + 4, bar.windowHeight), paint);
  }
  paint.setColor(SkColorSetARGB(static_cast<U8CPU>((bar.dragging ? 170 : 120) * bar.alpha), 0, 0, 0));
  canvas.drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(bar.windowWidth - width - 2, bar.top + 2, width, bar.height - 4), width / 2, width / 2), paint);
}

}  // namespace solar::gui
