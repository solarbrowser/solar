/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <vector>

#include "solar/css/Cssom.h"
#include "solar/dom/Node.h"

namespace solar::css {

// The presentational hints of an element (https://html.spec.whatwg.org/multipage/rendering.html#presentational-hints): what its attributes (width, bgcolor, align, cellpadding, size of a font...) say
// as declarations. They are declarations of no weight: the style of the author wins over them.
std::vector<DeclarationEntry> PresentationalHints(const dom::Element* element);

}  // namespace solar::css
