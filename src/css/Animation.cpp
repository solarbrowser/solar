/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <set>
// The values animations give to properties, and how two values of a property are combined (interpolated, added).
#include "solar/css/Animation.h"

#include <algorithm>
#include <cmath>

#include "solar/css/Properties.h"
#include "solar/css/Style.h"
#include "solar/css/Values.h"

namespace solar::css {

namespace {

bool g_suppressed = false;
int g_noFlush = 0;

std::string LowerAscii(std::string text) {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return text;
}

// ---- Combining ----
//
// Every combination is wa * a + wb * b: interpolation is (1 - t, t), addition (1, 1), scaling (factor, 0). The values are taken
// apart into the same shape (numbers, percentages, dimensions of one unit, functions of the same name, equal keywords, colors) and
// each part is combined with its counterpart; where the shapes differ there is nothing to combine.

struct Weights {
  double a = 1, b = 0;
};

struct Item {
  const ComponentValue* value = nullptr;
  char separator = ' ';  // what comes before it: ' ', ',' or '/'
};

std::vector<Item> ItemsOf(const ComponentValues& values) {
  std::vector<Item> items;
  char separator = ' ';
  for (const ComponentValue& v : values) {
    if (v.IsWhitespace()) continue;
    if (v.IsToken(Token::Type::Comma)) {
      separator = ',';
      continue;
    }
    if (v.IsDelim('/')) {
      separator = '/';
      continue;
    }
    items.push_back({&v, separator});
    separator = ' ';
  }
  return items;
}

// A length and a percentage, the two parts of a <length-percentage> (px and %).
struct LengthPercentage {
  double px = 0, pct = 0;
};

bool ReadLengthPercentage(const ComponentValue& v, LengthPercentage& out, double sign = 1);

bool ReadCalcTerms(const ComponentValues& values, LengthPercentage& out) {
  double sign = 1;
  bool expectOperand = true;
  for (const ComponentValue& v : values) {
    if (v.IsWhitespace()) continue;
    if (v.IsDelim('+') || v.IsDelim('-')) {
      if (expectOperand) return false;
      sign = v.IsDelim('+') ? 1 : -1;
      expectOperand = true;
      continue;
    }
    if (!expectOperand) return false;
    LengthPercentage term;
    if (!ReadLengthPercentage(v, term, sign)) return false;
    out.px += term.px;
    out.pct += term.pct;
    expectOperand = false;
    sign = 1;
  }
  return !expectOperand;
}

bool ReadLengthPercentage(const ComponentValue& v, LengthPercentage& out, double sign) {
  if (v.IsToken(Token::Type::Percentage)) {
    out = {0, sign * v.token.number};
    return true;
  }
  if (v.IsToken(Token::Type::Dimension) && LowerAscii(v.token.value) == "px") {
    out = {sign * v.token.number, 0};
    return true;
  }
  if (v.IsToken(Token::Type::Number) && v.token.number == 0) {
    out = {0, 0};
    return true;
  }
  if (v.kind == ComponentValue::Kind::Function && LowerAscii(v.name) == "calc") {
    LengthPercentage sum;
    if (!ReadCalcTerms(v.children, sum)) return false;
    out = {sign * sum.px, sign * sum.pct};
    return true;
  }
  return false;
}

std::string LengthPercentageText(double px, double pct) {
  if (std::fabs(px) < 1e-9) px = 0;
  if (std::fabs(pct) < 1e-9) pct = 0;
  if (pct == 0) return FormatNumber(px) + "px";
  if (px == 0) return FormatNumber(pct) + "%";
  const bool negative = px < 0;
  return "calc(" + FormatNumber(pct) + "% " + (negative ? "- " : "+ ") + FormatNumber(std::fabs(px)) + "px)";
}

struct Rgba {
  double r = 0, g = 0, b = 0, a = 1;
};

bool ReadColor(const ComponentValue& v, Rgba& out) {
  if (v.IsIdent() && LowerAscii(v.token.value) == "transparent") {
    out = {0, 0, 0, 0};
    return true;
  }
  if (v.kind != ComponentValue::Kind::Function) return false;
  const std::string name = LowerAscii(v.name);
  if (name != "rgb" && name != "rgba") return false;
  std::vector<double> numbers;
  for (const ComponentValue& child : v.children) {
    if (child.IsToken(Token::Type::Number)) numbers.push_back(child.token.number);
    else if (child.IsToken(Token::Type::Percentage)) numbers.push_back(child.token.number / 100);
    else if (!(child.IsWhitespace() || child.IsToken(Token::Type::Comma) || child.IsDelim('/'))) return false;
  }
  if (numbers.size() != 3 && numbers.size() != 4) return false;
  out = {numbers[0], numbers[1], numbers[2], numbers.size() == 4 ? numbers[3] : 1};
  return true;
}

std::string ColorText(Rgba c) {
  c.a = std::clamp(c.a, 0.0, 1.0);
  const auto channel = [](double v) { return std::lround(std::clamp(v, 0.0, 255.0)); };
  if (c.a >= 1) return "rgb(" + std::to_string(channel(c.r)) + ", " + std::to_string(channel(c.g)) + ", " + std::to_string(channel(c.b)) + ")";
  return "rgba(" + std::to_string(channel(c.r)) + ", " + std::to_string(channel(c.g)) + ", " + std::to_string(channel(c.b)) + ", " +
         FormatNumber(std::round(c.a * 100) / 100) + ")";
}

bool CombineSequence(const ComponentValues& a, const ComponentValues& b, Weights w, std::string& out);

bool CombineItem(const ComponentValue& a, const ComponentValue& b, Weights w, std::string& out) {
  using T = Token::Type;
  if (a.kind == ComponentValue::Kind::Token && b.kind == ComponentValue::Kind::Token) {
    if (a.token.type == T::Number && b.token.type == T::Number) {
      const double v = w.a * a.token.number + w.b * b.token.number;
      out = FormatNumber(v);
      return true;
    }
    if (a.token.type == T::Percentage && b.token.type == T::Percentage) {
      out = FormatNumber(w.a * a.token.number + w.b * b.token.number) + "%";
      return true;
    }
    if (a.token.type == T::Dimension && b.token.type == T::Dimension && LowerAscii(a.token.value) == LowerAscii(b.token.value)) {
      out = FormatNumber(w.a * a.token.number + w.b * b.token.number) + LowerAscii(a.token.value);
      return true;
    }
    if (a.token.type == T::Ident && b.token.type == T::Ident && LowerAscii(a.token.value) == LowerAscii(b.token.value)) {
      out = a.token.value;
      return true;
    }
  }
  Rgba ca, cb;
  if (ReadColor(a, ca) && ReadColor(b, cb)) {
    // Premultiplied: a color with no alpha does not pull the others toward black.
    const double alpha = std::clamp(w.a * ca.a + w.b * cb.a, 0.0, 1.0);
    Rgba c;
    c.a = alpha;
    if (alpha > 0) {
      c.r = (w.a * ca.r * ca.a + w.b * cb.r * cb.a) / alpha;
      c.g = (w.a * ca.g * ca.a + w.b * cb.g * cb.a) / alpha;
      c.b = (w.a * ca.b * ca.a + w.b * cb.b * cb.a) / alpha;
    }
    out = ColorText(c);
    return true;
  }
  LengthPercentage la, lb;
  if (ReadLengthPercentage(a, la) && ReadLengthPercentage(b, lb)) {
    out = LengthPercentageText(w.a * la.px + w.b * lb.px, w.a * la.pct + w.b * lb.pct);
    return true;
  }
  if (a.kind == ComponentValue::Kind::Function && b.kind == ComponentValue::Kind::Function && LowerAscii(a.name) == LowerAscii(b.name)) {
    std::string inner;
    if (!CombineSequence(a.children, b.children, w, inner)) return false;
    out = LowerAscii(a.name) + "(" + inner + ")";
    return true;
  }
  return false;
}

bool CombineSequence(const ComponentValues& a, const ComponentValues& b, Weights w, std::string& out) {
  const std::vector<Item> ia = ItemsOf(a), ib = ItemsOf(b);
  if (ia.size() != ib.size()) return false;
  out.clear();
  for (size_t i = 0; i < ia.size(); ++i) {
    if (ia[i].separator != ib[i].separator) return false;
    std::string piece;
    if (!CombineItem(*ia[i].value, *ib[i].value, w, piece)) return false;
    if (i > 0) out += ia[i].separator == ',' ? ", " : ia[i].separator == '/' ? " / " : " ";
    out += piece;
  }
  return true;
}

bool IsKeywordList(const std::string& property) { return property == "transform" || property == "filter" || property == "backdrop-filter"; }

std::optional<std::string> Combine(const std::string& property, const std::string& from, const std::string& to, Weights w) {
  const PropertyDefinition* definition = FindProperty(property);
  if (!definition || IsShorthand(*definition)) return std::nullopt;
  const ComponentValues a = Trimmed(ParseComponentValues(from)), b = Trimmed(ParseComponentValues(to));
  if (a.empty() || b.empty()) return std::nullopt;
  std::string out;
  if (!CombineSequence(a, b, w, out)) return std::nullopt;
  return ClampValue(property, out);
}

}  // namespace

NoStyleFlush::NoStyleFlush() { ++g_noFlush; }
NoStyleFlush::~NoStyleFlush() { --g_noFlush; }
bool StyleFlushSuppressed() { return g_noFlush > 0; }

// ---- What animations give ----

int g_animatedSlots = 0;  // how many (element, pseudo-element) pairs have values: without any, there is nothing to suppress

size_t g_sizingAnimations = 0;
bool AnyAnimationAffectsSizes() { return g_sizingAnimations > 0; }

namespace {
// What an animation can give that nothing else is made from: not inherited, and not a length the others take their own from.
bool OnlyItself(const AnimatedValues& values) {
  static const std::set<std::string> alone = {"opacity", "transform", "translate", "rotate", "scale", "filter", "backdrop-filter", "box-shadow", "clip-path", "background-color",
                                              "border-color", "outline-color", "background-position", "mask-position", "text-shadow-none"};
  for (const auto& entry : values) {
    if (!alone.count(entry.first)) return false;
  }
  return true;
}
}  // namespace

void SetAnimatedValues(dom::Element* element, const std::string& pseudo, AnimatedValues values) {
  if (!element) return;
  {
    const auto before = element->animatedValues.find(pseudo);
    const bool wasAlone = before == element->animatedValues.end() || OnlyItself(before->second);
    if (!wasAlone || !OnlyItself(values)) ++element->animInheritVersion;
    dom::MarkPending(element, dom::kPendingVersions | dom::kPendingAnimated);
  }
  if (values.empty()) {
    const auto at = element->animatedValues.find(pseudo);
    if (at == element->animatedValues.end()) return;
    element->animatedValues.erase(at);
    --g_animatedSlots;
  } else {
    auto at = element->animatedValues.find(pseudo);
    if (at == element->animatedValues.end()) {
      element->animatedValues.emplace(pseudo, std::move(values));
      ++g_animatedSlots;
    } else {
      if (at->second == values) return;
      at->second = std::move(values);
    }
  }
  ++element->animVersion;
  // (This is script's doing, as the flush of animations is script: the pending tree is what it walks.)
  for (dom::Node* n = element; n; n = n->parentNode) {
    if (n->IsElement()) {
      ++static_cast<dom::Element*>(n)->subtreeVersion;
      dom::MarkPending(n, dom::kPendingVersions);
    }
  }
  // What only changes how an element is drawn is nothing for a container query to see.
  static const std::set<std::string> drawOnly = {"opacity", "transform", "translate", "rotate", "scale", "color", "background-color", "border-color", "outline-color", "fill", "stroke",
                                                 "stroke-dashoffset", "stroke-dasharray", "filter", "backdrop-filter", "box-shadow", "text-shadow", "clip-path", "mask-position",
                                                 "background-position", "stop-color", "fill-opacity", "stroke-opacity"};
  static const std::set<std::string> notInherited = {"opacity", "transform", "translate", "rotate", "scale", "filter", "backdrop-filter", "box-shadow", "clip-path", "background-color",
                                                     "border-color", "outline-color", "background-position", "mask-position"};
  static std::set<const dom::Element*> sizingElements;
  bool sizing = false, itself = true;
  const auto now = element->animatedValues.find(pseudo);
  if (now != element->animatedValues.end()) {
    for (const auto& entry : now->second) {
      sizing = sizing || !drawOnly.count(entry.first);
      itself = itself && notInherited.count(entry.first);
    }
  } else {
    itself = false;  // (it ended: what was there before is what changes)
  }
  const bool wasSizing = sizingElements.count(element) != 0;
  if (sizing) sizingElements.insert(element);
  else sizingElements.erase(element);
  g_sizingAnimations = sizingElements.size();
  sizing = sizing || wasSizing;
  NoteAnimatedStyleChange(sizing, itself);
}

const std::string* AnimatedValue(dom::Element* element, const std::string& pseudo, const std::string& property) {
  if (g_suppressed || !element) return nullptr;
  const auto& animated = element->AnimatedValuesInView();
  if (animated.empty()) return nullptr;
  const auto at = animated.find(pseudo);
  if (at == animated.end()) return nullptr;
  const auto value = at->second.find(property);
  return value == at->second.end() ? nullptr : &value->second;
}

void SuppressAnimatedValues(bool suppressed) {
  if (g_suppressed == suppressed) return;
  g_suppressed = suppressed;  // the styles it asks for are kept apart from the others, so nothing needs to be made again
}

bool AnimatedValuesSuppressed() { return g_suppressed; }

namespace {
const dom::Element* g_startingElement = nullptr;
std::string g_startingPseudo;
}  // namespace

void SetStartingStyleElement(const dom::Element* element, const std::string& pseudo) {
  g_startingElement = element;
  g_startingPseudo = pseudo;
}

bool StartingStyleFor(const dom::Element* element, const std::string& pseudo) { return g_startingElement && element == g_startingElement && pseudo == g_startingPseudo; }

// ---- Combining values ----

std::optional<std::string> InterpolateValues(const std::string& property, const std::string& from, const std::string& to, double progress) {
  return Combine(property, from, to, {1 - progress, progress});
}

std::optional<std::string> AddValues(const std::string& property, const std::string& a, const std::string& b) {
  if (IsKeywordList(property)) {
    // The lists are put one after the other.
    if (LowerAscii(a) == "none") return b;
    if (LowerAscii(b) == "none") return a;
    return a + " " + b;
  }
  if (property == "box-shadow" || property == "text-shadow") {
    if (LowerAscii(a) == "none") return b;
    if (LowerAscii(b) == "none") return a;
    return a + ", " + b;
  }
  return Combine(property, a, b, {1, 1});
}

std::optional<std::string> ScaleValue(const std::string& property, const std::string& value, double factor) {
  return Combine(property, value, value, {factor, 0});
}

bool IsInterpolableProperty(const std::string& property) {
  const PropertyDefinition* definition = FindProperty(property);
  if (!definition || IsShorthand(*definition)) return false;
  const std::string syntax = definition->syntax;
  return syntax.find("<length") != std::string::npos || syntax.find("<number") != std::string::npos || syntax.find("<integer") != std::string::npos ||
         syntax.find("<percentage") != std::string::npos || syntax.find("<color>") != std::string::npos || syntax.find("<angle") != std::string::npos ||
         syntax.find("<opacity-value>") != std::string::npos || property == "transform" || property == "visibility";
}

std::string ClampValue(const std::string& property, const std::string& value) {
  const PropertyDefinition* definition = FindProperty(property);
  if (!definition) return value;
  const ComponentValues parsed = Trimmed(ParseComponentValues(value));
  if (parsed.size() != 1) return value;
  const ComponentValue& v = parsed[0];
  const std::string syntax = definition->syntax;
  const bool unit = property == "opacity" || property == "fill-opacity" || property == "stroke-opacity" || property == "shape-image-threshold" ||
                    property == "flood-opacity" || property == "stop-opacity";
  const bool integer = syntax.find("<integer") != std::string::npos && syntax.find("<number") == std::string::npos;
  if (v.IsToken(Token::Type::Number)) {
    double n = v.token.number;
    if (unit) n = std::clamp(n, 0.0, 1.0);
    else if (syntax.find("[0,∞]") != std::string::npos) n = std::max(n, 0.0);
    if (integer) n = std::round(n);
    return FormatNumber(n);
  }
  if ((v.IsToken(Token::Type::Dimension) || v.IsToken(Token::Type::Percentage)) && v.token.number < 0 && syntax.find("[0,∞]") != std::string::npos) {
    return v.IsToken(Token::Type::Percentage) ? "0%" : "0" + LowerAscii(v.token.value);
  }
  return value;
}

}  // namespace solar::css
