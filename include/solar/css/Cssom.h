/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "quanta/Embed.h"
#include "solar/css/RareString.h"
#include "solar/css/Registry.h"
#include "solar/css/Selectors.h"
#include "solar/css/Syntax.h"
#include "solar/dom/Node.h"

// The CSS Object Model (https://drafts.csswg.org/cssom/): style sheets, their rules and declaration blocks, as
// objects of the same heap the DOM lives in. Script holds the very cells that the engine reads its style from.
namespace solar::css {

struct CssRule;
struct CssStyleSheet;
struct CssDeclarations;
struct MediaList;
struct CssRuleList;

// The kinds of rule. The first values are CSSRule.type's.
enum class RuleKind {
  Style = 1,
  Import = 3,
  Media = 4,
  FontFace = 5,
  Page = 6,
  Keyframes = 7,
  Keyframe = 8,
  Namespace = 10,
  CounterStyle = 11,
  Supports = 12,
  LayerBlock = 100,
  LayerStatement,
  Property,
  Container,
  Scope,
  StartingStyle,
  NestedDeclarations,
  FontPaletteValues,
  PositionTry,
};

// One declaration of a block: its name (lowercased for a known property, as written for a custom one), its
// value as the text it serializes to, and its priority.
struct DeclarationEntry {
  std::string name;
  std::string value;
  bool important = false;
  // A longhand set by a shorthand with a var() in it has no value of its own until the variable is known: it reads as
  // empty, and the shorthand's text is kept here.
  RareString pendingShorthand;
  RareString pendingText;
  dom::Name base;  // what a relative url() in the value is taken against, when it is computed (the few addresses of a page's sheets: one copy of each)
};

// The declarations of a block, made when they are first asked for. A plain declaration of a style rule (no custom property, no var(), nothing about animations, no keyword that takes its value
// from elsewhere) is kept as the text `Apply` is given (name, value, priority) until the rule is one an element matches or script reads: a page has tens of thousands of rules, most of
// which no element ever matches, and a declaration made (a shorthand into its longhands) is a hundred bytes and more where the text of it is thirty. Where the whole of the rules is
// looked at for what they declare (animations, pseudo-elements, custom properties), a rule that has not been made has declared none of it by what it was let to be.
struct CssDeclarations;
class DeclarationItems {
 public:
  using Vec = std::vector<DeclarationEntry>;
  explicit DeclarationItems(CssDeclarations* owner) : owner_(owner) {}
  // Whether the declarations are still the text they were given as, which looking at them would make.
  bool Lazy() const { return pending_ != nullptr; }
  void SetPending(std::string text) { pending_ = std::make_unique<std::string>(std::move(text)); }
  Vec& vec() { Force(); return items_; }
  const Vec& vec() const { Force(); return items_; }
  operator Vec&() { return vec(); }
  operator const Vec&() const { return vec(); }
  size_t size() const { return vec().size(); }
  bool empty() const { return !Lazy() && items_.empty(); }
  DeclarationEntry& operator[](size_t i) { return vec()[i]; }
  const DeclarationEntry& operator[](size_t i) const { return vec()[i]; }
  Vec::iterator begin() { return vec().begin(); }
  Vec::iterator end() { return vec().end(); }
  Vec::const_iterator begin() const { return vec().begin(); }
  Vec::const_iterator end() const { return vec().end(); }
  DeclarationEntry& back() { return vec().back(); }
  void push_back(DeclarationEntry entry) { vec().push_back(std::move(entry)); }
  Vec::iterator erase(Vec::iterator at) { return vec().erase(at); }
  Vec::iterator erase(Vec::iterator from, Vec::iterator to) { return vec().erase(from, to); }
  template <class... A> Vec::iterator insert(A&&... a) { return vec().insert(std::forward<A>(a)...); }
  void clear() { pending_.reset(); items_.clear(); }
  void shrink_to_fit() { items_.shrink_to_fit(); }
  DeclarationItems& operator=(Vec other) { pending_.reset(); items_ = std::move(other); return *this; }

