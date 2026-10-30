/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <memory>

#include "solar/gui/GpuSurface.h"

namespace solar::gui {

// One for each backend (GpuSurface.cpp, VulkanSurface.cpp).
std::unique_ptr<GpuSurface> MakeOpenGlSurface(SDL_Window* window);
std::unique_ptr<GpuSurface> MakeVulkanSurface(SDL_Window* window);

}  // namespace solar::gui
