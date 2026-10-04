#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "quanta/Embed.h"
#include "solar/dom/Node.h"
#include "solar/html/Tokenizer.h"

// The HTML tree builder (https://html.spec.whatwg.org/#tree-construction): tokens in, DOM tree out. Most of
// what is in it is the standard's insertion modes, one function each, and the algorithms they share.
namespace solar::html {

// What the parser does with a script element: Normal, Disabled (noscript is content) and the two modes
// of fragment parsing, Inert (scripts are marked as already started) and Fragment.
enum class ScriptingMode { Normal, Disabled, Inert, Fragment };

class TreeBuilder {
 public:
  // Builds the tree of `markup` into `document`, which is taken to be empty.
  TreeBuilder(Quanta::Context& ctx, dom::Document* document, std::string_view markup, ScriptingMode scripting);

  // Makes this a fragment parser for `context`, which `fragment` (made by the caller) receives the nodes of.
  void SetUpFragment(dom::Element* context, dom::DocumentFragment* fragment);

  void Run();

  // The parse errors, by name, in the tokenizer's and then the tree builder's order of finding them.
  std::vector<std::string> Errors() const;

 private:
  enum class Mode {
    Initial,
    BeforeHtml,
    BeforeHead,
    InHead,
    InHeadNoscript,
    AfterHead,
    InBody,
    Text,
    InTable,
    InTableText,
    InCaption,
    InColumnGroup,
    InTableBody,
    InRow,
    InCell,
    InTemplate,
    AfterBody,
    InFrameset,
    AfterFrameset,
    AfterAfterBody,
    AfterAfterFrameset,
  };
  enum class Scope { Default, ListItem, Button, Table };

  struct Location {
    dom::Node* parent = nullptr;
    dom::Node* reference = nullptr;
  };

  // An entry of the list of active formatting elements: an element and the token it was made for, or a marker.
  struct Formatting {
    dom::Element* element = nullptr;
    Token token;
    bool marker = false;
  };

  void Error(const char* code) { errors_.push_back(code); }

  // The dispatcher and the insertion modes.
  void Process(Token& token);
  void ProcessUsing(Mode mode, Token& token);
  void ProcessForeign(Token& token);
  void Initial(Token& token);
  void BeforeHtml(Token& token);
  void BeforeHead(Token& token);
  void InHead(Token& token);
  void InHeadNoscript(Token& token);
  void AfterHead(Token& token);
  void InBody(Token& token);
  void InBodyAnyOtherEndTag(Token& token);
  void TextMode(Token& token);
  void InTable(Token& token);
  void InTableText(Token& token);
  void InCaption(Token& token);
  void InColumnGroup(Token& token);
  void InTableBody(Token& token);
  void InRow(Token& token);
  void InCell(Token& token);
  void InTemplate(Token& token);
  void AfterBody(Token& token);
  void InFrameset(Token& token);
  void AfterFrameset(Token& token);
  void AfterAfterBody(Token& token);
  void AfterAfterFrameset(Token& token);

  // The stack of open elements.
  dom::Element* CurrentNode() const { return open_.empty() ? nullptr : open_.back(); }
  dom::Element* AdjustedCurrentNode() const;
  void Pop() { open_.pop_back(); }
  void PopUntilHtml(std::string_view name);
  void PopUntilOneOf(std::initializer_list<std::string_view> names);
  void RemoveFromStack(dom::Element* element);
  bool InStack(const dom::Element* element) const;
  bool HasInScope(std::string_view name, Scope scope = Scope::Default) const;
  bool HasInScopeOneOf(std::initializer_list<std::string_view> names, Scope scope = Scope::Default) const;
  bool HasInScopeElement(const dom::Element* element) const;
  bool IsScopeBoundary(const dom::Element* element, Scope scope) const;
  bool HasTemplateInStack() const;
  bool ParsingTemplateContents() const;
  void GenerateImpliedEndTags(std::string_view except = "");
  void GenerateAllImpliedEndTagsThoroughly();
  void ClosePElement();
  void ClearStackBackTo(std::initializer_list<std::string_view> names);
  void ResetInsertionMode();

  // Creating and inserting nodes.
  Location AppropriatePlace(dom::Node* overrideTarget = nullptr) const;
  Location AdjustedInsertionLocation(dom::Node* overrideTarget = nullptr) const;
  dom::Element* CreateElementForToken(const Token& token, std::string_view ns, dom::Node* intendedParent);
  dom::Document* TemplateContentsOwner(dom::Document* document);
  void InsertElementAt(dom::Element* element, Location location);
  dom::Element* InsertForeignElement(const Token& token, std::string_view ns, bool onlyAddToStack);
  dom::Element* InsertHtmlElement(const Token& token) { return InsertForeignElement(token, dom::kHtmlNamespace, false); }
  // An HTML element for a start tag with this name and no attributes.
  dom::Element* InsertHtmlElementNamed(std::string_view name);
  // Adds the token's attributes the element does not have, for a second html or body start tag.
  void AddMissingAttributes(dom::Element* element, const Token& token);
  void InsertCharacter(const std::string& utf8);
  void InsertComment(const Token& token, const Location* at = nullptr);
  void InsertProcessingInstruction(const Token& token, const Location* at = nullptr);
  void GenericTextElement(Token& token, Tokenizer::State state);

  // The list of active formatting elements.
  void PushFormatting(dom::Element* element, const Token& token);
  void InsertMarker();
  void ReconstructFormatting();
  void ClearFormattingToMarker();
  int FindFormatting(const dom::Element* element) const;
  void AdoptionAgency(Token& token);

  // Odds and ends.
  void SetQuirks(dom::Document::Mode mode);
  void AdjustMathAttributes(Token& token);
  void AdjustSvgAttributes(Token& token);
  void AdjustForeignAttributes(Token& token);
  bool IsHtmlIntegrationPoint(const dom::Element* element) const;
  static bool IsMathTextIntegrationPoint(const dom::Element* element);
  static bool IsSpecial(const dom::Element* element);
  static bool IsFormatting(std::string_view name);
  void SwitchMode(Mode mode) { mode_ = mode; }
  void StopParsing() { done_ = true; }

  Quanta::Context& ctx_;
  dom::Document* document_;
  Tokenizer tokenizer_;
  ScriptingMode scripting_;
  std::vector<std::string> errors_;

  Mode mode_ = Mode::Initial;
  Mode originalMode_ = Mode::Initial;
  std::vector<Mode> templateModes_;
  std::vector<dom::Element*> open_;
  std::vector<Formatting> formatting_;
  dom::Element* head_ = nullptr;
  dom::Element* form_ = nullptr;
  bool framesetOk_ = true;
  bool fosterParenting_ = false;
  bool skipNextLineFeed_ = false;
  bool done_ = false;
  std::vector<std::string> pendingTableCharacters_;

  dom::Element* fragmentContext_ = nullptr;
  dom::DocumentFragment* rootInsertionTarget_ = nullptr;
};

}  // namespace solar::html
