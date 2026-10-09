#include "solar/css/Syntax.h"

#include <cmath>
#include <cstdio>

namespace solar::css {

namespace {

using T = Token::Type;

bool StartsWith(const std::string& text, std::string_view prefix) { return text.compare(0, prefix.size(), prefix) == 0; }

Token::Type ClosingOf(Token::Type open) {
  switch (open) {
    case T::LeftBrace: return T::RightBrace;
    case T::LeftBracket: return T::RightBracket;
    default: return T::RightParen;
  }
}

// The cursor over a token list that every algorithm of the syntax module reads through.
class Stream {
 public:
  explicit Stream(const std::vector<Token>& tokens) : tokens_(tokens) {}
  explicit Stream(const ComponentValues& values) { Flatten(values, tokens_); tokens_.push_back(Token{}); }

  const Token& Peek(size_t ahead = 0) const { return pos_ + ahead < tokens_.size() ? tokens_[pos_ + ahead] : eof_; }
  const Token& Next() { return pos_ < tokens_.size() ? tokens_[pos_++] : eof_; }
  bool AtEnd() const { return Peek().type == T::EndOfFile; }
  size_t Mark() const { return pos_; }
  void Reset(size_t mark) { pos_ = mark; }
  void SkipWhitespace() {
    while (Peek().type == T::Whitespace) ++pos_;
  }

 private:
  // Component values back into the token stream they came from, for the algorithms that run over a block's contents.
  static void Flatten(const ComponentValues& values, std::vector<Token>& out) {
    for (const ComponentValue& value : values) {
      switch (value.kind) {
        case ComponentValue::Kind::Token: out.push_back(value.token); break;
        case ComponentValue::Kind::Function: {
          Token function;
          function.type = T::Function;
          function.value = value.name;
          out.push_back(function);
          Flatten(value.children, out);
          Token close;
          close.type = T::RightParen;
          out.push_back(close);
          break;
        }
        case ComponentValue::Kind::Block: {
          Token open;
          open.type = value.open;
          out.push_back(open);
          Flatten(value.children, out);
          Token close;
          close.type = ClosingOf(value.open);
          out.push_back(close);
          break;
        }
      }
    }
  }

