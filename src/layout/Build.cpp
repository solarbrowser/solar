// The box tree: boxes for elements and text (https://www.w3.org/TR/CSS22/visuren.html#box-gen), with the anonymous boxes the model needs.
#include <cstdlib>

#include "Internal.h"
#include "solar/css/Style.h"

namespace solar::layout {

std::shared_ptr<const BoxStyle> AnonymousStyle(const BoxStyle& parent, Display display) {
  auto style = std::make_shared<BoxStyle>(parent);
  BoxStyle& s = *style;
  s.display = display;
  s.position = Position::Static;
  s.floating = Float::None;
  s.clear = Clear::None;
  s.boxSizing = BoxSizing::ContentBox;
  s.overflowX = s.overflowY = Overflow::Visible;
  s.width = s.height = s.minWidth = s.minHeight = Length();
  s.maxWidth = s.maxHeight = Length(Length::Kind::None);
  for (int i = 0; i < 4; ++i) {
    s.margin[i] = s.padding[i] = Length::Px(0);
    s.inset[i] = Length();
    s.border[i] = 0;
    s.borderStyle[i] = BorderStyle::None;
  }
  s.aspectRatio = 0;
  s.verticalAlign = VerticalAlign::Baseline;
  s.backgroundColor = "rgba(0, 0, 0, 0)";
  s.zIndex = "auto";
  return style;
}

namespace {

bool IsHtmlSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }

bool OnlyCollapsibleSpace(const Box& text) {
  if (text.style->whiteSpaceCollapse != WhiteSpaceCollapse::Collapse && text.style->whiteSpaceCollapse != WhiteSpaceCollapse::PreserveBreaks) return false;
  for (char c : text.text) {
    if (!IsHtmlSpace(c)) return false;
    if (c == '\n' && text.style->whiteSpaceCollapse == WhiteSpaceCollapse::PreserveBreaks) return false;
  }
  return true;
}

// The natural size of the elements that are replaced by something that is not in the document.
bool ReplacedSize(dom::Element* element, double& width, double& height) {
  const std::string& name = element->localName;
  if (!element->IsHtml()) return false;
  const auto attribute = [&](const char* attr) -> double {
    const dom::Attr* a = element->FindAttribute(attr);
    if (!a) return -1;
    char* end = nullptr;
    const double v = std::strtod(a->value.c_str(), &end);
    return end == a->value.c_str() || v < 0 ? -1 : v;
  };
  if (name == "img") {
    width = attribute("width");
    height = attribute("height");
    return true;
  }
  if (name == "canvas") {
    width = attribute("width");
    height = attribute("height");
    if (width < 0) width = 300;
    if (height < 0) height = 150;
    return true;
  }
  if (name == "video" || name == "iframe" || name == "embed" || name == "object") {
    width = attribute("width");
    height = attribute("height");
    if (width < 0) width = 300;
    if (height < 0) height = 150;
    return true;
  }
  return false;
}

class Builder {
 public:
  explicit Builder(LayoutContext& lc) : lc_(lc) {}

  void Build(dom::Document* document) {
    auto root = std::make_unique<Box>();
    root->kind = Box::Kind::Block;
    root->anonymous = true;
    root->node = document;
    root->style = AnonymousStyle(BoxStyle(), Display::Block);
    Box* initial = root.get();
    lc_.tree.root = std::move(root);
    dom::Element* html = document->DocumentElement();
    if (html) BuildElement(*initial, html);
    Normalize(*initial);
  }

 private:
  LayoutContext& lc_;

  void Register(Box* box) {
    if (box->node) lc_.tree.boxesOf[box->node].push_back(box);
  }

  static bool IsInline(const Box& b) { return b.inlineLevel; }

  // The children layout sees: the flat tree's.
  static std::vector<dom::Node*> RenderedChildren(dom::Node* node) {
    std::vector<dom::Node*> out;
    if (node->IsElement()) {
      dom::Element* element = static_cast<dom::Element*>(node);
      if (element->shadowRoot) {
        for (dom::Node* c = element->shadowRoot->firstChild; c; c = c->nextSibling) out.push_back(c);
        return out;
      }
      if (dom::IsSlot(element) && !element->assignedNodes.empty()) return dom::FindFlattenedSlottables(element);
    }
    for (dom::Node* c = node->firstChild; c; c = c->nextSibling) out.push_back(c);
    return out;
  }

  void BuildChildren(Box& parent, dom::Node* node, const std::shared_ptr<const BoxStyle>& textStyle) {
    for (dom::Node* child : RenderedChildren(node)) {
      if (child->IsElement()) {
        BuildElement(parent, static_cast<dom::Element*>(child));
      } else if (child->nodeType == dom::NodeType::Text || child->nodeType == dom::NodeType::CdataSection) {
        const std::string& data = static_cast<dom::CharacterData*>(child)->data;
        if (data.empty()) continue;
        auto text = std::make_unique<Box>();
        text->kind = Box::Kind::Text;
        text->inlineLevel = true;
        text->node = child;
        text->style = textStyle;
        text->text = data;
        Register(parent.AddChild(std::move(text)));
      }
    }
  }