 private:
  void Force() const;
  CssDeclarations* owner_;
  mutable std::unique_ptr<std::string> pending_;
  mutable Vec items_;
};

// A CSSStyleDeclaration: the declarations of a style rule, a style attribute, or the computed style of an element.
struct CssDeclarations : Quanta::DOMObject {
  DeclarationItems items{this};
  CssRule* parentRule = nullptr;
  dom::Element* ownerElement = nullptr;  // for the style attribute
  bool readonly = false;                  // computed style
  bool updatingAttribute = false;
  // getComputedStyle's: the element the values are the computed ones of, read when they are asked for.
  // Only those objects have it, so it is made for them and the others keep one pointer.
  struct Computed {
    dom::Element* element = nullptr;
    Quanta::Context* context = nullptr;
    std::string pseudo;   // "before", "after"...: the style of the pseudo-element
  };
  dom::Element* computedElement() const { return computed_ ? computed_->element : nullptr; }
  Quanta::Context* computedContext() const { return computed_->context; }
  const std::string& computedPseudo() const { return computed_->pseudo; }
  Computed& ComputedMut() { if (!computed_) computed_ = std::make_unique<Computed>(); return *computed_; }
  std::unique_ptr<Computed> computed_;

  // The CSSOM serialization of the block: "a: b; c: d !important;".
  std::string Serialize() const;
  // Replaces the declarations with those in `text`.
  void SetText(Quanta::Context& ctx, std::string_view text);
  // Adds or replaces a declaration (with the order the standard asks for); false if the value is invalid.
  bool Set(Quanta::Context& ctx, const std::string& name, const std::string& value, bool important);
  std::string Remove(Quanta::Context& ctx, const std::string& name);
  const DeclarationEntry* Find(const std::string& name) const;
  // A declaration of a property (a shorthand is expanded into its longhands); false if the value is not valid for it.
  bool Apply(const std::string& name, const std::string& text, bool important);
  // Whether the block is a rule's descriptors (@font-face, @font-palette-values): what it holds are not properties.
  bool IsDescriptorBlock() const;
  // The address relative urls in the declarations are taken against.
  std::string BaseUrl() const;
  // What getPropertyValue and getPropertyPriority answer; a shorthand is answered from its longhands.
  std::string ValueOf(const std::string& name) const;
  std::string PriorityOf(const std::string& name) const;
  // Told to the owner: a style attribute is rewritten, a rule's sheet is marked changed.
  void Changed(Quanta::Context& ctx);

  // The legacy platform object hooks: style[0], style.color, style["background-color"], style.cssFloat.
  static bool IndexedGetter(Quanta::Context& ctx, CssDeclarations& self, uint32_t index, Quanta::Value& out);
  static uint32_t IndexedLength(Quanta::Context&, CssDeclarations& self);
  static bool NamedGetter(Quanta::Context& ctx, CssDeclarations& self, const std::string& name, Quanta::Value& out);
  static void NamedSetter(Quanta::Context& ctx, CssDeclarations& self, const std::string& name, const Quanta::Value& value);
  static std::vector<std::string> NamedKeys(Quanta::Context&, CssDeclarations&) { return {}; }
  static constexpr bool LegacyUnenumerableNamedProperties = true;

  void Visit(Quanta::Visitor& visitor);
};

// The property a CSSStyleDeclaration attribute name stands for ("backgroundColor", "cssFloat", "webkitFoo",
// "background-color"), dashed and lowercase, or empty if it stands for none.
std::string PropertyForIdlName(const std::string& name);

struct MediaList : Quanta::DOMObject {
  std::vector<std::string> queries;  // each serialized
  CssStyleSheet* ownerSheet = nullptr;
  CssRule* ownerRule = nullptr;

  std::string Text() const;
  void SetText(std::string_view text);
  static bool IndexedGetter(Quanta::Context& ctx, MediaList& self, uint32_t index, Quanta::Value& out);
  static uint32_t IndexedLength(Quanta::Context&, MediaList& self) { return static_cast<uint32_t>(self.queries.size()); }
  void Visit(Quanta::Visitor& visitor);
};

struct CssRule : Quanta::DOMObject {
  RuleKind kind = RuleKind::Style;
  CssRule* parentRule = nullptr;
  CssStyleSheet* parentSheet = nullptr;

