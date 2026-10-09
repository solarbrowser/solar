#include "solar/css/Selectors.h"
#include "solar/css/Syntax.h"

#include <algorithm>

#include "solar/css/Tokenizer.h"

namespace solar::css {

namespace {

struct ParseError {};

std::string LowerAscii(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 0x20);
  }
  return out;
}

struct PseudoName {
  const char* name;
  PseudoClass pseudo;
};

const PseudoName kPseudoClasses[] = {
    {"root", PseudoClass::Root},
    {"empty", PseudoClass::Empty},
    {"first-child", PseudoClass::FirstChild},
    {"last-child", PseudoClass::LastChild},
    {"only-child", PseudoClass::OnlyChild},
    {"first-of-type", PseudoClass::FirstOfType},
    {"last-of-type", PseudoClass::LastOfType},
    {"only-of-type", PseudoClass::OnlyOfType},
    {"scope", PseudoClass::Scope},
    {"link", PseudoClass::Link},
    {"any-link", PseudoClass::AnyLink},
    {"visited", PseudoClass::Visited},
    {"hover", PseudoClass::Hover},
    {"active", PseudoClass::Active},
    {"focus", PseudoClass::Focus},
    {"focus-visible", PseudoClass::FocusVisible},
    {"focus-within", PseudoClass::FocusWithin},
    {"target", PseudoClass::Target},
    {"target-within", PseudoClass::TargetWithin},
    {"checked", PseudoClass::Checked},
    {"disabled", PseudoClass::Disabled},
    {"enabled", PseudoClass::Enabled},
    {"required", PseudoClass::Required},
    {"optional", PseudoClass::Optional},
    {"read-only", PseudoClass::ReadOnly},
    {"read-write", PseudoClass::ReadWrite},
    {"placeholder-shown", PseudoClass::PlaceholderShown},
    {"default", PseudoClass::Default},
    {"indeterminate", PseudoClass::Indeterminate},
    {"valid", PseudoClass::Valid},
    {"invalid", PseudoClass::Invalid},
    {"in-range", PseudoClass::InRange},
    {"out-of-range", PseudoClass::OutOfRange},
    {"defined", PseudoClass::Defined},
    {"fullscreen", PseudoClass::Fullscreen},
    {"modal", PseudoClass::Modal},
    {"popover-open", PseudoClass::PopoverOpen},
    {"user-invalid", PseudoClass::UserInvalid},
    {"user-valid", PseudoClass::UserValid},
    {"playing", PseudoClass::Playing},
    {"paused", PseudoClass::Paused},
    {"muted", PseudoClass::Muted},
    {"autofill", PseudoClass::Autofill},
    {"past", PseudoClass::Past},
    {"current", PseudoClass::Current},
    {"future", PseudoClass::Future},
    {"local-link", PseudoClass::LocalLink},
    {"host", PseudoClass::Host},
    {"open", PseudoClass::Open},
    {"closed", PseudoClass::Closed},
};

const char* const kPseudoElements[] = {"before", "after", "first-line", "first-letter", "selection", "marker", "placeholder", "backdrop",
                                       "file-selector-button", "grammar-error", "spelling-error", "target-text", "highlight", "cue", "cue-region",
                                       "slotted", "part", "view-transition", "details-content", "checkmark", "picker-icon", "scroll-marker"};

class Parser {
 public:
  explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

  std::optional<SelectorList> ParseTop() {
    try {
      SelectorList list = ParseList(false, false);
      if (Peek().type != Token::Type::EndOfFile) throw ParseError();
      return list;
    } catch (const ParseError&) {
      return std::nullopt;
    }
  }

 private:
  const Token& Peek(size_t ahead = 0) const { return pos_ + ahead < tokens_.size() ? tokens_[pos_ + ahead] : tokens_.back(); }
  const Token& Next() { return pos_ < tokens_.size() ? tokens_[pos_++] : tokens_.back(); }
  bool SkipWhitespace() {
    bool any = false;
    while (Peek().type == Token::Type::Whitespace) {
      ++pos_;
      any = true;
    }
    return any;
  }

