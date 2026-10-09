#include "solar/css/Registry.h"

#include <algorithm>
#include <map>
#include <set>

#include "solar/css/Calc.h"
#include "solar/css/Cssom.h"
#include "solar/css/Values.h"

namespace solar::css {

namespace {

using T = Token::Type;

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string TrimSpace(const std::string& text) {
  size_t begin = 0, end = text.size();
  const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; };
  while (begin < end && space(text[begin])) ++begin;
  while (end > begin && space(text[end - 1])) --end;
  return text.substr(begin, end - begin);
}

std::map<dom::Document*, std::map<std::string, RegisteredProperty>>& Registered() {
  static std::map<dom::Document*, std::map<std::string, RegisteredProperty>> map;
  return map;
}

// A value that does not depend on anything else: no var(), no font-relative lengths.
bool ComputationallyIndependent(const ComponentValues& values) {
  for (const ComponentValue& v : values) {
    if (v.kind == ComponentValue::Kind::Function) {
      const std::string name = Lower(v.name);
      if (name == "var" || name == "env" || name == "attr") return false;
    }
    if (v.kind == ComponentValue::Kind::Token && v.token.type == T::Dimension) {
      static const char* const relative[] = {"em", "ex", "ch", "cap", "ic", "lh", "rem", "rex", "rch", "rcap", "ric", "rlh"};
      const std::string unit = Lower(v.token.value);
      for (const char* r : relative) {
        if (unit == r) return false;
      }
    }
    if (v.kind != ComponentValue::Kind::Token && !ComputationallyIndependent(v.children)) return false;
  }
  return true;
}

}  // namespace

std::optional<std::string> ParseSyntaxDefinition(const std::string& text) {
  const std::string trimmed = TrimSpace(text);
  if (trimmed.empty()) return std::nullopt;
  if (trimmed == "*") return std::string("*");
  static const std::set<std::string> types = {"angle", "color", "custom-ident", "image", "integer", "length", "length-percentage", "number", "percentage",
                                              "resolution", "string", "time", "transform-function", "transform-list", "url"};
  std::string out;
  size_t start = 0;
  for (;;) {
    const size_t bar = trimmed.find('|', start);
    const std::string part = TrimSpace(trimmed.substr(start, bar == std::string::npos ? std::string::npos : bar - start));
    if (part.empty()) return std::nullopt;
    std::string item;
    if (part[0] == '<') {
      const size_t close = part.find('>');
      if (close == std::string::npos) return std::nullopt;
      const std::string name = part.substr(1, close - 1);
      const std::string rest = TrimSpace(part.substr(close + 1));
      if (!types.count(name)) return std::nullopt;
      if (!rest.empty() && rest != "+" && rest != "#") return std::nullopt;
      if (!rest.empty() && name == "transform-list") return std::nullopt;
      item = "<" + name + ">" + rest;
    } else {
      // A keyword: one identifier, as written.
      const std::vector<Token> tokens = Tokenize(part);
      if (tokens.size() != 2 || tokens[0].type != T::Ident || tokens[1].type != T::EndOfFile) return std::nullopt;
      const std::string lower = Lower(tokens[0].value);
      if (lower == "inherit" || lower == "initial" || lower == "unset" || lower == "revert" || lower == "revert-layer" || lower == "default") return std::nullopt;
      item = SerializeIdentifier(tokens[0].value);
    }
    out += (out.empty() ? "" : " | ") + item;
    if (bar == std::string::npos) break;
    start = bar + 1;
  }
  return out;
}

std::optional<RegisteredProperty> MakeRegistration(const std::string& name, const std::string& syntax, bool inherits, const std::optional<std::string>& initialValue, std::string& error) {
  RegisteredProperty property;
  property.name = name;
  property.inherits = inherits;
  property.syntax = syntax;
  const std::optional<std::string> matcher = ParseSyntaxDefinition(syntax);
  if (!matcher) {
    error = "SyntaxError";
    return std::nullopt;
  }
  property.matcherSyntax = *matcher;
  if (property.universal()) {
    if (initialValue) {
      // Any token stream will do, but it must parse.
      const ComponentValues values = ParseComponentValues(*initialValue);
      if (!ComputationallyIndependent(values)) {
        error = "SyntaxError";
        return std::nullopt;
      }
      property.initialValue = Serialize(Trimmed(values));
    }
    return property;
  }
  if (!initialValue) {
    error = "TypeError";
    return std::nullopt;
  }
  const ComponentValues values = Trimmed(ParseComponentValues(*initialValue));
  ValueMatch match;
  if (!ComputationallyIndependent(values) || values.empty() || !MatchSyntax(property.matcherSyntax, values, match)) {
    error = "SyntaxError";
    return std::nullopt;
  }
  property.initialValue = SerializeValue(match.normalized);
  return property;
}

bool RegisterProperty(dom::Document* document, const RegisteredProperty& property, std::string& error) {
  auto& map = Registered()[document];
  if (map.count(property.name)) {
    error = "InvalidModificationError";
    return false;
  }
  map[property.name] = property;
  NoteStyleChangeForRegistry();
  return true;
}

namespace {

// The registrations @property rules make: the last valid one for a name in the sheets of the document.
void CollectRules(const std::vector<CssRule*>& rules, std::map<std::string, RegisteredProperty>& out) {
  for (const CssRule* rule : rules) {
    if (rule->kind == RuleKind::Property && rule->registered) out[rule->name] = *rule->registered;
    else if (rule->kind == RuleKind::Media || rule->kind == RuleKind::Supports || rule->kind == RuleKind::LayerBlock) CollectRules(rule->rules, out);
  }
}

}  // namespace

std::optional<RegisteredProperty> LookupRegistered(dom::Document* document, const std::string& name) {
  if (!document) return std::nullopt;
  // The API wins over @property.
  const auto api = Registered().find(document);
  if (api != Registered().end()) {
    const auto found = api->second.find(name);
    if (found != api->second.end()) return found->second;
  }
  std::map<std::string, RegisteredProperty> fromRules;
  for (dom::Node* node = document; node; node = node->NextInTree(document)) {
    dom::Element* element = dom::AsElement(node);
    if (element && element->styleSheet) {
      const CssStyleSheet* sheet = static_cast<const CssStyleSheet*>(element->styleSheet);
      if (!sheet->disabled) CollectRules(sheet->rules, fromRules);
    }
  }
  const auto found = fromRules.find(name);
  if (found == fromRules.end()) return std::nullopt;
  return found->second;
}

std::vector<std::string> RegisteredNames(dom::Document* document) {
  std::vector<std::string> names;
  const auto api = Registered().find(document);
  if (api != Registered().end()) {
    for (const auto& [name, property] : api->second) names.push_back(name);
  }
  std::map<std::string, RegisteredProperty> fromRules;
  for (dom::Node* node = document; node; node = node->NextInTree(document)) {
    dom::Element* element = dom::AsElement(node);
    if (element && element->styleSheet) CollectRules(static_cast<const CssStyleSheet*>(element->styleSheet)->rules, fromRules);
  }
  for (const auto& [name, property] : fromRules) {
    if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
  }
  return names;
}

}  // namespace solar::css
