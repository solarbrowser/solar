#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The CSS tokenizer (https://drafts.csswg.org/css-syntax/#tokenization): text in, tokens out. Selectors
// are parsed from its tokens, and so are style sheets.
namespace solar::css {

struct Token {
  enum class Type {
    Ident,
    Function,    // an identifier and the "(" after it
    AtKeyword,
    Hash,
    String,
    BadString,
    Url,
    BadUrl,
    Delim,
    Number,
    Percentage,
    Dimension,
    Whitespace,
    Cdo,         // <!--
    Cdc,         // -->
    Colon,
    Semicolon,
    Comma,
    LeftBracket,
    RightBracket,
    LeftParen,
    RightParen,
    LeftBrace,
    RightBrace,
    EndOfFile,
  };
  Type type = Type::EndOfFile;
  // An identifier's, function's, at-keyword's, hash's or string's text (escapes decoded), a url's address,
  // or a dimension's unit.
  std::string value;
  char32_t delim = 0;
  double number = 0;
  bool isInteger = false;
  bool hasSign = false;       // a number written with a leading + or -
  bool hashIsId = false;      // a hash that is also a valid identifier: #foo and not #1f
  std::string representation;  // a number's text as it was written

  bool Is(Type other) const { return type == other; }
  bool IsDelim(char32_t c) const { return type == Type::Delim && delim == c; }
};

std::vector<Token> Tokenize(std::string_view text);

// Appends a code point as UTF-8.
void AppendUtf8(std::string& out, char32_t c);
// The CSS identifier serialization of `text` (CSS.escape).
std::string EscapeIdentifier(std::string_view text);

}  // namespace solar::css
