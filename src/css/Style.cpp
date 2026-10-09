#include "solar/css/Style.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cmath>
#include <deque>
#include <map>
#include <set>
#include <unordered_map>

#include "solar/css/Calc.h"
#include "solar/css/Color.h"
#include "solar/css/Cssom.h"
#include "solar/css/MediaQuery.h"
#include "solar/css/Registry.h"
#include "solar/css/Selectors.h"
#include "solar/css/Shorthands.h"
#include "solar/css/Values.h"

namespace solar::css {

namespace {

using Cv = ComponentValue;
using T = Token::Type;

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

uint64_t g_styleVersion = 1;

// ---- The user agent style sheet ----

// The rendering section of the HTML standard, as far as this engine looks at it: what each element is by default.
const char* const kUserAgentSheet = R"CSS(
html, address, blockquote, body, center, dialog, div, figure, figcaption, footer, form, header, hr, legend, listing, main, p, plaintext, pre, search, xmp,
article, aside, h1, h2, h3, h4, h5, h6, hgroup, nav, section, dir, dd, dl, dt, menu, ol, ul, details, summary, fieldset, optgroup, option { display: block; }
[hidden], area, base, basefont, datalist, head, link, meta, noembed, noframes, param, rp, script, style, template, title { display: none; }
li { display: list-item; }
table { display: table; } caption { display: table-caption; } colgroup, colgroup[hidden] { display: table-column-group; } col, col[hidden] { display: table-column; }
thead, thead[hidden] { display: table-header-group; } tbody, tbody[hidden] { display: table-row-group; } tfoot, tfoot[hidden] { display: table-footer-group; }
tr, tr[hidden] { display: table-row; } td, th, td[hidden], th[hidden] { display: table-cell; }
input, select, button, textarea, meter, progress { display: inline-block; }
ruby { display: ruby; } rt { display: ruby-text; } slot { display: contents; }
body { margin: 8px; }
p, dl, multicol { margin-block: 1em; }
blockquote, figure { margin-block: 1em; margin-inline: 40px; }
address, cite, dfn, em, i, var { font-style: italic; }
b, strong, th { font-weight: bold; }
h1 { font-size: 2em; margin-block: 0.67em; font-weight: bold; }
h2 { font-size: 1.5em; margin-block: 0.83em; font-weight: bold; }
h3 { font-size: 1.17em; margin-block: 1em; font-weight: bold; }
h4 { font-size: 1em; margin-block: 1.33em; font-weight: bold; }
h5 { font-size: 0.83em; margin-block: 1.67em; font-weight: bold; }
h6 { font-size: 0.67em; margin-block: 2.33em; font-weight: bold; }
dd { margin-inline-start: 40px; }
ol, ul, menu, dir { margin-block: 1em; padding-inline-start: 40px; }
ul, menu, dir { list-style-type: disc; } ol { list-style-type: decimal; }
u, ins { text-decoration: underline; } s, strike, del { text-decoration: line-through; }
pre, listing, plaintext, xmp, code, kbd, samp, tt, textarea { font-family: monospace; }
pre, listing, plaintext, xmp { white-space: pre; }
small { font-size: smaller; } big { font-size: larger; } sub { vertical-align: sub; font-size: smaller; } sup { vertical-align: super; font-size: smaller; }
mark { background-color: yellow; color: black; }
a:any-link { color: #0000ee; text-decoration: underline; cursor: pointer; }
hr { color: gray; border-style: inset; border-width: 1px; margin-block: 0.5em; margin-inline: auto; overflow: hidden; }
fieldset { margin-inline: 2px; border: 2px groove; padding-block: 0.35em 0.625em; padding-inline: 0.75em; }
table { box-sizing: border-box; border-spacing: 2px; border-collapse: separate; text-indent: initial; }
td, th { padding: 1px; } th { text-align: center; }
caption { text-align: center; }
img { }
button, input, select, textarea { text-indent: initial; }
)CSS";

struct UserAgentRule {
  SelectorList selectors;
  std::vector<DeclarationEntry> declarations;
};

const std::vector<UserAgentRule>& UserAgentRules() {
  static const std::vector<UserAgentRule> rules = [] {
    std::vector<UserAgentRule> list;
    for (const Rule& syntax : ParseStylesheetContents(kUserAgentSheet)) {
      if (syntax.isAtRule) continue;
      UserAgentRule rule;
      std::optional<SelectorList> selectors = ParseSelectorList(Serialize(Trimmed(syntax.prelude)));
      if (!selectors) continue;
      rule.selectors = std::move(*selectors);
      for (const BlockItem& item : ParseBlockContents(syntax.block)) {
        if (!item.isDeclaration) continue;
        std::vector<Longhand> longhands;
        if (!ExpandDeclaration(Lower(item.declaration.name), Serialize(Trimmed(item.declaration.value)), longhands)) continue;
        for (Longhand& l : longhands) rule.declarations.push_back({l.name, l.value, false, l.pendingShorthand, l.pendingText});
      }
      list.push_back(std::move(rule));
    }
    return list;
  }();
  return rules;
}

// ---- Collecting what applies ----

struct Winner {
  const DeclarationEntry* entry = nullptr;
  int origin = -1;
  int layer = 0;
  Specificity specificity;
  uint32_t order = 0;
  bool author = false;
};

constexpr int kUnlayered = INT_MAX / 2;

bool Beats(const Winner& a, const Winner& b) {
  if (!b.entry) return true;
  if (a.origin != b.origin) return a.origin > b.origin;
  if (a.layer != b.layer) return a.layer > b.layer;
  if (b.specificity < a.specificity) return true;
  if (a.specificity < b.specificity) return false;
  return a.order > b.order;
}

struct Gatherer {
  dom::Element* element;
  MatchContext matchContext;
  std::map<std::string, Winner>& winners;
  uint32_t order = 0;
  std::map<std::string, int> layers;
  int nextLayer = 0;
  const MediaEnvironment& media = CurrentMediaEnvironment();
  bool hostScoped = false;  // the rules are of a shadow tree and the element is its host: author rules of the outer tree win

