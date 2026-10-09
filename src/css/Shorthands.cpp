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
  if (initial.empty()) return "initial";
  const ComponentValues values = Trimmed(ParseComponentValues(initial));
  ValueMatch match;
  if (!values.empty() && MatchPropertyValue(property, values, match)) return SerializeValue(match.normalized);
  return "initial";
}

bool IsShorthandProperty(const PropertyDefinition& property) { return IsRealShorthand(property); }

const std::vector<std::string>& AllLonghands() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> list;
    for (size_t i = 0; i < kPropertyDefinitionCount; ++i) {
      const PropertyDefinition& p = kPropertyDefinitions[i];
      const std::string name = p.name;
      if (IsRealShorthand(p) || name == "all" || name == "direction" || name == "unicode-bidi" || name[0] == '-') continue;
      list.push_back(name);
    }
    return list;
  }();
  return names;
}

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
      std::stable_sort(list.begin(), list.end(), [](const PropertyDefinition* a, const PropertyDefinition* b) {
        const size_t x = LeavesOf(*a).size(), y = LeavesOf(*b).size();
        if (x != y) return x > y;
        return a->name[0] != '-' && b->name[0] == '-';
      });
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

// The longhands of `shorthand` that can take what the type `<name>` matched, in the order the shorthand lists them.
std::vector<std::string> LonghandsForType(const PropertyDefinition& shorthand, const std::string& typeName) {
  static const std::map<std::string, std::string> special = {{"<font-variant-css2>", "font-variant"}, {"<font-width-css3>", "font-stretch"}};
  std::vector<std::string> found;
  if (const auto s = special.find(typeName); s != special.end()) {
    for (const char* name : shorthand.longhands) {
      if (s->second == name) found.push_back(name);
    }
    if (!found.empty()) return found;
  }
  // "<time>" is also written "<time [0,∞]>" in a syntax.
  const std::string stem = typeName.substr(0, typeName.size() - 1);
  for (const char* name : shorthand.longhands) {
    const PropertyDefinition* longhand = FindProperty(name);
    if (!longhand) continue;
    const std::string syntax = longhand->syntax;
    bool contains = false;
    for (size_t at = syntax.find(stem); at != std::string::npos; at = syntax.find(stem, at + 1)) {
      const size_t after = at + stem.size();
      if (after < syntax.size() && (syntax[after] == '>' || syntax[after] == ' ')) {
        contains = true;
        break;
      }
    }
    if (!contains && IsRealShorthand(*longhand)) contains = !LonghandsForType(*longhand, typeName).empty();
    if (contains) found.push_back(name);
  }
  return found;
}

std::string Join(const ComponentValues& values, size_t begin, size_t end) {
  ComponentValues slice(values.begin() + begin, values.begin() + end);
  return SerializeValue(slice);
}

// What each longhand of `shorthand` took of a match: the largest matches first, each component to one longhand; a type that
// more than one longhand has (the duration and the delay are both a <time>) goes to them in the order they are written.
std::map<std::string, std::string> AssignLonghands(const PropertyDefinition& property, const ValueMatch& match) {
  const ComponentValues& items = match.normalized;
  std::vector<ValueMatch::Assignment> assigned = match.assigned;
  std::stable_sort(assigned.begin(), assigned.end(), [](const auto& a, const auto& b) {
    if ((a.end - a.begin) != (b.end - b.begin)) return (a.end - a.begin) > (b.end - b.begin);
    return a.begin < b.begin;
  });
  std::vector<bool> claimed(items.size(), false);
  std::map<std::string, std::string> taken;
  std::map<std::string, size_t> occurrence;  // how many of a type have been given out
  for (const auto& a : assigned) {
    if (a.begin >= a.end || Overlaps(claimed, a.begin, a.end)) continue;
    std::string target;
    if (!a.property.empty() && a.property[0] == '<') {
      const std::vector<std::string> candidates = LonghandsForType(property, a.property);
      size_t& used = occurrence[a.property];
      // The first candidate that has not taken something yet, in written order.
      for (const std::string& candidate : candidates) {
        if (!taken.count(candidate)) {
          target = candidate;
          break;
        }
      }
      (void)used;
    } else {
      for (const char* longhand : property.longhands) {
        if (a.property == longhand) target = longhand;
      }
    }
    if (target.empty() || taken.count(target)) continue;
    for (size_t i = a.begin; i < a.end; ++i) claimed[i] = true;
    taken[target] = Join(items, a.begin, a.end);
  }
  return taken;
}