  std::vector<Token> tokens_;
  size_t pos_ = 0;
  Token eof_;
};

// ---- Component values ----

ComponentValue ConsumeComponentValue(Stream& in);

ComponentValue ConsumeSimpleBlock(Stream& in) {
  ComponentValue block;
  block.kind = ComponentValue::Kind::Block;
  block.open = in.Next().type;
  const Token::Type closing = ClosingOf(block.open);
  for (;;) {
    const Token& next = in.Peek();
    if (next.type == T::EndOfFile) return block;
    if (next.type == closing) {
      in.Next();
      return block;
    }
    block.children.push_back(ConsumeComponentValue(in));
  }
}

ComponentValue ConsumeFunction(Stream& in) {
  ComponentValue function;
  function.kind = ComponentValue::Kind::Function;
  function.name = in.Next().value;
  for (;;) {
    const Token& next = in.Peek();
    if (next.type == T::EndOfFile) return function;
    if (next.type == T::RightParen) {
      in.Next();
      return function;
    }
    function.children.push_back(ConsumeComponentValue(in));
  }
}

ComponentValue ConsumeComponentValue(Stream& in) {
  const T type = in.Peek().type;
  if (type == T::LeftBrace || type == T::LeftBracket || type == T::LeftParen) return ConsumeSimpleBlock(in);
  if (type == T::Function) return ConsumeFunction(in);
  ComponentValue value;
  value.token = in.Next();
  return value;
}

// ---- Declarations ----

// "consume the remnants of a bad declaration".
void ConsumeBadDeclarationRemnants(Stream& in, bool nested) {
  for (;;) {
    const T type = in.Peek().type;
    if (type == T::EndOfFile) return;
    if (type == T::Semicolon) {
      in.Next();
      return;
    }
    if (type == T::RightBrace && nested) return;
    ConsumeComponentValue(in);
  }
}

bool IsCustomName(const std::string& name) { return StartsWith(name, "--"); }

// The text a run of component values was written as, for a custom property's value.
std::string TextOf(const ComponentValues& values) { return Serialize(values); }

// "consume a declaration": false (and the input spent to the end of the bad declaration) if it is not one.
bool ConsumeDeclaration(Stream& in, bool nested, Declaration& out) {
  if (in.Peek().type != T::Ident) {
    ConsumeBadDeclarationRemnants(in, nested);
    return false;
  }
  Declaration declaration;
  declaration.name = in.Next().value;
  in.SkipWhitespace();
  if (in.Peek().type != T::Colon) {
    ConsumeBadDeclarationRemnants(in, nested);
    return false;
  }
  in.Next();
  const bool spaceAfterColon = in.Peek().type == T::Whitespace;
  in.SkipWhitespace();
  // The value runs to the ; or, in a block, the }.
  for (;;) {
    const T type = in.Peek().type;
    if (type == T::EndOfFile || type == T::Semicolon || (nested && type == T::RightBrace)) break;
    declaration.value.push_back(ConsumeComponentValue(in));
  }
  if (in.Peek().type == T::Semicolon) in.Next();
  // Trailing whitespace goes, and then !important.
  ComponentValues& value = declaration.value;
  while (!value.empty() && value.back().IsWhitespace()) value.pop_back();
  {
    size_t end = value.size();
    // ! important, with whitespace allowed between and before.
    if (end >= 2 && value[end - 1].IsIdent()) {
      std::string word = value[end - 1].token.value;
      for (char& c : word) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (word == "important") {
        size_t bang = end - 1;
        while (bang > 0 && value[bang - 1].IsWhitespace()) --bang;
        if (bang > 0 && value[bang - 1].IsDelim('!')) {
          declaration.important = true;
          value.resize(bang - 1);
          while (!value.empty() && value.back().IsWhitespace()) value.pop_back();
        }
      }
    }
  }
  if (IsCustomName(declaration.name)) {
    // A custom property keeps its value as written, minus the whitespace at either end; an empty one is allowed, and one
    // of only whitespace is a space.
    declaration.originalText = TextOf(Trimmed(value));
    if (declaration.originalText.empty() && spaceAfterColon) declaration.originalText = " ";
  } else {
    // A {} block may be the whole value of a property, and nothing else.
    bool block = false, other = false;
    for (const ComponentValue& v : value) {
      if (v.IsBlock(T::LeftBrace)) block = true;
      else if (!v.IsWhitespace()) other = true;
    }
    if (block && other) return false;
    if (value.empty()) return false;
  }
  out = std::move(declaration);
  return true;
}

// ---- Rules ----

bool FirstTwoAreCustomProperty(const ComponentValues& prelude) {
  size_t i = 0;
  while (i < prelude.size() && prelude[i].IsWhitespace()) ++i;
  if (i >= prelude.size() || !prelude[i].IsIdent() || !StartsWith(prelude[i].token.value, "--")) return false;
  ++i;
  while (i < prelude.size() && prelude[i].IsWhitespace()) ++i;
  return i < prelude.size() && prelude[i].IsToken(T::Colon);
}

bool ConsumeAtRule(Stream& in, bool nested, Rule& out);
bool ConsumeQualifiedRule(Stream& in, bool nested, T stop, Rule& out);

// The component values of a {} block, which the rule keeps for the CSSOM to read as declarations and rules.
ComponentValues ConsumeBlockBody(Stream& in) {
  ComponentValue block = ConsumeSimpleBlock(in);
  return std::move(block.children);
}

bool ConsumeAtRule(Stream& in, bool nested, Rule& out) {
  Rule rule;
  rule.isAtRule = true;
  rule.name = in.Next().value;
  for (;;) {
    const T type = in.Peek().type;
    if (type == T::Semicolon || type == T::EndOfFile) {
      if (type == T::Semicolon) in.Next();
      break;
    }
    if (type == T::RightBrace) {
      if (nested) break;  // the } of the block this rule is in
      rule.prelude.push_back(ConsumeComponentValue(in));
      continue;
    }
    if (type == T::LeftBrace) {
      rule.block = ConsumeBlockBody(in);
      rule.hasBlock = true;
      break;
    }
    rule.prelude.push_back(ConsumeComponentValue(in));
  }
  out = std::move(rule);
  return true;
}

bool ConsumeQualifiedRule(Stream& in, bool nested, T stop, Rule& out) {
  Rule rule;
  for (;;) {
    const T type = in.Peek().type;
    if (type == T::EndOfFile || (stop != T::EndOfFile && type == stop)) return false;
    if (type == T::RightBrace) {
      if (nested) return false;
      rule.prelude.push_back(ConsumeComponentValue(in));
      continue;
    }
    if (type == T::LeftBrace) {
      if (FirstTwoAreCustomProperty(rule.prelude)) {
        if (nested) ConsumeBadDeclarationRemnants(in, true);
        else ConsumeSimpleBlock(in);
        return false;
      }
      rule.block = ConsumeBlockBody(in);
      rule.hasBlock = true;
      out = std::move(rule);
      return true;
    }
    rule.prelude.push_back(ConsumeComponentValue(in));
  }
}

}  // namespace

// ---- Public parsing ----

std::vector<Rule> ParseStylesheetContents(std::string_view text) {
  const std::vector<Token> tokens = Tokenize(text);
  Stream in(tokens);
  std::vector<Rule> rules;
  for (;;) {
    const T type = in.Peek().type;
    if (type == T::EndOfFile) break;
    if (type == T::Whitespace || type == T::Cdo || type == T::Cdc) {
      in.Next();
      continue;
    }
    Rule rule;
    if (type == T::AtKeyword) {
      if (ConsumeAtRule(in, false, rule)) rules.push_back(std::move(rule));
    } else if (ConsumeQualifiedRule(in, false, T::EndOfFile, rule)) {
      rules.push_back(std::move(rule));
    }
  }
  return rules;
}

namespace {

std::vector<Rule> ConsumeRuleList(Stream& in) {
  std::vector<Rule> rules;
  for (;;) {
    const T type = in.Peek().type;
    if (type == T::EndOfFile) break;
    if (type == T::Whitespace) {
      in.Next();
      continue;
    }
    Rule rule;
    if (type == T::AtKeyword) {
      if (ConsumeAtRule(in, false, rule)) rules.push_back(std::move(rule));
    } else if (ConsumeQualifiedRule(in, false, T::EndOfFile, rule)) {
      rules.push_back(std::move(rule));
    }
  }
  return rules;
}

}  // namespace

std::vector<Rule> ParseRuleList(std::string_view text) {
  const std::vector<Token> tokens = Tokenize(text);
  Stream in(tokens);
  return ConsumeRuleList(in);
}

std::vector<Rule> ParseRuleList(const ComponentValues& values) {
  Stream in(values);
  return ConsumeRuleList(in);
}

bool ParseRule(std::string_view text, Rule& out) {
  const std::vector<Token> tokens = Tokenize(text);
  Stream in(tokens);
  in.SkipWhitespace();
  if (in.AtEnd()) return false;
  Rule rule;
  if (in.Peek().type == T::AtKeyword) {
    // An at-rule that ends at end of input without its ; or block still parses as one.
    ConsumeAtRule(in, false, rule);
  } else if (!ConsumeQualifiedRule(in, false, T::EndOfFile, rule)) {
    return false;
  }
  in.SkipWhitespace();
  if (!in.AtEnd()) return false;
  out = std::move(rule);
  return true;
}

namespace {

std::vector<BlockItem> ConsumeBlockContents(Stream& in, bool topLevel = false) {
  std::vector<BlockItem> items;
  for (;;) {
    const T type = in.Peek().type;
    if (type == T::EndOfFile || (type == T::RightBrace && !topLevel)) return items;
    if (type == T::Whitespace || type == T::Semicolon) {
      in.Next();
      continue;
    }
    BlockItem item;
    if (type == T::AtKeyword) {
      if (ConsumeAtRule(in, true, item.rule)) {
        item.isDeclaration = false;
        items.push_back(std::move(item));
      }
      continue;
    }
    const size_t mark = in.Mark();
    if (ConsumeDeclaration(in, !topLevel, item.declaration)) {
      item.isDeclaration = true;
      items.push_back(std::move(item));
      continue;
    }
    // Not a declaration: a nested qualified rule, from the same place.
    in.Reset(mark);
    if (ConsumeQualifiedRule(in, !topLevel, T::Semicolon, item.rule)) {
      items.push_back(std::move(item));
    } else {
      // Whatever the rule left unread up to the ; is gone with it.
      ConsumeBadDeclarationRemnants(in, !topLevel);
    }
  }
}

}  // namespace

std::vector<BlockItem> ParseBlockContents(const ComponentValues& block) {
  Stream in(block);
  return ConsumeBlockContents(in);
}

std::vector<BlockItem> ParseBlockContents(std::string_view text) {
  const std::vector<Token> tokens = Tokenize(text);
  Stream in(tokens);
  // Not inside a block: a } is not the end of anything here.
  return ConsumeBlockContents(in, true);
}

bool ParseDeclaration(std::string_view text, Declaration& out) {
  const std::vector<Token> tokens = Tokenize(text);
  Stream in(tokens);
  in.SkipWhitespace();
  Declaration declaration;
  if (!ConsumeDeclaration(in, false, declaration)) return false;
  in.SkipWhitespace();
  if (!in.AtEnd()) return false;
  out = std::move(declaration);
  return true;
}

bool ParseComponentValue(std::string_view text, ComponentValue& out) {
  const std::vector<Token> tokens = Tokenize(text);
  Stream in(tokens);
  in.SkipWhitespace();
  if (in.AtEnd()) return false;
  ComponentValue value = ConsumeComponentValue(in);
  in.SkipWhitespace();
  if (!in.AtEnd()) return false;
  out = std::move(value);
  return true;
}

ComponentValues ParseComponentValues(const std::vector<Token>& tokens) {
  Stream in(tokens);
  ComponentValues values;
  while (!in.AtEnd()) values.push_back(ConsumeComponentValue(in));
  return values;
}

ComponentValues ParseComponentValues(std::string_view text) { return ParseComponentValues(Tokenize(text)); }

std::vector<ComponentValues> SplitOnCommas(const ComponentValues& values) {
  std::vector<ComponentValues> lists(1);
  for (const ComponentValue& value : values) {
    if (value.IsToken(T::Comma)) lists.emplace_back();
    else lists.back().push_back(value);
  }
  return lists;
}

ComponentValues Trimmed(const ComponentValues& values) {
  size_t begin = 0, end = values.size();
  while (begin < end && values[begin].IsWhitespace()) ++begin;
  while (end > begin && values[end - 1].IsWhitespace()) --end;
  return ComponentValues(values.begin() + begin, values.begin() + end);
}

// ---- Serialization ----

std::string SerializeIdentifier(std::string_view text) { return EscapeIdentifier(text); }

std::string SerializeString(std::string_view text) {
  std::string out = "\"";
  size_t i = 0;
  while (i < text.size()) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c == 0) {
      out += "\xEF\xBF\xBD";
    } else if ((c >= 1 && c <= 0x1F) || c == 0x7F) {
      char buffer[16];
      std::snprintf(buffer, sizeof(buffer), "\\%x ", static_cast<unsigned>(c));
      out += buffer;
    } else if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else {
      out += static_cast<char>(c);
    }
    ++i;
  }
  return out + "\"";
}

