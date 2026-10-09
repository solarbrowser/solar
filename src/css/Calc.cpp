#include "solar/css/Calc.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace solar::css {

namespace {

using T = Token::Type;

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

constexpr double kPi = 3.14159265358979323846;

// A dimension in its canonical unit; nothing for a unit that is relative or unknown.
std::optional<MathValue> FromDimension(double number, const std::string& unitText) {
  const std::string unit = Lower(unitText);
  const auto length = [&](double factor) { return MathValue{number * factor, MathKind::Length}; };
  if (unit == "px") return length(1);
  if (unit == "cm") return length(96.0 / 2.54);
  if (unit == "mm") return length(96.0 / 25.4);
  if (unit == "q") return length(96.0 / 101.6);
  if (unit == "in") return length(96);
  if (unit == "pt") return length(96.0 / 72);
  if (unit == "pc") return length(16);
  if (unit == "deg") return MathValue{number, MathKind::Angle};
  if (unit == "grad") return MathValue{number * 0.9, MathKind::Angle};
  if (unit == "rad") return MathValue{number * 180 / kPi, MathKind::Angle};
  if (unit == "turn") return MathValue{number * 360, MathKind::Angle};
  if (unit == "s") return MathValue{number, MathKind::Time};
  if (unit == "ms") return MathValue{number / 1000, MathKind::Time};
  if (unit == "hz") return MathValue{number, MathKind::Frequency};
  if (unit == "khz") return MathValue{number * 1000, MathKind::Frequency};
  if (unit == "dppx" || unit == "x") return MathValue{number, MathKind::Resolution};
  if (unit == "dpi") return MathValue{number / 96, MathKind::Resolution};
  if (unit == "dpcm") return MathValue{number / (96 / 2.54), MathKind::Resolution};
  if (unit == "fr") return MathValue{number, MathKind::Flex};
  return std::nullopt;
}

class Evaluator {
 public:
  explicit Evaluator(const ComponentValues& values) : values_(values) {}

  std::optional<MathValue> Sum() {
    std::optional<MathValue> left = Product();
    for (;;) {
      const size_t mark = pos_;
      const bool before = SkipSpace();
      if (pos_ >= values_.size() || !(values_[pos_].IsDelim('+') || values_[pos_].IsDelim('-'))) {
        pos_ = mark;
        return left;
      }
      const bool minus = values_[pos_].IsDelim('-');
      ++pos_;
      const bool after = SkipSpace();
      if (!before || !after || !left) return std::nullopt;
      std::optional<MathValue> right = Product();
      if (!right || right->kind != left->kind) return std::nullopt;
      left->value = minus ? left->value - right->value : left->value + right->value;
    }
  }

  bool AtEnd() {
    SkipSpace();
    return pos_ >= values_.size();
  }

  bool AcceptComma() {
    SkipSpace();
    if (pos_ < values_.size() && values_[pos_].IsToken(T::Comma)) {
      ++pos_;
      return true;
    }
    return false;
  }

  const ComponentValue* Peek() {
    SkipSpace();
    return pos_ < values_.size() ? &values_[pos_] : nullptr;
  }
  void Skip() { ++pos_; }

 private:
  bool SkipSpace() {
    bool any = false;
    while (pos_ < values_.size() && values_[pos_].IsWhitespace()) {
      ++pos_;
      any = true;
    }
    return any;
  }

  std::optional<MathValue> Product() {
    std::optional<MathValue> left = Operand();
    for (;;) {
      const size_t mark = pos_;
      SkipSpace();
      if (pos_ < values_.size() && (values_[pos_].IsDelim('*') || values_[pos_].IsDelim('/'))) {
        const bool divide = values_[pos_].IsDelim('/');
        ++pos_;
        SkipSpace();
        std::optional<MathValue> right = Operand();
        if (!left || !right) return std::nullopt;
        if (divide) {
          if (right->kind != MathKind::Number) return std::nullopt;
          left->value = left->value / right->value;
        } else if (right->kind == MathKind::Number) {
          left->value *= right->value;
        } else if (left->kind == MathKind::Number) {
          right->value *= left->value;
          left = right;
        } else {
          return std::nullopt;
        }
      } else {
        pos_ = mark;
        return left;
      }
    }
  }

  std::optional<MathValue> Operand() {
    SkipSpace();
    if (pos_ >= values_.size()) return std::nullopt;
    return EvaluateNumeric(values_[pos_++]);
  }

