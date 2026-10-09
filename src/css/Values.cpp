#include "solar/css/Values.h"
#include "solar/css/Calc.h"
#include "solar/css/Color.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>

namespace solar::css {

namespace {

using T = Token::Type;

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool IEquals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  }
  return true;
}

}  // namespace

std::string FormatNumber(double number) {
  if (std::isnan(number)) return "NaN";
  if (std::isinf(number)) return number < 0 ? "-infinity" : "infinity";
  if (number == 0) return "0";
  char buffer[400];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), number, std::chars_format::fixed);
  std::string text(buffer, result.ptr);
  if (text.find('.') != std::string::npos) {
    while (text.back() == '0') text.pop_back();
    if (text.back() == '.') text.pop_back();
  }
  return text;
}

// ---- Value definition syntax ----

namespace {

struct SyntaxNode;
using NodePtr = std::shared_ptr<SyntaxNode>;

struct SyntaxNode {
  enum class Kind { Keyword, Type, Property, Comma, Slash, Delim, Seq, AllOf, AnyOf, OneOf, Repeat, NonEmpty, Function, ParenBlock, BracketBlock };
  Kind kind = Kind::Keyword;
  std::string text;      // keyword, type or property name, function name (lowercase)
  std::string original;  // a function's name as the syntax writes it
  char32_t delim = 0;
  std::vector<NodePtr> children;
  bool hasRange = false;
  double min = 0, max = 0;
  int repMin = 1, repMax = 1;  // Repeat; repMax < 0: no limit
  bool commaSeparated = false;
};

class SyntaxParser {
 public:
  explicit SyntaxParser(std::string_view text) : text_(text) {}

  NodePtr Parse() {
    NodePtr node = ParseOneOf();
    SkipSpace();
    if (!node || pos_ != text_.size()) return nullptr;
    return node;
  }

 private:
  void SkipSpace() {
    while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\n' || text_[pos_] == '\t')) ++pos_;
  }
  bool Lookahead(std::string_view s) {
    SkipSpace();
    return text_.compare(pos_, s.size(), s) == 0;
  }
  bool Accept(std::string_view s) {
    if (!Lookahead(s)) return false;
    pos_ += s.size();
    return true;
  }

  static NodePtr Make(SyntaxNode::Kind kind) {
    auto node = std::make_shared<SyntaxNode>();
    node->kind = kind;
    return node;
  }

  NodePtr ParseOneOf() {
    NodePtr first = ParseAnyOf();
    if (!first) return nullptr;
    if (!(Lookahead("|") && !Lookahead("||"))) return first;
    NodePtr node = Make(SyntaxNode::Kind::OneOf);
    node->children.push_back(first);
    while (Lookahead("|") && !Lookahead("||")) {
      ++pos_;
      NodePtr next = ParseAnyOf();
      if (!next) return nullptr;
      node->children.push_back(next);
    }
    return node;
  }

  NodePtr ParseAnyOf() {
    NodePtr first = ParseAllOf();
    if (!first) return nullptr;
    if (!Lookahead("||")) return first;
    NodePtr node = Make(SyntaxNode::Kind::AnyOf);
    node->children.push_back(first);
    while (Accept("||")) {
      NodePtr next = ParseAllOf();
      if (!next) return nullptr;
      node->children.push_back(next);
    }
    return node;
  }

  NodePtr ParseAllOf() {
    NodePtr first = ParseSeq();
    if (!first) return nullptr;
    if (!Lookahead("&&")) return first;
    NodePtr node = Make(SyntaxNode::Kind::AllOf);
    node->children.push_back(first);
    while (Accept("&&")) {
      NodePtr next = ParseSeq();
      if (!next) return nullptr;
      node->children.push_back(next);
    }
    return node;
  }

  bool AtSequenceEnd() {
    SkipSpace();
    if (pos_ >= text_.size()) return true;
    const char c = text_[pos_];
    return c == ']' || c == ')' || c == '|' || (c == '&' && Lookahead("&&")) || (c == '\'' && (Lookahead("']'") || Lookahead("')'")));
  }

  NodePtr ParseSeq() {
    NodePtr node = Make(SyntaxNode::Kind::Seq);
    while (!AtSequenceEnd()) {
      NodePtr term = ParseTerm();
      if (!term) return nullptr;
      node->children.push_back(term);
    }
    if (node->children.empty()) return nullptr;
    if (node->children.size() == 1) return node->children[0];
    return node;
  }

  NodePtr ParseTerm() {
    NodePtr node = ParsePrimary();
    if (!node) return nullptr;
    for (;;) {
      // Multipliers bind to the term, and may follow one another: X#?, X#{2}.
      if (pos_ >= text_.size()) break;
      const char c = text_[pos_];
      int lo, hi;
      bool comma = false;
      if (c == '*') { lo = 0; hi = -1; ++pos_; }
      else if (c == '+') { lo = 1; hi = -1; ++pos_; }
      else if (c == '?') { lo = 0; hi = 1; ++pos_; }
      else if (c == '#') {
        ++pos_;
        comma = true;
        lo = 1;
        hi = -1;
        if (pos_ < text_.size() && text_[pos_] == '{') {
          if (!ParseBraces(lo, hi)) return nullptr;
        }
      } else if (c == '{') {
        if (!ParseBraces(lo, hi)) return nullptr;
      } else if (c == '!') {
        ++pos_;
        NodePtr wrapped = Make(SyntaxNode::Kind::NonEmpty);
        wrapped->children.push_back(node);
        node = wrapped;
        continue;
      } else {
        break;
      }
      NodePtr repeat = Make(SyntaxNode::Kind::Repeat);
      repeat->children.push_back(node);
      repeat->repMin = lo;
      repeat->repMax = hi;
      repeat->commaSeparated = comma;
      node = repeat;
    }
    return node;
  }

  bool ParseBraces(int& lo, int& hi) {
    ++pos_;  // {
    auto number = [&](int& out) {
      size_t start = pos_;
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
      if (start == pos_) return false;
      out = std::atoi(std::string(text_.substr(start, pos_ - start)).c_str());
      return true;
    };
    if (!number(lo)) return false;
    hi = lo;
    if (pos_ < text_.size() && text_[pos_] == ',') {
      ++pos_;
      if (pos_ < text_.size() && text_[pos_] == '}') hi = -1;
      else if (!number(hi)) return false;
    }
    if (pos_ >= text_.size() || text_[pos_] != '}') return false;
    ++pos_;
    return true;
  }

  static bool IsNameChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || (static_cast<unsigned char>(c) >= 0x80); }

  NodePtr ParsePrimary() {
    SkipSpace();
    if (pos_ >= text_.size()) return nullptr;
    const char c = text_[pos_];
    if (c == '[') {
      ++pos_;
      NodePtr inner = ParseOneOf();
      if (!inner || !Accept("]")) return nullptr;
      return inner;
    }
    if (c == '<') return ParseType();
    if (c == ',') { ++pos_; return Make(SyntaxNode::Kind::Comma); }
    if (c == '/') { ++pos_; return Make(SyntaxNode::Kind::Slash); }
    if (c == '(') {
      ++pos_;
      NodePtr inner = ParseOneOf();
      if (!inner || !Accept(")")) return nullptr;
      NodePtr block = Make(SyntaxNode::Kind::ParenBlock);
      block->children.push_back(inner);
      return block;
    }
    if (c == '\'') {
      // A quoted literal: a delimiter, or a [ ] block.
      if (Lookahead("'['")) {
        pos_ += 3;
        NodePtr block = Make(SyntaxNode::Kind::BracketBlock);
        NodePtr inner = AtQuoted("']'") ? Make(SyntaxNode::Kind::Seq) : ParseOneOf();
        if (!inner || !Accept("']'")) return nullptr;
        block->children.push_back(inner);
        return block;
      }
      const size_t close = text_.find('\'', pos_ + 1);
      if (close == std::string_view::npos || close != pos_ + 2) return nullptr;
      NodePtr node = Make(SyntaxNode::Kind::Delim);
      node->delim = static_cast<unsigned char>(text_[pos_ + 1]);
      pos_ += 3;
      return node;
    }
    if (IsNameChar(c)) {
      const size_t start = pos_;
      while (pos_ < text_.size() && IsNameChar(text_[pos_])) ++pos_;
      std::string name(text_.substr(start, pos_ - start));
      if (pos_ < text_.size() && text_[pos_] == '(') {
        ++pos_;
        NodePtr function = Make(SyntaxNode::Kind::Function);
        function->text = Lower(name);
        function->original = name;
        NodePtr inner = Lookahead(")") ? Make(SyntaxNode::Kind::Seq) : ParseOneOf();
        if (!inner || !Accept(")")) return nullptr;
        function->children.push_back(inner);
        return function;
      }
      NodePtr node = Make(SyntaxNode::Kind::Keyword);
      node->text = name;
      return node;
    }
    return nullptr;
  }

