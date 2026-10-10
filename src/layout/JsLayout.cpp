// The natives under the geometry APIs of elements (getBoundingClientRect, offsetWidth, clientWidth...), which are written in script over them.
#include <cmath>
#include <iostream>

#include "Internal.h"
#include "solar/css/Style.h"
#include "solar/dom/NodeBindingsInternal.h"

namespace solar::layout {

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
  double clientWidth = inlineBox ? 0 : box->width - box->border.Horizontal();
  double clientHeight = inlineBox ? 0 : box->height - box->border.Vertical();
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
  push(inlineBox ? 0 : box->border.top);
  push(inlineBox ? 0 : box->border.left);
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
  if (args.size() >= 4 && qe::ToBoolean(args[1])) ApplySticky(*tree);
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

}  // namespace

void InstallResolvedHook();

void InstallLayoutNatives(Context& ctx) {
  InstallResolvedHook();
  qe::DefineGlobalFunction(ctx, "__solarLayoutMetrics", Metrics, 1);
  qe::DefineGlobalFunction(ctx, "__solarLayoutRects", ClientRectsNative, 1);
  qe::DefineGlobalFunction(ctx, "__solarLayoutOffsetParent", OffsetParent, 1);
  qe::DefineGlobalFunction(ctx, "__solarLayoutScroll", Scroll, 4);
  qe::DefineGlobalFunction(ctx, "__solarLayoutElementsAt", ElementsAt, 2);
  qe::DefineGlobalFunction(ctx, "__solarLayoutDump", Dump, 0);
}

}  // namespace solar::layout