  const ComponentValues& values_;
  size_t pos_ = 0;
};

// Round to a multiple of `step`, by the strategy.
double RoundTo(const std::string& strategy, double value, double step) {
  if (step == 0 || std::isnan(step) || std::isnan(value)) return std::numeric_limits<double>::quiet_NaN();
  step = std::fabs(step);
  if (std::isinf(value)) return std::isinf(step) ? std::numeric_limits<double>::quiet_NaN() : value;
  if (std::isinf(step)) {
    if (strategy == "up") return value > 0 ? std::numeric_limits<double>::infinity() : (value == 0 ? value : -0.0);
    if (strategy == "down") return value < 0 ? -std::numeric_limits<double>::infinity() : (value == 0 ? value : 0.0);
    return value < 0 ? -0.0 : 0.0;
  }
  const double quotient = value / step;
  double rounded;
  if (strategy == "up") rounded = std::ceil(quotient);
  else if (strategy == "down") rounded = std::floor(quotient);
  else if (strategy == "to-zero") rounded = std::trunc(quotient);
  else rounded = std::floor(quotient + 0.5) == quotient + 0.5 && quotient < 0 ? std::floor(quotient + 0.5) + 0 : std::floor(quotient + 0.5);  // nearest, ties up
  if (strategy == "nearest" || strategy.empty()) {
    const double down = std::floor(quotient), up = std::ceil(quotient);
    rounded = (quotient - down) < (up - quotient) ? down : up;  // a tie goes up
  }
  return rounded * step;
}

std::optional<MathValue> Function(const ComponentValue& function) {
  const std::string name = Lower(function.name);
  if (name == "calc" || name == "-webkit-calc" || name == "-moz-calc") {
    Evaluator evaluator(function.children);
    std::optional<MathValue> value = evaluator.Sum();
    if (!value || !evaluator.AtEnd()) return std::nullopt;
    return value;
  }
  // Comma-separated arguments, each a sum (or a keyword for clamp() and round()).
  std::vector<MathValue> args;
  std::vector<std::string> words;
  {
    Evaluator evaluator(function.children);
    for (;;) {
      const ComponentValue* next = evaluator.Peek();
      if (!next) return std::nullopt;
      if (next->IsIdent()) {
        const std::string word = Lower(next->token.value);
        if (word == "none" || word == "nearest" || word == "up" || word == "down" || word == "to-zero") {
          words.push_back(word);
          args.push_back(MathValue{0, MathKind::Number});
          evaluator.Skip();
          if (evaluator.AcceptComma()) continue;
          if (!evaluator.AtEnd()) return std::nullopt;
          break;
        }
      }
      std::optional<MathValue> value = evaluator.Sum();
      if (!value) return std::nullopt;
      args.push_back(*value);
      words.push_back("");
      if (evaluator.AcceptComma()) continue;
      if (!evaluator.AtEnd()) return std::nullopt;
      break;
    }
  }
  const auto sameKind = [&](size_t from = 0) {
    for (size_t i = from; i < args.size(); ++i) {
      if (!words[i].empty()) continue;
      if (args[i].kind != args[from].kind) return false;
    }
    return true;
  };
  const double nan = std::numeric_limits<double>::quiet_NaN();
  if (name == "min" || name == "max") {
    if (args.empty() || !sameKind()) return std::nullopt;
    MathValue result = args[0];
    for (const MathValue& a : args) {
      if (std::isnan(a.value)) return MathValue{nan, result.kind};
      result.value = name == "min" ? std::min(result.value, a.value) : std::max(result.value, a.value);
    }
    return result;
  }
  if (name == "clamp") {
    if (args.size() != 3) return std::nullopt;
    std::vector<MathValue> real;
    for (size_t i = 0; i < 3; ++i) {
      if (words[i] != "none") real.push_back(args[i]);
    }
    for (const MathValue& a : real) {
      if (a.kind != real[0].kind) return std::nullopt;
    }
    if (real.empty()) return std::nullopt;
    const MathKind kind = real[0].kind;
    double lo = words[0] == "none" ? -std::numeric_limits<double>::infinity() : args[0].value;
    double hi = words[2] == "none" ? std::numeric_limits<double>::infinity() : args[2].value;
    const double v = args[1].value;
    if (std::isnan(lo) || std::isnan(hi) || std::isnan(v)) return MathValue{nan, kind};
    return MathValue{std::max(lo, std::min(v, hi)), kind};
  }
  if (name == "round") {
    std::string strategy;
    size_t first = 0;
    if (!words.empty() && !words[0].empty() && words[0] != "none") {
      strategy = words[0];
      first = 1;
    }
    if (args.size() - first < 1 || args.size() - first > 2) return std::nullopt;
    const MathValue a = args[first];
    MathValue b = args.size() - first == 2 ? args[first + 1] : MathValue{1, a.kind == MathKind::Number ? MathKind::Number : a.kind};
    if (args.size() - first == 1 && a.kind != MathKind::Number) return std::nullopt;
    if (b.kind != a.kind) return std::nullopt;
    return MathValue{RoundTo(strategy, a.value, b.value), a.kind};
  }
  if (name == "mod" || name == "rem") {
    if (args.size() != 2 || !sameKind()) return std::nullopt;
    const double a = args[0].value, b = args[1].value;
    if (b == 0 || std::isnan(a) || std::isnan(b) || std::isinf(a)) return MathValue{nan, args[0].kind};
    if (std::isinf(b)) return MathValue{name == "rem" ? a : ((a < 0) != (b < 0) && a != 0 ? b : a), args[0].kind};
    double result = std::fmod(a, b);
    if (name == "mod" && result != 0 && ((result < 0) != (b < 0))) result += b;
    return MathValue{result, args[0].kind};
  }
  if (name == "abs") return args.size() == 1 ? std::optional<MathValue>(MathValue{std::fabs(args[0].value), args[0].kind}) : std::nullopt;
  if (name == "sign") return args.size() == 1 ? std::optional<MathValue>(MathValue{std::isnan(args[0].value) ? nan : (args[0].value > 0 ? 1.0 : args[0].value < 0 ? -1.0 : args[0].value), MathKind::Number}) : std::nullopt;
  if (name == "sin" || name == "cos" || name == "tan") {
    if (args.size() != 1) return std::nullopt;
    double radians;
    if (args[0].kind == MathKind::Number) radians = args[0].value;
    else if (args[0].kind == MathKind::Angle) radians = args[0].value * kPi / 180;
    else return std::nullopt;
    const double v = name == "sin" ? std::sin(radians) : name == "cos" ? std::cos(radians) : std::tan(radians);
    return MathValue{v, MathKind::Number};
  }
  if (name == "asin" || name == "acos" || name == "atan") {
    if (args.size() != 1 || args[0].kind != MathKind::Number) return std::nullopt;
    const double v = name == "asin" ? std::asin(args[0].value) : name == "acos" ? std::acos(args[0].value) : std::atan(args[0].value);
    return MathValue{v * 180 / kPi, MathKind::Angle};
  }
  if (name == "atan2") {
    if (args.size() != 2 || !sameKind()) return std::nullopt;
    return MathValue{std::atan2(args[0].value, args[1].value) * 180 / kPi, MathKind::Angle};
  }
  if (name == "pow") {
    if (args.size() != 2 || args[0].kind != MathKind::Number || args[1].kind != MathKind::Number) return std::nullopt;
    return MathValue{std::pow(args[0].value, args[1].value), MathKind::Number};
  }
  if (name == "sqrt" || name == "exp") {
    if (args.size() != 1 || args[0].kind != MathKind::Number) return std::nullopt;
    return MathValue{name == "sqrt" ? std::sqrt(args[0].value) : std::exp(args[0].value), MathKind::Number};
  }
  if (name == "log") {
    if (args.empty() || args.size() > 2) return std::nullopt;
    for (const MathValue& a : args) {
      if (a.kind != MathKind::Number) return std::nullopt;
    }
    return MathValue{args.size() == 1 ? std::log(args[0].value) : std::log(args[0].value) / std::log(args[1].value), MathKind::Number};
  }
  if (name == "hypot") {
    if (args.empty() || !sameKind()) return std::nullopt;
    double sum = 0;
    for (const MathValue& a : args) sum += a.value * a.value;
    return MathValue{std::sqrt(sum), args[0].kind};
  }
  return std::nullopt;
}

}  // namespace

