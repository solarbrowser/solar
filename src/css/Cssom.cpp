#include "solar/css/Cssom.h"
#include "solar/css/Descriptors.h"

#include <functional>
#include "solar/css/Shorthands.h"
#include "solar/css/Style.h"
#include "solar/url/Parser.h"
#include "solar/url/Serializer.h"
#include "solar/css/Values.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace solar::css {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::Heap;
using Quanta::Object;
using Quanta::Value;
using T = Token::Type;

// ---- Text helpers ----

namespace {

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool IEquals(std::string_view a, std::string_view b) { return Lower(a) == Lower(b); }

std::string Trim(const std::string& text) {
  size_t begin = 0, end = text.size();
  const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; };
  while (begin < end && space(text[begin])) ++begin;
  while (end > begin && space(text[end - 1])) --end;
  return text.substr(begin, end - begin);
}

}  // namespace

// ---- Properties ----

bool NormalizePropertyName(std::string& name) {
  if (name.size() >= 2 && name[0] == '-' && name[1] == '-') return name.size() > 2;
  const std::string lower = Lower(name);
  if (const PropertyDefinition* property = FindProperty(lower)) {
    name = property->name;  // a legacy name is its property
    return true;
  }
  // A vendor prefix: the engine does not know what it means, but the property is there for script to set and read.
  if (lower.size() > 1 && lower[0] == '-' && lower[1] != '-' &&
      (lower.starts_with("-webkit-") || lower.starts_with("-moz-") || lower.starts_with("-ms-") || lower.starts_with("-o-"))) {
    name = lower;
    return true;
  }
  return false;
}

// ---- Declarations ----

namespace {

void AddOrReplace(std::vector<DeclarationEntry>& items, DeclarationEntry entry) {
  if (entry.name.rfind("animation", 0) == 0 || entry.name.rfind("transition", 0) == 0 || entry.name.rfind("-webkit-animation", 0) == 0 ||
      entry.name.rfind("-webkit-transition", 0) == 0) {
    NoteAnimationMention();
  }
  for (DeclarationEntry& existing : items) {
    if (existing.name == entry.name) {
      existing = std::move(entry);
      return;
    }
  }
  items.push_back(std::move(entry));
}

}  // namespace

const DeclarationEntry* CssDeclarations::Find(const std::string& name) const {
  for (const DeclarationEntry& entry : items) {
    if (entry.name == name) return &entry;
  }
  return nullptr;
}

bool ApplyDeclaration(std::vector<DeclarationEntry>& items, const std::string& name, const std::string& text, bool important) {
  if (name.starts_with("--")) {
    AddOrReplace(items, {name, css::Serialize(Trimmed(ParseComponentValues(text))), important, "", ""});
    return true;
  }
  const ComponentValues values = Trimmed(ParseComponentValues(text));
  if (values.empty()) return false;
  for (const ComponentValue& value : values) {
    if (value.IsToken(T::Semicolon) || value.IsToken(T::BadString) || value.IsToken(T::BadUrl) || value.IsToken(T::RightBrace) || value.IsToken(T::RightParen) || value.IsToken(T::RightBracket)) return false;
    if (value.IsDelim('!')) return false;
  }
  std::vector<Longhand> longhands;
  if (!ExpandDeclaration(name, css::Serialize(values), longhands)) return false;
  for (Longhand& longhand : longhands) AddOrReplace(items, {longhand.name, longhand.value, important, longhand.pendingShorthand, longhand.pendingText, ValueBaseUrl()});
  return true;
}

// A bad string or bad url anywhere in a value: the declaration is not valid, whatever the rest is.
bool HasBadTokens(const ComponentValues& values) {
  for (const ComponentValue& v : values) {
    if (v.IsToken(T::BadString) || v.IsToken(T::BadUrl)) return true;
    if (v.kind != ComponentValue::Kind::Token && HasBadTokens(v.children)) return true;
  }
  return false;
}

std::vector<DeclarationEntry> ParseDeclarationList(std::string_view text) {
  std::vector<DeclarationEntry> items;
  for (const BlockItem& item : ParseDeclarationItems(text)) {
    if (!item.isDeclaration) continue;
    std::string name = item.declaration.name;
    if (!NormalizePropertyName(name)) continue;
    if (!name.starts_with("--") && HasBadTokens(item.declaration.value)) continue;
    const std::string source = name.starts_with("--") ? item.declaration.originalText : css::Serialize(Trimmed(item.declaration.value));
    ApplyDeclaration(items, name, source, item.declaration.important);
  }
  return items;
}

bool CssDeclarations::IsDescriptorBlock() const { return parentRule && (parentRule->kind == RuleKind::FontFace || parentRule->kind == RuleKind::FontPaletteValues || parentRule->kind == RuleKind::CounterStyle); }

namespace {
DescriptorSet SetOf(const CssRule* rule) {
  return rule->kind == RuleKind::FontFace ? DescriptorSet::FontFace : rule->kind == RuleKind::CounterStyle ? DescriptorSet::CounterStyle : DescriptorSet::FontPaletteValues;
}
}  // namespace

namespace {
thread_local std::string g_parsingBase;  // the base of the sheet being parsed, when there is no rule to ask yet
}

void SetParsingBase(const std::string& base) { g_parsingBase = base; }

std::string CssDeclarations::BaseUrl() const {
  if (!g_parsingBase.empty()) return g_parsingBase;
  for (const CssRule* rule = parentRule; rule; rule = rule->parentRule) {
    if (rule->parentSheet) return rule->parentSheet->baseUrl;
  }
  if (ownerElement && ownerElement->nodeDocument) return dom::DocumentBaseUri(ownerElement->nodeDocument);
  return "";
}

bool CssDeclarations::Apply(const std::string& name, const std::string& text, bool important) {
  SetValueBaseUrl(BaseUrl());
  struct Reset {
    ~Reset() { SetValueBaseUrl(""); }
  } reset;
  if (IsDescriptorBlock()) {
    // A descriptor is not important, and takes the whole value or none.
    const std::optional<std::string> descriptor = DescriptorName(SetOf(parentRule), name);
    if (!descriptor || important) return false;
    const std::optional<std::string> value = DescriptorValue(SetOf(parentRule), *descriptor, ParseComponentValues(text));
    if (!value) return false;
    AddOrReplace(items, {*descriptor, *value, false, "", ""});
    return true;
  }
  return ApplyDeclaration(items, name, text, important);
}

namespace {

// The value a block gives a shorthand, from its longhands: nothing when it has not all of them, or they differ in
// priority, or cannot be written as the shorthand.
std::optional<std::string> ShorthandValue(const std::vector<DeclarationEntry>& items, const PropertyDefinition& shorthand, std::optional<bool>& important) {
  const auto entryOf = [&](const std::string& name) -> const DeclarationEntry* {
    for (const DeclarationEntry& entry : items) {
      if (entry.name == name) return &entry;
    }
    return nullptr;
  };
  // Pending on a var(): every longhand is, on this very text.
  size_t pendingCount = 0;
  const std::vector<std::string> leaves = LeavesOf(shorthand);
  for (const std::string& leaf : leaves) {
    const DeclarationEntry* entry = entryOf(leaf);
    if (!entry) return std::nullopt;
    if (!entry->pendingShorthand.empty()) ++pendingCount;
  }
  if (pendingCount > 0) {
    if (pendingCount != leaves.size()) return std::nullopt;
    const DeclarationEntry* first = entryOf(leaves[0]);
    for (const std::string& leaf : leaves) {
      const DeclarationEntry* entry = entryOf(leaf);
      if (entry->pendingShorthand != first->pendingShorthand || entry->pendingText != first->pendingText || entry->important != first->important) return std::nullopt;
    }
    if (first->pendingShorthand != shorthand.name) return std::nullopt;
    if (important && *important != first->important) return std::nullopt;
    important = first->important;
    return first->pendingText;
  }
  ShorthandInput input;
  for (const char* name : shorthand.longhands) {
    const PropertyDefinition* inner = FindProperty(name);
    std::optional<std::string> value;
    if (inner && IsShorthandProperty(*inner)) {
      value = ShorthandValue(items, *inner, important);
      if (!value) return std::nullopt;
    } else {
      const DeclarationEntry* found = entryOf(name);
      if (!found) return std::nullopt;
      if (important && *important != found->important) return std::nullopt;
      important = found->important;
      value = found->value;
    }
    input.values.push_back(*value);
    input.pending.push_back(false);
  }
  // What the shorthand resets must be what it would reset: the initial values.
  for (const std::string& leaf : ResetLeavesOf(shorthand)) {
    const DeclarationEntry* found = entryOf(leaf);
    if (!found) return std::nullopt;
    const PropertyDefinition* inner = FindProperty(leaf);
    if (!(found->value == "initial" || (inner && found->value == InitialValueText(*inner)))) return std::nullopt;
  }
  return SerializeShorthand(shorthand, input);
}

}  // namespace

