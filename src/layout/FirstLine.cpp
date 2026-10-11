// ::first-line (https://www.w3.org/TR/css-pseudo-4/#first-line-pseudo): the first formatted line of a block container is styled by the pseudo-element's
// rules. The font of the line decides where it breaks, so it is laid out with the style first, and then the boxes that run over the end of it are split
// where it broke: the pieces on the line keep the style, the rest are as they were. The split is undone before the block is laid out again.
#include <algorithm>
#include <functional>

#include "Internal.h"
#include "solar/css/Selectors.h"
#include "solar/css/Style.h"

namespace solar::layout {

namespace {

bool BlockContainer(const Box& b) {
  const Display d = b.style->display;
  return d == Display::Block || d == Display::ListItem || d == Display::FlowRoot || d == Display::InlineBlock;
}

// A style with what the ::first-line gives to the text that inherits it: what the box has from its container unchanged is the line's instead.
std::shared_ptr<const BoxStyle> Variant(const BoxStyle& box, const BoxStyle& container, const BoxStyle& line) {
  const auto apply = [&](BoxStyle& s, const BoxStyle& b, const BoxStyle& c, const BoxStyle& l) {
    if (b.fontSize == c.fontSize) s.fontSize = l.fontSize;
    if (b.fontWeight == c.fontWeight) s.fontWeight = l.fontWeight;
    if (b.italic == c.italic) s.italic = l.italic;
    if (b.fontStretch == c.fontStretch) s.fontStretch = l.fontStretch;
    if (b.fontFamily == c.fontFamily) s.fontFamily = l.fontFamily;
    if (b.lineHeight.kind == c.lineHeight.kind && b.lineHeight.value == c.lineHeight.value) {
      s.lineHeight = l.lineHeight;
      s.lineHeightNumber = l.lineHeightNumber;
    }
    if (b.letterSpacing == c.letterSpacing) s.letterSpacing = l.letterSpacing;
    if (b.wordSpacing == c.wordSpacing) s.wordSpacing = l.wordSpacing;
    if (b.textTransform == c.textTransform) s.textTransform = l.textTransform;
    if (b.color == c.color) s.color = l.color;
  };
  auto style = std::make_shared<BoxStyle>(box);
  apply(*style, box, container, line);
  if (box.physicalStyle) {
    auto physical = std::make_shared<BoxStyle>(*box.physicalStyle);
    apply(*physical, *box.physicalStyle, container.Physical(), line.Physical());
    style->physicalStyle = physical;
  }
  return style;
}

// The inline-level boxes of the container's inline formatting context in tree order: text and inline boxes, and the atomic ones to stop at.
void Visit(Box& parent, const std::function<bool(Box&)>& f, bool& stop) {
  for (auto& c : parent.children) {
    if (stop) return;
    if (c->IsOutOfFlow() || c->outsideMarker) continue;
    if (c->kind == Box::Kind::Text || c->kind == Box::Kind::Inline || (c->kind == Box::Kind::Block && c->inlineLevel)) {
      if (!f(*c)) {
        stop = true;
        return;
      }
      if (c->kind == Box::Kind::Inline) Visit(*c, f, stop);
    }
  }
}

void Undo(LayoutContext& lc, Box& container) {
  for (size_t i = container.firstLineEdits.size(); i-- > 0;) {
    FirstLineEdit& e = container.firstLineEdits[i];
    if (e.tail) {
      Box* head = e.box;
      if (head->kind == Box::Kind::Text) {
        head->text = e.text;
      } else {
        for (auto& c : e.tail->children) {
          c->parent = head;
          head->children.push_back(std::move(c));
        }
        e.tail->children.clear();
      }
      auto& siblings = e.tail->parent->children;
      for (size_t k = 0; k < siblings.size(); ++k) {
        if (siblings[k].get() != e.tail) continue;
        siblings.erase(siblings.begin() + static_cast<std::ptrdiff_t>(k));
        break;
      }
      if (head->node) {
        auto& boxes = lc.tree.boxesOf[head->node];
        boxes.erase(std::remove(boxes.begin(), boxes.end(), e.tail), boxes.end());
      }
    } else {
      e.box->style = e.style;
    }
  }
  container.firstLineEdits.clear();
}

// The edge of an inline box that a split took away: the sides where it was cut have no border, padding or margin (box-decoration-break: slice).
std::shared_ptr<const BoxStyle> Sliced(const BoxStyle& style, bool head) {
  const auto cut = [&](BoxStyle& s) {
    const int side = head ? 1 : 3;
    s.border[side] = 0;
    s.padding[side] = Length::Px(0);
    s.margin[side] = Length::Px(0);
  };
  auto copy = std::make_shared<BoxStyle>(style);
  cut(*copy);
  if (style.physicalStyle) {
    auto physical = std::make_shared<BoxStyle>(*style.physicalStyle);
    cut(*physical);
    copy->physicalStyle = physical;
  }
  return copy;
}

size_t IndexOf(Box& parent, const Box* child) {
  for (size_t i = 0; i < parent.children.size(); ++i) if (parent.children[i].get() == child) return i;
  return parent.children.size();
}

void Register(LayoutContext& lc, Box& head, Box& tail) {
  if (!head.node) return;
  auto& boxes = lc.tree.boxesOf[head.node];
  auto at = std::find(boxes.begin(), boxes.end(), &head);
  boxes.insert(at == boxes.end() ? boxes.end() : at + 1, &tail);
}

// Splits the inline formatting context of the container after `leaf` (a text box, after `offset` bytes of its text, or an atomic inline).
void Split(LayoutContext& lc, Box& container, Box& leaf, size_t offset) {
  std::unique_ptr<Box> tail;
  if (leaf.kind == Box::Kind::Text && offset < leaf.text.size()) {
    tail = std::make_unique<Box>();
    tail->kind = Box::Kind::Text;
    tail->inlineLevel = true;
    tail->node = leaf.node;
    tail->style = leaf.style;
    tail->text = leaf.text.substr(offset);
    tail->textOffset = leaf.textOffset + offset;
    FirstLineEdit edit;
    edit.box = &leaf;
    edit.text = leaf.text;
    edit.tail = tail.get();
    leaf.text.resize(offset);
    container.firstLineEdits.push_back(edit);
  }
  Box* cur = &leaf;
  while (true) {
    Box* parent = cur->parent;
    const size_t index = IndexOf(*parent, cur);
    if (parent == &container) {
      if (tail) {
        tail->parent = parent;
        Register(lc, *cur, *tail);
        parent->children.insert(parent->children.begin() + static_cast<std::ptrdiff_t>(index + 1), std::move(tail));
      }
      break;
    }
    // The inline box the cut is in: what comes after the cut goes to a box of the same element after it.
    auto clone = std::make_unique<Box>();
    clone->kind = parent->kind;
    clone->inlineLevel = parent->inlineLevel;
    clone->node = parent->node;
    clone->pseudo = parent->pseudo;
    clone->anonymous = parent->anonymous;
    clone->style = Sliced(*parent->style, false);
    if (tail) {
      tail->parent = clone.get();
      Register(lc, *cur, *tail);
      clone->children.push_back(std::move(tail));
    }
    size_t moved = 0;
    for (size_t k = index + 1; k < parent->children.size(); ++k) {
      parent->children[k]->parent = clone.get();
      clone->children.push_back(std::move(parent->children[k]));
      ++moved;
    }
    parent->children.resize(index + 1);
    FirstLineEdit edit;
    edit.box = parent;
    edit.tail = clone.get();
    edit.moved = moved;
    edit.style = parent->style;
    // (The style of the head is cut at the end; the edit that restores it comes with the variants.)
    container.firstLineEdits.push_back(edit);
    parent->style = Sliced(*parent->style, true);
    FirstLineEdit styleEdit;
    styleEdit.box = parent;
    styleEdit.style = edit.style;
    container.firstLineEdits.push_back(styleEdit);
    Box* head = parent;
    Box* raw = clone.get();
    raw->parent = head->parent;
    // The clone goes next to the head in its parent, which is the next level up.
    tail = std::move(clone);
    cur = head;
    (void)raw;
  }
}

}  // namespace

double LayoutInlineContent(LayoutContext& lc, Box& container, double width, double& baselineOut) {
  if (!container.firstLineStyle) return LayoutInlineContentOnce(lc, container, width, baselineOut);
  Undo(lc, container);
  const BoxStyle& line = *container.firstLineStyle;
  const auto applyVariants = [&](Box* stopAfter) {
    bool stop = false;
    Visit(container, [&](Box& b) {
      if (b.kind == Box::Kind::Text || b.kind == Box::Kind::Inline) {
        FirstLineEdit edit;
        edit.box = &b;
        edit.style = b.style;
        container.firstLineEdits.push_back(edit);
        b.style = Variant(*b.style, *container.style, line);
      }
      return &b != stopAfter;
    }, stop);
  };
  // First as if the whole of the content were on the line: where it breaks tells where the line ends.
  applyVariants(nullptr);
  Bfc scratch = *lc.bfc;
  Bfc* const real = lc.bfc;
  lc.bfc = &scratch;
  const double probeHeight = LayoutInlineContentOnce(lc, container, width, baselineOut);
  lc.bfc = real;
  if (container.lines.size() < 2) return LayoutInlineContentOnce(lc, container, width, baselineOut);
  (void)probeHeight;
  // The last thing on the first line.
  const LineItem* last = nullptr;
  for (const LineItem& item : container.lines[0].items) {
    if (item.kind == LineItem::Kind::Text || item.kind == LineItem::Kind::Atomic) last = &item;
  }
  if (!last || !last->box) return LayoutInlineContentOnce(lc, container, width, baselineOut);
  Box* leaf = last->box;
  size_t offset = 0;
  if (leaf->kind == Box::Kind::Text) {
    const size_t processedEnd = last->text.end;
    offset = processedEnd < leaf->processedSource.size() ? leaf->processedSource[processedEnd] : leaf->text.size();
  }
  Undo(lc, container);
  Split(lc, container, *leaf, offset);
  applyVariants(leaf);
  return LayoutInlineContentOnce(lc, container, width, baselineOut);
}

void ApplyFirstLines(LayoutContext& lc, Box& root) {
  if (!solar::css::g_firstLineSeen) return;
  const std::function<void(Box&)> walk = [&](Box& box) {
    for (auto& c : box.children) if (c->kind == Box::Kind::Block) walk(*c);
    dom::Element* element = box.element();
    if (!element || box.kind != Box::Kind::Block || box.replaced || box.formControl || !BlockContainer(box)) return;
    if (!solar::css::HasPseudoStyle(element, "first-line")) return;
    std::shared_ptr<const BoxStyle> style = Logicalize(ReadStyle(lc.ctx, element, "first-line"));
    // The first line of the block is that of its first block child, if it starts with one.
    Box* target = &box;
    while (target && !target->hasInlineContent) {
      Box* next = nullptr;
      for (auto& c : target->children) {
        if (c->IsOutOfFlow() || c->outsideMarker) continue;
        if (c->kind == Box::Kind::Block && !c->inlineLevel && BlockContainer(*c) && !c->replaced) next = c.get();
        break;
      }
      target = next;
    }
    if (target && !target->firstLineStyle) target->firstLineStyle = style;
  };
  walk(root);
}

}  // namespace solar::layout
