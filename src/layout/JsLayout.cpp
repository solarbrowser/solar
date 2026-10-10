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
  const bool isRoot = element->nodeDocument->DocumentElement() == element;
  const bool inlineBox = box->kind != Box::Kind::Block;
  double clientWidth = inlineBox ? 0 : box->width - box->border.Horizontal();
  double clientHeight = inlineBox ? 0 : box->height - box->border.Vertical();
  if (isRoot) {
    clientWidth = tree->viewportWidth;
    clientHeight = tree->viewportHeight;
  }
  double right = 0, bottom = 0;
  bool any = false;
  if (!inlineBox) Extent(*box, abs.x, abs.y, right, bottom, any);
  const double scrollWidth = std::max(clientWidth, right - box->border.left);
  const double scrollHeight = std::max(clientHeight, bottom - box->border.top);
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

// The element offsetTop and the others are relative to, or null.
Value OffsetParent(Context& ctx, Value, qe::Args args, Value) {
  dom::Element* element = ElementArg(args);
  Tree* tree = TreeFor(ctx, element);
  Box* box = tree ? PrincipalBox(*tree, element) : nullptr;
  if (!box) return qe::Null();
  if (box->style->position == Position::Fixed) return qe::Null();
  dom::Document* document = element->nodeDocument;
  if (document->DocumentElement() == element) return qe::Null();
  for (dom::Node* n = element->parentNode; n; n = n->parentNode) {
    if (!n->IsElement()) continue;
    dom::Element* e = static_cast<dom::Element*>(n);
    if (e->IsHtml("body")) return dom::NodeValue(e);
    Box* b = PrincipalBox(*tree, e);
    if (!b) continue;
    if (b->style->position != Position::Static) return dom::NodeValue(e);
    if (e->IsHtml("td") || e->IsHtml("th") || e->IsHtml("table")) return dom::NodeValue(e);
  }
  return qe::Null();
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
  qe::DefineGlobalFunction(ctx, "__solarLayoutDump", Dump, 0);
}

}  // namespace solar::layout