  // A selector list up to the end of the input or a closing parenthesis. `forgiving`: what is invalid is dropped.
  SelectorList ParseList(bool forgiving, bool relative) {
    SelectorList list;
    for (;;) {
      SkipWhitespace();
      const size_t start = pos_;
      try {
        if (forgiving && (Peek().type == Token::Type::Comma || Peek().type == Token::Type::RightParen || Peek().type == Token::Type::EndOfFile)) {
          // An empty item.
        } else {
          list.push_back(ParseComplex(relative));
        }
        SkipWhitespace();
        if (Peek().type != Token::Type::Comma && Peek().type != Token::Type::RightParen && Peek().type != Token::Type::EndOfFile) throw ParseError();
      } catch (const ParseError&) {
        if (!forgiving) throw;
        pos_ = start;
        SkipToComma();
        // An invalid item makes the whole of what it was a part of dropped, and so does nothing in the list.
      }
      if (Peek().type == Token::Type::Comma) {
        ++pos_;
        continue;
      }
      return list;
    }
  }

  // To the next comma, or the end, that is not inside brackets.
  void SkipToComma() {
    int depth = 0;
    while (Peek().type != Token::Type::EndOfFile) {
      const Token::Type type = Peek().type;
      if (type == Token::Type::LeftParen || type == Token::Type::Function || type == Token::Type::LeftBracket || type == Token::Type::LeftBrace) ++depth;
      else if (type == Token::Type::RightParen || type == Token::Type::RightBracket || type == Token::Type::RightBrace) {
        if (depth == 0) return;
        --depth;
      } else if (type == Token::Type::Comma && depth == 0) return;
      ++pos_;
    }
  }

  static bool IsCombinatorDelim(const Token& token) { return token.IsDelim('>') || token.IsDelim('+') || token.IsDelim('~'); }

  static Combinator CombinatorOf(const Token& token) {
    if (token.IsDelim('>')) return Combinator::Child;
    if (token.IsDelim('+')) return Combinator::NextSibling;
    return Combinator::SubsequentSibling;
  }

  bool StartsCompound() const {
    const Token& token = Peek();
    switch (token.type) {
      case Token::Type::Ident:
      case Token::Type::Hash:
      case Token::Type::LeftBracket:
      case Token::Type::Colon:
        return true;
      case Token::Type::Delim:
        return token.delim == '*' || token.delim == '.' || token.delim == '|';
      default:
        return false;
    }
  }

  ComplexSelector ParseComplex(bool relative) {
    ComplexSelector selector;
    if (relative) {
      selector.relative = true;
      SkipWhitespace();
      if (IsCombinatorDelim(Peek())) {
        selector.leading = CombinatorOf(Next());
        SkipWhitespace();
      }
    }
    selector.compounds.push_back(ParseCompound());
    for (;;) {
      const bool whitespace = SkipWhitespace();
      if (IsCombinatorDelim(Peek())) {
        selector.combinators.push_back(CombinatorOf(Next()));
        SkipWhitespace();
        selector.compounds.push_back(ParseCompound());
      } else if (whitespace && StartsCompound()) {
        selector.combinators.push_back(Combinator::Descendant);
        selector.compounds.push_back(ParseCompound());
      } else {
        return selector;
      }
    }
  }

  // The name of a type or an attribute and what namespace it has, from "a", "*", "ns|a", "*|a" and "|a".
  struct QualifiedName {
    std::optional<std::string> namespaceName;
    std::string name;  // "*" for any
  };

  bool IsNameToken(const Token& token) const { return token.type == Token::Type::Ident || token.IsDelim('*'); }

  // Whether a namespace separator comes next: a "|" that is not the start of "|=".
  // The end of a function's arguments: its parenthesis, or the end of the input, which closes it.
  bool CloseParen() {
    if (Peek().type == Token::Type::EndOfFile) return true;
    return Next().type == Token::Type::RightParen;
  }
  bool SeparatorNext() const { return Peek().IsDelim('|') && !Peek(1).IsDelim('='); }

  std::optional<QualifiedName> TryParseQualifiedName(bool allowWildcardName) {
    const size_t start = pos_;
    QualifiedName result;
    if (Peek().IsDelim('|') && SeparatorNext() && IsNameToken(Peek(1))) {
      ++pos_;
      result.namespaceName = "";
    } else if (IsNameToken(Peek()) && Peek(1).IsDelim('|') && !Peek(2).IsDelim('=') && IsNameToken(Peek(2))) {
      const Token& prefix = Next();
      ++pos_;
      if (prefix.IsDelim('*')) {
        result.namespaceName = "*";
      } else {
        // A prefix names a namespace some @namespace rule declared, and there is none here.
        throw ParseError();
      }
    }
    const Token& name = Peek();
    if (name.type == Token::Type::Ident) {
      result.name = name.value;
      ++pos_;
    } else if (name.IsDelim('*') && allowWildcardName) {
      result.name = "*";
      ++pos_;
    } else {
      pos_ = start;
      return std::nullopt;
    }
    return result;
  }

