#include "solar/layout/Layout.h"

#include <cmath>
#include <sstream>

#include "Internal.h"
#include "solar/css/Logical.h"
#include "solar/css/MediaQuery.h"
#include "solar/css/Style.h"

namespace solar::layout {

Tree* UpdateLayout(Quanta::Context& ctx, dom::Document* document) {
  const css::MediaEnvironment environment = css::EnvironmentFor(document);
  const uint64_t version = (dom::TreeVersion() * 1000003 + css::StyleVersion()) * 1000003 + static_cast<uint64_t>(environment.width * 7 + environment.height);
  if (document->layoutTree && document->layoutTree->builtFor == version && document->layoutTree->root) return document->layoutTree.get();
  auto tree = std::make_shared<Tree>();
  tree->viewportWidth = environment.width;
  tree->viewportHeight = environment.height;
  LayoutContext lc{ctx, *tree, environment.width, environment.height};
  BuildTree(lc, document);
  LayoutRoot(lc);
  tree->builtFor = version;
  document->layoutTree = tree;
  return tree.get();
}

const std::vector<Box*>& BoxesOf(Tree& tree, const dom::Node* node) {
  static const std::vector<Box*> none;
  const auto found = tree.boxesOf.find(node);
  return found == tree.boxesOf.end() ? none : found->second;
}

namespace {

// The box whose border box the coordinates of this box's x and y are in.
const Box* CoordinateParent(const Box& box) {
  const Box* p = box.parent;
  if (box.inlineLevel) {
    while (p && p->kind != Box::Kind::Block) p = p->parent;
  }
  return p;
}

}  // namespace

Rect AbsoluteBorderBox(const Box& box) {
  double x = box.x, y = box.y;
  for (const Box* p = CoordinateParent(box); p; p = CoordinateParent(*p)) {
    x += p->x;
    y += p->y;
  }
  return {x, y, box.width, box.height};
}

namespace {

Rect Origin(const Box& block) {
  Rect r = AbsoluteBorderBox(block);
  return r;
}

}  // namespace

std::vector<Rect> ClientRects(Tree& tree, dom::Element* element) {
  std::vector<Rect> out;
  for (Box* box : BoxesOf(tree, element)) {
    if (box->kind == Box::Kind::Inline || box->kind == Box::Kind::Text) {
      const Box* container = box->parent;
      while (container && container->kind != Box::Kind::Block) container = container->parent;
      if (!container) continue;
      const Rect origin = Origin(*container);
      for (const Rect& f : box->fragments) out.push_back({origin.x + f.x, origin.y + f.y, f.width, f.height});
    } else if (box->kind == Box::Kind::LineBreak) {
      continue;
    } else {
      out.push_back(AbsoluteBorderBox(*box));
    }
  }
  return out;
}

namespace {

void Dump(const Box& box, int depth, std::ostringstream& out) {
  out << std::string(depth * 2, ' ');
  const char* kind = box.kind == Box::Kind::Block ? (box.inlineLevel ? "atomic" : "block") : box.kind == Box::Kind::Inline ? "inline" : box.kind == Box::Kind::Text ? "text" : "br";
  out << kind;
  if (box.node && box.node->IsElement()) out << " <" << static_cast<const dom::Element*>(box.node)->localName << ">";
  if (box.anonymous) out << " (anonymous)";
  if (box.kind == Box::Kind::Text) {
    out << " \"" << box.processed << "\"";
  } else if (box.kind != Box::Kind::Inline) {
    out << " [" << box.x << "," << box.y << " " << box.width << "x" << box.height << "]";
  }
  if (box.kind == Box::Kind::Inline) {
    for (const Rect& r : box.fragments) out << " {" << r.x << "," << r.y << " " << r.width << "x" << r.height << "}";
  }
  out << "\n";
  for (const Line& line : box.lines) {
    out << std::string(depth * 2 + 2, ' ') << "line [" << line.rect.x << "," << line.rect.y << " " << line.rect.width << "x" << line.rect.height << "] baseline " << line.baseline << "\n";
  }
  for (const auto& c : box.children) Dump(*c, depth + 1, out);
}

}  // namespace

std::string DumpTree(const Tree& tree) {
  std::ostringstream out;
  if (tree.root) Dump(*tree.root, 0, out);
  return out.str();
}

namespace {

std::string Px(double v) {
  if (std::fabs(v) < 1e-9) return "0px";
  char buffer[64];
  std::snprintf(buffer, sizeof buffer, "%.6g", v);
  return std::string(buffer) + "px";
}

}  // namespace

bool UsedValue(Tree& tree, dom::Element* element, const std::string& property, std::string& out) {
  const std::vector<Box*>& boxes = BoxesOf(tree, element);
  if (boxes.empty()) return false;
  Box* box = nullptr;
  for (Box* b : boxes) if (b->kind == Box::Kind::Block) { box = b; break; }
  if (!box) box = boxes[0];
  const bool isInline = box->kind != Box::Kind::Block;  // a non-replaced inline: width and height are not used values
  static const char* const sides[4] = {"top", "right", "bottom", "left"};
  for (int i = 0; i < 4; ++i) {
    const std::string side = sides[i];
    if (property == "margin-" + side) {
      const double v = i == 0 ? box->margin.top : i == 1 ? box->margin.right : i == 2 ? box->margin.bottom : box->margin.left;
      out = Px(v);
      return true;
    }
    if (property == "padding-" + side) {
      const double v = i == 0 ? box->padding.top : i == 1 ? box->padding.right : i == 2 ? box->padding.bottom : box->padding.left;
      out = Px(v);
      return true;
    }
    if (property == side && (box->style->position == Position::Relative || box->style->position == Position::Sticky)) {
      const bool horizontal = i == 1 || i == 3;
      const double shift = horizontal ? box->shiftX : box->shiftY;
      const bool first = i == 0 || i == 3;  // top and left
      out = Px(first ? shift : -shift);
      return true;
    }
  }
  if (property == "min-width" || property == "min-height") {
    const Length& l = property == "min-width" ? box->style->minWidth : box->style->minHeight;
    // (In a flex or grid container an auto minimum size is the content-based one, which stays auto.)
    const Box* parent = box->parent;
    const bool item = parent && parent->style && (parent->style->display == Display::Flex || parent->style->display == Display::InlineFlex ||
                                                  parent->style->display == Display::Grid || parent->style->display == Display::InlineGrid);
    // (In a flex or grid container an auto minimum size is the content-based one, which stays auto.)
    if (l.IsAuto() && !item) { out = "0px"; return true; }
    return false;
  }
  if (isInline) return false;
  if (property == "width") {
    out = Px(box->style->boxSizing == BoxSizing::BorderBox ? box->width : box->ContentWidth());
    return true;
  }
  if (property == "height") {
    out = Px(box->style->boxSizing == BoxSizing::BorderBox ? box->height : box->ContentHeight());
    return true;
  }
  return false;
}

bool ResolvedHook(Quanta::Context& ctx, dom::Element* element, const std::string& given, const std::string&, std::string& out) {
  if (!element->nodeDocument || !dom::ShadowIncludingRoot(element)->IsDocument()) return false;
  std::string property = given;
  // A logical property is the physical one of the element's writing mode and direction.
  const std::string physical = css::PhysicalOf(given, {css::ComputedValue(ctx, element, "writing-mode"), css::ComputedValue(ctx, element, "direction")});
  if (!physical.empty()) property = physical;
  static const char* const names[] = {"width", "height", "min-width", "min-height", "margin-top", "margin-right", "margin-bottom", "margin-left", "padding-top",
                                      "padding-right", "padding-bottom", "padding-left", "top", "right", "bottom", "left"};
  bool relevant = false;
  for (const char* n : names) if (property == n) relevant = true;
  if (!relevant) return false;
  Tree* tree = UpdateLayout(ctx, element->nodeDocument);
  return tree && UsedValue(*tree, element, property, out);
}

void InstallResolvedHook() { css::SetResolvedValueHook(ResolvedHook); }

}  // namespace solar::layout
