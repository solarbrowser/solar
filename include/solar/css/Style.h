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

// What getComputedStyle answers: the used value where the property has one that layout works out (the width of a box, margins in px),
// the computed value otherwise. Layout installs the hook that knows the used values.
using ResolvedValueHook = bool (*)(Quanta::Context& ctx, dom::Element* element, const std::string& property, const std::string& pseudo, std::string& out);
void SetResolvedValueHook(ResolvedValueHook hook);
std::string ResolvedValue(Quanta::Context& ctx, dom::Element* element, const std::string& property, const std::string& pseudo = "");

// The longhand properties getComputedStyle lists, in the order it lists them.
const std::vector<std::string>& ComputedPropertyNames();

// The style sheets that apply to the tree the node is the root of (a document or a shadow root), in the order of the cascade.
struct CssStyleSheet;
std::vector<CssStyleSheet*> SheetsOfTreeRoot(dom::Node* root);

// Something a style depends on changed (a sheet, a declaration): computed values are made again.
void NoteStyleChange();
uint64_t StyleVersion();
// An animation changed the values it gives: computed values are made again, but nothing that was declared is different.
void NoteAnimatedStyleChange();
// Keeps the computed values the element has now (of the properties that can be animated), as the style that a change of style that
// follows is compared with, which a transition starts from.
void SnapshotComputedValues(Quanta::Context& ctx, dom::Element* element);
// Layout, for the styles that depend on it (container queries): called before a computed style is read when the sizes of containers may have changed.
using LayoutHook = void (*)(Quanta::Context& ctx, dom::Document* document);
void SetLayoutHook(LayoutHook hook);
// Something declared animation-* or transition-* was parsed: how many times, so that whatever watches for animations can know
// whether there is a reason to.
void NoteAnimationMention();
// The document whose animationWake is called when there is one.
void SetAnimationWake(Quanta::Context& ctx, dom::Document* document);
uint64_t AnimationMentions();
// A number that changes with the tree and with what is declared, not with what animations give.
uint64_t AuthorStyleVersion();

}  // namespace solar::css
