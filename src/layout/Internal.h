#pragma once

#include <limits>

#include "solar/layout/Layout.h"

namespace solar::layout {

// A float, as its margin box in the coordinates of the block formatting context it is in.
struct FloatRecord {
  Box* box;
  double left, top, right, bottom;
  bool isLeft;
};

// The floats of a block formatting context, in coordinates of the context's root content box.
struct Bfc {
  std::vector<FloatRecord> floats;
  double lowest = 0;  // the largest bottom of any float
  // The part of [left, right] that is free of floats between y and y + height: the line a box can use there.
  void Band(double y, double height, double& left, double& right) const;
  // The y below the lowest float edge above `y` that is still in the way, or y if none.
  double NextEdge(double y, double height) const;
  double ClearY(bool left, bool right) const;
};

struct LayoutContext {
  Quanta::Context& ctx;
  Tree& tree;
  double viewportWidth, viewportHeight;
  // The block formatting context being laid out in, and where the box being laid out is in it.
  Bfc* bfc = nullptr;
  double cbX = 0;     // the containing block's content left edge, in the context's coordinates
  double boxY = 0;    // the border-top edge of the box about to be laid out (an estimate: it is checked afterwards)
  uint64_t floatEvents = 0;  // floats placed or looked at, to tell whether a layout depended on where a box was
  // One-shot overrides for the box laid out next (positioned boxes whose sizes their offsets give).
  double forceWidth = std::numeric_limits<double>::quiet_NaN();   // content width
  double forceHeight = std::numeric_limits<double>::quiet_NaN();  // content height
  bool keepOwnFrame = false;  // positioned boxes: left in their own frame, placed by PlaceOne
  // The width an auto-width box has to fit in when it is not the containing block's (beside floats).
  double availOverride = std::numeric_limits<double>::quiet_NaN();
  // For the lines of an inline formatting context: where the container's content box is in the block formatting context.
  double containerX = 0, containerY = 0;
};

// Build.cpp: the box tree of the document.
void BuildTree(LayoutContext& lc, dom::Document* document);
std::shared_ptr<const BoxStyle> AnonymousStyle(const BoxStyle& parent, Display display);

// Block.cpp: block layout.
void LayoutRoot(LayoutContext& lc);
// Lays out a block-level box in a containing block `cbWidth` wide, and `cbHeight` high where that is known (NaN where not).
// With `shrinkToFit` an auto width is the box's contents' preferred width, within what is available (floats, inline-blocks, positioned boxes).
void LayoutBlockLevel(LayoutContext& lc, Box& box, double cbWidth, double cbHeight, bool shrinkToFit = false);
// The intrinsic widths of the box's contents.
void ComputeContentSizes(LayoutContext& lc, Box& box);

// Places a float (a block laid out shrink-to-fit) as high and as far to its side as the floats before it and `minY` allow, in a containing block whose
// content box is [cbLeft, cbLeft + cbWidth] wide in the context. Returns its margin box.
FloatRecord PlaceFloat(LayoutContext& lc, Box& box, double cbLeft, double cbWidth, double minY);
// Out-of-flow positioned boxes, once everything else is where it is.
void PlacePositioned(LayoutContext& lc);

// Flex.cpp: a flex container's items laid out. Sets the box's content height; `heightBasis` is the definite content height, or NaN.
void LayoutFlex(LayoutContext& lc, Box& container, double contentWidth, double heightBasis, double& contentHeight);
void FlexContentSizes(LayoutContext& lc, Box& container, double& minContent, double& maxContent);
inline bool IsFlexDisplay(Display d) { return d == Display::Flex || d == Display::InlineFlex; }

// Table.cpp
void LayoutTable(LayoutContext& lc, Box& table, double contentWidth, double heightBasis, double& contentHeight);
void TableContentSizes(LayoutContext& lc, Box& table, double& minContent, double& maxContent);
inline bool IsTableDisplay(Display d) { return d == Display::Table || d == Display::InlineTable; }

// Grid.cpp
void LayoutGrid(LayoutContext& lc, Box& container, double contentWidth, double heightBasis, double& contentHeight);
void GridContentSizes(LayoutContext& lc, Box& container, double& minContent, double& maxContent);
inline bool IsGridDisplay(Display d) { return d == Display::Grid || d == Display::InlineGrid; }

// Inline.cpp: the lines of a block container with inline content, in a content box `width` wide. Sets the box's content height.
double LayoutInlineContent(LayoutContext& lc, Box& container, double width, double& baseline);
void InlineContentSizes(LayoutContext& lc, Box& container, double& minContent, double& maxContent);

// Image.cpp: the size an image file says it has (width and height negative where it gives none, the ratio 0 where it has none).
bool ReadImageMetrics(const std::string& data, double& width, double& height, double& ratio);
bool ImageMetricsOf(const std::string& address, const std::string& base, double& width, double& height, double& ratio);

// The content size a replaced box comes to in a containing block of that size (NaN height: not known).
void ReplacedContentSize(const Box& box, double cbWidth, double cbHeight, double& width, double& height);

// Frame.cpp: writing modes.
bool Orthogonal(WritingMode a, WritingMode b);
void FrameToPhysical(WritingMode mode, double frameWidth, double frameHeight, double x, double y, double w, double h, Rect& out);
void PhysicalToFrame(WritingMode mode, double frameWidth, double frameHeight, const Rect& physical, double& x, double& y, double& w, double& h);
void VectorToPhysical(WritingMode mode, double x, double y, double& px, double& py);
void VectorToFrame(WritingMode mode, double px, double py, double& x, double& y);
Edges EdgesToPhysical(WritingMode mode, const Edges& e);
Edges EdgesToFrame(WritingMode mode, const Edges& e);
// At the end of a box's layout: its size and edges in its parent's frame (own sizes kept in ownWidth and ownHeight).
void AdaptToParentFrame(Box& box);
void ConvertTree(Tree& tree);
void ConvertPlaced(Box& b, WritingMode frameMode, double fw, double fh, const Rect& rectInFrame);

// Fonts of a box's style.
std::shared_ptr<font::Face> PrimaryFace(LayoutContext& lc, const Box& box);

}  // namespace solar::layout
