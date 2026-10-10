// The box tree: boxes for elements and text (https://www.w3.org/TR/CSS22/visuren.html#box-gen), with the anonymous boxes the model needs.
#include <cstdlib>
#include "solar/dom/Node.h"
#include <algorithm>
#include <functional>
#include <map>

#include "Internal.h"
#include "solar/css/Style.h"
#include "solar/css/Syntax.h"
#include "solar/css/Tokenizer.h"

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
  s.columnCount = 0;
  s.columnWidth = -1;
  s.columnFillAuto = false;
  s.columnSpanAll = false;
  s.breakBefore = s.breakAfter = s.breakInside = BreakKind::Auto;
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

// The elements that are replaced by something that is not in the document, and what is known of the size of that.
bool ReplacedSize(dom::Element* element, const std::string& base, Box& box) {
  const std::string& name = element->localName;
  const bool html = element->IsHtml();
  const bool svgRoot = element->namespaceUri == dom::kSvgNamespace && name == "svg" && !(element->parentNode && element->parentNode->IsElement() && static_cast<dom::Element*>(element->parentNode)->namespaceUri == dom::kSvgNamespace);
  if (!html && !svgRoot) return false;
  const auto attribute = [&](const char* attr) -> double {
    const dom::Attr* a = element->FindAttribute(attr);
    if (!a) return -1;
    char* end = nullptr;
    const double v = std::strtod(a->value.c_str(), &end);
    if (end == a->value.c_str() || v < 0) return -1;
    const std::string unit = end;
    return (unit.empty() || unit == "px") ? v : -1;
  };
  if (svgRoot) {
    box.attrWidth = attribute("width");
    box.attrHeight = attribute("height");
    box.naturalWidth = box.attrWidth;
    box.naturalHeight = box.attrHeight;
    if (const dom::Attr* vb = element->FindAttribute("viewBox")) {
      double v[4] = {0, 0, 0, 0};
      const char* p = vb->value.c_str();
      int n = 0;
      while (n < 4) {
        char* e = nullptr;
        while (*p == ' ' || *p == ',') ++p;
        v[n] = std::strtod(p, &e);
        if (e == p) break;
        p = e;
        ++n;
      }
      if (n == 4 && v[2] > 0 && v[3] > 0) box.naturalRatio = v[2] / v[3];
    }
    if (box.naturalWidth < 0 && box.naturalHeight < 0 && box.naturalRatio == 0) { box.naturalWidth = 300; box.naturalHeight = 150; }
    return true;
  }
  if (name == "img" || (name == "input" && element->FindAttribute("type") && element->FindAttribute("type")->value == "image")) {
    box.attrWidth = attribute("width");
    box.attrHeight = attribute("height");
    std::string src;
    if (const dom::Attr* a = element->FindAttribute("src")) src = a->value;
    // The first candidate of srcset stands in when there is no src.
    if (src.empty()) {
      if (const dom::Attr* set = element->FindAttribute("srcset")) {
        size_t i = 0;
        while (i < set->value.size() && std::isspace(static_cast<unsigned char>(set->value[i]))) ++i;
        size_t e = i;
        while (e < set->value.size() && !std::isspace(static_cast<unsigned char>(set->value[e])) && set->value[e] != ',') ++e;
        src = set->value.substr(i, e - i);
      }
    }
    double w = -1, h = -1, ratio = 0;
    if (!src.empty() && ImageMetricsOf(src, base, w, h, ratio)) {
      box.naturalWidth = w;
      box.naturalHeight = h;
      box.naturalRatio = ratio;
    } else {
      box.naturalWidth = box.naturalHeight = 0;  // broken: nothing, unless the attributes say
      box.naturalRatio = 0;
    }
    return true;
  }
  if (name == "canvas") {
    box.naturalWidth = attribute("width");
    box.naturalHeight = attribute("height");
    if (box.naturalWidth < 0) box.naturalWidth = 300;
    if (box.naturalHeight < 0) box.naturalHeight = 150;
    box.naturalRatio = box.naturalHeight > 0 ? box.naturalWidth / box.naturalHeight : 0;
    return true;
  }
  if (name == "video" || name == "iframe" || name == "embed" || name == "object") {
    box.attrWidth = attribute("width");
    box.attrHeight = attribute("height");
    box.naturalWidth = 300;
    box.naturalHeight = 150;
    box.naturalRatio = name == "video" ? 0 : 2;
    return true;
  }
  return false;
}

