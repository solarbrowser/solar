#include "solar/css/Shorthands.h"

#include <algorithm>
#include <cstring>
#include <map>
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

// Properties the specifications give longhands to that browsers (and so pages) take as longhands themselves.
bool IsRealShorthand(const PropertyDefinition& property) {
  if (property.longhands.empty()) return false;
  static const std::set<std::string> notShorthands = {"text-align", "vertical-align", "text-decoration-skip", "line-clamp", "-webkit-line-clamp", "text-spacing"};
  return notShorthands.count(property.name) == 0;
}

bool EndsWith(const std::string& text, const char* suffix) { return text.size() >= std::strlen(suffix) && text.compare(text.size() - std::strlen(suffix), std::string::npos, suffix) == 0; }

// "<'margin-top'>{1,4}" and its kind: one term, repeated one to four (or two) times, each one component.
bool IsEdgeShorthand(const PropertyDefinition& property) {
  const std::string syntax = property.syntax;
  if (!(EndsWith(syntax, "{1,4}") || EndsWith(syntax, "{1,2}"))) return false;
  if (syntax.find("||") != std::string::npos || syntax.find('/') != std::string::npos) return false;
  const size_t n = property.longhands.size();
  return n == 4 || n == 2;
}

std::vector<size_t> EdgeIndices(size_t count, size_t longhands) {
  if (longhands == 2) return count == 1 ? std::vector<size_t>{0, 0} : std::vector<size_t>{0, 1};
  switch (count) {
    case 1: return {0, 0, 0, 0};
    case 2: return {0, 1, 0, 1};
    case 3: return {0, 1, 2, 1};
    default: return {0, 1, 2, 3};
  }
}

void FlattenLeavesImpl(const PropertyDefinition& property, std::vector<std::string>& leaves, bool withResets = true) {
  if (!IsRealShorthand(property)) {
    leaves.push_back(property.name);
    return;
  }
  for (const char* name : property.longhands) {
    const PropertyDefinition* inner = FindProperty(name);
    if (inner) FlattenLeavesImpl(*inner, leaves, withResets);
    else leaves.push_back(name);
  }
  if (!withResets) return;
  for (const char* name : property.resetLonghands) {
    const PropertyDefinition* inner = FindProperty(name);
    if (inner) FlattenLeavesImpl(*inner, leaves, withResets);
    else leaves.push_back(name);
  }
}

}  // namespace

std::string InitialValueText(const PropertyDefinition& property) {
  const std::string initial = property.initial;
  if (initial.empty() || !property.longhands.empty()) return "initial";
  const ComponentValues values = Trimmed(ParseComponentValues(initial));
  ValueMatch match;
  if (!values.empty() && MatchPropertyValue(property, values, match)) return SerializeValue(match.normalized);
  return "initial";
}

bool IsShorthandProperty(const PropertyDefinition& property) { return IsRealShorthand(property); }

std::vector<std::string> LeavesOf(const PropertyDefinition& property) {
  std::vector<std::string> leaves;
  FlattenLeavesImpl(property, leaves);
  return leaves;
}

std::vector<std::string> ResetLeavesOf(const PropertyDefinition& property) {
  std::vector<std::string> leaves;
  for (const char* name : property.resetLonghands) {
    const PropertyDefinition* inner = FindProperty(name);
    if (inner) FlattenLeavesImpl(*inner, leaves);
    else leaves.push_back(name);
  }
  return leaves;
}

const std::vector<const PropertyDefinition*>& ShorthandsOf(const std::string& longhand) {
  static const std::unordered_map<std::string, std::vector<const PropertyDefinition*>> index = [] {
    std::unordered_map<std::string, std::vector<const PropertyDefinition*>> map;
    for (size_t i = 0; i < kPropertyDefinitionCount; ++i) {
      const PropertyDefinition& property = kPropertyDefinitions[i];
      if (!IsRealShorthand(property)) continue;
      for (const std::string& leaf : LeavesOf(property)) map[leaf].push_back(&property);
    }
    for (auto& [name, list] : map) {
      std::stable_sort(list.begin(), list.end(), [](const PropertyDefinition* a, const PropertyDefinition* b) { return LeavesOf(*a).size() > LeavesOf(*b).size(); });
    }
    return map;
  }();
  static const std::vector<const PropertyDefinition*> none;
  const auto found = index.find(longhand);
  return found == index.end() ? none : found->second;
}