namespace {

// The keyword every longhand of `all` has in the block, if they have one and the same, and whether it is important.
std::optional<std::pair<std::string, bool>> AllKeyword(const std::vector<DeclarationEntry>& items) {
  std::optional<std::pair<std::string, bool>> found;
  for (const std::string& name : AllLonghands()) {
    const DeclarationEntry* entry = nullptr;
    for (const DeclarationEntry& e : items) {
      if (e.name == name) entry = &e;
    }
    if (!entry || !entry->pendingShorthand.empty()) return std::nullopt;
    if (!IsCssWideKeyword(ParseComponentValues(entry->value))) return std::nullopt;
    if (found && (found->first != entry->value || found->second != entry->important)) return std::nullopt;
    found = std::make_pair(entry->value, entry->important);
  }
  return found;
}

}  // namespace

std::string CssDeclarations::ValueOf(const std::string& given) const {
  if (IsDescriptorBlock()) {
    const std::optional<std::string> descriptor = DescriptorName(SetOf(parentRule), given);
    const DeclarationEntry* entry = descriptor ? Find(*descriptor) : nullptr;
    return entry ? entry->value : "";
  }
  const PropertyDefinition* named = FindProperty(given);
  const std::string name = named ? named->name : given;  // a legacy name is its property
  if (name == "all" && !computedElement) {
    const auto keyword = AllKeyword(items);
    return keyword ? keyword->first : "";
  }
  const PropertyDefinition* definition = FindProperty(name);
  if (computedElement) {
    if (name.starts_with("--")) return ComputedValue(*computedContext, computedElement, name, computedPseudo);
    if (!definition) return "";
    if (IsShorthandProperty(*definition)) {
      std::vector<DeclarationEntry> leaves;
      for (const std::string& leaf : LeavesOf(*definition)) leaves.push_back({leaf, ResolvedValue(*computedContext, computedElement, leaf, computedPseudo), false, "", ""});
      std::optional<bool> important;
      return ShorthandValue(leaves, *definition, important).value_or("");
    }
    return ResolvedValue(*computedContext, computedElement, name, computedPseudo);
  }
  if (definition && IsShorthandProperty(*definition)) {
    std::optional<bool> important;
    return ShorthandValue(items, *definition, important).value_or("");
  }
  const DeclarationEntry* entry = Find(name);
  return entry ? entry->value : "";
}

std::string CssDeclarations::PriorityOf(const std::string& given) const {
  if (IsDescriptorBlock()) return "";
  const PropertyDefinition* named = FindProperty(given);
  const std::string name = named ? named->name : given;
  if (name == "all") {
    const auto keyword = AllKeyword(items);
    return keyword && keyword->second ? "important" : "";
  }
  const PropertyDefinition* definition = FindProperty(name);
  if (definition && IsShorthandProperty(*definition)) {
    const std::vector<std::string> leaves = LeavesOf(*definition);
    if (leaves.empty()) return "";
    for (const std::string& leaf : leaves) {
      const DeclarationEntry* entry = Find(leaf);
      if (!entry || !entry->important) return "";
    }
    return "important";
  }
  const DeclarationEntry* entry = Find(name);
  return entry && entry->important ? "important" : "";
}

std::string CssDeclarations::Serialize() const {
  std::string out;
  if (IsDescriptorBlock()) {
    for (const DeclarationEntry& entry : items) out += (out.empty() ? "" : " ") + entry.name + ": " + entry.value + ";";
    return out;
  }
  std::set<std::string> done;
  if (const auto keyword = AllKeyword(items)) {
    out = "all: " + keyword->first + (keyword->second ? " !important" : "") + ";";
    for (const std::string& name : AllLonghands()) done.insert(name);
  }
  for (const DeclarationEntry& entry : items) {
    if (done.count(entry.name)) continue;
    bool written = false;
    if (!entry.pendingShorthand.empty() || FindProperty(entry.name)) {
      for (const PropertyDefinition* shorthand : ShorthandsOf(entry.name)) {
        const std::vector<std::string> leaves = LeavesOf(*shorthand);
        bool all = true;
        for (const std::string& leaf : leaves) all = all && Find(leaf) && !done.count(leaf);
        if (!all) continue;
        std::optional<bool> important;
        const std::optional<std::string> value = ShorthandValue(items, *shorthand, important);
        if (!value) continue;
        if (!out.empty()) out += ' ';
        out += std::string(shorthand->name) + ": " + *value + (important.value_or(false) ? " !important" : "") + ";";
        for (const std::string& leaf : leaves) done.insert(leaf);
        written = true;
        break;
      }
    }
    if (written) continue;
    done.insert(entry.name);
    if (!entry.pendingShorthand.empty()) continue;  // known only once its variables are
    if (!out.empty()) out += ' ';
    out += (entry.name.starts_with("--") ? SerializeIdentifier(entry.name) : entry.name) + ": " + entry.value + (entry.important ? " !important" : "") + ";";
  }
  return out;
}

void CssDeclarations::SetText(Context& ctx, std::string_view text) {
  items = ParseDeclarationList(text);
  Changed(ctx);
}

bool CssDeclarations::Set(Context& ctx, const std::string& propertyName, const std::string& value, bool important) {
  std::string name = propertyName;
  if (IsDescriptorBlock()) {
    const std::optional<std::string> descriptor = DescriptorName(SetOf(parentRule), name);
    if (!descriptor || !Apply(*descriptor, value, important)) return false;
    Changed(ctx);
    return true;
  }
  if (!NormalizePropertyName(name)) return false;
  if (!Apply(name, value, important)) return false;
  Changed(ctx);
  return true;
}

std::string CssDeclarations::Remove(Context& ctx, const std::string& propertyName) {
  std::string name = propertyName;
  if (!name.starts_with("--")) name = Lower(name);
  if (IsDescriptorBlock()) {
    if (const std::optional<std::string> descriptor = DescriptorName(SetOf(parentRule), name)) name = *descriptor;
    const std::string removed = ValueOf(name);
    for (auto it = items.begin(); it != items.end(); ++it) {
      if (it->name == name) {
        items.erase(it);
        Changed(ctx);
        break;
      }
    }
    return removed;
  }
  if (const PropertyDefinition* named = FindProperty(name)) name = named->name;
  const std::string old = ValueOf(name);
  const PropertyDefinition* definition = FindProperty(name);
  std::vector<std::string> targets = name == "all" ? AllLonghands() : definition ? LeavesOf(*definition) : std::vector<std::string>{name};
  if (targets.empty()) targets.push_back(name);
  bool removed = false;
  for (const std::string& target : targets) {
    for (auto it = items.begin(); it != items.end(); ++it) {
      if (it->name == target) {
        items.erase(it);
        removed = true;
        break;
      }
    }
  }
  if (removed) Changed(ctx);
  return old;
}

void CssDeclarations::Changed(Context& ctx) {
  NoteStyleChange();
  if (ownerElement && !updatingAttribute) {
    updatingAttribute = true;
    dom::SetAttribute(ctx, ownerElement, "style", Serialize());
    updatingAttribute = false;
  }
}

void CssDeclarations::Visit(Quanta::Visitor& visitor) {
  visitor.Mark(parentRule);
  visitor.Mark(ownerElement);
  visitor.Mark(computedElement);
}

// ---- Media ----

