#include "solar/css/Tokenizer.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace solar::css {

void AppendUtf8(std::string& out, char32_t c) {
  if (c < 0x80) {
    out.push_back(static_cast<char>(c));
  } else if (c < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (c >> 6)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  } else if (c < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (c >> 12)));
    out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (c >> 18)));
    out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  }
}

namespace {

constexpr char32_t kEof = 0xFFFFFFFF;

bool IsDigit(char32_t c) { return c >= '0' && c <= '9'; }
bool IsHexDigit(char32_t c) { return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool IsNewline(char32_t c) { return c == '\n'; }
bool IsWhitespace(char32_t c) { return c == '\n' || c == '\t' || c == ' '; }
// The end of the input is not a code point, whatever its value would say.
bool IsIdentStart(char32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= 0x80 && c != kEof) || c == '_'; }
bool IsIdent(char32_t c) { return IsIdentStart(c) || IsDigit(c) || c == '-'; }
bool IsNonPrintable(char32_t c) { return c <= 0x08 || c == 0x0B || (c >= 0x0E && c <= 0x1F) || c == 0x7F; }

// UTF-8 to code points, with the preprocessing of the input stream: CR, CRLF and FF are LF, NUL is U+FFFD.
std::u32string Preprocess(std::string_view text) {
  std::u32string out;
  out.reserve(text.size());
  size_t i = 0;
  while (i < text.size()) {
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    char32_t point;
    int length = 1;
    if (lead < 0x80) point = lead;
    else if (lead >= 0xF0 && lead <= 0xF4) { point = lead & 0x07; length = 4; }
    else if (lead >= 0xE0) { point = lead & 0x0F; length = 3; }
    else if (lead >= 0xC2) { point = lead & 0x1F; length = 2; }
    else { point = 0xFFFD; }
    if (length > 1) {
      bool ok = i + length <= text.size();
      for (int k = 1; ok && k < length; ++k) {
        const unsigned char next = static_cast<unsigned char>(text[i + k]);
        if ((next & 0xC0) != 0x80) ok = false;
        else point = (point << 6) | (next & 0x3F);
      }
      if (!ok) {
        point = 0xFFFD;
        length = 1;
      } else if ((point >= 0xD800 && point <= 0xDFFF) || point > 0x10FFFF) {
        point = 0xFFFD;
      }
    }
    i += length;
    if (point == '\r') {
      out.push_back('\n');
      if (i < text.size() && text[i] == '\n') ++i;
    } else if (point == '\f') {
      out.push_back('\n');
    } else if (point == 0) {
      out.push_back(0xFFFD);
    } else {
      out.push_back(point);
    }
  }
  return out;
}

class Lexer {
 public:
  explicit Lexer(std::u32string input) : input_(std::move(input)) {}

  std::vector<Token> Run() {
    std::vector<Token> tokens;
    for (;;) {
      Token token = ConsumeToken();
      const bool end = token.type == Token::Type::EndOfFile;
      tokens.push_back(std::move(token));
      if (end) break;
    }
    return tokens;
  }

 private:
  char32_t Peek(size_t ahead = 0) const { return pos_ + ahead < input_.size() ? input_[pos_ + ahead] : kEof; }
  char32_t Consume() { return pos_ < input_.size() ? input_[pos_++] : kEof; }

  static bool ValidEscape(char32_t first, char32_t second) { return first == '\\' && !IsNewline(second); }
  bool StartsValidEscape(size_t at = 0) const { return ValidEscape(Peek(at), Peek(at + 1)); }

  bool StartsIdentifier(size_t at = 0) const {
    const char32_t first = Peek(at);
    if (first == '-') {
      const char32_t second = Peek(at + 1);
      return IsIdentStart(second) || second == '-' || ValidEscape(second, Peek(at + 2));
    }
    if (IsIdentStart(first)) return true;
    return ValidEscape(first, Peek(at + 1));
  }

  bool StartsNumber(size_t at = 0) const {
    const char32_t first = Peek(at);
    if (first == '+' || first == '-') {
      if (IsDigit(Peek(at + 1))) return true;
      return Peek(at + 1) == '.' && IsDigit(Peek(at + 2));
    }
    if (first == '.') return IsDigit(Peek(at + 1));
    return IsDigit(first);
  }

  void SkipComments() {
    while (Peek() == '/' && Peek(1) == '*') {
      pos_ += 2;
      while (pos_ < input_.size() && !(Peek() == '*' && Peek(1) == '/')) ++pos_;
      pos_ = pos_ < input_.size() ? pos_ + 2 : input_.size();
    }
  }