// ---- Expansion ----

namespace {

bool ExpandValue(const PropertyDefinition& property, const ComponentValues& values, std::vector<Longhand>& out);

void FlattenLeaves(const PropertyDefinition& property, std::vector<std::string>& leaves) { FlattenLeavesImpl(property, leaves); }

// A longhand of a shorthand takes `text`; if it is a shorthand too it is expanded in turn.
void ExpandLonghand(const std::string& name, const std::string& text, std::vector<Longhand>& out) {
  const PropertyDefinition* property = FindProperty(name);
  const ComponentValues values = Trimmed(ParseComponentValues(text));
  if (!property) {
    out.push_back({name, text, "", ""});
    return;
  }
  if (!ExpandValue(*property, values, out)) out.push_back({name, text, "", ""});
}

bool Overlaps(const std::vector<bool>& claimed, size_t begin, size_t end) {
  for (size_t i = begin; i < end && i < claimed.size(); ++i) {
    if (claimed[i]) return true;
  }
  return false;
}

// The longhand of `shorthand` that takes what the type `<name>` matched.
std::string LonghandForType(const PropertyDefinition& shorthand, const std::string& typeName) {
  static const std::map<std::string, std::string> special = {{"<font-variant-css2>", "font-variant"}, {"<font-width-css3>", "font-stretch"}};
  if (const auto found = special.find(typeName); found != special.end()) {
    for (const char* name : shorthand.longhands) {
      if (found->second == name) return name;
    }
  }
  for (const char* name : shorthand.longhands) {
    const PropertyDefinition* longhand = FindProperty(name);
    if (!longhand) continue;
    const std::string syntax = longhand->syntax;
    // "<line-width>" in "<line-width>" and in "<'border-top-width'>{1,4}"-less syntaxes: the longhand takes the type.
    size_t at = syntax.find(typeName);
    while (at != std::string::npos) {
      const size_t after = at + typeName.size();
      if (after >= syntax.size() || syntax[after] != '[') return name;
      at = syntax.find(typeName, after);
    }
    if (IsRealShorthand(*longhand)) {
      // A nested shorthand takes it when one of its own longhands does.
      if (!LonghandForType(*longhand, typeName).empty()) return name;
    }
  }
  return "";
}

std::string Join(const ComponentValues& values, size_t begin, size_t end) {
  ComponentValues slice(values.begin() + begin, values.begin() + end);
  return SerializeValue(slice);
}

bool ExpandBorderRadius(const PropertyDefinition& property, const ComponentValues& normalized, std::vector<Longhand>& out) {
  std::vector<ComponentValues> parts(1);
  for (const ComponentValue& v : normalized) {
    if (v.IsDelim('/')) parts.emplace_back();
    else parts.back().push_back(v);
  }
  if (parts.size() > 2 || parts[0].empty() || parts[0].size() > 4 || (parts.size() == 2 && (parts[1].empty() || parts[1].size() > 4))) return false;
  const std::vector<size_t> horizontal = EdgeIndices(parts[0].size(), 4);
  const std::vector<size_t> vertical = parts.size() == 2 ? EdgeIndices(parts[1].size(), 4) : horizontal;
  for (size_t i = 0; i < 4 && i < property.longhands.size(); ++i) {
    std::string text = SerializeValue({parts[0][horizontal[i]]});
    if (parts.size() == 2) text += " " + SerializeValue({parts[1][vertical[i]]});
    out.push_back({property.longhands[i], text, "", ""});
  }
  return true;
}

bool ExpandShorthand(const PropertyDefinition& property, const ComponentValues& values, std::vector<Longhand>& out) {
  ValueMatch match;
  if (!MatchPropertyValue(property, values, match)) return false;
  const ComponentValues& items = match.normalized;
  const std::string name = property.name;
  std::vector<Longhand> mine;
  const auto add = [&](const std::string& longhand, const std::string& text) { ExpandLonghand(longhand, text, mine); };

  if (name == "border-radius" || name == "-webkit-border-radius") {
    if (!ExpandBorderRadius(property, items, mine)) return false;
    for (Longhand& l : mine) out.push_back(std::move(l));
    return true;
  }
  if (IsEdgeShorthand(property)) {
    const std::vector<size_t> at = EdgeIndices(items.size(), property.longhands.size());
    if (items.empty() || items.size() > property.longhands.size()) return false;
    for (size_t i = 0; i < property.longhands.size(); ++i) add(property.longhands[i], SerializeValue({items[at[i]]}));
    for (Longhand& l : mine) out.push_back(std::move(l));
    return true;
  }
  if (name == "flex" && items.size() == 1 && items[0].IsIdent() && Lower(items[0].token.value) == "none") {
    add("flex-grow", "0");
    add("flex-shrink", "0");
    add("flex-basis", "auto");
    for (Longhand& l : mine) out.push_back(std::move(l));
    return true;
  }

  // What each longhand took: the largest matches first, each component to one longhand.
  std::vector<ValueMatch::Assignment> assigned = match.assigned;
  std::stable_sort(assigned.begin(), assigned.end(), [](const auto& a, const auto& b) { return (a.end - a.begin) > (b.end - b.begin); });
  std::vector<bool> claimed(items.size(), false);
  std::map<std::string, std::string> taken;
  for (const auto& a : assigned) {
    std::string target;
    if (!a.property.empty() && a.property[0] == '<') {
      target = LonghandForType(property, a.property);
    } else {
      for (const char* longhand : property.longhands) {
        if (a.property == longhand) target = longhand;
      }
    }
    if (target.empty() || taken.count(target) || Overlaps(claimed, a.begin, a.end) || a.begin >= a.end) continue;
    for (size_t i = a.begin; i < a.end; ++i) claimed[i] = true;
    taken[target] = Join(items, a.begin, a.end);
  }
  // A slash in a value the longhands did not take is a syntax this table does not know: fail closed.
  for (size_t i = 0; i < items.size(); ++i) {
    if (!claimed[i] && !items[i].IsDelim('/') && !items[i].IsToken(T::Comma)) {
      // Keywords of the shorthand itself (a system font, "none"): all longhands go to their initial values.
      break;
    }
  }
  // Omitted: the initial value, except where the specification says otherwise.
  static const std::set<std::string> copyFirst = {"gap", "grid-gap", "place-content", "place-items", "place-self"};
  const std::string firstLonghand = property.longhands.empty() ? "" : property.longhands[0];
  for (size_t i = 0; i < property.longhands.size(); ++i) {
    const std::string longhand = property.longhands[i];
    std::string text;
    if (taken.count(longhand)) {
      text = taken[longhand];
    } else if (name == "flex") {
      text = longhand == "flex-basis" ? "0%" : "1";
    } else if (copyFirst.count(name) && i == 1 && taken.count(firstLonghand)) {
      text = taken[firstLonghand];
    } else if (const PropertyDefinition* l = FindProperty(longhand)) {
      text = InitialValueText(*l);
    } else {
      text = "initial";
    }
    add(longhand, text);
  }
  for (const char* reset : property.resetLonghands) add(reset, "initial");
  for (Longhand& l : mine) out.push_back(std::move(l));
  return true;
}

bool ExpandValue(const PropertyDefinition& property, const ComponentValues& values, std::vector<Longhand>& out) {
  const std::string name = property.name;
  const bool shorthand = IsRealShorthand(property);
  if (name == "all") {
    if (!IsCssWideKeyword(values)) return false;
    const std::string keyword = Lower(values[0].token.value);
    for (size_t i = 0; i < kPropertyDefinitionCount; ++i) {
      const PropertyDefinition& p = kPropertyDefinitions[i];
      if (IsRealShorthand(p) || p.name == std::string("all") || p.name == std::string("direction") || p.name == std::string("unicode-bidi")) continue;
      out.push_back({p.name, keyword, "", ""});
    }
    return true;
  }
  if (values.empty()) return false;
  if (IsCssWideKeyword(values)) {
    const std::string keyword = Lower(values[0].token.value);
    if (!shorthand) {
      out.push_back({name, keyword, "", ""});
      return true;
    }
    std::vector<std::string> leaves;
    FlattenLeaves(property, leaves);
    for (const std::string& leaf : leaves) out.push_back({leaf, keyword, "", ""});
    return true;
  }
  if (ContainsSubstitution(values)) {
    const std::string text = SerializeValue(values);
    if (!shorthand) {
      out.push_back({name, text, "", ""});
      return true;
    }
    std::vector<std::string> leaves;
    FlattenLeaves(property, leaves);
    for (const std::string& leaf : leaves) out.push_back({leaf, "", name, text});
    return true;
  }
  if (shorthand) return ExpandShorthand(property, values, out);
  ValueMatch match;
  if (!MatchPropertyValue(property, values, match)) return false;
  out.push_back({name, SerializeValue(match.normalized), "", ""});
  return true;
}

}  // namespace