namespace {

std::string WordOf(const ComponentValue& v) { return v.IsIdent() ? Lower(v.token.value) : std::string(); }

// "serialize a media condition": the parentheses of a feature hold `name`, `name: value` or a range with single spaces; a
// condition holds not, and, or and the conditions they join; anything else is as written with its spaces collapsed.
std::string SerializeConditionGroup(const ComponentValue& v) {
  if (!v.IsBlock(T::LeftParen)) {
    std::string collapsed;
    bool space = false;
    for (char c : Serialize(v)) {
      if (c == ' ' || c == '\n' || c == '\t') {
        space = true;
        continue;
      }
      if (space && !collapsed.empty() && collapsed.back() != '(') collapsed += ' ';
      space = false;
      collapsed += c;
    }
    return collapsed;
  }
  const ComponentValues inner = Trimmed(v.children);
  std::vector<ComponentValue> items;
  for (const ComponentValue& c : inner) {
    if (!c.IsWhitespace()) items.push_back(c);
  }
  const auto isGroup = [](const ComponentValue& c) { return c.IsBlock(T::LeftParen) || c.kind == ComponentValue::Kind::Function; };
  const bool isCondition = !items.empty() && ((WordOf(items[0]) == "not" && items.size() == 2 && isGroup(items[1])) ||
                                              (isGroup(items[0]) && (items.size() == 1 || WordOf(items[1]) == "and" || WordOf(items[1]) == "or")));
  if (isCondition) {
    std::string out;
    for (const ComponentValue& c : items) {
      if (!out.empty()) out += ' ';
      out += isGroup(c) ? SerializeConditionGroup(c) : WordOf(c);
    }
    return "(" + out + ")";
  }
  std::string out;
  bool space = false;
  const auto closed = [&] { return out.empty() || out.back() == ' '; };
  for (size_t k = 0; k < inner.size(); ++k) {
    const ComponentValue& c = inner[k];
    if (c.IsWhitespace()) {
      space = true;
      continue;
    }
    if (c.IsToken(T::Colon)) {
      out += ": ";
      space = false;
      continue;
    }
    if (c.IsDelim('/')) {
      // A ratio is written with spaces around the slash.
      const bool ratio = k > 0 && k + 1 < inner.size() && !out.empty() && std::isdigit(static_cast<unsigned char>(out.back())) && (inner[k + 1].IsToken(T::Number) || (k + 2 < inner.size() && inner[k + 1].IsWhitespace() && inner[k + 2].IsToken(T::Number)));
      if (ratio) {
        out += " / ";
        while (k + 1 < inner.size() && inner[k + 1].IsWhitespace()) ++k;
      } else {
        out += "/";
      }
      space = false;
      continue;
    }
    if (c.IsDelim('<') || c.IsDelim('>') || c.IsDelim('=')) {
      std::string op(1, static_cast<char>(c.token.delim));
      if (op != "=" && k + 1 < inner.size() && inner[k + 1].IsDelim('=')) {
        op += "=";
        ++k;
      }
      if (!closed()) out += ' ';
      out += op + " ";
      space = false;
      continue;
    }
    if (space && !closed()) out += ' ';
    space = false;
    out += c.IsIdent() && !c.token.value.starts_with("--") ? SerializeIdentifier(Lower(c.token.value)) : Serialize(c);
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return "(" + out + ")";
}


// ---- Container queries ----

struct ContainerPrelude {
  std::string text;       // serialized: the queries, comma-separated
  std::string name;       // of the first query, when that is the only one
  std::string condition;  // the same
};

std::string SerializeContainerGroup(const ComponentValue& v) {
  if (v.kind == ComponentValue::Kind::Function) {
    const std::string name = Lower(v.name);
    std::string inner;
    bool space = false;
    for (char c : Serialize(Trimmed(v.children))) {
      if (c == ' ' || c == '\n' || c == '\t') {
        space = true;
        continue;
      }
      if (space && !inner.empty()) inner += ' ';
      space = false;
      inner += c;
    }
    return (name == "style" || name == "scroll-state" ? name : v.name) + "(" + inner + ")";
  }
  return SerializeConditionGroup(v);
}

// <container-condition># : each `<container-name>? <container-query>?` (not both missing); a query is `not (..)`, or groups
// joined by and, or by or. What is inside a group that is not understood is "general enclosed": valid and unknown.
std::optional<ContainerPrelude> ParseContainerPrelude(const ComponentValues& prelude) {
  ContainerPrelude result;
  const std::vector<ComponentValues> parts = SplitOnCommas(Trimmed(prelude));
  if (parts.empty()) return std::nullopt;
  for (const ComponentValues& part : parts) {
    std::vector<ComponentValue> items;
    for (const ComponentValue& c : Trimmed(part)) {
      if (!c.IsWhitespace()) items.push_back(c);
    }
    if (items.empty()) return std::nullopt;
    size_t i = 0;
    std::string name;
    const auto isGroup = [](const ComponentValue& c) { return c.IsBlock(T::LeftParen) || c.kind == ComponentValue::Kind::Function; };
    if (items[0].IsIdent() && WordOf(items[0]) != "not") {
      const std::string word = WordOf(items[0]);
      if (word == "and" || word == "or" || word == "none" || word == "default" || IsCssWideKeyword({items[0]})) return std::nullopt;
      name = SerializeIdentifier(items[0].token.value);
      i = 1;
    } else if (!items[0].IsIdent() && !isGroup(items[0])) {
      return std::nullopt;
    }
    std::string condition;
    if (i < items.size()) {
      if (WordOf(items[i]) == "not") {
        if (i + 2 != items.size() || !isGroup(items[i + 1])) return std::nullopt;
        condition = "not " + SerializeContainerGroup(items[i + 1]);
      } else {
        if (!isGroup(items[i])) return std::nullopt;
        condition = SerializeContainerGroup(items[i]);
        std::string joiner;
        for (size_t k = i + 1; k < items.size(); k += 2) {
          const std::string op = WordOf(items[k]);
          if ((op != "and" && op != "or") || (!joiner.empty() && joiner != op) || k + 1 >= items.size() || !isGroup(items[k + 1])) return std::nullopt;
          joiner = op;
          condition += " " + op + " " + SerializeContainerGroup(items[k + 1]);
        }
      }
    }
    std::string text = name;
    if (!condition.empty()) text += (text.empty() ? "" : " ") + condition;
    result.text += (result.text.empty() ? "" : ", ") + text;
    if (parts.size() == 1) {
      result.name = name;
      result.condition = condition;
    }
  }
  return result;
}

// "parse a media query list", approximately: each query is a media type with conditions, or conditions alone; one
// that is not made of those is "not all".
std::string SerializeMediaQuery(const ComponentValues& tokens) {
  ComponentValues values = Trimmed(tokens);
  if (values.empty()) return "";
  const auto word = [](const ComponentValue& v) { return v.IsIdent() ? Lower(v.token.value) : std::string(); };
  std::string out;
  size_t i = 0;
  bool ok = true;
  const auto skipSpace = [&] {
    while (i < values.size() && values[i].IsWhitespace()) ++i;
  };
  const auto condition = [&](const ComponentValue& v) {
    return v.IsBlock(T::LeftParen) || (v.kind == ComponentValue::Kind::Function);
  };
  skipSpace();
  std::string prefix;
  if (i < values.size() && (word(values[i]) == "not" || word(values[i]) == "only")) {
    prefix = word(values[i]);
    ++i;
    skipSpace();
    if (i >= values.size()) return "not all";
  }
  if (i < values.size() && values[i].IsIdent()) {
    const std::string type = word(values[i]);
    // A media type cannot be one of the operators.
    if (type == "and" || type == "or" || type == "not" || type == "only" || type == "layer") return "not all";
    ++i;
    if (!prefix.empty()) out = prefix + " ";
    out += SerializeIdentifier(type);
    skipSpace();
    const bool omittable = type == "all" && prefix.empty();
    if (omittable && i < values.size()) out.clear();
    while (i < values.size()) {
      if (word(values[i]) != "and") return "not all";
      ++i;
      skipSpace();
      if (i >= values.size() || !condition(values[i])) return "not all";
      out += (out.empty() ? "" : " and ") + SerializeConditionGroup(values[i]);
      ++i;
      skipSpace();
    }
    return out;
  }
  // A condition by itself: (a) and (b), (a) or (b), not (a).
  if (!prefix.empty() && prefix == "only") return "not all";
  if (!prefix.empty()) out = prefix + " ";
  if (i >= values.size() || !condition(values[i])) return "not all";
  out += SerializeConditionGroup(values[i]);
  ++i;
  skipSpace();
  std::string joiner;
  while (i < values.size()) {
    const std::string op = word(values[i]);
    if ((op != "and" && op != "or") || (!joiner.empty() && joiner != op) || prefix == "not") {
      ok = false;
      break;
    }
    joiner = op;
    ++i;
    skipSpace();
    if (i >= values.size() || !condition(values[i])) {
      ok = false;
      break;
    }
    out += " " + op + " " + SerializeConditionGroup(values[i]);
    ++i;
    skipSpace();
  }
  return ok ? out : "not all";
}

}  // namespace

std::string MediaList::Text() const {
  std::string out;
  for (size_t i = 0; i < queries.size(); ++i) out += (i ? ", " : "") + queries[i];
  return out;
}

void MediaList::SetText(std::string_view text) {
  NoteStyleChange();
  queries.clear();
  if (Trim(std::string(text)).empty()) return;
  for (const ComponentValues& part : SplitOnCommas(ParseComponentValues(text))) {
    const std::string query = SerializeMediaQuery(part);
    queries.push_back(query.empty() ? "not all" : query);
  }
}

void MediaList::Visit(Quanta::Visitor& visitor) {
  visitor.Mark(ownerSheet);
  visitor.Mark(ownerRule);
}

// ---- Rules ----

void CssRule::Visit(Quanta::Visitor& visitor) {
  visitor.Mark(parentRule);
  visitor.Mark(parentSheet);
  visitor.Mark(style);
  for (CssRule* rule : rules) visitor.Mark(rule);
  visitor.Mark(ruleList);
  visitor.Mark(media);
  visitor.Mark(importedSheet);
}

const std::vector<CssRule*>& CssRuleList::Rules() const {
  static const std::vector<CssRule*> none;
  if (ownerSheet) return ownerSheet->rules;
  if (ownerRule) return ownerRule->rules;
  return none;
}

void CssRuleList::Visit(Quanta::Visitor& visitor) {
  visitor.Mark(ownerSheet);
  visitor.Mark(ownerRule);
}

void CssStyleSheet::Visit(Quanta::Visitor& visitor) {
  visitor.Mark(ownerNode);
  visitor.Mark(parentSheet);
  visitor.Mark(ownerRule);
  visitor.Mark(media);
  for (CssRule* rule : rules) visitor.Mark(rule);
  visitor.Mark(ruleList);
  visitor.Mark(constructorDocument);
}

namespace {

// The rules inside a rule as its text lists them: one to a line, each after two spaces, empty nested declarations left out.
std::string Children(const CssRule& rule) {
  std::string out;
  for (const CssRule* inner : rule.rules) {
    if (inner->kind == RuleKind::NestedDeclarations && (!inner->style || inner->style->items.empty())) continue;
    out += "  " + inner->CssText() + "\n";
  }
  return out;
}

std::string GroupBody(const CssRule& rule) { return " {\n" + Children(rule) + "}"; }

}  // namespace

std::string CssRule::CssText() const {
  switch (kind) {
    case RuleKind::Style: {
      const std::string body = style ? style->Serialize() : "";
      const std::string children = Children(*this);
      if (children.empty()) return selectorText + (body.empty() ? " { }" : " { " + body + " }");
      return selectorText + " {\n" + (body.empty() ? "" : "  " + body + "\n") + children + "}";
    }
    case RuleKind::NestedDeclarations: return style ? style->Serialize() : "";
    case RuleKind::Import: {
      std::string out = "@import " + SerializeUrl(href);
      if (!layerName.empty()) out += layerName == "\x01" ? " layer" : " layer(" + layerName + ")";
      if (!supportsText.empty()) out += " supports(" + supportsText + ")";
      if (media && !media->queries.empty()) out += " " + media->Text();
      return out + ";";
    }
    case RuleKind::Media: return "@media " + (media ? media->Text() : std::string()) + GroupBody(*this).replace(0, 0, "");
    case RuleKind::Supports: return "@supports " + supportsText + GroupBody(*this);
    case RuleKind::Namespace: return "@namespace " + (prefix.empty() ? "" : SerializeIdentifier(prefix) + " ") + SerializeUrl(namespaceUri) + ";";
    case RuleKind::FontFace: return "@font-face { " + (style ? style->Serialize() : "") + " }";
    case RuleKind::FontPaletteValues: {
      const std::string body = style ? style->Serialize() : "";
      return "@font-palette-values " + SerializeIdentifier(name) + " {" + (body.empty() ? " }" : " " + body + " }");
    }
    case RuleKind::Page: {
      std::string body = style ? style->Serialize() : "";
      return "@page " + (selectorText.empty() ? "" : selectorText + " ") + "{" + (body.empty() ? " }" : " " + body + " }");
    }
    case RuleKind::Keyframes: {
      // A name that is not an identifier, or is one that the syntax reserves, is written as a string.
      const std::string lower = Lower(name);
      const bool reserved = lower == "none" || lower == "default" || lower == "initial" || lower == "inherit" || lower == "unset" || lower == "revert" || lower == "revert-layer" || lower == "revert-rule";
      return "@keyframes " + (reserved ? SerializeString(name) : SerializeIdentifier(name)) + GroupBody(*this);
    }
    case RuleKind::Keyframe: {
      std::string body = style ? style->Serialize() : "";
      return name + " {" + (body.empty() ? " }" : " " + body + " }");
    }
    case RuleKind::CounterStyle: return "@counter-style " + SerializeIdentifier(name) + " { " + (style ? style->Serialize() : "") + " }";
    case RuleKind::Property: return "@property " + name + " { " + (style ? style->Serialize() : "") + " }";
    case RuleKind::LayerBlock: return "@layer " + (name.empty() ? "" : name + " ") + "{\n" + Children(*this) + "}";
    case RuleKind::LayerStatement: return "@layer " + layerName + ";";
    case RuleKind::Container: return "@container " + prelude + GroupBody(*this);
    case RuleKind::Scope: return "@scope" + (prelude.empty() ? "" : " " + prelude) + GroupBody(*this);
    case RuleKind::StartingStyle: return "@starting-style" + GroupBody(*this);
  }
  return "";
}

// ---- Parsing a sheet ----

namespace {

// The namespaces of a selector list against the sheet's @namespace rules: a prefix has to have been declared, and with no
// default namespace a selector for any namespace is just a selector.
bool ResolveNamespacesImpl(SelectorList& list, const CssStyleSheet* sheet) {
  bool hasDefault = false;
  std::string defaultUri;
  if (sheet) {
    for (const CssRule* rule : sheet->rules) {
      if (rule->kind == RuleKind::Namespace && rule->prefix.empty()) {
        hasDefault = true;
        defaultUri = rule->namespaceUri;
      }
    }
  }
  const auto uriOf = [&](const std::string& prefix) {
    if (sheet) {
      for (const CssRule* rule : sheet->rules) {
        if (rule->kind == RuleKind::Namespace && rule->prefix == prefix) return rule->namespaceUri;
      }
    }
    return std::string();
  };
  const auto declared = [&](const std::string& prefix) {
    if (!sheet) return false;
    for (const CssRule* rule : sheet->rules) {
      if (rule->kind == RuleKind::Namespace && rule->prefix == prefix) return true;
    }
    return false;
  };
  const std::function<bool(SelectorList&)> walk = [&](SelectorList& selectors) {
    for (ComplexSelector& complex : selectors) {
      for (CompoundSelector& compound : complex.compounds) {
        for (SimpleSelector& simple : compound.simples) {
          const bool named = simple.kind == SimpleSelector::Kind::Type || simple.kind == SimpleSelector::Kind::Universal || simple.kind == SimpleSelector::Kind::Attribute;
          if (named && simple.namespaceName && *simple.namespaceName != "*" && !simple.namespaceName->empty() && !declared(*simple.namespaceName)) return false;
          if (named && !hasDefault && simple.kind != SimpleSelector::Kind::Attribute && simple.namespaceName && *simple.namespaceName == "*") simple.namespaceName.reset();
          if (named && simple.namespaceName && *simple.namespaceName != "*" && !simple.namespaceName->empty()) simple.namespaceUri = uriOf(*simple.namespaceName);
          else if (named && !simple.namespaceName && hasDefault && simple.kind != SimpleSelector::Kind::Attribute) simple.namespaceUri = defaultUri;
          if (simple.list && !walk(*simple.list)) return false;
        }
      }
    }
    return true;
  };
  return walk(list);
}

// Builds the rule `syntax` is, in the context of `sheet` and `parent`; null when it is no valid rule there.
CssRule* BuildRule(Context& ctx, const Rule& syntax, CssStyleSheet* sheet, CssRule* parent);

CssDeclarations* DeclarationsFrom(Context& ctx, const std::vector<BlockItem>& items, CssRule* owner) {
  CssDeclarations* declarations = NewDeclarations(ctx);
  declarations->parentRule = owner;
  for (const BlockItem& item : items) {
    if (!item.isDeclaration) continue;
    std::string name = item.declaration.name;
    if (declarations->IsDescriptorBlock()) {
      if (const std::optional<std::string> descriptor = DescriptorName(SetOf(owner), name)) declarations->Apply(*descriptor, css::Serialize(Trimmed(item.declaration.value)), item.declaration.important);
      continue;
    }
    if (!NormalizePropertyName(name)) continue;
    if (!name.starts_with("--") && HasBadTokens(item.declaration.value)) continue;
    const std::string source = name.starts_with("--") ? item.declaration.originalText : Serialize(Trimmed(item.declaration.value));
    declarations->Apply(name, source, item.declaration.important);
  }
  return declarations;
}

void AddChild(CssRule* parent, CssRule* child) {
  parent->rules.push_back(child);
  parent->NoteWrite();
}

// The rules inside the block of a grouping rule; declarations in it (nested style rule context) are kept apart.
// The selectors of the style rule that & stands for in the rules inside `parent`: the nearest style rule going outwards.
std::shared_ptr<SelectorList> NestingParentOf(const CssRule* parent) {
  for (const CssRule* rule = parent; rule; rule = rule->parentRule) {
    if (rule->kind == RuleKind::Style) return rule->selectors;
  }
  return nullptr;
}

// Whether rules inside `parent` are nested in a style rule or a scope rule, which is what lets declarations stand in them.
bool InNesting(const CssRule* parent) {
  for (const CssRule* rule = parent; rule; rule = rule->parentRule) {
    if (rule->kind == RuleKind::Style || rule->kind == RuleKind::Scope) return true;
  }
  return false;
}

// The nearest style or scope rule going outwards.
const CssRule* NestingRuleOf(const CssRule* parent) {
  for (const CssRule* rule = parent; rule; rule = rule->parentRule) {
    if (rule->kind == RuleKind::Style || rule->kind == RuleKind::Scope) return rule;
  }
  return nullptr;
}

// mode 0: a rule list. 1: a style rule's block: its declarations and the rules in it. 2: the block of a conditional group
// rule in a style rule: declarations in it are nested declarations rules.
void FillGroup(Context& ctx, CssRule* group, const Rule& syntax, CssStyleSheet* sheet, int mode) {
  if (!syntax.hasBlock) return;
  if (mode == 2) {
    const std::vector<BlockItem> items = ParseBlockContents(syntax.block);
    std::vector<BlockItem> run;
    const auto flush = [&] {
      if (run.empty()) return;
      CssRule* nested = NewRule(ctx, RuleKind::NestedDeclarations);
      nested->parentRule = group;
      nested->parentSheet = sheet;
      nested->style = DeclarationsFrom(ctx, run, nested);
      AddChild(group, nested);
      run.clear();
    };
    for (const BlockItem& item : items) {
      if (item.isDeclaration) {
        run.push_back(item);
        continue;
      }
      flush();
      if (CssRule* inner = BuildRule(ctx, item.rule, sheet, group)) AddChild(group, inner);
    }
    flush();
    return;
  }
  if (mode == 1) {
    // A style rule: declarations and nested rules, in order. The declarations before the first nested rule are the
    // rule's own; the rest are wrapped in nested declarations rules.
    const std::vector<BlockItem> items = ParseBlockContents(syntax.block);
    std::vector<BlockItem> own;
    std::vector<BlockItem> run;
    bool seenRule = false;
    CssDeclarations* declarations = nullptr;
    for (const BlockItem& item : items) {
      if (item.isDeclaration) {
        (seenRule ? run : own).push_back(item);
        continue;
      }
      if (!seenRule) {
        seenRule = true;
        declarations = DeclarationsFrom(ctx, own, group);
        group->style = declarations;
      } else if (!run.empty()) {
        CssRule* nested = NewRule(ctx, RuleKind::NestedDeclarations);
        nested->parentRule = group;
        nested->parentSheet = sheet;
        nested->style = DeclarationsFrom(ctx, run, nested);
        AddChild(group, nested);
        run.clear();
      }
      if (CssRule* inner = BuildRule(ctx, item.rule, sheet, group)) AddChild(group, inner);
    }
    if (!seenRule) group->style = DeclarationsFrom(ctx, own, group);
    else if (!run.empty()) {
      CssRule* nested = NewRule(ctx, RuleKind::NestedDeclarations);
      nested->parentRule = group;
      nested->parentSheet = sheet;
      nested->style = DeclarationsFrom(ctx, run, nested);
      AddChild(group, nested);
    }
    return;
  }
  for (const Rule& inner : ParseRuleList(syntax.block)) {
    if (CssRule* rule = BuildRule(ctx, inner, sheet, group)) AddChild(group, rule);
  }
}

// The url of an @import or @namespace: a string or url(); false if the prelude does not begin with one.
bool TakeUrl(const ComponentValues& values, size_t& i, std::string& out) {
  while (i < values.size() && values[i].IsWhitespace()) ++i;
  if (i >= values.size()) return false;
  const ComponentValue& v = values[i];
  if (v.IsToken(T::String) || v.IsToken(T::Url)) {
    out = v.token.value;
    ++i;
    return true;
  }
  if (v.kind == ComponentValue::Kind::Function && IEquals(v.name, "url")) {
    // url("x") with a string argument.
    for (const ComponentValue& child : v.children) {
      if (child.IsToken(T::String)) {
        out = child.token.value;
        ++i;
        return true;
      }
    }
  }
  return false;
}

CssRule* BuildImport(Context& ctx, const Rule& syntax, CssStyleSheet* sheet, CssRule* parent) {
  if (parent || syntax.hasBlock) return nullptr;
  CssRule* rule = NewRule(ctx, RuleKind::Import);
  size_t i = 0;
  if (!TakeUrl(syntax.prelude, i, rule->href)) return nullptr;
  rule->media = NewMediaList(ctx);
  rule->media->ownerRule = rule;
  ComponentValues rest;
  for (; i < syntax.prelude.size(); ++i) {
    const ComponentValue& v = syntax.prelude[i];
    if (v.IsWhitespace() && rest.empty()) continue;
    if (rest.empty() && v.IsIdent() && IEquals(v.token.value, "layer")) {
      rule->layerName = "\x01";
      continue;
    }
    if (rest.empty() && v.kind == ComponentValue::Kind::Function && IEquals(v.name, "layer")) {
      // ident(.ident)*, no whitespace inside; anything else is left to the media query to make what it can of.
      bool valid = !v.children.empty();
      bool expectIdent = true;
      std::string name;
      for (const ComponentValue& c : v.children) {
        if (expectIdent && c.IsIdent()) name += SerializeIdentifier(c.token.value);
        else if (!expectIdent && c.IsDelim('.')) name += ".";
        else valid = false;
        expectIdent = !expectIdent;
      }
      if (valid && !expectIdent) {
        rule->layerName = name;
        continue;
      }
    }
    if (rest.empty() && v.kind == ComponentValue::Kind::Function && IEquals(v.name, "supports")) {
      rule->supportsText = Serialize(Trimmed(v.children));
      continue;
    }
    rest.push_back(v);
  }
  rule->media->SetText(Serialize(Trimmed(rest)));
  (void)sheet;
  return rule;
}

}  // namespace

std::optional<std::string> NormalizeKeyText(const std::string& text) {
  std::string key;
  for (const ComponentValues& part : SplitOnCommas(Trimmed(ParseComponentValues(text)))) {
    const ComponentValues trimmed = Trimmed(part);
    if (trimmed.size() != 1) return std::nullopt;
    std::string item;
    if (trimmed[0].IsIdent() && Lower(trimmed[0].token.value) == "from") item = "0%";
    else if (trimmed[0].IsIdent() && Lower(trimmed[0].token.value) == "to") item = "100%";
    else if (trimmed[0].IsToken(T::Percentage) && trimmed[0].token.number >= 0 && trimmed[0].token.number <= 100) item = FormatNumber(trimmed[0].token.number) + "%";
    else return std::nullopt;
    key += (key.empty() ? "" : ", ") + item;
  }
  return key.empty() ? std::nullopt : std::optional<std::string>(key);
}

CssRule* BuildKeyframe(Context& ctx, const Rule& frame, CssStyleSheet* sheet, CssRule* parent) {
  if (frame.isAtRule || !frame.hasBlock) return nullptr;
  const std::optional<std::string> key = NormalizeKeyText(Serialize(Trimmed(frame.prelude)));
  if (!key) return nullptr;
  CssRule* keyframe = NewRule(ctx, RuleKind::Keyframe);
  keyframe->name = *key;
  keyframe->parentRule = parent;
  keyframe->parentSheet = sheet;
  keyframe->style = DeclarationsFrom(ctx, ParseBlockContents(frame.block), keyframe);
  return keyframe;
}

namespace {

CssRule* BuildRule(Context& ctx, const Rule& syntax, CssStyleSheet* sheet, CssRule* parent) {
  CssRule* rule = nullptr;
  if (!syntax.isAtRule) {
    // A style rule.
    const std::string text = Serialize(Trimmed(syntax.prelude));
    const CssRule* nesting = NestingRuleOf(parent);
    const std::shared_ptr<SelectorList> nestingParent = nesting && nesting->kind == RuleKind::Style ? nesting->selectors : nullptr;
    std::optional<SelectorList> selectors = nesting ? ParseNestedSelectorList(text, nestingParent, nesting->kind == RuleKind::Scope) : ParseSelectorListForRule(text);
    if (!selectors || !ResolveNamespaces(*selectors, sheet)) return nullptr;
    rule = NewRule(ctx, RuleKind::Style);
    rule->selectors = std::make_shared<SelectorList>(std::move(*selectors));
    rule->selectorText = SerializeSelectorList(*rule->selectors);
    rule->parentRule = parent;
    rule->parentSheet = sheet;
    rule->isNested = parent != nullptr;
    FillGroup(ctx, rule, syntax, sheet, 1);
    return rule;
  }
  const std::string name = Lower(syntax.name);
  if (name == "import") {
    rule = BuildImport(ctx, syntax, sheet, parent);
  } else if (name == "media") {
    if (!syntax.hasBlock) return nullptr;
    rule = NewRule(ctx, RuleKind::Media);
    rule->media = NewMediaList(ctx);
    rule->media->ownerRule = rule;
    rule->media->SetText(Serialize(Trimmed(syntax.prelude)));
    rule->parentRule = parent;
    rule->parentSheet = sheet;
    FillGroup(ctx, rule, syntax, sheet, InNesting(parent) ? 2 : 0);
    return rule;
  } else if (name == "supports") {
    if (!syntax.hasBlock) return nullptr;
    const std::string condition = Serialize(Trimmed(syntax.prelude));
    if (condition.empty()) return nullptr;
    rule = NewRule(ctx, RuleKind::Supports);
    rule->supportsText = condition;
    rule->parentRule = parent;
    rule->parentSheet = sheet;
    FillGroup(ctx, rule, syntax, sheet, InNesting(parent) ? 2 : 0);
    return rule;
  } else if (name == "namespace") {
    if (parent || syntax.hasBlock) return nullptr;
    rule = NewRule(ctx, RuleKind::Namespace);
    size_t i = 0;
    while (i < syntax.prelude.size() && syntax.prelude[i].IsWhitespace()) ++i;
    if (i < syntax.prelude.size() && syntax.prelude[i].IsIdent()) {
      rule->prefix = syntax.prelude[i].token.value;
      ++i;
    }
    if (!TakeUrl(syntax.prelude, i, rule->namespaceUri)) return nullptr;
    while (i < syntax.prelude.size() && syntax.prelude[i].IsWhitespace()) ++i;
    if (i != syntax.prelude.size()) return nullptr;
  } else if (name == "font-face") {
    if (!syntax.hasBlock || !syntax.prelude.empty()) {
      if (!Trimmed(syntax.prelude).empty() || !syntax.hasBlock) return nullptr;
    }
    rule = NewRule(ctx, RuleKind::FontFace);
    rule->style = DeclarationsFrom(ctx, ParseBlockContents(syntax.block), rule);
  } else if (name == "page") {
    if (!syntax.hasBlock) return nullptr;
    rule = NewRule(ctx, RuleKind::Page);
    rule->selectorText = Serialize(Trimmed(syntax.prelude));
    rule->style = DeclarationsFrom(ctx, ParseBlockContents(syntax.block), rule);
  } else if (name == "keyframes" || name == "-webkit-keyframes") {
    if (!syntax.hasBlock) return nullptr;
    const ComponentValues prelude = Trimmed(syntax.prelude);
    if (prelude.size() != 1 || !(prelude[0].IsIdent() || prelude[0].IsToken(T::String))) return nullptr;
    if (prelude[0].IsToken(T::String) ? prelude[0].token.value.empty() : (Lower(prelude[0].token.value) == "none" || IsCssWideKeyword(prelude) || Lower(prelude[0].token.value) == "default")) return nullptr;
    rule = NewRule(ctx, RuleKind::Keyframes);
    rule->name = prelude[0].token.value;
    rule->parentRule = parent;
    rule->parentSheet = sheet;
    for (const Rule& frame : ParseRuleList(Serialize(syntax.block))) {
      if (frame.isAtRule) continue;
      if (CssRule* keyframe = BuildKeyframe(ctx, frame, sheet, rule)) AddChild(rule, keyframe);
    }
    return rule;
  } else if (name == "layer") {
    const ComponentValues prelude = Trimmed(syntax.prelude);
    // A name is idents separated by dots, a statement is a comma-separated list of them.
    std::string names;
    bool valid = true;
    for (const ComponentValues& part : SplitOnCommas(prelude)) {
      std::string one;
      for (const ComponentValue& v : Trimmed(part)) {
        if (v.IsIdent()) one += SerializeIdentifier(v.token.value);
        else if (v.IsDelim('.')) one += ".";
        else valid = false;  // whitespace inside a name too: A . B is not A.B
      }
      if (one.empty() || one.front() == '.' || one.back() == '.' || one.find("..") != std::string::npos) {
        if (!(prelude.empty() && syntax.hasBlock)) valid = false;
      }
      names += (names.empty() ? "" : ", ") + one;
    }
    if (!valid) return nullptr;
    if (syntax.hasBlock) {
      if (names.find(',') != std::string::npos) return nullptr;
      rule = NewRule(ctx, RuleKind::LayerBlock);
      rule->name = names;
      rule->parentRule = parent;
      rule->parentSheet = sheet;
      FillGroup(ctx, rule, syntax, sheet, InNesting(parent) ? 2 : 0);
      return rule;
    }
    if (names.empty()) return nullptr;
    rule = NewRule(ctx, RuleKind::LayerStatement);
    rule->layerName = names;
  } else if (name == "property") {
    const ComponentValues prelude = Trimmed(syntax.prelude);
    if (!syntax.hasBlock || prelude.size() != 1 || !prelude[0].IsIdent() || !prelude[0].token.value.starts_with("--") || prelude[0].token.value.size() < 3) return nullptr;
    // The descriptors: syntax and inherits are required, and initial-value unless the syntax is universal.
    std::optional<std::string> syntaxText, initialText;
    std::optional<bool> inherits;
    for (const BlockItem& item : ParseBlockContents(syntax.block)) {
      if (!item.isDeclaration) continue;
      const std::string descriptor = Lower(item.declaration.name);
      const ComponentValues value = Trimmed(item.declaration.value);
      if (descriptor == "syntax") {
        if (value.size() == 1 && value[0].IsToken(T::String)) syntaxText = value[0].token.value;
        else syntaxText.reset();
      } else if (descriptor == "inherits") {
        if (value.size() == 1 && value[0].IsIdent() && (Lower(value[0].token.value) == "true" || Lower(value[0].token.value) == "false")) inherits = Lower(value[0].token.value) == "true";
        else inherits.reset();
      } else if (descriptor == "initial-value") {
        initialText = css::Serialize(value);
      }
    }
    if (!syntaxText || !inherits) return nullptr;
    std::string error;
    std::optional<RegisteredProperty> registration = MakeRegistration(prelude[0].token.value, *syntaxText, *inherits, initialText, error);
    if (!registration) return nullptr;
    rule = NewRule(ctx, RuleKind::Property);
    rule->name = prelude[0].token.value;
    rule->registered = registration;
    CssDeclarations* declarations = NewDeclarations(ctx);
    declarations->parentRule = rule;
    AddOrReplace(declarations->items, {"syntax", SerializeString(*syntaxText), false, "", ""});
    AddOrReplace(declarations->items, {"inherits", *inherits ? "true" : "false", false, "", ""});
    if (initialText) AddOrReplace(declarations->items, {"initial-value", registration->universal() ? *initialText : *registration->initialValue, false, "", ""});
    rule->style = declarations;
  } else if (name == "font-palette-values") {
    const ComponentValues prelude = Trimmed(syntax.prelude);
    if (!syntax.hasBlock || prelude.size() != 1 || !prelude[0].IsIdent() || !prelude[0].token.value.starts_with("--")) return nullptr;
    rule = NewRule(ctx, RuleKind::FontPaletteValues);
    rule->name = prelude[0].token.value;
    rule->style = DeclarationsFrom(ctx, ParseBlockContents(syntax.block), rule);
  } else if (name == "counter-style") {
    const ComponentValues prelude = Trimmed(syntax.prelude);
    if (!syntax.hasBlock || prelude.size() != 1 || !prelude[0].IsIdent()) return nullptr;
    static const std::set<std::string> reserved = {"none", "initial", "inherit", "unset", "default", "revert", "revert-layer", "decimal", "disc", "square", "circle", "disclosure-open", "disclosure-closed"};
    if (reserved.count(Lower(prelude[0].token.value))) return nullptr;
    rule = NewRule(ctx, RuleKind::CounterStyle);
    rule->name = prelude[0].token.value;
    rule->style = DeclarationsFrom(ctx, ParseBlockContents(syntax.block), rule);
  } else if (name == "container") {
    if (!syntax.hasBlock) return nullptr;
    const std::optional<ContainerPrelude> container = ParseContainerPrelude(syntax.prelude);
    if (!container) return nullptr;
    rule = NewRule(ctx, RuleKind::Container);
    rule->prelude = container->text;
    rule->containerName = container->name;
    rule->containerQuery = container->condition;
    rule->parentRule = parent;
    rule->parentSheet = sheet;
    FillGroup(ctx, rule, syntax, sheet, InNesting(parent) ? 2 : 0);
    return rule;
  } else if (name == "scope") {
    if (!syntax.hasBlock) return nullptr;
    // [ ( <scope-start> ) ]? [ to ( <scope-end> ) ]?
    const ComponentValues prelude = Trimmed(syntax.prelude);
    size_t i = 0;
    const auto skipSpace = [&] {
      while (i < prelude.size() && prelude[i].IsWhitespace()) ++i;
    };
    const CssRule* nesting = NestingRuleOf(parent);
    const std::shared_ptr<SelectorList> nestingParent = nesting && nesting->kind == RuleKind::Style ? nesting->selectors : nullptr;
    std::shared_ptr<SelectorList> start, end;
    std::string text = "@scope";
    skipSpace();
    if (i < prelude.size() && prelude[i].IsBlock(T::LeftParen)) {
      std::optional<SelectorList> list = ParseScopeStart(Serialize(Trimmed(prelude[i].children)), nestingParent);
      if (!list || !ResolveNamespaces(*list, sheet)) return nullptr;
      start = std::make_shared<SelectorList>(std::move(*list));
      text += " (" + SerializeSelectorList(*start) + ")";
      ++i;
      skipSpace();
    }
    if (i < prelude.size()) {
      if (!prelude[i].IsIdent() || !IEquals(prelude[i].token.value, "to")) return nullptr;
      ++i;
      skipSpace();
      if (i >= prelude.size() || !prelude[i].IsBlock(T::LeftParen)) return nullptr;
      std::optional<SelectorList> list = ParseNestedSelectorList(Serialize(Trimmed(prelude[i].children)), nestingParent, true);
      if (!list || ContainsPseudoElement(*list) || !ResolveNamespaces(*list, sheet)) return nullptr;
      end = std::make_shared<SelectorList>(std::move(*list));
      text += " to (" + SerializeSelectorList(*end) + ")";
      ++i;
      skipSpace();
      if (i < prelude.size()) return nullptr;
    }
    rule = NewRule(ctx, RuleKind::Scope);
    rule->prelude = text.substr(6 + (text.size() > 6 ? 1 : 0));
    rule->scopeStart = start;
    rule->scopeEnd = end;
    rule->parentRule = parent;
    rule->parentSheet = sheet;
    FillGroup(ctx, rule, syntax, sheet, 2);
    return rule;
  } else if (name == "starting-style") {
    if (!syntax.hasBlock || !Trimmed(syntax.prelude).empty()) return nullptr;
    rule = NewRule(ctx, RuleKind::StartingStyle);
    rule->parentRule = parent;
    rule->parentSheet = sheet;
    FillGroup(ctx, rule, syntax, sheet, InNesting(parent) ? 2 : 0);
    return rule;
  } else {
    return nullptr;
  }
  if (rule) {
    rule->parentRule = parent;
    rule->parentSheet = sheet;
  }
  return rule;
}

}  // namespace

void ParseSheetInto(Context& ctx, CssStyleSheet* sheet, std::string_view text) {
  NoteStyleChange();
  struct ParsingBase {
    explicit ParsingBase(const std::string& base) { SetParsingBase(base); }
    ~ParsingBase() { SetParsingBase(""); }
  } parsingBase(sheet->baseUrl);
  for (CssRule* old : sheet->rules) DetachRule(old);
  sheet->rules.clear();
  bool importsAllowed = !sheet->disallowImport, namespacesAllowed = true;
  for (const Rule& syntax : ParseStylesheetContents(text)) {
    const std::string name = syntax.isAtRule ? Lower(syntax.name) : "";
    // @import comes before every other rule but @layer statements, and @namespace after those and before the rest.
    if (name == "import" && !importsAllowed) continue;
    if (name == "namespace" && !namespacesAllowed) continue;
    CssRule* rule = BuildRule(ctx, syntax, sheet, nullptr);
    if (!rule) continue;
    if (rule->kind != RuleKind::Import && rule->kind != RuleKind::LayerStatement) importsAllowed = false;
    if (rule->kind != RuleKind::Import && rule->kind != RuleKind::LayerStatement && rule->kind != RuleKind::Namespace) namespacesAllowed = false;
    sheet->rules.push_back(rule);
  }
  sheet->NoteWrite();
}

CssRule* ParseRuleText(Context& ctx, std::string_view text, CssStyleSheet* sheet, CssRule* parent, size_t index, std::string& error) {
  Rule syntax;
  if (!ParseRule(text, syntax)) {
    // Inside a style rule a run of declarations is a rule too: a nested declarations rule.
    if (parent && InNesting(parent)) {
      const std::vector<BlockItem> items = ParseBlockContents(text);
      bool all = !items.empty();
      for (const BlockItem& item : items) all = all && item.isDeclaration;
      if (all) {
        CssRule* nested = NewRule(ctx, RuleKind::NestedDeclarations);
        nested->parentRule = parent;
        nested->parentSheet = sheet;
        nested->style = DeclarationsFrom(ctx, items, nested);
        return nested;
      }
    }
    error = "SyntaxError";
    return nullptr;
  }
  if (parent && syntax.isAtRule && (Lower(syntax.name) == "import" || Lower(syntax.name) == "namespace")) {
    // Valid, but not where it is: a different mistake from a bad rule.
    error = BuildRule(ctx, syntax, sheet, nullptr) ? "HierarchyRequestError" : "SyntaxError";
    return nullptr;
  }
  CssRule* rule = BuildRule(ctx, syntax, sheet, parent);
  if (!rule) {
    error = "SyntaxError";
    return nullptr;
  }
  const std::vector<CssRule*>& list = parent ? parent->rules : sheet->rules;
  if (rule->kind == RuleKind::Namespace) {
    for (const CssRule* existing : list) {
      if (existing->kind != RuleKind::Import && existing->kind != RuleKind::Namespace && existing->kind != RuleKind::LayerStatement) {
        error = "InvalidStateError";
        return nullptr;
      }
    }
    for (size_t i = index; i < list.size(); ++i) {
      (void)i;
    }
  }
  if (rule->kind == RuleKind::Import) {
    if (sheet && sheet->disallowImport) {
      error = "SyntaxError";
      return nullptr;
    }
    for (size_t i = 0; i < std::min(index, list.size()); ++i) {
      if (list[i]->kind != RuleKind::Import && list[i]->kind != RuleKind::LayerStatement) {
        error = "HierarchyRequestError";
        return nullptr;
      }
    }
  } else if (rule->kind != RuleKind::LayerStatement && rule->kind != RuleKind::Namespace) {
    // Nothing that is not an @import or @namespace goes before one of those.
    for (size_t i = index; i < list.size(); ++i) {
      if (list[i]->kind == RuleKind::Import || list[i]->kind == RuleKind::Namespace) {
        error = "HierarchyRequestError";
        return nullptr;
      }
    }
  }
  if (rule->kind == RuleKind::Namespace) {
    for (size_t i = 0; i < index && i < list.size(); ++i) {
      if (list[i]->kind != RuleKind::Import && list[i]->kind != RuleKind::Namespace && list[i]->kind != RuleKind::LayerStatement) {
        error = "HierarchyRequestError";
        return nullptr;
      }
    }
  }
  return rule;
}

namespace {
SheetLoader g_sheetLoader;
}
void SetSheetLoader(SheetLoader loader) { g_sheetLoader = std::move(loader); }
const SheetLoader& GetSheetLoader() { return g_sheetLoader; }

std::string InsertRule(Context& ctx, CssStyleSheet* sheet, CssRule* parent, std::string_view text, uint32_t index, uint32_t& result) {
  std::vector<CssRule*>& list = parent ? parent->rules : sheet->rules;
  if (index > list.size()) return "IndexSizeError";
  std::string error;
  struct ParsingBase {
    explicit ParsingBase(const std::string& base) { SetParsingBase(base); }
    ~ParsingBase() { SetParsingBase(""); }
  } parsingBase(sheet ? sheet->baseUrl : "");
  CssRule* rule = ParseRuleText(ctx, text, sheet, parent, index, error);
  if (!rule) return error;
  NoteStyleChange();
  list.insert(list.begin() + index, rule);
  (parent ? static_cast<Quanta::DOMObject*>(parent) : static_cast<Quanta::DOMObject*>(sheet))->NoteWrite();
  result = index;
  if (rule->kind == RuleKind::Import && g_sheetLoader && !sheet->constructed) ProcessImports(ctx, sheet, g_sheetLoader);
  return "";
}

namespace {
void ForgetSheet(CssRule* rule) {
  rule->parentSheet = nullptr;
  for (CssRule* child : rule->rules) ForgetSheet(child);
  if (rule->importedSheet) rule->importedSheet->parentSheet = nullptr;
}
}  // namespace

void DetachRule(CssRule* rule) {
  rule->parentRule = nullptr;
  ForgetSheet(rule);
}

std::string DeleteRule(Context&, CssStyleSheet* sheet, CssRule* parent, uint32_t index) {
  std::vector<CssRule*>& list = parent ? parent->rules : sheet->rules;
  if (index >= list.size()) return "IndexSizeError";
  // A namespace rule cannot be removed while rules other than imports and namespaces follow it.
  if (list[index]->kind == RuleKind::Namespace) {
    for (const CssRule* other : list) {
      if (other->kind != RuleKind::Import && other->kind != RuleKind::Namespace && other->kind != RuleKind::LayerStatement) return "InvalidStateError";
    }
  }
  NoteStyleChange();
  DetachRule(list[index]);
  list.erase(list.begin() + index);
  return "";
}

}  // namespace solar::css