  CompoundSelector ParseCompound() {
    CompoundSelector compound;
    if (auto name = TryParseQualifiedName(true)) {
      SimpleSelector simple;
      simple.kind = name->name == "*" ? SimpleSelector::Kind::Universal : SimpleSelector::Kind::Type;
      simple.name = name->name;
      simple.namespaceName = name->namespaceName;
      compound.simples.push_back(std::move(simple));
    }
    for (;;) {
      const Token& token = Peek();
      if (token.type == Token::Type::Hash) {
        if (!token.hashIsId) throw ParseError();
        SimpleSelector simple;
        simple.kind = SimpleSelector::Kind::Id;
        simple.name = token.value;
        compound.simples.push_back(std::move(simple));
        ++pos_;
      } else if (token.IsDelim('.')) {
        ++pos_;
        if (Peek().type != Token::Type::Ident) throw ParseError();
        SimpleSelector simple;
        simple.kind = SimpleSelector::Kind::Class;
        simple.name = Next().value;
        compound.simples.push_back(std::move(simple));
      } else if (token.type == Token::Type::LeftBracket) {
        compound.simples.push_back(ParseAttribute());
      } else if (token.type == Token::Type::Colon) {
        compound.simples.push_back(ParsePseudo());
      } else {
        break;
      }
    }
    if (compound.simples.empty()) throw ParseError();
    return compound;
  }

  SimpleSelector ParseAttribute() {
    ++pos_;  // [
    SkipWhitespace();
    auto name = TryParseQualifiedName(false);
    if (!name) throw ParseError();
    SimpleSelector simple;
    simple.kind = SimpleSelector::Kind::Attribute;
    simple.name = name->name;
    simple.namespaceName = name->namespaceName;
    SkipWhitespace();
    // The end of the input closes what is open, as it does everywhere in CSS.
    if (Peek().type == Token::Type::EndOfFile) return simple;
    if (Peek().type == Token::Type::RightBracket) {
      ++pos_;
      return simple;
    }
    // The operator: "=" or one of ~ | ^ $ * followed at once by "=".
    const Token& first = Next();
    if (first.IsDelim('=')) {
      simple.op = AttributeOperator::Equals;
    } else if (first.type == Token::Type::Delim && Peek().IsDelim('=')) {
      switch (first.delim) {
        case '~': simple.op = AttributeOperator::Includes; break;
        case '|': simple.op = AttributeOperator::DashMatch; break;
        case '^': simple.op = AttributeOperator::Prefix; break;
        case '$': simple.op = AttributeOperator::Suffix; break;
        case '*': simple.op = AttributeOperator::Substring; break;
        default: throw ParseError();
      }
      ++pos_;
    } else {
      throw ParseError();
    }
    SkipWhitespace();
    const Token& value = Next();
    if (value.type != Token::Type::Ident && value.type != Token::Type::String) throw ParseError();
    simple.value = value.value;
    SkipWhitespace();
    if (Peek().type == Token::Type::Ident) {
      const std::string flag = LowerAscii(Peek().value);
      if (flag == "i") simple.caseMode = CaseMode::Insensitive;
      else if (flag == "s") simple.caseMode = CaseMode::Sensitive;
      else throw ParseError();
      ++pos_;
      SkipWhitespace();
    }
    if (Peek().type == Token::Type::EndOfFile) return simple;
    if (Next().type != Token::Type::RightBracket) throw ParseError();
    return simple;
  }