bool ExpandDeclaration(const std::string& name, const std::string& text, std::vector<Longhand>& out) {
  const PropertyDefinition* property = FindProperty(name);
  const ComponentValues values = Trimmed(ParseComponentValues(text));
  if (!property) {
    if (values.empty()) return false;
    out.push_back({name, SerializeValue(values), "", ""});
    return true;
  }
  std::vector<Longhand> expanded;
  if (!ExpandValue(*property, values, expanded)) return false;
  for (Longhand& l : expanded) out.push_back(std::move(l));
  return true;
}

// ---- Serialization ----

namespace {

std::vector<std::string> Words(const std::string& text) {
  std::vector<std::string> words;
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && text[i] == ' ') ++i;
    size_t j = i;
    int depth = 0;
    while (j < text.size() && (text[j] != ' ' || depth > 0)) {
      if (text[j] == '(' || text[j] == '[') ++depth;
      else if (text[j] == ')' || text[j] == ']') --depth;
      ++j;
    }
    if (j > i) words.push_back(text.substr(i, j - i));
    i = j;
  }
  return words;
}

// The shortest of the one-to-four values that stand for the four edges.
std::vector<std::string> CompressEdges(const std::vector<std::string>& v) {
  if (v.size() == 2) return v[0] == v[1] ? std::vector<std::string>{v[0]} : v;
  if (v[1] == v[3]) {
    if (v[0] == v[2]) return v[0] == v[1] ? std::vector<std::string>{v[0]} : std::vector<std::string>{v[0], v[1]};
    return {v[0], v[1], v[2]};
  }
  return v;
}