  int LayerId(const std::string& name, bool create) {
    const auto found = layers.find(name);
    if (found != layers.end()) return found->second;
    if (!create) return kUnlayered;
    layers[name] = nextLayer;
    return nextLayer++;
  }

  void Offer(const DeclarationEntry& entry, bool author, bool inlineStyle, int layer, Specificity spec) {
    Winner candidate;
    candidate.entry = &entry;
    candidate.author = author;
    candidate.specificity = spec;
    candidate.order = ++order;
    if (inlineStyle) candidate.specificity = Specificity{1000000, 0, 0};
    // Normal: user agent, author; important: author, user agent. Layers reverse for important declarations.
    candidate.origin = entry.important ? (author ? 2 : 3) : (author ? 1 : 0);
    candidate.layer = entry.important ? (layer == kUnlayered ? -1 : 1000000 - layer) : layer;
    Winner& slot = winners[entry.name];
    if (Beats(candidate, slot)) slot = candidate;
  }

  // `outer` is the specificity of the style rule these rules are in, when it matched the element: its nested declarations
  // apply as it does.
  void Rules(const std::vector<CssRule*>& rules, int layer, const Specificity* outer = nullptr) {
    for (CssRule* rule : rules) {
      switch (rule->kind) {
        case RuleKind::NestedDeclarations:
          if (outer && rule->style) {
            for (const DeclarationEntry& entry : rule->style->items) Offer(entry, true, false, layer, *outer);
          }
          break;
        case RuleKind::Style: {
          if (!rule->selectors || !rule->style) break;
          Specificity best;
          bool matched = false;
          for (const ComplexSelector& selector : *rule->selectors) {
            if (MatchesComplex(selector, element, matchContext)) {
              const Specificity s = SpecificityOf(selector);
              if (!matched || best < s) best = s;
              matched = true;
            }
          }
          if (matched) {
            for (const DeclarationEntry& entry : rule->style->items) Offer(entry, true, false, layer, best);
          }
          // Rules nested in a style rule apply on their own selectors; its trailing declarations as it does.
          Rules(rule->rules, layer, matched ? &best : nullptr);
          break;
        }
        case RuleKind::Media:
          if (!rule->media || MediaListMatches(rule->media->queries, media)) Rules(rule->rules, layer, outer);
          break;
        case RuleKind::Supports:
          if (SupportsCondition(ParseComponentValues(rule->supportsText))) Rules(rule->rules, layer, outer);
          break;
        case RuleKind::Import: {
          CssStyleSheet* imported = rule->importedSheet;
          if (!imported || imported->disabled) break;
          if (rule->media && !MediaListMatches(rule->media->queries, media)) break;
          if (!rule->supportsText.empty() && !SupportsCondition(ParseComponentValues(rule->supportsText))) break;
          int id = layer;
          if (!rule->layerName.empty()) id = rule->layerName == "\x01" ? LayerId("\x01import" + std::to_string(order) + std::to_string(nextLayer), true) : LayerId(rule->layerName, true);
          Rules(imported->rules, id);
          break;
        }
        case RuleKind::LayerBlock: {
          const int id = rule->name.empty() ? LayerId("\x01" + std::to_string(order) + std::to_string(nextLayer), true) : LayerId(rule->name, true);
          Rules(rule->rules, id, outer);
          break;
        }
        case RuleKind::LayerStatement: {
          // The layers are ordered by where they are first named.
          size_t start = 0;
          const std::string& names = rule->layerName;
          while (start < names.size()) {
            size_t comma = names.find(", ", start);
            LayerId(names.substr(start, comma == std::string::npos ? std::string::npos : comma - start), true);
            if (comma == std::string::npos) break;
            start = comma + 2;
          }
          break;
        }
        default: break;
      }
    }
  }
};

std::vector<CssStyleSheet*> SheetsOfTree(dom::Node* root, Quanta::Context* ctx = nullptr) {
  std::vector<CssStyleSheet*> sheets;
  for (dom::Node* node = root; node; node = node->NextInTree(root)) {
    dom::Element* element = dom::AsElement(node);
    if (element && element->styleSheet) sheets.push_back(static_cast<CssStyleSheet*>(element->styleSheet));
  }
  // Then the ones adopted, in the order they are in the array.
  Quanta::Object* adopted = root->IsDocument() ? static_cast<dom::Document*>(root)->adoptedStyleSheets : root->IsFragment() ? static_cast<dom::DocumentFragment*>(root)->adoptedStyleSheets : nullptr;
  if (adopted && ctx) {
    const uint32_t length = Quanta::Embed::ToUint32(*ctx, Quanta::Embed::Get(*ctx, Quanta::Embed::FromObject(adopted), "length"));
    for (uint32_t i = 0; i < length; ++i) {
      if (CssStyleSheet* sheet = Quanta::DOMObject::Cast<CssStyleSheet>(Quanta::Embed::Get(*ctx, Quanta::Embed::FromObject(adopted), std::to_string(i)))) sheets.push_back(sheet);
    }
  }
  return sheets;
}

dom::Element* FlatParent(dom::Element* element) {
  dom::Node* parent = element->parentNode;
  if (!parent) return nullptr;
  if (dom::Element* e = dom::AsElement(parent)) return e;
  if (parent->IsFragment() && static_cast<dom::DocumentFragment*>(parent)->isShadowRoot) return static_cast<dom::DocumentFragment*>(parent)->host;
  return nullptr;
}

// ---- The resolver ----

struct ElementStyle {
  uint64_t version = 0;
  std::map<std::string, Winner> cascade;
  std::unordered_map<std::string, std::string> computed;
  std::unordered_map<std::string, std::optional<std::string>> custom;
  std::set<std::string> resolving;
  std::vector<std::vector<DeclarationEntry>> storage;  // the style attribute's declarations
  bool built = false;
};

std::map<std::pair<dom::Element*, std::string>, ElementStyle>& Cache() {
  static std::map<std::pair<dom::Element*, std::string>, ElementStyle> cache;
  return cache;
}

ElementStyle& StyleOf(dom::Element* element, const std::string& pseudo = "") {
  static uint64_t cachedVersion = 0;
  const uint64_t version = dom::TreeVersion() * 1000003 + g_styleVersion;
  if (cachedVersion != version) {
    Cache().clear();
    cachedVersion = version;
  }
  ElementStyle& style = Cache()[{element, pseudo}];
  style.version = version;
  return style;
}

void Build(dom::Element* element, ElementStyle& style, const std::string& pseudo);

class Resolver {
 public:
  explicit Resolver(Quanta::Context& ctx) : ctx_(ctx) {}