// A shorthand written as <A># or <A>#? , <B>: a comma-separated list of layers, the last of which may be a different kind.
struct LayerForm {
  std::string layer;
  std::string last;
};

std::optional<LayerForm> LayersOf(const PropertyDefinition& property) {
  const std::string syntax = property.syntax;
  if (syntax.size() < 4 || syntax[0] != '<') return std::nullopt;
  const size_t close = syntax.find('>');
  if (close == std::string::npos) return std::nullopt;
  const std::string first = syntax.substr(1, close - 1);
  const std::string rest = syntax.substr(close + 1);
  if (first.empty() || first.find_first_of(" '[") != std::string::npos) return std::nullopt;
  if (rest == "#") return LayerForm{first, first};
  if (rest.starts_with("#? , <") && rest.back() == '>') {
    const std::string last = rest.substr(6, rest.size() - 7);
    if (last.find_first_of(" '[") == std::string::npos) return LayerForm{first, last};
  }
  return std::nullopt;
}

bool IsListLonghand(const PropertyDefinition& property) { return std::string(property.syntax).find('#') != std::string::npos; }

// The x and the y of a position, as the longhands take them.
void SplitPosition(const ComponentValue& group, std::string& x, std::string& y) {
  const ComponentValues& v = group.children;
  size_t verticalAt = v.size();
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i].IsIdent()) {
      const std::string w = Lower(v[i].token.value);
      if (w == "top" || w == "bottom" || w == "y-start" || w == "y-end") {
        verticalAt = i;
        break;
      }
    }
  }
  if (v.size() == 2 || verticalAt >= v.size()) verticalAt = 1;
  x = SerializeValue(ComponentValues(v.begin(), v.begin() + std::min(verticalAt, v.size())));
  y = SerializeValue(ComponentValues(v.begin() + std::min(verticalAt, v.size()), v.end()));
}

