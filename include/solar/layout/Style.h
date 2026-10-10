#pragma once

#include <memory>
#include <string>

#include "solar/dom/Node.h"

// The computed style of an element as layout reads it: the values parsed out of the text getComputedStyle gives, in the types the
// algorithms want (lengths in px, keywords as enums).
namespace solar::layout {

struct Length {
  enum class Kind : uint8_t { Auto, Px, Percent, Calc, None, MinContent, MaxContent, FitContent };
  Kind kind = Kind::Auto;
  double value = 0;
  std::string calc;  // a calc() that has a percentage in it, as written

  Length() = default;
  explicit Length(Kind k, double v = 0) : kind(k), value(v) {}
  static Length Px(double v) { return Length(Kind::Px, v); }
  static Length Percent(double v) { return Length(Kind::Percent, v); }
  bool IsAuto() const { return kind == Kind::Auto; }
  bool IsNone() const { return kind == Kind::None; }
  // Whether the value depends on the containing block (a percentage).
  bool IsPercentage() const { return kind == Kind::Percent || kind == Kind::Calc; }
  bool IsKeyword() const { return kind == Kind::MinContent || kind == Kind::MaxContent || kind == Kind::FitContent; }
  bool IsFixed() const { return kind == Kind::Px; }
  // The length in px for a percentage of `basis`; `fallback` for auto, none and the intrinsic keywords.
  double Resolve(double basis, double fallback = 0) const;
};

enum class Display : uint8_t {
  None, Contents, Block, Inline, InlineBlock, ListItem, FlowRoot, InlineFlowRoot, Flex, InlineFlex, Grid, InlineGrid,
  Table, InlineTable, TableRowGroup, TableHeaderGroup, TableFooterGroup, TableRow, TableCell, TableColumn, TableColumnGroup, TableCaption,
  Ruby, RubyText, RubyBase, RubyTextContainer, RubyBaseContainer,
};
enum class Position : uint8_t { Static, Relative, Absolute, Fixed, Sticky };
enum class Float : uint8_t { None, Left, Right, InlineStart, InlineEnd };
enum class Clear : uint8_t { None, Left, Right, Both, InlineStart, InlineEnd };
enum class BoxSizing : uint8_t { ContentBox, BorderBox };
enum class Overflow : uint8_t { Visible, Hidden, Clip, Scroll, Auto };
enum class BorderStyle : uint8_t { None, Hidden, Dotted, Dashed, Solid, Double, Groove, Ridge, Inset, Outset };
enum class TextAlign : uint8_t { Start, End, Left, Right, Center, Justify, MatchParent };
enum class WhiteSpaceCollapse : uint8_t { Collapse, Preserve, PreserveBreaks, PreserveSpaces, BreakSpaces };
enum class VerticalAlign : uint8_t { Baseline, Sub, Super, Top, TextTop, Middle, Bottom, TextBottom, Length };
enum class Direction : uint8_t { Ltr, Rtl };
enum class WritingMode : uint8_t { HorizontalTb, VerticalRl, VerticalLr, SidewaysRl, SidewaysLr };
enum class TextTransform : uint8_t { None, Capitalize, Uppercase, Lowercase };
enum class Visibility : uint8_t { Visible, Hidden, Collapse };
enum class OverflowWrap : uint8_t { Normal, BreakWord, Anywhere };
enum class WordBreak : uint8_t { Normal, BreakAll, KeepAll, BreakWord };

enum class FlexDirection : uint8_t { Row, RowReverse, Column, ColumnReverse };
enum class FlexWrap : uint8_t { Nowrap, Wrap, WrapReverse };

// A value of the box alignment properties (justify-content, align-items, align-self, align-content, justify-self...).
struct Align {
  enum class Kind : uint8_t {
    Auto, Normal, Stretch, Baseline, LastBaseline, Start, End, FlexStart, FlexEnd, SelfStart, SelfEnd, Center, Left, Right, SpaceBetween, SpaceAround, SpaceEvenly,
  };
  Kind kind = Kind::Normal;
  bool safe = false;    // `safe`: not past the start edge
  bool unsafe = false;  // `unsafe`
};

struct BoxStyle {
  Display display = Display::Inline;
  Position position = Position::Static;
  Float floating = Float::None;
  Clear clear = Clear::None;
  BoxSizing boxSizing = BoxSizing::ContentBox;
  Overflow overflowX = Overflow::Visible, overflowY = Overflow::Visible;
  Visibility visibility = Visibility::Visible;
  Direction direction = Direction::Ltr;
  WritingMode writingMode = WritingMode::HorizontalTb;

  Length width, height, minWidth, minHeight, maxWidth{Length::Kind::None}, maxHeight{Length::Kind::None};
  Length margin[4];   // top, right, bottom, left
  Length padding[4];
  Length inset[4];    // top, right, bottom, left
  double border[4] = {0, 0, 0, 0};
  BorderStyle borderStyle[4] = {BorderStyle::None, BorderStyle::None, BorderStyle::None, BorderStyle::None};
  double aspectRatio = 0;  // width / height, 0 for auto
  bool aspectRatioAuto = false;

