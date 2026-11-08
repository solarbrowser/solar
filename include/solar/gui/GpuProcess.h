/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <memory>
#include <string>

namespace solar::gui {

// The GPU's process (`solar --gpu`, with the browser's connection as descriptor 3): it draws the display lists of the pages with the GPU, for each page that is given it by the browser, and
// ends when the browser does. Returns its exit code.
int GpuMain(int controlFd);

// The browser's side: starts the GPU's process and gives the pages their connections to it. Null where there is no GPU to draw with (the pages draw with the CPU then).
class GpuHost {
 public:
  // `why` says why there is none.
  static std::unique_ptr<GpuHost> Start(std::string* why = nullptr);
  ~GpuHost();
  GpuHost(const GpuHost&) = delete;
  GpuHost& operator=(const GpuHost&) = delete;

  // A new page's connection: the page's end of it (the other is the GPU's), or -1.
  // `glFd`, if given, is the page's connection for WebGL (-1 where there is none).
  int NewClient(int* glFd = nullptr);
  // The name of the GPU.
  const std::string& renderer() const;

  struct Impl;

 private:
  GpuHost();
  std::unique_ptr<Impl> impl_;
};

}  // namespace solar::gui
