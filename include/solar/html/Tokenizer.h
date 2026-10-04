#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The HTML tokenizer (https://html.spec.whatwg.org/#tokenization): markup in, tokens out. It is driven by
// the tree builder, which asks for a token at a time and changes the tokenizer's state between them, as
// the standard has the two work together.
namespace solar::html {

struct TokenAttribute {
  std::string name;
  std::string value;
  // Set by the tree builder when it makes the attribute a namespaced one ("xlink:href"): what it is on the
  // element. Empty `localName` is an attribute of no namespace.
  std::string namespaceUri;
  std::string prefix;
  std::string localName;
};

struct Token {
  // NeedMoreInput is only given by a tokenizer that is fed in pieces: the input so far ends in the middle of a token.
  enum class Type { Doctype, StartTag, EndTag, Comment, ProcessingInstruction, Character, EndOfFile, NeedMoreInput };
  Type type = Type::EndOfFile;
  // A tag's name, or a processing instruction's target; a DOCTYPE's name is `doctypeName`, which can be missing as well as empty.
  std::string name;
  std::vector<TokenAttribute> attributes;
  bool selfClosing = false;
  // A comment's text, a processing instruction's data, or the one code point a Character token is, as UTF-8.
  std::string data;
  std::optional<std::string> doctypeName;
  std::optional<std::string> publicId;
  std::optional<std::string> systemId;
  bool forceQuirks = false;

  bool IsCharacter() const { return type == Type::Character; }
  // The attribute's value, or null if there is none of that name.
  const std::string* Attribute(std::string_view attributeName) const {
    for (const TokenAttribute& attribute : attributes) {
      if (attribute.name == attributeName) return &attribute.value;
    }
    return nullptr;
  }
};

class Tokenizer {
 public:
  // The states the tree builder puts the tokenizer in (and the tests start it in); the rest are its own.
  enum class State {
    Data,
    Rcdata,
    Rawtext,
    ScriptData,
    Plaintext,
    // The tokenizer's own.
    TagOpen,
    EndTagOpen,
    TagName,
    RcdataLessThanSign,
    RcdataEndTagOpen,
    RcdataEndTagName,
    RawtextLessThanSign,
    RawtextEndTagOpen,
    RawtextEndTagName,
    ScriptDataLessThanSign,
    ScriptDataEndTagOpen,
    ScriptDataEndTagName,
    ScriptDataEscapeStart,
    ScriptDataEscapeStartDash,
    ScriptDataEscaped,
    ScriptDataEscapedDash,
    ScriptDataEscapedDashDash,
    ScriptDataEscapedLessThanSign,
    ScriptDataEscapedEndTagOpen,
    ScriptDataEscapedEndTagName,
    ScriptDataDoubleEscapeStart,
    ScriptDataDoubleEscaped,
    ScriptDataDoubleEscapedDash,
    ScriptDataDoubleEscapedDashDash,
    ScriptDataDoubleEscapedLessThanSign,
    ScriptDataDoubleEscapeEnd,
    BeforeAttributeName,
    AttributeName,
    AfterAttributeName,
    BeforeAttributeValue,
    AttributeValueDoubleQuoted,
    AttributeValueSingleQuoted,
    AttributeValueUnquoted,
    AfterAttributeValueQuoted,
    SelfClosingStartTag,
    BogusComment,
    MarkupDeclarationOpen,
    CommentStart,
    CommentStartDash,
    Comment,
    CommentLessThanSign,
    CommentLessThanSignBang,
    CommentLessThanSignBangDash,
    CommentLessThanSignBangDashDash,
    CommentEndDash,
    CommentEnd,
    CommentEndBang,
    Doctype,
    BeforeDoctypeName,
    DoctypeName,
    AfterDoctypeName,
    AfterDoctypePublicKeyword,
    BeforeDoctypePublicIdentifier,
    DoctypePublicIdentifierDoubleQuoted,
    DoctypePublicIdentifierSingleQuoted,
    AfterDoctypePublicIdentifier,
    BetweenDoctypePublicAndSystemIdentifiers,
    AfterDoctypeSystemKeyword,
    BeforeDoctypeSystemIdentifier,
    DoctypeSystemIdentifierDoubleQuoted,
    DoctypeSystemIdentifierSingleQuoted,
    AfterDoctypeSystemIdentifier,
    BogusDoctype,
    CdataSection,
    CdataSectionBracket,
    CdataSectionEnd,
    ProcessingInstructionOpen,
    ProcessingInstructionTarget,
    AfterProcessingInstructionTarget,
    ProcessingInstructionData,
    ProcessingInstructionQuestionable,
    CharacterReference,
    NamedCharacterReference,
    AmbiguousAmpersand,
    NumericCharacterReference,
    HexadecimalCharacterReferenceStart,
    DecimalCharacterReferenceStart,
    HexadecimalCharacterReference,
    DecimalCharacterReference,
    NumericCharacterReferenceEnd,
  };

