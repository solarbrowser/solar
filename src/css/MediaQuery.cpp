#include "solar/css/MediaQuery.h"
#include "solar/css/Fonts.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include "solar/css/Calc.h"
#include "solar/css/Syntax.h"

namespace solar::css {

namespace {

// The initial font at its initial size (16px): what the font-relative units of a media query are of.
FontUnits InitialFontUnits() {
  static const FontUnits units = [] {
    FontUnits u;
    const auto faces = FontsForRequest(nullptr, MakeFontRequest("", 400, 100, false));
    if (faces.empty()) return FontUnits{8, 8, 11.2, 16, 18.4};
    const font::Metrics& m = faces[0]->metrics();
    u.ex = m.xHeight * 16;
    u.cap = m.capHeight * 16;
    u.ch = faces[0]->HasGlyph('0') ? faces[0]->Advance('0') * 16 : 8;
    u.ic = faces[0]->HasGlyph(0x6C34) ? faces[0]->Advance(0x6C34) * 16 : 16;
    u.lineHeight = std::round(m.ascent * 16) + std::round(m.descent * 16) + m.lineGap * 16;
    return u;
  }();
  return units;
}

}  // namespace

namespace {

using T = Token::Type;

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// A media condition evaluates to true, false or unknown (css-mediaqueries-4, "evaluating media queries"): what is not understood is
// unknown, which counts as false at the end but does not turn true by being negated.
enum class Tri { False, True, Unknown };

Tri Not(Tri t) { return t == Tri::Unknown ? Tri::Unknown : (t == Tri::True ? Tri::False : Tri::True); }
Tri And(Tri a, Tri b) {
  if (a == Tri::False || b == Tri::False) return Tri::False;
  if (a == Tri::Unknown || b == Tri::Unknown) return Tri::Unknown;
  return Tri::True;
}
Tri Or(Tri a, Tri b) {
  if (a == Tri::True || b == Tri::True) return Tri::True;
  if (a == Tri::Unknown || b == Tri::Unknown) return Tri::Unknown;
  return Tri::False;
}

enum class FeatureType { None, Length, Ratio, Resolution, Integer, Keyword };

struct FeatureInfo {
  FeatureType type = FeatureType::None;
  bool range = false;                  // may be compared with <, > and min- and max-
  double number = 0;                   // the current value of a numeric feature
  std::string keyword;                 // of a keyword feature
  std::vector<const char*> keywords;   // the ones it can have
};

FeatureInfo InfoOf(const std::string& name, const MediaEnvironment& e) {
  FeatureInfo f;
  const auto numeric = [&](FeatureType type, double value, bool range = true) {
    f.type = type;
    f.number = value;
    f.range = range;
  };
  const auto keyword = [&](const char* current, std::vector<const char*> all) {
    f.type = FeatureType::Keyword;
    f.keyword = current;
    f.keywords = std::move(all);
  };
  if (name == "width") numeric(FeatureType::Length, e.width);
  else if (name == "device-width") numeric(FeatureType::Length, e.deviceWidth);
  else if (name == "height") numeric(FeatureType::Length, e.height);
  else if (name == "device-height") numeric(FeatureType::Length, e.deviceHeight);
  else if (name == "aspect-ratio") numeric(FeatureType::Ratio, e.width / e.height);
  else if (name == "device-aspect-ratio") numeric(FeatureType::Ratio, e.deviceWidth / e.deviceHeight);
  else if (name == "resolution") numeric(FeatureType::Resolution, e.resolution);
  else if (name == "color") numeric(FeatureType::Integer, 8);
  else if (name == "color-index" || name == "monochrome") numeric(FeatureType::Integer, 0);
  else if (name == "grid") numeric(FeatureType::Integer, 0, false);
  else if (name == "scan") keyword("none", {"interlace", "progressive"});
  else if (name == "orientation") keyword(e.width >= e.height ? "landscape" : "portrait", {"portrait", "landscape"});
  else if (name == "hover" || name == "any-hover") keyword("hover", {"none", "hover"});
  else if (name == "pointer" || name == "any-pointer") keyword("fine", {"none", "coarse", "fine"});
  else if (name == "prefers-color-scheme") keyword(e.colorScheme == "dark" ? "dark" : "light", {"light", "dark"});
  else if (name == "prefers-reduced-motion") keyword("no-preference", {"no-preference", "reduce"});
  else if (name == "prefers-contrast") keyword("no-preference", {"no-preference", "more", "less", "custom"});
  else if (name == "prefers-reduced-transparency") keyword("no-preference", {"no-preference", "reduce"});
  else if (name == "prefers-reduced-data") keyword("no-preference", {"no-preference", "reduce"});
  else if (name == "forced-colors") keyword("none", {"none", "active"});
  else if (name == "inverted-colors") keyword("none", {"none", "inverted"});
  else if (name == "scripting") keyword("enabled", {"none", "initial-only", "enabled"});
  else if (name == "update") keyword("fast", {"none", "slow", "fast"});
  else if (name == "overflow-block") keyword("scroll", {"none", "scroll", "paged"});
  else if (name == "overflow-inline") keyword("scroll", {"none", "scroll"});
  else if (name == "display-mode") keyword("browser", {"fullscreen", "standalone", "minimal-ui", "browser", "picture-in-picture", "window-controls-overlay"});
  else if (name == "dynamic-range" || name == "video-dynamic-range") keyword("standard", {"standard", "high"});
  else if (name == "color-gamut") keyword("srgb", {"srgb", "p3", "rec2020"});
  return f;
}

// A <number>, optionally a calculation.
std::optional<double> NumberOf(const ComponentValue& c) {
  if (c.IsToken(T::Number)) return c.token.number;
  if (c.kind == ComponentValue::Kind::Function && Lower(c.name) == "calc") {
    const std::optional<MathValue> m = EvaluateNumeric(c);
    if (m && m->kind == MathKind::Number) return m->value;
  }
  return std::nullopt;
}

// The value of a feature of this type, in px, dppx or as the number it is; nothing if the text is not one.
std::optional<double> ParseTyped(const ComponentValues& values, FeatureType type, const MediaEnvironment& e) {
  std::vector<ComponentValue> v;
  for (const ComponentValue& c : values) {
    if (!c.IsWhitespace()) v.push_back(c);
  }
  if (v.empty()) return std::nullopt;
  switch (type) {
    case FeatureType::Integer:
      if (v.size() == 1 && v[0].IsToken(T::Number) && v[0].token.isInteger) return v[0].token.number;
      return std::nullopt;
    case FeatureType::Ratio: {
      if (v.size() == 1) {
        const std::optional<double> n = NumberOf(v[0]);
        if (n && *n >= 0) return *n;
        return std::nullopt;
      }
      if (v.size() == 3 && v[1].IsDelim('/')) {
        const std::optional<double> a = NumberOf(v[0]), b = NumberOf(v[2]);
        if (!a || !b || *a < 0 || *b < 0) return std::nullopt;
        return *b == 0 ? (*a == 0 ? 0 : INFINITY) : *a / *b;
      }
      return std::nullopt;
    }
    case FeatureType::Length: {
      if (v.size() != 1) return std::nullopt;
      const ComponentValue& c = v[0];
      if (c.IsToken(T::Number)) return c.token.number == 0 ? std::optional<double>(0) : std::nullopt;
      if (c.IsToken(T::Dimension)) {
        const std::string unit = Lower(c.token.value);
        const double n = c.token.number;
        if (unit == "px") return n;
        if (unit == "em" || unit == "rem") return n * 16;
        if (unit == "ex" || unit == "rex") return n * InitialFontUnits().ex;
        if (unit == "ch" || unit == "rch") return n * InitialFontUnits().ch;
        if (unit == "cap" || unit == "rcap") return n * InitialFontUnits().cap;
        if (unit == "ic" || unit == "ric") return n * InitialFontUnits().ic;
        if (unit == "lh" || unit == "rlh") return n * InitialFontUnits().lineHeight;
        if (unit == "in") return n * 96;
        if (unit == "cm") return n * 96 / 2.54;
        if (unit == "mm") return n * 96 / 25.4;
        if (unit == "q") return n * 96 / 101.6;
        if (unit == "pt") return n * 96 / 72;
        if (unit == "pc") return n * 16;
        if (unit == "vw" || unit == "svw" || unit == "lvw" || unit == "dvw" || unit == "vi") return n * e.width / 100;
        if (unit == "vh" || unit == "svh" || unit == "lvh" || unit == "dvh" || unit == "vb") return n * e.height / 100;
        if (unit == "vmin") return n * std::min(e.width, e.height) / 100;
        if (unit == "vmax") return n * std::max(e.width, e.height) / 100;
        return std::nullopt;
      }
      if (c.kind == ComponentValue::Kind::Function && Lower(c.name) == "calc") {
        const std::optional<MathValue> m = EvaluateNumeric(c);
        if (m && m->kind == MathKind::Length) return m->value;
      }
      return std::nullopt;
    }
    case FeatureType::Resolution: {
      if (v.size() != 1) return std::nullopt;
      const ComponentValue& c = v[0];
      if (c.IsToken(T::Dimension)) {
        const std::string unit = Lower(c.token.value);
        const double n = c.token.number;
        if (n < 0) return std::nullopt;
        if (unit == "dppx" || unit == "x") return n;
        if (unit == "dpi") return n / 96;
        if (unit == "dpcm") return n * 2.54 / 96;
        return std::nullopt;
      }
      if (c.kind == ComponentValue::Kind::Function && Lower(c.name) == "calc") {
        const std::optional<MathValue> m = EvaluateNumeric(c);
        if (m && m->kind == MathKind::Resolution) return m->value;
      }
      return std::nullopt;
    }
    default: return std::nullopt;
  }
}

bool Compare(double a, const std::string& op, double b) {
  if (op == "=") return a == b;
  if (op == "<") return a < b;
  if (op == "<=") return a <= b;
  if (op == ">") return a > b;
  if (op == ">=") return a >= b;
  return false;
}

std::string Flip(const std::string& op) {
  if (op == "<") return ">";
  if (op == "<=") return ">=";
  if (op == ">") return "<";
  if (op == ">=") return "<=";
  return op;
}

// A feature on its own is true unless its value is zero or none.
Tri BooleanContext(const FeatureInfo& f) {
  if (f.type == FeatureType::Keyword) return f.keyword != "none" && f.keyword != "no-preference" ? Tri::True : Tri::False;
  return f.number != 0 ? Tri::True : Tri::False;
}

// The inside of a pair of parentheses taken as a media feature; whatever it is not, it is "general enclosed": unknown.
Tri Feature(const ComponentValues& inner, const MediaEnvironment& e) {
  const ComponentValues raw = Trimmed(inner);
  std::vector<ComponentValue> items;
  for (const ComponentValue& c : raw) {
    if (!c.IsWhitespace()) items.push_back(c);
  }
  if (items.empty()) return Tri::Unknown;
  if (items.size() == 1 && items[0].IsIdent()) {
    const FeatureInfo f = InfoOf(Lower(items[0].token.value), e);
    return f.type == FeatureType::None ? Tri::Unknown : BooleanContext(f);
  }
  // name : value
  if (items.size() >= 3 && items[0].IsIdent() && items[1].IsToken(T::Colon)) {
    std::string name = Lower(items[0].token.value);
    std::string op = "=";
    if (name.starts_with("min-")) {
      op = ">=";
      name = name.substr(4);
    } else if (name.starts_with("max-")) {
      op = "<=";
      name = name.substr(4);
    }
    const FeatureInfo f = InfoOf(name, e);
    if (f.type == FeatureType::None || (op != "=" && !f.range)) return Tri::Unknown;
    const ComponentValues value(items.begin() + 2, items.end());
    if (f.type == FeatureType::Keyword) {
      if (value.size() != 1 || !value[0].IsIdent()) return Tri::Unknown;
      const std::string wanted = Lower(value[0].token.value);
      for (const char* k : f.keywords) {
        if (wanted == k) return f.keyword == wanted ? Tri::True : Tri::False;
      }
      return Tri::Unknown;
    }
    const std::optional<double> wanted = ParseTyped(value, f.type, e);
    if (!wanted) return Tri::Unknown;
    if (name == "grid" && *wanted != 0 && *wanted != 1) return Tri::Unknown;
    return Compare(f.number, op, *wanted) ? Tri::True : Tri::False;
  }
  // Range syntax: the operators are < <= > >= = and the two characters of <= and >= are written together.
  struct Operator {
    size_t at;  // in raw
    std::string text;
  };
  std::vector<Operator> operators;
  for (size_t i = 0; i < raw.size(); ++i) {
    if (raw[i].IsDelim('<') || raw[i].IsDelim('>') || raw[i].IsDelim('=')) {
      std::string text(1, static_cast<char>(raw[i].token.delim));
      if (text != "=" && i + 1 < raw.size() && raw[i + 1].IsDelim('=')) {
        text += "=";
        operators.push_back({i, text});
        ++i;
      } else {
        operators.push_back({i, text});
      }
    }
  }
  if (operators.empty() || operators.size() > 2) return Tri::Unknown;
  const auto section = [&](size_t from, size_t to) {
    ComponentValues out;
    for (size_t i = from; i < to; ++i) {
      if (!raw[i].IsWhitespace()) out.push_back(raw[i]);
    }
    return out;
  };
  const auto endOf = [&](const Operator& o) { return o.at + o.text.size(); };
  const auto nameOf = [&](const ComponentValues& v) -> std::optional<FeatureInfo> {
    if (v.size() != 1 || !v[0].IsIdent()) return std::nullopt;
    const FeatureInfo f = InfoOf(Lower(v[0].token.value), e);
    if (f.type == FeatureType::None || !f.range) return std::nullopt;
    return f;
  };
  if (operators.size() == 1) {
    const ComponentValues left = section(0, operators[0].at), right = section(endOf(operators[0]), raw.size());
    if (left.empty() || right.empty()) return Tri::Unknown;
    if (const std::optional<FeatureInfo> f = nameOf(left)) {
      const std::optional<double> wanted = ParseTyped(right, f->type, e);
      return wanted ? (Compare(f->number, operators[0].text, *wanted) ? Tri::True : Tri::False) : Tri::Unknown;
    }
    if (const std::optional<FeatureInfo> f = nameOf(right)) {
      const std::optional<double> wanted = ParseTyped(left, f->type, e);
      return wanted ? (Compare(f->number, Flip(operators[0].text), *wanted) ? Tri::True : Tri::False) : Tri::Unknown;
    }
    return Tri::Unknown;
  }
  // value op name op value: both go the same way, and neither is =.
  const std::string &a = operators[0].text, &b = operators[1].text;
  const bool less = (a == "<" || a == "<=") && (b == "<" || b == "<=");
  const bool greater = (a == ">" || a == ">=") && (b == ">" || b == ">=");
  if (!less && !greater) return Tri::Unknown;
  const ComponentValues low = section(0, operators[0].at), middle = section(endOf(operators[0]), operators[1].at), high = section(endOf(operators[1]), raw.size());
  const std::optional<FeatureInfo> f = nameOf(middle);
  if (!f) return Tri::Unknown;
  const std::optional<double> first = ParseTyped(low, f->type, e), second = ParseTyped(high, f->type, e);
  if (!first || !second) return Tri::Unknown;
  return Compare(f->number, Flip(a), *first) && Compare(f->number, b, *second) ? Tri::True : Tri::False;
}

std::optional<Tri> Condition(const ComponentValues& values, const MediaEnvironment& e);

std::optional<Tri> InParens(const ComponentValue& v, const MediaEnvironment& e) {
  if (v.IsBlock(T::LeftParen)) {
    if (std::optional<Tri> nested = Condition(v.children, e)) return nested;
    return Feature(v.children, e);
  }
  if (v.kind == ComponentValue::Kind::Function) return Tri::Unknown;  // general-enclosed
  return std::nullopt;
}

std::optional<Tri> Condition(const ComponentValues& values, const MediaEnvironment& e) {
  std::vector<ComponentValue> items;
  for (const ComponentValue& c : values) {
    if (!c.IsWhitespace()) items.push_back(c);
  }
  if (items.empty()) return std::nullopt;
  const auto word = [&](size_t i) { return i < items.size() && items[i].IsIdent() ? Lower(items[i].token.value) : std::string(); };
  if (word(0) == "not") {
    if (items.size() != 2) return std::nullopt;
    const std::optional<Tri> inner = InParens(items[1], e);
    if (!inner) return std::nullopt;
    return Not(*inner);
  }
  std::optional<Tri> result = InParens(items[0], e);
  if (!result) return std::nullopt;
  std::string op;
  for (size_t i = 1; i < items.size(); i += 2) {
    const std::string w = word(i);
    if ((w != "and" && w != "or") || (!op.empty() && op != w) || i + 1 >= items.size()) return std::nullopt;
    op = w;
    const std::optional<Tri> next = InParens(items[i + 1], e);
    if (!next) return std::nullopt;
    result = w == "and" ? And(*result, *next) : Or(*result, *next);
  }
  return result;
}

}  // namespace

const MediaEnvironment& CurrentMediaEnvironment() {
  static const MediaEnvironment environment;
  return environment;
}

bool MediaQueryMatches(const std::string& query, const MediaEnvironment& e) {
  ComponentValues values = Trimmed(ParseComponentValues(query));
  if (values.empty()) return true;
  std::vector<ComponentValue> items;
  for (const ComponentValue& c : values) {
    if (!c.IsWhitespace()) items.push_back(c);
  }
  size_t i = 0;
  bool negated = false;
  if (items[i].IsIdent() && (Lower(items[i].token.value) == "not" || Lower(items[i].token.value) == "only")) {
    negated = Lower(items[i].token.value) == "not";
    ++i;
  }
  bool typeMatches = true;
  bool hasType = false;
  if (i < items.size() && items[i].IsIdent()) {
    const std::string type = Lower(items[i].token.value);
    if (type != "and" && type != "or" && type != "not") {
      hasType = true;
      typeMatches = type == "all" || type == "screen";
      ++i;
    }
  }
  Tri condition = Tri::True;
  if (i < items.size()) {
    ComponentValues rest(items.begin() + i, items.end());
    if (hasType) {
      // type and (cond) and (cond)
      if (!items[i].IsIdent() || Lower(items[i].token.value) != "and") return false;
      rest.erase(rest.begin());
    }
    const std::optional<Tri> parsed = Condition(rest, e);
    if (!parsed) return false;
    condition = *parsed;
  }
  Tri result = And(typeMatches ? Tri::True : Tri::False, condition);
  if (negated) result = Not(result);
  return result == Tri::True;
}

bool MediaListMatches(const std::vector<std::string>& queries, const MediaEnvironment& e) {
  if (queries.empty()) return true;
  for (const std::string& q : queries) {
    if (MediaQueryMatches(q, e)) return true;
  }
  return false;
}

}  // namespace solar::css