  // Style rule, page rule, font-face, keyframe, counter-style, property, nested declarations: a block of declarations.
  CssDeclarations* style = nullptr;
  // Style rule: its selector list as written (serialized), and parsed. Page rule: the selector text.
  RareString selectorText;  // (a page rule's; a style rule's is made from `selectors` when it is asked for: SelectorTextOf)
  std::shared_ptr<SelectorList> selectors;
  // What a style rule needs of an element at the least, one key (an id, a class or a type) for each of its selectors: worked out when it is first looked at, and kept
  // beside the rule (seventy-three bytes in every rule of a page that has tens of thousands, whether or not anything has looked).
  struct Quick {
    std::vector<uint64_t> keys;
    std::vector<uint64_t> markers;  // and the pseudo-element each selector ends in (0: none)
    std::vector<std::vector<uint64_t>> ancestors;  // for each selector, the keys the elements around the one it is for must have
    uint8_t state = 0;  // 0: not worked out; 1: every selector has a key; 2: some has none
  };
  std::unique_ptr<Quick> quick_;
  Quick& QuickMut() { if (!quick_) quick_ = std::make_unique<Quick>(); return *quick_; }
  const std::vector<uint64_t>& quickKeys() const { static const std::vector<uint64_t> none; return quick_ ? quick_->keys : none; }
  const std::vector<uint64_t>& quickMarkers() const { static const std::vector<uint64_t> none; return quick_ ? quick_->markers : none; }
  const std::vector<std::vector<uint64_t>>& quickAncestors() const { static const std::vector<std::vector<uint64_t>> none; return quick_ ? quick_->ancestors : none; }
  uint8_t quickState() const { return quick_ ? quick_->state : 0; }
  // Grouping rules (media, supports, layer, container, scope, starting-style, style rules with nested rules,
  // keyframes, page rules with margin rules): the rules in them.
  std::vector<CssRule*> rules;
  CssRuleList* ruleList = nullptr;
  // Media rule, import rule: the media; import rule: the address and the sheet it brought in.
  MediaList* media = nullptr;
  // What a few kinds of rule have and a style rule does not (the layer of an import or a layer rule, the conditions of an import, a supports rule and a container rule, the
  // bounds of a scope rule, the sheet an import brought in): made for the rule that has one.
  struct Extras {
    RareString layerName;       // import and layer rules: the layer, a layer statement: the names, comma-separated
    RareString supportsText;    // import rule's supports(), supports rule's condition, container rule's condition
    RareString containerName, containerQuery;  // a container rule: the name, and the condition, when there is one query
    std::shared_ptr<SelectorList> scopeStart, scopeEnd;  // scope rule: <scope-start> and <scope-end> (none: absent)
    CssStyleSheet* importedSheet = nullptr;  // an import rule: the sheet it brought in
    RareString href;            // import rule: the address
    RareString prefix, namespaceUri;  // namespace rule
    RareString name;            // keyframes rule, property rule, counter-style, layer block: the name. Keyframe rule: the key text
    RareString prelude;         // the prelude of a rule this module keeps as written (container, scope, page): its serialization
  };
  std::unique_ptr<Extras> extras_;
  Extras& ExtrasMut() { if (!extras_) extras_ = std::make_unique<Extras>(); return *extras_; }
  const RareString& layerName() const { static const RareString none; return extras_ ? extras_->layerName : none; }
  const RareString& supportsText() const { static const RareString none; return extras_ ? extras_->supportsText : none; }
  const RareString& containerName() const { static const RareString none; return extras_ ? extras_->containerName : none; }
  const RareString& containerQuery() const { static const RareString none; return extras_ ? extras_->containerQuery : none; }
  const std::shared_ptr<SelectorList>& scopeStart() const { static const std::shared_ptr<SelectorList> none; return extras_ ? extras_->scopeStart : none; }
  const std::shared_ptr<SelectorList>& scopeEnd() const { static const std::shared_ptr<SelectorList> none; return extras_ ? extras_->scopeEnd : none; }
  const RareString& href() const { static const RareString none; return extras_ ? extras_->href : none; }
  const RareString& prefix() const { static const RareString none; return extras_ ? extras_->prefix : none; }
  const RareString& namespaceUri() const { static const RareString none; return extras_ ? extras_->namespaceUri : none; }
  const RareString& name() const { static const RareString none; return extras_ ? extras_->name : none; }
  const RareString& prelude() const { static const RareString none; return extras_ ? extras_->prelude : none; }
  CssStyleSheet* importedSheet() const { return extras_ ? extras_->importedSheet : nullptr; }
  // Scope rule: <scope-start> and <scope-end> (none: absent).
  bool isNested = false;  // a style rule inside another
  // A property rule: what it registers, if it is a valid one.
  std::unique_ptr<RegisteredProperty> registered;  // (made for the rule that registers one: of a rule in a hundred it is a hundred and fifty bytes)

