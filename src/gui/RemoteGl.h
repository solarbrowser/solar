/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <memory>

#include "solar/paint/GlRuntime.h"

namespace solar::gui {

// WebGL's contexts, in the GPU's process: the page's process has no GPU, and this is its way to one. The calls of the script go on in batches (the ones that give something back are
// waited for, and flush what came before them), and the picture of what was drawn comes back in memory this process made. `fd` is the connection to the GPU's process, taken over.
std::unique_ptr<paint::gl::Backend> MakeRemoteGlBackend(int fd);

}  // namespace solar::gui