namespace solar::css {

bool SupportsDeclaration(const std::string& property, const std::string& value) {
  std::string name = property;
  if (!NormalizePropertyName(name)) return false;
  const ComponentValues values = Trimmed(ParseComponentValues(value));
  if (name.starts_with("--")) return true;
  if (values.empty()) return false;
  std::vector<Longhand> out;
  return ExpandDeclaration(name, Serialize(values), out);
}

namespace {

// <supports-in-parens>: (condition), (declaration), selector(...), or something unknown, which is false.
std::optional<bool> InParens(const ComponentValue& v);

std::optional<bool> Condition(const ComponentValues& values) {
  size_t i = 0;
  const auto skip = [&] {
    while (i < values.size() && values[i].IsWhitespace()) ++i;
  };
  skip();
  if (i >= values.size()) return std::nullopt;
  const auto word = [&](size_t at) { return at < values.size() && values[at].IsIdent() ? Lower(values[at].token.value) : std::string(); };
  if (word(i) == "not") {
    ++i;
    skip();
    if (i >= values.size()) return std::nullopt;
    const std::optional<bool> inner = InParens(values[i++]);
    skip();
    if (!inner || i != values.size()) return std::nullopt;
    return !*inner;
  }
  std::optional<bool> result = InParens(values[i++]);
  if (!result) return std::nullopt;
  std::string op;
  for (;;) {
    skip();
    if (i >= values.size()) return result;
    const std::string w = word(i);
    if ((w != "and" && w != "or") || (!op.empty() && op != w)) return std::nullopt;
    op = w;
    ++i;
    skip();
    if (i >= values.size()) return std::nullopt;
    const std::optional<bool> next = InParens(values[i++]);
    if (!next) return std::nullopt;
    result = w == "and" ? (*result && *next) : (*result || *next);
  }
}

std::optional<bool> InParens(const ComponentValue& v) {
  if (v.IsBlock(T::LeftParen)) {
    const ComponentValues inner = Trimmed(v.children);
    // A declaration: ident, colon, value.
    if (inner.size() >= 2 && inner[0].IsIdent() && (inner[1].IsToken(T::Colon) || (inner.size() > 2 && inner[1].IsWhitespace() && inner[2].IsToken(T::Colon)))) {
      Declaration declaration;
      if (!ParseDeclaration(Serialize(inner), declaration)) return false;
      const std::string value = declaration.name.starts_with("--") ? declaration.originalText : Serialize(Trimmed(declaration.value));
      return SupportsDeclaration(declaration.name, value);
    }
    if (std::optional<bool> nested = Condition(inner)) return *nested;
    return false;  // general-enclosed
  }
  if (v.kind == ComponentValue::Kind::Function) {
    const std::string name = Lower(v.name);
    if (name == "selector") return ParseSelectorList(Serialize(Trimmed(v.children))).has_value();
    return false;  // font-tech(), font-format() and unknown functions
  }
  return std::nullopt;
}

}  // namespace

bool SupportsCondition(const ComponentValues& condition) { return Condition(condition).value_or(false); }

}  // namespace solar::css