  // Text.
  double fontSize = 16;
  int fontWeight = 400;
  bool italic = false;
  double fontStretch = 100;
  Length lineHeight;       // Auto: normal; Px: a length or number * font size; the number itself in lineHeightNumber if it was one
  double lineHeightNumber = -1;
  VerticalAlign verticalAlign = VerticalAlign::Baseline;
  Length verticalAlignLength;
  TextAlign textAlign = TextAlign::Start;
  TextAlign textAlignLast = TextAlign::Start;
  bool textAlignLastAuto = true;
  Length textIndent;
  bool textIndentHanging = false, textIndentEachLine = false;
  WhiteSpaceCollapse whiteSpaceCollapse = WhiteSpaceCollapse::Collapse;
  bool wrap = true;         // text-wrap-mode: wrap
  double letterSpacing = 0, wordSpacing = 0;
  TextTransform textTransform = TextTransform::None;
  OverflowWrap overflowWrap = OverflowWrap::Normal;
  WordBreak wordBreak = WordBreak::Normal;
  int tabSize = 8;
  std::string color = "rgb(0, 0, 0)";
  std::string backgroundColor = "rgba(0, 0, 0, 0)";
  std::string fontFamily;
  std::string zIndex = "auto";
  std::string content = "normal";
  // Containment (css-contain): layout and paint containment make a box its own formatting context; size containment lets its contents count for
  // nothing in the sizes it asks for. content-visibility: hidden skips the contents altogether.
  bool containLayout = false, containPaint = false, containSizeInline = false, containSizeBlock = false;
  bool skipContents = false;
  Length containIntrinsicWidth, containIntrinsicHeight;  // px, or none (Auto kind with value 0)
  bool containIntrinsicWidthSet = false, containIntrinsicHeightSet = false;
  // What makes a box the containing block of the positioned boxes inside it (besides being positioned), and a stacking context.
  bool containsPositioned = false;
  bool stackingContext = false;
  double opacity = 1;
  bool pointerEventsNone = false;

  // Flexible boxes (css-flexbox) and box alignment (css-align).
  FlexDirection flexDirection = FlexDirection::Row;
  FlexWrap flexWrap = FlexWrap::Nowrap;
  double flexGrow = 0, flexShrink = 1;
  Length flexBasis;      // Auto: auto; Px/Percent; Calc; MaxContent for content
  bool flexBasisContent = false;
  int order = 0;
  Align justifyContent, alignItems, alignSelf, alignContent, justifyItems, justifySelf;
  // Transforms (css-transforms): the computed values as text.
  std::string transform = "none", transformOrigin, translate = "none", rotate = "none", scale = "none", perspective = "none", perspectiveOrigin;
  bool preserve3d = false;

  // Counters and lists.
  std::string counterReset, counterIncrement, counterSet, listStyleType, quotes;
  bool listStyleInside = false;

  // Tables.
  bool tableLayoutFixed = false, borderCollapse = false, captionBottom = false, emptyCellsHide = false;
  double borderSpacingH = 0, borderSpacingV = 0;
  BorderStyle borderStyleRaw[4] = {BorderStyle::None, BorderStyle::None, BorderStyle::None, BorderStyle::None};  // as specified, before the width is zeroed
  double borderWidthRaw[4] = {0, 0, 0, 0};

  // Grid (css-grid): the computed values as text, read where the grid is laid out.
  std::string gridTemplateColumns, gridTemplateRows, gridTemplateAreas, gridAutoFlow, gridAutoColumns, gridAutoRows;
  std::string gridColumnStart, gridColumnEnd, gridRowStart, gridRowEnd;
  Length rowGap, columnGap;  // Auto: normal (0 in flex, the default in grid)

  // For a style in the logical frame of its writing mode (see Logicalize): the style as written, with the physical properties.
  std::shared_ptr<const BoxStyle> physicalStyle;
  const BoxStyle& Physical() const { return physicalStyle ? *physicalStyle : *this; }

  bool IsOutOfFlow() const { return position == Position::Absolute || position == Position::Fixed; }
  bool IsFloating() const { return floating != Float::None; }
  bool IsBlockLevel() const;
  bool CreatesBlockFormattingContext() const;
};

// The style of the element (or of a pseudo-element of it).
std::shared_ptr<const BoxStyle> ReadStyle(Quanta::Context& ctx, dom::Element* element, const std::string& pseudo = "");

// The writing modes: whether the block axis is horizontal, and the way the lines and blocks run.
inline bool IsVertical(WritingMode m) { return m != WritingMode::HorizontalTb; }
// Which physical side (0 top, 1 right, 2 bottom, 3 left) each logical side (0 block-start, 1 line-right, 2 block-end, 3 line-left) is, in a writing mode.
int PhysicalSideOf(WritingMode mode, int logicalSide);
int LogicalSideOf(WritingMode mode, int physicalSide);

// The style with its physical properties (width, margin-left...) turned into the logical frame of its writing mode, in which x runs
// along the lines, left to right, and y from block-start to block-end: all the layout algorithms are written for that frame. A horizontal
// style comes back as it is.
std::shared_ptr<const BoxStyle> Logicalize(const std::shared_ptr<const BoxStyle>& physical);

// A calc() with percentages, with the percentage of `basis`; nothing if it is not one this can work out.
bool EvaluateCalc(const std::string& text, double basis, double& out);

}  // namespace solar::layout