class Builder {
 public:
  explicit Builder(LayoutContext& lc) : lc_(lc) {}

  void Build(dom::Document* document) {
    styles_ = std::make_unique<CounterStyles>(document);
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
    AttachMarkers(*initial);
  }

 private:
  LayoutContext& lc_;
  std::unique_ptr<CounterStyles> styles_;

  // ---- Counters (https://www.w3.org/TR/css-lists-3/#counters) ----
  std::map<std::string, std::vector<int>> counters_;
  std::vector<std::vector<std::string>> frames_;
  int quoteDepth_ = 0;

  static std::vector<std::pair<std::string, int>> ParseCounterList(const std::string& text, int defaultValue) {
    std::vector<std::pair<std::string, int>> out;
    if (text == "none" || text.empty()) return out;
    const solar::css::ComponentValues values = solar::css::ParseComponentValues(text);
    for (size_t i = 0; i < values.size(); ++i) {
      if (!values[i].IsIdent()) continue;
      int value = defaultValue;
      size_t j = i + 1;
      while (j < values.size() && values[j].IsWhitespace()) ++j;
      if (j < values.size() && values[j].IsToken(solar::css::Token::Type::Number)) {
        value = static_cast<int>(values[j].token.number);
        i = j;
      }
      out.push_back({values[i - (i == j ? 0 : 0)].token.value, value});
    }
    return out;
  }

  void Instantiate(const std::string& name, int value) {
    if (frames_.empty()) frames_.emplace_back();
    auto& frame = frames_.back();
    // Two resets of one name in a scope: the second replaces the first.
    if (std::find(frame.begin(), frame.end(), name) != frame.end() && !counters_[name].empty()) {
      counters_[name].back() = value;
      return;
    }
    counters_[name].push_back(value);
    frame.push_back(name);
  }

  void ApplyCounters(const BoxStyle& style, bool listItem, dom::Element* element) {
    for (const auto& [name, value] : ParseCounterList(style.counterReset, 0)) Instantiate(name, value);
    // HTML: ol, ul and menu reset the list item counter; li may set it.
    if (element && element->IsHtml()) {
      const std::string& n = element->localName;
      if (n == "ol" || n == "ul" || n == "menu" || n == "dir") {
        int start = 0;
        if (n == "ol") {
          if (const dom::Attr* a = element->FindAttribute("start")) start = std::atoi(a->value.c_str()) - 1;
          if (element->FindAttribute("reversed")) {
            int count = 0;
            for (dom::Node* c = element->firstChild; c; c = c->nextSibling) if (c->IsElement() && static_cast<dom::Element*>(c)->IsHtml("li")) ++count;
            const dom::Attr* a = element->FindAttribute("start");
            start = (a ? std::atoi(a->value.c_str()) : count) + 1;
          }
        }
        Instantiate("list-item", start);
      }
    }
    bool incrementsListItem = false;
    for (const auto& [name, value] : ParseCounterList(style.counterIncrement, 1)) {
      if (counters_[name].empty()) Instantiate(name, 0);
      counters_[name].back() += value;
      if (name == "list-item") incrementsListItem = true;
    }
    if (listItem && !incrementsListItem) {
      if (counters_["list-item"].empty()) Instantiate("list-item", 0);
      const bool reversed = [&] {
        for (dom::Node* p = element ? element->parentNode : nullptr; p; p = nullptr) return p->IsElement() && static_cast<dom::Element*>(p)->FindAttribute("reversed") != nullptr;
        return false;
      }();
      counters_["list-item"].back() += reversed ? -1 : 1;
      if (element && element->IsHtml("li")) {
        if (const dom::Attr* a = element->FindAttribute("value")) counters_["list-item"].back() = std::atoi(a->value.c_str());
      }
    }
    for (const auto& [name, value] : ParseCounterList(style.counterSet, 0)) {
      if (counters_[name].empty()) Instantiate(name, 0);
      counters_[name].back() = value;
    }
  }

  void PushFrame() { frames_.emplace_back(); }
  void PopFrame() {
    if (frames_.empty()) return;
    for (const std::string& name : frames_.back()) {
      auto& stack = counters_[name];
      if (!stack.empty()) stack.pop_back();
    }
    frames_.pop_back();
  }