namespace solar::css {

void ProcessImports(Context& ctx, CssStyleSheet* sheet, const SheetLoader& load, int depth) {
  if (depth > 8) return;
  for (CssRule* rule : sheet->rules) {
    if (rule->kind != RuleKind::Import || rule->importedSheet) continue;
    std::optional<url::Url> base = url::Parse(sheet->baseUrl);
    std::optional<url::Url> parsed = url::Parse(rule->href, base ? &*base : nullptr);
    if (!parsed) continue;
    const std::string address = url::Serialize(*parsed);
    // A sheet reaching itself again through its imports.
    bool cycle = false;
    for (CssStyleSheet* up = sheet; up; up = up->parentSheet) {
      if (up->hasHref && up->href == address) cycle = true;
    }
    if (cycle) continue;
    std::optional<std::string> text = load(address);
    if (!text) continue;
    CssStyleSheet* imported = NewStyleSheet(ctx);
    imported->parentSheet = sheet;
    imported->ownerRule = rule;
    imported->href = address;
    imported->hasHref = true;
    imported->baseUrl = address;
    if (rule->media) imported->media = rule->media;  // the same list
    ParseSheetInto(ctx, imported, *text);
    rule->importedSheet = imported;
    rule->NoteWrite();
    ProcessImports(ctx, imported, load, depth + 1);
  }
}

CssStyleSheet* NewLinkedSheet(Context& ctx, dom::Element* link, const std::string& address, const std::string& text) {
  CssStyleSheet* sheet = NewStyleSheet(ctx);
  sheet->ownerNode = link;
  sheet->href = address;
  sheet->hasHref = true;
  sheet->baseUrl = address;
  if (const dom::Attr* media = link->FindAttribute("", "media")) sheet->media->SetText(media->value);
  if (const dom::Attr* title = link->FindAttribute("", "title")) {
    if (!title->value.empty()) {
      sheet->title = title->value;
      sheet->hasTitle = true;
    }
  }
  ParseSheetInto(ctx, sheet, text);
  return sheet;
}

}  // namespace solar::css

namespace solar::css {
bool ResolveNamespaces(SelectorList& list, const CssStyleSheet* sheet) { return ResolveNamespacesImpl(list, sheet); }
}  // namespace solar::css
