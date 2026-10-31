/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkColor.h"
#include "include/core/SkFont.h"
#include "include/core/SkImage.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkShader.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTypeface.h"
#include "solar/font/Face.h"
#include "solar/paint/Canvas.h"

namespace solar::paint {

// A canvas's surface and its 2D drawing state (https://html.spec.whatwg.org/multipage/canvas.html#the-canvas-state): what CanvasRenderingContext2D does is done here, the
// script's side being the interface. The drawing is on the page's thread; the window reads the last picture that was published, which is an image that does not change.
// A path being written: the points are mapped by `m` as they come (the canvas's current path has the matrix of the moment of each call; a Path2D has none).
struct PathWriter {
  SkPathBuilder path;
  SkMatrix m = SkMatrix::I();
  void Close() { path.close(); }
  void MoveTo(double x, double y);
  void LineTo(double x, double y);
  void BezierCurveTo(double x1, double y1, double x2, double y2, double x, double y);
  void QuadraticCurveTo(double x1, double y1, double x, double y);
  void Ellipse(double x, double y, double rx, double ry, double rotation, double start, double end, bool ccw);
  void ArcTo(double x1, double y1, double x2, double y2, double r);
  void Rect(double x, double y, double w, double h);
  void RoundRect(double x, double y, double w, double h, const double radii[8]);
  // The commands of a Path2D (the numbers CanvasNatives.cpp describes).
  void AddCommands(const std::vector<double>& commands);
};
SkPath PathFromCommands(const std::vector<double>& commands);

class CanvasSurface {
 public:
  CanvasSurface(int width, int height);

  int width() const { return width_; }
  int height() const { return height_; }
  // A new bitmap of the size, empty (transparent black), and the state as it begins.
  void Reset(int width, int height);

  // ---- State ----
  void Save();
  void Restore();
  void Transform(double a, double b, double c, double d, double e, double f);
  void SetTransform(double a, double b, double c, double d, double e, double f);
  void SetGlobalAlpha(double alpha);
  bool SetBlend(const std::string& operation);
  void SetLineWidth(double width) { state_.lineWidth = static_cast<float>(width); }
  void SetLineCap(int cap) { state_.cap = cap; }
  void SetLineJoin(int join) { state_.join = join; }
  void SetMiterLimit(double limit) { state_.miter = static_cast<float>(limit); }
  void SetLineDash(std::vector<float> dash) { state_.dash = std::move(dash); }
  void SetLineDashOffset(double offset) { state_.dashOffset = static_cast<float>(offset); }
  void SetShadow(double blur, double dx, double dy, SkColor4f color);
  void SetShadowBlur(double blur) { state_.shadowBlur = static_cast<float>(blur); }
  void SetShadowOffset(double dx, double dy) { state_.shadowX = static_cast<float>(dx); state_.shadowY = static_cast<float>(dy); }
  void SetShadowOffsetX(double dx) { state_.shadowX = static_cast<float>(dx); }
  void SetShadowOffsetY(double dy) { state_.shadowY = static_cast<float>(dy); }
  void SetShadowColor(SkColor4f color) { state_.shadowColor = color; }
  void SetSmoothing(bool enabled, int quality) { state_.smooth = enabled; state_.quality = quality; }
  void SetFillColor(SkColor4f color) { state_.fillColor = color; state_.fillShader = nullptr; }
  void SetStrokeColor(SkColor4f color) { state_.strokeColor = color; state_.strokeShader = nullptr; }
  void SetFillShader(sk_sp<SkShader> shader) { state_.fillShader = std::move(shader); }
  void SetStrokeShader(sk_sp<SkShader> shader) { state_.strokeShader = std::move(shader); }
  void SetTextAlign(int align) { state_.textAlign = align; }
  void SetTextBaseline(int baseline) { state_.textBaseline = baseline; }
  void SetDirection(int direction) { state_.direction = direction; }
  bool SetFont(const std::string& font);
  void SetFilter(sk_sp<SkImageFilter> filter) { state_.filter = std::move(filter); }
  SkMatrix Matrix() const { return canvas_->getTotalMatrix(); }

