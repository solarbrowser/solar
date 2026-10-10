#pragma once

#include <vector>

#include "solar/layout/Box.h"

// Layout (https://www.w3.org/TR/CSS22/visudet.html): the sizes and places of the boxes of a document. Computation only: nothing here draws.
namespace solar::layout {

// The layout of the document, made again if the tree or its style changed since it was last made.
Tree* UpdateLayout(Quanta::Context& ctx, dom::Document* document);

// The boxes an element made (more than one when an inline box was split around a block), in tree order; none for display: none.
const std::vector<Box*>& BoxesOf(Tree& tree, const dom::Node* node);

// A box's border box in the coordinates of the document (the initial containing block's origin).
Rect AbsoluteBorderBox(const Box& box);
// The rectangles of an element as getClientRects gives them, in the coordinates of the viewport, and their union.
std::vector<Rect> ClientRects(Tree& tree, dom::Element* element);

// The elements under a point of the viewport, topmost first (as elementsFromPoint has them); empty outside the viewport.
std::vector<dom::Element*> ElementsAtPoint(Tree& tree, double x, double y);
// How far the element may scroll in each direction (0 for what does not scroll); the viewport for the root element.
void ScrollRange(Tree& tree, dom::Element* element, double& maxX, double& maxY);

// Text for looking at layout by eye or in a test.
std::string DumpTree(const Tree& tree);

// The computed style properties whose resolved value is the used one, for an element that has a box: width, height, margins...
// Nothing for what is not one (or an element with no box).
bool UsedValue(Tree& tree, dom::Element* element, const std::string& property, std::string& out);

}  // namespace solar::layout