  bool AtQuoted(std::string_view s) { return Lookahead(s); }

  NodePtr ParseType() {
    ++pos_;  // <
    NodePtr node;
    if (pos_ < text_.size() && text_[pos_] == '\'') {
      const size_t close = text_.find('\'', pos_ + 1);
      if (close == std::string_view::npos) return nullptr;
      node = Make(SyntaxNode::Kind::Property);
      node->text = std::string(text_.substr(pos_ + 1, close - pos_ - 1));
      pos_ = close + 1;
    } else {
      const size_t start = pos_;
      while (pos_ < text_.size() && text_[pos_] != '>' && text_[pos_] != ' ' && text_[pos_] != '[') ++pos_;
      node = Make(SyntaxNode::Kind::Type);
      node->text = std::string(text_.substr(start, pos_ - start));
    }
    SkipSpace();
    if (pos_ < text_.size() && text_[pos_] == '[') {
      const size_t close = text_.find(']', pos_);
      if (close == std::string_view::npos) return nullptr;
      const std::string range(text_.substr(pos_ + 1, close - pos_ - 1));
      const size_t comma = range.find(',');
      if (comma == std::string::npos) return nullptr;
      const auto bound = [](std::string s) {
        s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
        if (s == "∞" || s == "+∞") return HUGE_VAL;
        if (s == "-∞") return -HUGE_VAL;
        return std::atof(s.c_str());
      };
      node->hasRange = true;
      node->min = bound(range.substr(0, comma));
      node->max = bound(range.substr(comma + 1));
      pos_ = close + 1;
      SkipSpace();
    }
    if (pos_ >= text_.size() || text_[pos_] != '>') return nullptr;
    ++pos_;
    return node;
  }

  std::string_view text_;
  size_t pos_ = 0;
};

NodePtr SyntaxOf(std::string_view syntax) {
  static std::unordered_map<std::string, NodePtr> cache;
  const std::string key(syntax);
  auto found = cache.find(key);
  if (found != cache.end()) return found->second;
  NodePtr node = SyntaxParser(syntax).Parse();
  cache.emplace(key, node);
  return node;
}

}  // namespace

// ---- Units and numeric types ----

namespace {

enum class Cat { None, Number, Length, Angle, Time, Frequency, Resolution, Flex, OtherDimension };

Cat CategoryOfUnit(std::string_view unit) {
  static const std::unordered_map<std::string, Cat> units = [] {
    std::unordered_map<std::string, Cat> map;
    for (const char* u : {"em", "ex", "cap", "ch", "ic", "rem", "rex", "rcap", "rch", "ric", "lh", "rlh", "vw", "vh", "vi", "vb", "vmin", "vmax", "svw", "svh",
                          "svi", "svb", "svmin", "svmax", "lvw", "lvh", "lvi", "lvb", "lvmin", "lvmax", "dvw", "dvh", "dvi", "dvb", "dvmin", "dvmax", "cqw",
                          "cqh", "cqi", "cqb", "cqmin", "cqmax", "cm", "mm", "q", "in", "pt", "pc", "px"})
      map[u] = Cat::Length;
    for (const char* u : {"deg", "grad", "rad", "turn"}) map[u] = Cat::Angle;
    for (const char* u : {"s", "ms"}) map[u] = Cat::Time;
    for (const char* u : {"hz", "khz"}) map[u] = Cat::Frequency;
    for (const char* u : {"dpi", "dpcm", "dppx", "x"}) map[u] = Cat::Resolution;
    map["fr"] = Cat::Flex;
    return map;
  }();
  const auto found = units.find(Lower(unit));
  return found == units.end() ? Cat::OtherDimension : found->second;
}

// What a numeric expression is: a number, a dimension of some kind, a percentage, or a dimension that may also be a
// percentage (what calc(1px + 5%) is).
struct NumType {
  Cat cat = Cat::None;
  bool percent = false;  // with Cat::Number: a percentage; with another: the dimension may be a percentage too
  bool operator==(const NumType& o) const { return cat == o.cat && percent == o.percent; }
};

bool IsPureNumber(const NumType& t) { return t.cat == Cat::Number && !t.percent; }

bool IsMathFunction(const std::string& lowerName) {
  static const char* const names[] = {"calc", "min", "max", "clamp", "round", "mod", "rem", "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "pow", "sqrt",
                                      "hypot", "log", "exp", "abs", "sign", "-webkit-calc", "-moz-calc"};
  for (const char* n : names) {
    if (lowerName == n) return true;
  }
  return false;
}

std::optional<NumType> TypeOfMath(const ComponentValue& function);

// The type of one operand: a number, percentage or dimension token, a keyword constant, a parenthesized sum, a math function.
std::optional<NumType> TypeOfOperand(const ComponentValue& v) {
  if (v.kind == ComponentValue::Kind::Token) {
    switch (v.token.type) {
      case T::Number: return NumType{Cat::Number, false};
      case T::Percentage: return NumType{Cat::Number, true};
      case T::Dimension: {
        const Cat cat = CategoryOfUnit(v.token.value);
        if (cat == Cat::OtherDimension) return std::nullopt;
        return NumType{cat, false};
      }
      case T::Ident: {
        const std::string name = Lower(v.token.value);
        if (name == "e" || name == "pi" || name == "infinity" || name == "-infinity" || name == "nan") return NumType{Cat::Number, false};
        return std::nullopt;
      }
      default: return std::nullopt;
    }
  }
  if (v.kind == ComponentValue::Kind::Function) return IsMathFunction(Lower(v.name)) ? TypeOfMath(v) : std::nullopt;
  if (v.IsBlock(T::LeftParen)) {
    ComponentValue wrapper;
    wrapper.kind = ComponentValue::Kind::Function;
    wrapper.name = "calc";
    wrapper.children = v.children;
    return TypeOfMath(wrapper);
  }
  return std::nullopt;
}

std::optional<NumType> Add(const std::optional<NumType>& a, const std::optional<NumType>& b) {
  if (!a || !b) return std::nullopt;
  if (a->cat == b->cat) {
    if (a->cat == Cat::Number && a->percent != b->percent) return std::nullopt;
    return NumType{a->cat, a->percent || b->percent};
  }
  // A percentage beside a dimension is that dimension that may be a percentage.
  if (a->cat == Cat::Number && a->percent && b->cat != Cat::Number) return NumType{b->cat, true};
  if (b->cat == Cat::Number && b->percent && a->cat != Cat::Number) return NumType{a->cat, true};
  return std::nullopt;
}

std::optional<NumType> Multiply(const std::optional<NumType>& a, const std::optional<NumType>& b) {
  if (!a || !b) return std::nullopt;
  if (IsPureNumber(*a)) return b;
  if (IsPureNumber(*b)) return a;
  return std::nullopt;
}

class MathParser {
 public:
  explicit MathParser(const ComponentValues& values) : values_(values) {}