bool IsMathFunctionName(const std::string& name) {
  static const char* const names[] = {"calc", "min", "max", "clamp", "round", "mod", "rem", "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "pow", "sqrt",
                                      "hypot", "log", "exp", "abs", "sign", "-webkit-calc", "-moz-calc"};
  const std::string lower = Lower(name);
  for (const char* n : names) {
    if (lower == n) return true;
  }
  return false;
}

std::optional<MathValue> EvaluateNumeric(const ComponentValue& value) {
  switch (value.kind) {
    case ComponentValue::Kind::Token:
      switch (value.token.type) {
        case T::Number: return MathValue{value.token.number, MathKind::Number};
        case T::Percentage: return MathValue{value.token.number, MathKind::Percentage};
        case T::Dimension: return FromDimension(value.token.number, value.token.value);
        case T::Ident: {
          const std::string word = Lower(value.token.value);
          if (word == "e") return MathValue{2.718281828459045, MathKind::Number};
          if (word == "pi") return MathValue{kPi, MathKind::Number};
          if (word == "infinity") return MathValue{std::numeric_limits<double>::infinity(), MathKind::Number};
          if (word == "-infinity") return MathValue{-std::numeric_limits<double>::infinity(), MathKind::Number};
          if (word == "nan") return MathValue{std::numeric_limits<double>::quiet_NaN(), MathKind::Number};
          return std::nullopt;
        }
        default: return std::nullopt;
      }
    case ComponentValue::Kind::Function:
      if (!IsMathFunctionName(value.name)) return std::nullopt;
      return Function(value);
    case ComponentValue::Kind::Block:
      if (value.open != T::LeftParen) return std::nullopt;
      {
        Evaluator evaluator(value.children);
        std::optional<MathValue> inner = evaluator.Sum();
        if (!inner || !evaluator.AtEnd()) return std::nullopt;
        return inner;
      }
  }
  return std::nullopt;
}

}  // namespace solar::css

