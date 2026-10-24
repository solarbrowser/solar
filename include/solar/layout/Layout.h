#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "solar/layout/Box.h"

// Layout (https://www.w3.org/TR/CSS22/visudet.html): the sizes and places of the boxes of a document. Computation only: nothing here draws.
namespace solar::layout {

// The layout of the document, made again if the tree or its style changed since it was last made.
// Moves the sticky positioned boxes to where the scroll positions of their scroll containers put them.
void ApplySticky(Tree& tree);

Tree* UpdateLayout(Quanta::Context& ctx, dom::Document* document);

// The boxes an element made (more than one when an inline box was split around a block), in tree order; none for display: none.
const std::vector<Box*>& BoxesOf(Tree& tree, const dom::Node* node);

// A 4x4 matrix, column-major as the CSS matrix3d() has it.
struct Matrix {
  double m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  Matrix operator*(const Matrix& other) const;
  bool IsIdentity() const;
  bool Is2d() const;
  void Map(double x, double y, double& outX, double& outY) const;
};
// The transform list as a matrix (percentages of the given box size); false if it is not valid.
bool ParseTransformList(const std::string& text, double width, double height, Matrix& out);
// The matrix a box's transform properties come to, about its transform origin, in the box's own coordinates; false if it has none.
bool TransformOf(const Box& box, Matrix& out);

// The bytes of an image at `address` (against `base`): a data: address decoded, any other from the loader of the page; each is loaded once. Null where there are none.
// `absolute` gets the address the image is known by.
// `pending` says the picture is on its way (see SetImageRequestHandler): there is nothing now, and it is not a failure.
std::shared_ptr<const std::string> ImageDataOf(const std::string& address, const std::string& base, std::string* absolute = nullptr, bool* pending = nullptr);
// Asked of an address that is not here yet: true if the program is getting it (then the answer is a later one), false to have it loaded where it is asked for.
using ImageRequestHandler = std::function<bool(const std::string& absolute)>;
// The pictures kept for the pages (their bytes and sizes) are let go: a page that is left has no use for them.
void ForgetImages();
// How long layout has taken (for looking at where the time of a page goes).
double LayoutMillis();
void SetImageRequestHandler(ImageRequestHandler handler);
std::string AbsoluteAddress(const std::string& address, const std::string& base);

// The font a box's text is set in (the first of its family list that has a face), for text the program draws itself, as in form controls.
std::shared_ptr<font::Face> FaceOf(Quanta::Context& ctx, Tree& tree, const Box& box);

// A box's border box in the coordinates of the document (the initial containing block's origin).
Rect AbsoluteBorderBox(const Box& box);
double AbsoluteInset(const Box& box, int side, double viewportWidth, double viewportHeight);
// The rectangles of an element as getClientRects gives them, in the coordinates of the viewport, and their union.
std::vector<Rect> ClientRects(Tree& tree, dom::Element* element);

// The elements under a point of the viewport, topmost first (as elementsFromPoint has them); empty outside the viewport.
std::vector<dom::Element*> ElementsAtPoint(Tree& tree, double x, double y);
// The element whose scroll position and sizes are the viewport's: the root element, or in quirks mode the body.
dom::Element* ViewportElement(dom::Document* document);
// The scrollable overflow area of a scroll container (the viewport, for the root box: pass the root element's box's element), in the coordinates of its
// padding box: the padding box and what sticks out of it where it can be scrolled to.
Rect ScrollableOverflow(Tree& tree, const Box& scroller, dom::Element* rootElement);
// The transforms of a box and the boxes above it down from `stop` (excluded), as a matrix; false if there are none.
bool TransformBelow(const Box& box, const Box* stop, Matrix& out);
// How far the element may scroll in each direction, and how far back (negative where the start of the scrolling is the far side).
void ScrollBounds(Tree& tree, dom::Element* element, double& minX, double& minY, double& maxX, double& maxY);
// How far the element may scroll in each direction (0 for what does not scroll); the viewport for the root element.
void ScrollRange(Tree& tree, dom::Element* element, double& maxX, double& maxY);

// Text for looking at layout by eye or in a test.
std::string DumpTree(const Tree& tree);

// The computed style properties whose resolved value is the used one, for an element that has a box: width, height, margins...
// Nothing for what is not one (or an element with no box).
bool UsedValue(Tree& tree, dom::Element* element, const std::string& property, std::string& out);

}  // namespace solar::layout
