// ::first-letter (https://www.w3.org/TR/css-pseudo-4/#first-letter-pseudo): the first typographic letter unit of the first formatted line of a block
// container, with the punctuation around it, is a box of its own, styled by the pseudo-element's rules.
#include "Internal.h"
#include "solar/css/Selectors.h"
#include "solar/css/Style.h"
#include "solar/css/Tokenizer.h"
#include "solar/text/Unicode.h"

namespace solar::layout {

namespace {

using solar::text::GeneralCategory;
using solar::text::GeneralCategoryOf;

char32_t Decode(const std::string& s, size_t& i) {
  const unsigned char c = static_cast<unsigned char>(s[i]);
  char32_t cp;
  int extra;
  if (c < 0x80) { cp = c; extra = 0; }
  else if ((c >> 5) == 6) { cp = c & 0x1f; extra = 1; }
  else if ((c >> 4) == 14) { cp = c & 0x0f; extra = 2; }
  else if ((c >> 3) == 30) { cp = c & 0x07; extra = 3; }
  else { ++i; return 0xFFFD; }
  ++i;
  for (int k = 0; k < extra && i < s.size(); ++k, ++i) cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3f);
  return cp;
}

bool IsTypographicSpace(char32_t c) { return GeneralCategoryOf(c) == GeneralCategory::Zs && c != 0x3000; }
bool IsWordSeparator(char32_t c) { return c == ' ' || c == 0xA0 || c == 0x1361 || c == 0x10100 || c == 0x10101 || c == 0x1039F || c == 0x1091F; }
bool IsCollapsibleSpace(char32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

// How many bytes of the text are the first letter: 0 when there is none. `skip` is the leading white space, which is not.
size_t FirstLetterLength(const std::string& text, size_t& skip, bool collapses) {
  size_t i = 0;
  skip = 0;
  if (collapses) {
    while (i < text.size() && IsCollapsibleSpace(static_cast<unsigned char>(text[i]))) ++i;
    skip = i;
  }
  const size_t start = i;
  // Punctuation before, and the typographic spaces between.
  while (i < text.size()) {
    size_t j = i;
    const char32_t c = Decode(text, j);
    if (solar::text::IsPunctuation(c) || IsTypographicSpace(c)) i = j;
    else break;
  }
  if (i >= text.size()) return 0;
  // The letter itself: anything that is not a space or a control, with the marks and joined characters that go with it.
  size_t j = i;
  const char32_t letter = Decode(text, j);
  const GeneralCategory g = GeneralCategoryOf(letter);
  if (g == GeneralCategory::Zs || g == GeneralCategory::Zl || g == GeneralCategory::Zp || g == GeneralCategory::Cc || IsCollapsibleSpace(letter)) return 0;
  i = j;
  while (i < text.size()) {
    size_t k = i;
    const char32_t c = Decode(text, k);
    if (solar::text::IsMark(c) || c == 0x200D || (c >= 0xFE00 && c <= 0xFE0F)) {
      i = k;
      if (c == 0x200D && i < text.size()) Decode(text, i);
    } else {
      break;
    }
  }
  // Punctuation after (but not opening punctuation or a dash), with the typographic spaces between.
  size_t end = i;
  size_t pending = i;
  while (pending < text.size()) {
    size_t k = pending;
    const char32_t c = Decode(text, k);
    const GeneralCategory cat = GeneralCategoryOf(c);
    if (solar::text::IsPunctuation(c) && cat != GeneralCategory::Ps && cat != GeneralCategory::Pd) {
      pending = end = k;
    } else if (IsTypographicSpace(c) && !IsWordSeparator(c)) {
      pending = k;
    } else {
      break;
    }
  }
  return end - start + start;  // from the start of the text: the white space before is counted by the caller
}

struct Place {
  Box* parent = nullptr;
  size_t index = 0;
};

bool Container(const Box& b) {
  const Display d = b.style->display;
  return d == Display::Block || d == Display::ListItem || d == Display::FlowRoot || d == Display::InlineBlock || d == Display::TableCell;
}

// The text box with the first letter in it, if the first formatted line starts with one.
bool Find(Box& container, Place& place, bool& stop) {
  for (size_t i = 0; i < container.children.size(); ++i) {
    Box& c = *container.children[i];
    if (c.kind == Box::Kind::Text) {
      size_t skip;
      const bool collapses = c.style->whiteSpaceCollapse == WhiteSpaceCollapse::Collapse || c.style->whiteSpaceCollapse == WhiteSpaceCollapse::PreserveBreaks;
      if (c.text.empty()) continue;
      if (collapses && c.style->whiteSpaceCollapse == WhiteSpaceCollapse::Collapse) {
        size_t k = 0;
        while (k < c.text.size() && IsCollapsibleSpace(static_cast<unsigned char>(c.text[k]))) ++k;
        if (k == c.text.size()) continue;
      }
      (void)skip;
      place = {&container, i};
      return true;
    }
    if (c.kind == Box::Kind::LineBreak) {
      stop = true;
      return false;
    }
    if (c.IsOutOfFlow()) continue;
    if (c.pseudo == "first-letter") {
      stop = true;
      return false;
    }
    if (c.kind == Box::Kind::Inline) {
      if (Find(c, place, stop)) return true;
      if (stop) return false;
      continue;
    }
    // An atomic inline, a replaced box, or a box of another kind: the line starts with that.
    if (c.inlineLevel || c.replaced || c.formControl || !Container(c)) {
      stop = true;
      return false;
    }
    if (c.style->display == Display::InlineBlock) {
      stop = true;
      return false;
    }
    // A block: its first line is the first line.
    if (Find(c, place, stop)) return true;
    stop = true;
    return false;
  }
  return false;
}

void Walk(LayoutContext& lc, Box& box) {
  for (size_t i = 0; i < box.children.size(); ++i) {
    if (box.children[i]->kind == Box::Kind::Block) Walk(lc, *box.children[i]);
  }
  dom::Element* element = box.element();
  if (!element || box.kind != Box::Kind::Block || box.replaced || box.formControl || !Container(box) || box.style->display == Display::TableCell) return;
  if (!solar::css::HasPseudoStyle(element, "first-letter")) return;
  Place place;
  bool stop = false;
  if (!Find(box, place, stop)) return;
  Box& textBox = *place.parent->children[place.index];
  const bool collapses = textBox.style->whiteSpaceCollapse == WhiteSpaceCollapse::Collapse || textBox.style->whiteSpaceCollapse == WhiteSpaceCollapse::PreserveBreaks;
  size_t skip = 0;
  const size_t length = FirstLetterLength(textBox.text, skip, collapses);
  if (length == 0) return;
  std::shared_ptr<const BoxStyle> style = Logicalize(ReadStyle(lc.ctx, element, "first-letter"));
  if (style->display == Display::None) return;
  const std::string letters = textBox.text.substr(skip, length - skip);
  const std::string before = textBox.text.substr(0, skip);
  const std::string rest = textBox.text.substr(length);
  auto pseudo = std::make_unique<Box>();
  const bool floating = style->IsFloating();
  pseudo->kind = floating || style->display != Display::Inline ? Box::Kind::Block : Box::Kind::Inline;
  pseudo->inlineLevel = !floating && (style->display == Display::Inline || style->display == Display::InlineBlock);
  if (floating && style->display == Display::Inline) {
    auto blockified = std::make_shared<BoxStyle>(*style);
    blockified->display = Display::Block;
    style = blockified;
  }
  pseudo->pseudo = "first-letter";
  pseudo->style = style;
  auto letterText = std::make_unique<Box>();
  letterText->kind = Box::Kind::Text;
  letterText->inlineLevel = true;
  letterText->style = AnonymousStyle(*style, Display::Inline);
  letterText->text = letters;
  letterText->node = textBox.node;
  letterText->textOffset = textBox.textOffset + skip;
  pseudo->AddChild(std::move(letterText));
  Box* parent = place.parent;
  pseudo->parent = parent;
  textBox.text = rest;
  textBox.textOffset += length;
  auto at = parent->children.begin() + static_cast<std::ptrdiff_t>(place.index);
  at = parent->children.insert(at, std::move(pseudo));
  if (!before.empty()) {
    auto space = std::make_unique<Box>();
    space->kind = Box::Kind::Text;
    space->inlineLevel = true;
    space->style = textBox.style;
    space->text = before;
    space->node = textBox.node;
    space->textOffset = textBox.textOffset - length;
    space->parent = parent;
    parent->children.insert(at, std::move(space));
  }
}

}  // namespace

void ApplyFirstLetters(LayoutContext& lc, Box& root) {
  if (!solar::css::g_firstLetterSeen) return;
  Walk(lc, root);
}

}  // namespace solar::layout
