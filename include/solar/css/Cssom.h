#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "quanta/Embed.h"
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
};

// One declaration of a block: its name (lowercased for a known property, as written for a custom one), its
// value as the text it serializes to, and its priority.
struct DeclarationEntry {
  std::string name;
  std::string value;
  bool important = false;
  // A longhand set by a shorthand with a var() in it has no value of its own until the variable is known: it reads as
  // empty, and the shorthand's text is kept here.
  std::string pendingShorthand;
  std::string pendingText;
};

// A CSSStyleDeclaration: the declarations of a style rule, a style attribute, or the computed style of an element.
struct CssDeclarations : Quanta::DOMObject {
  std::vector<DeclarationEntry> items;
  CssRule* parentRule = nullptr;
  dom::Element* ownerElement = nullptr;  // for the style attribute
  bool readonly = false;                  // computed style
  bool updatingAttribute = false;
  // getComputedStyle's: the element the values are the computed ones of, read when they are asked for.
  dom::Element* computedElement = nullptr;
  Quanta::Context* computedContext = nullptr;
  std::string computedPseudo;   // "before", "after"...: the style of the pseudo-element

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
  std::string selectorText;
  std::shared_ptr<SelectorList> selectors;
  // Grouping rules (media, supports, layer, container, scope, starting-style, style rules with nested rules,
  // keyframes, page rules with margin rules): the rules in them.
  std::vector<CssRule*> rules;
  CssRuleList* ruleList = nullptr;
  // Media rule, import rule: the media; import rule: the address and the sheet it brought in.
  MediaList* media = nullptr;
  std::string href;
  CssStyleSheet* importedSheet = nullptr;
  std::string layerName;       // import and layer rules: the layer, a layer statement: the names, comma-separated
  std::string supportsText;    // import rule's supports(), supports rule's condition, container rule's condition
  // Namespace rule.
  std::string prefix;
  std::string namespaceUri;
  // Keyframes rule, property rule, counter-style, layer block: the name. Keyframe rule: the key text.
  std::string name;
  // The prelude of a rule this module keeps as written (container, scope, page): its serialization.
  std::string prelude;
  bool isNested = false;  // a style rule inside another
  // A property rule: what it registers, if it is a valid one.
  std::optional<RegisteredProperty> registered;

  std::string CssText() const;
  void Visit(Quanta::Visitor& visitor);
};

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
  dom::Document* document = nullptr;
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
bool ApplyDeclaration(std::vector<DeclarationEntry>& items, const std::string& name, const std::string& text, bool important);

// ---- Making them ----

CssStyleSheet* NewStyleSheet(Quanta::Context& ctx);
CssRule* NewRule(Quanta::Context& ctx, RuleKind kind);
CssDeclarations* NewDeclarations(Quanta::Context& ctx);
MediaList* NewMediaList(Quanta::Context& ctx);
CssRuleList* NewRuleList(Quanta::Context& ctx);
StyleSheetList* NewStyleSheetList(Quanta::Context& ctx, dom::Document* document);

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