  static std::string ToRoman(int n, bool upper) {
    if (n <= 0 || n >= 4000) return std::to_string(n);
    static const std::pair<int, const char*> table[] = {{1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"}, {100, "c"}, {90, "xc"}, {50, "l"}, {40, "xl"}, {10, "x"}, {9, "ix"}, {5, "v"}, {4, "iv"}, {1, "i"}};
    std::string out;
    for (const auto& [value, symbol] : table) while (n >= value) { out += symbol; n -= value; }
    if (upper) for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
  }
  static std::string Alphabetic(int n, const char32_t* letters, int count) {
    if (n <= 0) return std::to_string(n);
    std::string out;
    while (n > 0) {
      --n;
      std::string one;
      solar::css::AppendUtf8(one, letters[n % count]);
      out = one + out;
      n /= count;
    }
    return out;
  }

  // The counter's value in a list-style-type; empty for none.
  std::string FormatCounter(int n, const std::string& type) {
    if (type == "none") return "";
    if (!type.empty() && (type[0] == '"' || type[0] == '\'')) return type.substr(1, type.size() - 2);
    return styles_->Format(type.empty() ? "decimal" : type, n);
  }
  static std::string OldFormatCounter(int n, const std::string& type) {
    static const char32_t latin[] = U"abcdefghijklmnopqrstuvwxyz";
    static const char32_t latinUpper[] = U"ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static const char32_t greek[] = U"αβγδεζηθικλμνξοπρστυφχψω";
    if (type == "none") return "";
    if (type == "decimal" || type.empty()) return std::to_string(n);
    if (type == "decimal-leading-zero") {
      const int m = n < 0 ? -n : n;
      return std::string(n < 0 ? "-" : "") + (m < 10 ? "0" : "") + std::to_string(m);
    }
    if (type == "lower-roman") return ToRoman(n, false);
    if (type == "upper-roman") return ToRoman(n, true);
    if (type == "lower-alpha" || type == "lower-latin") return Alphabetic(n, latin, 26);
    if (type == "upper-alpha" || type == "upper-latin") return Alphabetic(n, latinUpper, 26);
    if (type == "lower-greek") return Alphabetic(n, greek, 24);
    if (type == "disc") return "\xE2\x80\xA2";
    if (type == "circle") return "\xE2\x97\xA6";
    if (type == "square") return "\xE2\x96\xAA";
    if (type.size() >= 2 && (type[0] == '"' || type[0] == '\'')) return type.substr(1, type.size() - 2);
    return std::to_string(n);
  }

  // The text of a content value, with counters worked out now; false if it is not text (none, normal).
  bool ContentText(const BoxStyle& style, dom::Element* element, std::string& out) {
    if (style.content == "normal" || style.content == "none" || style.content.empty()) return false;
    const solar::css::ComponentValues values = solar::css::ParseComponentValues(style.content);
    for (const solar::css::ComponentValue& v : values) {
      if (v.IsWhitespace()) continue;
      if (v.IsToken(solar::css::Token::Type::String)) { out += v.token.value; continue; }
      if (v.IsIdent()) {
        const std::string n = v.token.value;
        const std::vector<std::string> pairs = QuotePairs(style);
        const auto quote = [&](bool open) {
          if (pairs.empty()) return;
          const size_t level = static_cast<size_t>(std::max(0, open ? quoteDepth_ : quoteDepth_ - 1));
          const size_t index = std::min(level, pairs.size() / 2 - 1);
          out += pairs[index * 2 + (open ? 0 : 1)];
        };
        if (n == "open-quote") { quote(true); ++quoteDepth_; }
        else if (n == "close-quote") { if (quoteDepth_ > 0) { quote(false); --quoteDepth_; } }
        else if (n == "no-open-quote") ++quoteDepth_;
        else if (n == "no-close-quote") { if (quoteDepth_ > 0) --quoteDepth_; }
        continue;
      }
      if (v.kind == solar::css::ComponentValue::Kind::Function) {
        std::string name = v.name;
        for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::vector<solar::css::ComponentValues> args(1);
        for (const auto& c : v.children) {
          if (c.IsToken(solar::css::Token::Type::Comma)) args.emplace_back();
          else if (!c.IsWhitespace()) args.back().push_back(c);
        }
        const auto word = [&](size_t i) -> std::string {
          if (i >= args.size() || args[i].empty()) return "";
          return args[i][0].token.value;
        };
        if (name == "counter" || name == "counters") {
          const std::string counter = word(0);
          const std::string style2 = word(name == "counter" ? 1 : 2);
          const std::string separator = name == "counters" ? word(1) : "";
          const auto& stack = counters_[counter];
          if (name == "counter") out += FormatCounter(stack.empty() ? 0 : stack.back(), style2.empty() ? "decimal" : style2);
          else {
            if (stack.empty()) out += FormatCounter(0, style2.empty() ? "decimal" : style2);
            for (size_t i = 0; i < stack.size(); ++i) out += (i ? separator : "") + FormatCounter(stack[i], style2.empty() ? "decimal" : style2);
          }
        } else if (name == "attr") {
          if (element) if (const dom::Attr* a = element->FindAttribute(word(0))) out += a->value;
        }
      }
    }
    return true;
  }

  static std::vector<std::string> QuotePairs(const BoxStyle& style) {
    std::vector<std::string> pairs;
    if (style.quotes == "none") return pairs;
    if (style.quotes != "auto" && !style.quotes.empty()) {
      for (const solar::css::ComponentValue& v : solar::css::ParseComponentValues(style.quotes)) if (v.IsToken(solar::css::Token::Type::String)) pairs.push_back(v.token.value);
      if (pairs.size() >= 2) return pairs;
    }
    return {"\xE2\x80\x9C", "\xE2\x80\x9D", "\xE2\x80\x98", "\xE2\x80\x99"};
  }

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
    PushFrame();
    struct Pop {
      Builder* b;
      ~Pop() { b->PopFrame(); }
    } pop{this};
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
    std::shared_ptr<const BoxStyle> style = Logicalize(ReadStyle(lc_.ctx, element, pseudo));
    if (style->display == Display::None) return;
    // Counters change for the pseudo-element as for any.
    ApplyCounters(*style, style->display == Display::ListItem, nullptr);
    std::string text;
    if (!ContentText(*style, element, text)) return;
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
      t->text = text;
      raw->AddChild(std::move(t));
    }
  }