  // The "An+B" of :nth-child() and the like, which ends at what it is not part of.
  void ParseAnPlusB(int& a, int& b) {
    SkipWhitespace();
    const Token& token = Next();
    a = 0;
    b = 0;
    const auto toInt = [](double value) { return value > 2147483647.0 ? 2147483647 : value < -2147483648.0 ? -2147483647 : static_cast<int>(value); };
    // "n" followed by an optional sign and number, as its own tokens.
    const auto tail = [&](int sign) {
      // After "n": nothing, "+ 1", "- 1", "+1" or "-1".
      const size_t saved = pos_;
      SkipWhitespace();
      if (Peek().IsDelim('+') || Peek().IsDelim('-')) {
        const int s = Peek().IsDelim('-') ? -1 : 1;
        ++pos_;
        SkipWhitespace();
        if (Peek().type == Token::Type::Number && Peek().isInteger && !Peek().hasSign) {
          b = s * toInt(Next().number);
          return;
        }
        throw ParseError();
      }
      if (Peek().type == Token::Type::Number && Peek().isInteger && Peek().hasSign) {
        b = toInt(Next().number);
        return;
      }
      pos_ = saved;
      (void)sign;
    };
    // "n-" followed by the digits as a number token.
    const auto minusTail = [&] {
      SkipWhitespace();
      if (Peek().type == Token::Type::Number && Peek().isInteger && !Peek().hasSign) {
        b = -toInt(Next().number);
        return;
      }
      throw ParseError();
    };
    // The text "n-<digits>" as a unit or an identifier: b is the negative digits.
    const auto digitsAfterMinus = [&](const std::string& rest) {
      if (rest.empty()) throw ParseError();
      for (char c : rest) {
        if (c < '0' || c > '9') throw ParseError();
      }
      b = -toInt(std::stod(rest));
    };

    if (token.type == Token::Type::Number && token.isInteger) {
      b = toInt(token.number);
      return;
    }
    if (token.type == Token::Type::Dimension && token.isInteger) {
      a = toInt(token.number);
      const std::string unit = LowerAscii(token.value);
      if (unit == "n") { tail(1); return; }
      if (unit == "n-") { minusTail(); return; }
      if (unit.size() > 2 && unit.starts_with("n-")) { digitsAfterMinus(unit.substr(2)); return; }
      throw ParseError();
    }
    if (token.type == Token::Type::Ident) {
      const std::string name = LowerAscii(token.value);
      if (name == "even") { a = 2; b = 0; return; }
      if (name == "odd") { a = 2; b = 1; return; }
      std::string rest;
      if (name == "n" || name.starts_with("n-")) { a = 1; rest = name.substr(1); }
      else if (name == "-n" || name.starts_with("-n-")) { a = -1; rest = name.substr(2); }
      else throw ParseError();
      if (rest.empty()) { tail(a); return; }
      if (rest == "-") { minusTail(); return; }
      digitsAfterMinus(rest.substr(1));
      return;
    }
    if (token.IsDelim('+') && Peek().type == Token::Type::Ident) {
      const std::string name = LowerAscii(Next().value);
      if (name == "n") { a = 1; tail(1); return; }
      if (name == "n-") { a = 1; minusTail(); return; }
      if (name.size() > 2 && name.starts_with("n-")) { a = 1; digitsAfterMinus(name.substr(2)); return; }
    }
    throw ParseError();
  }