// ---- Calculation trees ----

namespace solar::css {

namespace {

using T = Token::Type;

struct CalcNode {
  enum class Kind { Number, Percent, Dimension, Sum, Product, Negate, Invert, Function, Opaque };
  Kind kind = Kind::Number;
  double value = 0;
  std::string unit;  // of a dimension, lowercase (canonical when the unit is absolute)
  std::string name;  // of a function
  std::vector<CalcNode> kids;
  ComponentValue opaque;

  bool Numeric() const { return kind == Kind::Number || kind == Kind::Percent || kind == Kind::Dimension; }
};

std::string CanonicalUnit(MathKind kind) {
  switch (kind) {
    case MathKind::Length: return "px";
    case MathKind::Angle: return "deg";
    case MathKind::Time: return "s";
    case MathKind::Frequency: return "hz";
    case MathKind::Resolution: return "dppx";
    case MathKind::Flex: return "fr";
    default: return "";
  }
}

CalcNode Num(double v) {
  CalcNode node;
  node.kind = CalcNode::Kind::Number;
  node.value = v;
  return node;
}

class TreeParser {
 public:
  explicit TreeParser(const ComponentValues& values, const std::vector<std::string>* idents = nullptr) : values_(values), idents_(idents) {}

  bool Sum(CalcNode& out) {
    CalcNode first;
    if (!Product(first)) return false;
    std::vector<CalcNode> terms{first};
    for (;;) {
      const size_t mark = pos_;
      const bool before = SkipSpace();
      if (pos_ < values_.size() && (values_[pos_].IsDelim('+') || values_[pos_].IsDelim('-'))) {
        const bool minus = values_[pos_].IsDelim('-');
        ++pos_;
        const bool after = SkipSpace();
        if (!before || !after) return false;
        CalcNode next;
        if (!Product(next)) return false;
        if (minus) {
          CalcNode negated;
          negated.kind = CalcNode::Kind::Negate;
          negated.kids.push_back(next);
          next = negated;
        }
        terms.push_back(next);
      } else {
        pos_ = mark;
        break;
      }
    }
    if (terms.size() == 1) {
      out = terms[0];
    } else {
      out.kind = CalcNode::Kind::Sum;
      out.kids = terms;
    }
    return true;
  }

  bool AtEnd() {
    SkipSpace();
    return pos_ >= values_.size();
  }
  bool AcceptComma() {
    SkipSpace();
    if (pos_ < values_.size() && values_[pos_].IsToken(T::Comma)) {
      ++pos_;
      return true;
    }
    return false;
  }
  const ComponentValue* Peek() {
    SkipSpace();
    return pos_ < values_.size() ? &values_[pos_] : nullptr;
  }
  void Skip() { ++pos_; }

 private:
  bool SkipSpace() {
    bool any = false;
    while (pos_ < values_.size() && values_[pos_].IsWhitespace()) {
      ++pos_;
      any = true;
    }
    return any;
  }