  std::optional<NumType> ParseSum() {
    std::optional<NumType> left = ParseProduct();
    for (;;) {
      const size_t mark = pos_;
      const bool before = SkipSpace();
      if (pos_ >= values_.size() || !(values_[pos_].IsDelim('+') || values_[pos_].IsDelim('-'))) {
        pos_ = mark;
        return left;
      }
      ++pos_;
      const bool after = SkipSpace();
      if (!before || !after) return std::nullopt;  // + and - need whitespace on both sides
      left = Add(left, ParseProduct());
      if (!left) return std::nullopt;
    }
  }

  bool AtEnd() {
    SkipSpace();
    return pos_ >= values_.size();
  }

  bool SkipSpace() {
    bool any = false;
    while (pos_ < values_.size() && values_[pos_].IsWhitespace()) {
      ++pos_;
      any = true;
    }
    return any;
  }

  bool AcceptComma() {
    SkipSpace();
    if (pos_ < values_.size() && values_[pos_].IsToken(T::Comma)) {
      ++pos_;
      return true;
    }
    return false;
  }

  const ComponentValue* PeekValue() {
    SkipSpace();
    return pos_ < values_.size() ? &values_[pos_] : nullptr;
  }
  void Skip() { ++pos_; }

 private:
  std::optional<NumType> ParseProduct() {
    std::optional<NumType> left = ParseValue();
    for (;;) {
      const size_t mark = pos_;
      SkipSpace();
      if (pos_ < values_.size() && (values_[pos_].IsDelim('*') || values_[pos_].IsDelim('/'))) {
        const bool divide = values_[pos_].IsDelim('/');
        ++pos_;
        SkipSpace();
        std::optional<NumType> right = ParseValue();
        if (!left || !right) return std::nullopt;
        if (divide) {
          if (!IsPureNumber(*right)) return std::nullopt;
        } else {
          left = Multiply(left, right);
          if (!left) return std::nullopt;
        }
      } else {
        pos_ = mark;
        return left;
      }
    }
  }

  std::optional<NumType> ParseValue() {
    SkipSpace();
    if (pos_ >= values_.size()) return std::nullopt;
    const ComponentValue& v = values_[pos_++];
    return TypeOfOperand(v);
  }