  SimpleSelector ParsePseudo() {
    ++pos_;  // :
    SimpleSelector simple;
    bool doubleColon = false;
    if (Peek().type == Token::Type::Colon) {
      ++pos_;
      doubleColon = true;
    }
    const Token token = Next();
    if (token.type == Token::Type::Ident) {
      const std::string name = LowerAscii(token.value);
      if (doubleColon || name == "before" || name == "after" || name == "first-line" || name == "first-letter") {
        if (std::find_if(std::begin(kPseudoElements), std::end(kPseudoElements), [&](const char* known) { return name == known; }) == std::end(kPseudoElements)) throw ParseError();
        simple.kind = SimpleSelector::Kind::PseudoElement;
        simple.name = name;
        return simple;
      }
      for (const PseudoName& known : kPseudoClasses) {
        if (name == known.name) {
          simple.kind = SimpleSelector::Kind::Pseudo;
          simple.pseudo = known.pseudo;
          simple.name = name;
          return simple;
        }
      }
      throw ParseError();
    }
    if (token.type != Token::Type::Function) throw ParseError();
    const std::string name = LowerAscii(token.value);
    simple.name = name;
    if (doubleColon) {
      // ::part(), ::slotted(), ::highlight() and the like: valid, and no element of a document is one.
      if (std::find_if(std::begin(kPseudoElements), std::end(kPseudoElements), [&](const char* known) { return name == known; }) == std::end(kPseudoElements)) throw ParseError();
      simple.value = SkipFunctionBody();
      simple.kind = SimpleSelector::Kind::PseudoElement;
      return simple;
    }
    if (name == "not" || name == "is" || name == "where" || name == "matches" || name == "-webkit-any") {
      const bool forgiving = name != "not";
      simple.kind = name == "not" ? SimpleSelector::Kind::Not : name == "where" ? SimpleSelector::Kind::Where : SimpleSelector::Kind::Is;
      simple.list = std::make_shared<SelectorList>(ParseList(forgiving, false));
      SkipWhitespace();
      if (!CloseParen()) throw ParseError();
      return simple;
    }
    if (name == "has") {
      simple.kind = SimpleSelector::Kind::Has;
      SelectorList list = ParseList(false, true);
      if (list.empty()) throw ParseError();
      simple.list = std::make_shared<SelectorList>(std::move(list));
      SkipWhitespace();
      if (!CloseParen()) throw ParseError();
      return simple;
    }
    if (name == "nth-child" || name == "nth-last-child" || name == "nth-of-type" || name == "nth-last-of-type") {
      simple.kind = SimpleSelector::Kind::Nth;
      simple.fromEnd = name == "nth-last-child" || name == "nth-last-of-type";
      simple.ofType = name == "nth-of-type" || name == "nth-last-of-type";
      ParseAnPlusB(simple.a, simple.b);
      SkipWhitespace();
      if (!simple.ofType && Peek().type == Token::Type::Ident && LowerAscii(Peek().value) == "of") {
        ++pos_;
        if (!SkipWhitespace()) throw ParseError();
        simple.list = std::make_shared<SelectorList>(ParseList(false, false));
        if (simple.list->empty()) throw ParseError();
        SkipWhitespace();
      }
      if (!CloseParen()) throw ParseError();
      return simple;
    }
    if (name == "lang") {
      simple.kind = SimpleSelector::Kind::Lang;
      for (;;) {
        SkipWhitespace();
        const Token& language = Next();
        if (language.type != Token::Type::Ident && language.type != Token::Type::String) throw ParseError();
        simple.languages.push_back(language.value);
        SkipWhitespace();
        if (Peek().type == Token::Type::Comma) {
          ++pos_;
          continue;
        }
        break;
      }
      if (!CloseParen()) throw ParseError();
      return simple;
    }
    if (name == "dir") {
      simple.kind = SimpleSelector::Kind::Dir;
      SkipWhitespace();
      const Token& direction = Next();
      if (direction.type != Token::Type::Ident) throw ParseError();
      simple.value = LowerAscii(direction.value);
      if (simple.value != "ltr" && simple.value != "rtl") throw ParseError();
      SkipWhitespace();
      if (!CloseParen()) throw ParseError();
      return simple;
    }
    if (name == "host" || name == "host-context" || name == "state" || name == "current" || name == "past" || name == "future") {
      // Valid, and matching nothing in the light tree of a document.
      simple.value = SkipFunctionBody();
      simple.kind = SimpleSelector::Kind::Unknown;
      return simple;
    }
    throw ParseError();
  }

  // To the parenthesis that closes the function whose name has been consumed.
  // Returns what was in it, as text, for the selector to be written back as it was.
  std::string SkipFunctionBody() {
    int depth = 1;
    std::vector<Token> body;
    while (depth > 0) {
      const Token& token = Next();
      if (token.type == Token::Type::EndOfFile) break;
      if (token.type == Token::Type::LeftParen || token.type == Token::Type::Function) ++depth;
      else if (token.type == Token::Type::RightParen && --depth == 0) break;
      body.push_back(token);
    }
    return SerializeTokens(body);
  }

  std::vector<Token> tokens_;
  size_t pos_ = 0;
};

Specificity Max(const SelectorList& list) {
  Specificity best;
  for (const ComplexSelector& selector : list) {
    const Specificity s = SpecificityOf(selector);
    if (best < s) best = s;
  }
  return best;
}

}  // namespace

std::optional<SelectorList> ParseSelectorList(std::string_view text) {
  Parser parser(Tokenize(text));
  std::optional<SelectorList> list = parser.ParseTop();
  if (list && list->empty()) return std::nullopt;
  return list;
}

Specificity SpecificityOf(const ComplexSelector& selector) {
  Specificity result;
  for (const CompoundSelector& compound : selector.compounds) {
    for (const SimpleSelector& simple : compound.simples) {
      switch (simple.kind) {
        case SimpleSelector::Kind::Id: ++result.ids; break;
        case SimpleSelector::Kind::Class:
        case SimpleSelector::Kind::Attribute:
        case SimpleSelector::Kind::Pseudo:
        case SimpleSelector::Kind::Lang:
        case SimpleSelector::Kind::Dir:
        case SimpleSelector::Kind::Unknown:
          ++result.classes;
          break;
        case SimpleSelector::Kind::Type:
        case SimpleSelector::Kind::PseudoElement:
          ++result.types;
          break;
        case SimpleSelector::Kind::Not:
        case SimpleSelector::Kind::Is:
        case SimpleSelector::Kind::Has: {
          const Specificity s = Max(*simple.list);
          result.ids += s.ids;
          result.classes += s.classes;
          result.types += s.types;
          break;
        }
        case SimpleSelector::Kind::Nth: {
          ++result.classes;
          if (simple.list) {
            const Specificity s = Max(*simple.list);
            result.ids += s.ids;
            result.classes += s.classes;
            result.types += s.types;
          }
          break;
        }
        case SimpleSelector::Kind::Where:
        case SimpleSelector::Kind::Universal:
          break;
      }
    }
  }
  return result;
}