  std::string CssText() const;
  // What selectorText answers: the serialization of the selectors of a style rule, made when it is asked for (a page has tens of thousands of rules, which almost none is asked of).
  std::string SelectorTextOf() const;
  // A keyframes rule is indexable by its keyframes.
  static bool IndexedGetter(Quanta::Context& ctx, CssRule& self, uint32_t index, Quanta::Value& out);
  static uint32_t IndexedLength(Quanta::Context&, CssRule& self) { return self.kind == RuleKind::Keyframes ? static_cast<uint32_t>(self.rules.size()) : 0; }
  void Visit(Quanta::Visitor& visitor);
};

// A rule taken out of its sheet or its parent rule: nothing holds it any more.
void DetachRule(CssRule* rule);

struct CssRuleList : Quanta::DOMObject {
  CssStyleSheet* ownerSheet = nullptr;
  CssRule* ownerRule = nullptr;

  const std::vector<CssRule*>& Rules() const;
  static bool IndexedGetter(Quanta::Context& ctx, CssRuleList& self, uint32_t index, Quanta::Value& out);
  static uint32_t IndexedLength(Quanta::Context&, CssRuleList& self) { return static_cast<uint32_t>(self.Rules().size()); }
  void Visit(Quanta::Visitor& visitor);
};

// document.styleSheets: the sheets of the document's style and link elements, in tree order, as they are now.
struct StyleSheetList : Quanta::DOMObject {
  dom::Node* document = nullptr;  // the document, or the shadow root, whose style and link elements these are the sheets of
  std::vector<CssStyleSheet*> Sheets() const;
  static bool IndexedGetter(Quanta::Context& ctx, StyleSheetList& self, uint32_t index, Quanta::Value& out);
  static uint32_t IndexedLength(Quanta::Context&, StyleSheetList& self) { return static_cast<uint32_t>(self.Sheets().size()); }
  void Visit(Quanta::Visitor& visitor) { visitor.Mark(document); }
};

struct CssStyleSheet : Quanta::DOMObject {
  std::string type = "text/css";
  std::string href;           // absent (null) for an inline sheet: hasHref says which
  bool hasHref = false;
  dom::Node* ownerNode = nullptr;
  CssStyleSheet* parentSheet = nullptr;
  CssRule* ownerRule = nullptr;
  std::string title;
  bool hasTitle = false;
  MediaList* media = nullptr;
  bool disabled = false;
  bool alternate = false;
  bool pendingAttach = false;      // made for an element (or an @import) that is to have it: what it changes is told when it has it (see NoteSheetAttached)
  bool additiveAnnounced = false;  // it came into the document as new rules only, and style made what that changed for the elements it can be for (not for all of them)
  std::vector<CssRule*> rules;
  CssRuleList* ruleList = nullptr;
  std::string baseUrl;
  bool constructed = false;
  bool originClean = true;
  bool disallowImport = false;
  dom::Document* constructorDocument = nullptr;