  char32_t ConsumeEscape() {
    // After the backslash.
    const char32_t c = Consume();
    if (c == kEof) return 0xFFFD;
    if (IsHexDigit(c)) {
      char32_t value = 0;
      int digits = 0;
      pos_--;
      while (digits < 6 && IsHexDigit(Peek())) {
        const char32_t d = Consume();
        value = value * 16 + (IsDigit(d) ? d - '0' : (d | 0x20) - 'a' + 10);
        ++digits;
      }
      if (IsWhitespace(Peek())) Consume();
      if (value == 0 || (value >= 0xD800 && value <= 0xDFFF) || value > 0x10FFFF) return 0xFFFD;
      return value;
    }
    return c;
  }

  std::string ConsumeIdentSequence() {
    std::string result;
    for (;;) {
      const char32_t c = Peek();
      if (IsIdent(c)) {
        AppendUtf8(result, Consume());
      } else if (StartsValidEscape()) {
        Consume();
        AppendUtf8(result, ConsumeEscape());
      } else {
        return result;
      }
    }
  }

  Token ConsumeNumeric() {
    Token token;
    const size_t start = pos_;
    bool integer = true;
    bool sign = false;
    if (Peek() == '+' || Peek() == '-') {
      sign = true;
      Consume();
    }
    while (IsDigit(Peek())) Consume();
    if (Peek() == '.' && IsDigit(Peek(1))) {
      Consume();
      integer = false;
      while (IsDigit(Peek())) Consume();
    }
    if ((Peek() == 'e' || Peek() == 'E') && (IsDigit(Peek(1)) || ((Peek(1) == '+' || Peek(1) == '-') && IsDigit(Peek(2))))) {
      Consume();
      integer = false;
      if (Peek() == '+' || Peek() == '-') Consume();
      while (IsDigit(Peek())) Consume();
    }
    std::string text;
    for (size_t i = start; i < pos_; ++i) AppendUtf8(text, input_[i]);
    token.number = std::strtod(text.c_str(), nullptr);
    token.isInteger = integer;
    token.hasSign = sign;
    token.representation = text;
    if (StartsIdentifier()) {
      token.type = Token::Type::Dimension;
      token.value = ConsumeIdentSequence();
    } else if (Peek() == '%') {
      Consume();
      token.type = Token::Type::Percentage;
    } else {
      token.type = Token::Type::Number;
    }
    return token;
  }

  Token ConsumeString(char32_t ending) {
    Token token;
    token.type = Token::Type::String;
    for (;;) {
      const char32_t c = Consume();
      if (c == ending || c == kEof) return token;
      if (IsNewline(c)) {
        --pos_;
        token.type = Token::Type::BadString;
        return token;
      }
      if (c == '\\') {
        if (Peek() == kEof) continue;
        if (IsNewline(Peek())) {
          Consume();
          continue;
        }
        AppendUtf8(token.value, ConsumeEscape());
      } else {
        AppendUtf8(token.value, c);
      }
    }
  }

  void ConsumeBadUrlRemnants() {
    for (;;) {
      const char32_t c = Consume();
      if (c == ')' || c == kEof) return;
      if (ValidEscape(c, Peek())) {
        Consume();
        ConsumeEscape();
      }
    }
  }

  Token ConsumeUrl() {
    Token token;
    token.type = Token::Type::Url;
    while (IsWhitespace(Peek())) Consume();
    for (;;) {
      const char32_t c = Consume();
      if (c == ')' || c == kEof) return token;
      if (IsWhitespace(c)) {
        while (IsWhitespace(Peek())) Consume();
        if (Peek() == ')' || Peek() == kEof) {
          Consume();
          return token;
        }
        ConsumeBadUrlRemnants();
        token.type = Token::Type::BadUrl;
        return token;
      }
      if (c == '"' || c == '\'' || c == '(' || IsNonPrintable(c)) {
        ConsumeBadUrlRemnants();
        token.type = Token::Type::BadUrl;
        return token;
      }
      if (c == '\\') {
        if (ValidEscape(c, Peek())) {
          AppendUtf8(token.value, ConsumeEscape());
        } else {
          ConsumeBadUrlRemnants();
          token.type = Token::Type::BadUrl;
          return token;
        }
      } else {
        AppendUtf8(token.value, c);
      }
    }
  }

