// Container queries (https://www.w3.org/TR/css-conditional-5/#container-queries): whether an @container rule applies to an element, from the size layout
// found for the nearest ancestor that is a container, or from its style.
#include <algorithm>
#include <cmath>

#include "solar/css/Calc.h"
#include "solar/css/Container.h"
#include "solar/css/MediaQuery.h"
#include "solar/css/Style.h"
#include "solar/css/Syntax.h"

namespace solar::css {

namespace {

enum class Truth { False, True, Unknown };

Truth Not(Truth t) { return t == Truth::True ? Truth::False : t == Truth::False ? Truth::True : Truth::Unknown; }

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

dom::Element* FlatParentOf(dom::Element* element) {
  if (element->assignedSlot) return const_cast<dom::Element*>(element->assignedSlot);
  dom::Node* parent = element->parentNode;
  if (!parent) return nullptr;
  if (dom::Element* e = dom::AsElement(parent)) return e;
  if (parent->IsFragment() && static_cast<dom::DocumentFragment*>(parent)->isShadowRoot) return const_cast<dom::Element*>(static_cast<dom::DocumentFragment*>(parent)->host);
  return nullptr;
}

struct Query {
  Quanta::Context& ctx;
  dom::Element* container = nullptr;
  bool sizeContainer = false;  // it has container-type size or inline-size
  bool inlineOnly = false;     // inline-size
  bool vertical = false;       // its writing mode is vertical: width is the block size
  double width = -1, height = -1;
};

double RelativeLength(Query& q, const Token& token, bool& ok) {
  const double n = token.number;
  const std::string unit = Lower(token.value);
  ok = true;
  const MediaEnvironment env = EnvironmentFor(q.container->nodeDocument);
  const auto fontSize = [&](dom::Element* e) {
    const std::string v = ComputedValue(q.ctx, e, "font-size");
    const double px = std::atof(v.c_str());
    return px > 0 ? px : 16.0;
  };
  if (unit == "px") return n;
  if (unit == "em") return n * fontSize(q.container);
  if (unit == "rem") return n * (q.container->nodeDocument->DocumentElement() ? fontSize(q.container->nodeDocument->DocumentElement()) : 16.0);
  if (unit == "ex" || unit == "ch") return n * fontSize(q.container) * 0.5;
  if (unit == "cm") return n * 96 / 2.54;
  if (unit == "mm") return n * 96 / 25.4;
  if (unit == "q") return n * 96 / 101.6;
  if (unit == "in") return n * 96;
  if (unit == "pt") return n * 96 / 72;
  if (unit == "pc") return n * 16;
  if (unit == "vw" || unit == "svw" || unit == "lvw" || unit == "dvw" || unit == "vi") return n * env.width / 100;
  if (unit == "vh" || unit == "svh" || unit == "lvh" || unit == "dvh" || unit == "vb") return n * env.height / 100;
  if (unit == "vmin") return n * std::min(env.width, env.height) / 100;
  if (unit == "vmax") return n * std::max(env.width, env.height) / 100;
  ok = false;
  return 0;
}

// A length in a feature's value: a dimension, a plain zero, or a calculation with no relative units.
bool LengthValue(Query& q, const ComponentValue& v, double& out) {
  bool ok = false;
  if (v.IsToken(Token::Type::Dimension)) {
    out = RelativeLength(q, v.token, ok);
    return ok;
  }
  if (v.IsToken(Token::Type::Number) && v.token.number == 0) {
    out = 0;
    return true;
  }
  if (v.kind == ComponentValue::Kind::Function && IsMathFunctionName(Lower(v.name))) {
    if (const std::optional<MathValue> m = EvaluateNumeric(v)) {
      if (m->kind == MathKind::Length) {
        out = m->value;
        return true;
      }
    }
  }
  return false;
}

// A ratio: `a / b` or a number.
bool RatioValue(const std::vector<const ComponentValue*>& items, size_t& at, double& out) {
  if (at >= items.size() || !items[at]->IsToken(Token::Type::Number)) return false;
  double numerator = items[at]->token.number, denominator = 1;
  ++at;
  if (at + 1 < items.size() && items[at]->IsDelim('/') && items[at + 1]->IsToken(Token::Type::Number)) {
    denominator = items[at + 1]->token.number;
    at += 2;
  }
  if (denominator == 0) {
    out = numerator == 0 ? 0 : INFINITY;
    return true;
  }
  out = numerator / denominator;
  return true;
}

enum class Op { Eq, Lt, Le, Gt, Ge };

bool Compare(double a, Op op, double b) {
  const double eps = 1e-6;
  switch (op) {
    case Op::Eq: return std::fabs(a - b) < eps;
    case Op::Lt: return a < b - eps;
    case Op::Le: return a <= b + eps;
    case Op::Gt: return a > b + eps;
    case Op::Ge: return a >= b - eps;
  }
  return false;
}

Op Flip(Op op) {
  switch (op) {
    case Op::Lt: return Op::Gt;
    case Op::Le: return Op::Ge;
    case Op::Gt: return Op::Lt;
    case Op::Ge: return Op::Le;
    default: return op;
  }
}

// The value of a size feature of the container, or nothing when the container does not have it (the axis is not its).
std::optional<double> FeatureValue(Query& q, const std::string& name, bool& isRatio) {
  isRatio = false;
  if (q.width < 0 || q.height < 0) return std::nullopt;
  const double inlineSize = q.vertical ? q.height : q.width, blockSize = q.vertical ? q.width : q.height;
  if (name == "inline-size") return inlineSize;
  if (name == "width") {
    if (q.inlineOnly && q.vertical) return std::nullopt;
    return q.width;
  }
  if (name == "height") {
    if (q.inlineOnly && !q.vertical) return std::nullopt;
    return q.height;
  }
  if (name == "block-size") {
    if (q.inlineOnly) return std::nullopt;
    return blockSize;
  }
  if (name == "aspect-ratio") {
    if (q.inlineOnly) return std::nullopt;
    isRatio = true;
    return q.height == 0 ? INFINITY : q.width / q.height;
  }
  return std::nullopt;
}

Truth Feature(Query& q, const ComponentValues& children) {
  // The items of the feature, without white space.
  std::vector<const ComponentValue*> items;
  for (const ComponentValue& v : children) if (!v.IsWhitespace()) items.push_back(&v);
  if (items.empty()) return Truth::Unknown;
  if (!q.sizeContainer) return Truth::Unknown;
  const auto featureName = [](const ComponentValue* v) -> std::string { return v->IsIdent() ? Lower(v->token.value) : std::string(); };
  const auto isFeature = [&](const std::string& n) { return n == "width" || n == "height" || n == "inline-size" || n == "block-size" || n == "aspect-ratio" || n == "orientation"; };
  // (name: value), with min- and max- before the name.
  if (items.size() >= 3 && items[0]->IsIdent() && items[1]->IsToken(Token::Type::Colon)) {
    std::string name = featureName(items[0]);
    Op op = Op::Eq;
    if (name.rfind("min-", 0) == 0) {
      name = name.substr(4);
      op = Op::Ge;
    } else if (name.rfind("max-", 0) == 0) {
      name = name.substr(4);
      op = Op::Le;
    }
    if (!isFeature(name)) return Truth::Unknown;
    if (name == "orientation") {
      if (op != Op::Eq || items.size() != 3 || !items[2]->IsIdent()) return Truth::Unknown;
      if (q.width < 0 || q.height < 0 || q.inlineOnly) return Truth::Unknown;
      const std::string v = Lower(items[2]->token.value);
      if (v == "portrait") return q.height >= q.width ? Truth::True : Truth::False;
      if (v == "landscape") return q.width > q.height ? Truth::True : Truth::False;
      return Truth::Unknown;
    }
    bool isRatio = false;
    const std::optional<double> have = FeatureValue(q, name, isRatio);
    if (!have) return Truth::Unknown;
    double want;
    if (name == "aspect-ratio") {
      size_t at = 2;
      if (!RatioValue(items, at, want) || at != items.size()) return Truth::Unknown;
    } else {
      if (items.size() != 3 || !LengthValue(q, *items[2], want)) return Truth::Unknown;
      if (want < 0) return Truth::Unknown;
    }
    return Compare(*have, op, want) ? Truth::True : Truth::False;
  }
  // (name) on its own: the feature is not zero.
  if (items.size() == 1 && items[0]->IsIdent() && isFeature(featureName(items[0]))) {
    const std::string name = featureName(items[0]);
    if (name == "orientation") return Truth::True;
    bool isRatio = false;
    const std::optional<double> have = FeatureValue(q, name, isRatio);
    if (!have) return Truth::Unknown;
    return *have != 0 ? Truth::True : Truth::False;
  }
  // Range context: value op name, name op value, value op name op value.
  struct Piece {
    bool isOp = false;
    Op op = Op::Eq;
    const ComponentValue* value = nullptr;
    size_t span = 1;
  };
  std::vector<Piece> pieces;
  for (size_t i = 0; i < items.size(); ++i) {
    const ComponentValue* v = items[i];
    Piece p;
    if (v->IsDelim('<') || v->IsDelim('>') || v->IsDelim('=')) {
      p.isOp = true;
      const char32_t c = v->token.delim;
      const bool eq = i + 1 < items.size() && items[i + 1]->IsDelim('=');
      if (c == '=') p.op = Op::Eq;
      else if (c == '<') p.op = eq ? Op::Le : Op::Lt;
      else p.op = eq ? Op::Ge : Op::Gt;
      if (eq && c != '=') ++i;
    } else {
      p.value = v;
    }
    pieces.push_back(p);
  }
  // The pieces are alternately values and operators, with one name among the values.
  if (pieces.size() != 3 && pieces.size() != 5) return Truth::Unknown;
  for (size_t i = 0; i < pieces.size(); ++i) if (pieces[i].isOp != (i % 2 == 1)) return Truth::Unknown;
  size_t nameAt = pieces.size();
  for (size_t i = 0; i < pieces.size(); i += 2) {
    if (pieces[i].value->IsIdent() && isFeature(featureName(pieces[i].value))) nameAt = i;
  }
  if (nameAt == pieces.size()) return Truth::Unknown;
  const std::string name = featureName(pieces[nameAt].value);
  if (name == "orientation") return Truth::Unknown;
  bool isRatio = false;
  const std::optional<double> have = FeatureValue(q, name, isRatio);
  if (!have) return Truth::Unknown;
  const auto valueAt = [&](size_t i, double& out) {
    if (name == "aspect-ratio") {
      std::vector<const ComponentValue*> one = {pieces[i].value};
      size_t at = 0;
      // (a ratio with a slash is several items: this takes the plain number only)
      return RatioValue(one, at, out);
    }
    return LengthValue(q, *pieces[i].value, out);
  };
  bool result = true;
  for (size_t i = 1; i < pieces.size(); i += 2) {
    const size_t left = i - 1, right = i + 1;
    double a, b;
    if (left == nameAt) {
      if (!valueAt(right, b)) return Truth::Unknown;
      result = result && Compare(*have, pieces[i].op, b);
    } else if (right == nameAt) {
      if (!valueAt(left, a)) return Truth::Unknown;
      result = result && Compare(a, pieces[i].op, *have);
    } else {
      return Truth::Unknown;
    }
  }
  return result ? Truth::True : Truth::False;
}

Truth EvalQuery(Query& q, const ComponentValues& values);

Truth EvalStyleFeature(Query& q, const ComponentValues& children) {
  // name: value, or name
  std::vector<const ComponentValue*> items;
  for (const ComponentValue& v : children) if (!v.IsWhitespace()) items.push_back(&v);
  if (items.empty() || !items[0]->IsIdent()) return Truth::Unknown;
  const std::string name = items[0]->token.value;
  const std::string have = ComputedValue(q.ctx, q.container, name.rfind("--", 0) == 0 ? name : Lower(name));
  if (items.size() == 1) return have.empty() ? Truth::False : Truth::True;
  if (items.size() < 3 || !items[1]->IsToken(Token::Type::Colon)) return Truth::Unknown;
  ComponentValues rest;
  for (size_t i = 2; i < items.size(); ++i) rest.push_back(*items[i]);
  const std::string want = Serialize(rest);
  // Compared as the container's value comes: a typed value in its serialization, a custom one in its tokens.
  const auto squeeze = [](const std::string& text) {
    std::string out;
    bool space = false;
    for (char c : text) {
      if (c == ' ' || c == '\t' || c == '\n') {
        space = !out.empty();
        continue;
      }
      if (space) out += ' ';
      space = false;
      out += c;
    }
    return out;
  };
  return squeeze(have) == squeeze(want) ? Truth::True : Truth::False;
}

Truth EvalStyleQuery(Query& q, const ComponentValues& values);

Truth EvalStyleInParens(Query& q, const ComponentValue& v) {
  if (!v.IsBlock(Token::Type::LeftParen)) return Truth::Unknown;
  size_t first = 0;
  while (first < v.children.size() && v.children[first].IsWhitespace()) ++first;
  if (first >= v.children.size()) return Truth::Unknown;
  const ComponentValue& head = v.children[first];
  const bool nested = head.IsBlock(Token::Type::LeftParen) || (head.IsIdent() && Lower(head.token.value) == "not");
  return nested ? EvalStyleQuery(q, v.children) : EvalStyleFeature(q, v.children);
}

Truth EvalStyleQuery(Query& q, const ComponentValues& values) {
  std::vector<const ComponentValue*> items;
  for (const ComponentValue& v : values) if (!v.IsWhitespace()) items.push_back(&v);
  if (items.empty()) return Truth::Unknown;
  if (items[0]->IsIdent() && Lower(items[0]->token.value) == "not") {
    if (items.size() != 2) return Truth::Unknown;
    return Not(EvalStyleInParens(q, *items[1]));
  }
  if (!items[0]->IsBlock(Token::Type::LeftParen)) return EvalStyleFeature(q, values);
  Truth result = EvalStyleInParens(q, *items[0]);
  std::string combinator;
  for (size_t i = 1; i < items.size(); i += 2) {
    if (i + 1 >= items.size() || !items[i]->IsIdent()) return Truth::Unknown;
    const std::string word = Lower(items[i]->token.value);
    if (word != "and" && word != "or") return Truth::Unknown;
    if (!combinator.empty() && combinator != word) return Truth::Unknown;
    combinator = word;
    const Truth next = EvalStyleInParens(q, *items[i + 1]);
    if (word == "and") {
      if (result == Truth::False || next == Truth::False) result = Truth::False;
      else if (result == Truth::Unknown || next == Truth::Unknown) result = Truth::Unknown;
      else result = Truth::True;
    } else {
      if (result == Truth::True || next == Truth::True) result = Truth::True;
      else if (result == Truth::Unknown || next == Truth::Unknown) result = Truth::Unknown;
      else result = Truth::False;
    }
  }
  return result;
}

Truth EvalInParens(Query& q, const ComponentValue& v) {
  if (v.IsBlock(Token::Type::LeftParen)) {
    // A nested query, or a feature.
    size_t first = 0;
    while (first < v.children.size() && v.children[first].IsWhitespace()) ++first;
    if (first >= v.children.size()) return Truth::Unknown;
    const ComponentValue& head = v.children[first];
    const bool nested = head.IsBlock(Token::Type::LeftParen) || (head.IsIdent() && Lower(head.token.value) == "not") ||
                        (head.kind == ComponentValue::Kind::Function && (Lower(head.name) == "style" || Lower(head.name) == "size" || Lower(head.name) == "scroll-state"));
    if (nested) return EvalQuery(q, v.children);
    return Feature(q, v.children);
  }
  if (v.kind == ComponentValue::Kind::Function) {
    const std::string name = Lower(v.name);
    if (name == "size") return EvalQuery(q, v.children);
    if (name == "style") return EvalStyleQuery(q, v.children);
    return Truth::Unknown;  // scroll-state() and what is not known
  }
  return Truth::Unknown;
}

Truth EvalQuery(Query& q, const ComponentValues& values) {
  std::vector<const ComponentValue*> items;
  for (const ComponentValue& v : values) if (!v.IsWhitespace()) items.push_back(&v);
  if (items.empty()) return Truth::Unknown;
  if (items[0]->IsIdent() && Lower(items[0]->token.value) == "not") {
    if (items.size() != 2) return Truth::Unknown;
    return Not(EvalInParens(q, *items[1]));
  }
  Truth result = EvalInParens(q, *items[0]);
  std::string combinator;
  for (size_t i = 1; i < items.size(); i += 2) {
    if (i + 1 >= items.size() || !items[i]->IsIdent()) return Truth::Unknown;
    const std::string word = Lower(items[i]->token.value);
    if (word != "and" && word != "or") return Truth::Unknown;
    if (!combinator.empty() && combinator != word) return Truth::Unknown;
    combinator = word;
    const Truth next = EvalInParens(q, *items[i + 1]);
    if (word == "and") {
      if (result == Truth::False || next == Truth::False) result = Truth::False;
      else if (result == Truth::Unknown || next == Truth::Unknown) result = Truth::Unknown;
      else result = Truth::True;
    } else {
      if (result == Truth::True || next == Truth::True) result = Truth::True;
      else if (result == Truth::Unknown || next == Truth::Unknown) result = Truth::Unknown;
      else result = Truth::False;
    }
  }
  return result;
}

// The facts about an element as a container.
struct ContainerFacts {
  bool size = false, inlineSize = false;
  bool eligible = true;  // size containment applies to it (it has a box of its own that is not a table or ruby)
  std::vector<std::string> names;
};

ContainerFacts FactsOf(Quanta::Context& ctx, dom::Element* element) {
  ContainerFacts facts;
  // Size containment does not apply to everything: tables, ruby and what has no box of its own are not containers.
  const std::string display = ComputedValue(ctx, element, "display");
  const bool eligible = !(display == "inline" || display == "contents" || display == "none" || display.rfind("table", 0) == 0 || display == "inline-table" || display.rfind("ruby", 0) == 0);
  facts.eligible = eligible;
  const std::string type = ComputedValue(ctx, element, "container-type");
  facts.size = type.find("size") != std::string::npos && type.find("inline-size") == std::string::npos;
  facts.inlineSize = type.find("inline-size") != std::string::npos;
  const std::string names = ComputedValue(ctx, element, "container-name");
  size_t at = 0;
  while (at < names.size()) {
    while (at < names.size() && names[at] == ' ') ++at;
    size_t end = names.find(' ', at);
    if (end == std::string::npos) end = names.size();
    if (end > at) facts.names.push_back(names.substr(at, end - at));
    at = end;
  }
  if (facts.names.size() == 1 && facts.names[0] == "none") facts.names.clear();
  return facts;
}

}  // namespace

bool ContainerRuleMatches(Quanta::Context& ctx, dom::Element* element, bool pseudoElement, const std::string& prelude) {
  const ComponentValues parsed = ParseComponentValues(prelude);
  for (const ComponentValues& condition : SplitOnCommas(parsed)) {
    ComponentValues trimmed = Trimmed(condition);
    if (trimmed.empty()) continue;
    std::string name;
    size_t start = 0;
    if (trimmed[0].IsIdent() && Lower(trimmed[0].token.value) != "not") {
      name = trimmed[0].token.value;
      start = 1;
    }
    ComponentValues query(trimmed.begin() + static_cast<long>(start), trimmed.end());
    query = Trimmed(query);
    // What kind of container the query needs.
    bool needsSize = false;
    {
      const std::vector<const ComponentValue*> stack = [&] {
        std::vector<const ComponentValue*> out;
        for (const ComponentValue& v : query) out.push_back(&v);
        return out;
      }();
      std::vector<const ComponentValue*> walk = stack;
      while (!walk.empty()) {
        const ComponentValue* v = walk.back();
        walk.pop_back();
        if (v->kind == ComponentValue::Kind::Function && Lower(v->name) == "style") continue;
        if (v->IsBlock(Token::Type::LeftParen)) {
          bool nested = false;
          for (const ComponentValue& c : v->children) {
            if (c.IsWhitespace()) continue;
            nested = c.IsBlock(Token::Type::LeftParen) || (c.IsIdent() && Lower(c.token.value) == "not") || c.kind == ComponentValue::Kind::Function;
            break;
          }
          if (!nested) needsSize = true;
        }
        for (const ComponentValue& c : v->children) walk.push_back(&c);
      }
    }
    // The nearest ancestor that is such a container (the element itself, for a pseudo-element).
    dom::Element* candidate = pseudoElement ? element : FlatParentOf(element);
    for (; candidate; candidate = FlatParentOf(candidate)) {
      const ContainerFacts facts = FactsOf(ctx, candidate);
      if (!name.empty() && std::find(facts.names.begin(), facts.names.end(), name) == facts.names.end()) continue;
      if (needsSize && !facts.size && !facts.inlineSize) continue;
      // The nearest container is the one asked, even if it cannot be one: then the query does not match.
      if (needsSize && !facts.eligible) break;
      Query q{ctx, candidate};
      q.sizeContainer = facts.size || facts.inlineSize;
      q.inlineOnly = facts.inlineSize && !facts.size;
      const std::string mode = ComputedValue(ctx, candidate, "writing-mode");
      q.vertical = mode.rfind("vertical", 0) == 0 || mode.rfind("sideways", 0) == 0;
      q.width = candidate->containerWidth;
      q.height = candidate->containerHeight;
      if (query.empty()) return true;
      if (EvalQuery(q, query) == Truth::True) return true;
      break;
    }
  }
  return false;
}

bool NearestContainerSize(Quanta::Context& ctx, dom::Element* element, bool pseudoElement, double& width, double& height, bool& vertical) {
  dom::Element* candidate = pseudoElement ? element : FlatParentOf(element);
  for (; candidate; candidate = FlatParentOf(candidate)) {
    const ContainerFacts facts = FactsOf(ctx, candidate);
    if (!facts.size && !facts.inlineSize) continue;
    if (!facts.eligible) return false;
    if (candidate->containerWidth < 0 || candidate->containerHeight < 0) return false;
    width = candidate->containerWidth;
    height = candidate->containerHeight;
    const std::string mode = ComputedValue(ctx, candidate, "writing-mode");
    vertical = mode.rfind("vertical", 0) == 0 || mode.rfind("sideways", 0) == 0;
    return true;
  }
  return false;
}

}  // namespace solar::css
