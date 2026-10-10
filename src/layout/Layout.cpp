#include "solar/layout/Layout.h"

#include <cmath>
#include <algorithm>
#include <functional>
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
  if (document->layoutTree) {
    for (const auto& entry : document->layoutTree->scroll) {
      if (entry.first == document || tree->boxesOf.count(entry.first)) tree->scroll[entry.first] = entry.second;
    }
  }
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

namespace {

// The transforms of a box and the boxes around it, as one matrix in document coordinates; false if none of them is transformed.
bool CumulativeTransform(const Box& box, Matrix& out) {
  std::vector<const Box*> chain;
  for (const Box* b = &box; b; b = CoordinateParent(*b)) chain.push_back(b);
  std::reverse(chain.begin(), chain.end());
  Matrix total;
  bool any = false;
  for (size_t i = 0; i < chain.size(); ++i) {
    const Box& b = *chain[i];
    if (b.kind != Box::Kind::Block || !b.style) continue;
    const Box* parent = i > 0 ? chain[i - 1] : nullptr;
    const Rect abs = AbsoluteBorderBox(b);
    Matrix own;
    const bool transformed = TransformOf(b, own);
    // The parent's perspective acts on this box.
    Matrix perspective;
    bool hasPerspective = false;
    if (parent && parent->style && parent->style->perspective != "none") {
      Matrix p;
      if (ParseTransformList("perspective(" + parent->style->perspective + ")", 0, 0, p)) {
        const Rect pabs = AbsoluteBorderBox(*parent);
        double ox = parent->width / 2, oy = parent->height / 2;
        const std::vector<std::string> parts = [&] {
          std::vector<std::string> v;
          std::string word;
          for (char c : parent->style->perspectiveOrigin + " ") { if (c == ' ') { if (!word.empty()) v.push_back(word); word.clear(); } else word += c; }
          return v;
        }();
        const auto px = [&](const std::string& t, double basis, double& out2) {
          char* end = nullptr;
          const double v = std::strtod(t.c_str(), &end);
          if (end == t.c_str()) return;
          out2 = std::string(end) == "%" ? basis * v / 100 : v;
        };
        if (parts.size() >= 2) { px(parts[0], parent->width, ox); px(parts[1], parent->height, oy); }
        Matrix to, back;
        to.m[12] = pabs.x + ox; to.m[13] = pabs.y + oy;
        back.m[12] = -(pabs.x + ox); back.m[13] = -(pabs.y + oy);
        perspective = to * p * back;
        hasPerspective = true;
      }
    }
    if (!transformed && !hasPerspective) continue;
    if (parent && parent->style && !parent->style->preserve3d) {
      // Not preserving 3D: what came before is flattened into the plane.
      total.m[2] = total.m[6] = total.m[8] = total.m[9] = total.m[11] = total.m[14] = 0;
      total.m[10] = 1;
    }
    if (hasPerspective) total = total * perspective;
    if (transformed) {
      Matrix to, back;
      to.m[12] = abs.x; to.m[13] = abs.y;
      back.m[12] = -abs.x; back.m[13] = -abs.y;
      total = total * to * own * back;
    }
    any = true;
  }
  out = total;
  return any;
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
    } else if (box->fragments.size() > 1) {
      // A block cut into columns has a piece in each.
      const Rect origin = AbsoluteBorderBox(*box);
      for (const Rect& f : box->fragments) out.push_back({origin.x + f.x, origin.y + f.y, f.width, f.height});
    } else {
      out.push_back(AbsoluteBorderBox(*box));
    }
    // Scrolled ancestors move what is in them.
    double dx = 0, dy = 0;
    for (const Box* p = box->parent; p; p = p->parent) {
      if (!p->node) continue;
      const auto found = tree.scroll.find(p->node);
      if (found != tree.scroll.end()) { dx += found->second.first; dy += found->second.second; }
    }
    const size_t added = box->kind == Box::Kind::Inline || box->kind == Box::Kind::Text || box->fragments.size() > 1 ? box->fragments.size() : 1;
    Matrix matrix;
    const Box* holder = box;
    if (box->kind != Box::Kind::Block) {
      holder = box->parent;
      while (holder && holder->kind != Box::Kind::Block) holder = holder->parent;
    }
    if (holder && CumulativeTransform(*holder, matrix)) {
      for (size_t i = out.size() - added; i < out.size(); ++i) {
        double xs[4], ys[4];
        matrix.Map(out[i].x, out[i].y, xs[0], ys[0]);
        matrix.Map(out[i].x + out[i].width, out[i].y, xs[1], ys[1]);
        matrix.Map(out[i].x, out[i].y + out[i].height, xs[2], ys[2]);
        matrix.Map(out[i].x + out[i].width, out[i].y + out[i].height, xs[3], ys[3]);
        double l = xs[0], r = xs[0], t = ys[0], b = ys[0];
        for (int k = 1; k < 4; ++k) { l = std::min(l, xs[k]); r = std::max(r, xs[k]); t = std::min(t, ys[k]); b = std::max(b, ys[k]); }
        out[i] = {l, t, r - l, b - t};
      }
    }
    if (dx != 0 || dy != 0) {
      for (size_t i = out.size() - added; i < out.size(); ++i) {
        out[i].x -= dx;
        out[i].y -= dy;
      }
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

namespace {

std::string Num(double v) {
  if (std::fabs(v) < 5e-7) return "0";
  char buffer[64];
  std::snprintf(buffer, sizeof buffer, "%.6g", v);
  return buffer;
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
  if (property == "transform") {
    if (isInline || box->style->transform == "none") return false;
    Matrix m;
    if (!ParseTransformList(box->style->transform, box->width, box->height, m)) return false;
    if (m.Is2d()) out = "matrix(" + Num(m.m[0]) + ", " + Num(m.m[1]) + ", " + Num(m.m[4]) + ", " + Num(m.m[5]) + ", " + Num(m.m[12]) + ", " + Num(m.m[13]) + ")";
    else {
      out = "matrix3d(";
      for (int i = 0; i < 16; ++i) out += (i ? ", " : "") + Num(m.m[i]);
      out += ")";
    }
    return true;
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
                                      "padding-right", "padding-bottom", "padding-left", "top", "right", "bottom", "left", "transform"};
  bool relevant = false;
  for (const char* n : names) if (property == n) relevant = true;
  if (!relevant) return false;
  Tree* tree = UpdateLayout(ctx, element->nodeDocument);
  return tree && UsedValue(*tree, element, property, out);
}

void InstallResolvedHook() { css::SetResolvedValueHook(ResolvedHook); }

}  // namespace solar::layout

namespace solar::layout {

namespace {

bool Contains(const Rect& r, double x, double y) { return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height; }

bool IsPositionedBox(const Box& b) { return b.style && b.style->position != Position::Static; }

// z-index as a number; auto is 0 for ordering.
int ZOf(const Box& b) {
  const std::string& z = b.style->zIndex;
  if (z == "auto" || z.empty()) return 0;
  return std::atoi(z.c_str());
}

// Boxes in the order they are painted, bottom first, for the stacking context (or pseudo one) rooted at `root`.
void PaintOrder(const Box& root, std::vector<const Box*>& out) {
  std::vector<std::pair<int, const Box*>> negative, positive;
  std::vector<const Box*> zero, blocks, floats, inlines;
  std::vector<const Box*> stack;
  const std::function<void(const Box&)> walk = [&](const Box& b) {
    for (const auto& childPtr : b.children) {
      const Box& c = *childPtr;
      const bool context = c.style->stackingContext;
      if (IsPositionedBox(c) || context) {
        const int z = ZOf(c);
        if (z < 0 && context) negative.push_back({z, &c});
        else if (z > 0 && context) positive.push_back({z, &c});
        else zero.push_back(&c);
        continue;
      }
      if (c.style->IsFloating()) { floats.push_back(&c); continue; }
      if (c.kind == Box::Kind::Block && c.inlineLevel) { inlines.push_back(&c); continue; }  // an atomic inline paints as a whole
      if (c.kind == Box::Kind::Block) blocks.push_back(&c);
      else inlines.push_back(&c);
      walk(c);
    }
  };
  out.push_back(&root);
  walk(root);
  std::stable_sort(negative.begin(), negative.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  std::stable_sort(positive.begin(), positive.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  for (const auto& n : negative) PaintOrder(*n.second, out);
  for (const Box* b : blocks) out.push_back(b);
  for (const Box* f : floats) PaintOrder(*f, out);
  for (const Box* i : inlines) {
    if (i->kind == Box::Kind::Block) PaintOrder(*i, out);
    else out.push_back(i);
  }
  for (const Box* z : zero) PaintOrder(*z, out);
  for (const auto& p : positive) PaintOrder(*p.second, out);
}

// Whether a point is inside every ancestor's clip.
bool Clipped(Tree& tree, const Box& box, double x, double y) {
  for (const Box* p = box.parent; p; p = p->parent) {
    if (!p->style || p->kind != Box::Kind::Block) continue;
    if (p->style->Physical().overflowX == Overflow::Visible && p->style->Physical().overflowY == Overflow::Visible) continue;
    Rect r = AbsoluteBorderBox(*p);
    double dx = 0, dy = 0;
    for (const Box* q = p->parent; q; q = q->parent) {
      if (!q->node) continue;
      const auto found = tree.scroll.find(q->node);
      if (found != tree.scroll.end()) { dx += found->second.first; dy += found->second.second; }
    }
    r.x += p->border.left - dx;
    r.y += p->border.top - dy;
    r.width -= p->border.Horizontal();
    r.height -= p->border.Vertical();
    if (p->style->Physical().overflowX != Overflow::Visible && (x < r.x || x >= r.x + r.width)) return true;
    if (p->style->Physical().overflowY != Overflow::Visible && (y < r.y || y >= r.y + r.height)) return true;
  }
  return false;
}

}  // namespace

std::vector<dom::Element*> ElementsAtPoint(Tree& tree, double x, double y) {
  std::vector<dom::Element*> hits;
  if (x < 0 || y < 0 || x >= tree.viewportWidth || y >= tree.viewportHeight || !tree.root) return hits;
  std::vector<const Box*> order;
  PaintOrder(*tree.root, order);
  const auto add = [&](dom::Node* node) {
    // (a text node directly in a shadow root is stood for by the host)
    while (node && !node->IsElement()) {
      dom::Node* up = node->parentNode;
      if (up && up->IsFragment() && static_cast<dom::DocumentFragment*>(up)->isShadowRoot) up = static_cast<dom::DocumentFragment*>(up)->host;
      node = up;
    }
    if (!node) return;
    dom::Element* element = static_cast<dom::Element*>(node);
    if (std::find(hits.begin(), hits.end(), element) == hits.end()) hits.push_back(element);
  };
  for (size_t i = order.size(); i-- > 0;) {
    const Box& b = *order[i];
    if (b.style->visibility != Visibility::Visible || b.style->pointerEventsNone) continue;
    bool skipped = false;
    for (const Box* p = b.parent; p; p = p->parent) if (p->style && p->style->skipContents) skipped = true;
    if (skipped) continue;
    bool inside = false;
    if (b.kind == Box::Kind::Block) {
      Rect r = AbsoluteBorderBox(b);
      double dx = 0, dy = 0;
      for (const Box* q = b.parent; q; q = q->parent) {
        if (!q->node) continue;
        const auto found = tree.scroll.find(q->node);
        if (found != tree.scroll.end()) { dx += found->second.first; dy += found->second.second; }
      }
      r.x -= dx;
      r.y -= dy;
      inside = Contains(r, x, y);
    } else if (b.kind == Box::Kind::Inline || b.kind == Box::Kind::Text) {
      const Box* container = b.parent;
      while (container && container->kind != Box::Kind::Block) container = container->parent;
      if (!container) continue;
      const Rect origin = AbsoluteBorderBox(*container);
      double dx = 0, dy = 0;
      for (const Box* q = container; q; q = q->parent) {
        if (!q->node) continue;
        const auto found = tree.scroll.find(q->node);
        if (found != tree.scroll.end() && q != container) { dx += found->second.first; dy += found->second.second; }
        else if (found != tree.scroll.end()) { dx += found->second.first; dy += found->second.second; }
      }
      for (const Rect& f : b.fragments) {
        if (Contains({origin.x + f.x - dx, origin.y + f.y - dy, f.width, f.height}, x, y)) inside = true;
      }
    }
    if (!inside || Clipped(tree, b, x, y)) continue;
    const Box* owner = &b;
    while (owner && !owner->node) owner = owner->parent;
    if (owner && owner->node && owner->node->IsDocument()) continue;
    if (owner) add(owner->node);
  }
  // Over the canvas, the root element is what is there.
  if (tree.root && !tree.root->children.empty() && tree.root->node && tree.root->node->IsDocument()) {
    dom::Element* html = static_cast<dom::Document*>(tree.root->node)->DocumentElement();
    if (html && std::find(hits.begin(), hits.end(), html) == hits.end()) hits.push_back(html);
  }
  return hits;
}

namespace {

void FarthestExtent(const Box& box, double ox, double oy, double& right, double& bottom, bool& any) {
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
  for (const auto& c : box.children) FarthestExtent(*c, ox, oy, right, bottom, any);
}

}  // namespace

void ScrollRange(Tree& tree, dom::Element* element, double& maxX, double& maxY) {
  maxX = maxY = 0;
  if (!tree.root) return;
  dom::Document* document = element ? element->nodeDocument : (tree.root->node && tree.root->node->IsDocument() ? static_cast<dom::Document*>(tree.root->node) : nullptr);
  if (!document) return;
  if (!element || element == document->DocumentElement()) {
    double right = 0, bottom = 0;
    bool any = false;
    FarthestExtent(*tree.root, 0, 0, right, bottom, any);
    maxX = std::max(0.0, right - tree.viewportWidth);
    maxY = std::max(0.0, bottom - tree.viewportHeight);
    return;
  }
  const std::vector<Box*>& boxes = BoxesOf(tree, element);
  const Box* box = nullptr;
  for (const Box* b : boxes) if (b->kind == Box::Kind::Block) { box = b; break; }
  if (!box || (box->style->Physical().overflowX == Overflow::Visible && box->style->Physical().overflowY == Overflow::Visible) || box->style->Physical().overflowX == Overflow::Clip) return;
  const Rect abs = AbsoluteBorderBox(*box);
  double right = 0, bottom = 0;
  bool any = false;
  FarthestExtent(*box, abs.x, abs.y, right, bottom, any);
  const double clientW = box->width - box->border.Horizontal(), clientH = box->height - box->border.Vertical();
  maxX = std::max(0.0, right - box->border.left - clientW);
  maxY = std::max(0.0, bottom - box->border.top - clientH);
  if (box->style->Physical().overflowX == Overflow::Visible) maxX = 0;
  if (box->style->Physical().overflowY == Overflow::Visible) maxY = 0;
}

}  // namespace solar::layout
