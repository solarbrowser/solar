#pragma once

#include <memory>

#include <string>
#include <vector>

#include "solar/dom/Node.h"

// Style (https://drafts.csswg.org/css-cascade/, css-values): which declarations apply to an element, which of them wins,
// and the computed value each property comes to.
namespace solar::css {

// The computed value of a longhand property for an element, as getComputedStyle answers: empty for an element that is not
// in a document, or a property that has no such value.
// Whether a declaration of content-visibility: auto has been read: then layout has the work of finding which elements are skipped.
extern bool g_contentVisibilityAutoMentioned;
// Whether what layout made of an element's style can be kept while the element, its parent and its sheets are not changed, and what else it depends on:
// the position among siblings (structural pseudo-classes) and attributes of the siblings before it (sibling combinators).
struct StyleReuse {
  bool allowed = false;
  bool structural = false;
  bool sibling = false;
  bool shadows = false;  // the document has shadow trees: the boxes are of the flat tree, which these stamps do not follow
  bool has = false;  // :has(): an element depends on what is under it (and beside it), which the stamps of the styles kept say
  bool stateAncestor = false;  // a state (hover, focus) is asked of an element that is not the subject of a rule: the ones under it depend on it
};
StyleReuse StyleReuseFor(dom::Document* document);
// A number that changes when the styles kept of the element are made anew (see Validate): what was read of them is good while it is the same.
uint64_t StyleEpochOf(dom::Element* element);
// Changes when a style sheet or what the cascade depends on besides the elements does.
uint64_t SheetsVersion();
// Changes when what was kept of the styles is no longer good (animations count).
uint64_t StyleCacheVersion();
// The version that animations and transitions look at: changes with the tree, the sheets and the states of elements.
uint64_t FlushVersion();
// Changes when what an animation gives changes (for what is made again from it, as a layout).
uint64_t AnimationVersion();
// Whether any rule gives the pseudo-element of the element a style.
bool HasPseudoStyle(dom::Element* element, const std::string& pseudo);
// The width the vertical scrollbar or gutter of the viewport takes, and the height of the horizontal one.
// Whether the overflow of the viewport is auto on each axis (a scrollbar of that thickness comes when the contents overflow).
struct ViewportAuto {
  bool x = false, y = false;
  double thickness = 0;
};
void ViewportScrollbars(Quanta::Context& ctx, dom::Document* document, double& vertical, double& horizontal, ViewportAuto* autos = nullptr);
std::string ComputedValue(Quanta::Context& ctx, dom::Element* element, const std::string& property, const std::string& pseudo = "");

// Many computed values of one element (or one of its pseudo-elements) in a row: the same as ComputedValue, without what each call does around the lookup.
class StyleReader {
 public:
  StyleReader(Quanta::Context& ctx, dom::Element* element, const std::string& pseudo = "");
  ~StyleReader();
  std::string Get(const char* property);
  std::string Get(const std::string& property) { return Get(property.c_str()); }

 private:
  Quanta::Context& ctx_;
  dom::Element* element_;
  const std::string& pseudo_;
  std::shared_ptr<void> memo_;
  uint64_t version_ = 0;
};

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
// A style sheet is being parsed: script must not run until it is done (what is half made is not safe to collect).
double SheetParseMillis();
void BeginSheetParse();
void EndSheetParse();
// Whether any declaration for the element (or its pseudo-element) is about animations or transitions: what has none has nothing to start.
bool DeclaresAnimation(dom::Element* element, const std::string& pseudo);
// Whether a rule that could apply to the element (or its pseudo-element) is about animations or transitions, worked out without making the style: false is sure.
// Whether a rule of the sheets of the document is for ::before, ::after or ::marker.
bool AuthorSheetsHavePseudoElements(dom::Document* document);
bool MayDeclareAnimation(dom::Element* element, const std::string& pseudo);
// Scrollbars of the viewport that float over the page and take no room (a window; the tests have the ones that do).
void SetOverlayScrollbars(bool overlay);
// A document that is gone (a page that was left): what style kept for it is let go.
void ForgetDocument(dom::Document* document);
uint64_t StyleVersion();
// An animation changed the values it gives: computed values are made again, but nothing that was declared is different.
void NoteAnimatedStyleChange(bool affectsSizes = true, bool onlyItself = false);
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

// A token that elements with the same computed style have in common (they share what was worked out of them), or none: what is read of the style
// of one of them is the same for the others.
std::shared_ptr<void> SharedStyleToken(Quanta::Context& ctx, dom::Element* element, const std::string& pseudo = "");

}  // namespace solar::css