  const ComponentValues& values_;
  size_t pos_ = 0;
};

// The comma-separated arguments of a math function, each a sum.
bool ParseArguments(const ComponentValues& children, std::vector<std::optional<NumType>>& types, std::vector<std::string>* keywords = nullptr) {
  MathParser parser(children);
  for (;;) {
    const ComponentValue* next = parser.PeekValue();
    if (!next) return false;
    if (keywords && next->IsIdent()) {
      const std::string word = Lower(next->token.value);
      if (word == "none" || word == "nearest" || word == "up" || word == "down" || word == "to-zero" || word == "line-width") {
        keywords->push_back(word);
        parser.Skip();
        types.push_back(std::nullopt);
        if (parser.AcceptComma()) continue;
        return parser.AtEnd();
      }
    }
    std::optional<NumType> type = parser.ParseSum();
    if (!type) return false;
    types.push_back(type);
    if (parser.AcceptComma()) continue;
    return parser.AtEnd();
  }
}

std::optional<NumType> TypeOfMath(const ComponentValue& function) {
  const std::string name = Lower(function.name);
  const ComponentValues& children = function.children;
  if (name == "calc" || name == "-webkit-calc" || name == "-moz-calc") {
    MathParser parser(children);
    std::optional<NumType> type = parser.ParseSum();
    if (!type || !parser.AtEnd()) return std::nullopt;
    return type;
  }
  std::vector<std::optional<NumType>> types;
  std::vector<std::string> keywords;
  if (!ParseArguments(children, types, &keywords)) return std::nullopt;
  const auto args = [&] {
    std::vector<NumType> list;
    for (const auto& t : types) {
      if (t) list.push_back(*t);
    }
    return list;
  };
  const auto same = [&](const std::vector<NumType>& list) -> std::optional<NumType> {
    if (list.empty()) return std::nullopt;
    std::optional<NumType> result = list[0];
    for (size_t i = 1; i < list.size(); ++i) result = Add(result, list[i]);
    return result;
  };
  const std::vector<NumType> list = args();
  if (name == "min" || name == "max" || name == "hypot") return same(list);
  if (name == "clamp") {
    // The middle one is not optional.
    if (types.size() != 3 || !types[1]) return std::nullopt;
    return same(list);
  }
  if (name == "round") {
    const size_t strategy = keywords.size();
    if (strategy > 1 || (strategy == 1 && (types.empty() || types[0]))) return std::nullopt;
    const size_t operands = list.size();
    if (operands < 1 || operands > 2) return std::nullopt;
    // Without B, A has to be a number.
    if (operands == 1 && !IsPureNumber(list[0])) return std::nullopt;
    return same(list);
  }
  if (name == "mod" || name == "rem") return list.size() == 2 ? same(list) : std::nullopt;
  if (name == "abs") return list.size() == 1 ? std::optional<NumType>(list[0]) : std::nullopt;
  if (name == "sign") return list.size() == 1 ? std::optional<NumType>(NumType{Cat::Number, false}) : std::nullopt;
  if (name == "sin" || name == "cos" || name == "tan") {
    if (list.size() != 1 || !(IsPureNumber(list[0]) || (list[0].cat == Cat::Angle && !list[0].percent))) return std::nullopt;
    return NumType{Cat::Number, false};
  }
  if (name == "asin" || name == "acos" || name == "atan") {
    if (list.size() != 1 || !IsPureNumber(list[0])) return std::nullopt;
    return NumType{Cat::Angle, false};
  }
  if (name == "atan2") {
    if (list.size() != 2 || !same(list)) return std::nullopt;
    return NumType{Cat::Angle, false};
  }
  if (name == "pow" || name == "log" || name == "sqrt" || name == "exp") {
    for (const NumType& t : list) {
      if (!IsPureNumber(t)) return std::nullopt;
    }
    const size_t expected = name == "pow" ? 2 : (name == "log" ? 0 : 1);
    if (name == "log" ? (list.size() < 1 || list.size() > 2) : list.size() != expected) return std::nullopt;
    return NumType{Cat::Number, false};
  }
  return std::nullopt;
}

std::optional<NumType> TypeOfComponent(const ComponentValue& v) {
  if (v.kind == ComponentValue::Kind::Function) return IsMathFunction(Lower(v.name)) ? TypeOfMath(v) : std::nullopt;
  return TypeOfOperand(v);
}

bool InRange(const SyntaxNode& node, const ComponentValue& v) {
  if (!node.hasRange || v.kind != ComponentValue::Kind::Token) return true;
  const double value = v.token.number;
  return value >= node.min && value <= node.max;
}

bool IsHexColorToken(const ComponentValue& v) {
  if (!v.IsToken(T::Hash)) return false;
  const std::string& s = v.token.value;
  if (!(s.size() == 3 || s.size() == 4 || s.size() == 6 || s.size() == 8)) return false;
  return std::all_of(s.begin(), s.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
}

bool IsCssWide(const std::string& lower) {
  return lower == "initial" || lower == "inherit" || lower == "unset" || lower == "revert" || lower == "revert-layer";
}

}  // namespace

bool IsValidMathFunction(const ComponentValue& function) { return TypeOfComponent(function).has_value(); }

bool IsCssWideKeyword(const ComponentValues& values) {
  const ComponentValues trimmed = Trimmed(values);
  return trimmed.size() == 1 && trimmed[0].IsIdent() && IsCssWide(Lower(trimmed[0].token.value));
}

bool ParseAttrCall(const ComponentValue& v, AttrCall& out) {
  if (v.kind != ComponentValue::Kind::Function || Lower(v.name) != "attr") return false;
  std::vector<ComponentValue> items;
  size_t comma = v.children.size();
  for (size_t i = 0; i < v.children.size(); ++i) {
    if (v.children[i].IsToken(T::Comma)) {
      comma = i;
      break;
    }
    if (!v.children[i].IsWhitespace()) items.push_back(v.children[i]);
  }
  // Name: ident, |ident, ns|ident, *|ident; the parts are adjacent so look at the written text.
  std::string nameText;
  size_t n = 0;
  while (n < v.children.size() && v.children[n].IsWhitespace()) ++n;
  for (; n < v.children.size() && n < comma; ++n) {
    if (v.children[n].IsWhitespace()) break;
    const ComponentValue& c = v.children[n];
    if (c.IsIdent()) nameText += c.token.value;
    else if (c.IsDelim('|')) nameText += "|";
    else if (c.IsDelim('*')) nameText += "*";
    else return false;
  }
  if (nameText.empty()) return false;
  const size_t bar = nameText.find('|');
  if (bar == std::string::npos) {
    out.name = nameText;
  } else {
    out.name = nameText.substr(bar + 1);
    const std::string prefix = nameText.substr(0, bar);
    if (prefix == "*") out.anyNamespace = true;
    else if (!prefix.empty()) out.hasNamespace = true, out.prefix = prefix;
  }
  if (out.name.empty() || out.name.find('|') != std::string::npos || out.name.find('*') != std::string::npos) return false;
  while (n < comma && v.children[n].IsWhitespace()) ++n;
  if (n < comma) {
    const ComponentValue& t = v.children[n];
    size_t end = n + 1;
    if (t.kind == ComponentValue::Kind::Function && Lower(t.name) == "type") {
      out.type = AttrCall::Type::Syntax;
      out.syntax = Serialize(Trimmed(t.children));
      if (out.syntax.empty()) return false;
    } else if (t.IsIdent()) {
      const std::string word = Lower(t.token.value);
      static const std::set<std::string> units = {"em", "ex", "cap", "ch", "ic", "lh", "rem", "rex", "rcap", "rch", "ric", "rlh", "vw", "vh", "vi", "vb", "vmin", "vmax", "svw", "svh", "lvw", "lvh", "dvw", "dvh", "cqw", "cqh", "cqi", "cqb", "cqmin", "cqmax", "cm", "mm", "q", "in", "pt", "pc", "px", "deg", "grad", "rad", "turn", "s", "ms", "hz", "khz", "dpi", "dpcm", "dppx", "x", "fr"};
      if (word == "raw-string") out.type = AttrCall::Type::Raw;
      else if (word == "number") out.type = AttrCall::Type::Number;
      else if (units.count(word)) out.type = AttrCall::Type::Unit, out.unit = t.token.value;
      else return false;
    } else if (t.IsDelim('%')) {
      out.type = AttrCall::Type::Unit;
      out.unit = "%";
    } else if (t.IsToken(T::Percentage)) {
      return false;
    } else {
      return false;
    }
    out.explicitType = true;
    while (end < comma && v.children[end].IsWhitespace()) ++end;
    if (end < comma) return false;
  }
  if (comma < v.children.size()) {
    out.hasFallback = true;
    out.fallback.assign(v.children.begin() + comma + 1, v.children.end());
  }
  return true;
}

bool ValidSubstitutions(const ComponentValues& values) {
  for (const ComponentValue& v : values) {
    if (v.kind == ComponentValue::Kind::Function && Lower(v.name) == "attr") {
      AttrCall call;
      bool hasVar = false;
      for (const ComponentValue& c : v.children) hasVar = hasVar || (c.kind == ComponentValue::Kind::Function && Lower(c.name) == "var");
      if (hasVar) {
        if (!ValidSubstitutions(v.children)) return false;
        continue;  // the name is known only once the var()s are substituted
      }
      if (!ParseAttrCall(v, call)) return false;
      if (call.hasFallback && !ValidSubstitutions(call.fallback)) return false;
      continue;
    }
    if (v.kind == ComponentValue::Kind::Function && Lower(v.name) == "var") {
      // var( --name [, fallback]? ): after the name there is the end or a comma.
      size_t i = 0;
      while (i < v.children.size() && v.children[i].IsWhitespace()) ++i;
      if (i >= v.children.size() || !v.children[i].IsIdent() || !v.children[i].token.value.starts_with("--") || v.children[i].token.value.size() < 3) return false;
      ++i;
      while (i < v.children.size() && v.children[i].IsWhitespace()) ++i;
      if (i < v.children.size() && !v.children[i].IsToken(T::Comma)) return false;
    }
    if (v.kind != ComponentValue::Kind::Token && !ValidSubstitutions(v.children)) return false;
  }
  return true;
}

bool ContainsSubstitution(const ComponentValues& values) {
  for (const ComponentValue& v : values) {
    if (v.kind == ComponentValue::Kind::Function) {
      const std::string name = Lower(v.name);
      if (name == "var" || name == "env" || name == "attr") return true;
    }
    if (v.kind != ComponentValue::Kind::Token && ContainsSubstitution(v.children)) return true;
  }
  return false;
}

// ---- The matcher ----

namespace {

namespace {

// A dimension token in its canonical unit, with the relative lengths worked out against the context.
ComponentValue Canonical(const ComponentValue& v, const ComputeContext& c, bool& changed) {
  ComponentValue out = v;
  if (v.kind != ComponentValue::Kind::Token || v.token.type != T::Dimension) return out;
  const std::string unit = Lower(v.token.value);
  double px = -1;
  const double n = v.token.number;
  if (unit == "em") px = n * c.fontSize;
  else if (unit == "rem") px = n * c.rootFontSize;
  else if (unit == "ex" || unit == "ch") px = n * c.fontSize * 0.5;
  else if (unit == "cap") px = n * c.fontSize * 0.7;
  else if (unit == "ic") px = n * c.fontSize;
  else if (unit == "rex" || unit == "rch") px = n * c.rootFontSize * 0.5;
  else if (unit == "rcap") px = n * c.rootFontSize * 0.7;
  else if (unit == "ric") px = n * c.rootFontSize;
  else if (unit == "lh") px = n * c.lineHeight;
  else if (unit == "rlh") px = n * c.rootLineHeight;
  else {
    // viewport units: 1% of the viewport, whichever flavor (small, large, dynamic), and container units as the viewport's
    std::string base = unit;
    if (base.size() > 2 && (base[0] == 's' || base[0] == 'l' || base[0] == 'd') && base[1] == 'v') base = base.substr(1);
    if (base.size() > 2 && base.starts_with("cq")) base = "v" + base.substr(2);
    const double w = c.viewportWidth / 100, h = c.viewportHeight / 100;
    if (base == "vw" || base == "vi") px = n * w;
    else if (base == "vh" || base == "vb") px = n * h;
    else if (base == "vmin") px = n * std::min(w, h);
    else if (base == "vmax") px = n * std::max(w, h);
    // cqw and cqh stand for the width and the height, cqi and cqb for the inline and the block axis
    else if (base == "vw") px = n * w;
  }
  if (px >= 0 || (px == -1 && false)) {
    out.token.number = px;
    out.token.value = "px";
    changed = true;
    return out;
  }
  if (const std::optional<MathValue> m = EvaluateNumeric(v)) {
    out.token.number = m->value;
    switch (m->kind) {
      case MathKind::Length: out.token.value = "px"; break;
      case MathKind::Angle: out.token.value = "deg"; break;
      case MathKind::Time: out.token.value = "s"; break;
      case MathKind::Frequency: out.token.value = "hz"; break;
      case MathKind::Resolution: out.token.value = "dppx"; break;
      default: break;
    }
    changed = true;
  }
  return out;
}

ComponentValue CanonicalTree(const ComponentValue& v, const ComputeContext& c) {
  if (v.kind == ComponentValue::Kind::Token) {
    bool changed = false;
    return Canonical(v, c, changed);
  }
  ComponentValue copy = v;
  for (ComponentValue& child : copy.children) child = CanonicalTree(child, c);
  return copy;
}

}  // namespace


// A <position> in its canonical form: two values (an x and a y), or the edge-and-offset forms as x then y.
bool IsHorizontalKeyword(const ComponentValue& v) {
  if (!v.IsIdent()) return false;
  const std::string w = Lower(v.token.value);
  return w == "left" || w == "right" || w == "x-start" || w == "x-end";
}
bool IsVerticalKeyword(const ComponentValue& v) {
  if (!v.IsIdent()) return false;
  const std::string w = Lower(v.token.value);
  return w == "top" || w == "bottom" || w == "y-start" || w == "y-end";
}
bool IsCenterKeyword(const ComponentValue& v) { return v.IsIdent() && Lower(v.token.value) == "center"; }

ComponentValues CanonicalPosition(const ComponentValues& in) {
  ComponentValues v;
  for (const ComponentValue& c : in) {
    if (!c.IsWhitespace()) v.push_back(c);
  }
  ComponentValue center;
  center.token.type = T::Ident;
  center.token.value = "center";
  // Already made one by a part of the syntax.
  if (v.size() == 1 && v[0].kind == ComponentValue::Kind::Function && v[0].name == "\x01") return v[0].children;
  if (v.size() == 1) {
    if (IsVerticalKeyword(v[0])) return {center, v[0]};
    return {v[0], center};
  }
  if (v.size() == 2) {
    if (IsVerticalKeyword(v[0]) || IsHorizontalKeyword(v[1])) return {v[1], v[0]};
    return v;
  }
  // Edge and offset pairs: the horizontal ones first.
  size_t verticalAt = v.size();
  for (size_t i = 0; i < v.size(); ++i) {
    if (IsVerticalKeyword(v[i])) { verticalAt = i; break; }
  }
  size_t horizontalAt = v.size();
  for (size_t i = 0; i < v.size(); ++i) {
    if (IsHorizontalKeyword(v[i])) { horizontalAt = i; break; }
  }
  if (verticalAt < horizontalAt && horizontalAt < v.size()) {
    // top 10px left 20px -> left 20px top 10px
    ComponentValues out(v.begin() + horizontalAt, v.end());
    out.insert(out.end(), v.begin() + verticalAt, v.begin() + horizontalAt);
    return out;
  }
  return v;
}

// Properties that keep a combination in the order it was written.
thread_local bool g_keepWrittenOrder = false;
thread_local bool g_numberZero = false;  // border-image-*: a bare 0 is a number, not a length

class Matcher {
 public:
  using K = std::function<bool(size_t)>;