  void BuildGenerated(Box& parent, dom::Element* element, const char* pseudo) {
    std::shared_ptr<const BoxStyle> style = ReadStyle(lc_.ctx, element, pseudo);
    if (style->display == Display::None || style->content == "normal" || style->content == "none" || style->content.empty()) return;
    // A string, or strings, are all of the content that is made here.
    std::string text;
    const std::string& c = style->content;
    for (size_t i = 0; i < c.size();) {
      if (c[i] == '"' || c[i] == '\'') {
        const char quote = c[i++];
        while (i < c.size() && c[i] != quote) {
          if (c[i] == '\\' && i + 1 < c.size()) ++i;
          text += c[i++];
        }
        ++i;
      } else {
        ++i;
      }
    }
    auto box = std::make_unique<Box>();
    box->kind = style->display == Display::Inline ? Box::Kind::Inline : Box::Kind::Block;
    box->inlineLevel = style->display == Display::Inline || style->display == Display::InlineBlock;
    box->node = nullptr;
    box->pseudo = pseudo;
    box->style = style;
    Box* raw = parent.AddChild(std::move(box));
    if (!text.empty()) {
      auto t = std::make_unique<Box>();
      t->kind = Box::Kind::Text;
      t->inlineLevel = true;
      t->style = AnonymousStyle(*style, Display::Inline);
      const_cast<BoxStyle&>(*t->style).color = style->color;
      t->text = text;
      raw->AddChild(std::move(t));
    }
    // The generated box belongs to its element for geometry.
    lc_.tree.boxesOf[element].push_back(raw);
  }

  void BuildElement(Box& parent, dom::Element* element) {
    std::shared_ptr<const BoxStyle> style = ReadStyle(lc_.ctx, element);
    if (style->display == Display::None) return;
    if (style->display == Display::Contents) {
      BuildGenerated(parent, element, "::before");
      BuildChildren(parent, element, style);
      BuildGenerated(parent, element, "::after");
      return;
    }
    auto box = std::make_unique<Box>();
    box->node = element;
    box->style = style;
    // Floating and positioning blockify what they apply to (https://www.w3.org/TR/CSS22/visuren.html#dis-pos-flo).
    if ((style->IsOutOfFlow() || style->IsFloating()) && !style->IsBlockLevel()) {
      auto changed = std::make_shared<BoxStyle>(*style);
      switch (changed->display) {
        case Display::InlineBlock: case Display::Inline: changed->display = Display::Block; break;
        case Display::InlineFlex: changed->display = Display::Flex; break;
        case Display::InlineGrid: changed->display = Display::Grid; break;
        case Display::InlineTable: changed->display = Display::Table; break;
        default: break;
      }
      box->style = changed;
    }
    const BoxStyle& s = *box->style;
    box->inlineLevel = !s.IsBlockLevel() && !s.IsOutOfFlow() && !s.IsFloating() &&
                       (s.display == Display::Inline || s.display == Display::InlineBlock || s.display == Display::InlineFlex || s.display == Display::InlineGrid ||
                        s.display == Display::InlineTable || s.display == Display::Ruby || s.display == Display::RubyText || s.display == Display::RubyBase);
    if (!s.IsBlockLevel() && !box->inlineLevel && !s.IsOutOfFlow() && !s.IsFloating()) {
      // table internals and the like: treated as blocks until tables are
      box->inlineLevel = false;
    }
    box->kind = (s.display == Display::Inline && !s.IsOutOfFlow() && !s.IsFloating()) ? Box::Kind::Inline : Box::Kind::Block;
    if (element->IsHtml("br")) {
      box->kind = Box::Kind::LineBreak;
      box->inlineLevel = true;
    }
    double w = -1, h = -1;
    if (ReplacedSize(element, w, h)) {
      box->replaced = true;
      box->naturalWidth = w;
      box->naturalHeight = h;
      // A replaced inline is atomic: it is laid out as a block inside the line.
      if (box->kind == Box::Kind::Inline) box->kind = Box::Kind::Block;
      Box* raw = parent.AddChild(std::move(box));
      Register(raw);
      return;
    }
    Box* raw = parent.AddChild(std::move(box));
    Register(raw);
    if (raw->kind == Box::Kind::LineBreak) return;
    BuildGenerated(*raw, element, "::before");
    BuildChildren(*raw, element, raw->style);
    BuildGenerated(*raw, element, "::after");
  }

  // ---- Anonymous boxes ----

  static bool ContainsBlock(const Box& inlineBox) {
    for (const auto& c : inlineBox.children) {
      if (!c->inlineLevel && !c->IsOutOfFlow()) return true;
      if (c->kind == Box::Kind::Inline && ContainsBlock(*c)) return true;
    }
    return false;
  }

