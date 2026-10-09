#include "solar/css/Cssom.h"

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

namespace {

const std::set<std::string>& KnownProperties() {
  static const std::set<std::string> names = {
      "align-content", "align-items", "align-self", "all", "animation", "animation-delay", "animation-direction", "animation-duration",
      "animation-fill-mode", "animation-iteration-count", "animation-name", "animation-play-state", "animation-timing-function",
      "appearance", "aspect-ratio", "backdrop-filter", "backface-visibility", "background", "background-attachment", "background-blend-mode",
      "background-clip", "background-color", "background-image", "background-origin", "background-position", "background-position-x",
      "background-position-y", "background-repeat", "background-size", "block-size", "border", "border-block", "border-block-color",
      "border-block-end", "border-block-end-color", "border-block-end-style", "border-block-end-width", "border-block-start",
      "border-block-start-color", "border-block-start-style", "border-block-start-width", "border-block-style", "border-block-width",
      "border-bottom", "border-bottom-color", "border-bottom-left-radius", "border-bottom-right-radius", "border-bottom-style",
      "border-bottom-width", "border-collapse", "border-color", "border-image", "border-inline", "border-inline-color", "border-inline-end",
      "border-inline-end-color", "border-inline-end-style", "border-inline-end-width", "border-inline-start", "border-inline-start-color",
      "border-inline-start-style", "border-inline-start-width", "border-inline-style", "border-inline-width", "border-left",
      "border-left-color", "border-left-style", "border-left-width", "border-radius", "border-right", "border-right-color",
      "border-right-style", "border-right-width", "border-spacing", "border-style", "border-top", "border-top-color",
      "border-top-left-radius", "border-top-right-radius", "border-top-style", "border-top-width", "border-width", "bottom", "box-shadow",
      "box-sizing", "break-after", "break-before", "break-inside", "caption-side", "caret-color", "clear", "clip", "clip-path", "color",
      "color-scheme", "column-count", "column-fill", "column-gap", "column-rule", "column-rule-color", "column-rule-style",
      "column-rule-width", "column-span", "column-width", "columns", "contain", "content", "counter-increment", "counter-reset",
      "counter-set", "cursor", "direction", "display", "empty-cells", "fill", "filter", "flex", "flex-basis", "flex-direction",
      "flex-flow", "flex-grow", "flex-shrink", "flex-wrap", "float", "font", "font-family", "font-feature-settings", "font-kerning",
      "font-size", "font-size-adjust", "font-stretch", "font-style", "font-variant", "font-variant-caps", "font-variant-east-asian",
      "font-variant-ligatures", "font-variant-numeric", "font-variation-settings", "font-weight", "gap", "grid", "grid-area",
      "grid-auto-columns", "grid-auto-flow", "grid-auto-rows", "grid-column", "grid-column-end", "grid-column-gap", "grid-column-start",
      "grid-gap", "grid-row", "grid-row-end", "grid-row-gap", "grid-row-start", "grid-template", "grid-template-areas",
      "grid-template-columns", "grid-template-rows", "height", "hyphens", "image-rendering", "inline-size", "inset", "inset-block",
      "inset-block-end", "inset-block-start", "inset-inline", "inset-inline-end", "inset-inline-start", "isolation", "justify-content",
      "justify-items", "justify-self", "left", "letter-spacing", "line-break", "line-height", "list-style", "list-style-image",
      "list-style-position", "list-style-type", "margin", "margin-block", "margin-block-end", "margin-block-start", "margin-bottom",
      "margin-inline", "margin-inline-end", "margin-inline-start", "margin-left", "margin-right", "margin-top", "mask", "max-block-size",
      "max-height", "max-inline-size", "max-width", "min-block-size", "min-height", "min-inline-size", "min-width", "mix-blend-mode",
      "object-fit", "object-position", "opacity", "order", "orphans", "outline", "outline-color", "outline-offset", "outline-style",
      "outline-width", "overflow", "overflow-anchor", "overflow-wrap", "overflow-x", "overflow-y", "padding", "padding-block",
      "padding-block-end", "padding-block-start", "padding-bottom", "padding-inline", "padding-inline-end", "padding-inline-start",
      "padding-left", "padding-right", "padding-top", "page-break-after", "page-break-before", "page-break-inside", "perspective",
      "perspective-origin", "place-content", "place-items", "place-self", "pointer-events", "position", "quotes", "resize", "right",
      "rotate", "row-gap", "scale", "scroll-behavior", "scroll-margin", "scroll-padding", "scrollbar-color", "scrollbar-width",
      "shape-outside", "table-layout", "tab-size", "text-align", "text-align-last", "text-decoration", "text-decoration-color",
      "text-decoration-line", "text-decoration-style", "text-decoration-thickness", "text-indent", "text-justify", "text-overflow",
      "text-shadow", "text-transform", "text-underline-offset", "text-underline-position", "top", "touch-action", "transform",
      "transform-origin", "transform-style", "transition", "transition-delay", "transition-duration", "transition-property",
      "transition-timing-function", "translate", "unicode-bidi", "user-select", "vertical-align", "visibility", "white-space",
      "widows", "width", "will-change", "word-break", "word-spacing", "word-wrap", "writing-mode", "z-index", "zoom",
  };
  return names;
}

}  // namespace

