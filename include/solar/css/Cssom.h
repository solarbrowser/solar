#pragma once

#include <memory>
#include <string>
#include <vector>

#include "quanta/Embed.h"
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
};

// A CSSStyleDeclaration: the declarations of a style rule, a style attribute, or the computed style of an element.
struct CssDeclarations : Quanta::DOMObject {
  std::vector<DeclarationEntry> items;
  CssRule* parentRule = nullptr;
  dom::Element* ownerElement = nullptr;  // for the style attribute
  bool readonly = false;                  // computed style
  bool updatingAttribute = false;

  // The CSSOM serialization of the block: "a: b; c: d !important;".
  std::string Serialize() const;
  // Replaces the declarations with those in `text`.
  void SetText(Quanta::Context& ctx, std::string_view text);
  // Adds or replaces a declaration (with the order the standard asks for); false if the value is invalid.
  bool Set(Quanta::Context& ctx, const std::string& name, const std::string& value, bool important);
  std::string Remove(Quanta::Context& ctx, const std::string& name);
  const DeclarationEntry* Find(const std::string& name) const;
  // Told to the owner: a style attribute is rewritten, a rule's sheet is marked changed.
  void Changed(Quanta::Context& ctx);

  // The legacy platform object hooks: style[0], style.color, style["background-color"], style.cssFloat.
  static bool IndexedGetter(Quanta::Context& ctx, CssDeclarations& self, uint32_t index, Quanta::Value& out);
  static uint32_t IndexedLength(Quanta::Context&, CssDeclarations& self) { return static_cast<uint32_t>(self.items.size()); }
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

// Whether a property name is one this engine knows (or a custom property), lowercased in `name` if so.
bool NormalizePropertyName(std::string& name);

// ---- Binding ----

// Defines the CSSOM interfaces in a realm; the prototypes are kept there for NewRule and the rest.
void DefineCssomClasses(Quanta::Context& ctx);
// The StyleSheet of a <style> or <link> element, made and parsed on demand; null if the element has none.
CssStyleSheet* StyleSheetOfElement(Quanta::Context& ctx, dom::Element* element);
// Re-reads the sheet of an element whose text, type or media changed (<style>), or that was inserted or removed.
void UpdateStyleElement(Quanta::Context& ctx, dom::Element* element);
// The style attribute of an element changed: its CSSStyleDeclaration, if script has one, follows.
void StyleAttributeChanged(dom::Element* element);

}  // namespace solar::css