  void Visit(Quanta::Visitor& visitor);
};

// The declarations of a style attribute or of a declaration block's text, expanded into longhands.
std::vector<DeclarationEntry> ParseDeclarationList(std::string_view text);
// A declaration added to a list of entries: false if its value is not valid for the property.
// The base of the sheet being parsed, for the declarations made before their rules know their sheet.
void SetParsingBase(const std::string& base);
bool ApplyDeclaration(std::vector<DeclarationEntry>& items, const std::string& name, const std::string& text, bool important);

// ---- Making them ----

CssStyleSheet* NewStyleSheet(Quanta::Context& ctx);
CssRule* NewRule(Quanta::Context& ctx, RuleKind kind);
CssDeclarations* NewDeclarations(Quanta::Context& ctx);
// Whether @position-try takes the property.
bool IsPositionTryProperty(const std::string& name);
// The declarations of a @position-try rule (a CSSPositionTryDescriptors).
CssDeclarations* NewPositionTryDeclarations(Quanta::Context& ctx);
MediaList* NewMediaList(Quanta::Context& ctx);
CssRuleList* NewRuleList(Quanta::Context& ctx);
StyleSheetList* NewStyleSheetList(Quanta::Context& ctx, dom::Node* document);

// Fills a sheet with the rules of `text`: "parse a CSS style sheet".
void ParseSheetInto(Quanta::Context& ctx, CssStyleSheet* sheet, std::string_view text);
// "parse a rule" for insertRule: the rule `text` is, or null when it is not one. `error` is the name of the
// DOMException to throw ("SyntaxError", "HierarchyRequestError", "InvalidStateError") when it is.
CssRule* ParseRuleText(Quanta::Context& ctx, std::string_view text, CssStyleSheet* sheet, CssRule* parent, size_t index, std::string& error);

// "insert a CSS rule" and "remove a CSS rule" into the rules of a sheet or of a grouping rule. The error is a
// DOMException's name, empty if there is none.
std::string InsertRule(Quanta::Context& ctx, CssStyleSheet* sheet, CssRule* parent, std::string_view text, uint32_t index, uint32_t& result);
std::string DeleteRule(Quanta::Context& ctx, CssStyleSheet* sheet, CssRule* parent, uint32_t index);

// A keyframe selector as it is written back: from and to as percentages. Nothing if it is not a list of them.
std::optional<std::string> NormalizeKeyText(const std::string& text);
// A keyframe rule from its parsed form; null when it is not one.
CssRule* BuildKeyframe(Quanta::Context& ctx, const Rule& frame, CssStyleSheet* sheet, CssRule* parent);

// Whether a property name is one this engine knows (or a custom property), lowercased in `name` if so.
bool NormalizePropertyName(std::string& name);

// Checks the namespaces a selector list names against the sheet's @namespace rules (and drops the any-namespace prefix when no
// default is declared); false if a prefix is not declared.
bool ResolveNamespaces(SelectorList& list, const CssStyleSheet* sheet);

// ---- Loading ----

using SheetLoader = std::function<std::optional<std::string>(const std::string& absoluteUrl)>;
// Loads the style sheets that the @import rules of `sheet` name, and theirs in turn: each rule's styleSheet is the sheet,
// or null where it could not be had. A sheet that imports one it is already in the middle of is not loaded again.
void ProcessImports(Quanta::Context& ctx, CssStyleSheet* sheet, const SheetLoader& load, int depth = 0);
// The loader that an @import inserted with insertRule is fetched with.
void SetSheetLoader(SheetLoader loader);
// What loads a font (a font is asked for with CORS, a style sheet without): when none is set, the sheet loader does.
void SetFontLoader(SheetLoader loader);
const SheetLoader& GetFontLoader();
// Whatever was set, for what else needs to fetch (font files).
const SheetLoader& GetSheetLoader();
// A sheet for a link element, made of the text at `address`.
CssStyleSheet* NewLinkedSheet(Quanta::Context& ctx, dom::Element* link, const std::string& address, const std::string& text);

// ---- Feature queries ----

// CSS.supports(property, value): whether the engine takes the declaration.
bool SupportsDeclaration(const std::string& property, const std::string& value);
// The condition of @supports and CSS.supports(condition): its truth, false for one that is not a <supports-condition>.
bool SupportsCondition(const ComponentValues& condition);

// ---- Binding ----

// Defines the CSSOM interfaces in a realm; the prototypes are kept there for NewRule and the rest.
void DefineCssomClasses(Quanta::Context& ctx);
// window.matchMedia and MediaQueryList.
void DefineMediaQueryList(Quanta::Context& ctx);
// Defines the native that the CSS namespace's supports() is (as __solarCssSupports, for the script that makes CSS to take).
void InstallCssSupports(Quanta::Context& ctx);
// The StyleSheet of a <style> or <link> element, made and parsed on demand; null if the element has none.
CssStyleSheet* StyleSheetOfElement(Quanta::Context& ctx, dom::Element* element);
// Re-reads the sheet of an element whose text, type or media changed (<style>), or that was inserted or removed.
void UpdateStyleElement(Quanta::Context& ctx, dom::Element* element);
// The style attribute of an element changed: its CSSStyleDeclaration, if script has one, follows.
void StyleAttributeChanged(dom::Element* element);

}  // namespace solar::css