  bool Product(CalcNode& out) {
    CalcNode first;
    if (!Value(first)) return false;
    std::vector<CalcNode> factors{first};
    for (;;) {
      const size_t mark = pos_;
      SkipSpace();
      if (pos_ < values_.size() && (values_[pos_].IsDelim('*') || values_[pos_].IsDelim('/'))) {
        const bool divide = values_[pos_].IsDelim('/');
        ++pos_;
        SkipSpace();
        CalcNode next;
        if (!Value(next)) return false;
        if (divide) {
          CalcNode inverted;
          inverted.kind = CalcNode::Kind::Invert;
          inverted.kids.push_back(next);
          next = inverted;
        }
        factors.push_back(next);
      } else {
        pos_ = mark;
        break;
      }
    }
    if (factors.size() == 1) {
      out = factors[0];
    } else {
      out.kind = CalcNode::Kind::Product;
      out.kids = factors;
    }
    return true;
  }

  bool Value(CalcNode& out) {
    SkipSpace();
    if (pos_ >= values_.size()) return false;
    const ComponentValue& v = values_[pos_++];
    if (v.kind == ComponentValue::Kind::Token) {
      switch (v.token.type) {
        case T::Number: out = Num(v.token.number); return true;
        case T::Percentage:
          out.kind = CalcNode::Kind::Percent;
          out.value = v.token.number;
          return true;
        case T::Dimension: {
          out.kind = CalcNode::Kind::Dimension;
          out.value = v.token.number;
          out.unit = Lower(v.token.value);
          if (const std::optional<MathValue> canonical = FromDimension(v.token.number, v.token.value)) {
            out.value = canonical->value;
            out.unit = CanonicalUnit(canonical->kind);
          }
          return true;
        }
        case T::Ident: {
          const std::string word = Lower(v.token.value);
          if (std::optional<MathValue> constant = EvaluateNumeric(v); constant && (word == "e" || word == "pi" || word == "infinity" || word == "-infinity" || word == "nan")) {
            out = Num(constant->value);
            return true;
          }
          if (idents_ && std::find(idents_->begin(), idents_->end(), word) != idents_->end()) {
            out.kind = CalcNode::Kind::Opaque;
            out.opaque = v;
            out.opaque.token.value = word;
            return true;
          }
          return false;
        }
        default: return false;
      }
    }
    if (v.kind == ComponentValue::Kind::Block) {
      if (v.open != T::LeftParen) return false;
      TreeParser inner(v.children, idents_);
      if (!inner.Sum(out) || !inner.AtEnd()) return false;
      return true;
    }
    // A function.
    const std::string name = Lower(v.name);
    if (name == "calc" || name == "-webkit-calc" || name == "-moz-calc") {
      TreeParser inner(v.children, idents_);
      return inner.Sum(out) && inner.AtEnd();
    }
    if (IsMathFunctionName(name)) {
      out.kind = CalcNode::Kind::Function;
      out.name = name;
      TreeParser inner(v.children, idents_);
      for (;;) {
        const ComponentValue* next = inner.Peek();
        if (!next) return false;
        CalcNode arg;
        if (next->IsIdent() && (name == "round" || name == "clamp")) {
          const std::string word = Lower(next->token.value);
          if (word == "none" || word == "nearest" || word == "up" || word == "down" || word == "to-zero") {
            arg.kind = CalcNode::Kind::Opaque;
            arg.opaque = *next;
            arg.opaque.token.value = word;
            inner.Skip();
            out.kids.push_back(arg);
            if (inner.AcceptComma()) continue;
            return inner.AtEnd();
          }
        }
        if (!inner.Sum(arg)) return false;
        out.kids.push_back(arg);
        if (inner.AcceptComma()) continue;
        return inner.AtEnd();
      }
    }
    // var(), env(), attr() and the like: only known later.
    out.kind = CalcNode::Kind::Opaque;
    out.opaque = v;
    return true;
  }