std::string SerializeUrl(std::string_view text) { return "url(" + SerializeString(text) + ")"; }

namespace {

std::string NumberText(const Token& token) {
  if (!token.representation.empty()) return token.representation;
  char buffer[64];
  if (token.isInteger) std::snprintf(buffer, sizeof(buffer), "%.0f", token.number);
  else std::snprintf(buffer, sizeof(buffer), "%.17g", token.number);
  return buffer;
}

// A hash's name is escaped as an identifier would be, except that it may begin with a digit.
std::string HashName(const Token& token) {
  if (token.hashIsId) return SerializeIdentifier(token.value);
  std::string out;
  for (char32_t c : [&] {
         std::u32string points;
         size_t i = 0;
         const std::string& s = token.value;
         while (i < s.size()) {
           const unsigned char lead = static_cast<unsigned char>(s[i]);
           char32_t point = lead;
           int length = 1;
           if (lead >= 0xF0) { point = lead & 0x07; length = 4; }
           else if (lead >= 0xE0) { point = lead & 0x0F; length = 3; }
           else if (lead >= 0xC0) { point = lead & 0x1F; length = 2; }
           for (int k = 1; k < length && i + k < s.size(); ++k) point = (point << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
           points.push_back(point);
           i += length;
         }
         return points;
       }()) {
    if (c >= 0x80 || c == '-' || c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
      AppendUtf8(out, c);
    } else if (c == 0 || c < 0x20 || c == 0x7F) {
      char buffer[16];
      std::snprintf(buffer, sizeof(buffer), "\\%x ", static_cast<unsigned>(c ? c : 0xFFFD));
      out += buffer;
    } else {
      out += '\\';
      AppendUtf8(out, c);
    }
  }
  return out;
}

}  // namespace

std::string SerializeToken(const Token& token) {
  switch (token.type) {
    case T::Ident: return SerializeIdentifier(token.value);
    case T::Function: return SerializeIdentifier(token.value) + "(";
    case T::AtKeyword: return "@" + SerializeIdentifier(token.value);
    case T::Hash: return "#" + HashName(token);
    case T::String: return SerializeString(token.value);
    case T::BadString: return "\"" + token.value + "\n";
    case T::Url: {
      // As written: url(address), with what would end the token escaped.
      std::string out = "url(";
      for (char c : token.value) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\'' || c == '(' || c == ')' || c == '\\' || c == ' ' || c == '\t' || c == '\n') {
          out += '\\';
          out += c;
        } else if (u < 0x20 || u == 0x7F) {
          char buffer[16];
          std::snprintf(buffer, sizeof(buffer), "\\%x ", static_cast<unsigned>(u));
          out += buffer;
        } else {
          out += c;
        }
      }
      return out + ")";
    }
    case T::BadUrl: return "url(" + token.value + "\"\x27)";  // a quote inside makes it a bad url again
    case T::Delim: {
      std::string out;
      AppendUtf8(out, token.delim);
      return out;
    }
    case T::Number: return NumberText(token);
    case T::Percentage: return NumberText(token) + "%";
    case T::Dimension: return NumberText(token) + SerializeIdentifier(token.value);
    case T::Whitespace: return " ";
    case T::Cdo: return "<!--";
    case T::Cdc: return "-->";
    case T::Colon: return ":";
    case T::Semicolon: return ";";
    case T::Comma: return ",";
    case T::LeftBracket: return "[";
    case T::RightBracket: return "]";
    case T::LeftParen: return "(";
    case T::RightParen: return ")";
    case T::LeftBrace: return "{";
    case T::RightBrace: return "}";
    case T::EndOfFile: return "";
  }
  return "";
}