  // ---- The current path (in the surface's coordinates: a point is mapped by the matrix of the moment it was added) ----
  void BeginPath() { path_ = PathWriter(); }
  void ClosePath() { path_.Close(); }
  void MoveTo(double x, double y);
  void LineTo(double x, double y);
  void BezierCurveTo(double x1, double y1, double x2, double y2, double x, double y);
  void QuadraticCurveTo(double x1, double y1, double x, double y);
  void Arc(double x, double y, double r, double start, double end, bool ccw) { Ellipse(x, y, r, r, 0, start, end, ccw); }
  void Ellipse(double x, double y, double rx, double ry, double rotation, double start, double end, bool ccw);
  void ArcTo(double x1, double y1, double x2, double y2, double r);
  void Rect(double x, double y, double w, double h);
  void RoundRect(double x, double y, double w, double h, const double radii[8]);
  // Adds the commands of a Path2D (numbers: see CanvasNatives.cpp), in user space, mapped by the matrix of now.
  void AddPathCommands(const std::vector<double>& commands, const SkMatrix& matrix);
  bool PathEmpty() const { return path_.path.isEmpty(); }

  // ---- Drawing ----
  // `path` is a Path2D's (in user space), or null for the current path.
  void Fill(const SkPath* path, bool evenOdd);
  void Stroke(const SkPath* path);
  void Clip(const SkPath* path, bool evenOdd);
  bool InPath(const SkPath* path, double x, double y, bool evenOdd);
  bool InStroke(const SkPath* path, double x, double y);
  void FillRect(double x, double y, double w, double h);
  void StrokeRect(double x, double y, double w, double h);
  void ClearRect(double x, double y, double w, double h);
  void DrawText(const std::string& text, double x, double y, std::optional<double> maxWidth, bool stroke);
  // Measures: JSON text of TextMetrics.
  std::string Measure(const std::string& text);
  void DrawImage(const sk_sp<SkImage>& image, SkRect source, SkRect destination);
  // Pixels: RGBA, not premultiplied, `w` by `h` from (x, y) of the bitmap (what is outside is transparent black).
  std::vector<uint8_t> GetImageData(int x, int y, int w, int h);
  void PutImageData(const uint8_t* rgba, int w, int h, int dx, int dy, int sx, int sy, int sw, int sh);

  // The surface becomes the picture (replacing what it had), turned over if `flip` (a WebGL drawing buffer has its first row at the bottom).
  void SetContents(const sk_sp<SkImage>& image, bool flip);

  // ---- The picture ----
  // The page's thread: the surface as it is now becomes what the window shows.
  void Publish();
  // Any thread: what was last published (null before the first).
  sk_sp<SkImage> Latest() const;
  // The page's thread: the surface as it is now (for drawImage from a canvas, and for making a file of it).
  sk_sp<SkImage> Snapshot() { return surface_ ? surface_->makeImageSnapshot() : nullptr; }
  // Set when something was drawn since the last Publish.
  bool dirty = false;

 private:
  struct State {
    SkColor4f fillColor{0, 0, 0, 1}, strokeColor{0, 0, 0, 1};
    sk_sp<SkShader> fillShader, strokeShader;
    float alpha = 1;
    SkBlendMode blend = SkBlendMode::kSrcOver;
    float lineWidth = 1, miter = 10, dashOffset = 0, shadowBlur = 0, shadowX = 0, shadowY = 0;
    int cap = 0, join = 0;  // butt, round, square / miter, round, bevel
    std::vector<float> dash;
    SkColor4f shadowColor{0, 0, 0, 0};
    bool smooth = true;
    int quality = 0;  // low, medium, high
    int textAlign = 0, textBaseline = 0, direction = 0;  // start, end, left, right, center / alphabetic, top, hanging, middle, ideographic, bottom
    std::string fontText = "10px sans-serif";
    float fontSize = 10;
    sk_sp<SkTypeface> typeface;
    sk_sp<SkImageFilter> filter;
  };

  SkPaint MakePaint(bool stroke, bool forShadowOnly = false) const;
  SkSamplingOptions Sampling() const;
  sk_sp<SkImageFilter> ShadowFilter(sk_sp<SkImageFilter> input) const;
  bool HasShadow() const { return state_.shadowColor.fA > 0 && (state_.shadowBlur > 0 || state_.shadowX != 0 || state_.shadowY != 0); }
  SkPath UserPath(const SkPath* path, SkPathFillType fill);
  void Mark() { dirty = true; }
  SkFont MakeFont() const;
  void MapPoint(double x, double y, SkPoint* out) const;

  int width_ = 0, height_ = 0;
  sk_sp<SkSurface> surface_;
  SkCanvas* canvas_ = nullptr;
  State state_;
  std::vector<State> stack_;
  PathWriter path_;
  mutable std::mutex latestMutex_;
  sk_sp<SkImage> latest_;
};

// Decodes the file of an image: null if it is not one.
sk_sp<SkImage> DecodeImage(const void* data, size_t size);
// The typeface of a font of the database, made once.
sk_sp<SkTypeface> TypefaceFor(const std::shared_ptr<font::Face>& face);

}  // namespace solar::paint
