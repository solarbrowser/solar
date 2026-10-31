/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <mutex>
#include <unordered_map>

#include "solar/dom/MediaHook.h"
#include "solar/paint/Canvas.h"

namespace solar::paint {

namespace {
std::mutex g_mutex;
std::unordered_map<const void*, std::shared_ptr<CanvasSurface>>& Surfaces() {
  static std::unordered_map<const void*, std::shared_ptr<CanvasSurface>> surfaces;
  return surfaces;
}
}  // namespace

void AttachCanvas(const void* element, std::shared_ptr<CanvasSurface> surface) {
  std::shared_ptr<CanvasSurface> old;
  std::lock_guard lock(g_mutex);
  const auto found = Surfaces().find(element);
  if (surface) {
    if (found == Surfaces().end()) {
      ++dom::g_canvasAttached;
      dom::g_canvasDetach = &DetachCanvas;
      Surfaces()[element] = std::move(surface);
    } else {
      old = std::move(found->second);
      found->second = std::move(surface);
    }
  } else if (found != Surfaces().end()) {
    old = std::move(found->second);
    Surfaces().erase(found);
    --dom::g_canvasAttached;
  }
}

void DetachCanvas(const void* element) {
  std::shared_ptr<CanvasSurface> old;
  std::lock_guard lock(g_mutex);
  const auto found = Surfaces().find(element);
  if (found == Surfaces().end()) return;
  old = std::move(found->second);
  Surfaces().erase(found);
  --dom::g_canvasAttached;
}

std::shared_ptr<CanvasSurface> CanvasOf(const void* element) {
  std::lock_guard lock(g_mutex);
  const auto found = Surfaces().find(element);
  return found == Surfaces().end() ? nullptr : found->second;
}

}  // namespace solar::paint
