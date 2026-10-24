#include <unordered_set>
#include <functional>
#include "solar/css/Style.h"
#include "solar/css/Animation.h"
#include "solar/css/Fonts.h"
#include "solar/css/Logical.h"

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
#include "solar/css/Container.h"
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
uint64_t g_authorVersion = 1;  // changed when the rules do
bool g_anyContainerQueries = false;  // a rule with a container query has been looked at
uint64_t g_cacheVersion = 1;   // changed when what the kept styles were made from is (not for the values animations give: each element has its own number for those)
LayoutHook g_layoutHook = nullptr;
Quanta::Context* g_activeContext = nullptr;  // the realm a style is being worked out in, for what a rule's condition needs of other elements

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
ruby { display: ruby; } rb { display: ruby-base; } rt { display: ruby-text; font-size: 50%; white-space: nowrap; } rtc { display: ruby-text-container; } slot { display: contents; }
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
input, select, button, textarea { margin: 0em; font-size: 13.3333px; letter-spacing: normal; word-spacing: normal; line-height: normal; text-transform: none; text-shadow: none; text-align: start; }
input { padding: 1px 2px; border: 2px inset; }
input[type=hidden i] { display: none; }
input[type=search i] { box-sizing: border-box; }
input[type=checkbox i], input[type=radio i] { margin: 3px 3px 3px 4px; padding: initial; border: initial; box-sizing: border-box; }
input[type=range i] { margin: 2px; padding: initial; border: initial; }
input[type=color i] { width: 50px; height: 27px; border: 1px solid; padding: 1px 2px; box-sizing: border-box; }
input[type=button i], input[type=submit i], input[type=reset i], input[type=file i], button { padding: 1px 6px; border: 2px outset; box-sizing: border-box; text-align: center; }
textarea { padding: 2px; border: 1px solid; overflow: auto; white-space: pre-wrap; word-wrap: break-word; }
select { border: 1px solid; box-sizing: border-box; }
progress, meter { box-sizing: border-box; vertical-align: -0.2em; }
[dir]:dir(ltr), bdi:dir(ltr), input[type=tel i]:dir(ltr) { direction: ltr; }
[dir]:dir(rtl), bdi:dir(rtl) { direction: rtl; }
address, blockquote, center, dialog, div, figure, figcaption, footer, form, header, hr, legend, listing, main, p, plaintext, pre, search, xmp, bdi, output { unicode-bidi: isolate; }
bdo, bdo[dir] { unicode-bidi: isolate-override; }
textarea[dir=auto i], pre[dir=auto i] { unicode-bidi: plaintext; }
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
  uint32_t rule = 0;  // the rule it came from
  uint32_t proximity = UINT32_MAX;  // from the scoping root, when in an @scope
  int context = 0;  // 0 the element's own tree; more is further out (a ::part() rule), less further in (a ::slotted() rule)
  std::vector<Winner> rest;  // what lost to it, for revert and revert-layer
};

constexpr int kUnlayered = INT_MAX / 2;

bool Beats(const Winner& a, const Winner& b) {
  if (!b.entry) return true;
  if (a.origin != b.origin) return a.origin > b.origin;
  // Outer contexts win for normal declarations, inner ones for important.
  if (a.context != b.context) return (a.origin >= 2) ? a.context < b.context : a.context > b.context;
  if (a.layer != b.layer) return a.layer > b.layer;
  if (b.specificity < a.specificity) return true;
  if (a.specificity < b.specificity) return false;
  if (a.proximity != b.proximity) return a.proximity < b.proximity;
  return a.order > b.order;
}

// ---- Rejecting a style rule at a glance ----

