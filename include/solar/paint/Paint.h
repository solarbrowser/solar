/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <string>

#include "solar/layout/Box.h"
#include "solar/paint/DisplayList.h"

namespace Quanta {
class Context;
}

// Painting (https://www.w3.org/TR/CSS22/zindex.html): the boxes of a document in the order they are painted, as a display list.
namespace solar::paint {

// A CSS color, as a computed value gives it (rgb(), rgba(), color(), oklab()...); transparent if it is not one.
Color ParseColor(const std::string& text);

// The display list of the document in `tree`. The scroll position of the viewport and of the scroll containers is the one in the tree.
// `overscan` is how far above and below the viewport items are made for, so that the list can be drawn at other scrolls than the one it was made at.
DisplayList BuildDisplayList(Quanta::Context& ctx, layout::Tree& tree, double overscan = 0);

// The drop-down list of a select that is open: drawn over everything, under the select (above it where there is no room below). `hover` is the row the pointer
// or the keys are on, `top` the first row shown (a list shows up to 20).
struct Popup {
  dom::Element* select = nullptr;
  size_t hover = static_cast<size_t>(-1), top = 0;
};
void SetPopup(const Popup& popup);
// Where the list of `select` is, in the coordinates of the document, how tall each row is and how many rows it shows; false for a select with no box.
struct PopupGeometry {
  double x = 0, y = 0, width = 0, height = 0, rowHeight = 0;
  size_t rows = 0;
};
bool PopupGeometryOf(Quanta::Context& ctx, layout::Tree& tree, dom::Element* select, size_t top, PopupGeometry& out);

// The place in the text of the document a point of the window is at (its coordinates in the document, scroll included): the text node and the UTF-16 units into
// its data, nearest to the point on the nearest line; false when the page has no text. `block` is the element of the block the text is in.
bool TextPointAt(layout::Tree& tree, double x, double y, dom::Node*& node, uint32_t& offset, dom::Element** block = nullptr);

// Whether the caret of the field that has the focus is drawn in the frames made from now on (it blinks).
void SetCaretVisible(bool visible);

}  // namespace solar::paint
