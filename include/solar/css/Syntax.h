#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "solar/css/Tokenizer.h"

// CSS Syntax (https://drafts.csswg.org/css-syntax/#parsing): the tokens of a text grouped into component values,
// declarations and rules, and the serialization that turns them back into text. Style sheets, the CSSOM and every
// value grammar are built on it.
namespace solar::css {

// A preserved token, a function (`name`, with its arguments in `children`) or a simple block (`open` is the token
// that opens it, `children` what is in it).
struct ComponentValue {
  enum class Kind { Token, Function, Block };
  Kind kind = Kind::Token;
  Token token;
  std::string name;
  Token::Type open = Token::Type::LeftBrace;
  std::vector<ComponentValue> children;

  bool IsToken(Token::Type type) const { return kind == Kind::Token && token.type == type; }
  bool IsDelim(char32_t c) const { return kind == Kind::Token && token.IsDelim(c); }
  bool IsWhitespace() const { return IsToken(Token::Type::Whitespace); }
  bool IsIdent() const { return IsToken(Token::Type::Ident); }
  bool IsBlock(Token::Type opener) const { return kind == Kind::Block && open == opener; }
};

using ComponentValues = std::vector<ComponentValue>;

struct Declaration {
  std::string name;  // as written: property names are matched ASCII case-insensitively, custom property names are not
  ComponentValues value;
  bool important = false;
  // The value as written, for a custom property, whose value is its tokens' text exactly.
  std::string originalText;
};

struct Rule {
  bool isAtRule = false;
  std::string name;  // an at-rule's name, without the @
  ComponentValues prelude;
  bool hasBlock = false;
  // The block of an at-rule or qualified rule, as the component values between its braces.
  ComponentValues block;
};

// What a block holds: declarations and, in a nested context, rules, in the order they were written.
struct BlockItem {
  bool isDeclaration = false;
  Declaration declaration;
  Rule rule;
};

// "parse a stylesheet's contents": the top-level rules of a style sheet. CDO and CDC are skipped.
std::vector<Rule> ParseStylesheetContents(std::string_view text);
// "parse a list of rules" (a rule list inside a block, where CDO and CDC are ordinary): the rules in `text`.
std::vector<Rule> ParseRuleList(std::string_view text);
std::vector<Rule> ParseRuleList(const ComponentValues& values);
// "parse a rule": exactly one rule (qualified or at-rule), nothing else. False if there is none or more.
bool ParseRule(std::string_view text, Rule& out);
// "parse a block's contents" as the declarations and rules of a style rule's block, or of a style attribute.
std::vector<BlockItem> ParseBlockContents(const ComponentValues& block);
std::vector<BlockItem> ParseBlockContents(std::string_view text);
// "parse a declaration": exactly one declaration. False if it is not one.
bool ParseDeclaration(std::string_view text, Declaration& out);
// "parse a component value": exactly one. False if there are none or more.
bool ParseComponentValue(std::string_view text, ComponentValue& out);
// "parse a list of component values".
ComponentValues ParseComponentValues(std::string_view text);
ComponentValues ParseComponentValues(const std::vector<Token>& tokens);
// "parse a comma-separated list of component values": the lists between the top-level commas.
std::vector<ComponentValues> SplitOnCommas(const ComponentValues& values);

// "serialize a CSS component value" / the tokens of a value as text. A comment is put between two tokens that would
// otherwise read back as one, as the spec's serialization asks.
std::string SerializeToken(const Token& token);
std::string Serialize(const ComponentValues& values);
std::string Serialize(const ComponentValue& value);
std::string SerializeTokens(const std::vector<Token>& tokens);

// Removes the whitespace at both ends of a list of component values.
ComponentValues Trimmed(const ComponentValues& values);

// "serialize a string" / "serialize an identifier" / "serialize a URL", from CSSOM.
std::string SerializeString(std::string_view text);
std::string SerializeIdentifier(std::string_view text);
std::string SerializeUrl(std::string_view text);

}  // namespace solar::css
