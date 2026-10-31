/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <atomic>

namespace solar::dom {

// A media element has a player in a registry (src/media/Registry.cpp) that is looked up by the element's address, so a node that is freed has to be taken out of it, or the next
// node at that address would be given its player. The registry sets the counter and the function; a node's destructor looks at the counter (cheap) and calls the function when
// there is something registered.
inline std::atomic<int> g_mediaAttached{0};
inline void (*g_mediaDetach)(const void* element) = nullptr;

// The same for the surface of a canvas element (src/paint/CanvasRegistry.cpp).
inline std::atomic<int> g_canvasAttached{0};
inline void (*g_canvasDetach)(const void* element) = nullptr;

// What a form control shows (its value, its placeholder) is in the page's script: layout asks for it to be read into the control before it needs the sizes (src/html/Interaction.cpp sets this).
struct Document;
inline void (*g_refreshControls)(Document* document) = nullptr;

// Whether an element is :valid (0), :invalid (1), :in-range (2) or :out-of-range (3): the constraints are checked by the page's script (src/html/FormsValidity.inc), which style asks through this.
struct Element;
inline bool (*g_validityQuery)(const Element* element, int which) = nullptr;

}  // namespace solar::dom