  void BuildElement(Box& parent, dom::Element* element) {
    std::shared_ptr<const BoxStyle> style = Logicalize(ReadStyle(lc_.ctx, element));
    if (style->display == Display::None) return;
    ApplyCounters(*style, style->display == Display::ListItem, element);
    const int listValue = counters_["list-item"].empty() ? 0 : counters_["list-item"].back();
    if (style->display == Display::Contents) {
      BuildGenerated(parent, element, "before");
      BuildChildren(parent, element, style);
      BuildGenerated(parent, element, "after");
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
    box->listValue = listValue;
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
    if (ReplacedSize(element, dom::DocumentBaseUri(element->nodeDocument), *box)) {
      box->replaced = true;
      // A replaced inline is atomic: it is laid out as a block inside the line.
      if (box->kind == Box::Kind::Inline) box->kind = Box::Kind::Block;
      Box* raw = parent.AddChild(std::move(box));
      Register(raw);
      return;
    }
    Box* raw = parent.AddChild(std::move(box));
    Register(raw);
    if (raw->kind == Box::Kind::LineBreak) return;
    BuildGenerated(*raw, element, "before");
    BuildChildren(*raw, element, raw->style);
    BuildGenerated(*raw, element, "after");
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

  static Display Blockified(Display d) {
    switch (d) {
      case Display::Inline: case Display::InlineBlock: return Display::Block;
      case Display::InlineFlex: return Display::Flex;
      case Display::InlineGrid: return Display::Grid;
      case Display::InlineTable: return Display::Table;
      default: return d;
    }
  }

  // The children of a flex or grid container are its items: each element, and each run of text as an anonymous one.
  void NormalizeItems(Box& container) {
    std::vector<std::unique_ptr<Box>> old = std::move(container.children);
    container.children.clear();
    std::vector<std::unique_ptr<Box>> run;
    const auto flushRun = [&] {
      bool significant = false;
      for (auto& r : run) if (!OnlyCollapsibleSpace(*r)) significant = true;
      if (significant) {
        auto anonymous = std::make_unique<Box>();
        anonymous->kind = Box::Kind::Block;
        anonymous->anonymous = true;
        anonymous->forceBfc = true;
        anonymous->style = AnonymousStyle(*container.style, Display::Block);
        anonymous->hasInlineContent = true;
        for (auto& r : run) anonymous->AddChild(std::move(r));
        container.AddChild(std::move(anonymous));
      }
      run.clear();
    };
    for (auto& child : old) {
      if (child->kind == Box::Kind::Text) {
        run.push_back(std::move(child));
        continue;
      }
      flushRun();
      if (!child->IsOutOfFlow()) {
        if (child->inlineLevel || child->kind == Box::Kind::Inline) {
          child->inlineLevel = false;
          if (child->kind == Box::Kind::Inline) child->kind = Box::Kind::Block;
          auto changed = std::make_shared<BoxStyle>(*child->style);
          changed->display = Blockified(changed->display);
          child->style = changed;
        }
        child->forceBfc = true;
      }
      container.AddChild(std::move(child));
    }
    flushRun();
    for (auto& c : container.children) {
      if (c->kind == Box::Kind::Block || c->kind == Box::Kind::Inline) Normalize(*c);
    }
  }

  // ---- List markers ----
  void AttachMarkers(Box& box) {
    for (size_t i = 0; i < box.children.size(); ++i) AttachMarkers(*box.children[i]);
    if (box.style->display != Display::ListItem || !box.node || !box.node->IsElement() || box.kind != Box::Kind::Block) return;
    dom::Element* element = static_cast<dom::Element*>(box.node);
    const std::string type = box.style->listStyleType;
    if (type == "none") return;
    std::shared_ptr<const BoxStyle> markerStyle = Logicalize(ReadStyle(lc_.ctx, element, "marker"));
    std::string text;
    if (markerStyle->content != "normal" && markerStyle->content != "none" && !markerStyle->content.empty()) {
      if (!ContentText(*markerStyle, element, text)) return;
    } else {
      const bool string = !type.empty() && (type[0] == '"' || type[0] == '\'');
      text = FormatCounter(box.listValue, type);
      if (!string) text = styles_->Prefix(type.empty() ? "decimal" : type) + text + styles_->Suffix(type.empty() ? "decimal" : type);
    }
    if (text.empty()) return;
    auto textBox = std::make_unique<Box>();
    textBox->kind = Box::Kind::Text;
    textBox->inlineLevel = true;
    textBox->style = AnonymousStyle(*markerStyle, Display::Inline);
    textBox->text = text;
    if (box.style->listStyleInside) {
      Box* target = nullptr;
      if (box.hasInlineContent) target = &box;
      else if (!box.children.empty() && box.children[0]->anonymous && box.children[0]->hasInlineContent) target = box.children[0].get();
      if (!target) {
        auto anonymous = std::make_unique<Box>();
        anonymous->kind = Box::Kind::Block;
        anonymous->anonymous = true;
        anonymous->hasInlineContent = true;
        anonymous->style = AnonymousStyle(*box.style, Display::Block);
        target = anonymous.get();
        anonymous->parent = &box;
        box.children.insert(box.children.begin(), std::move(anonymous));
        box.hasInlineContent = false;
      }
      textBox->parent = target;
      target->children.insert(target->children.begin(), std::move(textBox));
    } else {
      auto marker = std::make_unique<Box>();
      marker->kind = Box::Kind::Block;
      marker->anonymous = true;
      marker->outsideMarker = true;
      marker->forceBfc = true;
      marker->hasInlineContent = true;
      marker->style = AnonymousStyle(*markerStyle, Display::Block);
      marker->AddChild(std::move(textBox));
      box.AddChild(std::move(marker));
    }
  }

  // ---- Tables (https://www.w3.org/TR/CSS22/tables.html#anonymous-boxes) ----

  static bool IsRowGroup(Display d) { return d == Display::TableRowGroup || d == Display::TableHeaderGroup || d == Display::TableFooterGroup; }

  std::unique_ptr<Box> AnonymousTablePart(const Box& parent, Display display) {
    auto box = std::make_unique<Box>();
    box->kind = Box::Kind::Block;
    box->anonymous = true;
    box->style = AnonymousStyle(*parent.style, display);
    if (display == Display::TableCell) box->forceBfc = true;
    return box;
  }

  // Children that are not cells become the contents of cells; each run of them one cell.
  void WrapInCells(Box& row) {
    std::vector<std::unique_ptr<Box>> old = std::move(row.children);
    row.children.clear();
    std::unique_ptr<Box> cell;
    const auto flush = [&] {
      if (!cell) return;
      bool significant = false;
      for (auto& c : cell->children) if (c->kind != Box::Kind::Text || !OnlyCollapsibleSpace(*c)) significant = true;
      if (significant) row.AddChild(std::move(cell));
      cell.reset();
    };
    for (auto& child : old) {
      const Display d = child->style->display;
      if (d == Display::TableCell) {
        flush();
        child->forceBfc = true;
        row.AddChild(std::move(child));
      } else if (d == Display::TableCaption || d == Display::TableColumn || d == Display::TableColumnGroup) {
        flush();
        row.AddChild(std::move(child));
      } else if (child->IsOutOfFlow()) {
        row.AddChild(std::move(child));
      } else {
        if (!cell) cell = AnonymousTablePart(row, Display::TableCell);
        child->inlineLevel = child->inlineLevel;
        cell->AddChild(std::move(child));
      }
    }
    flush();
  }

  void WrapInRows(Box& group) {
    std::vector<std::unique_ptr<Box>> old = std::move(group.children);
    group.children.clear();
    std::unique_ptr<Box> row;
    const auto flush = [&] {
      if (!row) return;
      WrapInCells(*row);
      if (!row->children.empty()) group.AddChild(std::move(row));
      row.reset();
    };
    for (auto& child : old) {
      const Display d = child->style->display;
      if (d == Display::TableRow) {
        flush();
        WrapInCells(*child);
        group.AddChild(std::move(child));
      } else if (child->IsOutOfFlow() || d == Display::TableCaption || d == Display::TableColumn || d == Display::TableColumnGroup) {
        flush();
        group.AddChild(std::move(child));
      } else {
        if (!row) row = AnonymousTablePart(group, Display::TableRow);
        row->AddChild(std::move(child));
      }
    }
    flush();
  }

  void NormalizeTable(Box& table) {
    std::vector<std::unique_ptr<Box>> old = std::move(table.children);
    table.children.clear();
    std::unique_ptr<Box> row;
    const auto flush = [&] {
      if (!row) return;
      WrapInCells(*row);
      if (!row->children.empty()) table.AddChild(std::move(row));
      row.reset();
    };
    for (auto& child : old) {
      const Display d = child->style->display;
      if (IsRowGroup(d)) {
        flush();
        WrapInRows(*child);
        table.AddChild(std::move(child));
      } else if (d == Display::TableRow) {
        flush();
        WrapInCells(*child);
        table.AddChild(std::move(child));
      } else if (child->IsOutOfFlow() || d == Display::TableCaption || d == Display::TableColumn || d == Display::TableColumnGroup) {
        flush();
        table.AddChild(std::move(child));
      } else if (d == Display::TableCell) {
        if (!row) row = AnonymousTablePart(table, Display::TableRow);
        child->forceBfc = true;
        row->AddChild(std::move(child));
      } else {
        if (!row) row = AnonymousTablePart(table, Display::TableRow);
        row->AddChild(std::move(child));
      }
    }
    flush();
    // Each cell is a block container of its own: normalize it and everything inside.
    const std::function<void(Box&)> walk = [&](Box& b) {
      for (auto& c : b.children) {
        const Display d = c->style->display;
        if (d == Display::TableCell || d == Display::TableCaption) {
          c->forceBfc = true;
          Normalize(*c);
        } else if (IsRowGroup(d) || d == Display::TableRow || d == Display::TableColumnGroup || d == Display::TableColumn) {
          walk(*c);
        } else if (c->kind == Box::Kind::Block) {
          Normalize(*c);
        }
      }
    };
    walk(table);
  }

  void Normalize(Box& container) {
    if ((container.style->display == Display::Table || container.style->display == Display::InlineTable) && container.kind == Box::Kind::Block && !container.replaced) {
      NormalizeTable(container);
      return;
    }
    switch (container.style->display) {
      case Display::Flex: case Display::InlineFlex: case Display::Grid: case Display::InlineGrid:
        if (container.kind == Box::Kind::Block && !container.replaced) {
          NormalizeItems(container);
          return;
        }
        break;
      default: break;
    }
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