namespace {

// Whether two adjacent tokens would be read back as something else without a comment between them.
bool NeedsComment(const Token& a, const Token& b) {
  const auto is = [](const Token& t, T type) { return t.type == type; };
  const auto delim = [](const Token& t, char32_t c) { return t.IsDelim(c); };
  const bool bIdentLike = is(b, T::Ident) || is(b, T::Function) || is(b, T::Url) || is(b, T::BadUrl);
  const bool bNumeric = is(b, T::Number) || is(b, T::Percentage) || is(b, T::Dimension);
  if (is(a, T::Ident)) return bIdentLike || delim(b, '-') || bNumeric || is(b, T::Cdc) || is(b, T::LeftParen);
  if (is(a, T::AtKeyword) || is(a, T::Hash) || is(a, T::Dimension)) return bIdentLike || delim(b, '-') || bNumeric || is(b, T::Cdc);
  if (is(a, T::Number)) return bIdentLike || bNumeric || delim(b, '%');
  if (delim(a, '#') || delim(a, '-')) return bIdentLike || delim(b, '-') || bNumeric;
  if (delim(a, '@')) return bIdentLike || delim(b, '-');
  if (delim(a, '.') || delim(a, '+')) return bNumeric;
  if (delim(a, '/')) return delim(b, '*');
  return false;
}

void Flatten(const ComponentValues& values, std::vector<Token>& out) {
  for (const ComponentValue& value : values) {
    switch (value.kind) {
      case ComponentValue::Kind::Token: out.push_back(value.token); break;
      case ComponentValue::Kind::Function: {
        Token function;
        function.type = T::Function;
        function.value = value.name;
        out.push_back(function);
        Flatten(value.children, out);
        Token close;
        close.type = T::RightParen;
        out.push_back(close);
        break;
      }
      case ComponentValue::Kind::Block: {
        Token open;
        open.type = value.open;
        out.push_back(open);
        Flatten(value.children, out);
        Token close;
        close.type = ClosingOf(value.open);
        out.push_back(close);
        break;
      }
    }
  }
}

}  // namespace

std::string Serialize(const ComponentValues& values) {
  std::vector<Token> tokens;
  Flatten(values, tokens);
  std::string out;
  for (size_t i = 0; i < tokens.size(); ++i) {
    if (i > 0 && NeedsComment(tokens[i - 1], tokens[i])) out += "/**/";
    out += SerializeToken(tokens[i]);
  }
  return out;
}

std::string SerializeTokens(const std::vector<Token>& tokens) { return Serialize(ParseComponentValues(tokens)); }

std::string Serialize(const ComponentValue& value) { return Serialize(ComponentValues{value}); }

}  // namespace solar::css