bool NormalizePropertyName(std::string& name) {
  if (name.size() >= 2 && name[0] == '-' && name[1] == '-') return name.size() > 2;
  std::string lower = Lower(name);
  if (lower.size() > 1 && lower[0] == '-' && lower[1] != '-') {
    // A vendor prefix: the engine does not know what it means, but the property is there for script to set and read.
    if (lower.starts_with("-webkit-") || lower.starts_with("-moz-") || lower.starts_with("-ms-") || lower.starts_with("-o-")) {
      name = lower;
      return true;
    }
  }
  if (KnownProperties().count(lower) == 0) return false;
  name = lower;
  return true;
}

// ---- Declarations ----

const DeclarationEntry* CssDeclarations::Find(const std::string& name) const {
  for (const DeclarationEntry& entry : items) {
    if (entry.name == name) return &entry;
  }
  return nullptr;
}

std::string CssDeclarations::Serialize() const {
  std::string out;
  for (const DeclarationEntry& entry : items) {
    if (!out.empty()) out += ' ';
    out += entry.name + ": " + entry.value + (entry.important ? " !important" : "") + ";";
  }
  return out;
}

namespace {

// Whether `value` (already trimmed) is a value the property may be given, and its serialization.
bool ParseValue(const std::string& name, const std::string& text, std::string& out) {
  const ComponentValues values = ParseComponentValues(text);
  // A ; or an unbalanced bit that closes the declaration is not a value.
  if (name.starts_with("--")) {
    out = Serialize(Trimmed(values));
    return true;
  }
  const ComponentValues trimmed = Trimmed(values);
  if (trimmed.empty()) return false;
  for (const ComponentValue& value : trimmed) {
    if (value.IsToken(T::Semicolon) || value.IsToken(T::BadString) || value.IsToken(T::BadUrl) || value.IsToken(T::RightBrace) || value.IsToken(T::RightParen) || value.IsToken(T::RightBracket)) return false;
    if (value.IsDelim('!')) return false;
  }
  out = Serialize(trimmed);
  return true;
}

void AddOrReplace(std::vector<DeclarationEntry>& items, DeclarationEntry entry) {
  for (DeclarationEntry& existing : items) {
    if (existing.name == entry.name) {
      existing = std::move(entry);
      return;
    }
  }
  items.push_back(std::move(entry));
}

}  // namespace

void CssDeclarations::SetText(Context& ctx, std::string_view text) {
  items.clear();
  for (const BlockItem& item : ParseBlockContents(text)) {
    if (!item.isDeclaration) continue;
    std::string name = item.declaration.name;
    if (!NormalizePropertyName(name)) continue;
    std::string value;
    const std::string source = name.starts_with("--") ? item.declaration.originalText : css::Serialize(Trimmed(item.declaration.value));
    if (!ParseValue(name, source, value)) continue;
    AddOrReplace(items, {name, value, item.declaration.important});
  }
  Changed(ctx);
}

