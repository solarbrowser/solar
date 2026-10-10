#include "solar/layout/Style.h"

#include <cmath>
#include <cstdlib>
#include <unordered_map>

#include "solar/css/Calc.h"
#include "solar/css/Style.h"
#include "solar/css/Syntax.h"

namespace solar::layout {

namespace {

using solar::css::ComponentValue;
using solar::css::ComponentValues;

double Number(const std::string& text, double fallback = 0) {
  char* end = nullptr;
  const double v = std::strtod(text.c_str(), &end);
  return end == text.c_str() ? fallback : v;
}

// ---- calc() with percentages ----

struct Calc {
  double px = 0, percent = 0;  // a sum of both
  bool number = false;         // a plain number (no unit)
  bool ok = true;
};

Calc Evaluate(const ComponentValues& values, double basis);

Calc Term(const ComponentValue& v, double basis) {
  Calc c;
  using Type = solar::css::Token::Type;
  if (v.kind == ComponentValue::Kind::Token) {
    const solar::css::Token& t = v.token;
    if (t.type == Type::Percentage) {
      c.percent = t.number;
    } else if (t.type == Type::Dimension) {
      const std::optional<solar::css::MathValue> m = solar::css::EvaluateNumeric(v);
      if (!m) c.ok = false;
      else c.px = m->value;
    } else if (t.type == Type::Number) {
      c.px = t.number;
      c.number = true;
    } else {
      c.ok = false;
    }
    return c;
  }
  if (v.kind == ComponentValue::Kind::Block) return Evaluate(v.children, basis);
  if (v.kind == ComponentValue::Kind::Function) {
    std::string name = v.name;
    for (char& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    // Arguments apart at commas.
    std::vector<Calc> args;
    ComponentValues current;
    const auto flush = [&] {
      args.push_back(Evaluate(current, basis));
      current.clear();
    };
    for (const ComponentValue& child : v.children) {
      if (child.IsToken(Type::Comma)) flush();
      else current.push_back(child);
    }
    flush();
    for (const Calc& a : args) if (!a.ok) { c.ok = false; return c; }
    const auto resolved = [&](const Calc& a) { return a.px + a.percent * basis / 100; };
    if (name == "calc" || name == "-webkit-calc") return args[0];
    if (name == "min" || name == "max") {
      double best = resolved(args[0]);
      for (const Calc& a : args) best = name == "min" ? std::min(best, resolved(a)) : std::max(best, resolved(a));
      c.px = best;
      return c;
    }
    if (name == "clamp" && args.size() == 3) {
      c.px = std::max(resolved(args[0]), std::min(resolved(args[1]), resolved(args[2])));
      return c;
    }
    c.ok = false;
    return c;
  }
  c.ok = false;
  return c;
}

Calc Evaluate(const ComponentValues& values, double basis) {
  // sum of products
  Calc total;
  bool first = true;
  int sign = 1;
  size_t i = 0;
  const auto next = [&]() -> const ComponentValue* {
    while (i < values.size() && values[i].IsWhitespace()) ++i;
    return i < values.size() ? &values[i] : nullptr;
  };
  const auto isDelim = [](const ComponentValue* v, char c) { return v && v->IsDelim(static_cast<char32_t>(c)); };
  for (;;) {
    const ComponentValue* v = next();
    if (!v) break;
    Calc product = Term(*v, basis);
    ++i;
    for (;;) {
      const ComponentValue* op = next();
      if (!(isDelim(op, '*') || isDelim(op, '/'))) break;
      ++i;
      const ComponentValue* rhs = next();
      if (!rhs) { product.ok = false; break; }
      Calc r = Term(*rhs, basis);
      ++i;
      if (!r.ok || !product.ok) { product.ok = false; break; }
      if (isDelim(op, '*')) {
        if (r.number) { product.px *= r.px; product.percent *= r.px; }
        else if (product.number) { const double f = product.px; product = r; product.px *= f; product.percent *= f; }
        else product.ok = false;
      } else {
        if (r.number && r.px != 0) { product.px /= r.px; product.percent /= r.px; }
        else product.ok = false;
      }
    }
    if (!product.ok) { total.ok = false; return total; }
    if (first) { total = product; first = false; }
    else { total.px += sign * product.px; total.percent += sign * product.percent; total.number = total.number && product.number; }
    const ComponentValue* op = next();
    if (!op) break;
    if (isDelim(op, '+')) sign = 1;
    else if (isDelim(op, '-')) sign = -1;
    else { total.ok = false; return total; }
    ++i;
  }
  return total;
}

}  // namespace

bool EvaluateCalc(const std::string& text, double basis, double& out) {
  const ComponentValues values = solar::css::ParseComponentValues(text);
  const ComponentValues trimmed = solar::css::Trimmed(values);
  if (trimmed.size() != 1) return false;
  const Calc c = Term(trimmed[0], basis);
  if (!c.ok) return false;
  out = c.px + c.percent * basis / 100;
  return true;
}

double Length::Resolve(double basis, double fallback) const {
  switch (kind) {
    case Kind::Px: return value;
    case Kind::Percent: return basis * value / 100;
    case Kind::Calc: {
      double v = 0;
      return EvaluateCalc(calc, basis, v) ? v : fallback;
    }
    default: return fallback;
  }
}

bool BoxStyle::IsBlockLevel() const {
  switch (display) {
    case Display::Block: case Display::ListItem: case Display::FlowRoot: case Display::Flex: case Display::Grid: case Display::Table: return true;
    default: return false;
  }
}

bool BoxStyle::CreatesBlockFormattingContext() const {
  if (floating != Float::None || IsOutOfFlow()) return true;
  switch (display) {
    case Display::InlineBlock: case Display::FlowRoot: case Display::TableCell: case Display::TableCaption: case Display::InlineFlex: case Display::Flex:
    case Display::Grid: case Display::InlineGrid: case Display::Table: case Display::InlineTable: return true;
    default: break;
  }
  if (containLayout || containPaint) return true;
  return (overflowX != Overflow::Visible && overflowX != Overflow::Clip) || (overflowY != Overflow::Visible && overflowY != Overflow::Clip);
}

namespace {

Length ParseLength(const std::string& text) {
  Length l;
  if (text.empty() || text == "auto") return l;
  if (text == "none") { l.kind = Length::Kind::None; return l; }
  if (text == "min-content") { l.kind = Length::Kind::MinContent; return l; }
  if (text == "max-content") { l.kind = Length::Kind::MaxContent; return l; }
  if (text == "fit-content") { l.kind = Length::Kind::FitContent; return l; }
  if (text.starts_with("calc(") || text.starts_with("min(") || text.starts_with("max(") || text.starts_with("clamp(")) {
    // Without a percentage in it it is known now.
    if (text.find('%') == std::string::npos) {
      double v = 0;
      if (EvaluateCalc(text, 0, v)) return Length::Px(v);
    }
    l.kind = Length::Kind::Calc;
    l.calc = text;
    return l;
  }
  char* end = nullptr;
  const double v = std::strtod(text.c_str(), &end);
  if (end == text.c_str()) return l;
  const std::string unit = end;
  if (unit == "%") return Length::Percent(v);
  if (unit == "px" || unit.empty()) return Length::Px(v);
  // (Other units are px by the time they are computed; a stray one is read as px.)
  return Length::Px(v);
}

template <typename T>
T Pick(const std::string& text, std::initializer_list<std::pair<const char*, T>> table, T fallback) {
  for (const auto& [name, value] : table) if (text == name) return value;
  return fallback;
}

}  // namespace

std::shared_ptr<const BoxStyle> ReadStyle(Quanta::Context& ctx, dom::Element* element, const std::string& pseudo) {
  auto style = std::make_shared<BoxStyle>();
  BoxStyle& s = *style;
  const auto get = [&](const char* property) { return solar::css::ComputedValue(ctx, element, property, pseudo); };

  const std::string display = get("display");
  s.display = Pick<Display>(display, {
      {"none", Display::None}, {"contents", Display::Contents}, {"block", Display::Block}, {"flow-root", Display::FlowRoot}, {"inline", Display::Inline},
      {"inline-block", Display::InlineBlock}, {"inline flow-root", Display::InlineBlock}, {"list-item", Display::ListItem}, {"flex", Display::Flex},
      {"inline-flex", Display::InlineFlex}, {"grid", Display::Grid}, {"inline-grid", Display::InlineGrid}, {"table", Display::Table},
      {"inline-table", Display::InlineTable}, {"table-row-group", Display::TableRowGroup}, {"table-header-group", Display::TableHeaderGroup},
      {"table-footer-group", Display::TableFooterGroup}, {"table-row", Display::TableRow}, {"table-cell", Display::TableCell},
      {"table-column", Display::TableColumn}, {"table-column-group", Display::TableColumnGroup}, {"table-caption", Display::TableCaption},
      {"ruby", Display::Ruby}, {"ruby-text", Display::RubyText}, {"ruby-base", Display::RubyBase}, {"ruby-text-container", Display::RubyTextContainer},
      {"ruby-base-container", Display::RubyBaseContainer}, {"block flow", Display::Block}, {"inline flow", Display::Inline}, {"block flow-root", Display::FlowRoot},
      {"block flex", Display::Flex}, {"inline flex", Display::InlineFlex}, {"block grid", Display::Grid}, {"inline grid", Display::InlineGrid},
      {"block table", Display::Table}, {"inline table", Display::InlineTable}, {"list-item block flow", Display::ListItem}, {"block flow list-item", Display::ListItem}}, Display::Inline);
  s.position = Pick<Position>(get("position"), {{"relative", Position::Relative}, {"absolute", Position::Absolute}, {"fixed", Position::Fixed}, {"sticky", Position::Sticky}, {"-webkit-sticky", Position::Sticky}}, Position::Static);
  s.floating = Pick<Float>(get("float"), {{"left", Float::Left}, {"right", Float::Right}, {"inline-start", Float::InlineStart}, {"inline-end", Float::InlineEnd}}, Float::None);
  s.clear = Pick<Clear>(get("clear"), {{"left", Clear::Left}, {"right", Clear::Right}, {"both", Clear::Both}, {"inline-start", Clear::InlineStart}, {"inline-end", Clear::InlineEnd}}, Clear::None);
  s.boxSizing = get("box-sizing") == "border-box" ? BoxSizing::BorderBox : BoxSizing::ContentBox;
  const auto overflow = [](const std::string& v) {
    return Pick<Overflow>(v, {{"hidden", Overflow::Hidden}, {"clip", Overflow::Clip}, {"scroll", Overflow::Scroll}, {"auto", Overflow::Auto}}, Overflow::Visible);
  };
  s.overflowX = overflow(get("overflow-x"));
  s.overflowY = overflow(get("overflow-y"));
  s.visibility = Pick<Visibility>(get("visibility"), {{"hidden", Visibility::Hidden}, {"collapse", Visibility::Collapse}}, Visibility::Visible);
  s.direction = get("direction") == "rtl" ? Direction::Rtl : Direction::Ltr;
  s.writingMode = Pick<WritingMode>(get("writing-mode"), {{"vertical-rl", WritingMode::VerticalRl}, {"vertical-lr", WritingMode::VerticalLr}, {"sideways-rl", WritingMode::SidewaysRl}, {"sideways-lr", WritingMode::SidewaysLr}}, WritingMode::HorizontalTb);

  s.width = ParseLength(get("width"));
  s.height = ParseLength(get("height"));
  s.minWidth = ParseLength(get("min-width"));
  s.minHeight = ParseLength(get("min-height"));
  s.maxWidth = ParseLength(get("max-width"));
  s.maxHeight = ParseLength(get("max-height"));
  static const char* const sides[4] = {"top", "right", "bottom", "left"};
  for (int i = 0; i < 4; ++i) {
    const std::string side = sides[i];
    s.margin[i] = ParseLength(get(("margin-" + side).c_str()));
    s.padding[i] = ParseLength(get(("padding-" + side).c_str()));
    s.inset[i] = ParseLength(get(side.c_str()));
    s.borderStyle[i] = Pick<BorderStyle>(get(("border-" + side + "-style").c_str()), {{"hidden", BorderStyle::Hidden}, {"dotted", BorderStyle::Dotted}, {"dashed", BorderStyle::Dashed}, {"solid", BorderStyle::Solid}, {"double", BorderStyle::Double}, {"groove", BorderStyle::Groove}, {"ridge", BorderStyle::Ridge}, {"inset", BorderStyle::Inset}, {"outset", BorderStyle::Outset}}, BorderStyle::None);
    s.border[i] = (s.borderStyle[i] == BorderStyle::None || s.borderStyle[i] == BorderStyle::Hidden) ? 0 : Number(get(("border-" + side + "-width").c_str()));
  }
  const std::string ratio = get("aspect-ratio");
  if (ratio != "auto" && !ratio.empty()) {
    const size_t slash = ratio.find('/');
    const std::string numerator = ratio.substr(0, slash);
    const double w = Number(numerator, 0), h = slash == std::string::npos ? 1 : Number(ratio.substr(slash + 1), 1);
    if (w > 0 && h > 0) s.aspectRatio = w / h;
    s.aspectRatioAuto = ratio.starts_with("auto");
  }

  s.fontSize = Number(get("font-size"), 16);
  s.fontWeight = static_cast<int>(Number(get("font-weight"), 400));
  s.italic = get("font-style") != "normal";
  s.fontStretch = Number(get("font-width"), Number(get("font-stretch"), 100));
  const std::string lineHeight = get("line-height");
  if (lineHeight == "normal" || lineHeight.empty()) {
    s.lineHeight = Length();
  } else if (lineHeight.find("px") != std::string::npos || lineHeight.find('%') != std::string::npos) {
    s.lineHeight = ParseLength(lineHeight);
    if (s.lineHeight.kind == Length::Kind::Percent) s.lineHeight = Length::Px(s.fontSize * s.lineHeight.value / 100);
  } else {
    s.lineHeightNumber = Number(lineHeight, -1);
    s.lineHeight = s.lineHeightNumber >= 0 ? Length::Px(s.fontSize * s.lineHeightNumber) : Length();
  }
  const std::string va = get("vertical-align");
  s.verticalAlign = Pick<VerticalAlign>(va, {{"baseline", VerticalAlign::Baseline}, {"sub", VerticalAlign::Sub}, {"super", VerticalAlign::Super}, {"top", VerticalAlign::Top}, {"text-top", VerticalAlign::TextTop}, {"middle", VerticalAlign::Middle}, {"bottom", VerticalAlign::Bottom}, {"text-bottom", VerticalAlign::TextBottom}}, VerticalAlign::Length);
  if (s.verticalAlign == VerticalAlign::Length) s.verticalAlignLength = ParseLength(va);
  const auto align = [](const std::string& v) {
    return Pick<TextAlign>(v, {{"end", TextAlign::End}, {"left", TextAlign::Left}, {"right", TextAlign::Right}, {"center", TextAlign::Center}, {"justify", TextAlign::Justify}, {"match-parent", TextAlign::MatchParent}, {"-webkit-left", TextAlign::Left}, {"-webkit-right", TextAlign::Right}, {"-webkit-center", TextAlign::Center}}, TextAlign::Start);
  };
  s.textAlign = align(get("text-align"));
  const std::string last = get("text-align-last");
  s.textAlignLastAuto = last == "auto" || last.empty();
  s.textAlignLast = align(last);
  const std::string indent = get("text-indent");
  s.textIndentHanging = indent.find("hanging") != std::string::npos;
  s.textIndentEachLine = indent.find("each-line") != std::string::npos;
  s.textIndent = ParseLength(indent.substr(0, indent.find(' ')));
  if (s.textIndent.IsAuto()) s.textIndent = Length::Px(0);
  s.whiteSpaceCollapse = Pick<WhiteSpaceCollapse>(get("white-space-collapse"), {{"preserve", WhiteSpaceCollapse::Preserve}, {"preserve-breaks", WhiteSpaceCollapse::PreserveBreaks}, {"preserve-spaces", WhiteSpaceCollapse::PreserveSpaces}, {"break-spaces", WhiteSpaceCollapse::BreakSpaces}}, WhiteSpaceCollapse::Collapse);
  s.wrap = get("text-wrap-mode") != "nowrap";
  s.letterSpacing = get("letter-spacing") == "normal" ? 0 : Number(get("letter-spacing"));
  s.wordSpacing = get("word-spacing") == "normal" ? 0 : Number(get("word-spacing"));
  s.textTransform = Pick<TextTransform>(get("text-transform"), {{"capitalize", TextTransform::Capitalize}, {"uppercase", TextTransform::Uppercase}, {"lowercase", TextTransform::Lowercase}}, TextTransform::None);
  s.overflowWrap = Pick<OverflowWrap>(get("overflow-wrap"), {{"break-word", OverflowWrap::BreakWord}, {"anywhere", OverflowWrap::Anywhere}}, OverflowWrap::Normal);
  s.wordBreak = Pick<WordBreak>(get("word-break"), {{"break-all", WordBreak::BreakAll}, {"keep-all", WordBreak::KeepAll}, {"break-word", WordBreak::BreakWord}}, WordBreak::Normal);
  s.tabSize = static_cast<int>(Number(get("tab-size"), 8));
  s.color = get("color");
  s.backgroundColor = get("background-color");
  s.fontFamily = get("font-family");
  s.zIndex = get("z-index");
  s.content = get("content");
  {
    const std::string contain = get("contain");
    const auto has = [&](const char* word) { return (" " + contain + " ").find(std::string(" ") + word + " ") != std::string::npos; };
    const bool strict = has("strict"), content = has("content");
    s.containLayout = strict || content || has("layout");
    s.containPaint = strict || content || has("paint");
    s.containSizeInline = strict || has("size") || has("inline-size");
    s.containSizeBlock = strict || has("size");
    const std::string type = get("container-type");
    if (type == "size") { s.containLayout = true; s.containSizeInline = s.containSizeBlock = true; }
    else if (type == "inline-size") { s.containLayout = true; s.containSizeInline = true; }
    const auto none = [&](const char* property) { const std::string v = get(property); return v.empty() || v == "none"; };
    const std::string willChange = get("will-change");
    const bool willTransform = willChange.find("transform") != std::string::npos || willChange.find("perspective") != std::string::npos || willChange.find("filter") != std::string::npos;
    s.containsPositioned = !none("transform") || !none("perspective") || !none("filter") || !none("backdrop-filter") || !none("translate") || !none("rotate") || !none("scale") ||
                           willTransform || s.containLayout || s.containPaint;
    s.opacity = Number(get("opacity"), 1);
    s.pointerEventsNone = get("pointer-events") == "none";
    const bool positionedZ = s.position != Position::Static && get("z-index") != "auto";
    s.stackingContext = positionedZ || s.position == Position::Fixed || s.position == Position::Sticky || s.opacity < 1 || s.containsPositioned || get("isolation") == "isolate" ||
                        get("mix-blend-mode") != "normal" || !none("clip-path") || !none("mask-image") || willChange.find("opacity") != std::string::npos;
    const auto intrinsic = [&](const char* property, Length& l, bool& set) {
      std::string v = get(property);
      if (v.rfind("auto ", 0) == 0) v = v.substr(5);
      if (v == "none" || v.empty()) return;
      l = ParseLength(v);
      set = l.kind == Length::Kind::Px;
    };
    intrinsic("contain-intrinsic-width", s.containIntrinsicWidth, s.containIntrinsicWidthSet);
    intrinsic("contain-intrinsic-height", s.containIntrinsicHeight, s.containIntrinsicHeightSet);
    if (get("content-visibility") == "hidden") {
      s.containLayout = s.containPaint = s.containSizeInline = s.containSizeBlock = true;
      s.skipContents = true;
    }
  }
  return style;
}

}  // namespace solar::layout
