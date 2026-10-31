/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <memory>

#include "quanta/Embed.h"

// Canvas 2D (https://html.spec.whatwg.org/multipage/canvas.html): the drawing is Skia's, on a surface of the canvas's own; what the page draws is shown by the display list
// asking the surface for its latest picture. This is what the other parts of the program see of it (the drawing itself is in src/paint/Canvas.cpp, and only the window program
// has Skia).
namespace solar::paint {

class CanvasSurface;

// The surface of a canvas element (the document's `canvas`), for layout and painting to look at; set by the script's natives.
void AttachCanvas(const void* element, std::shared_ptr<CanvasSurface> surface);
void DetachCanvas(const void* element);
std::shared_ptr<CanvasSurface> CanvasOf(const void* element);

}  // namespace solar::paint

namespace solar::webapi {

// Defines in a realm the natives that CanvasRenderingContext2D (src/web-api/js/canvas.js) draws with: __solarCanvasCreate, __solarCv, __solarCvStr and the rest (Canvas
// natives in src/paint/CanvasNatives.cpp). Only the window program has them.
void InstallCanvasNatives(Quanta::Embed::Realm& realm);

}  // namespace solar::webapi
