#pragma once

#include <string>
#include <vector>

#include "solar/dom/Node.h"

// Style (https://drafts.csswg.org/css-cascade/, css-values): which declarations apply to an element, which of them wins,
// and the computed value each property comes to.
namespace solar::css {

// The computed value of a longhand property for an element, as getComputedStyle answers: empty for an element that is not
// in a document, or a property that has no such value.
std::string ComputedValue(Quanta::Context& ctx, dom::Element* element, const std::string& property, const std::string& pseudo = "");

// The longhand properties getComputedStyle lists, in the order it lists them.
const std::vector<std::string>& ComputedPropertyNames();

// The style sheets that apply to the tree the node is the root of (a document or a shadow root), in the order of the cascade.
struct CssStyleSheet;
std::vector<CssStyleSheet*> SheetsOfTreeRoot(dom::Node* root);

// Something a style depends on changed (a sheet, a declaration): computed values are made again.
void NoteStyleChange();
uint64_t StyleVersion();

}  // namespace solar::css
