#include "solar/css/MediaQuery.h"

#include <cmath>
#include <optional>

#include "solar/css/Calc.h"
#include "solar/css/Syntax.h"

namespace solar::css {

namespace {

using T = Token::Type;

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// A feature value: a number, a length in px, a resolution in dppx, a ratio, or a keyword.
struct Value {
  enum class Kind { None, Number, Length, Resolution, Ratio, Keyword } kind = Kind::None;
  double number = 0;
  std::string keyword;
};

Value ParseValue(const ComponentValues& values) {
  Value out;
  const ComponentValues v = Trimmed(values);
  if (v.empty()) return out;
  if (v.size() == 1 && v[0].IsIdent()) {
    out.kind = Value::Kind::Keyword;
    out.keyword = Lower(v[0].token.value);
    return out;
  }
  // A ratio: a / b.
  size_t slash = v.size();
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i].IsDelim('/')) slash = i;
  }
  if (slash < v.size()) {
    const std::optional<MathValue> a = EvaluateNumeric(Trimmed(ComponentValues(v.begin(), v.begin() + slash)).empty() ? v[0] : Trimmed(ComponentValues(v.begin(), v.begin() + slash))[0]);
    const ComponentValues rest(v.begin() + slash + 1, v.end());
    const ComponentValues rt = Trimmed(rest);
    if (!a || rt.empty()) return out;
    const std::optional<MathValue> b = EvaluateNumeric(rt[0]);
    if (!b) return out;
    out.kind = Value::Kind::Ratio;
    out.number = b->value == 0 ? INFINITY : a->value / b->value;
    return out;
  }
  const ComponentValue* first = nullptr;
  for (const ComponentValue& c : v) {
    if (!c.IsWhitespace()) { first = &c; break; }
  }
  if (!first) return out;
  // em and rem are 16px here.
  if (first->kind == ComponentValue::Kind::Token && first->token.type == T::Dimension) {
    const std::string unit = Lower(first->token.value);
    if (unit == "em" || unit == "rem") {
      out.kind = Value::Kind::Length;
      out.number = first->token.number * 16;
      return out;
    }
  }
  const std::optional<MathValue> m = EvaluateNumeric(*first);
  if (!m) return out;
  switch (m->kind) {
    case MathKind::Number: out.kind = Value::Kind::Number; break;
    case MathKind::Length: out.kind = Value::Kind::Length; break;
    case MathKind::Resolution: out.kind = Value::Kind::Resolution; break;
    default: return out;
  }
  out.number = m->value;
  return out;
}

// The value a feature has here; kind None for a feature this engine does not know.
Value FeatureValue(const std::string& name, const MediaEnvironment& e) {
  Value v;
  const auto number = [&](double n) { v.kind = Value::Kind::Number; v.number = n; };
  const auto length = [&](double n) { v.kind = Value::Kind::Length; v.number = n; };
  const auto keyword = [&](const char* k) { v.kind = Value::Kind::Keyword; v.keyword = k; };
  if (name == "width") length(e.width);
  else if (name == "height") length(e.height);
  else if (name == "device-width") length(e.width);
  else if (name == "device-height") length(e.height);
  else if (name == "aspect-ratio" || name == "device-aspect-ratio") { v.kind = Value::Kind::Ratio; v.number = e.width / e.height; }
  else if (name == "orientation") keyword(e.width >= e.height ? "landscape" : "portrait");
  else if (name == "resolution") { v.kind = Value::Kind::Resolution; v.number = e.resolution; }
  else if (name == "color") number(8);
  else if (name == "color-index") number(0);
  else if (name == "monochrome") number(0);
  else if (name == "grid") number(0);
  else if (name == "hover" || name == "any-hover") keyword("hover");
  else if (name == "pointer" || name == "any-pointer") keyword("fine");
  else if (name == "prefers-color-scheme") keyword(e.colorScheme == "dark" ? "dark" : "light");
  else if (name == "prefers-reduced-motion") keyword("no-preference");
  else if (name == "prefers-contrast") keyword("no-preference");
  else if (name == "prefers-reduced-transparency") keyword("no-preference");
  else if (name == "prefers-reduced-data") keyword("no-preference");
  else if (name == "forced-colors") keyword("none");
  else if (name == "inverted-colors") keyword("none");
  else if (name == "scripting") keyword("enabled");
  else if (name == "update") keyword("fast");
  else if (name == "overflow-block") keyword("scroll");
  else if (name == "overflow-inline") keyword("scroll");
  else if (name == "display-mode") keyword("browser");
  else if (name == "dynamic-range" || name == "video-dynamic-range") keyword("standard");
  else if (name == "color-gamut") keyword("srgb");
  return v;
}

