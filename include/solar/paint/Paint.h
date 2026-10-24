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

}  // namespace solar::paint