std::string JoinWords(const std::vector<std::string>& words) {
  std::string out;
  for (size_t i = 0; i < words.size(); ++i) out += (i ? " " : "") + words[i];
  return out;
}

bool IsKeywordValue(const std::string& value) {
  const ComponentValues values = Trimmed(ParseComponentValues(value));
  return IsCssWideKeyword(values);
}

// The leaf longhands a value of `name` comes to, by name, for comparing two ways of writing the same thing.
std::map<std::string, std::string> LeafMap(const std::string& name, const std::string& text) {
  std::vector<Longhand> out;
  std::map<std::string, std::string> map;
  if (ExpandDeclaration(name, text, out)) {
    for (const Longhand& l : out) map[l.name] = l.value;
  } else {
    map["\x01invalid"] = text;
  }
  return map;
}

}  // namespace

std::optional<std::string> SerializeShorthand(const PropertyDefinition& shorthand, const ShorthandInput& input) {
  const size_t n = shorthand.longhands.size();
  if (input.values.size() != n) return std::nullopt;
  for (size_t i = 0; i < n; ++i) {
    if (input.pending[i] || input.values[i].empty()) return std::nullopt;
  }
  // The same keyword everywhere is that keyword; another keyword among other values cannot be written ("initial" can:
  // it is the initial value of what it stands for).
  const bool allSame = std::all_of(input.values.begin(), input.values.end(), [&](const std::string& v) { return v == input.values[0]; });
  if (allSame && IsKeywordValue(input.values[0])) return input.values[0];
  for (const std::string& value : input.values) {
    if (IsKeywordValue(value) && value != "initial") return std::nullopt;
  }
  const std::string name = shorthand.name;
  std::string candidate;
  if (name == "border-radius" || name == "-webkit-border-radius") {
    if (n != 4) return std::nullopt;
    std::vector<std::string> h(4), v(4);
    for (size_t i = 0; i < 4; ++i) {
      const std::vector<std::string> words = Words(input.values[i]);
      if (words.empty() || words.size() > 2) return std::nullopt;
      h[i] = words[0];
      v[i] = words.size() == 2 ? words[1] : words[0];
    }
    candidate = JoinWords(CompressEdges(h));
    if (h != v) candidate += " / " + JoinWords(CompressEdges(v));
  } else if (name == "font") {
    // [style] [weight] [stretch] size [/ line-height] family, the ones that are not initial.
    const auto value = [&](const char* longhand) {
      for (size_t i = 0; i < n; ++i) {
        if (std::string(shorthand.longhands[i]) == longhand) return input.values[i];
      }
      return std::string("initial");
    };
    const auto omitted = [](const std::string& v) { return v == "normal" || v == "initial"; };
    std::string text;
    for (const char* longhand : {"font-style", "font-variant", "font-weight", "font-stretch"}) {
      const std::string v = value(longhand);
      if (!omitted(v)) text += (text.empty() ? "" : " ") + v;
    }
    text += (text.empty() ? "" : " ") + value("font-size");
    const std::string lineHeight = value("line-height");
    if (!omitted(lineHeight)) text += " / " + lineHeight;
    candidate = text + " " + value("font-family");
  } else if (IsEdgeShorthand(shorthand)) {
    const std::vector<std::string> compressed = n == 4 ? CompressEdges(input.values) : (input.values[0] == input.values[1] ? std::vector<std::string>{input.values[0]} : input.values);
    candidate = JoinWords(compressed);
  } else {
    // The values that are not the initial ones, in the order the syntax names the longhands.
    std::vector<std::pair<size_t, size_t>> order;  // (position in the syntax, index)
    const std::string syntax = shorthand.syntax;
    for (size_t i = 0; i < n; ++i) {
      size_t at = syntax.find(std::string("<'") + shorthand.longhands[i] + "'>");
      order.emplace_back(at == std::string::npos ? 100000 + i : at, i);
    }
    std::stable_sort(order.begin(), order.end());
    for (const auto& [position, index] : order) {
      (void)position;
      const PropertyDefinition* longhand = FindProperty(shorthand.longhands[index]);
      if (input.values[index] == "initial" || (longhand && input.values[index] == InitialValueText(*longhand))) continue;
      candidate += (candidate.empty() ? "" : " ") + input.values[index];
    }
    if (candidate.empty()) candidate = input.values[0];
  }
  // It is the shorthand only if it reads back as the same longhands.
  std::map<std::string, std::string> wanted;
  for (size_t i = 0; i < n; ++i) {
    for (const auto& [leaf, value] : LeafMap(shorthand.longhands[i], input.values[i])) wanted[leaf] = value;
  }
  const std::map<std::string, std::string> read = LeafMap(name, candidate);
  for (const auto& [leaf, value] : wanted) {
    const auto found = read.find(leaf);
    if (found == read.end() || found->second != value) return std::nullopt;
  }
  return candidate;
}

}  // namespace solar::css