  struct Segment {
    bool block;
    std::unique_ptr<Box> box;
  };

  std::unique_ptr<Box> CloneShallow(const Box& b) {
    auto c = std::make_unique<Box>();
    c->kind = b.kind;
    c->anonymous = b.anonymous;
    c->inlineLevel = b.inlineLevel;
    c->node = b.node;
    c->style = b.style;
    c->pseudo = b.pseudo;
    return c;
  }

  // An inline box with a block in it, as the parts before, the blocks, and the parts after.
  std::vector<Segment> Split(std::unique_ptr<Box> inlineBox) {
    std::vector<Segment> result;
    std::unique_ptr<Box> current = CloneShallow(*inlineBox);
    bool hasContent = false;
    const auto flush = [&] {
      if (hasContent) result.push_back({false, std::move(current)});
      current = CloneShallow(*inlineBox);
      hasContent = false;
    };
    for (auto& child : inlineBox->children) {
      if (!child->inlineLevel && !child->IsOutOfFlow()) {
        flush();
        child->parent = nullptr;
        result.push_back({true, std::move(child)});
      } else if (child->kind == Box::Kind::Inline && ContainsBlock(*child)) {
        std::vector<Segment> inner = Split(std::move(child));
        for (Segment& segment : inner) {
          if (segment.block) {
            flush();
            result.push_back(std::move(segment));
          } else {
            current->AddChild(std::move(segment.box));
            hasContent = true;
          }
        }
      } else {
        current->AddChild(std::move(child));
        hasContent = true;
      }
    }
    flush();
    inlineBox->children.clear();
    return result;
  }

  void Normalize(Box& container) {
    // Children first, as an inline in a block may itself need work.
    std::vector<std::unique_ptr<Box>> old = std::move(container.children);
    container.children.clear();
    std::vector<std::unique_ptr<Box>> run;
    std::vector<std::unique_ptr<Box>> output;
    bool anyBlock = false;
    for (auto& child : old) {
      if (!child->inlineLevel && !child->IsOutOfFlow()) anyBlock = true;
      else if (child->kind == Box::Kind::Inline && ContainsBlock(*child)) anyBlock = true;
    }
    if (!anyBlock) {
      for (auto& child : old) container.AddChild(std::move(child));
      container.hasInlineContent = !container.children.empty() && container.kind == Box::Kind::Block;
      for (auto& c : container.children) {
        if (c->kind == Box::Kind::Inline) Normalize(*c);
        else if (c->kind == Box::Kind::Block) Normalize(*c);
      }
      return;
    }
    const auto flushRun = [&] {
      if (run.empty()) return;
      bool significant = false;
      for (auto& r : run) {
        if (r->IsOutOfFlow()) continue;
        if (r->kind == Box::Kind::Text && OnlyCollapsibleSpace(*r)) continue;
        significant = true;
      }
      if (!significant) {
        // Only white space and things taken out of the flow: the white space goes, the rest stands where it is.
        for (auto& r : run) {
          if (r->IsOutOfFlow()) output.push_back(std::move(r));
        }
      } else {
        auto anonymous = std::make_unique<Box>();
        anonymous->kind = Box::Kind::Block;
        anonymous->anonymous = true;
        anonymous->style = AnonymousStyle(*container.style, Display::Block);
        anonymous->hasInlineContent = true;
        for (auto& r : run) anonymous->AddChild(std::move(r));
        output.push_back(std::move(anonymous));
      }
      run.clear();
    };
    for (auto& child : old) {
      if (child->kind == Box::Kind::Inline && ContainsBlock(*child)) {
        for (Segment& segment : Split(std::move(child))) {
          if (segment.block) {
            flushRun();
            output.push_back(std::move(segment.box));
          } else {
            run.push_back(std::move(segment.box));
          }
        }
      } else if (!child->inlineLevel && !child->IsOutOfFlow()) {
        flushRun();
        output.push_back(std::move(child));
      } else {
        run.push_back(std::move(child));
      }
    }
    flushRun();
    for (auto& o : output) container.AddChild(std::move(o));
    for (auto& c : container.children) {
      if (c->kind == Box::Kind::Block || c->kind == Box::Kind::Inline) Normalize(*c);
    }
    // The boxes of an element that was split are all the element's.
    RebuildRegistry();
  }

  void RebuildRegistry() {}
};

void Collect(Tree& tree, Box& box) {
  if (box.node && box.parent) tree.boxesOf[box.node].push_back(&box);
  if (!box.node && box.element() == nullptr && !box.pseudo.empty()) {}
  for (auto& c : box.children) Collect(tree, *c);
}

}  // namespace

void BuildTree(LayoutContext& lc, dom::Document* document) {
  Builder builder(lc);
  lc.tree.boxesOf.clear();
  builder.Build(document);
  // (The registry is made from the final tree: boxes were moved and cloned while it was normalized.)
  lc.tree.boxesOf.clear();
  Collect(lc.tree, *lc.tree.root);
}

}  // namespace solar::layout