  explicit Matcher(const ComponentValues& values, const ComputeContext* compute = nullptr) : compute_(compute) {
    for (const ComponentValue& v : values) {
      if (!v.IsWhitespace()) items_.push_back(v);
    }
    out_ = items_;
  }

  bool Run(const SyntaxNode& root) {
    return Match(root, 0, [&](size_t p) { return p == items_.size(); });
  }

  const std::vector<ComponentValue>& Output() const { return out_; }
  const std::vector<ValueMatch::Assignment>& Assigned() const { return assigned_; }

 private:
  bool Match(const SyntaxNode& node, size_t pos, const K& k) {
    if (++steps_ > kBudget) return false;
    using Kind = SyntaxNode::Kind;
    switch (node.kind) {
      case Kind::Keyword: {
        if (pos >= items_.size() || !items_[pos].IsIdent() || !IEquals(items_[pos].token.value, node.text)) return false;
        const size_t mark = log_.size();
        ComponentValue lowered = items_[pos];
        lowered.token.value = Lower(lowered.token.value);
        Set(pos, lowered);
        if (k(pos + 1)) return true;
        Rollback(mark);
        return false;
      }
      case Kind::Type: return MatchType(node, pos, k);
      case Kind::Property: {
        const PropertyDefinition* property = FindProperty(node.text);
        if (!property) return false;
        NodePtr syntax = SyntaxOf(property->syntax);
        if (!syntax) return false;
        return Match(*syntax, pos, [&, pos](size_t end) {
          assigned_.push_back({node.text, pos, end});
          if (k(end)) return true;
          assigned_.pop_back();
          return false;
        });
      }
      case Kind::Comma: return pos < items_.size() && items_[pos].IsToken(T::Comma) && k(pos + 1);
      case Kind::Slash: return pos < items_.size() && items_[pos].IsDelim('/') && k(pos + 1);
      case Kind::Delim: return pos < items_.size() && items_[pos].IsDelim(node.delim) && k(pos + 1);
      case Kind::Seq: return MatchSeq(node, 0, pos, pos, k);
      case Kind::OneOf:
        for (const NodePtr& child : node.children) {
          const size_t mark = log_.size();
          const size_t assignedMark = assigned_.size();
          if (Match(*child, pos, k)) return true;
          Rollback(mark);
          assigned_.resize(assignedMark);
        }
        return false;
      case Kind::AllOf: return MatchUnordered(node, 0, 0, pos, true, k, pos, assigned_.size(), {});
      case Kind::AnyOf: return MatchUnordered(node, 0, 0, pos, false, k, pos, assigned_.size(), {});
      case Kind::Repeat: return MatchRepeat(node, 0, pos, k);
      case Kind::NonEmpty: return Match(*node.children[0], pos, [&, pos](size_t end) { return end > pos && k(end); });
      case Kind::Function: return MatchContainer(node, pos, k, true);
      case Kind::ParenBlock: return MatchContainer(node, pos, k, false, T::LeftParen);
      case Kind::BracketBlock: return MatchContainer(node, pos, k, false, T::LeftBracket);
    }
    return false;
  }

  // Whether the node can match nothing at all.
  static bool Nullable(const SyntaxNode& node) {
    using Kind = SyntaxNode::Kind;
    switch (node.kind) {
      case Kind::Repeat: return node.repMin == 0 || Nullable(*node.children[0]);
      case Kind::Seq:
      case Kind::AllOf:
        return std::all_of(node.children.begin(), node.children.end(), [](const NodePtr& c) { return Nullable(*c); });
      case Kind::AnyOf:
      case Kind::OneOf:
        return std::any_of(node.children.begin(), node.children.end(), [](const NodePtr& c) { return Nullable(*c); });
      default: return false;
    }
  }

  // `start` is where the sequence began: a comma written in the grammar is left out when only optional terms that are
  // left out come before it or after it ("Commas specified in the grammar are implicitly omissible").
  bool MatchSeq(const SyntaxNode& node, size_t index, size_t pos, size_t start, const K& k) {
    if (index == node.children.size()) return k(pos);
    const size_t mark = log_.size();
    const size_t assignedMark = assigned_.size();
    const SyntaxNode& child = *node.children[index];
    if (Match(child, pos, [&, index](size_t next) { return MatchSeq(node, index + 1, next, start, k); })) return true;
    Rollback(mark);
    assigned_.resize(assignedMark);
    if (child.kind == SyntaxNode::Kind::Comma) {
      if (pos == start && MatchSeq(node, index + 1, pos, start, k)) return true;
      bool restNullable = true;
      for (size_t i = index + 1; i < node.children.size(); ++i) restNullable = restNullable && Nullable(*node.children[i]);
      if (restNullable && MatchSeq(node, index + 1, pos, start, [&, pos](size_t e) { return e == pos && k(e); })) return true;
      Rollback(mark);
      assigned_.resize(assignedMark);
    }
    return false;
  }

  // && (every child, any order) and || (at least one, any order, each once). `used` is a bit set of the children done.
  struct Part {
    size_t child, begin, end;
  };