bool IsRangeFeature(const std::string& name) {
  static const char* const names[] = {"width", "height", "aspect-ratio", "resolution", "color", "color-index", "monochrome", "device-width", "device-height", "device-aspect-ratio", "grid"};
  for (const char* n : names) {
    if (name == n) return true;
  }
  return false;
}

// Whether `value` compares to `current` by `op`.
bool Compare(const Value& current, const std::string& op, const Value& wanted) {
  if (current.kind == Value::Kind::Keyword) return wanted.kind == Value::Kind::Keyword && op == "=" && current.keyword == wanted.keyword;
  // A bare zero length is also a number.
  const bool numeric = (current.kind == wanted.kind) || (current.kind == Value::Kind::Length && wanted.kind == Value::Kind::Number && wanted.number == 0);
  if (!numeric) return false;
  const double a = current.number, b = wanted.number;
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

// One (feature) in parentheses.
std::optional<bool> Feature(const ComponentValues& inner, const MediaEnvironment& e) {
  ComponentValues v = Trimmed(inner);
  // (name), (name: value), (min-name: value), (a < name <= b), (name >= value).
  std::vector<ComponentValue> items;
  for (const ComponentValue& c : v) {
    if (!c.IsWhitespace()) items.push_back(c);
  }
  if (items.empty()) return std::nullopt;
  if (items.size() == 1 && items[0].IsIdent()) {
    const std::string name = Lower(items[0].token.value);
    const Value current = FeatureValue(name, e);
    if (current.kind == Value::Kind::None) return false;
    if (current.kind == Value::Kind::Keyword) return current.keyword != "none";
    return current.number != 0;
  }
  // Mapping to comparisons: find operators.
  std::vector<std::pair<size_t, std::string>> operators;
  for (size_t i = 0; i < items.size(); ++i) {
    if (items[i].IsDelim('<') || items[i].IsDelim('>') || items[i].IsDelim('=')) {
      std::string op(1, static_cast<char>(items[i].token.delim));
      if (op != "=" && i + 1 < items.size() && items[i + 1].IsDelim('=')) {
        op += "=";
        operators.emplace_back(i, op);
        ++i;
      } else {
        operators.emplace_back(i, op);
      }
    }
  }
  if (operators.empty()) {
    // name : value
    if (items.size() < 3 || !items[0].IsIdent() || !items[1].IsToken(T::Colon)) return std::nullopt;
    std::string name = Lower(items[0].token.value);
    std::string op = "=";
    if (name.starts_with("min-")) { op = ">="; name = name.substr(4); }
    else if (name.starts_with("max-")) { op = "<="; name = name.substr(4); }
    if (op != "=" && !IsRangeFeature(name)) return false;
    const Value current = FeatureValue(name, e);
    if (current.kind == Value::Kind::None) return false;
    const Value wanted = ParseValue(ComponentValues(items.begin() + 2, items.end()));
    if (wanted.kind == Value::Kind::None) return false;
    // A keyword feature compares to a keyword.
    return Compare(current, op, wanted);
  }
  // Range syntax: [value op] name [op value].
  const auto identAt = [&](size_t i) { return i < items.size() && items[i].IsIdent(); };
  if (operators.size() == 1) {
    const size_t at = operators[0].first;
    const size_t opLen = operators[0].second.size();
    if (at == 1 && identAt(0)) {
      const Value current = FeatureValue(Lower(items[0].token.value), e);
      const Value wanted = ParseValue(ComponentValues(items.begin() + 1 + opLen, items.end()));
      if (current.kind == Value::Kind::None || wanted.kind == Value::Kind::None) return false;
      return Compare(current, operators[0].second, wanted);
    }
    if (at + opLen == items.size() - 1 && identAt(at + opLen)) {
      const Value current = FeatureValue(Lower(items[at + opLen].token.value), e);
      const Value wanted = ParseValue(ComponentValues(items.begin(), items.begin() + at));
      if (current.kind == Value::Kind::None || wanted.kind == Value::Kind::None) return false;
      return Compare(current, Flip(operators[0].second), wanted);
    }
    return std::nullopt;
  }
  if (operators.size() == 2 && identAt(operators[0].first + operators[0].second.size())) {
    const size_t nameAt = operators[0].first + operators[0].second.size();
    const Value current = FeatureValue(Lower(items[nameAt].token.value), e);
    const Value low = ParseValue(ComponentValues(items.begin(), items.begin() + operators[0].first));
    const Value high = ParseValue(ComponentValues(items.begin() + operators[1].first + operators[1].second.size(), items.end()));
    if (current.kind == Value::Kind::None || low.kind == Value::Kind::None || high.kind == Value::Kind::None) return false;
    return Compare(current, Flip(operators[0].second), low) && Compare(current, operators[1].second, high);
  }
  return std::nullopt;
}

std::optional<bool> Condition(const ComponentValues& values, const MediaEnvironment& e);

std::optional<bool> InParens(const ComponentValue& v, const MediaEnvironment& e) {
  if (v.IsBlock(T::LeftParen)) {
    if (std::optional<bool> nested = Condition(v.children, e)) return nested;
    return Feature(v.children, e);
  }
  if (v.kind == ComponentValue::Kind::Function) return false;  // general-enclosed
  return std::nullopt;
}

std::optional<bool> Condition(const ComponentValues& values, const MediaEnvironment& e) {
  std::vector<ComponentValue> items;
  for (const ComponentValue& c : values) {
    if (!c.IsWhitespace()) items.push_back(c);
  }
  if (items.empty()) return std::nullopt;
  const auto word = [&](size_t i) { return i < items.size() && items[i].IsIdent() ? Lower(items[i].token.value) : std::string(); };
  if (word(0) == "not") {
    if (items.size() != 2) return std::nullopt;
    const std::optional<bool> inner = InParens(items[1], e);
    if (!inner) return std::nullopt;
    return !*inner;
  }
  std::optional<bool> result = InParens(items[0], e);
  if (!result) return std::nullopt;
  std::string op;
  for (size_t i = 1; i < items.size(); i += 2) {
    const std::string w = word(i);
    if ((w != "and" && w != "or") || (!op.empty() && op != w) || i + 1 >= items.size()) return std::nullopt;
    op = w;
    const std::optional<bool> next = InParens(items[i + 1], e);
    if (!next) return std::nullopt;
    result = w == "and" ? (*result && *next) : (*result || *next);
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
  std::optional<bool> conditionMatches = true;
  if (i < items.size()) {
    ComponentValues rest(items.begin() + i, items.end());
    if (hasType) {
      // type and (cond) and (cond)
      if (!items[i].IsIdent() || Lower(items[i].token.value) != "and") return false;
      rest.erase(rest.begin());
    }
    conditionMatches = Condition(rest, e);
    if (!conditionMatches) return false;
  }
  const bool matches = typeMatches && *conditionMatches;
  return negated ? !matches : matches;
}

bool MediaListMatches(const std::vector<std::string>& queries, const MediaEnvironment& e) {
  if (queries.empty()) return true;
  for (const std::string& q : queries) {
    if (MediaQueryMatches(q, e)) return true;
  }
  return false;
}

}  // namespace solar::css
