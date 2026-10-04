#include "solar/html/Tokenizer.h"

#include <algorithm>

#include "solar/html/Entities.h"

namespace solar::html {

namespace {

bool IsWhitespace(char32_t c) { return c == '\t' || c == '\n' || c == '\f' || c == ' '; }
bool IsUpperAlpha(char32_t c) { return c >= 'A' && c <= 'Z'; }
bool IsLowerAlpha(char32_t c) { return c >= 'a' && c <= 'z'; }
bool IsAlpha(char32_t c) { return IsUpperAlpha(c) || IsLowerAlpha(c); }
bool IsDigit(char32_t c) { return c >= '0' && c <= '9'; }
bool IsHexDigit(char32_t c) { return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool IsAlnum(char32_t c) { return IsAlpha(c) || IsDigit(c); }
char32_t Lower(char32_t c) { return IsUpperAlpha(c) ? c + 0x20 : c; }
bool IsNoncharacter(char32_t c) { return (c >= 0xFDD0 && c <= 0xFDEF) || (c & 0xFFFE) == 0xFFFE; }

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

// UTF-8 to code points; what is malformed is U+FFFD, one for each maximal subpart as the Encoding Standard has it.
std::u32string DecodeUtf8(std::string_view text) {
  std::u32string out;
  out.reserve(text.size());
  size_t i = 0;
  while (i < text.size()) {
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    if (lead < 0x80) {
      out.push_back(lead);
      ++i;
      continue;
    }
    int needed = 0;
    char32_t point = 0;
    unsigned char lower = 0x80, upper = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) { needed = 1; point = lead & 0x1F; }
    else if (lead >= 0xE0 && lead <= 0xEF) {
      needed = 2;
      point = lead & 0x0F;
      if (lead == 0xE0) lower = 0xA0;
      if (lead == 0xED) upper = 0x9F;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
      needed = 3;
      point = lead & 0x07;
      if (lead == 0xF0) lower = 0x90;
      if (lead == 0xF4) upper = 0x8F;
    } else {
      out.push_back(0xFFFD);
      ++i;
      continue;
    }
    ++i;
    bool ok = true;
    for (int n = 0; n < needed; ++n) {
      if (i >= text.size() || static_cast<unsigned char>(text[i]) < lower || static_cast<unsigned char>(text[i]) > upper) {
        ok = false;
        break;
      }
      point = (point << 6) | (static_cast<unsigned char>(text[i]) & 0x3F);
      lower = 0x80;
      upper = 0xBF;
      ++i;
    }
    out.push_back(ok ? point : 0xFFFD);
  }
  return out;
}

// Code points of an UTF-8 string, which is what the temporary buffer is.
template <typename Fn>
void ForEachCodePoint(const std::string& text, Fn&& fn) {
  for (char32_t c : DecodeUtf8(text)) fn(c);
}

}  // namespace

Tokenizer::Tokenizer(std::string_view markup) {
  const std::u32string decoded = DecodeUtf8(markup);
  input_.reserve(decoded.size());
  // Newlines: CR LF and a lone CR are both a LF.
  for (size_t i = 0; i < decoded.size(); ++i) {
    if (decoded[i] == '\r') {
      input_.push_back('\n');
      if (i + 1 < decoded.size() && decoded[i + 1] == '\n') ++i;
    } else {
      input_.push_back(decoded[i]);
    }
  }
}

char32_t Tokenizer::Consume() {
  if (pos_ >= input_.size()) {
    ++pos_;
    return kEof;
  }
  CheckNextInput();
  const char32_t c = input_[pos_];
  ++pos_;
  return c;
}

bool Tokenizer::NextIs(std::string_view text, bool ignoreCase) {
  for (size_t i = 0; i < text.size(); ++i) {
    char32_t c = Peek(i);
    if (c == kEof) return false;
    if (ignoreCase) c = Lower(c);
    if (c != static_cast<unsigned char>(text[i])) return false;
  }
  return true;
}

Token Tokenizer::Next() {
  while (queue_.empty() && !finished_) Step();
  if (queue_.empty()) {
    Token eof;
    eof.type = Token::Type::EndOfFile;
    return eof;
  }
  Token token = std::move(queue_.front());
  queue_.pop_front();
  return token;
}

// ---- Emitting ----

void Tokenizer::EmitCharacter(char32_t c) {
  Token token;
  token.type = Token::Type::Character;
  AppendUtf8(token.data, c);
  queue_.push_back(std::move(token));
}

void Tokenizer::EmitEof() {
  Token token;
  token.type = Token::Type::EndOfFile;
  queue_.push_back(std::move(token));
  finished_ = true;
}

void Tokenizer::EmitCurrent() {
  CommitAttribute();
  if (current_.type == Token::Type::EndTag) {
    if (!current_.attributes.empty()) Error("end-tag-with-attributes");
    if (current_.selfClosing) Error("end-tag-with-trailing-solidus");
  } else {
    lastStartTag_ = current_.name;
  }
  queue_.push_back(std::move(current_));
  current_ = Token();
}

void Tokenizer::EmitComment() {
  queue_.push_back(std::move(current_));
  current_ = Token();
}

void Tokenizer::EmitDoctype() {
  queue_.push_back(std::move(current_));
  current_ = Token();
}

void Tokenizer::StartTag(Token::Type type) {
  current_ = Token();
  current_.type = type;
  attributeActive_ = false;
}

void Tokenizer::StartAttribute() {
  CommitAttribute();
  attribute_ = TokenAttribute();
  attributeActive_ = true;
  attributeDropped_ = false;
}

void Tokenizer::FinishAttributeName() {
  for (const TokenAttribute& other : current_.attributes) {
    if (other.name == attribute_.name) {
      Error("duplicate-attribute");
      attributeDropped_ = true;
      return;
    }
  }
}

void Tokenizer::CommitAttribute() {
  if (attributeActive_ && !attributeDropped_) current_.attributes.push_back(std::move(attribute_));
  attributeActive_ = false;
  attributeDropped_ = false;
  attribute_ = TokenAttribute();
}

void Tokenizer::AppendToTagName(char32_t c) { AppendUtf8(current_.name, c); }
void Tokenizer::AppendToAttributeName(char32_t c) { AppendUtf8(attribute_.name, c); }
void Tokenizer::AppendToAttributeValue(char32_t c) { AppendUtf8(attribute_.value, c); }

void Tokenizer::FlushReference() {
  ForEachCodePoint(temporary_, [this](char32_t c) {
    if (InAttribute()) AppendToAttributeValue(c);
    else EmitCharacter(c);
  });
}

void Tokenizer::EmitTemporary() {
  ForEachCodePoint(temporary_, [this](char32_t c) { EmitCharacter(c); });
}

// ---- Character references ----

void Tokenizer::NamedReference() {
  // The longest name of the table that the input begins with, with its ";" if it has one.
  std::string prefix;
  const NamedEntity* match = nullptr;
  size_t matchLength = 0;
  for (size_t i = 0;; ++i) {
    const char32_t c = Peek(i);
    if (c > 0x7F) break;
    prefix.push_back(static_cast<char>(c));
    const auto [first, last] = EntitiesWithPrefix(prefix);
    if (first == last) break;
    if (std::string_view(kNamedEntities[first].name) == prefix) {
      match = &kNamedEntities[first];
      matchLength = i + 1;
    }
  }
  if (!match) {
    temporary_ = "&";
    FlushReference();
    state_ = State::AmbiguousAmpersand;
    return;
  }
  pos_ += matchLength;
  const std::string name = match->name;
  const char32_t next = Peek();
  if (InAttribute() && name.back() != ';' && (next == '=' || IsAlnum(next))) {
    // For historical reasons a reference without its ";" is left alone in an attribute when more of a name or a value follows.
    temporary_ = "&" + name;
    FlushReference();
    state_ = returnState_;
    return;
  }
  if (name.back() != ';') Error("missing-semicolon-after-character-reference");
  temporary_.clear();
  AppendUtf8(temporary_, match->first);
  if (match->second) AppendUtf8(temporary_, match->second);
  FlushReference();
  state_ = returnState_;
}

void Tokenizer::ReferenceEnd() {
  uint32_t code = referenceCode_;
  if (code == 0) {
    Error("null-character-reference");
    code = 0xFFFD;
  } else if (code > 0x10FFFF) {
    Error("character-reference-outside-unicode-range");
    code = 0xFFFD;
  } else if (code >= 0xD800 && code <= 0xDFFF) {
    Error("surrogate-character-reference");
    code = 0xFFFD;
  } else if (IsNoncharacter(code)) {
    Error("noncharacter-character-reference");
  } else if (code == 0x0D || (code >= 0x80 && code <= 0x9F) || (code < 0x20 && !IsWhitespace(code)) || code == 0x7F) {
    Error("control-character-reference");
    static const uint32_t kWindows1252[32] = {0x20AC, 0x81,   0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                              0x2039, 0x0152, 0x8D,   0x017D, 0x8F,   0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                              0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D,   0x017E, 0x0178};
    if (code >= 0x80 && code <= 0x9F) code = kWindows1252[code - 0x80];
  }
  temporary_.clear();
  AppendUtf8(temporary_, code);
  FlushReference();
  state_ = returnState_;
}

// ---- The states ----

void Tokenizer::CheckNextInput() {
  if (pos_ >= input_.size() || pos_ < reported_) return;
  reported_ = pos_ + 1;
  const char32_t c = input_[pos_];
  if ((c < 0x20 && c != 0 && !IsWhitespace(c) && c != '\r') || (c >= 0x7F && c <= 0x9F)) Error("control-character-in-input-stream");
  else if (IsNoncharacter(c)) Error("noncharacter-in-input-stream");
}

void Tokenizer::ConvertTemporaryToComment() {
  current_ = Token();
  current_.type = Token::Type::Comment;
  current_.data = "?" + temporary_;
}

void Tokenizer::Step() {
  using S = State;
  switch (state_) {
    case S::Data: {
      const char32_t c = Consume();
      if (c == '&') { returnState_ = S::Data; state_ = S::CharacterReference; }
      else if (c == '<') state_ = S::TagOpen;
      else if (c == 0) { Error("unexpected-null-character"); EmitCharacter(c); }
      else if (c == kEof) EmitEof();
      else EmitCharacter(c);
      break;
    }
    case S::Rcdata: {
      const char32_t c = Consume();
      if (c == '&') { returnState_ = S::Rcdata; state_ = S::CharacterReference; }
      else if (c == '<') state_ = S::RcdataLessThanSign;
      else if (c == 0) { Error("unexpected-null-character"); EmitCharacter(0xFFFD); }
      else if (c == kEof) EmitEof();
      else EmitCharacter(c);
      break;
    }
    case S::Rawtext: {
      const char32_t c = Consume();
      if (c == '<') state_ = S::RawtextLessThanSign;
      else if (c == 0) { Error("unexpected-null-character"); EmitCharacter(0xFFFD); }
      else if (c == kEof) EmitEof();
      else EmitCharacter(c);
      break;
    }
    case S::ScriptData: {
      const char32_t c = Consume();
      if (c == '<') state_ = S::ScriptDataLessThanSign;
      else if (c == 0) { Error("unexpected-null-character"); EmitCharacter(0xFFFD); }
      else if (c == kEof) EmitEof();
      else EmitCharacter(c);
      break;
    }
    case S::Plaintext: {
      const char32_t c = Consume();
      if (c == 0) { Error("unexpected-null-character"); EmitCharacter(0xFFFD); }
      else if (c == kEof) EmitEof();
      else EmitCharacter(c);
      break;
    }
    case S::TagOpen: {
      const char32_t c = Consume();
      if (c == '!') state_ = S::MarkupDeclarationOpen;
      else if (c == '/') state_ = S::EndTagOpen;
      else if (IsAlpha(c)) { StartTag(Token::Type::StartTag); Reconsume(); state_ = S::TagName; }
      else if (c == '?') { temporary_.clear(); state_ = S::ProcessingInstructionOpen; }
      else if (c == kEof) { Error("eof-before-tag-name"); EmitCharacter('<'); EmitEof(); }
      else { Error("invalid-first-character-of-tag-name"); EmitCharacter('<'); Reconsume(); state_ = S::Data; }
      break;
    }
    case S::EndTagOpen: {
      const char32_t c = Consume();
      if (IsAlpha(c)) { StartTag(Token::Type::EndTag); Reconsume(); state_ = S::TagName; }
      else if (c == '>') { Error("missing-end-tag-name"); state_ = S::Data; }
      else if (c == kEof) { Error("eof-before-tag-name"); EmitCharacter('<'); EmitCharacter('/'); EmitEof(); }
      else {
        Error("invalid-first-character-of-tag-name");
        current_ = Token();
        current_.type = Token::Type::Comment;
        Reconsume();
        state_ = S::BogusComment;
      }
      break;
    }
    case S::TagName: {
      const char32_t c = Consume();
      if (IsWhitespace(c)) state_ = S::BeforeAttributeName;
      else if (c == '/') state_ = S::SelfClosingStartTag;
      else if (c == '>') { state_ = S::Data; EmitCurrent(); }
      else if (IsUpperAlpha(c)) AppendToTagName(Lower(c));
      else if (c == 0) { Error("unexpected-null-character"); AppendToTagName(0xFFFD); }
      else if (c == kEof) { Error("eof-in-tag"); EmitEof(); }
      else AppendToTagName(c);
      break;
    }

    // ---- RCDATA, RAWTEXT and script data: the less-than signs and the end tags that may close them ----
    case S::RcdataLessThanSign:
    case S::RawtextLessThanSign: {
      const bool rcdata = state_ == S::RcdataLessThanSign;
      const char32_t c = Consume();
      if (c == '/') { temporary_.clear(); state_ = rcdata ? S::RcdataEndTagOpen : S::RawtextEndTagOpen; }
      else { EmitCharacter('<'); Reconsume(); state_ = rcdata ? S::Rcdata : S::Rawtext; }
      break;
    }
    case S::RcdataEndTagOpen:
    case S::RawtextEndTagOpen:
    case S::ScriptDataEndTagOpen:
    case S::ScriptDataEscapedEndTagOpen: {
      const char32_t c = Consume();
      State name = S::RcdataEndTagName, text = S::Rcdata;
      if (state_ == S::RawtextEndTagOpen) { name = S::RawtextEndTagName; text = S::Rawtext; }
      if (state_ == S::ScriptDataEndTagOpen) { name = S::ScriptDataEndTagName; text = S::ScriptData; }
      if (state_ == S::ScriptDataEscapedEndTagOpen) { name = S::ScriptDataEscapedEndTagName; text = S::ScriptDataEscaped; }
      if (IsAlpha(c)) { StartTag(Token::Type::EndTag); Reconsume(); state_ = name; }
      else { EmitCharacter('<'); EmitCharacter('/'); Reconsume(); state_ = text; }
      break;
    }
    case S::RcdataEndTagName:
    case S::RawtextEndTagName:
    case S::ScriptDataEndTagName:
    case S::ScriptDataEscapedEndTagName: {
      State text = S::Rcdata;
      if (state_ == S::RawtextEndTagName) text = S::Rawtext;
      if (state_ == S::ScriptDataEndTagName) text = S::ScriptData;
      if (state_ == S::ScriptDataEscapedEndTagName) text = S::ScriptDataEscaped;
      const char32_t c = Consume();
      const auto anythingElse = [&] {
        EmitCharacter('<');
        EmitCharacter('/');
        EmitTemporary();
        Reconsume();
        state_ = text;
      };
      if (IsWhitespace(c)) { if (AppropriateEndTag()) state_ = S::BeforeAttributeName; else anythingElse(); }
      else if (c == '/') { if (AppropriateEndTag()) state_ = S::SelfClosingStartTag; else anythingElse(); }
      else if (c == '>') { if (AppropriateEndTag()) { state_ = S::Data; EmitCurrent(); } else anythingElse(); }
      else if (IsUpperAlpha(c)) { AppendToTagName(Lower(c)); AppendUtf8(temporary_, c); }
      else if (IsLowerAlpha(c)) { AppendToTagName(c); AppendUtf8(temporary_, c); }
      else anythingElse();
      break;
    }
    case S::ScriptDataLessThanSign: {
      const char32_t c = Consume();
      if (c == '/') { temporary_.clear(); state_ = S::ScriptDataEndTagOpen; }
      else if (c == '!') { state_ = S::ScriptDataEscapeStart; EmitCharacter('<'); EmitCharacter('!'); }
      else { EmitCharacter('<'); Reconsume(); state_ = S::ScriptData; }
      break;
    }
    case S::ScriptDataEscapeStart: {
      const char32_t c = Consume();
      if (c == '-') { state_ = S::ScriptDataEscapeStartDash; EmitCharacter('-'); }
      else { Reconsume(); state_ = S::ScriptData; }
      break;
    }
    case S::ScriptDataEscapeStartDash: {
      const char32_t c = Consume();
      if (c == '-') { state_ = S::ScriptDataEscapedDashDash; EmitCharacter('-'); }
      else { Reconsume(); state_ = S::ScriptData; }
      break;
    }
    case S::ScriptDataEscaped: {
      const char32_t c = Consume();
      if (c == '-') { state_ = S::ScriptDataEscapedDash; EmitCharacter('-'); }
      else if (c == '<') state_ = S::ScriptDataEscapedLessThanSign;
      else if (c == 0) { Error("unexpected-null-character"); EmitCharacter(0xFFFD); }
      else if (c == kEof) { Error("eof-in-script-html-comment-like-text"); EmitEof(); }
      else EmitCharacter(c);
      break;
    }
    case S::ScriptDataEscapedDash: {
      const char32_t c = Consume();
      if (c == '-') { state_ = S::ScriptDataEscapedDashDash; EmitCharacter('-'); }
      else if (c == '<') state_ = S::ScriptDataEscapedLessThanSign;
      else if (c == 0) { Error("unexpected-null-character"); state_ = S::ScriptDataEscaped; EmitCharacter(0xFFFD); }
      else if (c == kEof) { Error("eof-in-script-html-comment-like-text"); EmitEof(); }
      else { state_ = S::ScriptDataEscaped; EmitCharacter(c); }
      break;
    }
    case S::ScriptDataEscapedDashDash: {
      const char32_t c = Consume();
      if (c == '-') EmitCharacter('-');
      else if (c == '<') state_ = S::ScriptDataEscapedLessThanSign;
      else if (c == '>') { state_ = S::ScriptData; EmitCharacter('>'); }
      else if (c == 0) { Error("unexpected-null-character"); state_ = S::ScriptDataEscaped; EmitCharacter(0xFFFD); }
      else if (c == kEof) { Error("eof-in-script-html-comment-like-text"); EmitEof(); }
      else { state_ = S::ScriptDataEscaped; EmitCharacter(c); }
      break;
    }
    case S::ScriptDataEscapedLessThanSign: {
      const char32_t c = Consume();
      if (c == '/') { temporary_.clear(); state_ = S::ScriptDataEscapedEndTagOpen; }
      else if (IsAlpha(c)) { temporary_.clear(); EmitCharacter('<'); Reconsume(); state_ = S::ScriptDataDoubleEscapeStart; }
      else { EmitCharacter('<'); Reconsume(); state_ = S::ScriptDataEscaped; }
      break;
    }
    case S::ScriptDataDoubleEscapeStart:
    case S::ScriptDataDoubleEscapeEnd: {
      const bool start = state_ == S::ScriptDataDoubleEscapeStart;
      const char32_t c = Consume();
      if (IsWhitespace(c) || c == '/' || c == '>') {
        const bool script = temporary_ == "script";
        if (start) state_ = script ? S::ScriptDataDoubleEscaped : S::ScriptDataEscaped;
        else state_ = script ? S::ScriptDataEscaped : S::ScriptDataDoubleEscaped;
        EmitCharacter(c);
      } else if (IsUpperAlpha(c)) { AppendUtf8(temporary_, Lower(c)); EmitCharacter(c); }
      else if (IsLowerAlpha(c)) { AppendUtf8(temporary_, c); EmitCharacter(c); }
      else { Reconsume(); state_ = start ? S::ScriptDataEscaped : S::ScriptDataDoubleEscaped; }
      break;
    }
    case S::ScriptDataDoubleEscaped: {
      const char32_t c = Consume();
      if (c == '-') { state_ = S::ScriptDataDoubleEscapedDash; EmitCharacter('-'); }
      else if (c == '<') { state_ = S::ScriptDataDoubleEscapedLessThanSign; EmitCharacter('<'); }
      else if (c == 0) { Error("unexpected-null-character"); EmitCharacter(0xFFFD); }
      else if (c == kEof) { Error("eof-in-script-html-comment-like-text"); EmitEof(); }
      else EmitCharacter(c);
      break;
    }
    case S::ScriptDataDoubleEscapedDash: {
      const char32_t c = Consume();
      if (c == '-') { state_ = S::ScriptDataDoubleEscapedDashDash; EmitCharacter('-'); }
      else if (c == '<') { state_ = S::ScriptDataDoubleEscapedLessThanSign; EmitCharacter('<'); }
      else if (c == 0) { Error("unexpected-null-character"); state_ = S::ScriptDataDoubleEscaped; EmitCharacter(0xFFFD); }
      else if (c == kEof) { Error("eof-in-script-html-comment-like-text"); EmitEof(); }
      else { state_ = S::ScriptDataDoubleEscaped; EmitCharacter(c); }
      break;
    }
    case S::ScriptDataDoubleEscapedDashDash: {
      const char32_t c = Consume();
      if (c == '-') EmitCharacter('-');
      else if (c == '<') { state_ = S::ScriptDataDoubleEscapedLessThanSign; EmitCharacter('<'); }
      else if (c == '>') { state_ = S::ScriptData; EmitCharacter('>'); }
      else if (c == 0) { Error("unexpected-null-character"); state_ = S::ScriptDataDoubleEscaped; EmitCharacter(0xFFFD); }
      else if (c == kEof) { Error("eof-in-script-html-comment-like-text"); EmitEof(); }
      else { state_ = S::ScriptDataDoubleEscaped; EmitCharacter(c); }
      break;
    }
    case S::ScriptDataDoubleEscapedLessThanSign: {
      const char32_t c = Consume();
      if (c == '/') { temporary_.clear(); state_ = S::ScriptDataDoubleEscapeEnd; EmitCharacter('/'); }
      else { Reconsume(); state_ = S::ScriptDataDoubleEscaped; }
      break;
    }

    // ---- Attributes ----
    case S::BeforeAttributeName: {
      const char32_t c = Consume();
      if (IsWhitespace(c)) break;
      if (c == '/' || c == '>' || c == kEof) { Reconsume(); state_ = S::AfterAttributeName; }
      else if (c == '=') {
        Error("unexpected-equals-sign-before-attribute-name");
        StartAttribute();
        AppendToAttributeName(c);
        state_ = S::AttributeName;
      } else { StartAttribute(); Reconsume(); state_ = S::AttributeName; }
      break;
    }
    case S::AttributeName: {
      const char32_t c = Consume();
      if (IsWhitespace(c) || c == '/' || c == '>' || c == kEof) { FinishAttributeName(); Reconsume(); state_ = S::AfterAttributeName; }
      else if (c == '=') { FinishAttributeName(); state_ = S::BeforeAttributeValue; }
      else if (IsUpperAlpha(c)) AppendToAttributeName(Lower(c));
      else if (c == 0) { Error("unexpected-null-character"); AppendToAttributeName(0xFFFD); }
      else {
        if (c == '"' || c == '\'' || c == '<') Error("unexpected-character-in-attribute-name");
        AppendToAttributeName(c);
      }
      break;
    }
    case S::AfterAttributeName: {
      const char32_t c = Consume();
      if (IsWhitespace(c)) break;
      if (c == '/') state_ = S::SelfClosingStartTag;
      else if (c == '=') state_ = S::BeforeAttributeValue;
      else if (c == '>') { state_ = S::Data; EmitCurrent(); }
      else if (c == kEof) { Error("eof-in-tag"); EmitEof(); }
      else { StartAttribute(); Reconsume(); state_ = S::AttributeName; }
      break;
    }
    case S::BeforeAttributeValue: {
      const char32_t c = Consume();
      if (IsWhitespace(c)) break;
      if (c == '"') state_ = S::AttributeValueDoubleQuoted;
      else if (c == '\'') state_ = S::AttributeValueSingleQuoted;
      else if (c == '>') { Error("missing-attribute-value"); state_ = S::Data; EmitCurrent(); }
      else { Reconsume(); state_ = S::AttributeValueUnquoted; }
      break;
    }
    case S::AttributeValueDoubleQuoted:
    case S::AttributeValueSingleQuoted: {
      const char32_t quote = state_ == S::AttributeValueDoubleQuoted ? '"' : '\'';
      const char32_t c = Consume();
      if (c == quote) state_ = S::AfterAttributeValueQuoted;
      else if (c == '&') { returnState_ = state_; state_ = S::CharacterReference; }
      else if (c == 0) { Error("unexpected-null-character"); AppendToAttributeValue(0xFFFD); }
      else if (c == kEof) { Error("eof-in-tag"); EmitEof(); }
      else AppendToAttributeValue(c);
      break;
    }
    case S::AttributeValueUnquoted: {
      const char32_t c = Consume();
      if (IsWhitespace(c)) state_ = S::BeforeAttributeName;
      else if (c == '&') { returnState_ = S::AttributeValueUnquoted; state_ = S::CharacterReference; }
      else if (c == '>') { state_ = S::Data; EmitCurrent(); }
      else if (c == 0) { Error("unexpected-null-character"); AppendToAttributeValue(0xFFFD); }
      else if (c == kEof) { Error("eof-in-tag"); EmitEof(); }
      else {
        if (c == '"' || c == '\'' || c == '<' || c == '=' || c == '`') Error("unexpected-character-in-unquoted-attribute-value");
        AppendToAttributeValue(c);
      }
      break;
    }
    case S::AfterAttributeValueQuoted: {
      const char32_t c = Consume();
      if (IsWhitespace(c)) state_ = S::BeforeAttributeName;
      else if (c == '/') state_ = S::SelfClosingStartTag;
      else if (c == '>') { state_ = S::Data; EmitCurrent(); }
      else if (c == kEof) { Error("eof-in-tag"); EmitEof(); }
      else { Error("missing-whitespace-between-attributes"); Reconsume(); state_ = S::BeforeAttributeName; }
      break;
    }
    case S::SelfClosingStartTag: {
      const char32_t c = Consume();
      if (c == '>') { current_.selfClosing = true; state_ = S::Data; EmitCurrent(); }
      else if (c == kEof) { Error("eof-in-tag"); EmitEof(); }
      else { Error("unexpected-solidus-in-tag"); Reconsume(); state_ = S::BeforeAttributeName; }
      break;
    }

    // ---- Comments and markup declarations ----
    case S::BogusComment: {
      const char32_t c = Consume();
      if (c == '>') { state_ = S::Data; EmitComment(); }
      else if (c == kEof) { EmitComment(); EmitEof(); }
      else if (c == 0) { Error("unexpected-null-character"); AppendUtf8(current_.data, 0xFFFD); }
      else AppendUtf8(current_.data, c);
      break;
    }
    case S::MarkupDeclarationOpen: {
      CheckNextInput();  // it looks at what follows before taking any of it
      if (NextIs("--", false)) {
        pos_ += 2;
        current_ = Token();
        current_.type = Token::Type::Comment;
        state_ = S::CommentStart;
      } else if (NextIs("doctype", true)) {
        pos_ += 7;
        state_ = S::Doctype;
      } else if (NextIs("[CDATA[", false)) {
        pos_ += 7;
        if (cdataAllowed_) {
          state_ = S::CdataSection;
        } else {
          Error("cdata-in-html-content");
          current_ = Token();
          current_.type = Token::Type::Comment;
          current_.data = "[CDATA[";
          state_ = S::BogusComment;
        }
      } else {
        Error("incorrectly-opened-comment");
        current_ = Token();
        current_.type = Token::Type::Comment;
        state_ = S::BogusComment;
      }
      break;
    }
    case S::CommentStart: {
      const char32_t c = Consume();
      if (c == '-') state_ = S::CommentStartDash;
      else if (c == '>') { Error("abrupt-closing-of-empty-comment"); state_ = S::Data; EmitComment(); }
      else { Reconsume(); state_ = S::Comment; }
      break;
    }
    case S::CommentStartDash: {
      const char32_t c = Consume();
      if (c == '-') state_ = S::CommentEnd;
      else if (c == '>') { Error("abrupt-closing-of-empty-comment"); state_ = S::Data; EmitComment(); }
      else if (c == kEof) { Error("eof-in-comment"); EmitComment(); EmitEof(); }
      else { current_.data += '-'; Reconsume(); state_ = S::Comment; }
      break;
    }
    case S::Comment: {
      const char32_t c = Consume();
      if (c == '<') { current_.data += '<'; state_ = S::CommentLessThanSign; }
      else if (c == '-') state_ = S::CommentEndDash;
      else if (c == 0) { Error("unexpected-null-character"); AppendUtf8(current_.data, 0xFFFD); }
      else if (c == kEof) { Error("eof-in-comment"); EmitComment(); EmitEof(); }
      else AppendUtf8(current_.data, c);
      break;
    }
    case S::CommentLessThanSign: {
      const char32_t c = Consume();
      if (c == '!') { current_.data += '!'; state_ = S::CommentLessThanSignBang; }
      else if (c == '<') current_.data += '<';
      else { Reconsume(); state_ = S::Comment; }
      break;
    }
    case S::CommentLessThanSignBang: {
      const char32_t c = Consume();
      if (c == '-') state_ = S::CommentLessThanSignBangDash;
      else { Reconsume(); state_ = S::Comment; }
      break;
    }
    case S::CommentLessThanSignBangDash: {
      const char32_t c = Consume();
      if (c == '-') state_ = S::CommentLessThanSignBangDashDash;
      else { Reconsume(); state_ = S::CommentEndDash; }
      break;
    }
    case S::CommentLessThanSignBangDashDash: {
      const char32_t c = Consume();
      if (c == '>' || c == kEof) { Reconsume(); state_ = S::CommentEnd; }
      else { Error("nested-comment"); Reconsume(); state_ = S::CommentEnd; }
      break;
    }
    case S::CommentEndDash: {
      const char32_t c = Consume();
      if (c == '-') state_ = S::CommentEnd;
      else if (c == kEof) { Error("eof-in-comment"); EmitComment(); EmitEof(); }
      else { current_.data += '-'; Reconsume(); state_ = S::Comment; }
      break;
    }
    case S::CommentEnd: {
      const char32_t c = Consume();
      if (c == '>') { state_ = S::Data; EmitComment(); }
      else if (c == '!') state_ = S::CommentEndBang;
      else if (c == '-') current_.data += '-';
      else if (c == kEof) { Error("eof-in-comment"); EmitComment(); EmitEof(); }
      else { current_.data += "--"; Reconsume(); state_ = S::Comment; }
      break;
    }
    case S::CommentEndBang: {
      const char32_t c = Consume();
      if (c == '-') { current_.data += "--!"; state_ = S::CommentEndDash; }
      else if (c == '>') { Error("incorrectly-closed-comment"); state_ = S::Data; EmitComment(); }
      else if (c == kEof) { Error("eof-in-comment"); EmitComment(); EmitEof(); }
      else { current_.data += "--!"; Reconsume(); state_ = S::Comment; }
      break;
    }

    // ---- DOCTYPE ----
    case S::Doctype: {
      const char32_t c = Consume();
      if (IsWhitespace(c)) state_ = S::BeforeDoctypeName;
      else if (c == '>') { Reconsume(); state_ = S::BeforeDoctypeName; }
      else if (c == kEof) {
        Error("eof-in-doctype");
        current_ = Token();
        current_.type = Token::Type::Doctype;
        current_.forceQuirks = true;
        EmitDoctype();
        EmitEof();
      } else { Error("missing-whitespace-before-doctype-name"); Reconsume(); state_ = S::BeforeDoctypeName; }
      break;
    }
    case S::BeforeDoctypeName: {
      const char32_t c = Consume();
      const auto start = [this] {
        current_ = Token();
        current_.type = Token::Type::Doctype;
      };
      if (IsWhitespace(c)) break;
      if (IsUpperAlpha(c)) { start(); current_.doctypeName = std::string(1, static_cast<char>(Lower(c))); state_ = S::DoctypeName; }
      else if (c == 0) { Error("unexpected-null-character"); start(); current_.doctypeName = "\xEF\xBF\xBD"; state_ = S::DoctypeName; }
      else if (c == '>') { Error("missing-doctype-name"); start(); current_.forceQuirks = true; state_ = S::Data; EmitDoctype(); }
      else if (c == kEof) { Error("eof-in-doctype"); start(); current_.forceQuirks = true; EmitDoctype(); EmitEof(); }
      else { start(); current_.doctypeName = ""; AppendUtf8(*current_.doctypeName, c); state_ = S::DoctypeName; }
      break;
    }
    case S::DoctypeName: {
      const char32_t c = Consume();
      if (IsWhitespace(c)) state_ = S::AfterDoctypeName;
      else if (c == '>') { state_ = S::Data; EmitDoctype(); }
      else if (IsUpperAlpha(c)) AppendUtf8(*current_.doctypeName, Lower(c));
      else if (c == 0) { Error("unexpected-null-character"); AppendUtf8(*current_.doctypeName, 0xFFFD); }
      else if (c == kEof) { Error("eof-in-doctype"); current_.forceQuirks = true; EmitDoctype(); EmitEof(); }
      else AppendUtf8(*current_.doctypeName, c);
      break;
    }
    case S::AfterDoctypeName: {
      const char32_t c = Consume();
      if (IsWhitespace(c)) break;
      if (c == '>') { state_ = S::Data; EmitDoctype(); }
      else if (c == kEof) { Error("eof-in-doctype"); current_.forceQuirks = true; EmitDoctype(); EmitEof(); }
      else {
        Reconsume();
        if (NextIs("public", true)) { pos_ += 6; state_ = S::AfterDoctypePublicKeyword; }
        else if (NextIs("system", true)) { pos_ += 6; state_ = S::AfterDoctypeSystemKeyword; }
        else {
          Error("invalid-character-sequence-after-doctype-name");
          current_.forceQuirks = true;
          state_ = S::BogusDoctype;
        }
      }
      break;
    }
    case S::AfterDoctypePublicKeyword:
    case S::BeforeDoctypePublicIdentifier:
    case S::AfterDoctypeSystemKeyword:
    case S::BeforeDoctypeSystemIdentifier: {
      const bool isPublic = state_ == S::AfterDoctypePublicKeyword || state_ == S::BeforeDoctypePublicIdentifier;
      const bool afterKeyword = state_ == S::AfterDoctypePublicKeyword || state_ == S::AfterDoctypeSystemKeyword;
      const char32_t c = Consume();
      const auto begin = [&](char32_t quote) {
        if (isPublic) {
          current_.publicId = "";
          state_ = quote == '"' ? S::DoctypePublicIdentifierDoubleQuoted : S::DoctypePublicIdentifierSingleQuoted;
        } else {
          current_.systemId = "";
          state_ = quote == '"' ? S::DoctypeSystemIdentifierDoubleQuoted : S::DoctypeSystemIdentifierSingleQuoted;
        }
      };
      if (IsWhitespace(c)) {
        if (afterKeyword) state_ = isPublic ? S::BeforeDoctypePublicIdentifier : S::BeforeDoctypeSystemIdentifier;
      } else if (c == '"' || c == '\'') {
        if (afterKeyword) Error(isPublic ? "missing-whitespace-after-doctype-public-keyword" : "missing-whitespace-after-doctype-system-keyword");
        begin(c);
      } else if (c == '>') {
        Error(isPublic ? "missing-doctype-public-identifier" : "missing-doctype-system-identifier");
        current_.forceQuirks = true;
        state_ = S::Data;
        EmitDoctype();
      } else if (c == kEof) {
        Error("eof-in-doctype");
        current_.forceQuirks = true;
        EmitDoctype();
        EmitEof();
      } else {
        Error(isPublic ? "missing-quote-before-doctype-public-identifier" : "missing-quote-before-doctype-system-identifier");
        current_.forceQuirks = true;
        Reconsume();
        state_ = S::BogusDoctype;
      }
      break;
    }
    case S::DoctypePublicIdentifierDoubleQuoted:
    case S::DoctypePublicIdentifierSingleQuoted:
    case S::DoctypeSystemIdentifierDoubleQuoted:
    case S::DoctypeSystemIdentifierSingleQuoted: {
      const bool isPublic = state_ == S::DoctypePublicIdentifierDoubleQuoted || state_ == S::DoctypePublicIdentifierSingleQuoted;
      const char32_t quote = state_ == S::DoctypePublicIdentifierDoubleQuoted || state_ == S::DoctypeSystemIdentifierDoubleQuoted ? '"' : '\'';
      std::string& id = isPublic ? *current_.publicId : *current_.systemId;
      const char32_t c = Consume();
      if (c == quote) state_ = isPublic ? S::AfterDoctypePublicIdentifier : S::AfterDoctypeSystemIdentifier;
      else if (c == 0) { Error("unexpected-null-character"); AppendUtf8(id, 0xFFFD); }
      else if (c == '>') {
        Error(isPublic ? "abrupt-doctype-public-identifier" : "abrupt-doctype-system-identifier");
        current_.forceQuirks = true;
        state_ = S::Data;
        EmitDoctype();
      } else if (c == kEof) { Error("eof-in-doctype"); current_.forceQuirks = true; EmitDoctype(); EmitEof(); }
      else AppendUtf8(id, c);
      break;
    }
    case S::AfterDoctypePublicIdentifier:
    case S::BetweenDoctypePublicAndSystemIdentifiers: {
      const bool after = state_ == S::AfterDoctypePublicIdentifier;
      const char32_t c = Consume();
      if (IsWhitespace(c)) { if (after) state_ = S::BetweenDoctypePublicAndSystemIdentifiers; }
      else if (c == '>') { state_ = S::Data; EmitDoctype(); }
      else if (c == '"' || c == '\'') {
        if (after) Error("missing-whitespace-between-doctype-public-and-system-identifiers");
        current_.systemId = "";
        state_ = c == '"' ? S::DoctypeSystemIdentifierDoubleQuoted : S::DoctypeSystemIdentifierSingleQuoted;
      } else if (c == kEof) { Error("eof-in-doctype"); current_.forceQuirks = true; EmitDoctype(); EmitEof(); }
      else { Error("missing-quote-before-doctype-system-identifier"); current_.forceQuirks = true; Reconsume(); state_ = S::BogusDoctype; }
      break;
    }
    case S::AfterDoctypeSystemIdentifier: {
      const char32_t c = Consume();
      if (IsWhitespace(c)) break;
      if (c == '>') { state_ = S::Data; EmitDoctype(); }
      else if (c == kEof) { Error("eof-in-doctype"); current_.forceQuirks = true; EmitDoctype(); EmitEof(); }
      else { Error("unexpected-character-after-doctype-system-identifier"); Reconsume(); state_ = S::BogusDoctype; }
      break;
    }
    case S::BogusDoctype: {
      const char32_t c = Consume();
      if (c == '>') { state_ = S::Data; EmitDoctype(); }
      else if (c == 0) Error("unexpected-null-character");
      else if (c == kEof) { EmitDoctype(); EmitEof(); }
      break;
    }

    // ---- CDATA ----
    case S::CdataSection: {
      const char32_t c = Consume();
      if (c == ']') state_ = S::CdataSectionBracket;
      else if (c == kEof) { Error("eof-in-cdata"); EmitEof(); }
      else EmitCharacter(c);
      break;
    }
    case S::CdataSectionBracket: {
      const char32_t c = Consume();
      if (c == ']') state_ = S::CdataSectionEnd;
      else { EmitCharacter(']'); Reconsume(); state_ = S::CdataSection; }
      break;
    }
    case S::CdataSectionEnd: {
      const char32_t c = Consume();
      if (c == ']') EmitCharacter(']');
      else if (c == '>') state_ = S::Data;
      else { EmitCharacter(']'); EmitCharacter(']'); Reconsume(); state_ = S::CdataSection; }
      break;
    }

    // ---- Processing instructions ----
    case S::ProcessingInstructionOpen: {
      // Looked at, not taken: what is not a target is left for the bogus comment state to take.
      const char32_t c = Peek();
      if (IsAlpha(c) || c == '_') state_ = S::ProcessingInstructionTarget;
      else if (c == kEof) { Consume(); Error("eof-in-processing-instruction"); EmitEof(); }
      else { Error("invalid-first-character-of-processing-instruction-target"); ConvertTemporaryToComment(); state_ = S::BogusComment; }
      break;
    }
    case S::ProcessingInstructionTarget: {
      const char32_t c = Consume();
      if (IsWhitespace(c) || c == '?' || c == '>') {
        std::string lowered = temporary_;
        for (char& ch : lowered) ch = static_cast<char>(Lower(static_cast<unsigned char>(ch)));
        if (lowered == "xml" || lowered == "xml-stylesheet") {
          Error("disallowed-processing-instruction-target");
          ConvertTemporaryToComment();
          Reconsume();
          state_ = S::BogusComment;
        } else {
          current_ = Token();
          current_.type = Token::Type::ProcessingInstruction;
          current_.name = temporary_;
          Reconsume();
          state_ = S::AfterProcessingInstructionTarget;
        }
      } else if (IsAlnum(c) || c == '-' || c == '_') AppendUtf8(temporary_, c);
      else if (c == kEof) { Error("eof-in-processing-instruction"); EmitEof(); }
      else { Error("invalid-processing-instruction-target"); ConvertTemporaryToComment(); Reconsume(); state_ = S::BogusComment; }
      break;
    }
    case S::AfterProcessingInstructionTarget: {
      const char32_t c = Consume();
      if (IsWhitespace(c)) break;
      Reconsume();
      state_ = S::ProcessingInstructionData;
      break;
    }
    case S::ProcessingInstructionData: {
      const char32_t c = Consume();
      if (c == '?') state_ = S::ProcessingInstructionQuestionable;
      else if (c == '>') { state_ = S::Data; EmitComment(); }
      else if (c == kEof) { Error("eof-in-processing-instruction"); EmitEof(); }
      else AppendUtf8(current_.data, c);
      break;
    }
    case S::ProcessingInstructionQuestionable: {
      const char32_t c = Consume();
      if (c == '>') { state_ = S::Data; EmitComment(); }
      else if (c == kEof) { Error("eof-in-processing-instruction"); EmitEof(); }
      else { current_.data += '?'; Reconsume(); state_ = S::ProcessingInstructionData; }
      break;
    }

    // ---- Character references ----
    case S::CharacterReference: {
      temporary_ = "&";
      const char32_t c = Consume();
      if (IsAlnum(c)) { Reconsume(); state_ = S::NamedCharacterReference; }
      else if (c == '#') { temporary_ += '#'; state_ = S::NumericCharacterReference; }
      else { FlushReference(); Reconsume(); state_ = returnState_; }
      break;
    }
    case S::NamedCharacterReference:
      NamedReference();
      break;
    case S::AmbiguousAmpersand: {
      const char32_t c = Consume();
      if (IsAlnum(c)) {
        if (InAttribute()) AppendToAttributeValue(c);
        else EmitCharacter(c);
      } else if (c == ';') { Error("unknown-named-character-reference"); Reconsume(); state_ = returnState_; }
      else { Reconsume(); state_ = returnState_; }
      break;
    }
    case S::NumericCharacterReference: {
      referenceCode_ = 0;
      const char32_t c = Consume();
      if (c == 'x' || c == 'X') { AppendUtf8(temporary_, c); state_ = S::HexadecimalCharacterReferenceStart; }
      else { Reconsume(); state_ = S::DecimalCharacterReferenceStart; }
      break;
    }
    case S::HexadecimalCharacterReferenceStart:
    case S::DecimalCharacterReferenceStart: {
      const bool hex = state_ == S::HexadecimalCharacterReferenceStart;
      const char32_t c = Consume();
      if (hex ? IsHexDigit(c) : IsDigit(c)) { Reconsume(); state_ = hex ? S::HexadecimalCharacterReference : S::DecimalCharacterReference; }
      else { Error("absence-of-digits-in-numeric-character-reference"); FlushReference(); Reconsume(); state_ = returnState_; }
      break;
    }
    case S::HexadecimalCharacterReference:
    case S::DecimalCharacterReference: {
      const bool hex = state_ == S::HexadecimalCharacterReference;
      const char32_t c = Consume();
      if (hex ? IsHexDigit(c) : IsDigit(c)) {
        const uint32_t digit = IsDigit(c) ? c - '0' : Lower(c) - 'a' + 10;
        // Past the largest code point it no longer matters by how much; stay clear of overflowing.
        referenceCode_ = referenceCode_ > 0x10FFFF ? referenceCode_ : referenceCode_ * (hex ? 16 : 10) + digit;
      } else if (c == ';') state_ = S::NumericCharacterReferenceEnd;
      else { Error("missing-semicolon-after-character-reference"); Reconsume(); state_ = S::NumericCharacterReferenceEnd; }
      break;
    }
    case S::NumericCharacterReferenceEnd:
      ReferenceEnd();
      break;
  }
}

}  // namespace solar::html