  // Put what the parts matched in the order the syntax lists them (properties whose tests keep the order written excepted) ("canonical order: per grammar"), and move the
  // assignments made inside them along.
  void Reorder(const std::vector<Part>& parts, size_t start, size_t assignedFrom) {
    if (g_keepWrittenOrder) return;
    std::vector<Part> sorted = parts;
    std::sort(sorted.begin(), sorted.end(), [](const Part& a, const Part& b) { return a.child < b.child; });
    bool changed = false;
    for (size_t i = 0; i < parts.size(); ++i) changed = changed || parts[i].child != sorted[i].child;
    if (!changed) return;
    std::vector<ComponentValue> joined;
    std::vector<std::pair<Part, size_t>> moved;  // the part and where it begins now
    for (const Part& part : sorted) {
      moved.push_back({part, start + joined.size()});
      for (size_t i = part.begin; i < part.end; ++i) joined.push_back(out_[i]);
    }
    for (size_t i = 0; i < joined.size(); ++i) Set(start + i, joined[i]);
    for (size_t a = assignedFrom; a < assigned_.size(); ++a) {
      for (const auto& [part, begin] : moved) {
        if (assigned_[a].begin >= part.begin && assigned_[a].end <= part.end && assigned_[a].begin < part.end) {
          const size_t shift = begin - part.begin;
          assigned_[a].begin += shift;
          assigned_[a].end += shift;
          break;
        }
      }
    }
  }

  // && (every child, any order) and || (at least one, any order, each once). `used` is a bit set of the children done.
  bool MatchUnordered(const SyntaxNode& node, unsigned used, unsigned count, size_t pos, bool all, const K& k, size_t start, size_t assignedFrom, std::vector<Part> parts) {
    const size_t n = node.children.size();
    const auto finish = [&]() {
      const size_t mark = log_.size();
      const std::vector<ValueMatch::Assignment> savedAssignments(assigned_.begin() + assignedFrom, assigned_.end());
      Reorder(parts, start, assignedFrom);
      if (k(pos)) return true;
      Rollback(mark);
      assigned_.resize(assignedFrom);
      assigned_.insert(assigned_.end(), savedAssignments.begin(), savedAssignments.end());
      return false;
    };
    if (used == (1u << n) - 1) return finish();
    for (size_t i = 0; i < n; ++i) {
      if (used & (1u << i)) continue;
      const size_t mark = log_.size();
      const size_t assignedMark = assigned_.size();
      if (Match(*node.children[i], pos, [&, i](size_t next) {
            std::vector<Part> extended = parts;
            extended.push_back({i, pos, next});
            return MatchUnordered(node, used | (1u << i), count + 1, next, all, k, start, assignedFrom, extended);
          })) {
        return true;
      }
      Rollback(mark);
      assigned_.resize(assignedMark);
    }
    // || may stop once something matched.
    if (!all && count > 0) return finish();
    return false;
  }

  bool MatchRepeat(const SyntaxNode& node, int count, size_t pos, const K& k) {
    const bool more = node.repMax < 0 || count < node.repMax;
    if (more) {
      size_t start = pos;
      bool ok = true;
      if (node.commaSeparated && count > 0) {
        if (pos < items_.size() && items_[pos].IsToken(T::Comma)) start = pos + 1;
        else ok = false;
      }
      if (ok) {
        const size_t mark = log_.size();
        const size_t assignedMark = assigned_.size();
        if (Match(*node.children[0], start, [&, start](size_t end) {
              if (end == start && count >= node.repMin) return false;  // no progress
              return MatchRepeat(node, count + 1, end, k);
            })) {
          return true;
        }
        Rollback(mark);
        assigned_.resize(assignedMark);
      }
    }
    if (count >= node.repMin) return k(pos);
    return false;
  }

  // A function or block in the value whose content matches the one child of `node`, completely.
  bool MatchContainer(const SyntaxNode& node, size_t pos, const K& k, bool function, T opener = T::LeftParen) {
    if (pos >= items_.size()) return false;
    const ComponentValue& item = items_[pos];
    if (function) {
      if (item.kind != ComponentValue::Kind::Function || !IEquals(item.name, node.text)) return false;
    } else if (!item.IsBlock(opener)) {
      return false;
    }
    Matcher inner(item.children, compute_);
    inner.steps_ = steps_;
    if (!inner.Run(*node.children[0])) {
      steps_ = inner.steps_;
      return false;
    }
    steps_ = inner.steps_;
    ComponentValue replaced = item;
    replaced.children = inner.out_;
    if (function) replaced.name = node.original.empty() ? Lower(replaced.name) : node.original;
    const size_t mark = log_.size();
    Set(pos, replaced);
    if (k(pos + 1)) return true;
    Rollback(mark);
    return false;
  }

  bool MatchType(const SyntaxNode& node, size_t pos, const K& k) {
    const std::string& name = node.text;
    if (pos < items_.size() || name == "declaration-value" || name == "any-value") {
      bool consumed = false;
      size_t end = pos + 1;
      if (MatchLeaf(node, pos, consumed, end)) {
        if (!consumed) return false;
        // A math function stands in the value as its simplified, serialized self.
        const size_t mark = log_.size();
        if (name == "color") {
          std::optional<ComponentValue> normalized = NormalizeColor(items_[pos]);
          if (!normalized) return false;
          Set(pos, *normalized);
        } else if (!g_numberZero && (name == "length" || name == "length-percentage") && items_[pos].kind == ComponentValue::Kind::Token && items_[pos].token.type == T::Number && items_[pos].token.number == 0) {
          // A zero length is written with its unit.
          ComponentValue zero;
          zero.token.type = T::Dimension;
          zero.token.number = 0;
          zero.token.value = "px";
          Set(pos, zero);
        } else if ((name == "alpha-value" || name == "opacity-value") && items_[pos].kind == ComponentValue::Kind::Token && items_[pos].token.type == T::Percentage) {
          // A percentage here is the number it stands for.
          ComponentValue number;
          number.token.type = T::Number;
          number.token.number = items_[pos].token.number / 100;
          Set(pos, number);
        } else if (pos < items_.size() && items_[pos].kind == ComponentValue::Kind::Function && IsMathFunctionName(items_[pos].name)) {
          if (std::optional<ComponentValue> normalized = NormalizeMathFunction(items_[pos])) Set(pos, *normalized);
        }
        if (compute_ && pos < items_.size()) ComputeLeaf(name, pos);
        assigned_.push_back({"<" + name + ">", pos, end});
        if (k(end)) return true;
        assigned_.pop_back();
        Rollback(mark);
        return false;
      }
      if (consumed) return false;  // a leaf that did not match
    }
    // A type with a syntax of its own.
    std::string key = "<" + name + ">";
    const char* syntax = FindTypeSyntax(key);
    if (!syntax && name.size() > 2 && name.ends_with("()")) syntax = FindTypeSyntax(name);
    if (!syntax) return false;
    NodePtr root = SyntaxOf(syntax);
    if (!root) return false;
    // A defined type is told to the shorthand expansion, which finds the longhand that takes it.
    return Match(*root, pos, [&, pos](size_t end) {
      const size_t mark = log_.size();
      if ((name == "position" || name == "bg-position") && end > pos) {
        // The position as an x and a y, in one group standing where the matched values were.
        ComponentValues matched(out_.begin() + pos, out_.begin() + end);
        ComponentValue group;
        group.kind = ComponentValue::Kind::Function;
        group.name = "\x01";
        group.children = CanonicalPosition(matched);
        if (end - pos == 1 || true) {
          // Keep the items aligned: the group in the first slot, empty space after it.
          ComponentValue gap;
          gap.token.type = T::Whitespace;
          Set(pos, group);
          for (size_t i = pos + 1; i < end; ++i) Set(i, gap);
        }
      }
      assigned_.push_back({key, pos, end});
      if (k(end)) return true;
      assigned_.pop_back();
      Rollback(mark);
      return false;
    });
  }