uint64_t KeyHash(uint8_t kind, std::string_view text, bool lower) {
  uint64_t h = 1469598103934665603ull ^ kind;
  for (char c : text) {
    if (lower && c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
  }
  return h;
}

// The keys of an element: its id, its classes and its name. A style rule whose selectors all need a key the element has not can not match it.
struct ElementKeys {
  std::vector<uint64_t> keys;
  bool usable = false;
  explicit ElementKeys(const dom::Element* element) {
    // (Class names and ids are matched without regard to case in quirks mode: no key is used there.)
    if (!element->nodeDocument || element->nodeDocument->mode == dom::Document::Mode::Quirks) return;
    usable = true;
    keys.push_back(KeyHash(3, element->localName, true));
    if (const dom::Attr* id = element->FindAttribute("id")) keys.push_back(KeyHash(1, id->value, false));
    if (const dom::Attr* cls = element->FindAttribute("class")) {
      const std::string& v = cls->value;
      size_t i = 0;
      while (i < v.size()) {
        while (i < v.size() && (v[i] == ' ' || v[i] == '\t' || v[i] == '\n' || v[i] == '\r' || v[i] == '\f')) ++i;
        size_t j = i;
        while (j < v.size() && !(v[j] == ' ' || v[j] == '\t' || v[j] == '\n' || v[j] == '\r' || v[j] == '\f')) ++j;
        if (j > i) keys.push_back(KeyHash(2, std::string_view(v).substr(i, j - i), false));
        i = j;
      }
    }
  }
  bool Has(uint64_t key) const {
    for (uint64_t k : keys) if (k == key) return true;
    return false;
  }
};

// A selector that ends in a pseudo-element is only for that one (and for the element it is of), one that does not is only for the elements themselves.
uint64_t PseudoMarker(const std::string* pseudoElement) { return pseudoElement ? KeyHash(9, *pseudoElement, false) | 1 : 0; }

// The keys that the elements around the subject of a selector must have: those of the compounds before a descendant or child combinator.
void AncestorKeysOf(const ComplexSelector& selector, std::vector<uint64_t>& out) {
  for (size_t i = 0; i + 1 < selector.compounds.size() && i < selector.combinators.size(); ++i) {
    if (selector.combinators[i] != Combinator::Descendant && selector.combinators[i] != Combinator::Child) continue;
    uint64_t key = 0;
    bool found = false;
    const CompoundSelector& compound = selector.compounds[i];
    for (const SimpleSelector& simple : compound.simples) {
      if (simple.kind == SimpleSelector::Kind::Host || simple.kind == SimpleSelector::Kind::Unknown) { found = false; break; }
    }
    for (const SimpleSelector& simple : compound.simples) {
      if (simple.kind == SimpleSelector::Kind::Id) { key = KeyHash(1, simple.name, false); found = true; break; }
    }
    if (!found) for (const SimpleSelector& simple : compound.simples) {
      if (simple.kind == SimpleSelector::Kind::Class) { key = KeyHash(2, simple.name, false); found = true; break; }
    }
    if (!found) for (const SimpleSelector& simple : compound.simples) {
      if (simple.kind == SimpleSelector::Kind::Type && !simple.name.empty() && simple.name != "*") { key = KeyHash(3, simple.name, true); found = true; break; }
    }
    if (found) out.push_back(key);
  }
}

// What the elements above an element have, as a Bloom filter: a selector with a key that is not in it can not match.
struct Bloom {
  uint64_t words[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  void Add(uint64_t key) { const uint64_t bit = (key >> 7) & 511; words[bit >> 6] |= 1ull << (bit & 63); }
  bool Has(uint64_t key) const { const uint64_t bit = (key >> 7) & 511; return words[bit >> 6] >> (bit & 63) & 1; }
  void Merge(const Bloom& other) { for (int i = 0; i < 8; ++i) words[i] |= other.words[i]; }
};

const dom::Element* UpperElement(const dom::Element* element) {
  const dom::Node* parent = element->parentNode;
  if (!parent) return nullptr;
  if (parent->IsElement()) return static_cast<const dom::Element*>(parent);
  if (parent->IsFragment() && static_cast<const dom::DocumentFragment*>(parent)->isShadowRoot) return static_cast<const dom::DocumentFragment*>(parent)->host;
  return nullptr;
}

// The keys of the element and the ones above it.
const Bloom& BloomWithSelf(const dom::Element* element) {
  static std::unordered_map<const dom::Element*, Bloom> memo;
  static uint64_t version = 0;
  const uint64_t now = dom::TreeVersion();
  if (version != now) { memo.clear(); version = now; }
  const auto found = memo.find(element);
  if (found != memo.end()) return found->second;
  std::vector<const dom::Element*> chain;
  const dom::Element* up = element;
  while (up && memo.find(up) == memo.end()) { chain.push_back(up); up = UpperElement(up); }
  for (size_t i = chain.size(); i-- > 0;) {
    Bloom bloom;
    const dom::Element* above = UpperElement(chain[i]);
    if (above) bloom = memo[above];
    const ElementKeys own{chain[i]};
    for (uint64_t key : own.keys) bloom.Add(key);
    memo[chain[i]] = bloom;
  }
  return memo[element];
}

void WorkOutQuickKeys(CssRule& rule);

// A conditional group (@media, @supports) is for the elements that any of the style rules in it is for.
void WorkOutGroupKeys(CssRule& rule) {
  rule.quickKeys.clear();
  rule.quickMarkers.clear();
  rule.quickState = 1;
  for (CssRule* inner : rule.rules) {
    if (inner->kind != RuleKind::Style && inner->kind != RuleKind::Media && inner->kind != RuleKind::Supports) {
      // (Comments and the like are not rules; anything else the group holds is looked at every time.)
      rule.quickState = 2; rule.quickKeys.clear(); rule.quickMarkers.clear(); return;
    }
    if (inner->kind == RuleKind::Style && (!inner->rules.empty() || !inner->selectors || !inner->style)) { rule.quickState = 2; rule.quickKeys.clear(); rule.quickMarkers.clear(); return; }
    if (inner->quickState == 0) WorkOutQuickKeys(*inner);
    if (inner->quickState != 1) { rule.quickState = 2; rule.quickKeys.clear(); rule.quickMarkers.clear(); return; }
    rule.quickKeys.insert(rule.quickKeys.end(), inner->quickKeys.begin(), inner->quickKeys.end());
    rule.quickMarkers.insert(rule.quickMarkers.end(), inner->quickMarkers.begin(), inner->quickMarkers.end());
  }
}

// The key of each selector of a list (see above), or false when one of them has none.
bool KeysOf(const SelectorList& list, std::vector<uint64_t>& keys, std::vector<uint64_t>& markers) {
  keys.clear();
  markers.clear();
  for (const ComplexSelector& selector : list) {
    uint64_t key = 0, marker = 0;
    bool found = false, awkward = selector.compounds.empty();
    if (!awkward) {
      const CompoundSelector& last = selector.compounds.back();
      int pseudoElements = 0;
      for (const SimpleSelector& simple : last.simples) {
        if (simple.kind == SimpleSelector::Kind::PseudoElement) { ++pseudoElements; marker = PseudoMarker(&simple.name); }
        if (simple.kind == SimpleSelector::Kind::Host || simple.kind == SimpleSelector::Kind::Unknown) awkward = true;
      }
      if (pseudoElements > 1) awkward = true;
      if (!awkward) {
        // An id says most, then a class; a type is the last resort.
        for (const SimpleSelector& simple : last.simples) {
          if (simple.kind == SimpleSelector::Kind::Id) { key = KeyHash(1, simple.name, false); found = true; break; }
        }
        if (!found) for (const SimpleSelector& simple : last.simples) {
          if (simple.kind == SimpleSelector::Kind::Class) { key = KeyHash(2, simple.name, false); found = true; break; }
        }
        if (!found) for (const SimpleSelector& simple : last.simples) {
          if (simple.kind == SimpleSelector::Kind::Type && !simple.name.empty() && simple.name != "*") { key = KeyHash(3, simple.name, true); found = true; break; }
        }
        if (!found && marker) found = true;  // (a pseudo-element on its own: the pseudo-element is the key)
      }
    }
    if (awkward || !found) { keys.clear(); markers.clear(); return false; }
    keys.push_back(key ^ marker);
    markers.push_back(marker);
  }
  return true;
}

void WorkOutQuickKeys(CssRule& rule) {
  if (rule.kind == RuleKind::Media || rule.kind == RuleKind::Supports) { WorkOutGroupKeys(rule); return; }
  rule.quickState = rule.selectors && KeysOf(*rule.selectors, rule.quickKeys, rule.quickMarkers) ? 1 : 2;
  if (rule.quickState == 2) { rule.quickKeys.clear(); rule.quickMarkers.clear(); }
  rule.quickAncestors.clear();
  if (rule.selectors) {
    for (const ComplexSelector& selector : *rule.selectors) {
      rule.quickAncestors.emplace_back();
      AncestorKeysOf(selector, rule.quickAncestors.back());
    }
  }
}

// The style rules of a long list of rules, sorted by the key they need of an element: an element only looks at the rules it could match.
struct RuleIndex {
  uint64_t version = 0;
  size_t size = 0;
  std::vector<uint32_t> always;  // the rules that can not be told by a key (not style rules, nested rules, selectors with no key)
  std::unordered_map<uint64_t, std::vector<uint32_t>> buckets;
};

const RuleIndex& IndexOf(const std::vector<CssRule*>& rules) {
  static std::unordered_map<const void*, RuleIndex> indexes;
  const uint64_t version = dom::TreeVersion() * 1000003 + g_authorVersion;
  RuleIndex& index = indexes[&rules];
  if (index.version == version && index.size == rules.size()) return index;
  index = RuleIndex();
  index.version = version;
  index.size = rules.size();
  for (uint32_t i = 0; i < rules.size(); ++i) {
    CssRule* rule = rules[i];
    const bool group = rule->kind == RuleKind::Media || rule->kind == RuleKind::Supports;
    if (!group && (rule->kind != RuleKind::Style || !rule->selectors || !rule->style || !rule->rules.empty())) { index.always.push_back(i); continue; }
    if (rule->quickState == 0) WorkOutQuickKeys(*rule);
    if (rule->quickState != 1) { index.always.push_back(i); continue; }
    std::vector<uint64_t> done;
    for (uint64_t key : rule->quickKeys) {
      if (std::find(done.begin(), done.end(), key) != done.end()) continue;
      done.push_back(key);
      index.buckets[key].push_back(i);
    }
  }
  if (indexes.size() > 4096) { indexes.clear(); return IndexOf(rules); }
  return index;
}

// Whether an @supports condition holds: what is supported does not change, so the answer for a text is kept.
bool SupportsText(const std::string& text) {
  static std::unordered_map<std::string, bool> answers;
  const auto found = answers.find(text);
  if (found != answers.end()) return found->second;
  const bool result = SupportsCondition(ParseComponentValues(text));
  answers.emplace(text, result);
  return result;
}

struct Gatherer {
  dom::Element* element;
  MatchContext matchContext;
  std::map<std::string, Winner>& winners;
  uint32_t order = 0;
  uint32_t ruleSerial = 0;
  uint32_t proximity = UINT32_MAX;
  // The cascade layers of the tree, by path ("A.B"; an anonymous layer has a name no one can write). They are ordered by where they
  // are first named, a layer's sublayers below it: the rank of a path is its place in that order.
  std::map<std::string, std::vector<std::string>> layerChildren;
  std::map<std::string, int> layerRank;
  std::string layerPath;
  bool collecting = false;  // only finding the layers
  bool startingMode = false;  // the starting style of the element is being made: @starting-style rules count
  int anonymousLayers = 0;  // the anonymous layers are numbered as they are met, the same in both passes
  MediaEnvironment media = EnvironmentFor(element->nodeDocument);
  bool hostScoped = false;  // the rules are of a shadow tree and the element is its host: author rules of the outer tree win
  int layerBase = 0;        // added to the rank of a layer: a shadow host's rules are below the rules around them
  // Rules that reach into another tree: ::slotted() rules of the slot's tree, ::part() rules of the trees around a shadow host.
  enum class Mode { Own, Slotted, Part } mode = Mode::Own;
  const dom::Element* slot = nullptr;       // Slotted: the slot the element is assigned to
  const dom::Element* partHost = nullptr;   // Part: the host whose shadow tree the element is in
  std::vector<std::string> partNames;       // Part: the names the element has in that tree
  int contextRank = 0;
  ElementKeys keys{element};
  const Bloom* aboveBloom = nullptr;
  bool aboveKnown = false;
  // What the elements above the element have (none if it has none above).
  const Bloom* AncestorBloom() {
    if (!aboveKnown) {
      aboveKnown = true;
      const dom::Element* upper = UpperElement(element);
      aboveBloom = upper ? &BloomWithSelf(upper) : nullptr;
    }
    return aboveBloom;
  }

  void RegisterLayer(const std::string& path) {
    if (path.empty() || layerRank.count(path)) return;
    layerRank[path] = -1;
    const size_t dot = path.rfind('.');
    const std::string parent = dot == std::string::npos ? "" : path.substr(0, dot);
    RegisterLayer(parent);
    layerChildren[parent].push_back(path);
  }
  void RankLayers() {
    int counter = 0;
    anonymousLayers = 0;
    std::function<void(const std::string&)> visit = [&](const std::string& path) {
      for (const std::string& child : layerChildren[path]) visit(child);
      if (!path.empty()) layerRank[path] = counter++;
    };
    visit("");
  }
  std::string Child(const std::string& name) const { return layerPath.empty() ? name : layerPath + "." + name; }
  int RankOf(const std::string& path) {
    const auto found = layerRank.find(path);
    return layerBase + (found == layerRank.end() ? 0 : found->second);
  }

  // Whether the selector, as the rules of the current mode read it, is for the element.
  bool Matches(const ComplexSelector& selector, Specificity& extra);

  void Offer(const DeclarationEntry& entry, bool author, bool inlineStyle, int layer, Specificity spec) {
    Winner candidate;
    candidate.entry = &entry;
    candidate.author = author;
    candidate.specificity = spec;
    candidate.order = ++order;
    candidate.rule = ruleSerial;
    candidate.proximity = proximity;
    candidate.context = contextRank;
    if (inlineStyle) candidate.specificity = Specificity{1000000, 0, 0};
    // Normal: user agent, author; important: author, user agent. Layers reverse for important declarations.
    candidate.origin = entry.important ? (author ? 2 : 3) : (author ? 1 : 0);
    candidate.layer = entry.important ? (layer == kUnlayered ? -1 : 1000000 - layer) : layer;
    if (inlineStyle) candidate.layer = INT_MAX;  // element-attached styles come before the layers are looked at
    Winner& slot = winners[entry.name];
    if (Beats(candidate, slot)) {
      candidate.rest = std::move(slot.rest);
      if (slot.entry) {
        Winner lost = slot;
        lost.rest.clear();
        candidate.rest.push_back(std::move(lost));
      }
      slot = std::move(candidate);
    } else {
      slot.rest.push_back(std::move(candidate));
    }
  }

  // `outer` is the specificity of the style rule these rules are in, when it matched the element: its nested declarations
  // apply as it does.
  void Rules(const std::vector<CssRule*>& all, int layer, const Specificity* outer = nullptr, bool picked = false) {
    std::vector<CssRule*> candidates;
    const bool indexed = !picked && !collecting && mode == Mode::Own && keys.usable && all.size() >= 48;
    if (indexed) {
      const RuleIndex& index = IndexOf(all);
      std::vector<uint32_t> positions = index.always;
      const uint64_t marker = PseudoMarker(matchContext.pseudoElement);
      const auto take = [&](uint64_t key) {
        const auto found = index.buckets.find(key);
        if (found != index.buckets.end()) positions.insert(positions.end(), found->second.begin(), found->second.end());
      };
      for (uint64_t key : keys.keys) take(key ^ marker);
      if (marker) take(marker);
      std::sort(positions.begin(), positions.end());
      positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
      candidates.reserve(positions.size());
      for (uint32_t i : positions) candidates.push_back(all[i]);
    }
    const std::vector<CssRule*>& rules = indexed ? candidates : all;
    for (CssRule* rule : rules) {
      if (collecting && rule->kind == RuleKind::Scope) {
        Rules(rule->rules, layer, outer);
        continue;
      }
      if (collecting && (rule->kind == RuleKind::Style || rule->kind == RuleKind::NestedDeclarations)) continue;
      switch (rule->kind) {
        case RuleKind::NestedDeclarations:
          if (outer && rule->style) {
            ++ruleSerial;
            for (const DeclarationEntry& entry : rule->style->items) Offer(entry, true, false, layer, *outer);
          }
          break;
        case RuleKind::Style: {
          if (!rule->selectors || !rule->style) break;
          if (!collecting && mode == Mode::Own && keys.usable) {
            if (rule->quickState == 0) WorkOutQuickKeys(*rule);
            if (rule->quickState == 1) {
              const uint64_t marker = PseudoMarker(matchContext.pseudoElement);
              bool possible = false;
              for (size_t j = 0; j < rule->quickKeys.size() && !possible; ++j) {
                if (rule->quickMarkers[j] != marker) continue;
                const uint64_t base = rule->quickKeys[j] ^ marker;
                possible = base == 0 || keys.Has(base);
              }
              if (!possible) {
                if (!rule->rules.empty()) Rules(rule->rules, layer, nullptr);
                break;
              }
            }
          }
          Specificity best;
          bool matched = false;
          const bool screening = !collecting && mode == Mode::Own && keys.usable && rule->quickAncestors.size() == rule->selectors->size();
          for (size_t selectorIndex = 0; selectorIndex < rule->selectors->size(); ++selectorIndex) {
            const ComplexSelector& selector = (*rule->selectors)[selectorIndex];
            if (screening && !rule->quickAncestors[selectorIndex].empty()) {
              const Bloom* above = AncestorBloom();
              bool possible = above != nullptr;
              if (possible) for (uint64_t key : rule->quickAncestors[selectorIndex]) if (!above->Has(key)) { possible = false; break; }
              if (!possible) continue;
            }
            Specificity extra;
            if (Matches(selector, extra)) {
              Specificity s = SpecificityOf(selector);
              s.ids += extra.ids;
              s.classes += extra.classes;
              s.types += extra.types;
              if (!matched || best < s) best = s;
              matched = true;
            }
          }
          if (matched) {
            ++ruleSerial;
            for (const DeclarationEntry& entry : rule->style->items) Offer(entry, true, false, layer, best);
          }
          // Rules nested in a style rule apply on their own selectors; its trailing declarations as it does.
          Rules(rule->rules, layer, matched ? &best : nullptr);
          break;
        }
        case RuleKind::Scope: {
          // The scoping roots of the element: the elements it is inside of (or is) that <scope-start> matches, or the parent of
          // the sheet's owner; those with no <scope-end> element between them and the element.
          const auto parentElement = [](const dom::Element* e) -> const dom::Element* { return e->parentNode ? dom::AsElement(e->parentNode) : nullptr; };
          const dom::Element* implicitRoot = nullptr;
          if (!rule->scopeStart) {
            const CssStyleSheet* sheet = rule->parentSheet;
            while (sheet && sheet->ownerRule && sheet->ownerRule->parentSheet) sheet = sheet->ownerRule->parentSheet;
            if (sheet && sheet->ownerNode && sheet->ownerNode->parentNode) {
              const dom::Node* parent = sheet->ownerNode->parentNode;
              if (const dom::Element* e = dom::AsElement(parent)) implicitRoot = e;
              else if (parent->IsFragment() && static_cast<const dom::DocumentFragment*>(parent)->isShadowRoot) implicitRoot = static_cast<const dom::DocumentFragment*>(parent)->host;
            }
            if (!implicitRoot) break;
          }
          bool insideScope = false;
          for (const CssRule* up = rule->parentRule; up && !insideScope; up = up->parentRule) insideScope = up->kind == RuleKind::Scope;
          std::vector<std::pair<const dom::Element*, uint32_t>> roots;
          uint32_t distance = 0;
          for (const dom::Element* up = element; up; up = parentElement(up), ++distance) {
            bool isRoot;
            if (rule->scopeStart) {
              MatchContext c = matchContext;
              isRoot = MatchesAny(*rule->scopeStart, up, c);
            } else {
              isRoot = up == implicitRoot;
            }
            if (!isRoot) continue;
            if (insideScope && matchContext.scope) {
              // The root of a scope inside another is in the outer one.
              bool within = false;
              for (const dom::Element* x = up; x && !within; x = parentElement(x)) within = x == matchContext.scope;
              if (!within) continue;
            }
            bool limited = false;
            if (rule->scopeEnd) {
              MatchContext c = matchContext;
              c.scope = up;
              for (const dom::Element* x = element; x && !limited; x = x == up ? nullptr : parentElement(x)) limited = MatchesAny(*rule->scopeEnd, x, c);
            }
            if (!limited) roots.push_back({up, distance});
          }
          const dom::Element* savedScope = matchContext.scope;
          const uint32_t savedProximity = proximity;
          const Specificity none;
          for (const auto& [root, d] : roots) {
            matchContext.scope = root;
            proximity = d;
            Rules(rule->rules, layer, root == element ? &none : nullptr);
          }
          matchContext.scope = savedScope;
          proximity = savedProximity;
          break;
        }
        case RuleKind::StartingStyle:
          // (The layers it names are the document's, whether or not it is the starting style that is being made.)
          if (startingMode || collecting) Rules(rule->rules, layer, outer);
          break;
        case RuleKind::Media:
          if (!rule->media || MediaListMatches(rule->media->queries, media)) Rules(rule->rules, layer, outer);
          break;
        case RuleKind::Supports:
          if (SupportsText(rule->supportsText)) Rules(rule->rules, layer, outer);
          break;
        case RuleKind::Container:
          if (element->nodeDocument) element->nodeDocument->containerQueriesSeen = true;
          g_anyContainerQueries = true;
          if (g_activeContext && ContainerRuleMatches(*g_activeContext, const_cast<dom::Element*>(element), matchContext.pseudoElement != nullptr, rule->prelude)) Rules(rule->rules, layer, outer);
          break;
        case RuleKind::Import: {
          CssStyleSheet* imported = rule->importedSheet;
          if (imported && imported->disabled) break;
          if (rule->media && !MediaListMatches(rule->media->queries, media)) break;
          if (!rule->supportsText.empty() && !SupportsText(rule->supportsText)) break;
          const std::string savedPath = layerPath;
          int id = layer;
          if (!rule->layerName.empty()) {
            layerPath = Child(rule->layerName == "\x01" ? "\x01" + std::to_string(++anonymousLayers) : rule->layerName);
            if (collecting) RegisterLayer(layerPath);
            id = RankOf(layerPath);
          }
          if (imported) Rules(imported->rules, id);
          layerPath = savedPath;
          break;
        }
        case RuleKind::LayerBlock: {
          const std::string savedPath = layerPath;
          layerPath = Child(rule->name.empty() ? "\x01" + std::to_string(++anonymousLayers) : rule->name);
          if (collecting) RegisterLayer(layerPath);
          Rules(rule->rules, RankOf(layerPath), outer);
          layerPath = savedPath;
          break;
        }
        case RuleKind::LayerStatement: {
          size_t start = 0;
          const std::string& names = rule->layerName;
          while (start < names.size()) {
            size_t comma = names.find(", ", start);
            if (collecting) RegisterLayer(Child(names.substr(start, comma == std::string::npos ? std::string::npos : comma - start)));
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

std::vector<CssStyleSheet*> SheetsOfTreeUncached(dom::Node* root, Quanta::Context* ctx);

// The style sheets of a tree, kept until something changes: every element asks for them.
std::vector<CssStyleSheet*> SheetsOfTree(dom::Node* root, Quanta::Context* ctx = nullptr) {
  static std::unordered_map<dom::Node*, std::pair<uint64_t, std::vector<CssStyleSheet*>>> cache;
  const uint64_t version = dom::TreeVersion() * 1000003 + g_styleVersion;
  auto found = cache.find(root);
  if (found != cache.end() && found->second.first == version) return found->second.second;
  std::vector<CssStyleSheet*> sheets = SheetsOfTreeUncached(root, ctx);
  if (cache.size() > 64) cache.clear();
  cache[root] = {version, sheets};
  return sheets;
}

std::vector<CssStyleSheet*> SheetsOfTreeUncached(dom::Node* root, Quanta::Context* ctx) {
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
  if (element->assignedSlot) return element->assignedSlot;
  dom::Node* parent = element->parentNode;
  if (!parent) return nullptr;
  if (dom::Element* e = dom::AsElement(parent)) return e;
  if (parent->IsFragment() && static_cast<dom::DocumentFragment*>(parent)->isShadowRoot) return static_cast<dom::DocumentFragment*>(parent)->host;
  return nullptr;
}


namespace {

// The selector split at a ::part() or ::slotted() that ends it: the part before, as a selector, and the pseudo-element's argument.
bool SplitPseudoTail(const ComplexSelector& selector, const char* which, ComplexSelector& head, std::string& argument) {
  if (selector.compounds.empty()) return false;
  const CompoundSelector& last = selector.compounds.back();
  if (last.simples.empty()) return false;
  const SimpleSelector& tail = last.simples.back();
  if (tail.kind != SimpleSelector::Kind::PseudoElement || tail.name != which) return false;
  head = selector;
  head.compounds.back().simples.pop_back();
  if (head.compounds.back().simples.empty()) {
    SimpleSelector universal;
    universal.kind = SimpleSelector::Kind::Universal;
    head.compounds.back().simples.push_back(universal);
  }
  argument = tail.value;
  return true;
}

const SelectorList* ParsedArgument(const std::string& text) {
  static std::map<std::string, std::optional<SelectorList>> cache;
  auto found = cache.find(text);
  if (found == cache.end()) found = cache.emplace(text, ParseSelectorList(text)).first;
  return found->second ? &*found->second : nullptr;
}

std::vector<std::string> SplitWords(const std::string& text) {
  std::vector<std::string> out;
  std::string word;
  for (char c : text) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
      if (!word.empty()) out.push_back(word);
      word.clear();
    } else {
      word += c;
    }
  }
  if (!word.empty()) out.push_back(word);
  return out;
}

}  // namespace

bool Gatherer::Matches(const ComplexSelector& selector, Specificity& extra) {
  if (mode == Mode::Own) return MatchesComplex(selector, element, matchContext);
  ComplexSelector head;
  std::string argument;
  if (mode == Mode::Slotted) {
    if (!SplitPseudoTail(selector, "slotted", head, argument)) return false;
    const SelectorList* list = ParsedArgument(argument);
    if (!list) return false;
    // The slot is what the rest of the selector is about; the argument is about the slotted element.
    if (!MatchesComplex(head, slot, matchContext)) return false;
    MatchContext inElement;
    for (const ComplexSelector& s : *list) {
      if (s.compounds.size() == 1 && MatchesComplex(s, element, inElement)) {
        extra = SpecificityOf(s);
        return true;
      }
    }
    return false;
  }
  if (!SplitPseudoTail(selector, "part", head, argument)) return false;
  for (const std::string& name : SplitWords(argument)) {
    if (std::find(partNames.begin(), partNames.end(), name) == partNames.end()) return false;
  }
  return MatchesComplex(head, partHost, matchContext);
}

// ---- The resolver ----

// What was worked out for an element; elements that are styled the same way and inherit from the same share it.
struct StringKeyHash {
  using is_transparent = void;
  size_t operator()(std::string_view text) const { return std::hash<std::string_view>()(text); }
};

struct Memo {
  std::unordered_map<std::string, std::string, StringKeyHash, std::equal_to<>> computed;
  std::unordered_map<std::string, std::optional<std::string>> custom;
};

struct ElementStyle {
  uint64_t version = 0;
  std::map<std::string, Winner> cascade;
  std::shared_ptr<Memo> memo = std::make_shared<Memo>();
  std::set<std::string> resolving;
  std::vector<std::vector<DeclarationEntry>> storage;  // the style attribute's declarations
  bool built = false;
  bool usedHas = false;     // a :has() was looked at for it: what is under it, and beside it, is what its style was made from, too
  bool hasLogical = false;  // a logical property is declared for the element
};

// The styles of an element: the plain one, and those of its pseudo-elements and of the variants (animations suppressed, the starting style).
struct StyleEntry {
  ElementStyle plain;
  std::map<std::string, ElementStyle> others;
  // What the styles were made from: they are kept while it is the same (see Validate).
  uint64_t serial = 0;
  uint64_t epoch = 0;    // a number for each time the styles are made anew: the elements below compare with it
  uint64_t checked = 0;  // the version of the tree it was last seen to be good at
  bool shadowed = false;  // it was made when the element was in a shadow tree, a host or assigned to a slot
  const dom::Node* parent = nullptr;
  uint64_t parentEpoch = 0;
  uint32_t attr = 0, anim = 0, children = 0, parentChildren = 0, parentChildAttr = 0;
  uint64_t treeVersion = 0;  // of the tree when the styles were begun
};

std::unordered_map<dom::Element*, StyleEntry>& Cache() {
  static std::unordered_map<dom::Element*, StyleEntry> cache;
  return cache;
}

void ClearShared();
void DropSharedBuckets();

// A number for the sheets of a document that count and the environment its media queries are of: the styles kept are good while it is the same.
uint64_t DocumentSignature(dom::Document* document) {
  uint64_t h = 1469598103934665603ull;
  const auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
  for (CssStyleSheet* sheet : SheetsOfTree(document, document->context)) {
    mix(reinterpret_cast<uintptr_t>(sheet));
    mix(sheet->disabled ? 1 : 2);
  }
  const MediaEnvironment e = EnvironmentFor(document);
  mix(static_cast<uint64_t>(e.width * 8));
  mix(static_cast<uint64_t>(e.height * 8));
  mix(static_cast<uint64_t>(e.resolution * 100));
  mix(std::hash<std::string>()(e.colorScheme));
  mix(document->hasPreferredSheetSet ? std::hash<std::string>()(document->preferredSheetSet) : 7);
  const size_t hash = document->url.find('#');  // (:target)
  mix(hash == std::string::npos ? 3 : std::hash<std::string>()(document->url.substr(hash)));
  return h | 1;
}

uint64_t g_styleEpoch = 0;

// The styles made of an element are good while what they were made from is the same: the sheets, the element and its attributes, the styles of the elements
// above it, and (when a rule looks at them) its children and its siblings. A document with rules that look at more than that has nothing kept across changes.
void Validate(dom::Element* element, StyleEntry& entry, uint64_t treeVersion) {
  if (entry.checked == treeVersion) return;
  entry.checked = treeVersion;
  const StyleReuse reuse = element->nodeDocument ? StyleReuseFor(element->nodeDocument) : StyleReuse();
  const dom::Node* parentNode = element->parentNode;
  dom::Element* parent = parentNode && parentNode->IsElement() ? const_cast<dom::Element*>(static_cast<const dom::Element*>(parentNode)) : nullptr;
  uint64_t parentEpoch = 0;
  if (parent) {
    StyleEntry& up = Cache()[parent];
    Validate(parent, up, treeVersion);
    parentEpoch = up.epoch;
  }
  // An element of a shadow tree, a shadow host and one assigned to a slot are styled by more than what is around them in the tree.
  bool shadowed = false;
  if (reuse.shadows) {
    shadowed = element->shadowRoot || element->assignedSlot;
    if (!shadowed) {
      const dom::Node* top = element;
      while (top->parentNode) top = top->parentNode;
      shadowed = !top->IsDocument();
    }
  }
  bool good = reuse.allowed && !shadowed && !entry.shadowed && !(element->nodeDocument && element->nodeDocument->containerQueriesSeen) && entry.serial == element->serial && entry.parent == parentNode && entry.parentEpoch == parentEpoch && entry.attr == element->attrVersion && entry.anim == element->animVersion;
  if (good && reuse.structural) good = entry.children == element->childrenVersion && (!parent || entry.parentChildren == parent->childrenVersion);
  if (good && reuse.sibling) good = !parent || (entry.parentChildAttr == parent->childAttrVersion && entry.parentChildren == parent->childrenVersion);
  if (good && reuse.has) {
    bool used = entry.plain.usedHas;
    for (const auto& other : entry.others) used = used || other.second.usedHas;
    // (What it depends on can be anywhere: under it, beside it, beside the elements above it. It is made anew at every change of the tree.)
    if (used) good = entry.treeVersion == dom::TreeVersion();
  }
  if (good) return;
  entry.plain = ElementStyle();
  entry.others.clear();
  entry.serial = element->serial;
  entry.epoch = ++g_styleEpoch;
  entry.parent = parentNode;
  entry.parentEpoch = parentEpoch;
  entry.attr = element->attrVersion;
  entry.anim = element->animVersion;
  entry.children = element->childrenVersion;
  entry.parentChildren = parent ? parent->childrenVersion : 0;
  entry.parentChildAttr = parent ? parent->childAttrVersion : 0;
  entry.shadowed = shadowed;
  entry.treeVersion = dom::TreeVersion();
}

ElementStyle& StyleOf(dom::Element* element, const std::string& pseudo = "") {
  static uint64_t cachedStyleVersion = 0;
  const uint64_t version = dom::TreeVersion() * 1000003 + g_styleVersion;
  const uint64_t treeVersion = version;  // (what animations give changes the version too, and is looked at element by element)
  if (cachedStyleVersion != g_cacheVersion || Cache().size() > 60000) {
    ClearShared();
    Cache().clear();
    cachedStyleVersion = g_cacheVersion;
  }
  // The elements that share what was worked out are found anew when anything changes (what is kept of an element is still good for it).
  static uint64_t sharedVersion = 0;
  if (sharedVersion != version) {
    DropSharedBuckets();
    sharedVersion = version;
  }
  // What the sheets of the document are (which of them count, and the size of the window the media queries are of) is what every style depends on.
  if (element->nodeDocument) {
    struct DocumentState {
      uint64_t checkedAt = 0;
      uint64_t signature = 0;
    };
    static std::unordered_map<const dom::Document*, DocumentState> states;
    DocumentState& state = states[element->nodeDocument];
    if (state.checkedAt != version) {
      state.checkedAt = version;
      const uint64_t signature = DocumentSignature(element->nodeDocument);
      if (state.signature != signature) {
        if (state.signature != 0) {
          ClearShared();
          Cache().clear();
        }
        state.signature = signature;
      }
    }
  }
  StyleEntry& entry = Cache()[element];
  Validate(element, entry, treeVersion);
  const bool suppressed = AnimatedValuesSuppressed(), starting = StartingStyleFor(element, pseudo);
  ElementStyle& style = pseudo.empty() && !suppressed && !starting ? entry.plain : entry.others[(suppressed ? pseudo + "\x03" : pseudo) + (starting ? "\x04" : "")];
  style.version = version;
  return style;
}

void Build(dom::Element* element, ElementStyle& style, const std::string& pseudo);

// The room the scrollbars and the gutter of the viewport take: the overflow is that of the root element, or of the body when the root's is visible.
double ViewportScrollbarsUncached(dom::Document* document, const std::function<std::string(dom::Element*, const char*)>& get, double& horizontal, ViewportAuto* autos);

// (Every length with a digit in it asks, and it takes the overflow of two elements: what it comes to is kept until something changes.)
double ViewportScrollbarsWith(dom::Document* document, const std::function<std::string(dom::Element*, const char*)>& get, double& horizontal, ViewportAuto* autos = nullptr) {
  struct Kept {
    dom::Document* document = nullptr;
    uint64_t version = 0;
    double vertical = 0, horizontal = 0;
    ViewportAuto autos;
  };
  static Kept kept;
  const uint64_t version = dom::TreeVersion() * 1000003 + g_styleVersion;
  if (kept.document == document && kept.version == version && document) {
    horizontal = kept.horizontal;
    if (autos) *autos = kept.autos;
    return kept.vertical;
  }
  ViewportAuto computedAutos;
  const double vertical = ViewportScrollbarsUncached(document, get, horizontal, &computedAutos);
  kept = {document, version, vertical, horizontal, computedAutos};
  if (autos) *autos = computedAutos;
  return vertical;
}

double ViewportScrollbarsUncached(dom::Document* document, const std::function<std::string(dom::Element*, const char*)>& get, double& horizontal, ViewportAuto* autos) {
  horizontal = 0;
  if (autos) *autos = ViewportAuto();
  dom::Element* root = document ? document->DocumentElement() : nullptr;
  if (!root) return 0;
  const auto visible = [&](dom::Element* e) {
    const std::string x = get(e, "overflow-x"), y = get(e, "overflow-y");
    return (x == "visible" || x == "clip") && (y == "visible" || y == "clip");
  };
  dom::Element* source = root;
  if (visible(root)) {
    source = nullptr;
    for (dom::Node* child = root->firstChild; child; child = child->nextSibling) {
      if (child->IsElement() && static_cast<dom::Element*>(child)->IsHtml("body")) source = static_cast<dom::Element*>(child);
    }
    if (!source || visible(source)) source = root;  // (the overflow of the viewport is auto when it is visible)
  }
  std::string ox = get(source, "overflow-x"), oy = get(source, "overflow-y");
  if (ox == "clip") ox = "hidden";
  if (oy == "clip") oy = "hidden";
  if (ox == "visible") ox = "auto";
  if (oy == "visible") oy = "auto";
  const std::string width = get(root, "scrollbar-width"), gutter = get(root, "scrollbar-gutter");
  const double thickness = width == "none" ? 0.0 : width == "thin" ? 8.0 : 15.0;
  const bool stable = gutter.find("stable") != std::string::npos, both = gutter.find("both-edges") != std::string::npos;
  const std::string mode = get(source, "writing-mode");
  const bool vertical = mode.starts_with("vertical") || mode.starts_with("sideways");
  if (autos) {
    autos->x = ox == "auto";
    autos->y = oy == "auto";
    autos->thickness = thickness;
  }
  double verticalBar = 0;
  if (oy == "scroll") verticalBar = thickness;
  if (ox == "scroll") horizontal = thickness;
  if (stable) {
    if (!vertical) verticalBar = thickness * (both ? 2 : 1);
    else horizontal = thickness * (both ? 2 : 1);
  }
  return verticalBar;
}

// ---- Sharing what was worked out ----

// Elements that the same declarations apply to, with the same tag and a parent whose computed values are shared, have the same
// computed values: they are worked out once. (What looks at more than the declarations and the parent is left out: attr(), the
// sizes of containers, the siblings, the animations.)
struct SharedKey {
  std::shared_ptr<const Memo> parent;  // (kept: its address is not taken for another memo's)
  std::string localName;
  std::string namespaceUri;
  const ElementStyle* style = nullptr;
  std::string pseudo;
};

std::unordered_map<uint64_t, std::vector<SharedKey>>& SharedStyles() {
  static std::unordered_map<uint64_t, std::vector<SharedKey>> shared;
  return shared;
}

std::unordered_map<const DeclarationEntry*, bool>& EntrySafety() {
  static std::unordered_map<const DeclarationEntry*, bool> safety;
  return safety;
}

void DropSharedBuckets() { SharedStyles().clear(); }

void ClearShared() {
  SharedStyles().clear();
  EntrySafety().clear();
}

bool SafeToShare(const DeclarationEntry* entry) {
  const auto found = EntrySafety().find(entry);
  if (found != EntrySafety().end()) return found->second;
  bool safe = true;
  for (const std::string* text : {&entry->value, &entry->pendingText}) {
    for (const char* word : {"attr(", "cq", "sibling-", "anchor", "random(", "element(", "if(", "inherit(", "container"}) {
      if (text->find(word) != std::string::npos) safe = false;
    }
  }
  EntrySafety()[entry] = safe;
  return safe;
}

void Share(dom::Element* element, ElementStyle& style, const std::string& pseudo = "") {
  if (!style.memo->computed.empty() || !style.memo->custom.empty() || !style.storage.empty()) return;
  if (element->animVersion != 0 || !element->parentNode || !element->parentNode->IsElement()) return;
  // (An element of a shadow tree, a shadow host, and one that is assigned to a slot are styled by more than the declarations around them.)
  if (dom::HasShadowTrees()) {
    if (element->shadowRoot || element->assignedSlot) return;
    dom::Node* top = element;
    while (top->parentNode) top = top->parentNode;
    if (!top->IsDocument()) return;
  }
  if (element->nodeDocument && element->nodeDocument->containerQueriesSeen) return;  // (what a container query says is of the element's container)
  if (element->FindAttribute("", "style")) return;
  dom::Element* parent = static_cast<dom::Element*>(element->parentNode);
  if (parent->animVersion != 0) return;
  // A pseudo-element inherits from the element it is of; it is shared among those of the same style and the same name.
  ElementStyle& parentStyle = pseudo.empty() ? StyleOf(parent) : StyleOf(element);
  if (!pseudo.empty() && !parentStyle.built) return;
  uint64_t hash = std::hash<const void*>()(parentStyle.memo.get());
  hash = hash * 1000003 + std::hash<std::string>()(pseudo);
  hash = hash * 1000003 + std::hash<std::string>()(element->localName);
  hash = hash * 1000003 + std::hash<std::string>()(element->namespaceUri);
  for (const auto& [name, winner] : style.cascade) {
    if (!SafeToShare(winner.entry)) return;
    hash = hash * 1000003 + std::hash<std::string>()(name);
    hash = hash * 1000003 + std::hash<const void*>()(winner.entry);
    hash = hash * 1000003 + static_cast<uint64_t>(winner.origin * 31 + winner.layer);
    // (The declarations that lost are part of the style too: the same ones lost.)
    for (const Winner& lost : winner.rest) {
      if (!SafeToShare(lost.entry)) return;
      hash = hash * 1000003 + std::hash<const void*>()(lost.entry);
      hash = hash * 1000003 + static_cast<uint64_t>(lost.origin * 31 + lost.layer);
    }
  }
  std::vector<SharedKey>& bucket = SharedStyles()[hash];
  for (const SharedKey& other : bucket) {
    if (other.parent.get() != parentStyle.memo.get() || other.localName != element->localName || other.namespaceUri != element->namespaceUri || other.pseudo != pseudo) continue;
    const auto& a = other.style->cascade;
    const auto& b = style.cascade;
    if (a.size() != b.size()) continue;
    bool same = true;
    for (auto i = a.begin(), j = b.begin(); i != a.end(); ++i, ++j) {
      if (i->first != j->first || i->second.entry != j->second.entry || i->second.origin != j->second.origin || i->second.layer != j->second.layer || i->second.rest.size() != j->second.rest.size()) {
        same = false;
        break;
      }
      for (size_t k = 0; k < i->second.rest.size() && same; ++k) {
        const Winner& x = i->second.rest[k];
        const Winner& y = j->second.rest[k];
        same = x.entry == y.entry && x.origin == y.origin && x.layer == y.layer;
      }
      if (!same) break;
    }
    if (!same) continue;
    style.memo = other.style->memo;
    return;
  }
  bucket.push_back({parentStyle.memo, element->localName, element->namespaceUri, &style, pseudo});
}

// The cascade of an element is made, and what is shared with others of the same style is found.
void Prepare(dom::Element* element) {
  ElementStyle& style = StyleOf(element);
  if (style.built) return;
  if (element->parentNode && element->parentNode->IsElement()) Prepare(static_cast<dom::Element*>(element->parentNode));
  Build(element, style, "");
  Share(element, style);
}

// Whether a property that is not inherited applies to ::first-letter or ::first-line (https://www.w3.org/TR/css-pseudo-4/#first-line-styling).
bool AppliesToFirstPseudo(const std::string& property, bool letter) {
  const auto starts = [&](const char* prefix) { return property.rfind(prefix, 0) == 0; };
  if (property.rfind("--", 0) == 0 || property == "display" || property == "content" || property == "opacity" || property == "vertical-align" || property == "unicode-bidi" || property == "all") return true;
  if (starts("background") || starts("text-decoration") || starts("text-underline") || starts("-webkit-text") || starts("-webkit-background") || starts("mix-blend") || starts("text-emphasis")) return true;
  if (starts("font") || starts("line-height") || starts("-webkit-font") || property == "color" || property == "letter-spacing" || property == "word-spacing" || property == "text-transform" ||
      property == "text-shadow" || property == "text-justify") return true;
  if (letter) {
    if (starts("margin") || starts("padding") || starts("border") || starts("-webkit-border") || starts("-webkit-margin") || starts("-webkit-padding")) return true;
    if (property == "float" || property == "box-shadow" || property == "-webkit-box-shadow" || starts("initial-letter") || property == "clear" || property == "text-box-trim" || starts("text-box")) return true;
  }
  return false;
}

// What the initial value of a property computes to, for the properties it is the same for whatever the element (a plain keyword or a number of px).
const std::string* FastInitial(const PropertyDefinition& definition);
void NoteInitial(const PropertyDefinition& definition, const std::string& computed);

class Resolver {
 public:
  explicit Resolver(Quanta::Context& ctx) : ctx_(ctx) {}

  std::string Compute(dom::Element* element, const std::string& property, const std::string& pseudo = "");
  std::optional<std::string> Custom(dom::Element* element, const std::string& name, const std::string& pseudo = "");

 private:
  bool plainInherit_ = false;  // Specified answered with the parent's value
  std::optional<std::string> Substitute(dom::Element* element, const std::string& pseudo, const std::string& text, int depth, std::set<std::string>& stack);
  bool SubstituteValues(dom::Element* element, const std::string& pseudo, const ComponentValues& in, ComponentValues& out, int depth, std::set<std::string>& stack);
  ComputeContext ContextFor(dom::Element* element, const std::string& property, const std::string& pseudo, const std::string& text);
  std::string Specified(dom::Element* element, const std::string& property, const PropertyDefinition& definition, bool& valid, const std::string& pseudo);
  std::string Finish(dom::Element* element, const std::string& property, const PropertyDefinition& definition, const std::string& specified, ComputeContext& context, const std::string& pseudo);

  Quanta::Context& ctx_;
  bool cycleHit_ = false;  // a property was asked for while it was being worked out
};

// revert and revert-layer: the declaration is replaced by the one that applies when its origin or layer is left out.
void RollBack(std::map<std::string, Winner>& winners) {
  for (auto it = winners.begin(); it != winners.end();) {
    Winner* w = &it->second;
    bool gone = false;
    Winner holder;
    for (;;) {
      if (w->entry->value.find("evert") == std::string::npos && w->entry->value.find("EVERT") == std::string::npos) break;
      const ComponentValues value = Trimmed(ParseComponentValues(w->entry->value));
      if (value.size() != 1 || !value[0].IsIdent()) break;
      const std::string keyword = Lower(value[0].token.value);
      if (keyword != "revert" && keyword != "revert-layer" && keyword != "revert-rule") break;
      const bool layerOnly = keyword == "revert-layer";
      const bool ruleOnly = keyword == "revert-rule";
      Winner best;
      std::vector<Winner> left;
      for (Winner& c : w->rest) {
        const bool sameOrigin = c.author == w->author;
        const bool dropped = ruleOnly ? c.rule == w->rule : layerOnly ? (sameOrigin && c.layer == w->layer && (c.origin == w->origin)) : sameOrigin;
        if (dropped) continue;
        left.push_back(std::move(c));
      }
      if (left.empty()) {
        gone = true;
        break;
      }
      size_t top = 0;
      for (size_t i = 1; i < left.size(); ++i) {
        if (Beats(left[i], left[top])) top = i;
      }
      holder = std::move(left[top]);
      left.erase(left.begin() + top);
      holder.rest = std::move(left);
      w = &holder;
    }
    if (gone) {
      it = winners.erase(it);
      continue;
    }
    if (w == &holder) it->second = std::move(holder);
    ++it;
  }
}

// The rules of the user agent's sheet, by key, as the rules of the author are.
struct UserAgentIndex {
  std::vector<uint32_t> always;
  std::unordered_map<uint64_t, std::vector<uint32_t>> buckets;
};

const UserAgentIndex& UaIndex() {
  static const UserAgentIndex index = [] {
    UserAgentIndex built;
    const std::vector<UserAgentRule>& rules = UserAgentRules();
    for (uint32_t i = 0; i < rules.size(); ++i) {
      std::vector<uint64_t> keys, markers;
      if (!KeysOf(rules[i].selectors, keys, markers)) { built.always.push_back(i); continue; }
      std::vector<uint64_t> done;
      for (uint64_t key : keys) {
        if (std::find(done.begin(), done.end(), key) != done.end()) continue;
        done.push_back(key);
        built.buckets[key].push_back(i);
      }
    }
    return built;
  }();
  return index;
}

void Build(dom::Element* element, ElementStyle& style, const std::string& pseudo) {
  if (style.built) return;
  style.built = true;
  const bool hasOfOuter = TakeHasEvaluated();  // (another style may be under way, which asked for this one)
  dom::Node* root = dom::ShadowIncludingRoot(element);
  MatchContext matchContext;
  matchContext.pseudoElement = pseudo.empty() ? nullptr : &pseudo;
  Gatherer gatherer{element, matchContext, style.cascade};
  gatherer.startingMode = StartingStyleFor(element, pseudo);
  // The user agent's rules first, then the author's.
  const std::vector<UserAgentRule>& uaRules = UserAgentRules();
  std::vector<uint32_t> uaPositions;
  if (gatherer.keys.usable) {
    const UserAgentIndex& index = UaIndex();
    uaPositions = index.always;
    const uint64_t marker = PseudoMarker(matchContext.pseudoElement);
    const auto take = [&](uint64_t key) {
      const auto found = index.buckets.find(key);
      if (found != index.buckets.end()) uaPositions.insert(uaPositions.end(), found->second.begin(), found->second.end());
    };
    for (uint64_t key : gatherer.keys.keys) take(key ^ marker);
    if (marker) take(marker);
    std::sort(uaPositions.begin(), uaPositions.end());
    uaPositions.erase(std::unique(uaPositions.begin(), uaPositions.end()), uaPositions.end());
  } else {
    for (uint32_t i = 0; i < uaRules.size(); ++i) uaPositions.push_back(i);
  }
  for (const uint32_t position : uaPositions) {
    const UserAgentRule& rule = uaRules[position];
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
      ++gatherer.ruleSerial;
      for (const DeclarationEntry& entry : rule.declarations) gatherer.Offer(entry, false, false, kUnlayered, best);
    }
  }
  // The width and height attributes of images and the like are declarations of no weight: any rule of the author wins over them.
  if (pseudo.empty() && (element->IsHtml() || (element->namespaceUri == dom::kSvgNamespace && element->localName == "svg"))) {
    const std::string& tag = element->localName;
    if (tag == "svg" || tag == "img" || tag == "video" || tag == "embed" || tag == "object" || tag == "iframe" || tag == "input") {
      const bool input = tag == "input";
      for (const char* name : {"width", "height"}) {
        const dom::Attr* attribute = element->FindAttribute("", name);
        if (!attribute) continue;
        if (input) {
          const dom::Attr* type = element->FindAttribute("", "type");
          std::string t = type ? type->value : "";
          for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
          if (t != "image") continue;
        }
        // "Rules for parsing dimension values": digits with a fraction, then a percent sign or nothing.
        const std::string& text = attribute->value;
        size_t i = 0;
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\f' || text[i] == '\r')) ++i;
        const size_t start = i;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
        if (i == start) continue;
        if (i < text.size() && text[i] == '.') {
          ++i;
          while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
        }
        const std::string number = text.substr(start, i - start);
        const bool percent = i < text.size() && text[i] == '%';
        DeclarationEntry entry;
        entry.name = name;
        entry.value = number + (percent ? "%" : "px");
        style.storage.push_back({entry});
        ++gatherer.ruleSerial;
        gatherer.Offer(style.storage.back()[0], true, false, -1000000, Specificity{});  // (below every layer)
      }
    }
  }
  // The tree the element is in: a shadow tree's elements are styled by that tree's sheets (and the document's own are not for
  // them). The root of the element's own tree is not the document's when it is in a shadow tree.
  dom::Node* treeRoot = element;
  while (treeRoot->parentNode) treeRoot = treeRoot->parentNode;
  if (treeRoot && (treeRoot->IsDocument() || treeRoot->IsFragment())) {
    if (treeRoot->IsFragment() && static_cast<dom::DocumentFragment*>(treeRoot)->isShadowRoot) gatherer.matchContext.host = static_cast<dom::DocumentFragment*>(treeRoot)->host;
    std::vector<CssStyleSheet*> sheets = SheetsOfTree(treeRoot, element->nodeDocument ? element->nodeDocument->context : nullptr);
    // The preferred set is named by the first sheet with a title that was on; sheets with another title are alternates, and off.
    if (element->nodeDocument && element->nodeDocument->hasPreferredSheetSet) {
      const std::string& preferred = element->nodeDocument->preferredSheetSet;
      sheets.erase(std::remove_if(sheets.begin(), sheets.end(), [&](const CssStyleSheet* sheet) { return sheet->hasTitle && sheet->ownerNode && sheet->title != preferred; }), sheets.end());
    }
    // The layers there are, and their order, are the same for every element of the tree (unless a container query decides on them): they are worked out once.
    struct LayerSnapshot {
      uint64_t version = 0;
      std::map<std::string, std::vector<std::string>> children;
      std::map<std::string, int> rank;
    };
    static std::unordered_map<const dom::Node*, LayerSnapshot> snapshots;
    const bool shareable = !(element->nodeDocument && element->nodeDocument->containerQueriesSeen);
    uint64_t snapshotVersion = AuthorStyleVersion() * 1000003 + sheets.size();
    {
      const MediaEnvironment& m = gatherer.media;
      snapshotVersion = (snapshotVersion * 31 + static_cast<uint64_t>(m.width * 8)) * 31 + static_cast<uint64_t>(m.height * 8);
      snapshotVersion = (snapshotVersion * 31 + static_cast<uint64_t>(m.resolution * 100)) * 31 + std::hash<std::string>()(m.colorScheme);
      snapshotVersion = (snapshotVersion * 31 + static_cast<uint64_t>(m.deviceWidth)) * 31 + static_cast<uint64_t>(m.deviceHeight);
    }
    auto snapshot = snapshots.find(treeRoot);
    if (shareable && snapshot != snapshots.end() && snapshot->second.version == snapshotVersion) {
      gatherer.layerChildren = snapshot->second.children;
      gatherer.layerRank = snapshot->second.rank;
      gatherer.anonymousLayers = 0;
    } else {
      gatherer.collecting = true;
      for (CssStyleSheet* sheet : sheets) {
        if (sheet->disabled || !MediaListMatches(sheet->media ? sheet->media->queries : std::vector<std::string>(), gatherer.media)) continue;
        gatherer.Rules(sheet->rules, kUnlayered);
      }
      gatherer.RankLayers();
      if (shareable) {
        if (snapshots.size() > 64) snapshots.clear();
        snapshots[treeRoot] = {snapshotVersion, gatherer.layerChildren, gatherer.layerRank};
      }
    }
    gatherer.collecting = false;
    for (CssStyleSheet* sheet : sheets) {
      if (sheet->disabled || !MediaListMatches(sheet->media ? sheet->media->queries : std::vector<std::string>(), gatherer.media)) continue;
      gatherer.Rules(sheet->rules, kUnlayered);
    }
  }
  // A shadow host also takes the :host rules of its shadow tree, below the rules of the tree it is in.
  if (element->shadowRoot && pseudo.empty()) {
    Gatherer inner{element, gatherer.matchContext, style.cascade};
    inner.startingMode = gatherer.startingMode;
    inner.matchContext.host = element;
    inner.order = gatherer.order;
    inner.ruleSerial = gatherer.ruleSerial + 1000000;
    const std::vector<CssStyleSheet*> sheets = SheetsOfTree(element->shadowRoot, element->nodeDocument ? element->nodeDocument->context : nullptr);
    inner.hostScoped = true;
    inner.layerBase = -1000000;
    inner.collecting = true;
    for (CssStyleSheet* sheet : sheets) {
      if (sheet->disabled || !MediaListMatches(sheet->media ? sheet->media->queries : std::vector<std::string>(), inner.media)) continue;
      inner.Rules(sheet->rules, -1000);
    }
    inner.RankLayers();
    inner.collecting = false;
    for (CssStyleSheet* sheet : sheets) {
      if (sheet->disabled || !MediaListMatches(sheet->media ? sheet->media->queries : std::vector<std::string>(), inner.media)) continue;
      inner.Rules(sheet->rules, -1000);
    }
  }
  if (pseudo.empty()) {
    const auto runSheets = [&](Gatherer& g, dom::Node* tree) {
      const std::vector<CssStyleSheet*> sheets = SheetsOfTree(tree, element->nodeDocument ? element->nodeDocument->context : nullptr);
      g.order = gatherer.order;
      g.ruleSerial = gatherer.ruleSerial + 2000000;
      g.collecting = true;
      for (CssStyleSheet* sheet : sheets) {
        if (sheet->disabled || !MediaListMatches(sheet->media ? sheet->media->queries : std::vector<std::string>(), g.media)) continue;
        g.Rules(sheet->rules, kUnlayered);
      }
      g.RankLayers();
      g.collecting = false;
      for (CssStyleSheet* sheet : sheets) {
        if (sheet->disabled || !MediaListMatches(sheet->media ? sheet->media->queries : std::vector<std::string>(), g.media)) continue;
        g.Rules(sheet->rules, kUnlayered);
      }
      gatherer.order = std::max(gatherer.order, g.order);
      gatherer.ruleSerial = std::max(gatherer.ruleSerial, g.ruleSerial);
    };
    // ::slotted() rules of the trees of the slots the element is assigned to.
    for (const dom::Element* slot = element->assignedSlot; slot; slot = slot->assignedSlot) {
      dom::Node* slotRoot = const_cast<dom::Element*>(slot);
      while (slotRoot->parentNode) slotRoot = slotRoot->parentNode;
      if (!slotRoot->IsFragment() || !static_cast<dom::DocumentFragment*>(slotRoot)->isShadowRoot) break;
      Gatherer g{element, gatherer.matchContext, style.cascade};
      g.mode = Gatherer::Mode::Slotted;
      g.slot = slot;
      g.contextRank = -1;
      g.matchContext.host = static_cast<dom::DocumentFragment*>(slotRoot)->host;
      runSheets(g, slotRoot);
    }
    // ::part() rules of the trees around the shadow host whose tree the element is in.
    if (treeRoot && treeRoot->IsFragment() && static_cast<dom::DocumentFragment*>(treeRoot)->isShadowRoot) {
      std::vector<std::string> names;
      if (const dom::Attr* part = element->FindAttribute("part")) names = SplitWords(part->value);
      const dom::Element* host = static_cast<dom::DocumentFragment*>(treeRoot)->host;
      int rank = 1;
      while (host && !names.empty()) {
        dom::Node* outer = const_cast<dom::Element*>(host);
        while (outer->parentNode) outer = outer->parentNode;
        if (outer->IsDocument() || outer->IsFragment()) {
          Gatherer g{element, gatherer.matchContext, style.cascade};
          g.mode = Gatherer::Mode::Part;
          g.partHost = host;
          g.partNames = names;
          g.contextRank = rank++;
          g.matchContext.host = outer->IsFragment() && static_cast<dom::DocumentFragment*>(outer)->isShadowRoot ? static_cast<dom::DocumentFragment*>(outer)->host : nullptr;
          runSheets(g, outer);
        }
        // exportparts="inner: outer, ..." lets the names go on to the tree around that one.
        if (!outer->IsFragment() || !static_cast<dom::DocumentFragment*>(outer)->isShadowRoot) break;
        std::vector<std::string> exported;
        if (const dom::Attr* attr = host->FindAttribute("exportparts")) {
          std::string current;
          std::vector<std::string> pieces;
          for (char c : attr->value) {
            if (c == ',') { pieces.push_back(current); current.clear(); } else current += c;
          }
          pieces.push_back(current);
          for (const std::string& piece : pieces) {
            const size_t colon = piece.find(':');
            const std::vector<std::string> inner = SplitWords(colon == std::string::npos ? piece : piece.substr(0, colon));
            const std::vector<std::string> outerName = SplitWords(colon == std::string::npos ? piece : piece.substr(colon + 1));
            if (inner.empty() || outerName.empty()) continue;
            if (std::find(names.begin(), names.end(), inner[0]) != names.end()) exported.push_back(outerName[0]);
          }
        }
        names = exported;
        host = static_cast<dom::DocumentFragment*>(outer)->host;
      }
    }
  }
  // The style attribute (a pseudo-element has none).
  if (const dom::Attr* attribute = pseudo.empty() ? element->FindAttribute("", "style") : nullptr) {
    // (The same text is parsed once: a page puts the same style attribute on many elements, and a style made again reads it again.)
    const std::string base = element->nodeDocument ? dom::DocumentBaseUri(element->nodeDocument) : "";
    static std::unordered_map<std::string, std::vector<DeclarationEntry>> parsed;
    const std::string key = base + '\x01' + attribute->value;
    auto found = parsed.find(key);
    if (found == parsed.end()) {
      SetValueBaseUrl(base);
      std::vector<DeclarationEntry> entries = ParseDeclarationList(attribute->value);
      SetValueBaseUrl("");
      if (parsed.size() > 20000) parsed.clear();
      found = parsed.emplace(key, std::move(entries)).first;
    }
    style.storage.push_back(found->second);
    ++gatherer.ruleSerial;
    for (const DeclarationEntry& entry : style.storage.back()) gatherer.Offer(entry, true, true, kUnlayered, Specificity{});
  }
  style.usedHas = TakeHasEvaluated();
  if (hasOfOuter) NoteHasEvaluated();
  RollBack(style.cascade);
  for (const auto& [name, winner] : style.cascade) {
    (void)winner;
    if (IsLogicalProperty(name)) {
      style.hasLogical = true;
      break;
    }
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
    if (v.kind == Cv::Kind::Function && Lower(v.name) == "attr") {
      AttrCall call;
      Cv head = v;
      if (ContainsSubstitution(v.children)) {
        // var() in the name and type; the fallback is substituted only if it is used.
        size_t comma = v.children.size();
        for (size_t i = 0; i < v.children.size(); ++i) {
          if (v.children[i].IsToken(T::Comma)) {
            comma = i;
            break;
          }
        }
        const ComponentValues before(v.children.begin(), v.children.begin() + comma);
        head.children.clear();
        if (!SubstituteValues(element, pseudo, before, head.children, depth + 1, stack)) return false;
        head.children.insert(head.children.end(), v.children.begin() + comma, v.children.end());
      }
      if (!ParseAttrCall(head, call)) return false;
      const dom::Attr* attribute = nullptr;
      if (!call.hasNamespace) {
        attribute = call.anyNamespace ? element->FindAttribute(call.name) : element->FindAttribute("", call.name);
        if (!attribute && element->namespaceUri == "http://www.w3.org/1999/xhtml") {
          const std::string lower = Lower(call.name);
          attribute = call.anyNamespace ? element->FindAttribute(lower) : element->FindAttribute("", lower);
        }
      }
      ComponentValues produced;
      bool ok = false;
      if (attribute) {
        const std::string& text = attribute->value;
        switch (call.type) {
          case AttrCall::Type::Raw: {
            Cv string;
            string.token.type = T::String;
            string.token.value = text;
            produced.push_back(string);
            ok = true;
            break;
          }
          case AttrCall::Type::Number:
          case AttrCall::Type::Unit: {
            const ComponentValues parsed = Trimmed(ParseComponentValues(text));
            if (parsed.size() == 1 && parsed[0].IsToken(T::Number)) {
              Cv number = parsed[0];
              if (call.type == AttrCall::Type::Unit) {
                number.token.type = call.unit == "%" ? T::Percentage : T::Dimension;
                if (call.unit != "%") number.token.value = call.unit;
              }
              produced.push_back(number);
              ok = true;
            }
            break;
          }
          case AttrCall::Type::Syntax: {
            const ComponentValues parsed = Trimmed(ParseComponentValues(text));
            ValueMatch match;
            if (!parsed.empty() && !ContainsSubstitution(parsed) && (call.syntax == "*" || MatchSyntax(call.syntax, parsed, match))) {
              produced = parsed;
              for (Cv& p : produced) {
                if (p.IsToken(T::Dimension)) p.token.value = Lower(p.token.value);
              }
              ok = true;
            }
            break;
          }
        }
      }
      if (ok) {
        for (Cv& p : produced) out.push_back(p);
      } else if (call.hasFallback) {
        if (!SubstituteValues(element, pseudo, call.fallback, out, depth + 1, stack)) return false;
      } else if (!call.explicitType || call.type == AttrCall::Type::Raw) {
        Cv empty;
        empty.token.type = T::String;
        out.push_back(empty);
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
  const bool fresh = !style.built;
  Build(element, style, pseudo);
  if (fresh && !pseudo.empty() && !style.cascade.empty()) Share(element, style, pseudo);
  const auto cached = style.memo->custom.find(name);
  if (cached != style.memo->custom.end()) return cached->second;
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
          style.memo->custom[name] = std::nullopt;
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
  style.memo->custom[name] = result;
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
  // (A shadow with no color is of the current color too.)
  const bool needsColor = lowered.find("currentcolor") != std::string::npos || property == "box-shadow" || property == "text-shadow" || property == "-webkit-box-shadow";
  const bool needsSizes = hasDigit || property == "font-size";
  const Parent parent = ParentOf(element, pseudo);
  const MediaEnvironment& media = CurrentMediaEnvironment();
  c.viewportWidth = media.width;
  c.viewportHeight = media.height;
  const bool viewportUnits = hasDigit && (lowered.find("vw") != std::string::npos || lowered.find("vh") != std::string::npos || lowered.find("vi") != std::string::npos || lowered.find("vb") != std::string::npos || lowered.find("vmin") != std::string::npos || lowered.find("vmax") != std::string::npos);
  if (viewportUnits && property != "scrollbar-gutter" && property != "scrollbar-width" && !property.starts_with("overflow") && property != "writing-mode") {
    double bar = 0;
    const double barWidth = ViewportScrollbarsWith(element->nodeDocument, [&](dom::Element* e, const char* p) { return Compute(e, p); }, bar);
    c.viewportWidth -= barWidth;
    c.viewportHeight -= bar;
  }
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
  // The units that come from the font.
  {
    const auto mentions = [&](const char* unit) { return lowered.find(unit) != std::string::npos; };
    const bool fontUnits = needsSizes && (mentions("ex") || mentions("ch") || mentions("cap") || mentions("ic"));
    if (fontUnits || needsLineHeight) {
      // (font-size is relative to the parent's font)
      dom::Element* fontElement = isFontSize ? parent.element : element;
      const std::string fontPseudo = isFontSize ? parent.pseudo : pseudo;
      if (fontElement) {
        const FontUnits u = FontUnitsFor(fontElement, fontPseudo, c.fontSize);
        c.exHeight = u.ex;
        c.chWidth = u.ch;
        c.capHeight = u.cap;
        c.icWidth = u.ic;
        if (u.lineHeight >= 0) c.lineHeight = u.lineHeight;
      }
      if (root && (mentions("rex") || mentions("rch") || mentions("rcap") || mentions("ric") || mentions("rlh"))) {
        const FontUnits r = FontUnitsFor(root, "", c.rootFontSize);
        c.rootExHeight = r.ex;
        c.rootChWidth = r.ch;
        c.rootCapHeight = r.cap;
        c.rootIcWidth = r.ic;
        if (r.lineHeight >= 0) c.rootLineHeight = r.lineHeight;
      }
    }
  }
  if (needsLineHeight && property != "line-height" && property != "font-size") {
    const std::string lh = Compute(element, "line-height", pseudo);
    const double px = ParsePx(lh);
    if (!std::isnan(px)) c.lineHeight = px;
    else {
      const ComponentValues v = Trimmed(ParseComponentValues(lh));
      if (v.size() == 1 && v[0].kind == Cv::Kind::Token && v[0].token.type == T::Number) c.lineHeight = v[0].token.number * c.fontSize;
    }
  }
  // The container query units are against the nearest container.
  if (text.find("cq") != std::string::npos) {
    double w, h;
    bool vertical = false;
    if (NearestContainerSize(ctx_, element, !pseudo.empty(), w, h, vertical)) {
      c.containerWidth = w;
      c.containerHeight = h;
      c.containerVertical = vertical;
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
  plainInherit_ = false;
  ElementStyle& style = StyleOf(element, pseudo);
  const bool fresh = !style.built;
  Build(element, style, pseudo);
  if (fresh && !pseudo.empty() && !style.cascade.empty()) Share(element, style, pseudo);
  const Parent parent = ParentOf(element, pseudo);
  const auto fromParent = [&]() -> std::string {
    if (parent.element) return Compute(parent.element, property, parent.pseudo);
    return Compute(element, "\x01initial:" + property, pseudo);
  };
  // These two are inline, or a block when floated.
  if (property == "display" && (pseudo == "first-letter" || pseudo == "first-line")) {
    if (pseudo == "first-letter" && Compute(element, "float", pseudo) != "none") return "block";
    return "inline";
  }
  // The properties that do not apply to ::first-letter and ::first-line are the initial ones, if they are not inherited.
  if ((pseudo == "first-letter" || pseudo == "first-line") && !AppliesToFirstPseudo(property, pseudo == "first-letter")) return definition.inherited ? fromParent() : "initial";
  // What the rest of a declared value comes to: var() substituted, a CSS-wide keyword answered.
  const auto fromText = [&](std::string text) -> std::string {
    if (ContainsSubstitution(ParseComponentValues(text))) {
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
      if (definition.inherited && parent.element) return fromParent();
      return "initial";
    }
    return text;
  };
  // The animations' values come over the declared ones (but for !important and transitions, which are not here).
  if (const std::string* animated = AnimatedValue(element, pseudo, property)) return fromText(*animated);
  auto winner = style.cascade.find(property);
  std::string ownName = property;
  if (style.hasLogical && HasLogicalCounterparts(property)) {
    // A logical property that is, here, this one competes with it in the cascade.
    const WritingContext writing = {Compute(element, "writing-mode", pseudo), Compute(element, "direction", pseudo)};
    for (const std::string& logical : LogicalsOf(property, writing)) {
      const auto candidate = style.cascade.find(logical);
      if (candidate != style.cascade.end() && (winner == style.cascade.end() || Beats(candidate->second, winner->second))) {
        winner = candidate;
        ownName = logical;
      }
    }
  }
  if (winner == style.cascade.end()) {
    if (definition.inherited && parent.element) {
      std::string inherited = fromParent();
      plainInherit_ = true;  // (it is what the parent has, finished already)
      return inherited;
    }
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
      if (l.name == ownName) text = l.value;
    }
    if (text.empty()) {
      valid = false;
      return "initial";
    }
  }
  return fromText(text);
}

std::string TextOfComputed(const ValueMatch& match) { return SerializeValue(match.normalized); }

namespace {
std::unordered_map<const PropertyDefinition*, std::string>& InitialComputed() {
  static std::unordered_map<const PropertyDefinition*, std::string> cache;
  return cache;
}

// Whether the initial value of the property comes to the same for every element: only keywords that mean one thing, and numbers.
bool ContextFreeInitial(const PropertyDefinition& definition) {
  const std::string name = definition.name;
  if (name.size() >= 6 && name.compare(name.size() - 6, 6, "-width") == 0) return false;
  if (name.rfind("font", 0) == 0 || name.rfind("line-height", 0) == 0 || name.rfind("outline", 0) == 0 || name.rfind("text-decoration", 0) == 0 || name.rfind("-webkit", 0) == 0) return false;
  const std::string text = InitialValueText(definition);
  if (text.empty() || text == "initial") return false;
  static const std::set<std::string> keywords = {"auto", "none", "normal", "visible", "static", "baseline", "start", "stretch", "row", "nowrap", "ltr", "horizontal-tb", "inline", "block", "repeat",
                                                 "scroll", "padding-box", "border-box", "content-box", "transparent", "separate", "outside", "clip", "default", "wrap", "ease", "flex-start", "0",
                                                 "left", "top", "stroke", "butt", "miter", "evenodd", "nonzero", "fill", "visiblepainted", "match-parent", "space-around", "manipulation", "disc"};
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && text[i] == ' ') ++i;
    size_t j = i;
    while (j < text.size() && text[j] != ' ') ++j;
    if (j == i) break;
    const std::string token = text.substr(i, j - i);
    i = j;
    if (keywords.count(token)) continue;
    // A number, in px or a percentage.
    size_t k = 0;
    if (k < token.size() && (token[k] == '-' || token[k] == '+')) ++k;
    const size_t digits = k;
    while (k < token.size() && ((token[k] >= '0' && token[k] <= '9') || token[k] == '.')) ++k;
    if (k == digits) return false;
    const std::string unit = token.substr(k);
    if (!(unit.empty() || unit == "px" || unit == "%")) return false;
  }
  return true;
}
}  // namespace

const std::string* FastInitial(const PropertyDefinition& definition) {
  const auto found = InitialComputed().find(&definition);
  return found == InitialComputed().end() ? nullptr : &found->second;
}

void NoteInitial(const PropertyDefinition& definition, const std::string& computed) {
  if (!(&definition >= kPropertyDefinitions && &definition < kPropertyDefinitions + kPropertyDefinitionCount)) return;
  if (computed.empty() || InitialComputed().count(&definition)) return;
  if (ContextFreeInitial(definition)) InitialComputed()[&definition] = computed;
}

std::string Resolver::Compute(dom::Element* element, const std::string& property, const std::string& pseudo) {
  if (!element) return "";
  // The initial value, asked for by the root of the tree.
  bool initialOnly = false;
  std::string name = property;
  if (property.rfind("\x01initial:", 0) == 0) {
    initialOnly = true;
    name = property.substr(9);
  }
  if (!initialOnly && pseudo.empty() && !AnimatedValuesSuppressed() && !StartingStyleFor(element, pseudo)) {
    dom::Node* tree = dom::ShadowIncludingRoot(element);
    if (tree && tree->IsDocument()) Prepare(element);
  }
  ElementStyle& style = StyleOf(element, pseudo);
  if (!initialOnly) {
    const auto cached = style.memo->computed.find(name);
    if (cached != style.memo->computed.end()) return cached->second;
  }
  if (name.starts_with("--")) {
    std::optional<std::string> value = Custom(element, name, pseudo);
    return value.value_or("");
  }
  const PropertyDefinition* definition = FindProperty(name);
  if (!definition) return "";
  if (IsLogicalProperty(name)) {
    // A logical property computes to what the physical one it is here computes to.
    const WritingContext writing = {Compute(element, "writing-mode", pseudo), Compute(element, "direction", pseudo)};
    const std::string physical = PhysicalOf(name, writing);
    if (!physical.empty() && FindProperty(physical)) return Compute(element, initialOnly ? "\x01initial:" + physical : physical, pseudo);
  }
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
  // A value nothing was declared for is the parent's, as it was finished there, or the initial one: neither is worked out again for each element.
  if (!initialOnly && plainInherit_ && valid) {
    plainInherit_ = false;
    style.resolving.erase("\x02" + name);
    StyleOf(element, pseudo).memo->computed[name] = specified;
    return specified;
  }
  plainInherit_ = false;
  if (specified == "initial") {
    if (const std::string* fast = FastInitial(*definition)) {
      style.resolving.erase("\x02" + name);
      if (!initialOnly) StyleOf(element, pseudo).memo->computed[name] = *fast;
      return *fast;
    }
  }
  ComputeContext context = ContextFor(element, name, pseudo, specified == "initial" ? InitialValueText(*definition) : specified);
  std::string result = Finish(element, name, *definition, specified, context, pseudo);
  if (specified == "initial") NoteInitial(*definition, result);
  style.resolving.erase("\x02" + name);
  if (!initialOnly) StyleOf(element, pseudo).memo->computed[name] = result;
  return result;
}

// The computed value of a shadow list: each shadow as color, offsets, blur (and spread, for a box), then inset, nothing left out.
std::string NormalizeShadows(const std::string& text, size_t lengths, const std::string& currentColor) {
  const ComponentValues values = Trimmed(ParseComponentValues(text));
  if (values.size() == 1 && values[0].IsIdent()) return text;
  std::string out;
  for (const ComponentValues& layer : SplitOnCommas(values)) {
    std::string color;
    std::vector<std::string> parts;
    bool inset = false;
    for (const ComponentValue& v : layer) {
      if (v.IsWhitespace()) continue;
      if (v.IsIdent() && Lower(v.token.value) == "inset") inset = true;
      else if (v.kind == ComponentValue::Kind::Token && (v.token.type == T::Dimension || v.token.type == T::Number)) parts.push_back(Serialize({v}));
      else if (v.IsIdent() && Lower(v.token.value) == "currentcolor") color = currentColor;
      else color = Serialize({v});
    }
    if (color.empty()) color = currentColor;
    while (parts.size() < lengths) parts.push_back("0px");
    std::string piece = color;
    for (const std::string& part : parts) piece += " " + part;
    if (inset) piece += " inset";
    out += (out.empty() ? "" : ", ") + piece;
  }
  return out;
}

std::string Resolver::Finish(dom::Element* element, const std::string& property, const PropertyDefinition& definition, const std::string& specified, ComputeContext& context, const std::string& pseudo) {
  std::string text = specified;
  if (text == "initial") text = InitialValueText(definition);
  ComponentValues values = Trimmed(ParseComponentValues(text));
  if (values.empty()) return "";
  if (IsCssWideKeyword(values)) return Lower(values[0].token.value);
  const Parent parent = ParentOf(element, pseudo);

  // A subgrid is a grid container in a grid: elsewhere what it says of itself does not count.
  if ((property == "grid-template-columns" || property == "grid-template-rows") && values.size() >= 1 && values[0].IsIdent() && Lower(values[0].token.value) == "subgrid" && pseudo.empty()) {
    const std::string own = Compute(element, "display", pseudo);
    if (own == "grid" || own == "inline-grid" || own == "grid-lanes" || own == "inline-grid-lanes") {
      const std::string up = parent.element ? Compute(parent.element, "display", "") : std::string();
      if (up != "grid" && up != "inline-grid" && up != "grid-lanes" && up != "inline-grid-lanes") return "none";
      // The axis that grid lanes run along is not a grid of lines, so there is nothing to be a subgrid of.
      if (up == "grid-lanes" || up == "inline-grid-lanes") {
        const std::string direction = Compute(parent.element, "grid-lanes-direction", "");
        const bool rowLanes = direction.rfind("row", 0) == 0 || (direction.rfind("normal", 0) == 0 && Compute(parent.element, "grid-template-columns", "") == "none" && Compute(parent.element, "grid-template-rows", "") != "none");
        if ((property == "grid-template-columns") == rowLanes) return "none";
      }
    }
  }

  // The legend of a fieldset is block-level (https://html.spec.whatwg.org/multipage/rendering.html#fieldset-and-legend): its display is blockified.
  if (property == "display" && pseudo.empty() && element->IsHtml("legend") &&
      values.size() == 1 && values[0].IsIdent()) {
    const std::string word = Lower(values[0].token.value);
    if (word == "inline") return "block";
    if (word == "inline-block") return "flow-root";
    if (word == "inline-flex") return "flex";
    if (word == "inline-grid") return "grid";
    if (word == "inline-table") return "table";
  }

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
  if (property == "grid-lanes-direction") {
    // Computed, the reversals come in one order: fill-reverse, then track-reverse.
    std::string base;
    bool fill = false, track = false;
    for (const ComponentValue& v : values) {
      if (!v.IsIdent()) continue;
      const std::string w = Lower(v.token.value);
      if (w == "fill-reverse") fill = true;
      else if (w == "track-reverse") track = true;
      else base = w;
    }
    if (!base.empty()) return base + (fill ? " fill-reverse" : "") + (track ? " track-reverse" : "");
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
  {
    // A relative url() is taken against where the declaration came from.
    const auto source = StyleOf(element, pseudo).cascade.find(property);
    SetValueBaseUrl(source != StyleOf(element, pseudo).cascade.end() ? source->second.entry->base : "");
  }
  struct ResetBase {
    ~ResetBase() { SetValueBaseUrl(""); }
  } resetBase;
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
  if (property == "box-shadow" || property == "-webkit-box-shadow") return NormalizeShadows(TextOfComputed(match), 4, context.currentColor);
  if (property == "text-shadow") return NormalizeShadows(TextOfComputed(match), 3, context.currentColor);
  return TextOfComputed(match);
}

}  // namespace

void SetLayoutHook(LayoutHook hook) { g_layoutHook = hook; }
uint64_t g_animationMentions = 0;
void SnapshotComputedValues(Quanta::Context& ctx, dom::Element* element) {
  if (!element || !element->nodeDocument || !dom::ShadowIncludingRoot(element)->IsDocument()) return;
  for (size_t i = 0; i < kPropertyDefinitionCount; ++i) {
    const PropertyDefinition& definition = kPropertyDefinitions[i];
    if (IsShorthand(definition)) continue;
    const std::string name = definition.name;
    if (name.rfind("transition", 0) == 0 || name.rfind("animation", 0) == 0 || name.rfind("-webkit-", 0) == 0) continue;
    const std::string value = ComputedValue(ctx, element, name);
    if (!value.empty()) element->lastRead[""][name] = value;
  }
}
namespace {
Quanta::Context* g_wakeContext = nullptr;
dom::Document* g_wakeDocument = nullptr;
bool g_inWake = false;
}  // namespace
void SetAnimationWake(Quanta::Context& ctx, dom::Document* document) {
  g_wakeContext = &ctx;
  g_wakeDocument = document;
}
void NoteAnimationMention() {
  ++g_animationMentions;
  // The frame that starts what is declared has to be asked for, as nothing else tells that there is something to start.
  if (g_wakeDocument && g_wakeDocument->animationWake && g_wakeContext && !g_inWake) {
    g_inWake = true;
    Quanta::Embed::Call(*g_wakeContext, Quanta::Embed::FromObject(g_wakeDocument->animationWake), Quanta::Embed::Undefined());
    g_inWake = false;
  }
}
uint64_t AnimationMentions() { return g_animationMentions; }
void NoteStyleChange() {
  ++g_styleVersion;
  ++g_cacheVersion;
  ++g_authorVersion;
}
void NoteStyleChangeForRegistry() { NoteStyleChange(); }
void NoteAnimatedStyleChange() {
  ++g_styleVersion;
  if (g_anyContainerQueries) ++g_cacheVersion;  // (the size of a container may be what an animation gives)
}
uint64_t AuthorStyleVersion() { return dom::TreeVersion() * 1000003 + g_authorVersion; }
uint64_t StyleVersion() { return g_styleVersion; }

namespace {

struct SelectorFeatures {
  bool dynamic = false;     // depends on the state of elements (hover, focus, checked...) that no change of the tree tells of
  bool has = false;         // :has(): an element depends on what is under it
  bool structural = false;  // depends on the place among the siblings
  bool sibling = false;     // + and ~
};

void ScanComplex(const ComplexSelector& selector, SelectorFeatures& f);

void ScanList(const SelectorList& list, SelectorFeatures& f) {
  for (const ComplexSelector& s : list) ScanComplex(s, f);
}

void ScanComplex(const ComplexSelector& selector, SelectorFeatures& f) {
  for (const Combinator c : selector.combinators) if (c == Combinator::NextSibling || c == Combinator::SubsequentSibling) f.sibling = true;
  if (selector.leading == Combinator::NextSibling || selector.leading == Combinator::SubsequentSibling) f.sibling = true;
  for (const CompoundSelector& compound : selector.compounds) {
    for (const SimpleSelector& simple : compound.simples) {
      switch (simple.kind) {
        case SimpleSelector::Kind::Pseudo:
          switch (simple.pseudo) {
            case PseudoClass::Empty: case PseudoClass::FirstChild: case PseudoClass::LastChild: case PseudoClass::OnlyChild:
            case PseudoClass::FirstOfType: case PseudoClass::LastOfType: case PseudoClass::OnlyOfType: f.structural = true; break;
            // An upgrade of a custom element tells of nothing.
            case PseudoClass::Defined: f.dynamic = true; break;
            // The rest depend on attributes (the element's and those above it), on a state that tells of its changes (hover, active and focus:
            // NoteStateChange; target: the address of the document, which is in the signature of it), or are not matched by anything yet.
            default: break;
          }
          break;
        case SimpleSelector::Kind::Has: f.has = true; if (simple.list) ScanList(*simple.list, f); break;
        case SimpleSelector::Kind::Nth: f.structural = true; if (simple.list) { f.sibling = true; ScanList(*simple.list, f); } break;  // ("of S": the siblings' own attributes count)
        case SimpleSelector::Kind::Not: case SimpleSelector::Kind::Is: case SimpleSelector::Kind::Where: case SimpleSelector::Kind::Nesting:
          if (simple.list) ScanList(*simple.list, f);
          break;
        case SimpleSelector::Kind::Host: f.dynamic = true; break;
        default: break;
      }
    }
  }
}

void ScanRules(const std::vector<CssRule*>& rules, SelectorFeatures& f, int depth) {
  if (depth > 12) { f.dynamic = true; return; }
  for (const CssRule* rule : rules) {
    if (rule->selectors) ScanList(*rule->selectors, f);
    if (rule->scopeStart) ScanList(*rule->scopeStart, f);
    if (rule->scopeEnd) ScanList(*rule->scopeEnd, f);
    ScanRules(rule->rules, f, depth + 1);
    if (rule->kind == RuleKind::Import && rule->importedSheet) ScanRules(rule->importedSheet->rules, f, depth + 1);
  }
}

}  // namespace

uint64_t SheetsVersion() { return g_authorVersion; }

uint64_t StyleEpochOf(dom::Element* element) {
  StyleOf(element);
  return Cache()[element].epoch;
}

std::shared_ptr<void> SharedStyleToken(Quanta::Context&, dom::Element* element, const std::string& pseudo) {
  if (AnimatedValuesSuppressed() || StartingStyleFor(element, pseudo) || (element->nodeDocument && element->nodeDocument->containerQueriesSeen)) return nullptr;
  dom::Node* tree = dom::ShadowIncludingRoot(element);
  if (!tree || !tree->IsDocument()) return nullptr;
  ElementStyle& style = StyleOf(element, pseudo);
  if (!style.built || style.memo.use_count() <= 1) return nullptr;  // (the style is made by asking for a value, as ever)
  return style.memo;
}

StyleReuse StyleReuseFor(dom::Document* document) {
  struct Entry {
    dom::Document* document = nullptr;
    uint64_t author = 0;
    std::vector<CssStyleSheet*> sheets;
    StyleReuse reuse;
  };
  static Entry entry;
  StyleReuse none;
  // (Adopted style sheets are in the sheets: the array tells of every change of it. A container query is of a size that layout finds, not of the elements.)
  if (!document || document->containerQueriesSeen) return none;
  const std::vector<CssStyleSheet*> sheets = SheetsOfTree(document, document->context);
  if (entry.document == document && entry.author == g_authorVersion && entry.sheets == sheets && entry.reuse.shadows == dom::HasShadowTrees()) return entry.reuse;
  SelectorFeatures f;
  for (const CssStyleSheet* sheet : sheets) ScanRules(sheet->rules, f, 0);
  entry.document = document;
  entry.author = g_authorVersion;
  entry.sheets = sheets;
  entry.reuse = {!f.dynamic, f.structural, f.sibling, dom::HasShadowTrees(), f.has};
  return entry.reuse;
}

namespace {

void NotePseudoNames(const SelectorList& list, std::unordered_set<std::string>& names) {
  for (const ComplexSelector& selector : list) {
    for (const CompoundSelector& compound : selector.compounds) {
      for (const SimpleSelector& simple : compound.simples) {
        if (simple.kind == SimpleSelector::Kind::PseudoElement) names.insert(simple.name);
        if (simple.list) NotePseudoNames(*simple.list, names);
      }
    }
  }
}

void NotePseudoNamesOfRules(const std::vector<CssRule*>& rules, std::unordered_set<std::string>& names, int depth) {
  if (depth > 40) return;
  for (const CssRule* rule : rules) {
    if (rule->selectors) NotePseudoNames(*rule->selectors, names);
    if (rule->scopeStart) NotePseudoNames(*rule->scopeStart, names);
    if (rule->scopeEnd) NotePseudoNames(*rule->scopeEnd, names);
    NotePseudoNamesOfRules(rule->rules, names, depth + 1);
    if (rule->importedSheet) NotePseudoNamesOfRules(rule->importedSheet->rules, names, depth + 1);
  }
}

// Whether a rule of the document's sheets or of the user agent's could be for a pseudo-element of that name (a document with no shadow trees only).
bool PseudoNameMayHaveRules(dom::Element* element, const std::string& pseudo) {
  if (dom::HasShadowTrees() || !element->nodeDocument) return true;
  dom::Node* tree = dom::ShadowIncludingRoot(element);
  if (!tree || !tree->IsDocument()) return true;
  static const std::unordered_set<std::string> ua = [] {
    std::unordered_set<std::string> names;
    for (const UserAgentRule& rule : UserAgentRules()) NotePseudoNames(rule.selectors, names);
    return names;
  }();
  if (ua.count(pseudo)) return true;
  static std::unordered_map<const dom::Node*, std::pair<uint64_t, std::unordered_set<std::string>>> cache;
  const uint64_t version = AuthorStyleVersion();
  auto found = cache.find(tree);
  if (found == cache.end() || found->second.first != version) {
    std::unordered_set<std::string> names;
    for (CssStyleSheet* sheet : SheetsOfTree(tree, element->nodeDocument->context)) NotePseudoNamesOfRules(sheet->rules, names, 0);
    if (cache.size() > 64) cache.clear();
    found = cache.insert_or_assign(tree, std::make_pair(version, std::move(names))).first;
  }
  return found->second.second.count(pseudo) != 0;
}

}  // namespace

bool HasPseudoStyle(dom::Element* element, const std::string& pseudo) {
  if (!PseudoNameMayHaveRules(element, pseudo)) return false;
  ElementStyle& style = StyleOf(element, pseudo);
  if (!style.built) {
    Build(element, style, pseudo);
    if (!style.cascade.empty()) Share(element, style, pseudo);
  }
  return !style.cascade.empty();
}

void ViewportScrollbars(Quanta::Context& ctx, dom::Document* document, double& vertical, double& horizontal, ViewportAuto* autos) {
  vertical = ViewportScrollbarsWith(document, [&](dom::Element* e, const char* p) { return ComputedValue(ctx, e, p); }, horizontal, autos);
}

std::string ComputedValue(Quanta::Context& ctx, dom::Element* element, const std::string& property, const std::string& pseudo) {
  static int depth = 0;
  // What animations give is brought up to date first, when what it depends on has changed.
  static bool flushing = false;
  dom::Document* document = element ? element->nodeDocument : nullptr;
  const auto flush = [&]() {
    if (!document || !document->styleFlush || !document->animationStyleSeen || flushing || StyleFlushSuppressed()) return;
    const uint64_t version = AuthorStyleVersion();
    if (document->styleFlushedVersion == version) return;
    flushing = true;
    Quanta::Embed::Call(ctx, Quanta::Embed::FromObject(document->styleFlush), Quanta::Embed::Undefined());
    flushing = false;
    document->styleFlushedVersion = AuthorStyleVersion();
  };
  if (depth == 0) flush();
  struct Depth {
    Depth() { ++depth; }
    ~Depth() { --depth; }
  } guard;
  struct Active {
    Quanta::Context* saved;
    explicit Active(Quanta::Context& c) : saved(g_activeContext) { g_activeContext = &c; }
    ~Active() { g_activeContext = saved; }
  } active(ctx);
  // A style that depends on the size of a container is made from the sizes layout found: layout is brought up to date first.
  static bool inLayoutHook = false;
  const auto bringLayout = [&]() {
    if (!document || !document->containerQueriesSeen || !g_layoutHook || inLayoutHook || depth != 1) return;
    inLayoutHook = true;
    g_layoutHook(ctx, document);
    inLayoutHook = false;
  };
  bringLayout();
  Resolver resolver(ctx);
  std::string result = resolver.Compute(element, property, pseudo);
  if (document && document->containerQueriesSeen && g_layoutHook && !inLayoutHook && depth == 1) {
    // (The first time a rule asks for a container's size, the size is not known yet: layout finds it and the style is made again.)
    static std::set<const dom::Document*> asked;
    if (asked.insert(document).second) {
      bringLayout();
      result = resolver.Compute(element, property, pseudo);
    }
  }
  if (depth == 1 && document && !document->animationStyleSeen && element) {
    const ElementStyle& style = StyleOf(element, pseudo);
    if (style.cascade.count("animation-name") || style.cascade.count("transition-property") || style.cascade.count("transition-duration")) {
      document->animationStyleSeen = true;
      document->styleFlushedVersion = 0;
      depth = 0;
      flush();
      depth = 1;
    }
  }
  return result;
}

namespace {
ResolvedValueHook g_resolvedHook = nullptr;
}

void SetResolvedValueHook(ResolvedValueHook hook) { g_resolvedHook = hook; }

StyleReader::StyleReader(Quanta::Context& ctx, dom::Element* element, const std::string& pseudo) : ctx_(ctx), element_(element), pseudo_(pseudo) {}
StyleReader::~StyleReader() = default;

std::string StyleReader::Get(const char* property) {
  const uint64_t now = dom::TreeVersion() * 1000003 + g_styleVersion;
  if (memo_ && version_ == now && !element_->nodeDocument->containerQueriesSeen && !AnimatedValuesSuppressed() && !StartingStyleFor(element_, pseudo_) &&
      (!element_->nodeDocument->animationStyleSeen || element_->nodeDocument->styleFlushedVersion == AuthorStyleVersion())) {
    const Memo& memo = *static_cast<const Memo*>(memo_.get());
    const auto found = memo.computed.find(std::string_view(property));
    if (found != memo.computed.end()) return found->second;
  }
  std::string value = ComputedValue(ctx_, element_, property, pseudo_);
  // After a first answer the style of the element is made, and what is asked next is found in it (unless the document asks for more: containers, animations).
  if (!memo_ && element_->nodeDocument && !element_->nodeDocument->containerQueriesSeen && !AnimatedValuesSuppressed() && !StartingStyleFor(element_, pseudo_)) {
    dom::Node* tree = dom::ShadowIncludingRoot(element_);
    if (tree && tree->IsDocument()) {
      memo_ = StyleOf(element_, pseudo_).memo;
      version_ = dom::TreeVersion() * 1000003 + g_styleVersion;
    }
  }
  return value;
}

std::string ResolvedValue(Quanta::Context& ctx, dom::Element* element, const std::string& property, const std::string& pseudo) {
  std::string used;
  // The computed value is what a transition starts from the next time style changes: it is kept as far as it was read.
  const std::string computed = ComputedValue(ctx, element, property, pseudo);
  if (element && !computed.empty() && element->nodeDocument) {
    const PropertyDefinition* definition = FindProperty(property);
    if (definition && !IsShorthand(*definition)) {
      // An element with no box has no style to change from: what it has is a first style once it is rendered again. (Only styles already made are looked at.)
      bool rendered = true;
      if (property != "display") {
        const ElementStyle& made = StyleOf(element, pseudo);
        const auto display = made.memo->computed.find("display");
        rendered = display == made.memo->computed.end() || display->second != "none";
        if (rendered && !pseudo.empty()) {
          const ElementStyle& owner = StyleOf(element, "");
          const auto ownerDisplay = owner.memo->computed.find("display");
          rendered = ownerDisplay == owner.memo->computed.end() || ownerDisplay->second != "none";
        }
      }
      if (rendered) element->lastRead[pseudo][property] = computed;
      else element->lastRead.erase(pseudo);
    }
  }
  if (g_resolvedHook && pseudo.empty() && element && element->nodeDocument && g_resolvedHook(ctx, element, property, pseudo, used)) return used;
  return computed;
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

std::vector<CssStyleSheet*> SheetsOfTreeRoot(dom::Node* root) {
  return SheetsOfTree(root, root->IsDocument() ? static_cast<dom::Document*>(root)->context : nullptr);
}

MediaEnvironment EnvironmentFor(const dom::Document* document) {
  MediaEnvironment environment = CurrentMediaEnvironment();
  dom::Element* frame = document ? document->frameElement : nullptr;
  if (!frame || !frame->nodeDocument || !frame->nodeDocument->context || frame->nodeDocument == document) return environment;
  // The size of the frame: its width and height as its style attribute gives them in px, or its attributes, or 300 by 150.
  // (Not through the cascade: this is asked while a style is being worked out.)
  std::vector<DeclarationEntry> inlineStyle;
  if (const dom::Attr* attribute = frame->FindAttribute("", "style")) inlineStyle = ParseDeclarationList(attribute->value);
  const auto size = [&](const char* property, double fallback) {
    for (const DeclarationEntry& entry : inlineStyle) {
      if (entry.name == property && entry.value.size() > 2 && entry.value.compare(entry.value.size() - 2, 2, "px") == 0) return std::atof(entry.value.c_str());
    }
    if (const dom::Attr* attribute = frame->FindAttribute("", property)) {
      const int parsed = std::atoi(attribute->value.c_str());
      if (parsed > 0) return static_cast<double>(parsed);
    }
    return fallback;
  };
  environment.width = size("width", 300);
  environment.height = size("height", 150);
  return environment;
}

}  // namespace solar::css