bool ExpandLayered(const PropertyDefinition& property, const LayerForm& form, const ComponentValues& items, std::vector<Longhand>& out) {
  std::vector<ComponentValues> layers = SplitOnCommas(items);
  const size_t n = layers.size();
  std::map<std::string, std::vector<std::string>> perLonghand;
  const bool position = std::string(property.name) == "background-position";
  for (size_t i = 0; i < n; ++i) {
    const std::string type = i + 1 == n ? form.last : form.layer;
    ValueMatch m;
    if (!MatchSyntax("<" + type + ">", layers[i], m)) return false;
    if (position) {
      const ComponentValue* group = nullptr;
      size_t count = 0;
      for (const ComponentValue& v : m.normalized) {
        if (v.IsWhitespace()) continue;
        group = &v;
        ++count;
      }
      if (count != 1 || group->kind != ComponentValue::Kind::Function) return false;
      std::string x, y;
      SplitPosition(*group, x, y);
      perLonghand["background-position-x"].resize(n);
      perLonghand["background-position-y"].resize(n);
      perLonghand["background-position-x"][i] = x;
      perLonghand["background-position-y"][i] = y;
      continue;
    }
    for (const auto& [longhand, text] : AssignLonghands(property, m)) {
      perLonghand[longhand].resize(n);
      perLonghand[longhand][i] = text;
    }
  }
  for (const char* name : property.longhands) {
    const PropertyDefinition* longhand = FindProperty(name);
    std::vector<std::string> list = perLonghand[name];
    list.resize(n);
    const std::string initial = longhand ? (std::string(name) == "background-position" ? "0% 0%" : InitialValueText(*longhand)) : "initial";
    std::string text;
    if (longhand && IsListLonghand(*longhand)) {
      for (size_t i = 0; i < n; ++i) text += (i ? ", " : "") + (list[i].empty() ? initial : list[i]);
    } else {
      text = list[n - 1].empty() ? initial : list[n - 1];
    }
    ExpandLonghand(name, text, out);
  }
  return true;
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
  if (const std::optional<LayerForm> form = LayersOf(property)) {
    if (!ExpandLayered(property, *form, values, mine)) return false;
    for (const char* reset : property.resetLonghands) add(reset, "initial");
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

  std::map<std::string, std::string> taken = AssignLonghands(property, match);
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
    } else if ((name == "grid-row" || name == "grid-column" || name == "grid-area") && i > 0) {
      // A line left out is the line before it if that is a name, and auto otherwise.
      const size_t from = name == "grid-area" ? (i == 1 ? 0 : i == 2 ? 0 : 1) : 0;
      const std::string& source = property.longhands[from];
      const auto it = taken.find(source);
      const std::string named = it != taken.end() ? it->second : "";
      const bool isName = !named.empty() && named.find(' ') == std::string::npos && !std::isdigit(static_cast<unsigned char>(named[0])) && named[0] != '-' && named != "auto";
      text = isName ? named : "auto";
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
    for (const std::string& name : AllLonghands()) out.push_back({name, keyword, "", ""});
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
    if (!ValidSubstitutions(values)) return false;
    const std::string text = Serialize(values);
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
  } else if (const std::optional<LayerForm> form = LayersOf(shorthand)) {
    // Layer by layer: what is not the initial value, in the order the shorthand's longhands come.
    std::vector<std::vector<std::string>> lists(n);
    size_t layers = 1;
    for (size_t i = 0; i < n; ++i) {
      for (const ComponentValues& part : SplitOnCommas(ParseComponentValues(input.values[i]))) lists[i].push_back(Serialize(Trimmed(part)));
      const PropertyDefinition* longhand = FindProperty(shorthand.longhands[i]);
      if (longhand && IsListLonghand(*longhand)) layers = std::max(layers, lists[i].size());
    }
    for (size_t i = 0; i < n; ++i) {
      const PropertyDefinition* longhand = FindProperty(shorthand.longhands[i]);
      const bool isList = longhand && IsListLonghand(*longhand);
      if (isList && lists[i].size() != layers) return std::nullopt;
      if (!isList && lists[i].size() != 1) return std::nullopt;
    }
    if (name == "background-position") {
      // x and y of each layer.
      if (n != 2) return std::nullopt;
      for (size_t l = 0; l < layers; ++l) candidate += (l ? ", " : "") + lists[0][l] + " " + lists[1][l];
    } else {
      const auto initialOf = [&](size_t i) {
        const PropertyDefinition* longhand = FindProperty(shorthand.longhands[i]);
        if (std::string(shorthand.longhands[i]).ends_with("position")) return std::string("0% 0%");
        return longhand ? InitialValueText(*longhand) : std::string("initial");
      };
      const auto longhandAt = [&](const char* suffix) -> int {
        for (size_t i = 0; i < n; ++i) {
          if (std::string(shorthand.longhands[i]).ends_with(suffix)) return static_cast<int>(i);
        }
        return -1;
      };
      for (size_t l = 0; l < layers; ++l) {
        std::string layer;
        const auto value = [&](size_t i) { return lists[i].size() == 1 && !IsListLonghand(*FindProperty(shorthand.longhands[i])) ? lists[i][0] : lists[i][l]; };
        const int positionAt = longhandAt("position"), sizeAt = longhandAt("size"), originAt = longhandAt("origin"), clipAt = longhandAt("clip");
        for (size_t i = 0; i < n; ++i) {
          const std::string v = value(i);
          const bool isLast = l + 1 == layers;
          const PropertyDefinition* longhand = FindProperty(shorthand.longhands[i]);
          const bool isList = longhand && IsListLonghand(*longhand);
          if (!isList && !isLast) continue;  // a color is for the last layer
          if (static_cast<int>(i) == sizeAt) continue;  // with the position
          if (static_cast<int>(i) == originAt || static_cast<int>(i) == clipAt) continue;  // below
          if (v == initialOf(i) && !(static_cast<int>(i) == positionAt && sizeAt >= 0 && value(sizeAt) != initialOf(sizeAt))) continue;
          std::string piece = v;
          if (static_cast<int>(i) == positionAt && sizeAt >= 0 && value(sizeAt) != initialOf(sizeAt)) piece += " / " + value(sizeAt);
          layer += (layer.empty() ? "" : " ") + piece;
        }
        if (originAt >= 0 && clipAt >= 0) {
          const std::string origin = value(originAt), clip = value(clipAt);
          const bool originInitial = origin == initialOf(originAt), clipInitial = clip == initialOf(clipAt);
          if (!(originInitial && clipInitial)) {
            std::string boxes = origin == clip ? origin : origin + " " + clip;
            layer += (layer.empty() ? "" : " ") + boxes;
          }
        }
        // A color goes at the end of the last layer.
        if (layer.empty()) layer = "none";
        candidate += (l ? ", " : "") + layer;
      }
    }
  } else if (name == "grid-row" || name == "grid-column") {
    if (n != 2) return std::nullopt;
    const std::string start = input.values[0], end = input.values[1];
    const bool isName = !start.empty() && start.find(' ') == std::string::npos && !std::isdigit(static_cast<unsigned char>(start[0])) && start[0] != '-' && start != "auto";
    candidate = start;
    if (!(end == "auto" || (isName && end == start))) candidate += " / " + end;
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