  Token ConsumeIdentLike() {
    Token token;
    token.value = ConsumeIdentSequence();
    std::string lowered = token.value;
    for (char& c : lowered) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 0x20);
    }
    if (lowered == "url" && Peek() == '(') {
      Consume();
      // Whitespace before a quote is part of a function token; otherwise it is a url's.
      size_t ahead = 0;
      while (IsWhitespace(Peek(ahead))) ++ahead;
      if (Peek(ahead) == '"' || Peek(ahead) == '\'') {
        token.type = Token::Type::Function;
        return token;
      }
      return ConsumeUrl();
    }
    if (Peek() == '(') {
      Consume();
      token.type = Token::Type::Function;
      return token;
    }
    token.type = Token::Type::Ident;
    return token;
  }

  Token Delim(char32_t c) {
    Token token;
    token.type = Token::Type::Delim;
    token.delim = c;
    return token;
  }

  Token Simple(Token::Type type) {
    Token token;
    token.type = type;
    return token;
  }

  Token ConsumeToken() {
    SkipComments();
    const char32_t c = Consume();
    if (c == kEof) return Simple(Token::Type::EndOfFile);
    if (IsWhitespace(c)) {
      while (IsWhitespace(Peek())) Consume();
      return Simple(Token::Type::Whitespace);
    }
    switch (c) {
      case '"':
      case '\'':
        return ConsumeString(c);
      case '#': {
        if (IsIdent(Peek()) || StartsValidEscape()) {
          Token token;
          token.type = Token::Type::Hash;
          token.hashIsId = StartsIdentifier();
          token.value = ConsumeIdentSequence();
          return token;
        }
        return Delim(c);
      }
      case '(': return Simple(Token::Type::LeftParen);
      case ')': return Simple(Token::Type::RightParen);
      case ',': return Simple(Token::Type::Comma);
      case ':': return Simple(Token::Type::Colon);
      case ';': return Simple(Token::Type::Semicolon);
      case '[': return Simple(Token::Type::LeftBracket);
      case ']': return Simple(Token::Type::RightBracket);
      case '{': return Simple(Token::Type::LeftBrace);
      case '}': return Simple(Token::Type::RightBrace);
      case '+':
        --pos_;
        if (StartsNumber()) return ConsumeNumeric();
        ++pos_;
        return Delim(c);
      case '-':
        --pos_;
        if (StartsNumber()) return ConsumeNumeric();
        ++pos_;
        if (Peek() == '-' && Peek(1) == '>') {
          pos_ += 2;
          return Simple(Token::Type::Cdc);
        }
        --pos_;
        if (StartsIdentifier()) return ConsumeIdentLike();
        ++pos_;
        return Delim(c);
      case '.':
        --pos_;
        if (StartsNumber()) return ConsumeNumeric();
        ++pos_;
        return Delim(c);
      case '<':
        if (Peek() == '!' && Peek(1) == '-' && Peek(2) == '-') {
          pos_ += 3;
          return Simple(Token::Type::Cdo);
        }
        return Delim(c);
      case '@': {
        if (StartsIdentifier()) {
          Token token;
          token.type = Token::Type::AtKeyword;
          token.value = ConsumeIdentSequence();
          return token;
        }
        return Delim(c);
      }
      case '\\':
        if (Peek() != kEof && !IsNewline(Peek())) {
          --pos_;
          return ConsumeIdentLike();
        }
        return Delim(c);
      default:
        break;
    }
    if (IsDigit(c)) {
      --pos_;
      return ConsumeNumeric();
    }
    if (IsIdentStart(c)) {
      --pos_;
      return ConsumeIdentLike();
    }
    return Delim(c);
  }

  std::u32string input_;
  size_t pos_ = 0;
};

}  // namespace

std::vector<Token> Tokenize(std::string_view text) { return Lexer(Preprocess(text)).Run(); }

std::string EscapeIdentifier(std::string_view text) {
  // CSS.escape, from the CSSOM.
  std::string out;
  const std::u32string points = Preprocess(text);
  // Preprocess maps NUL, which CSS.escape wants as U+FFFD too.
  for (size_t i = 0; i < points.size(); ++i) {
    const char32_t c = points[i];
    if ((c >= 0x1 && c <= 0x1F) || c == 0x7F) {
      char buffer[16];
      std::snprintf(buffer, sizeof(buffer), "\\%x ", static_cast<unsigned>(c));
      out += buffer;
    } else if (i == 0 && IsDigit(c)) {
      char buffer[16];
      std::snprintf(buffer, sizeof(buffer), "\\%x ", static_cast<unsigned>(c));
      out += buffer;
    } else if (i == 1 && IsDigit(c) && points[0] == '-') {
      char buffer[16];
      std::snprintf(buffer, sizeof(buffer), "\\%x ", static_cast<unsigned>(c));
      out += buffer;
    } else if (i == 0 && c == '-' && points.size() == 1) {
      out += "\\-";
    } else if (c >= 0x80 || c == '-' || c == '_' || IsDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
      AppendUtf8(out, c);
    } else {
      out += '\\';
      AppendUtf8(out, c);
    }
  }
  return out;
}

}  // namespace solar::css