  // `markup` is UTF-8 (what the encoding sniffing has decoded to, in time): malformed bytes become U+FFFD,
  // and newlines are normalized as the standard's input stream preprocessing has it.
  explicit Tokenizer(std::string_view markup);

  // The next token. After the end of the input it is EndOfFile again and again.
  Token Next();

  // Feeding in pieces, as document.write does: the tokenizer then gives NeedMoreInput where the input stops in the
  // middle of a token, and picks up there when more has been appended. CloseInput() is the end of the input.
  void SetStreaming(bool streaming) {
    streaming_ = streaming;
    closed_ = !streaming;
  }
  void Append(std::string_view markup);
  void CloseInput() { closed_ = true; }
  // Text put at the position the tokenizer is at, which is what a script's document.write does while the parser runs.
  // Each insertion goes after the one before it; BeginInsertion puts the point back at the tokenizer's position.
  void BeginInsertion() { insertAt_ = pos_; }
  size_t insertAt() const { return insertAt_; }
  void SetInsertAt(size_t at) { insertAt_ = at; }
  size_t InputSize() const { return input_.size(); }
  // The input is taken to end at `limit` (to be given more of what lies past it later): a token that would need
  // more is NeedMoreInput. kNoLimit takes the input as it is.
  static constexpr size_t kNoLimit = static_cast<size_t>(-1);
  void SetLimit(size_t limit) { limit_ = limit; }
  size_t limit() const { return limit_; }
  void InsertAtPosition(std::string_view markup);

  void SetState(State state) { state_ = state; }
  State state() const { return state_; }
  // The name of the last start tag emitted, which decides whether an end tag in text is the one that ends it.
  void SetLastStartTag(std::string name) { lastStartTag_ = std::move(name); }
  // Whether "<![CDATA[" opens a CDATA section: when the adjusted current node is in another namespace than HTML's.
  void SetCdataAllowed(bool allowed) { cdataAllowed_ = allowed; }

  // The parse errors met so far, by the names the standard gives them ("eof-in-tag"), in order.
  const std::vector<std::string>& errors() const { return errors_; }
  // Reports one the tree builder found, to keep the errors in the order they happened.
  void ReportError(std::string code) { errors_.push_back(std::move(code)); }

 private:
  static constexpr char32_t kEof = 0xFFFFFFFF;

  char32_t Consume();
  void Reconsume() { --pos_; }
  bool Restricted() const { return (streaming_ && !closed_) || limit_ != kNoLimit; }
  size_t Available() const { return std::min(input_.size(), limit_); }
  char32_t Peek(size_t ahead = 0) const {
    if (pos_ + ahead < Available()) return input_[pos_ + ahead];
    if (Restricted()) starved_ = true;
    return kEof;
  }
  static std::u32string Normalize(std::string_view markup);
  bool NextIs(std::string_view text, bool ignoreCase);
  void Error(const char* code) { errors_.push_back(code); }

  void Step();
  // The input stream errors of the character that is next, which the standard has checked before each step.
  void CheckNextInput();
  void ConvertTemporaryToComment();
  void EmitCharacter(char32_t c);
  void EmitCurrent();
  void EmitEof();
  void EmitComment();
  void EmitDoctype();

  void StartTag(Token::Type type);
  void StartAttribute();
  void FinishAttributeName();
  void CommitAttribute();
  void AppendToTagName(char32_t c);
  void AppendToAttributeName(char32_t c);
  void AppendToAttributeValue(char32_t c);
  bool AppropriateEndTag() const { return current_.type == Token::Type::EndTag && current_.name == lastStartTag_; }
  bool InAttribute() const {
    return returnState_ == State::AttributeValueDoubleQuoted || returnState_ == State::AttributeValueSingleQuoted || returnState_ == State::AttributeValueUnquoted;
  }
  // "Flush code points consumed as a character reference": into the attribute value or as text.
  void FlushReference();
  void EmitTemporary();
  void NamedReference();
  void ReferenceEnd();

  std::u32string input_;
  size_t pos_ = 0;
  size_t reported_ = 0;  // input positions below it have had their input stream errors reported
  State state_ = State::Data;
  State returnState_ = State::Data;
  std::deque<Token> queue_;
  std::vector<std::string> errors_;

  Token current_;
  TokenAttribute attribute_;
  bool attributeActive_ = false;
  bool attributeDropped_ = false;
  std::string temporary_;
  std::string lastStartTag_;
  bool cdataAllowed_ = false;
  uint32_t referenceCode_ = 0;
  bool finished_ = false;
  size_t insertAt_ = 0;
  size_t limit_ = kNoLimit;
  bool streaming_ = false;
  bool closed_ = true;
  mutable bool starved_ = false;  // a step wanted input that has not come yet
};

}  // namespace solar::html