bool CssDeclarations::Set(Context& ctx, const std::string& propertyName, const std::string& value, bool important) {
  std::string name = propertyName;
  if (!NormalizePropertyName(name)) return false;
  std::string serialized;
  if (!ParseValue(name, value, serialized)) return false;
  AddOrReplace(items, {name, serialized, important});
  Changed(ctx);
  return true;
}

std::string CssDeclarations::Remove(Context& ctx, const std::string& propertyName) {
  std::string name = propertyName;
  if (!name.starts_with("--")) name = Lower(name);
  for (auto it = items.begin(); it != items.end(); ++it) {
    if (it->name == name) {
      std::string old = it->value;
      items.erase(it);
      Changed(ctx);
      return old;
    }
  }
  return "";
}

void CssDeclarations::Changed(Context& ctx) {
  if (ownerElement && !updatingAttribute) {
    updatingAttribute = true;
    dom::SetAttribute(ctx, ownerElement, "style", Serialize());
    updatingAttribute = false;
  }
}

void CssDeclarations::Visit(Quanta::Visitor& visitor) {
  visitor.Mark(parentRule);
  visitor.Mark(ownerElement);
}

// ---- Media ----

namespace {

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
  const auto serializeCondition = [&](const ComponentValue& v) {
    std::string text = Serialize(v);
    // Inside the parentheses, names of features and keywords are lowercase; the spacing is the author's, collapsed.
    std::string collapsed;
    bool space = false;
    for (char c : text) {
      if (c == ' ' || c == '\n' || c == '\t') {
        space = true;
        continue;
      }
      if (space && !collapsed.empty() && collapsed.back() != '(') collapsed += ' ';
      space = false;
      collapsed += c;
    }
    return collapsed;
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
    out += type;
    skipSpace();
    while (i < values.size()) {
      if (word(values[i]) != "and") return "not all";
      ++i;
      skipSpace();
      if (i >= values.size() || !condition(values[i])) return "not all";
      out += " and " + serializeCondition(values[i]);
      ++i;
      skipSpace();
    }
    return out;
  }
  // A condition by itself: (a) and (b), (a) or (b), not (a).
  if (!prefix.empty() && prefix == "only") return "not all";
  if (!prefix.empty()) out = prefix + " ";
  if (i >= values.size() || !condition(values[i])) return "not all";
  out += serializeCondition(values[i]);
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
    out += " " + op + " " + serializeCondition(values[i]);
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

std::string Indent(const std::string& text) {
  std::string out;
  for (char c : text) {
    out += c;
    if (c == '\n') out += "  ";
  }
  return out;
}

std::string GroupBody(const CssRule& rule) {
  std::string out = " {";
  for (const CssRule* inner : rule.rules) out += "\n  " + Indent(inner->CssText());
  return out + "\n}";
}

}  // namespace