  // The leaf at `pos`, in its computed form.
  void ComputeLeaf(const std::string& name, size_t pos) {
    const ComponentValue& original = items_[pos];
    if (name == "color") {
      std::optional<ComponentValue> normalized = NormalizeColor(original);
      if (!normalized) return;
      if (std::optional<ComponentValue> computed = ComputeColor(*normalized, *compute_)) Set(pos, *computed);
      return;
    }
    static const char* const numericTypes[] = {"length", "length-percentage", "angle", "angle-percentage", "time", "time-percentage", "frequency",
                                               "frequency-percentage", "resolution", "number", "integer", "percentage", "zero", "flex", "quirky-length", "dimension"};
    bool numeric = false;
    for (const char* t : numericTypes) numeric = numeric || name == t;
    if (!numeric) return;
    if (std::optional<ComponentValue> computed = ComputeNumber(original, name)) Set(pos, *computed);
  }

  std::optional<ComponentValue> ComputeNumber(const ComponentValue& v, const std::string& typeName) const {
  const ComputeContext& c = *compute_;
  if (v.kind == ComponentValue::Kind::Token) {
    if (v.token.type == T::Number && v.token.number == 0 && (typeName == "length" || typeName == "length-percentage" || typeName == "quirky-length")) {
      ComponentValue zero;
      zero.token.type = T::Dimension;
      zero.token.number = 0;
      zero.token.value = "px";
      return zero;
    }
    if (v.token.type == T::Dimension) {
      bool changed = false;
      ComponentValue out = Canonical(v, c, changed);
      return changed ? std::optional<ComponentValue>(out) : std::nullopt;
    }
    return std::nullopt;
  }
  if (v.kind == ComponentValue::Kind::Function && IsMathFunctionName(v.name)) {
    ComponentValue converted = CanonicalTree(v, c);
    std::optional<ComponentValue> normalized = NormalizeMathFunction(converted);
    if (!normalized) return std::nullopt;
    // calc() of one value is that value.
    if (normalized->kind == ComponentValue::Kind::Function && normalized->name == "calc" && normalized->children.size() == 1 && normalized->children[0].kind == ComponentValue::Kind::Token) {
      const ComponentValue& only = normalized->children[0];
      if (only.token.type == T::Number || only.token.type == T::Dimension || only.token.type == T::Percentage) {
        if (std::isfinite(only.token.number)) return only;
      }
    }
    return normalized;
  }
  return std::nullopt;
  }

  // Types that are not defined by a syntax. `handled` is set when this is one (whether or not it matched); the end of a
  // match is returned in `end`.
  bool MatchLeaf(const SyntaxNode& node, size_t pos, bool& handled, size_t& end) {
    const std::string& name = node.text;
    handled = true;
    end = pos + 1;
    if (name == "declaration-value" || name == "any-value") {
      // One or more components, anything but a bad token or a top-level ; or !.
      size_t p = pos;
      while (p < items_.size()) {
        const ComponentValue& v = items_[p];
        if (v.IsToken(T::BadString) || v.IsToken(T::BadUrl) || v.IsToken(T::Semicolon)) break;
        ++p;
      }
      if (p == pos && name == "declaration-value") return false;
      end = items_.size() > p ? p : p;
      // Greedy over the rest: the value is whatever is left.
      end = p;
      return true;
    }
    if (pos >= items_.size()) {
      handled = true;
      return false;
    }
    const ComponentValue& v = items_[pos];
    const std::optional<NumType> numeric = TypeOfComponent(v);
    const bool isToken = v.kind == ComponentValue::Kind::Token;
    if (name == "number") return numeric && IsPureNumber(*numeric) && InRange(node, v);
    if (name == "integer") return (isToken && v.token.type == T::Number && v.token.isInteger && InRange(node, v)) || (!isToken && numeric && IsPureNumber(*numeric));
    if (name == "percentage") return numeric && numeric->cat == Cat::Number && numeric->percent && InRange(node, v);
    if (name == "zero") return isToken && v.token.type == T::Number && v.token.number == 0;
    if (name == "length") {
      if (isToken && v.token.type == T::Number) return v.token.number == 0;
      return numeric && numeric->cat == Cat::Length && !numeric->percent && InRange(node, v);
    }
    if (name == "length-percentage") {
      if (isToken && v.token.type == T::Number) return v.token.number == 0;
      if (!numeric) return false;
      return ((numeric->cat == Cat::Length) || (numeric->cat == Cat::Number && numeric->percent)) && InRange(node, v);
    }
    const auto dimensionOf = [&](Cat cat, bool allowPercent) {
      if (!numeric) return false;
      if (numeric->cat == cat) return (!numeric->percent || allowPercent) && InRange(node, v);
      return allowPercent && numeric->cat == Cat::Number && numeric->percent && InRange(node, v);
    };
    if (name == "angle") return dimensionOf(Cat::Angle, false);
    if (name == "angle-percentage") return dimensionOf(Cat::Angle, true);
    if (name == "time") return dimensionOf(Cat::Time, false);
    if (name == "time-percentage") return dimensionOf(Cat::Time, true);
    if (name == "frequency") return dimensionOf(Cat::Frequency, false);
    if (name == "frequency-percentage") return dimensionOf(Cat::Frequency, true);
    if (name == "resolution") return dimensionOf(Cat::Resolution, false);
    if (name == "flex") return dimensionOf(Cat::Flex, false);
    if (name == "dimension") return isToken && v.token.type == T::Dimension;
    if (name == "string" || name == "string-token") return isToken && v.token.type == T::String;
    if (name == "ident" || name == "ident-token") return isToken && v.token.type == T::Ident;
    if (name == "custom-ident") {
      if (!(isToken && v.token.type == T::Ident)) return false;
      const std::string lower = Lower(v.token.value);
      return !IsCssWide(lower) && lower != "default";
    }
    if (name == "grid-ident") {
      // The name of a grid line: any identifier but span (and auto, which is a keyword of its own here).
      if (!(isToken && v.token.type == T::Ident)) return false;
      const std::string lower = Lower(v.token.value);
      return !IsCssWide(lower) && lower != "default" && lower != "span" && lower != "auto";
    }
    if (name == "dashed-ident") return isToken && v.token.type == T::Ident && v.token.value.size() > 2 && v.token.value.starts_with("--");
    if (name == "url-token") return isToken && v.token.type == T::Url;
    if (name == "number-token") return isToken && v.token.type == T::Number;
    if (name == "hash-token") return isToken && v.token.type == T::Hash;
    if (name == "hex-color") return IsHexColorToken(v);
    if (name == "url-modifier") return (isToken && v.token.type == T::Ident) || v.kind == ComponentValue::Kind::Function;
    if (name == "quirky-length") return false;
    if (name == "color") return NormalizeColor(v).has_value();
    if (name == "alpha-value" || name == "opacity-value") return numeric && numeric->cat == Cat::Number;
    handled = false;
    return false;
  }

  void Set(size_t pos, ComponentValue value) {
    log_.emplace_back(pos, std::move(out_[pos]));
    out_[pos] = std::move(value);
  }
  void Rollback(size_t mark) {
    while (log_.size() > mark) {
      out_[log_.back().first] = std::move(log_.back().second);
      log_.pop_back();
    }
  }

