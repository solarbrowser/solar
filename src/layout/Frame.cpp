// Writing modes: layout works in the logical frame of each box (x along the lines, y from block-start to block-end); these turn what
// one frame came to into another's, and into physical coordinates at the end.
#include <algorithm>
#include <cmath>

#include "Internal.h"

namespace solar::layout {

namespace {

const Box* FrameBox(const Box& box) {
  const Box* p = box.parent;
  while (p && p->kind != Box::Kind::Block) p = p->parent;
  return p;
}

}  // namespace

bool Orthogonal(WritingMode a, WritingMode b) { return IsVertical(a) != IsVertical(b); }

void FrameToPhysical(WritingMode mode, double frameWidth, double frameHeight, double x, double y, double w, double h, Rect& out) {
  switch (mode) {
    case WritingMode::HorizontalTb: out = {x, y, w, h}; break;
    case WritingMode::VerticalRl: case WritingMode::SidewaysRl: out = {frameHeight - (y + h), x, h, w}; break;
    case WritingMode::VerticalLr: out = {y, x, h, w}; break;
    case WritingMode::SidewaysLr: out = {y, frameWidth - (x + w), h, w}; break;
  }
}

void PhysicalToFrame(WritingMode mode, double frameWidth, double frameHeight, const Rect& r, double& x, double& y, double& w, double& h) {
  switch (mode) {
    case WritingMode::HorizontalTb: x = r.x; y = r.y; w = r.width; h = r.height; break;
    case WritingMode::VerticalRl: case WritingMode::SidewaysRl: x = r.y; y = frameHeight - (r.x + r.width); w = r.height; h = r.width; break;
    case WritingMode::VerticalLr: x = r.y; y = r.x; w = r.height; h = r.width; break;
    case WritingMode::SidewaysLr: x = frameWidth - (r.y + r.height); y = r.x; w = r.height; h = r.width; break;
  }
  (void)frameWidth;
}

void VectorToPhysical(WritingMode mode, double x, double y, double& px, double& py) {
  switch (mode) {
    case WritingMode::HorizontalTb: px = x; py = y; break;
    case WritingMode::VerticalRl: case WritingMode::SidewaysRl: px = -y; py = x; break;
    case WritingMode::VerticalLr: px = y; py = x; break;
    case WritingMode::SidewaysLr: px = y; py = -x; break;
  }
}

void VectorToFrame(WritingMode mode, double px, double py, double& x, double& y) {
  switch (mode) {
    case WritingMode::HorizontalTb: x = px; y = py; break;
    case WritingMode::VerticalRl: case WritingMode::SidewaysRl: x = py; y = -px; break;
    case WritingMode::VerticalLr: x = py; y = px; break;
    case WritingMode::SidewaysLr: x = -py; y = px; break;
  }
}

// Edges (top, right, bottom, left) of a frame as physical edges, and back.
Edges EdgesToPhysical(WritingMode mode, const Edges& e) {
  const double v[4] = {e.top, e.right, e.bottom, e.left};
  double p[4];
  for (int l = 0; l < 4; ++l) p[PhysicalSideOf(mode, l)] = v[l];
  return {p[0], p[1], p[2], p[3]};
}

Edges EdgesToFrame(WritingMode mode, const Edges& e) {
  const double v[4] = {e.top, e.right, e.bottom, e.left};
  double l[4];
  for (int i = 0; i < 4; ++i) l[i] = v[PhysicalSideOf(mode, i)];
  return {l[0], l[1], l[2], l[3]};
}

void AdaptToParentFrame(Box& box) {
  const Box* frame = FrameBox(box);
  const WritingMode own = box.Mode();
  const WritingMode parent = frame ? frame->Mode() : WritingMode::HorizontalTb;
  box.ownWidth = box.width;
  box.ownHeight = box.height;
  if (own == parent) return;
  const bool turn = Orthogonal(own, parent);
  const auto across = [&](const Edges& e) { return EdgesToFrame(parent, EdgesToPhysical(own, e)); };
  box.margin = across(box.margin);
  box.border = across(box.border);
  box.padding = across(box.padding);
  if (turn) std::swap(box.width, box.height);
  box.x = box.margin.left;
  double px, py, fx, fy;
  VectorToPhysical(own, box.shiftX, box.shiftY, px, py);
  VectorToFrame(parent, px, py, fx, fy);
  box.shiftX = fx;
  box.shiftY = fy;
  box.topPositive = std::max(0.0, box.margin.top);
  box.topNegative = std::min(0.0, box.margin.top);
  box.bottomPositive = std::max(0.0, box.margin.bottom);
  box.bottomNegative = std::min(0.0, box.margin.bottom);
  box.collapsedThrough = false;
  if (turn) box.baseline = box.firstBaseline = -1;
}

namespace {

void ConvertContents(Box& b);

// b's own rectangle, in the frame it was laid out in, as a physical one.
void ConvertSelf(Box& b, WritingMode mode, double fw, double fh) {
  Rect r;
  FrameToPhysical(mode, fw, fh, b.x, b.y, b.width, b.height, r);
  b.x = r.x;
  b.y = r.y;
  b.width = r.width;
  b.height = r.height;
  b.margin = EdgesToPhysical(mode, b.margin);
  b.border = EdgesToPhysical(mode, b.border);
  b.padding = EdgesToPhysical(mode, b.padding);
  double px, py;
  VectorToPhysical(mode, b.shiftX, b.shiftY, px, py);
  b.shiftX = px;
  b.shiftY = py;
  Rect s;
  FrameToPhysical(mode, fw, fh, b.staticX, b.staticY, 0, 0, s);
  b.staticX = s.x;
  b.staticY = s.y;
}

void ConvertFragments(Box& inlineBox, WritingMode mode, double fw, double fh) {
  for (Rect& f : inlineBox.fragments) {
    Rect r;
    FrameToPhysical(mode, fw, fh, f.x, f.y, f.width, f.height, r);
    f = r;
  }
  for (auto& c : inlineBox.children) {
    if (c->kind == Box::Kind::Inline || c->kind == Box::Kind::Text) ConvertFragments(*c, mode, fw, fh);
  }
}

// The boxes under an inline box that are in the frame of the block around it (atomic inlines, floats): converted there.
void ConvertThroughInline(Box& inlineBox, WritingMode mode, double fw, double fh) {
  for (auto& c : inlineBox.children) {
    if (c->kind == Box::Kind::Inline) {
      ConvertThroughInline(*c, mode, fw, fh);
    } else if (c->kind == Box::Kind::Block) {
      if (c->style->IsOutOfFlow()) {
        // Taken out of the flow: only its static position is the container's.
        Rect s;
        FrameToPhysical(mode, fw, fh, c->staticX, c->staticY, 0, 0, s);
        c->staticX = s.x;
        c->staticY = s.y;
        continue;
      }
      double ow = c->ownWidth, oh = c->ownHeight;
      ConvertSelf(*c, mode, fw, fh);
      c->ownWidth = ow;
      c->ownHeight = oh;
      ConvertContents(*c);
    }
  }
}

void ConvertContents(Box& b) {
  const WritingMode mode = b.Mode();
  const double fw = b.ownWidth, fh = b.ownHeight;
  for (Line& line : b.lines) {
    Rect r;
    FrameToPhysical(mode, fw, fh, line.rect.x, line.rect.y, line.rect.width, line.rect.height, r);
    line.rect = r;
    for (LineItem& item : line.items) {
      FrameToPhysical(mode, fw, fh, item.rect.x, item.rect.y, item.rect.width, item.rect.height, r);
      item.rect = r;
      item.text.x = r.x;
    }
  }
  for (auto& c : b.children) {
    if (c->outsideMarker || c->kind == Box::Kind::Block) {
      if (c->style->IsOutOfFlow() && !c->style->IsFloating()) {
        Rect s;
        FrameToPhysical(mode, fw, fh, c->staticX, c->staticY, 0, 0, s);
        c->staticX = s.x;
        c->staticY = s.y;
        continue;
      }
      const double ow = c->ownWidth, oh = c->ownHeight;
      ConvertSelf(*c, mode, fw, fh);
      c->ownWidth = ow;
      c->ownHeight = oh;
      ConvertContents(*c);
    } else if (c->kind == Box::Kind::Inline || c->kind == Box::Kind::Text) {
      ConvertFragments(*c, mode, fw, fh);
      if (c->kind == Box::Kind::Inline) ConvertThroughInline(*c, mode, fw, fh);
    }
  }
}

}  // namespace

// Everything laid out in the logical frames, as physical rectangles; boxes taken out of the flow are left to be placed.
void ConvertTree(Tree& tree) {
  Box& icb = *tree.root;
  const double ow = icb.ownWidth, oh = icb.ownHeight;
  // The initial containing block is its own frame; its size is the viewport's.
  const bool vertical = IsVertical(icb.Mode());
  icb.x = icb.y = 0;
  icb.width = vertical ? oh : ow;
  icb.height = vertical ? ow : oh;
  ConvertContents(icb);
  icb.ownWidth = ow;
  icb.ownHeight = oh;
}

// One box that was placed in its own frame, as physical: `rect` is its border box in the frame (of the given mode and size) it sits in,
// and the edges are its own frame's.
void ConvertPlaced(Box& b, WritingMode frameMode, double fw, double fh, const Rect& rectInFrame) {
  Rect r;
  FrameToPhysical(frameMode, fw, fh, rectInFrame.x, rectInFrame.y, rectInFrame.width, rectInFrame.height, r);
  b.x = r.x;
  b.y = r.y;
  b.width = r.width;
  b.height = r.height;
  b.margin = EdgesToPhysical(b.Mode(), b.margin);
  b.border = EdgesToPhysical(b.Mode(), b.border);
  b.padding = EdgesToPhysical(b.Mode(), b.padding);
  double px, py;
  VectorToPhysical(b.Mode(), b.shiftX, b.shiftY, px, py);
  b.shiftX = px;
  b.shiftY = py;
  const double ow = b.ownWidth, oh = b.ownHeight;
  ConvertContents(b);
  b.ownWidth = ow;
  b.ownHeight = oh;
}

}  // namespace solar::layout