  std::string Compute(dom::Element* element, const std::string& property, const std::string& pseudo = "");
  std::optional<std::string> Custom(dom::Element* element, const std::string& name, const std::string& pseudo = "");

 private:
  std::optional<std::string> Substitute(dom::Element* element, const std::string& pseudo, const std::string& text, int depth, std::set<std::string>& stack);
  bool SubstituteValues(dom::Element* element, const std::string& pseudo, const ComponentValues& in, ComponentValues& out, int depth, std::set<std::string>& stack);
  ComputeContext ContextFor(dom::Element* element, const std::string& property, const std::string& pseudo, const std::string& text);
  std::string Specified(dom::Element* element, const std::string& property, const PropertyDefinition& definition, bool& valid, const std::string& pseudo);
  std::string Finish(dom::Element* element, const std::string& property, const PropertyDefinition& definition, const std::string& specified, ComputeContext& context, const std::string& pseudo);

  Quanta::Context& ctx_;
  bool cycleHit_ = false;  // a property was asked for while it was being worked out
};

void Build(dom::Element* element, ElementStyle& style, const std::string& pseudo) {
  if (style.built) return;
  style.built = true;
  dom::Node* root = dom::ShadowIncludingRoot(element);
  MatchContext matchContext;
  matchContext.pseudoElement = pseudo.empty() ? nullptr : &pseudo;
  Gatherer gatherer{element, matchContext, style.cascade};
  // The user agent's rules first, then the author's.
  for (const UserAgentRule& rule : UserAgentRules()) {
    Specificity best;
    bool matched = false;
    for (const ComplexSelector& selector : rule.selectors) {
      if (MatchesComplex(selector, element, gatherer.matchContext)) {
        const Specificity s = SpecificityOf(selector);
        if (!matched || best < s) best = s;
        matched = true;
      }
    }
    if (matched) {
      for (const DeclarationEntry& entry : rule.declarations) gatherer.Offer(entry, false, false, kUnlayered, best);
    }
  }
  // The tree the element is in: a shadow tree's elements are styled by that tree's sheets (and the document's own are not for
  // them). The root of the element's own tree is not the document's when it is in a shadow tree.
  dom::Node* treeRoot = element;
  while (treeRoot->parentNode) treeRoot = treeRoot->parentNode;
  if (treeRoot && (treeRoot->IsDocument() || treeRoot->IsFragment())) {
    if (treeRoot->IsFragment() && static_cast<dom::DocumentFragment*>(treeRoot)->isShadowRoot) gatherer.matchContext.host = static_cast<dom::DocumentFragment*>(treeRoot)->host;
    for (CssStyleSheet* sheet : SheetsOfTree(treeRoot, element->nodeDocument ? element->nodeDocument->context : nullptr)) {
      if (sheet->disabled || !MediaListMatches(sheet->media ? sheet->media->queries : std::vector<std::string>(), gatherer.media)) continue;
      gatherer.Rules(sheet->rules, kUnlayered);
    }
  }
  // A shadow host also takes the :host rules of its shadow tree, below the rules of the tree it is in.
  if (element->shadowRoot && pseudo.empty()) {
    Gatherer inner{element, gatherer.matchContext, style.cascade};
    inner.matchContext.host = element;
    inner.order = gatherer.order;
    for (CssStyleSheet* sheet : SheetsOfTree(element->shadowRoot, element->nodeDocument ? element->nodeDocument->context : nullptr)) {
      if (sheet->disabled || !MediaListMatches(sheet->media ? sheet->media->queries : std::vector<std::string>(), inner.media)) continue;
      inner.hostScoped = true;
      inner.Rules(sheet->rules, -1000);
    }
  }
  // The style attribute (a pseudo-element has none).
  if (const dom::Attr* attribute = pseudo.empty() ? element->FindAttribute("", "style") : nullptr) {
    style.storage.push_back(ParseDeclarationList(attribute->value));
    for (const DeclarationEntry& entry : style.storage.back()) gatherer.Offer(entry, true, true, kUnlayered, Specificity{});
  }
}

// ---- var() ----

bool Resolver::SubstituteValues(dom::Element* element, const std::string& pseudo, const ComponentValues& in, ComponentValues& out, int depth, std::set<std::string>& stack) {
  if (depth > 64) return false;
  for (const Cv& v : in) {
    if (v.kind == Cv::Kind::Function && Lower(v.name) == "var") {
      // var( --name [, fallback] )
      size_t i = 0;
      while (i < v.children.size() && v.children[i].IsWhitespace()) ++i;
      if (i >= v.children.size() || !v.children[i].IsIdent() || !v.children[i].token.value.starts_with("--")) return false;
      const std::string name = v.children[i].token.value;
      ++i;
      while (i < v.children.size() && v.children[i].IsWhitespace()) ++i;
      ComponentValues fallback;
      bool hasFallback = false;
      if (i < v.children.size()) {
        if (!v.children[i].IsToken(T::Comma)) return false;
        hasFallback = true;
        for (size_t j = i + 1; j < v.children.size(); ++j) fallback.push_back(v.children[j]);
      }
      std::optional<std::string> value;
      if (!stack.count(name)) {
        stack.insert(name);
        value = Custom(element, name, pseudo);
        stack.erase(name);
      }
      if (value) {
        ComponentValues parts = ParseComponentValues(*value);
        for (Cv& p : parts) out.push_back(p);
      } else if (hasFallback) {
        if (!SubstituteValues(element, pseudo, fallback, out, depth + 1, stack)) return false;
      } else {
        return false;
      }
      continue;
    }
    if (v.kind != Cv::Kind::Token && ContainsSubstitution(v.children)) {
      Cv copy = v;
      copy.children.clear();
      if (!SubstituteValues(element, pseudo, v.children, copy.children, depth + 1, stack)) return false;
      out.push_back(copy);
      continue;
    }
    out.push_back(v);
  }
  return true;
}

std::optional<std::string> Resolver::Substitute(dom::Element* element, const std::string& pseudo, const std::string& text, int depth, std::set<std::string>& stack) {
  ComponentValues out;
  if (!SubstituteValues(element, pseudo, ParseComponentValues(text), out, depth, stack)) return std::nullopt;
  return Serialize(Trimmed(out));
}

// The registration of a custom property in the element's document, remembered until style changes.
std::optional<RegisteredProperty> RegistrationOf(dom::Element* element, const std::string& name) {
  static std::map<std::pair<dom::Document*, std::string>, std::optional<RegisteredProperty>> cache;
  static uint64_t cachedVersion = 0;
  const uint64_t version = dom::TreeVersion() * 1000003 + g_styleVersion;
  if (version != cachedVersion) {
    cache.clear();
    cachedVersion = version;
  }
  const auto key = std::make_pair(element->nodeDocument, name);
  const auto found = cache.find(key);
  if (found != cache.end()) return found->second;
  std::optional<RegisteredProperty> registration = LookupRegistered(element->nodeDocument, name);
  cache[key] = registration;
  return registration;
}

// The value of a custom property on an element: its own, or its parent's; nothing for the guaranteed-invalid value.
std::optional<std::string> Resolver::Custom(dom::Element* element, const std::string& name, const std::string& pseudo) {
  ElementStyle& style = StyleOf(element, pseudo);
  Build(element, style, pseudo);
  const auto cached = style.custom.find(name);
  if (cached != style.custom.end()) return cached->second;
  if (style.resolving.count(name)) return std::nullopt;  // a cycle
  style.resolving.insert(name);
  const std::optional<RegisteredProperty> registration = RegistrationOf(element, name);
  // The computed initial value of a registered property.
  const auto initialOf = [&]() -> std::optional<std::string> {
    if (!registration || !registration->initialValue) return std::nullopt;
    if (registration->universal()) return registration->initialValue;
    ComputeContext context = ContextFor(element, "--", pseudo, *registration->initialValue);
    ValueMatch match;
    if (!MatchSyntax(registration->matcherSyntax, Trimmed(ParseComponentValues(*registration->initialValue)), match, &context)) return std::nullopt;
    return SerializeValue(match.normalized);
  };
  const auto inheritedValue = [&]() -> std::optional<std::string> {
    if (!pseudo.empty()) return Custom(element, name, "");
    if (dom::Element* parent = FlatParent(element)) return Custom(parent, name, "");
    return std::nullopt;
  };
  std::optional<std::string> result;
  const auto winner = style.cascade.find(name);
  bool inherit = !registration || registration->inherits;  // an unregistered property always inherits
  bool useInitial = false;
  if (winner != style.cascade.end()) {
    const std::string& value = winner->second.entry->value;
    const ComponentValues parsed = Trimmed(ParseComponentValues(value));
    if (IsCssWideKeyword(parsed)) {
      const std::string keyword = Lower(parsed[0].token.value);
      if (keyword == "initial") {
        useInitial = true;
        inherit = false;
      } else if (keyword == "inherit") {
        inherit = true;
      }  // unset and the reverts: inherit if the property does
    } else {
      std::optional<std::string> text;
      if (ContainsSubstitution(parsed)) {
        std::set<std::string> stack{name};
        text = Substitute(element, pseudo, value, 0, stack);
      } else {
        text = value;
      }
      inherit = false;
      if (text && registration && !registration->universal()) {
        cycleHit_ = false;
        ComputeContext context = ContextFor(element, "--", pseudo, *text);
        ValueMatch match;
        if (cycleHit_) {
          // It depends on a value that depends on it.
          style.resolving.erase(name);
          style.custom[name] = std::nullopt;
          return std::nullopt;
        }
        if (MatchSyntax(registration->matcherSyntax, Trimmed(ParseComponentValues(*text)), match, &context)) result = SerializeValue(match.normalized);
        else inherit = registration->inherits, useInitial = !registration->inherits;  // invalid at computed-value time: as unset
      } else if (text) {
        result = Serialize(Trimmed(ParseComponentValues(*text)));
      } else if (registration) {
        inherit = registration->inherits;
        useInitial = !registration->inherits;
      }
    }
  } else if (registration && !registration->inherits) {
    useInitial = true;
    inherit = false;
  }
  if (useInitial) result = initialOf();
  else if (inherit) result = (pseudo.empty() && !FlatParent(element)) ? initialOf() : inheritedValue();
  style.resolving.erase(name);
  style.custom[name] = result;
  return result;
}

// ---- Computing ----

double FontSizeKeyword(const std::string& word) {
  static const std::map<std::string, double> sizes = {{"xx-small", 9}, {"x-small", 10}, {"small", 13}, {"medium", 16}, {"large", 18}, {"x-large", 24}, {"xx-large", 32}, {"xxx-large", 48}};
  const auto found = sizes.find(word);
  return found == sizes.end() ? -1 : found->second;
}

double ParsePx(const std::string& text) {
  const ComponentValues v = Trimmed(ParseComponentValues(text));
  if (v.size() == 1 && v[0].kind == Cv::Kind::Token && v[0].token.type == T::Dimension) return v[0].token.number;
  return NAN;
}

// Where inheritance comes from: a pseudo-element inherits from its element, an element from its parent.
struct Parent {
  dom::Element* element;
  std::string pseudo;
};

Parent ParentOf(dom::Element* element, const std::string& pseudo) {
  if (!pseudo.empty()) return {element, ""};
  return {FlatParent(element), ""};
}

ComputeContext Resolver::ContextFor(dom::Element* element, const std::string& property, const std::string& pseudo, const std::string& text) {
  ComputeContext c;
  // What the value needs to be worked out: the sizes and the color only when it says something about them.
  const bool hasDigit = std::any_of(text.begin(), text.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
  std::string lowered = text;
  for (char& ch : lowered) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  const bool needsLineHeight = lowered.find("lh") != std::string::npos;
  const bool needsColor = lowered.find("currentcolor") != std::string::npos;
  const bool needsSizes = hasDigit || property == "font-size";
  const Parent parent = ParentOf(element, pseudo);
  const MediaEnvironment& media = CurrentMediaEnvironment();
  c.viewportWidth = media.width;
  c.viewportHeight = media.height;
  c.colorScheme = media.colorScheme;
  dom::Document* document = element->nodeDocument;
  dom::Element* root = document ? document->DocumentElement() : nullptr;
  if (root) {
    const double rootSize = root == element && pseudo.empty() ? NAN : ParsePx(Compute(root, "font-size"));
    c.rootFontSize = std::isnan(rootSize) ? 16 : rootSize;
  }
  const bool isFontSize = property == "font-size";
  if (!needsSizes) {
    // keywords only
  } else if (isFontSize) {
    const double parentSize = parent.element ? ParsePx(Compute(parent.element, "font-size", parent.pseudo)) : 16;
    c.fontSize = std::isnan(parentSize) ? 16 : parentSize;
    if (root == element && pseudo.empty()) c.rootFontSize = c.fontSize;
  } else {
    const double own = ParsePx(Compute(element, "font-size", pseudo));
    c.fontSize = std::isnan(own) ? 16 : own;
    if (root == element && pseudo.empty()) c.rootFontSize = c.fontSize;
  }
  c.lineHeight = c.fontSize * 1.15;
  c.rootLineHeight = c.rootFontSize * 1.15;
  if (needsLineHeight && property != "line-height" && property != "font-size") {
    const std::string lh = Compute(element, "line-height", pseudo);
    const double px = ParsePx(lh);
    if (!std::isnan(px)) c.lineHeight = px;
    else {
      const ComponentValues v = Trimmed(ParseComponentValues(lh));
      if (v.size() == 1 && v[0].kind == Cv::Kind::Token && v[0].token.type == T::Number) c.lineHeight = v[0].token.number * c.fontSize;
    }
  }
  // currentcolor: the element's color, and for color itself the parent's.
  if (!needsColor) {
    c.currentColor = "rgb(0, 0, 0)";
  } else if (property == "color") {
    c.currentColor = parent.element ? Compute(parent.element, "color", parent.pseudo) : "rgb(0, 0, 0)";
  } else {
    c.currentColor = Compute(element, "color", pseudo);
  }
  if (c.currentColor.empty()) c.currentColor = "rgb(0, 0, 0)";
  return c;
}

// The specified value text of a property for the element: the declared one, or the inherited or initial one.
std::string Resolver::Specified(dom::Element* element, const std::string& property, const PropertyDefinition& definition, bool& valid, const std::string& pseudo) {
  valid = true;
  ElementStyle& style = StyleOf(element, pseudo);
  Build(element, style, pseudo);
  const Parent parent = ParentOf(element, pseudo);
  const auto fromParent = [&]() -> std::string {
    if (parent.element) return Compute(parent.element, property, parent.pseudo);
    return Compute(element, "\x01initial:" + property, pseudo);
  };
  const auto winner = style.cascade.find(property);
  if (winner == style.cascade.end()) {
    if (definition.inherited && parent.element) return fromParent();
    return "initial";
  }
  const DeclarationEntry& entry = *winner->second.entry;
  std::string text = entry.value;
  if (!entry.pendingShorthand.empty()) {
    std::set<std::string> stack;
    std::optional<std::string> substituted = Substitute(element, pseudo, entry.pendingText, 0, stack);
    std::vector<Longhand> longhands;
    if (!substituted || !ExpandDeclaration(entry.pendingShorthand, *substituted, longhands)) {
      valid = false;
      return definition.inherited && parent.element ? fromParent() : "initial";
    }
    text.clear();
    for (const Longhand& l : longhands) {
      if (l.name == property) text = l.value;
    }
    if (text.empty()) {
      valid = false;
      return "initial";
    }
  } else if (ContainsSubstitution(ParseComponentValues(text))) {
    std::set<std::string> stack;
    std::optional<std::string> substituted = Substitute(element, pseudo, text, 0, stack);
    if (!substituted) {
      valid = false;
      return definition.inherited && parent.element ? fromParent() : "initial";
    }
    text = *substituted;
  }
  const ComponentValues parsed = Trimmed(ParseComponentValues(text));
  if (IsCssWideKeyword(parsed)) {
    const std::string keyword = Lower(parsed[0].token.value);
    if (keyword == "initial") return "initial";
    if (keyword == "inherit") return parent.element ? Compute(parent.element, property, parent.pseudo) : "initial";
    // unset, revert, revert-layer
    if (definition.inherited && parent.element) return fromParent();
    return "initial";
  }
  return text;
}

std::string TextOfComputed(const ValueMatch& match) { return SerializeValue(match.normalized); }

std::string Resolver::Compute(dom::Element* element, const std::string& property, const std::string& pseudo) {
  if (!element) return "";
  // The initial value, asked for by the root of the tree.
  bool initialOnly = false;
  std::string name = property;
  if (property.rfind("\x01initial:", 0) == 0) {
    initialOnly = true;
    name = property.substr(9);
  }
  ElementStyle& style = StyleOf(element, pseudo);
  if (!initialOnly) {
    const auto cached = style.computed.find(name);
    if (cached != style.computed.end()) return cached->second;
  }
  if (name.starts_with("--")) {
    std::optional<std::string> value = Custom(element, name, pseudo);
    return value.value_or("");
  }
  const PropertyDefinition* definition = FindProperty(name);
  if (!definition) return "";
  // Not in a document: no computed style.
  dom::Node* root = dom::ShadowIncludingRoot(element);
  if (!root || !root->IsDocument()) return "";

  if (style.resolving.count("\x02" + name)) {
    // Only the sizes that lengths are relative to make a cycle of it.
    if (name == "font-size" || name == "line-height") cycleHit_ = true;
    return "";
  }
  style.resolving.insert("\x02" + name);
  bool valid = true;
  std::string specified = initialOnly ? "initial" : Specified(element, name, *definition, valid, pseudo);
  ComputeContext context = ContextFor(element, name, pseudo, specified == "initial" ? InitialValueText(*definition) : specified);
  std::string result = Finish(element, name, *definition, specified, context, pseudo);
  style.resolving.erase("\x02" + name);
  if (!initialOnly) StyleOf(element, pseudo).computed[name] = result;
  return result;
}

std::string Resolver::Finish(dom::Element* element, const std::string& property, const PropertyDefinition& definition, const std::string& specified, ComputeContext& context, const std::string& pseudo) {
  std::string text = specified;
  if (text == "initial") text = InitialValueText(definition);
  ComponentValues values = Trimmed(ParseComponentValues(text));
  if (values.empty()) return "";
  if (IsCssWideKeyword(values)) return Lower(values[0].token.value);
  const Parent parent = ParentOf(element, pseudo);

  // Properties whose computed values are not got by working through their types.
  if (property == "font-size") {
    if (values.size() == 1 && values[0].IsIdent()) {
      const std::string word = Lower(values[0].token.value);
      if (const double size = FontSizeKeyword(word); size > 0) return FormatNumber(size) + "px";
      if (word == "larger") return FormatNumber(context.fontSize * 1.2) + "px";
      if (word == "smaller") return FormatNumber(context.fontSize / 1.2) + "px";
    }
    if (values.size() == 1 && values[0].kind == Cv::Kind::Token && values[0].token.type == T::Percentage) return FormatNumber(context.fontSize * values[0].token.number / 100) + "px";
  }
  if (property == "font-weight" && values.size() == 1 && values[0].IsIdent()) {
    const std::string word = Lower(values[0].token.value);
    if (word == "normal") return "400";
    if (word == "bold") return "700";
    if (word == "bolder" || word == "lighter") {
      const double inherited = parent.element ? std::atof(Compute(parent.element, "font-weight", parent.pseudo).c_str()) : 400;
      if (word == "bolder") return inherited < 350 ? "400" : inherited < 550 ? "700" : "900";
      return inherited < 550 ? "100" : inherited < 750 ? "400" : "700";
    }
  }
  if (property == "line-height" && values.size() == 1 && values[0].kind == Cv::Kind::Token && values[0].token.type == T::Percentage) {
    return FormatNumber(context.fontSize * values[0].token.number / 100) + "px";
  }
  static const std::set<std::string> borderWidths = {"border-top-width", "border-right-width", "border-bottom-width", "border-left-width", "outline-width", "column-rule-width"};
  if (borderWidths.count(property)) {
    std::string styleProperty = property.substr(0, property.size() - 5) + "style";
    const std::string borderStyle = Compute(element, styleProperty, pseudo);
    if (borderStyle == "none" || borderStyle == "hidden") return "0px";
    if (values.size() == 1 && values[0].IsIdent()) {
      const std::string word = Lower(values[0].token.value);
      if (word == "thin") return "1px";
      if (word == "medium") return "3px";
      if (word == "thick") return "5px";
    }
  }

  ValueMatch match;
  if (!MatchPropertyValue(definition, values, match, &context)) {
    // Not a value of the property after all (a var() that made it one that is not): as if unset.
    if (definition.inherited && parent.element) return Compute(parent.element, property, parent.pseudo);
    ComponentValues initial = Trimmed(ParseComponentValues(InitialValueText(definition)));
    ValueMatch fallback;
    if (!initial.empty() && MatchPropertyValue(definition, initial, fallback, &context)) return TextOfComputed(fallback);
    return "";
  }
  // Opacities are computed to the range of 0 to 1.
  static const std::set<std::string> opacities = {"opacity", "shape-image-threshold", "fill-opacity", "stroke-opacity", "flood-opacity", "stop-opacity"};
  if (opacities.count(property) && match.normalized.size() == 1 && match.normalized[0].kind == Cv::Kind::Token && match.normalized[0].token.type == T::Number) {
    match.normalized[0].token.number = std::max(0.0, std::min(1.0, match.normalized[0].token.number));
  }
  return TextOfComputed(match);
}

}  // namespace

void NoteStyleChange() { ++g_styleVersion; }
void NoteStyleChangeForRegistry() { ++g_styleVersion; }
uint64_t StyleVersion() { return g_styleVersion; }

std::string ComputedValue(Quanta::Context& ctx, dom::Element* element, const std::string& property, const std::string& pseudo) {
  // An adopted sheets array can be changed in place, which nothing tells us of: styles are made again when there is one.
  if (element && element->nodeDocument) {
    dom::Node* root = element;
    while (root->parentNode) root = root->parentNode;
    if (element->nodeDocument->adoptedStyleSheets || (root->IsFragment() && static_cast<dom::DocumentFragment*>(root)->adoptedStyleSheets)) ++g_styleVersion;
  }
  Resolver resolver(ctx);
  return resolver.Compute(element, property, pseudo);
}

const std::vector<std::string>& ComputedPropertyNames() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> list;
    for (size_t i = 0; i < kPropertyDefinitionCount; ++i) {
      const PropertyDefinition& p = kPropertyDefinitions[i];
      if (IsShorthandProperty(p) || std::string(p.name) == "all") continue;
      if (p.name[0] == '-' ) continue;
      list.push_back(p.name);
    }
    return list;
  }();
  return names;
}

}  // namespace solar::css