  const ComputeContext* compute_ = nullptr;
  static constexpr size_t kBudget = 400000;
  std::vector<ComponentValue> items_;
  std::vector<ComponentValue> out_;
  std::vector<std::pair<size_t, ComponentValue>> log_;
  std::vector<ValueMatch::Assignment> assigned_;
  size_t steps_ = 0;
};

}  // namespace

bool MatchSyntax(std::string_view syntax, const ComponentValues& values, ValueMatch& out, const ComputeContext* compute) {
  NodePtr root = SyntaxOf(syntax);
  if (!root) return false;
  Matcher matcher(values, compute);
  if (!matcher.Run(*root)) return false;
  out.normalized = matcher.Output();
  out.assigned = matcher.Assigned();
  return true;
}

namespace {

// A track list with the line names run together: adjacent [ ] blocks are one, and an empty one is nothing.
void CanonicalTrackList(ValueMatch& match) {
  ComponentValues out;
  const auto isNames = [](const ComponentValue& v) { return v.IsBlock(T::LeftBracket); };
  for (size_t i = 0; i < match.normalized.size(); ++i) {
    const ComponentValue& v = match.normalized[i];
    if (v.IsWhitespace()) continue;
    if (!isNames(v)) {
      out.push_back(v);
      continue;
    }
    ComponentValue merged = v;
    merged.children.clear();
    size_t j = i;
    for (; j < match.normalized.size(); ++j) {
      const ComponentValue& w = match.normalized[j];
      if (w.IsWhitespace()) continue;
      if (!isNames(w)) break;
      for (const ComponentValue& name : w.children) {
        if (!name.IsWhitespace()) merged.children.push_back(name);
      }
    }
    i = j - 1;
    if (!merged.children.empty()) out.push_back(merged);
  }
  match.normalized = out;
  match.assigned.clear();
}

// The shortest way display is written (css-display-3, "Short display").
void CanonicalDisplay(ValueMatch& match) {
  std::vector<std::string> words;
  for (const ComponentValue& v : match.normalized) {
    if (v.IsWhitespace()) continue;
    if (!v.IsIdent()) return;
    words.push_back(Lower(v.token.value));
  }
  std::string outside, inside;
  bool listItem = false;
  for (const std::string& w : words) {
    if (w == "block" || w == "inline" || w == "run-in") outside = w;
    else if (w == "flow" || w == "flow-root" || w == "table" || w == "flex" || w == "grid" || w == "ruby") inside = w;
    else if (w == "list-item") listItem = true;
    else return;  // a keyword of its own
  }
  if (words.size() == 1 && outside.empty() && inside.empty() && !listItem) return;
  if (inside.empty()) inside = "flow";
  if (outside.empty()) outside = inside == "ruby" ? "inline" : "block";
  std::string text;
  if (listItem) {
    text = (outside == "block" ? "" : outside + " ") + (inside == "flow" ? "" : inside + " ") + "list-item";
  } else if (inside == "flow") {
    text = outside;
  } else if (inside == "flow-root") {
    text = outside == "block" ? "flow-root" : outside == "inline" ? "inline-block" : outside + " flow-root";
  } else if (inside == "ruby") {
    text = outside == "inline" ? "ruby" : outside + " ruby";
  } else {
    text = outside == "block" ? inside : outside == "inline" ? "inline-" + inside : outside + " " + inside;
  }
  match.normalized.clear();
  for (const ComponentValue& v : ParseComponentValues(text)) {
    if (!v.IsWhitespace()) match.normalized.push_back(v);
  }
  match.assigned.clear();
}

}  // namespace

bool MatchPropertyValue(const PropertyDefinition& property, const ComponentValues& values, ValueMatch& out, const ComputeContext* compute) {
  static const std::set<std::string> keepOrder = {"image-resolution", "hanging-punctuation", "word-space-transform"};
  g_keepWrittenOrder = keepOrder.count(property.name) > 0;
  g_numberZero = std::string(property.name).rfind("border-image", 0) == 0;
  const bool matched = MatchSyntax(property.syntax, values, out, compute);
  g_keepWrittenOrder = false;
  g_numberZero = false;
  if (!matched) return false;
  if (std::string(property.name) == "display") CanonicalDisplay(out);
  if (std::string(property.name) == "grid-template-rows" || std::string(property.name) == "grid-template-columns") CanonicalTrackList(out);
  {
    const std::string n = property.name;
    if (n == "grid-template-rows" || n == "grid-template-columns" || n == "grid-auto-rows" || n == "grid-auto-columns") {
      // A zero length is written with its unit.
      std::function<void(ComponentValues&)> fix = [&](ComponentValues& list) {
        for (ComponentValue& v : list) {
          if (v.kind == ComponentValue::Kind::Token && v.token.type == T::Number && v.token.number == 0) {
            v.token.type = T::Dimension;
            v.token.value = "px";
          } else {
            fix(v.children);
          }
        }
      };
      fix(out.normalized);
    }
  }
  if (std::string(property.name) == "grid-auto-flow") {
    // "row dense" is "dense".
    std::vector<std::string> words;
    for (const ComponentValue& v : out.normalized) {
      if (v.IsIdent()) words.push_back(Lower(v.token.value));
    }
    if (words.size() == 2 && words[0] == "row" && words[1] == "dense") {
      ComponentValue dense;
      dense.token.type = T::Ident;
      dense.token.value = "dense";
      out.normalized = {dense};
      out.assigned.clear();
    }
  }
  if (std::string(property.name) == "grid-template-areas") {
    // The cells of each string alone with single spaces; every row as long as the first.
    size_t cells = 0;
    for (ComponentValue& v : out.normalized) {
      if (!v.IsToken(T::String)) continue;
      std::string collapsed;
      size_t count = 0;
      bool inWord = false;
      for (char c : v.token.value) {
        const bool space = c == ' ' || c == '\t' || c == '\n';
        if (space) {
          inWord = false;
        } else {
          if (!inWord && !collapsed.empty()) collapsed += ' ';
          if (!inWord) ++count;
          collapsed += c;
          inWord = true;
        }
      }
      if (count == 0 || (cells != 0 && count != cells)) return false;
      cells = count;
      v.token.value = collapsed;
    }
  }
  return true;
}

// ---- Serialization ----

namespace {

std::string UnitText(const std::string& unit) {
  const std::string lower = Lower(unit);
  return lower == "q" ? "Q" : lower;
}

void SerializeList(const ComponentValues& values, std::string& out);

void SerializeOne(const ComponentValue& v, std::string& out) {
  switch (v.kind) {
    case ComponentValue::Kind::Token:
      switch (v.token.type) {
        case T::Number: out += FormatNumber(v.token.number); return;
        case T::Percentage: out += FormatNumber(v.token.number) + "%"; return;
        case T::Dimension: out += FormatNumber(v.token.number) + SerializeIdentifier(UnitText(v.token.value)); return;
        case T::Url: out += SerializeUrl(v.token.value); return;
        default: out += SerializeToken(v.token); return;
      }
    case ComponentValue::Kind::Function:
      // A group made by the matcher: its values stand where it is, without a name or parentheses.
      if (v.name == "\x01") {
        SerializeList(v.children, out);
        return;
      }
      out += SerializeIdentifier(Lower(v.name)) + "(";
      SerializeList(v.children, out);
      out += ")";
      return;
    case ComponentValue::Kind::Block: {
      const char* open = v.open == T::LeftBrace ? "{" : v.open == T::LeftBracket ? "[" : "(";
      const char* close = v.open == T::LeftBrace ? "}" : v.open == T::LeftBracket ? "]" : ")";
      out += open;
      SerializeList(v.children, out);
      out += close;
      return;
    }
  }
}

void SerializeList(const ComponentValues& values, std::string& out) {
  bool first = true;
  bool afterComma = false;
  for (const ComponentValue& v : values) {
    if (v.IsWhitespace()) continue;
    if (v.IsToken(T::Comma)) {
      out += ",";
      afterComma = true;
      continue;
    }
    if (v.IsDelim('/')) {
      out += first ? "/" : " /";
      first = false;
      afterComma = true;
      continue;
    }
    if (!first) out += " ";
    (void)afterComma;
    afterComma = false;
    SerializeOne(v, out);
    first = false;
  }
}

}  // namespace

std::string SerializeValue(const ComponentValues& values) {
  std::string out;
  SerializeList(values, out);
  return out;
}

ComponentValue ResolveRelativeLengths(const ComponentValue& value, const ComputeContext& context) { return CanonicalTree(value, context); }

}  // namespace solar::css
