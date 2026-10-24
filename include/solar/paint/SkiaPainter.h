#pragma once

#include <memory>

#include "solar/paint/DisplayList.h"

class SkCanvas;

namespace solar::paint {

// Runs a display list on a Skia canvas. It keeps what it makes of the fonts (a typeface for each face), so one is made for a page and kept.
class SkiaPainter {
 public:
  SkiaPainter();
  ~SkiaPainter();
  SkiaPainter(const SkiaPainter&) = delete;
  SkiaPainter& operator=(const SkiaPainter&) = delete;

  // Draws the list as the viewport is scrolled by `scrollX`, `scrollY` (the list's own scroll for the first form).
  void Draw(const DisplayList& list, SkCanvas& canvas);
  void Draw(const DisplayList& list, SkCanvas& canvas, double scrollX, double scrollY);
  // What was kept of a page that is left (decoded pictures, typefaces) is let go.
  void Forget();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace solar::paint