  const ComponentValues& values_;
  const std::vector<std::string>* idents_;
  size_t pos_ = 0;
};

bool SameKind(const CalcNode& a, const CalcNode& b) {
  if (a.kind != b.kind) return false;
  return a.kind != CalcNode::Kind::Dimension || a.unit == b.unit;
}

CalcNode Simplify(CalcNode node);

CalcNode SimplifySum(CalcNode node) {
  // Flatten, simplify the terms, and add up those of one kind.
  std::vector<CalcNode> terms;
  const std::function<void(CalcNode)> add = [&](CalcNode term) {
    term = Simplify(term);
    if (term.kind == CalcNode::Kind::Sum) {
      for (CalcNode& inner : term.kids) add(inner);
    } else {
      terms.push_back(term);
    }
  };
  for (CalcNode& kid : node.kids) add(kid);
  // A negated number is a number.
  std::vector<CalcNode> merged;
  for (CalcNode& term : terms) {
    bool done = false;
    if (term.Numeric()) {
      for (CalcNode& m : merged) {
        if (m.Numeric() && SameKind(m, term)) {
          m.value += term.value;
          done = true;
          break;
        }
      }
    }
    if (!done) merged.push_back(term);
  }
  // Numbers first, then percentages, then dimensions by unit, then the rest as they were.
  std::stable_sort(merged.begin(), merged.end(), [](const CalcNode& a, const CalcNode& b) {
    const auto rank = [](const CalcNode& n) { return n.kind == CalcNode::Kind::Number ? 0 : n.kind == CalcNode::Kind::Percent ? 1 : n.kind == CalcNode::Kind::Dimension ? 2 : 3; };
    if (rank(a) != rank(b)) return rank(a) < rank(b);
    if (a.kind == CalcNode::Kind::Dimension) return a.unit < b.unit;
    return false;
  });
  if (merged.size() == 1) return merged[0];
  node.kind = CalcNode::Kind::Sum;
  node.kids = merged;
  return node;
}

CalcNode SimplifyProduct(CalcNode node) {
  std::vector<CalcNode> factors;
  const std::function<void(CalcNode)> add = [&](CalcNode factor) {
    factor = Simplify(factor);
    if (factor.kind == CalcNode::Kind::Product) {
      for (CalcNode& inner : factor.kids) add(inner);
    } else {
      factors.push_back(factor);
    }
  };
  for (CalcNode& kid : node.kids) add(kid);
  // The numeric factors multiply; at most one of them is not a plain number.
  double scalar = 1;
  CalcNode* typed = nullptr;
  std::vector<CalcNode> rest;
  for (CalcNode& f : factors) {
    if (f.kind == CalcNode::Kind::Number) {
      scalar *= f.value;
    } else if ((f.kind == CalcNode::Kind::Percent || f.kind == CalcNode::Kind::Dimension) && !typed) {
      typed = &f;
    } else {
      rest.push_back(f);
    }
  }
  CalcNode numeric = typed ? *typed : Num(1);
  numeric.value = typed ? typed->value * scalar : scalar;
  if (rest.empty()) return numeric;
  // A number times a sum goes into the sum.
  const auto allNumeric = [](const CalcNode& sum) {
    for (const CalcNode& k : sum.kids) {
      if (!k.Numeric()) return false;
    }
    return true;
  };
  if (rest.size() == 1 && rest[0].kind == CalcNode::Kind::Sum && !typed && allNumeric(rest[0])) {
    CalcNode sum = rest[0];
    for (CalcNode& term : sum.kids) {
      CalcNode product;
      product.kind = CalcNode::Kind::Product;
      product.kids = {Num(scalar), term};
      term = product;
    }
    return Simplify(sum);
  }
  node.kind = CalcNode::Kind::Product;
  node.kids.clear();
  if (!(numeric.kind == CalcNode::Kind::Number && numeric.value == 1)) node.kids.push_back(numeric);
  for (CalcNode& r : rest) node.kids.push_back(r);
  if (node.kids.size() == 1 && node.kids[0].kind != CalcNode::Kind::Invert) return node.kids[0];
  return node;
}

// A function whose arguments are all numeric of one kind is evaluated.
CalcNode SimplifyFunction(CalcNode node) {
  bool allNumeric = true;
  for (CalcNode& kid : node.kids) {
    if (kid.kind != CalcNode::Kind::Opaque) kid = Simplify(kid);
  }
  // clamp() with a bound left out is a min() or a max().
  if (node.name == "clamp" && node.kids.size() == 3) {
    const auto none = [](const CalcNode& k) { return k.kind == CalcNode::Kind::Opaque && k.opaque.IsIdent() && Lower(k.opaque.token.value) == "none"; };
    if (none(node.kids[0]) && none(node.kids[2])) return node.kids[1];
    if (none(node.kids[0])) {
      node.name = "min";
      node.kids.erase(node.kids.begin());
    } else if (none(node.kids[2])) {
      node.name = "max";
      node.kids.pop_back();
    }
  }
  for (CalcNode& kid : node.kids) {
    if (kid.kind == CalcNode::Kind::Opaque && node.name != "round" && node.name != "clamp") allNumeric = false;
    else if (kid.kind != CalcNode::Kind::Opaque && !kid.Numeric()) allNumeric = false;
  }
  if (!allNumeric) return node;
  // Evaluate through the numeric evaluator, on a copy of the function written out.
  ComponentValue function;
  function.kind = ComponentValue::Kind::Function;
  function.name = node.name;
  std::vector<ComponentValue> args;
  bool first = true;
  for (const CalcNode& kid : node.kids) {
    ComponentValue c;
    if (kid.kind == CalcNode::Kind::Opaque) {
      c = kid.opaque;
    } else if (kid.kind == CalcNode::Kind::Number) {
      c.token.type = T::Number;
      c.token.number = kid.value;
    } else if (kid.kind == CalcNode::Kind::Percent) {
      c.token.type = T::Percentage;
      c.token.number = kid.value;
    } else {
      c.token.type = T::Dimension;
      c.token.number = kid.value;
      c.token.value = kid.unit;
    }
    if (!first) {
      ComponentValue comma;
      comma.token.type = T::Comma;
      function.children.push_back(comma);
    }
    function.children.push_back(c);
    first = false;
  }
  const std::optional<MathValue> result = EvaluateNumeric(function);
  if (!result) return node;
  CalcNode value;
  switch (result->kind) {
    case MathKind::Number: return Num(result->value);
    case MathKind::Percentage:
      value.kind = CalcNode::Kind::Percent;
      value.value = result->value;
      return value;
    default:
      value.kind = CalcNode::Kind::Dimension;
      value.value = result->value;
      value.unit = CanonicalUnit(result->kind);
      return value;
  }
}

CalcNode Simplify(CalcNode node) {
  switch (node.kind) {
    case CalcNode::Kind::Sum: return SimplifySum(std::move(node));
    case CalcNode::Kind::Product: return SimplifyProduct(std::move(node));
    case CalcNode::Kind::Function: return SimplifyFunction(std::move(node));
    case CalcNode::Kind::Negate: {
      CalcNode inner = Simplify(node.kids[0]);
      if (inner.Numeric()) {
        inner.value = -inner.value;
        return inner;
      }
      if (inner.kind == CalcNode::Kind::Negate) return inner.kids[0];
      if (inner.kind == CalcNode::Kind::Sum) {
        for (CalcNode& term : inner.kids) {
          CalcNode negated;
          negated.kind = CalcNode::Kind::Negate;
          negated.kids.push_back(term);
          term = negated;
        }
        return Simplify(inner);
      }
      node.kids[0] = inner;
      return node;
    }
    case CalcNode::Kind::Invert: {
      CalcNode inner = Simplify(node.kids[0]);
      if (inner.kind == CalcNode::Kind::Number) return Num(1 / inner.value);
      if (inner.kind == CalcNode::Kind::Invert) return inner.kids[0];
      node.kids[0] = inner;
      return node;
    }
    default: return node;
  }
}

// ---- Writing a tree back as component values ----

void Emit(const CalcNode& node, ComponentValues& out, bool inProduct);

// A child of a sum or a product: an operator in an operator is in parentheses.
void EmitOperand(const CalcNode& node, ComponentValues& out) {
  if (node.kind == CalcNode::Kind::Sum || node.kind == CalcNode::Kind::Product) {
    ComponentValue paren;
    paren.kind = ComponentValue::Kind::Block;
    paren.open = T::LeftParen;
    Emit(node, paren.children, false);
    out.push_back(paren);
  } else {
    Emit(node, out, false);
  }
}

ComponentValue PunctToken(T type) {
  ComponentValue v;
  v.token.type = type;
  return v;
}
ComponentValue DelimToken(char32_t c) {
  ComponentValue v;
  v.token.type = T::Delim;
  v.token.delim = c;
  return v;
}
// A + or - with the whitespace around it that makes it an operator (serialization leaves the whitespace out).
void Operator(ComponentValues& out, char32_t c) {
  ComponentValue space;
  space.token.type = T::Whitespace;
  out.push_back(space);
  out.push_back(DelimToken(c));
  out.push_back(space);
}

void EmitNumeric(const CalcNode& node, ComponentValues& out, double value) {
  if (value == 0) value = 0;  // not -0
  ComponentValue v;
  v.token.number = value;
  if (!std::isfinite(value) && node.kind != CalcNode::Kind::Number) {
    // NaN * 1px, infinity * 1%.
    ComponentValue scalar;
    scalar.token.type = T::Number;
    scalar.token.number = value;
    out.push_back(scalar);
    out.push_back(DelimToken('*'));
    v.token.number = 1;
  }
  switch (node.kind) {
    case CalcNode::Kind::Number: v.token.type = T::Number; break;
    case CalcNode::Kind::Percent: v.token.type = T::Percentage; break;
    default:
      v.token.type = T::Dimension;
      v.token.value = node.unit;
      break;
  }
  out.push_back(v);
}

void Emit(const CalcNode& node, ComponentValues& out, bool inProduct) {
  switch (node.kind) {
    case CalcNode::Kind::Number:
    case CalcNode::Kind::Percent:
    case CalcNode::Kind::Dimension: EmitNumeric(node, out, node.value); return;
    case CalcNode::Kind::Opaque: out.push_back(node.opaque); return;
    case CalcNode::Kind::Function: {
      ComponentValue f;
      f.kind = ComponentValue::Kind::Function;
      f.name = node.name;
      bool first = true;
      for (const CalcNode& kid : node.kids) {
        if (!first) f.children.push_back(PunctToken(T::Comma));
        Emit(kid, f.children, false);
        first = false;
      }
      out.push_back(f);
      return;
    }
    case CalcNode::Kind::Negate:
      // Only in a sum, where it is a subtraction; alone it is -1 times the term.
      out.push_back(DelimToken('-'));
      EmitOperand(node.kids[0], out);
      return;
    case CalcNode::Kind::Invert:
      out.push_back(DelimToken('/'));
      EmitOperand(node.kids[0], out);
      return;
    case CalcNode::Kind::Product: {
      ComponentValues body;
      bool first = true;
      for (const CalcNode& kid : node.kids) {
        if (kid.kind == CalcNode::Kind::Invert) {
          if (first) {
            ComponentValue one;
            one.token.type = T::Number;
            one.token.number = 1;
            body.push_back(one);
          }
          body.push_back(DelimToken('/'));
          EmitOperand(kid.kids[0], body);
        } else {
          if (!first) body.push_back(DelimToken('*'));
          EmitOperand(kid, body);
        }
        first = false;
      }
      for (ComponentValue& v : body) out.push_back(std::move(v));
      return;
    }
    case CalcNode::Kind::Sum: {
      ComponentValues body;
      bool first = true;
      for (const CalcNode& kid : node.kids) {
        if (kid.kind == CalcNode::Kind::Negate) {
          Operator(body, '-');
          EmitOperand(kid.kids[0], body);
        } else if (kid.Numeric() && kid.value < 0 && !first) {
          Operator(body, '-');
          EmitNumeric(kid, body, -kid.value);
        } else {
          if (!first) Operator(body, '+');
          EmitOperand(kid, body);
        }
        first = false;
      }
      for (ComponentValue& v : body) out.push_back(std::move(v));
      return;
    }
  }
}

}  // namespace

namespace {
bool HasSubstitution(const ComponentValue& v) {
  if (v.kind == ComponentValue::Kind::Function) {
    const std::string name = Lower(v.name);
    if (name == "var" || name == "env" || name == "attr") return true;
  }
  if (v.kind != ComponentValue::Kind::Token) {
    for (const ComponentValue& child : v.children) {
      if (HasSubstitution(child)) return true;
    }
  }
  return false;
}
}  // namespace

std::optional<ComponentValue> NormalizeMathFunction(const ComponentValue& function, const std::vector<std::string>* allowedIdents) {
  if (function.kind != ComponentValue::Kind::Function || !IsMathFunctionName(function.name)) return std::nullopt;
  // What has a var() in it is known only when the variable is: it stays as written.
  if (HasSubstitution(function)) return function;
  ComponentValues wrapper{function};
  TreeParser parser(wrapper, allowedIdents);
  CalcNode root;
  if (!parser.Sum(root) || !parser.AtEnd()) return std::nullopt;
  root = Simplify(root);
  ComponentValue result;
  result.kind = ComponentValue::Kind::Function;
  if (root.kind == CalcNode::Kind::Function) {
    // A math function stands for itself.
    result.name = root.name;
    bool first = true;
    for (const CalcNode& kid : root.kids) {
      if (!first) result.children.push_back(PunctToken(T::Comma));
      Emit(kid, result.children, false);
      first = false;
    }
    return result;
  }
  result.name = "calc";
  Emit(root, result.children, false);
  return result;
}

}  // namespace solar::css
