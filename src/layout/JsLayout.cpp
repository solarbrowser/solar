// The natives under the geometry APIs of elements (getBoundingClientRect, offsetWidth, clientWidth...), which are written in script over them.
#include <cmath>
#include <iostream>

#include "Internal.h"
#include "solar/css/Style.h"
#include "solar/dom/NodeBindingsInternal.h"

namespace solar::layout {

void AskForContentVisibilityFrame();

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Value;

namespace {

dom::Element* ElementArg(qe::Args args) { return args.empty() ? nullptr : DOMObject::Cast<dom::Element>(args[0]); }

Tree* TreeFor(Context& ctx, dom::Element* element) {
  dom::Document* document = element ? element->nodeDocument : nullptr;
  if (!document) return nullptr;
  return UpdateLayout(ctx, document);
}

// The box that stands for the element in its parent's layout: the first block-level or atomic one.
Box* PrincipalBox(Tree& tree, dom::Element* element) {
  const std::vector<Box*>& boxes = BoxesOf(tree, element);
  for (Box* b : boxes) if (b->kind == Box::Kind::Block) return b;
  return boxes.empty() ? nullptr : boxes[0];
}

// The union of the boxes below, relative to `origin`, for scrollWidth and scrollHeight.
void Extent(const Box& box, double ox, double oy, double& right, double& bottom, bool& any) {
  const Rect r = AbsoluteBorderBox(box);
  if (box.kind == Box::Kind::Block) {
    right = std::max(right, r.x + r.width - ox);
    bottom = std::max(bottom, r.y + r.height - oy);
    any = true;
  }
  for (const auto& line : box.lines) {
    for (const LineItem& item : line.items) {
      if (item.kind != LineItem::Kind::Text) continue;
      right = std::max(right, r.x + item.rect.Right() - ox);
      bottom = std::max(bottom, r.y + item.rect.Bottom() - oy);
      any = true;
    }
  }
  for (const auto& c : box.children) Extent(*c, ox, oy, right, bottom, any);
}

Value Metrics(Context& ctx, Value, qe::Args args, Value) {
  dom::Element* element = ElementArg(args);
  Value out = qe::NewArray(ctx);
  const auto push = [&](double v) { qe::ArrayPush(ctx, out, qe::FromNumber(v)); };
  Tree* tree = TreeFor(ctx, element);
  Box* box = tree ? PrincipalBox(*tree, element) : nullptr;
  if (!box) {
    for (int i = 0; i < 10; ++i) push(0);
    push(0);
    return out;
  }
  const Rect abs = AbsoluteBorderBox(*box);
  const bool isRoot = ViewportElement(element->nodeDocument) == element;
  const bool inlineBox = box->kind != Box::Kind::Block;
  double clientWidth = inlineBox ? 0 : box->width - box->border.Horizontal() - box->scrollbar.Horizontal();
  double clientHeight = inlineBox ? 0 : box->height - box->border.Vertical() - box->scrollbar.Vertical();
  if (isRoot) {
    clientWidth = tree->viewportWidth;
    clientHeight = tree->viewportHeight;
  }
  double scrollWidth = clientWidth, scrollHeight = clientHeight;
  if (!inlineBox) {
    const bool viewportBox = isRoot;
    const Box& scroller = viewportBox ? *tree->root : *box;
    const Rect overflow = ScrollableOverflow(*tree, scroller, viewportBox ? element : nullptr);
    scrollWidth = std::max(clientWidth, overflow.width);
    scrollHeight = std::max(clientHeight, overflow.height);
  }
  push(inlineBox ? 0 : box->border.top + box->scrollbar.top);
  push(inlineBox ? 0 : box->border.left + box->scrollbar.left);
  push(clientWidth);
  push(clientHeight);
  push(inlineBox ? 0 : scrollWidth);
  push(inlineBox ? 0 : scrollHeight);
  // The border box for offset*: an inline's is the union of its fragments.
  Rect border = abs;
  if (inlineBox) {
    const std::vector<Rect> rects = ClientRects(*tree, element);
    if (!rects.empty()) {
      double l = rects[0].x, t = rects[0].y, r = rects[0].Right(), b = rects[0].Bottom();
      for (const Rect& q : rects) { l = std::min(l, q.x); t = std::min(t, q.y); r = std::max(r, q.Right()); b = std::max(b, q.Bottom()); }
      border = {l, t, r - l, b - t};
    }
  }
  push(border.y);
  push(border.x);
  push(border.width);
  push(border.height);
  push(1);
  return out;
}

Value ClientRectsNative(Context& ctx, Value, qe::Args args, Value) {
  dom::Element* element = ElementArg(args);
  Value out = qe::NewArray(ctx);
  Tree* tree = TreeFor(ctx, element);
  if (!tree) return out;
  for (const Rect& r : ClientRects(*tree, element)) {
    qe::ArrayPush(ctx, out, qe::FromNumber(r.x));
    qe::ArrayPush(ctx, out, qe::FromNumber(r.y));
    qe::ArrayPush(ctx, out, qe::FromNumber(r.width));
    qe::ArrayPush(ctx, out, qe::FromNumber(r.height));
  }
  return out;
}

// The parent in the flat tree: the slot a slottable is assigned to, the host of a shadow root.
dom::Node* FlatParentOf(dom::Node* node) {
  if (node->IsElement() && static_cast<dom::Element*>(node)->assignedSlot) return static_cast<dom::Element*>(node)->assignedSlot;
  dom::Node* p = node->parentNode;
  if (p && p->IsFragment() && static_cast<dom::DocumentFragment*>(p)->isShadowRoot) return static_cast<dom::DocumentFragment*>(p)->host;
  return p;
}

// The element offsetTop and the others are relative to, or null.
Value OffsetParent(Context& ctx, Value, qe::Args args, Value) {
  dom::Element* element = ElementArg(args);
  Tree* tree = TreeFor(ctx, element);
  Box* box = tree ? PrincipalBox(*tree, element) : nullptr;
  if (!box) return qe::Null();
  dom::Document* document = element->nodeDocument;
  if (document->DocumentElement() == element) return qe::Null();
  if (element->IsHtml("body")) return qe::Null();
  const bool fixed = box->style->position == Position::Fixed;
  for (dom::Node* n = FlatParentOf(element); n; n = FlatParentOf(n)) {
    if (!n->IsElement()) continue;
    dom::Element* e = static_cast<dom::Element*>(n);
    Box* b = PrincipalBox(*tree, e);
    if (fixed) {
      if (b && b->style->containsPositioned) return dom::NodeValue(e);
      continue;
    }
    if (e->IsHtml("body")) return dom::NodeValue(e);
    if (!b) continue;
    if (b->style->position != Position::Static || b->style->containsPositioned) return dom::NodeValue(e);
    if (e->IsHtml("td") || e->IsHtml("th") || e->IsHtml("table")) return dom::NodeValue(e);
  }
  return qe::Null();
}

// __solarLayoutScroll(element or null, set, x, y): [x, y, maxX, maxY] of the element's scroll position (the viewport's for null).
Value Scroll(Context& ctx, Value, qe::Args args, Value) {
  dom::Element* element = args.empty() ? nullptr : DOMObject::Cast<dom::Element>(args[0]);
  dom::Document* document = element ? element->nodeDocument : dom::AssociatedDocument(ctx);
  Value out = qe::NewArray(ctx);
  if (!document) return out;
  Tree* tree = UpdateLayout(ctx, document);
  const bool viewport = !element || element == ViewportElement(document);
  const dom::Node* key = viewport ? static_cast<const dom::Node*>(document) : element;
  double minX = 0, minY = 0, maxX = 0, maxY = 0;
  ScrollBounds(*tree, viewport ? nullptr : element, minX, minY, maxX, maxY);
  auto& position = tree->scroll[key];
  if (args.size() >= 4 && qe::ToBoolean(args[1])) {
    const double x = qe::ToNumber(ctx, args[2]), y = qe::ToNumber(ctx, args[3]);
    position.first = std::isnan(x) ? position.first : std::max(minX, std::min(maxX, x));
    position.second = std::isnan(y) ? position.second : std::max(minY, std::min(maxY, y));
  }
  position.first = std::max(minX, std::min(position.first, maxX));
  position.second = std::max(minY, std::min(position.second, maxY));
  if (args.size() >= 4 && qe::ToBoolean(args[1])) {
    ApplySticky(*tree);
    // What is near the viewport is worked out from where it is scrolled to.
    if (document->contentVisibilityAutoSeen) tree->builtFor = 0;
    AskForContentVisibilityFrame();
  }
  qe::ArrayPush(ctx, out, qe::FromNumber(position.first));
  qe::ArrayPush(ctx, out, qe::FromNumber(position.second));
  qe::ArrayPush(ctx, out, qe::FromNumber(maxX));
  qe::ArrayPush(ctx, out, qe::FromNumber(maxY));
  return out;
}

// __solarLayoutElementsAt(x, y): the elements under the point, topmost first.
Value ElementsAt(Context& ctx, Value, qe::Args args, Value) {
  Value out = qe::NewArray(ctx);
  dom::Document* document = dom::AssociatedDocument(ctx);
  if (!document || args.size() < 2) return out;
  Tree* tree = UpdateLayout(ctx, document);
  for (dom::Element* e : ElementsAtPoint(*tree, qe::ToNumber(ctx, args[0]), qe::ToNumber(ctx, args[1]))) qe::ArrayPush(ctx, out, dom::NodeValue(e));
  return out;
}

Value Dump(Context& ctx, Value, qe::Args args, Value) {
  dom::Document* document = args.empty() ? dom::AssociatedDocument(ctx) : nullptr;
  if (!document) return qe::Undefined();
  Tree* tree = UpdateLayout(ctx, document);
  return qe::FromWtf8(ctx, DumpTree(*tree));
}

// The byte of the UTF-8 text at a UTF-16 offset.
size_t ByteOfUnit(const std::string& text, uint32_t unit) {
  size_t byte = 0;
  uint32_t units = 0;
  while (byte < text.size() && units < unit) {
    const unsigned char c = static_cast<unsigned char>(text[byte]);
    const size_t length = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    units += length == 4 ? 2 : 1;
    byte += length;
  }
  return std::min(byte, text.size());
}

// __solarLayoutProcessedText(textNode): the text of the node as the lines show it (white space processed, case transformed), or null if it has no box.
Value ProcessedText(Context& ctx, Value, qe::Args args, Value) {
  dom::Node* node = args.empty() ? nullptr : dom::ThisNode(ctx, args[0]);
  if (!node || !node->nodeDocument) return qe::Null();
  Tree* tree = UpdateLayout(ctx, node->nodeDocument);
  std::string out;
  bool any = false;
  for (Box* box : BoxesOf(*tree, node)) {
    if (box->kind != Box::Kind::Text) continue;
    // Contents that are skipped (content-visibility) are not rendered text.
    bool skipped = false;
    for (const Box* p = box->parent; p; p = p->parent) if (p->style && p->style->skipContents) skipped = true;
    if (skipped) continue;
    out += box->processed;
    any = true;
  }
  return any ? qe::FromWtf8(ctx, out) : qe::Null();
}

// __solarLayoutRangeRects(textNode, startUnit, endUnit): [x, y, w, h...] of the characters [start, end) of the text node, as the lines show them.
Value RangeRects(Context& ctx, Value, qe::Args args, Value) {
  Value out = qe::NewArray(ctx);
  dom::Node* node = args.empty() ? nullptr : dom::ThisNode(ctx, args[0]);
  if (!node || !node->nodeDocument || args.size() < 3) return out;
  const dom::CharacterData* text = static_cast<const dom::CharacterData*>(node);
  Tree* tree = UpdateLayout(ctx, node->nodeDocument);
  const size_t from = ByteOfUnit(text->data, static_cast<uint32_t>(qe::ToNumber(ctx, args[1])));
  const size_t to = ByteOfUnit(text->data, static_cast<uint32_t>(qe::ToNumber(ctx, args[2])));
  if (to <= from) return out;
  for (Box* box : BoxesOf(*tree, node)) {
    if (box->kind != Box::Kind::Text) continue;
    const Box* container = box->parent;
    while (container && container->kind != Box::Kind::Block) container = container->parent;
    if (!container) continue;
    const Rect origin = AbsoluteBorderBox(*container);
    for (const Line& line : container->lines) {
      for (const LineItem& item : line.items) {
        if (item.kind != LineItem::Kind::Text || item.box != box) continue;
        // The glyphs whose characters are in the range, along the piece.
        double pen = 0, low = 0, high = 0;
        bool any = false;
        for (const font::Glyph& g : item.text.glyphs) {
          const size_t processed = item.text.begin + g.cluster;
          const double width = g.advance * item.text.fontSize;
          const size_t source = processed < box->processedSource.size() ? box->processedSource[processed] + box->textOffset : box->textOffset + box->text.size();
          if (source >= from && source < to) {
            if (!any) low = pen;
            any = true;
            high = pen + width;
            low = std::min(low, pen);
          }
          pen += width;
        }
        if (!any) continue;
        const double x = origin.x + item.rect.x + low;
        qe::ArrayPush(ctx, out, qe::FromNumber(x));
        qe::ArrayPush(ctx, out, qe::FromNumber(origin.y + item.rect.y));
        qe::ArrayPush(ctx, out, qe::FromNumber(high - low));
        qe::ArrayPush(ctx, out, qe::FromNumber(item.rect.height));
      }
    }
  }
  return out;
}

}  // namespace

void InstallResolvedHook();


namespace {
Context* g_cvContext = nullptr;
bool g_cvRequested = false;
bool g_updatesWanted = false;  // observers are watching: a frame is asked for when the tree, or the scroll, changes
}  // namespace

// A change of the tree, where content-visibility: auto is in use: a frame is asked for, as the rendering is updated after it.
void AskForContentVisibilityFrame() {
  if (g_cvRequested || !(css::g_contentVisibilityAutoMentioned || g_updatesWanted) || !g_cvContext) return;
  dom::Document* document = dom::AssociatedDocument(*g_cvContext);
  if (!document || !document->contentVisibilityWake) return;
  g_cvRequested = true;
  Quanta::Embed::Call(*g_cvContext, qe::FromObject(document->contentVisibilityWake), qe::Undefined());
}

// __solarLayoutWantUpdates(bool): whether a frame is wanted when the tree or the scroll position changes.
Value WantUpdates(Context&, Value, qe::Args args, Value) {
  g_updatesWanted = !args.empty() && qe::ToBoolean(args[0]);
  return qe::Undefined();
}

// __solarLayoutBoxSizes(element): [border width, border height, content width, content height, padding left, padding top] of the box as layout made it (no transforms), or null.
Value BoxSizes(Context& ctx, Value, qe::Args args, Value) {
  dom::Element* element = args.empty() ? nullptr : DOMObject::Cast<dom::Element>(args[0]);
  if (!element || !element->nodeDocument) return qe::Null();
  Tree* tree = UpdateLayout(ctx, element->nodeDocument);
  const std::vector<Box*>& boxes = BoxesOf(*tree, element);
  Value out = qe::NewArray(ctx);
  for (double v : {0.0, 0.0, 0.0, 0.0, 0.0, 0.0}) qe::ArrayPush(ctx, out, qe::FromNumber(v));
  if (boxes.empty() || boxes[0]->kind != Box::Kind::Block) return out;
  const Box& box = *boxes[0];
  Value result = qe::NewArray(ctx);
  qe::ArrayPush(ctx, result, qe::FromNumber(box.width));
  qe::ArrayPush(ctx, result, qe::FromNumber(box.height));
  qe::ArrayPush(ctx, result, qe::FromNumber(std::max(0.0, box.width - box.border.Horizontal() - box.padding.Horizontal())));
  qe::ArrayPush(ctx, result, qe::FromNumber(std::max(0.0, box.height - box.border.Vertical() - box.padding.Vertical())));
  qe::ArrayPush(ctx, result, qe::FromNumber(box.padding.left));
  qe::ArrayPush(ctx, result, qe::FromNumber(box.padding.top));
  return result;
}

// __solarLayoutSetCvWake(function): the function that asks for a frame to send the contentvisibilityautostatechange events in.
Value SetCvWake(Context& ctx, Value, qe::Args args, Value) {
  dom::Document* document = dom::AssociatedDocument(ctx);
  if (document) {
    document->contentVisibilityWake = !args.empty() && qe::IsObject(args[0]) ? args[0].as_object() : nullptr;
    document->NoteWrite();
  }
  return qe::Undefined();
}

// __solarLayoutCvTake(): [element, skipped, ...] for the content-visibility: auto elements whose state is not what the last event told.
Value CvTake(Context& ctx, Value, qe::Args, Value) {
  g_cvRequested = false;
  Value out = qe::NewArray(ctx);
  dom::Document* document = dom::AssociatedDocument(ctx);
  if (!document || !(document->contentVisibilityAutoSeen || css::g_contentVisibilityAutoMentioned)) return out;
  Tree* tree = UpdateLayout(ctx, document);
  const auto walk = [&](auto&& self, const Box& box) -> void {
    // The size of what is rendered in a frame is what a skipped box has next, if it asks for the last size (contain-intrinsic-size: auto, and content-visibility: auto).
    if (box.kind == Box::Kind::Block && box.node && box.node->IsElement() && box.style && !box.style->skipContents &&
        (box.style->containIntrinsicWidthAuto || box.style->containIntrinsicHeightAuto || box.style->contentVisibilityAuto)) {
      dom::Element* element = static_cast<dom::Element*>(box.node);
      element->lastRenderedWidth = std::max(0.0, box.width - box.border.Horizontal() - box.padding.Horizontal());
      element->lastRenderedHeight = std::max(0.0, box.height - box.border.Vertical() - box.padding.Vertical());
    }
    if (box.kind == Box::Kind::Block && box.node && box.node->IsElement() && box.style && box.style->contentVisibilityAuto) {
      dom::Element* element = static_cast<dom::Element*>(box.node);
      const signed char state = element->contentVisible ? 0 : 1;
      if (element->contentVisibilityReported != state) {
        element->contentVisibilityReported = state;
        qe::ArrayPush(ctx, out, qe::FromObject(element));
        qe::ArrayPush(ctx, out, qe::FromBool(state == 1));
      }
    }
    for (const auto& child : box.children) self(self, *child);
  };
  if (tree && tree->root) walk(walk, *tree->root);
  return out;
}

bool BeingRendered(dom::Element* element) {
  dom::Document* document = element->nodeDocument;
  if (!g_cvContext || !document || document != dom::AssociatedDocument(*g_cvContext)) return true;
  Tree* tree = UpdateLayout(*g_cvContext, document);
  if (!tree) return true;
  const std::vector<Box*>& boxes = BoxesOf(*tree, element);
  if (boxes.empty()) {
    // (display: contents has no box of its own, and is rendered.)
    return solar::css::ComputedValue(*g_cvContext, element, "display") == "contents";
  }
  for (const Box* p = boxes[0]->parent; p; p = p->parent) {
    if (p->style && p->style->skipContents && !p->style->contentVisibilityAuto) return false;
  }
  return true;
}

void InstallLayoutNatives(Context& ctx) {
  dom::g_beingRenderedHook = BeingRendered;
  g_cvContext = &ctx;
  dom::g_treeChangeHook = AskForContentVisibilityFrame;
  InstallResolvedHook();
  qe::DefineGlobalFunction(ctx, "__solarLayoutMetrics", Metrics, 1);
  qe::DefineGlobalFunction(ctx, "__solarLayoutRects", ClientRectsNative, 1);
  qe::DefineGlobalFunction(ctx, "__solarLayoutOffsetParent", OffsetParent, 1);
  qe::DefineGlobalFunction(ctx, "__solarLayoutScroll", Scroll, 4);
  qe::DefineGlobalFunction(ctx, "__solarLayoutElementsAt", ElementsAt, 2);
  qe::DefineGlobalFunction(ctx, "__solarLayoutDump", Dump, 0);
  qe::DefineGlobalFunction(ctx, "__solarLayoutRangeRects", RangeRects, 3);
  qe::DefineGlobalFunction(ctx, "__solarLayoutProcessedText", ProcessedText, 1);
  qe::DefineGlobalFunction(ctx, "__solarLayoutSetCvWake", SetCvWake, 1);
  qe::DefineGlobalFunction(ctx, "__solarLayoutWantUpdates", WantUpdates, 1);
  qe::DefineGlobalFunction(ctx, "__solarLayoutBoxSizes", BoxSizes, 1);
  qe::DefineGlobalFunction(ctx, "__solarLayoutCvTake", CvTake, 0);
}

}  // namespace solar::layout
