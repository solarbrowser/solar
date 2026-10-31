/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "CanvasSurface.h"
#include "solar/dom/Damage.h"
#include "solar/dom/Node.h"
#include "solar/web/FetchHost.h"

namespace solar::paint {

// What a realm keeps for its canvases: the surfaces and the decoded images, each by a number the script holds (CanvasNatives.cpp and WebGl.cpp both use it).
struct Natives {
  Natives(Quanta::Context& c, web::FetchHost& h) : ctx(&c), host(&h) {}
  Quanta::Context* ctx;
  web::FetchHost* host;
  std::map<uint32_t, std::shared_ptr<CanvasSurface>> surfaces;
  std::map<uint32_t, sk_sp<SkImage>> images;
  std::map<uint32_t, std::vector<dom::Element*>> elements;
  std::set<uint32_t> queued;
  // Something to do before a surface's picture is made (a WebGL context copies its drawing buffer into the surface), and what to let go of when the realm goes.
  std::map<uint32_t, std::function<void()>> beforePublish;
  std::vector<std::function<void()>> onRelease;
  uint32_t next = 1;
  std::shared_ptr<bool> alive = std::make_shared<bool>(true);

  // What was drawn is shown a moment later: one task for all of it.
  void Touch(uint32_t id) {
    const auto found = surfaces.find(id);
    if (found == surfaces.end() || !found->second->dirty || queued.count(id)) return;
    queued.insert(id);
    host->config().loop->Post([this, id, alive = alive] {
      if (!*alive) return;
      queued.erase(id);
      const auto surface = surfaces.find(id);
      if (surface == surfaces.end()) return;
      if (const auto before = beforePublish.find(id); before != beforePublish.end()) before->second();
      surface->second->Publish();
      const auto attached = elements.find(id);
      if (attached == elements.end()) return;
      for (dom::Element* element : attached->second) {
        dom::NoteDamage(element, dom::kDamageAttributes);
        dom::NoteTreeChangeAt(element);
      }
    });
  }
  void Release() {
    *alive = false;
    for (auto& release : onRelease) release();
    onRelease.clear();
    beforePublish.clear();
    for (auto& [id, list] : elements) for (dom::Element* element : list) DetachCanvas(element);
    elements.clear();
    surfaces.clear();
    images.clear();
  }
};


Natives* NativesOf(Quanta::Context& ctx);

}  // namespace solar::paint