std::string CssRule::CssText() const {
  switch (kind) {
    case RuleKind::Style: {
      std::string body = style ? style->Serialize() : "";
      std::string out = selectorText + " {";
      if (!body.empty()) out += " " + body;
      for (const CssRule* inner : rules) out += "\n  " + Indent(inner->CssText());
      if (!rules.empty()) return out + "\n}";
      return out + (body.empty() ? " }" : " }");
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
    case RuleKind::Page: {
      std::string body = style ? style->Serialize() : "";
      return "@page " + (selectorText.empty() ? "" : selectorText + " ") + "{" + (body.empty() ? " }" : " " + body + " }");
    }
    case RuleKind::Keyframes: return "@keyframes " + SerializeIdentifier(name) + GroupBody(*this);
    case RuleKind::Keyframe: {
      std::string body = style ? style->Serialize() : "";
      return name + " {" + (body.empty() ? " }" : " " + body + " }");
    }
    case RuleKind::CounterStyle: return "@counter-style " + SerializeIdentifier(name) + " { " + (style ? style->Serialize() : "") + " }";
    case RuleKind::Property: return "@property " + name + " { " + (style ? style->Serialize() : "") + " }";
    case RuleKind::LayerBlock: return "@layer " + (name.empty() ? "" : name + " ") + "{" + (rules.empty() ? "\n}" : GroupBody(*this).substr(2));
    case RuleKind::LayerStatement: return "@layer " + layerName + ";";
    case RuleKind::Container: return "@container " + prelude + GroupBody(*this);
    case RuleKind::Scope: return "@scope" + (prelude.empty() ? "" : " " + prelude) + GroupBody(*this);
    case RuleKind::StartingStyle: return "@starting-style" + GroupBody(*this);
  }
  return "";
}

// ---- Parsing a sheet ----

namespace {

// Builds the rule `syntax` is, in the context of `sheet` and `parent`; null when it is no valid rule there.
CssRule* BuildRule(Context& ctx, const Rule& syntax, CssStyleSheet* sheet, CssRule* parent);

CssDeclarations* DeclarationsFrom(Context& ctx, const std::vector<BlockItem>& items, CssRule* owner) {
  CssDeclarations* declarations = NewDeclarations(ctx);
  declarations->parentRule = owner;
  for (const BlockItem& item : items) {
    if (!item.isDeclaration) continue;
    std::string name = item.declaration.name;
    if (!NormalizePropertyName(name)) continue;
    const std::string source = name.starts_with("--") ? item.declaration.originalText : Serialize(Trimmed(item.declaration.value));
    std::string value;
    if (!ParseValue(name, source, value)) continue;
    AddOrReplace(declarations->items, {name, value, item.declaration.important});
  }
  return declarations;
}

void AddChild(CssRule* parent, CssRule* child) {
  parent->rules.push_back(child);
  parent->NoteWrite();
}

// The rules inside the block of a grouping rule; declarations in it (nested style rule context) are kept apart.
void FillGroup(Context& ctx, CssRule* group, const Rule& syntax, CssStyleSheet* sheet, bool nestedStyle) {
  if (!syntax.hasBlock) return;
  if (nestedStyle) {
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
      rule->layerName = Serialize(Trimmed(v.children));
      continue;
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

CssRule* BuildRule(Context& ctx, const Rule& syntax, CssStyleSheet* sheet, CssRule* parent) {
  CssRule* rule = nullptr;
  if (!syntax.isAtRule) {
    // A style rule.
    const std::string text = Serialize(Trimmed(syntax.prelude));
    std::optional<SelectorList> selectors = ParseSelectorList(text);
    if (!selectors) return nullptr;
    rule = NewRule(ctx, RuleKind::Style);
    rule->selectors = std::make_shared<SelectorList>(std::move(*selectors));
    rule->selectorText = SerializeSelectorList(*rule->selectors);
    rule->parentRule = parent;
    rule->parentSheet = sheet;
    rule->isNested = parent != nullptr;
    FillGroup(ctx, rule, syntax, sheet, true);
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
    FillGroup(ctx, rule, syntax, sheet, false);
    return rule;
  } else if (name == "supports") {
    if (!syntax.hasBlock) return nullptr;
    const std::string condition = Serialize(Trimmed(syntax.prelude));
    if (condition.empty()) return nullptr;
    rule = NewRule(ctx, RuleKind::Supports);
    rule->supportsText = condition;
    rule->parentRule = parent;
    rule->parentSheet = sheet;
    FillGroup(ctx, rule, syntax, sheet, false);
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
    rule = NewRule(ctx, RuleKind::Keyframes);
    rule->name = prelude[0].token.value;
    rule->parentRule = parent;
    rule->parentSheet = sheet;
    for (const Rule& frame : ParseRuleList(Serialize(syntax.block))) {
      if (frame.isAtRule) continue;
      // The key text: from, to or percentages.
      std::string key;
      bool valid = true;
      for (const ComponentValues& part : SplitOnCommas(Trimmed(frame.prelude))) {
        const ComponentValues trimmed = Trimmed(part);
        if (trimmed.size() != 1) { valid = false; break; }
        std::string item;
        if (trimmed[0].IsIdent() && (IEquals(trimmed[0].token.value, "from") || IEquals(trimmed[0].token.value, "to"))) item = Lower(trimmed[0].token.value);
        else if (trimmed[0].IsToken(T::Percentage) && trimmed[0].token.number >= 0 && trimmed[0].token.number <= 100) item = Serialize(trimmed[0]);
        else { valid = false; break; }
        key += (key.empty() ? "" : ", ") + item;
      }
      if (!valid || key.empty()) continue;
      CssRule* keyframe = NewRule(ctx, RuleKind::Keyframe);
      keyframe->name = key;
      keyframe->parentRule = rule;
      keyframe->parentSheet = sheet;
      keyframe->style = DeclarationsFrom(ctx, ParseBlockContents(frame.block), keyframe);
      AddChild(rule, keyframe);
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
        else if (!v.IsWhitespace()) valid = false;
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
      FillGroup(ctx, rule, syntax, sheet, false);
      return rule;
    }
    if (names.empty()) return nullptr;
    rule = NewRule(ctx, RuleKind::LayerStatement);
    rule->layerName = names;
  } else if (name == "property") {
    const ComponentValues prelude = Trimmed(syntax.prelude);
    if (!syntax.hasBlock || prelude.size() != 1 || !prelude[0].IsIdent() || !prelude[0].token.value.starts_with("--") || prelude[0].token.value.size() < 3) return nullptr;
    rule = NewRule(ctx, RuleKind::Property);
    rule->name = prelude[0].token.value;
    rule->style = nullptr;
    const std::vector<BlockItem> items = ParseBlockContents(syntax.block);
    CssDeclarations* declarations = NewDeclarations(ctx);
    declarations->parentRule = rule;
    for (const BlockItem& item : items) {
      if (item.isDeclaration) AddOrReplace(declarations->items, {Lower(item.declaration.name), Serialize(Trimmed(item.declaration.value)), false});
    }
    rule->style = declarations;
  } else if (name == "counter-style") {
    const ComponentValues prelude = Trimmed(syntax.prelude);
    if (!syntax.hasBlock || prelude.size() != 1 || !prelude[0].IsIdent()) return nullptr;
    rule = NewRule(ctx, RuleKind::CounterStyle);
    rule->name = prelude[0].token.value;
    CssDeclarations* declarations = NewDeclarations(ctx);
    declarations->parentRule = rule;
    for (const BlockItem& item : ParseBlockContents(syntax.block)) {
      if (item.isDeclaration) AddOrReplace(declarations->items, {Lower(item.declaration.name), Serialize(Trimmed(item.declaration.value)), false});
    }
    rule->style = declarations;
  } else if (name == "container") {
    if (!syntax.hasBlock) return nullptr;
    rule = NewRule(ctx, RuleKind::Container);
    rule->prelude = Serialize(Trimmed(syntax.prelude));
    rule->parentRule = parent;
    rule->parentSheet = sheet;
    FillGroup(ctx, rule, syntax, sheet, false);
    return rule;
  } else if (name == "scope") {
    if (!syntax.hasBlock) return nullptr;
    rule = NewRule(ctx, RuleKind::Scope);
    rule->prelude = Serialize(Trimmed(syntax.prelude));
    rule->parentRule = parent;
    rule->parentSheet = sheet;
    FillGroup(ctx, rule, syntax, sheet, false);
    return rule;
  } else if (name == "starting-style") {
    if (!syntax.hasBlock || !Trimmed(syntax.prelude).empty()) return nullptr;
    rule = NewRule(ctx, RuleKind::StartingStyle);
    rule->parentRule = parent;
    rule->parentSheet = sheet;
    FillGroup(ctx, rule, syntax, sheet, false);
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
  sheet->rules.clear();
  bool importsAllowed = true, namespacesAllowed = true;
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
    error = "SyntaxError";
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

std::string InsertRule(Context& ctx, CssStyleSheet* sheet, CssRule* parent, std::string_view text, uint32_t index, uint32_t& result) {
  std::vector<CssRule*>& list = parent ? parent->rules : sheet->rules;
  if (index > list.size()) return "IndexSizeError";
  std::string error;
  CssRule* rule = ParseRuleText(ctx, text, sheet, parent, index, error);
  if (!rule) return error;
  list.insert(list.begin() + index, rule);
  (parent ? static_cast<Quanta::DOMObject*>(parent) : static_cast<Quanta::DOMObject*>(sheet))->NoteWrite();
  result = index;
  return "";
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
  list[index]->parentRule = nullptr;
  list[index]->parentSheet = nullptr;
  list.erase(list.begin() + index);
  return "";
}

}  // namespace solar::css
