/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <string>

#include "solar/dom/Node.h"

// Container queries (css-conditional-5): whether an @container rule applies to an element.
namespace solar::css {

// `prelude` is the rule's condition list. The container is looked for among the ancestors of the element (the element itself, for a pseudo-element).
bool ContainerRuleMatches(Quanta::Context& ctx, dom::Element* element, bool pseudoElement, const std::string& prelude);

// The size of the nearest ancestor that is a size container (the content box, in px) for the container query units; false if there is none.
bool NearestContainerSize(Quanta::Context& ctx, dom::Element* element, bool pseudoElement, double& width, double& height, bool& vertical);

}  // namespace solar::css
