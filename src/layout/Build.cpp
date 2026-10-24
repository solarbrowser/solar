// The box tree: boxes for elements and text (https://www.w3.org/TR/CSS22/visuren.html#box-gen), with the anonymous boxes the model needs.
#include <cstdlib>
#include "solar/dom/Node.h"
#include <algorithm>
#include <functional>
#include <map>

#include "Internal.h"
#include "solar/css/MediaQuery.h"
#include "solar/css/Selectors.h"
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
  const bool svgRoot = element->namespaceUri == dom::kSvgNamespace && name == "svg" && !(dom::ParentOf(element) && dom::ParentOf(element)->IsElement() && static_cast<dom::Element*>(dom::ParentOf(element))->namespaceUri == dom::kSvgNamespace);
  if (!html && !svgRoot) return false;
  const auto attribute = [&](const char* attr) -> double {
    const dom::Attr* a = element->FindAttribute(attr);
    if (!a) return -1;
    char* end = nullptr;
    const double v = std::strtod(a->Value().c_str(), &end);
    if (end == a->Value().c_str() || v < 0) return -1;
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
      const char* p = vb->Value().c_str();
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
    // The ratio is that of the attributes when there are both; the view box is for when there is not.
    if (box.attrWidth > 0 && box.attrHeight > 0) box.naturalRatio = box.attrWidth / box.attrHeight;
    if (box.naturalWidth < 0 && box.naturalHeight < 0 && box.naturalRatio == 0) { box.naturalWidth = 300; box.naturalHeight = 150; }
    return true;
  }
  if (name == "img" || (name == "input" && element->FindAttribute("type") && element->FindAttribute("type")->Value() == "image")) {
    box.attrWidth = attribute("width");
    box.attrHeight = attribute("height");
    std::string src;
    if (const dom::Attr* a = element->FindAttribute("src")) src = a->Value();
    // The first candidate of srcset stands in when there is no src.
    if (src.empty()) {
      if (const dom::Attr* set = element->FindAttribute("srcset")) {
        size_t i = 0;
        while (i < set->Value().size() && std::isspace(static_cast<unsigned char>(set->Value()[i]))) ++i;
        size_t e = i;
        while (e < set->Value().size() && !std::isspace(static_cast<unsigned char>(set->Value()[e])) && set->Value()[e] != ',') ++e;
        src = set->Value().substr(i, e - i);
      }
    }
    double w = -1, h = -1, ratio = 0;
    if (!src.empty()) box.imageUrl = AbsoluteAddress(src, base);
    if (!src.empty() && box.attrWidth > 0 && box.attrHeight > 0) {
      // A picture with both its sizes given is laid out by them: what it is is not waited for (and the page is not laid out again when it comes).
      box.naturalWidth = box.attrWidth;
      box.naturalHeight = box.attrHeight;
      box.naturalRatio = box.attrWidth / box.attrHeight;
    } else if (!src.empty() && ImageMetricsOf(src, base, w, h, ratio)) {
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

long g_visited = 0, g_recomputed = 0, g_reused = 0;

class Builder {
 public:
  Builder(LayoutContext& lc, Tree* previous) : lc_(lc), previous_(previous) {}

  uint64_t Serial() const { return serial_; }
  bool Reusing() const { return canReuse_; }

  // https://www.w3.org/TR/css-overflow-3/#overflow-propagation: the overflow of the root element is the viewport's, or the body's if the root
  // element's is visible; the element it came from does not clip.
  void PropagateViewportOverflow(Box& initial, dom::Document* document) {
    Box* html = nullptr;
    for (auto& c : initial.children) if (c->node == document->DocumentElement()) html = c.get();
    if (!html) return;
    const auto visible = [](const BoxStyle& p) { return p.overflowX == Overflow::Visible && p.overflowY == Overflow::Visible; };
    Box* source = html;
    if (visible(html->style->Physical())) {
      source = nullptr;
      for (auto& c : html->children) {
        if (c->node && c->node->IsElement() && static_cast<dom::Element*>(c->node)->IsHtml("body")) {
          source = c.get();
          break;
        }
      }
    }
    if (!source) return;
    const BoxStyle& p = source->style->Physical();
    if (visible(p)) return;
    Overflow x = p.overflowX, y = p.overflowY;
    // clip is hidden for the viewport; a visible one beside a scrolling one is auto.
    if (x == Overflow::Clip) x = Overflow::Hidden;
    if (y == Overflow::Clip) y = Overflow::Hidden;
    if (x == Overflow::Visible) x = Overflow::Auto;
    if (y == Overflow::Visible) y = Overflow::Auto;
    lc_.tree.viewportOverflowX = x;
    lc_.tree.viewportOverflowY = y;
    auto style = std::make_shared<BoxStyle>(*source->style);
    style->overflowX = style->overflowY = Overflow::Visible;
    if (style->physicalStyle) {
      auto physical = std::make_shared<BoxStyle>(*style->physicalStyle);
      physical->overflowX = physical->overflowY = Overflow::Visible;
      style->physicalStyle = physical;
    }
    source->style = style;
  }

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
    reuse_ = css::StyleReuseFor(document);
    if (reuse_.shadows) reuse_.allowed = false;  // (the boxes are of the flat tree)
    {
      // (What is kept of the styles is checked against the animations, which are brought up to date by asking for a style.)
      const css::MediaEnvironment env = css::EnvironmentFor(document);
      environmentKey_ = std::hash<double>()(env.width) * 31 + std::hash<double>()(env.height) * 17 + std::hash<double>()(env.resolution) * 7 + std::hash<std::string>()(env.colorScheme);
    }
    if (html) css::ComputedValue(lc_.ctx, html, "display");
    // The boxes of the tree before are kept for what has not changed, when nothing in the document makes a box depend on more than its own subtree.
    canReuse_ = previous_ && previous_->root && reuse_.allowed && !getenv("SOLAR_NOREUSE") && !document->contentVisibilityAutoSeen &&
                !css::g_firstLetterSeen && !css::g_firstLineSeen && !dom::HasShadowTrees();
    if (getenv("SOLAR_LAYOUT_DEBUG")) std::fprintf(stderr, "  build: previous %d allowed %d shadows %d cvauto %d firstLetter %d firstLine %d -> reuse %d\n", previous_ != nullptr, reuse_.allowed, dom::HasShadowTrees(), document->contentVisibilityAutoSeen, css::g_firstLetterSeen, css::g_firstLineSeen, canReuse_);
    if (canReuse_) {
      lc_.tree.boxesOf = std::move(previous_->boxesOf);
      previous_->damaged = true;
    }
    g_visited = g_recomputed = g_reused = 0;
    if (html) BuildElement(*initial, html);
    if (getenv("SOLAR_LAYOUT_DEBUG")) std::fprintf(stderr, "  build: visited %ld, style made again for %ld, boxes reused %ld\n", g_visited, g_recomputed, g_reused);
    Normalize(*initial);
    ApplyFirstLetters(lc_, *initial);
    ApplyFirstLines(lc_, *initial);
    AttachMarkers(*initial);
    PropagateViewportOverflow(*initial, document);
  }

 private:
  LayoutContext& lc_;
  solar::css::StyleReuse reuse_;
  uint64_t environmentKey_ = 0;
  bool dirtyBelow_ = false;  // the style of an element above was made again
  Tree* previous_ = nullptr;
  bool canReuse_ = false;
  static inline uint64_t g_serial = 0;
  const uint64_t serial_ = ++g_serial;
  uint64_t volatileEvents_ = 0;  // counted when something is built that depends on more than the nodes under it (counters, replaced contents)
  std::unordered_map<const Box*, std::unique_ptr<Box>*> slots_;  // where the boxes of the tree before are held
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
      const size_t nameAt = i;
      size_t j = i + 1;
      while (j < values.size() && values[j].IsWhitespace()) ++j;
      if (j < values.size() && values[j].IsToken(solar::css::Token::Type::Number)) {
        value = static_cast<int>(values[j].token.number);
        i = j;
      }
      out.push_back({values[nameAt].token.value, value});
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
    // (What reads or sets a counter depends on the elements before it.)
    if (listItem || (!style.counterReset.empty() && style.counterReset != "none") || (!style.counterIncrement.empty() && style.counterIncrement != "none") ||
        (!style.counterSet.empty() && style.counterSet != "none") || (element && element->IsHtml() && (element->localName == "ol" || element->localName == "ul" || element->localName == "menu" || element->localName == "dir" || element->localName == "li"))) {
      ++volatileEvents_;
    }
    for (const auto& [name, value] : ParseCounterList(style.counterReset, 0)) Instantiate(name, value);
    // HTML: ol, ul and menu reset the list item counter; li may set it.
    if (element && element->IsHtml()) {
      const std::string& n = element->localName;
      if (n == "ol" || n == "ul" || n == "menu" || n == "dir") {
        int start = 0;
        if (n == "ol") {
          if (const dom::Attr* a = element->FindAttribute("start")) start = std::atoi(a->Value().c_str()) - 1;
          if (element->FindAttribute("reversed")) {
            int count = 0;
            for (dom::Node* c = dom::FirstChildOf(element); c; c = dom::NextSiblingOf(c)) if (c->IsElement() && static_cast<dom::Element*>(c)->IsHtml("li")) ++count;
            const dom::Attr* a = element->FindAttribute("start");
            start = (a ? std::atoi(a->Value().c_str()) : count) + 1;
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
        for (dom::Node* p = element ? dom::ParentOf(element) : nullptr; p; p = nullptr) return p->IsElement() && static_cast<dom::Element*>(p)->FindAttribute("reversed") != nullptr;
        return false;
      }();
      counters_["list-item"].back() += reversed ? -1 : 1;
      if (element && element->IsHtml("li")) {
        if (const dom::Attr* a = element->FindAttribute("value")) counters_["list-item"].back() = std::atoi(a->Value().c_str());
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
    if (style.content.find("counter") != std::string::npos || style.content.find("quote") != std::string::npos) ++volatileEvents_;
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
          if (element) if (const dom::Attr* a = element->FindAttribute(word(0))) out += a->Value();
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

  // (The registry of boxes is made from the final tree.)
  void Register(Box*) {}

  bool Reused(const Box& box) const { return box.reusedIn == serial_; }

  // The box of an element from the tree before, taken out of it, when it is good as it was: its style and everything in it unchanged.
  std::unique_ptr<Box> TakeOld(const Box& parent, dom::Element* element) {
    if (!canReuse_ || element->subtreeBuilt != element->VersionsInView().subtree) return nullptr;
    if (element->IsHtml("html") || element->IsHtml("body")) return nullptr;  // (the viewport takes its overflow from them)
    switch (parent.style->display) {
      case Display::Ruby: case Display::RubyBase: case Display::RubyText: case Display::RubyBaseContainer: case Display::RubyTextContainer: return nullptr;
      default: break;
    }
    const auto found = lc_.tree.boxesOf.find(element);
    if (found == lc_.tree.boxesOf.end() || found->second.size() != 1) return nullptr;
    Box* old = found->second[0];
    if (old->volatileSubtree || old->node != element || !old->parent || old->kind == Box::Kind::LineBreak) return nullptr;
    // A box that block-in-inline splitting took out of the box of its parent is not taken over: where it stands is not where the elements put it.
    {
      const Box* holder = old->parent;
      while (holder && holder->anonymous && holder->parent) holder = holder->parent;
      if (!holder || holder->node != dom::ParentOf(element)) return nullptr;
    }
    auto slot = slots_.find(old);
    if (slot == slots_.end()) {
      for (auto& c : old->parent->children) slots_[c.get()] = &c;
      slot = slots_.find(old);
    }
    if (slot == slots_.end() || !*slot->second) return nullptr;
    std::unique_ptr<Box> taken = std::move(*slot->second);
    taken->reusedIn = serial_;
    return taken;
  }

  static bool IsInline(const Box& b) { return b.inlineLevel; }

  // The children layout sees: the flat tree's.
  static std::vector<dom::Node*> RenderedChildren(dom::Node* node) {
    std::vector<dom::Node*> out;
    if (node->IsElement()) {
      dom::Element* element = static_cast<dom::Element*>(node);
      if (element->ShadowRootInView()) {
        for (dom::Node* c = dom::FirstChildOf(element->ShadowRootInView()); c; c = dom::NextSiblingOf(c)) out.push_back(c);
        return out;
      }
      if (dom::IsSlot(element) && !element->AssignedNodesInView().empty()) return dom::FindFlattenedSlottables(element);
    }
    for (dom::Node* c = dom::FirstChildOf(node); c; c = dom::NextSiblingOf(c)) out.push_back(c);
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
        const std::string& data = static_cast<dom::CharacterData*>(child)->Data();
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

  // The language of an element, from the nearest lang attribute above it (the only thing in the style layout reads that is of the element and not of what it shares).
  static std::string LangOf(const dom::Element* element) {
    for (const dom::Node* n = element; n; n = dom::ParentOf(n)) {
      if (!n->IsElement()) continue;
      const dom::Element* e = static_cast<const dom::Element*>(n);
      const dom::Attr* a = e->FindAttribute("", "lang");
      if (!a) a = e->FindAttribute("http://www.w3.org/XML/1998/namespace", "lang");
      if (a) {
        std::string lang = a->Value();
        for (char& c : lang) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return lang;
      }
    }
    return "";
  }

  // Elements that share their computed style share what is read of it, too: it is made once for them (and once for each language they are in).
  std::shared_ptr<const BoxStyle> SharedBoxStyle(dom::Element* element, const std::string& pseudo = "") {
    struct Kept {
      std::shared_ptr<void> token;
      std::shared_ptr<const BoxStyle> style;
    };
    static std::map<std::pair<const void*, std::string>, Kept> kept;
    static uint64_t version = 0;
    const uint64_t now = (dom::DocumentVersion() * 1000003 + css::SheetsVersion()) * 1000003 + css::StyleVersion();
    if (version != now || kept.size() > 20000) {
      kept.clear();
      version = now;
    }
    if (pseudo.empty()) css::ComputedValue(lc_.ctx, element, "display");  // (the style is made, with all that goes with it, before its token is looked at)
    std::shared_ptr<void> token = css::SharedStyleToken(lc_.ctx, element, pseudo);
    if (!token) return Logicalize(ReadStyle(lc_.ctx, element, pseudo));
    const auto key = std::make_pair(static_cast<const void*>(token.get()), LangOf(element));
    const auto found = kept.find(key);
    if (found != kept.end()) return found->second.style;
    std::shared_ptr<const BoxStyle> style = Logicalize(ReadStyle(lc_.ctx, element, pseudo));
    kept[key] = {token, style};
    return style;
  }

  // The style layout reads of an element: what was made of it before, while nothing it depends on has changed, and else worked out again.
  std::shared_ptr<const BoxStyle> StyleFor(dom::Element* element, bool& recomputed) {
    const dom::Node* parent = dom::ParentOf(element);
    const dom::Element* parentElement = parent && parent->IsElement() ? static_cast<const dom::Element*>(parent) : nullptr;
    dom::Element::LayoutStamp want;
    want.author = css::SheetsVersion();
    want.environment = environmentKey_;
    want.parent = parent;
    const dom::Element::Versions ev = element->VersionsInView();
    const dom::Element::Versions pv = parentElement ? parentElement->VersionsInView() : dom::Element::Versions();
    want.attr = ev.attr;
    want.anim = ev.anim;
    want.children = reuse_.structural ? ev.children : 0;
    want.parentChildren = reuse_.structural && parentElement ? pv.children : 0;
    want.parentChildAttr = reuse_.sibling && parentElement ? pv.childAttr : 0;
    want.epoch = reuse_.allowed ? css::StyleEpochOf(element) : 0;
    const dom::Element::LayoutStamp& have = element->layoutStamp;
    if (reuse_.allowed && !dirtyBelow_ && element->layoutStyle && have.author == want.author && have.environment == want.environment && have.parent == want.parent &&
        have.attr == want.attr && have.anim == want.anim && have.children == want.children && have.parentChildren == want.parentChildren && have.parentChildAttr == want.parentChildAttr && have.epoch == want.epoch) {
      recomputed = false;
      return std::static_pointer_cast<const BoxStyle>(element->layoutStyle);
    }
    std::shared_ptr<const BoxStyle> style = SharedBoxStyle(element);
    element->layoutStyle = style;
    element->layoutStamp = want;
    recomputed = true;
    return style;
  }

  void BuildGenerated(Box& parent, dom::Element* element, const char* pseudo) {
    // Without a rule for it there is nothing generated: the style of it is not worth working out.
    if (!solar::css::HasPseudoStyle(element, pseudo)) return;
    std::shared_ptr<const BoxStyle> style = SharedBoxStyle(element, pseudo);
    if (style->display == Display::None) return;
    // Floating and positioning blockify a generated box as they do any (https://www.w3.org/TR/CSS22/visuren.html#dis-pos-flo).
    if ((style->IsOutOfFlow() || style->IsFloating()) && (style->display == Display::Inline || style->display == Display::InlineBlock)) {
      auto changed = std::make_shared<BoxStyle>(*style);
      changed->display = Display::Block;
      style = changed;
    }
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
    bool recomputed = false;
    std::shared_ptr<const BoxStyle> style = StyleFor(element, recomputed);
    ++g_visited;
    if (recomputed) ++g_recomputed;
    // What an element's style is made again from, its children's is too (they inherit).
    struct Below {
      Builder* builder;
      bool saved;
      ~Below() { builder->dirtyBelow_ = saved; }
    } below{this, dirtyBelow_};
    dirtyBelow_ = dirtyBelow_ || recomputed;
    if (style->display == Display::None) return;
    if (!recomputed && style->display != Display::Contents) {
      if (std::unique_ptr<Box> old = TakeOld(parent, element)) {
        ++g_reused;
        parent.AddChild(std::move(old));
        return;
      }
    }
    const uint64_t volatileBefore = volatileEvents_;
    struct Seal {
      Builder* b;
      dom::Element* element;
      Box** box;
      uint64_t before;
      ~Seal() {
        if (!*box) return;
        (*box)->volatileSubtree = b->volatileEvents_ != before;
        element->subtreeBuilt = element->VersionsInView().subtree;
      }
    };
    Box* sealed = nullptr;
    Seal seal{this, element, &sealed, volatileBefore};
    // Style containment keeps the counters of its subtree to itself.
    struct Scope {
      Builder* b;
      bool on;
      std::map<std::string, std::vector<int>> counters;
      int quotes;
      ~Scope() {
        if (!on) return;
        b->counters_ = std::move(counters);
        b->quoteDepth_ = quotes;
      }
    } scope{this, style->containStyle, style->containStyle ? counters_ : std::map<std::string, std::vector<int>>(), quoteDepth_};
    ApplyCounters(*style, style->display == Display::ListItem, element);
    const int listValue = counters_["list-item"].empty() ? 0 : counters_["list-item"].back();
    if (style->display == Display::Contents) {
      BuildGenerated(parent, element, "before");
      BuildChildren(parent, element, style);
      BuildGenerated(parent, element, "after");
      return;
    }
    if (style->contentVisibilityAuto) {
      if (element->nodeDocument) element->nodeDocument->contentVisibilityAutoSeen = true;
      if (!element->contentVisible) {
        auto skipped = std::make_shared<BoxStyle>(*style);
        skipped->containSizeInline = skipped->containSizeBlock = true;
        skipped->skipContents = true;
        style = skipped;
      }
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
        // (a ruby annotation or base that floats or is positioned is a block: it is not part of the ruby any more)
        case Display::RubyText: case Display::RubyBase: case Display::RubyTextContainer: case Display::RubyBaseContainer: changed->display = Display::Block; break;
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
    const bool isReplaced = ReplacedSize(element, dom::DocumentBaseUri(element->nodeDocument), *box);
    const bool control = !isReplaced && ControlSize(lc_, element, *box);
    // A form control is not a container of boxes: flex and grid are a block (or an inline-block) to it.
    if (control) {
      const Display d = box->style->display;
      if (d == Display::Flex || d == Display::Grid || d == Display::InlineFlex || d == Display::InlineGrid) {
        auto changed = std::make_shared<BoxStyle>(*box->style);
        changed->display = (d == Display::Flex || d == Display::Grid) ? Display::Block : Display::InlineBlock;
        changed->gridLanes = false;
        if (changed->physicalStyle) {
          auto physical = std::make_shared<BoxStyle>(*changed->physicalStyle);
          physical->display = changed->display;
          physical->gridLanes = false;
          changed->physicalStyle = physical;
        }
        box->style = changed;
        box->inlineLevel = changed->display == Display::InlineBlock && !box->style->IsOutOfFlow() && !box->style->IsFloating();
      }
    }
    // The table-internal displays do not apply to what is replaced or a control: it is inline.
    if (isReplaced || control) {
      const Display d = box->style->display;
      if (d == Display::TableRowGroup || d == Display::TableHeaderGroup || d == Display::TableFooterGroup || d == Display::TableRow || d == Display::TableCell ||
          d == Display::TableColumn || d == Display::TableColumnGroup || d == Display::TableCaption) {
        auto changed = std::make_shared<BoxStyle>(*box->style);
        changed->display = control ? Display::InlineBlock : Display::Inline;
        if (changed->physicalStyle) {
          auto physical = std::make_shared<BoxStyle>(*changed->physicalStyle);
          physical->display = changed->display;
          changed->physicalStyle = physical;
        }
        box->style = changed;
        box->inlineLevel = true;
        box->kind = Box::Kind::Block;
      }
    }
    if (isReplaced) {
      box->replaced = true;
      // A replaced inline is atomic: it is laid out as a block inside the line.
      if (box->kind == Box::Kind::Inline) box->kind = Box::Kind::Block;
      Box* raw = parent.AddChild(std::move(box));
      Register(raw);
      sealed = raw;
      ++volatileEvents_;  // (what a replaced element shows is not in the document's nodes)
      return;
    }
    // A control is atomic in a line, and its options and the like are not boxes of their own.
    if (control && box->kind == Box::Kind::Inline) box->kind = Box::Kind::Block;
    Box* raw = parent.AddChild(std::move(box));
    Register(raw);
    sealed = raw;
    if (control) ++volatileEvents_;  // (nor what a form control shows)
    if (raw->kind == Box::Kind::LineBreak) return;
    if (control && !element->IsHtml("textarea")) return;
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
    c->volatileSubtree = true;  // (a piece of an inline box with a block in it: the boxes of the block are not in it)
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
  // The base and the annotations of a ruby, in the order of its columns: a base, then what is over it and under it.
  void NormalizeRuby(Box& ruby) {
    std::vector<std::unique_ptr<Box>> old = std::move(ruby.children);
    ruby.children.clear();
    struct Column {
      std::unique_ptr<Box> base;
      std::vector<std::unique_ptr<Box>> annotations;
    };
    std::vector<Column> columns;
    const auto block = [&](std::unique_ptr<Box> box, int role) {
      auto style = std::make_shared<BoxStyle>(*box->style);
      style->display = Display::Block;
      // (a base or an annotation is a box of the ruby: it does not float or sit somewhere else)
      style->floating = Float::None;
      if (style->position == Position::Absolute || style->position == Position::Fixed) style->position = Position::Static;
      if (!ruby.style->rubyStart) style->textAlign = TextAlign::Center;
      if (style->physicalStyle) {
        auto physical = std::make_shared<BoxStyle>(*style->physicalStyle);
        physical->display = Display::Block;
        physical->floating = Float::None;
        if (physical->position == Position::Absolute || physical->position == Position::Fixed) physical->position = Position::Static;
        style->physicalStyle = physical;
      }
      box->style = style;
      box->inlineLevel = false;
      box->kind = Box::Kind::Block;
      box->rubyRole = static_cast<uint8_t>(role);
      return box;
    };
    std::vector<std::unique_ptr<Box>> pending;
    int containers = 0;
    bool lastUnder = false;
    const auto flush = [&] {
      bool significant = false;
      for (auto& p : pending) if (!(p->kind == Box::Kind::Text && OnlyCollapsibleSpace(*p))) significant = true;
      if (significant) {
        auto base = std::make_unique<Box>();
        base->kind = Box::Kind::Block;
        base->anonymous = true;
        base->style = AnonymousStyle(*ruby.style, Display::Block);
        if (!ruby.style->rubyStart) {
          auto centered = std::make_shared<BoxStyle>(*base->style);
          centered->textAlign = TextAlign::Center;
          base->style = centered;
        }
        base->rubyRole = 1;
        for (auto& p : pending) base->AddChild(std::move(p));
        columns.push_back({std::move(base), {}});
      }
      pending.clear();
    };
    const auto annotate = [&](size_t column, std::unique_ptr<Box> note) {
      while (columns.size() <= column) {
        auto base = std::make_unique<Box>();
        base->kind = Box::Kind::Block;
        base->anonymous = true;
        base->style = AnonymousStyle(*ruby.style, Display::Block);
        base->rubyRole = 1;
        columns.push_back({std::move(base), {}});
      }
      const int role = note->style->rubyUnder ? 3 : 2;
      columns[column].annotations.push_back(block(std::move(note), role));
    };
    for (auto& child : old) {
      switch (child->style->display) {
        case Display::RubyBase:
          flush();
          columns.push_back({block(std::move(child), 1), {}});
          break;
        case Display::RubyText: {
          flush();
          // The side is the ruby's, whatever the annotation says of itself.
          auto style = std::make_shared<BoxStyle>(*child->style);
          style->rubyUnder = ruby.style->rubyUnder;
          child->style = style;
          annotate(columns.empty() ? 0 : columns.size() - 1, std::move(child));
          break;
        }
        case Display::RubyTextContainer: {
          flush();
          // Where it is: as it says, or, when it alternates, the other side from the container before (the first is over unless the ruby starts under).
          bool under = child->style->rubyUnder;
          if (child->style->rubyAlternate) under = containers == 0 ? ruby.style->rubyUnder && ruby.style->rubyAlternate : !lastUnder;
          lastUnder = under;
          ++containers;
          size_t j = 0;
          std::vector<std::unique_ptr<Box>> notes = std::move(child->children);
          for (auto& n : notes) {
            if (n->style->display != Display::RubyText) continue;
            n->parent = nullptr;
            auto style = std::make_shared<BoxStyle>(*n->style);
            style->rubyUnder = under;
            n->style = style;
            annotate(j++, std::move(n));
          }
          break;
        }
        default:
          pending.push_back(std::move(child));
          break;
      }
    }
    flush();
    for (Column& c : columns) {
      ruby.AddChild(std::move(c.base));
      for (auto& a : c.annotations) ruby.AddChild(std::move(a));
    }
    ruby.hasInlineContent = false;
    for (auto& c : ruby.children) Normalize(*c);
  }

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
    if (Reused(box)) return;
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
    textBox->pseudo = "marker";
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
        if (!Reused(*child)) WrapInCells(*child);
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
        if (!Reused(*child)) WrapInRows(*child);
        table.AddChild(std::move(child));
      } else if (d == Display::TableRow) {
        flush();
        if (!Reused(*child)) WrapInCells(*child);
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
    if (Reused(container)) return;
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
      case Display::Ruby:
        if (container.kind == Box::Kind::Block && !container.replaced) {
          NormalizeRuby(container);
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

// The boxes of the final tree, but those taken over whole from the tree before (the registry has them).
void Collect(Tree& tree, Box& box, uint64_t serial) {
  if (box.reusedIn == serial) return;
  if (box.node && box.parent) tree.boxesOf[box.node].push_back(&box);
  for (auto& c : box.children) Collect(tree, *c, serial);
}

// What is left of the tree before are the boxes that were not kept: out of the registry.
void Forget(Tree& tree, const Box& box) {
  if (box.node) {
    const auto found = tree.boxesOf.find(box.node);
    if (found != tree.boxesOf.end()) {
      std::erase(found->second, const_cast<Box*>(&box));
      if (found->second.empty()) tree.boxesOf.erase(found);
    }
  }
  for (const auto& c : box.children) {
    if (c) Forget(tree, *c);
  }
}

}  // namespace

bool BuildTree(LayoutContext& lc, dom::Document* document, Tree* previous) {
  Builder builder(lc, previous);
  builder.Build(document);
  // (The registry is made from the final tree: boxes were moved and cloned while it was normalized.)
  if (builder.Reusing()) {
    Forget(lc.tree, *previous->root);
  } else {
    lc.tree.boxesOf.clear();
  }
  Collect(lc.tree, *lc.tree.root, builder.Serial());
  return builder.Reusing();
}

}  // namespace solar::layout