namespace {

std::string SerializeAnPlusB(int a, int b) {
  if (a == 0) return std::to_string(b);
  std::string out = a == 1 ? "n" : a == -1 ? "-n" : std::to_string(a) + "n";
  if (b > 0) out += " + " + std::to_string(b);
  else if (b < 0) out += " - " + std::to_string(-b);
  return out;
}

std::string SerializeSimple(const SimpleSelector& simple) {
  using K = SimpleSelector::Kind;
  switch (simple.kind) {
    case K::Type:
    case K::Universal: {
      std::string out;
      if (simple.namespaceName) out += (*simple.namespaceName == "*" ? "*" : SerializeIdentifier(*simple.namespaceName)) + "|";
      return out + (simple.kind == K::Universal ? "*" : SerializeIdentifier(simple.name));
    }
    case K::Class: return "." + SerializeIdentifier(simple.name);
    case K::Id: return "#" + SerializeIdentifier(simple.name);
    case K::Attribute: {
      std::string out = "[";
      if (simple.namespaceName) out += (*simple.namespaceName == "*" ? "*" : SerializeIdentifier(*simple.namespaceName)) + "|";
      out += SerializeIdentifier(simple.name);
      static const char* const kOps[] = {"", "=", "~=", "|=", "^=", "$=", "*="};
      if (simple.op != AttributeOperator::Exists) {
        out += kOps[static_cast<int>(simple.op)];
        out += SerializeString(simple.value);
        if (simple.caseMode == CaseMode::Insensitive) out += " i";
        else if (simple.caseMode == CaseMode::Sensitive) out += " s";
      }
      return out + "]";
    }
    case K::Pseudo: return ":" + simple.name;
    case K::PseudoElement: return "::" + simple.name + (simple.value.empty() ? "" : "(" + simple.value + ")");
    case K::Unknown: return ":" + simple.name + (simple.value.empty() ? "" : "(" + simple.value + ")");
    case K::Not:
    case K::Is:
    case K::Where:
    case K::Has: {
      const char* name = simple.kind == K::Not ? "not" : simple.kind == K::Where ? "where" : simple.kind == K::Has ? "has" : "is";
      return std::string(":") + name + "(" + SerializeSelectorList(*simple.list) + ")";
    }
    case K::Nth: {
      const char* name = simple.ofType ? (simple.fromEnd ? "nth-last-of-type" : "nth-of-type") : (simple.fromEnd ? "nth-last-child" : "nth-child");
      std::string out = std::string(":") + name + "(" + SerializeAnPlusB(simple.a, simple.b);
      if (simple.list) out += " of " + SerializeSelectorList(*simple.list);
      return out + ")";
    }
    case K::Lang: {
      std::string out = ":lang(";
      for (size_t i = 0; i < simple.languages.size(); ++i) out += (i ? ", " : "") + SerializeString(simple.languages[i]);
      return out + ")";
    }
    case K::Dir: return ":dir(" + simple.value + ")";
  }
  return "";
}

const char* CombinatorText(Combinator c) {
  switch (c) {
    case Combinator::Child: return " > ";
    case Combinator::NextSibling: return " + ";
    case Combinator::SubsequentSibling: return " ~ ";
    default: return " ";
  }
}

}  // namespace

std::string SerializeComplexSelector(const ComplexSelector& selector) {
  std::string out;
  if (selector.relative && selector.leading != Combinator::Descendant) out += std::string(CombinatorText(selector.leading)).substr(1);
  for (size_t i = 0; i < selector.compounds.size(); ++i) {
    if (i > 0) out += CombinatorText(selector.combinators[i - 1]);
    for (const SimpleSelector& simple : selector.compounds[i].simples) out += SerializeSimple(simple);
  }
  return out;
}

std::string SerializeSelectorList(const SelectorList& list) {
  std::string out;
  for (size_t i = 0; i < list.size(); ++i) out += (i ? ", " : "") + SerializeComplexSelector(list[i]);
  return out;
}

}  // namespace solar::css
