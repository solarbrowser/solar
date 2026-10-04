#include "solar/html/Xml.h"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "solar/html/Entities.h"

namespace solar::html {

namespace {

using dom::Document;
using dom::Element;
using dom::Node;

// ---- Characters ----

bool IsXmlChar(uint32_t c) { return c == 0x9 || c == 0xA || c == 0xD || (c >= 0x20 && c <= 0xD7FF) || (c >= 0xE000 && c <= 0xFFFD) || (c >= 0x10000 && c <= 0x10FFFF); }

bool IsNameStart(uint32_t c) {
  return c == ':' || (c >= 'A' && c <= 'Z') || c == '_' || (c >= 'a' && c <= 'z') || (c >= 0xC0 && c <= 0xD6) || (c >= 0xD8 && c <= 0xF6) || (c >= 0xF8 && c <= 0x2FF) ||
         (c >= 0x370 && c <= 0x37D) || (c >= 0x37F && c <= 0x1FFF) || (c >= 0x200C && c <= 0x200D) || (c >= 0x2070 && c <= 0x218F) || (c >= 0x2C00 && c <= 0x2FEF) ||
         (c >= 0x3001 && c <= 0xD7FF) || (c >= 0xF900 && c <= 0xFDCF) || (c >= 0xFDF0 && c <= 0xFFFD) || (c >= 0x10000 && c <= 0xEFFFF);
}

bool IsNameChar(uint32_t c) { return IsNameStart(c) || c == '-' || c == '.' || (c >= '0' && c <= '9') || c == 0xB7 || (c >= 0x300 && c <= 0x36F) || (c >= 0x203F && c <= 0x2040); }

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// The code point at `pos` of UTF-8 text (WTF-8: a lone surrogate is three bytes), and its length; 0 at the end.
uint32_t DecodeAt(std::string_view text, size_t pos, size_t& length) {
  if (pos >= text.size()) {
    length = 0;
    return 0;
  }
  const unsigned char c = static_cast<unsigned char>(text[pos]);
  if (c < 0x80) {
    length = 1;
    return c;
  }
  size_t n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
  if (pos + n > text.size()) n = text.size() - pos;
  uint32_t cp = n == 4 ? c & 0x07 : n == 3 ? c & 0x0F : c & 0x1F;
  for (size_t i = 1; i < n; ++i) cp = (cp << 6) | (static_cast<unsigned char>(text[pos + i]) & 0x3F);
  length = n;
  return cp;
}

void AppendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

bool AllXmlChars(std::string_view text) {
  for (size_t i = 0; i < text.size();) {
    size_t length = 0;
    const uint32_t cp = DecodeAt(text, i, length);
    if (!IsXmlChar(cp)) return false;
    i += length;
  }
  return true;
}

bool IsXmlNameString(std::string_view name) {
  if (name.empty()) return false;
  for (size_t i = 0; i < name.size();) {
    size_t length = 0;
    const uint32_t cp = DecodeAt(name, i, length);
    if (i == 0 ? !IsNameStart(cp) : !IsNameChar(cp)) return false;
    i += length;
  }
  return true;
}

// ---- The parser ----

constexpr int kMaxDepth = 2000;
constexpr size_t kMaxExpansion = 1 << 22;

class XmlParser {
 public:
  XmlParser(Quanta::Context& ctx, Document* document, ScriptHandler scripts) : ctx_(ctx), document_(document), scripts_(std::move(scripts)) {}

  XmlResult ParseDocument(std::string_view markup) {
    Prepare(markup);
    if (text_.starts_with("\xEF\xBB\xBF")) pos_ = 3;
    if (!ParseProlog()) return Failure();
    return Success();
  }

  XmlResult ParseFragment(Element* context, std::string_view markup, dom::DocumentFragment* fragment) {
    Prepare(markup);
    // The namespaces in scope at the context: those its ancestors declare and those they were made in.
    std::vector<Element*> chain;
    for (Node* node = context; node; node = node->parentNode) {
      if (Element* element = dom::AsElement(node)) chain.push_back(element);
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      Element* element = *it;
      if (!element->namespaceUri.empty() || !element->prefix.empty()) Bind(element->prefix, element->namespaceUri);
      for (const dom::Attr* attribute : element->attributes) {
        if (attribute->namespaceUri != dom::kXmlnsNamespace) continue;
        if (attribute->prefix.empty()) Bind("", attribute->value);
        else Bind(attribute->localName, attribute->value);
      }
    }
    if (!ParseNodes(fragment, 0, false)) return Failure();
    FlushText(fragment);
    return Success();
  }

 private:
  struct Open {
    Element* element;
    std::string qualified;
    size_t bindings;  // how many namespace bindings there were before it
  };

  void Prepare(std::string_view markup) {
    // Line ends are one character: CRLF and CR are LF.
    text_.reserve(markup.size());
    for (size_t i = 0; i < markup.size(); ++i) {
      if (markup[i] == '\r') {
        text_ += '\n';
        if (i + 1 < markup.size() && markup[i + 1] == '\n') ++i;
      } else {
        text_ += markup[i];
      }
    }
    bindings_.emplace_back("xml", std::string(dom::kXmlNamespace));
  }

  XmlResult Success() { return {}; }
  XmlResult Failure() { return {false, error_}; }

  bool Fail(const std::string& message) {
    if (!error_.empty()) return false;
    size_t line = 1, column = 1;
    for (size_t i = 0; i < pos_ && i < text_.size(); ++i) {
      if (text_[i] == '\n') {
        ++line;
        column = 1;
      } else {
        ++column;
      }
    }
    error_ = message + " (line " + std::to_string(line) + ", column " + std::to_string(column) + ")";
    return false;
  }

  bool AtEnd() const { return pos_ >= text_.size(); }
  char Peek(size_t offset = 0) const { return pos_ + offset < text_.size() ? text_[pos_ + offset] : '\0'; }
  bool Looking(std::string_view prefix) const { return std::string_view(text_).substr(pos_, prefix.size()) == prefix; }

  void SkipSpace() {
    while (!AtEnd() && IsSpace(text_[pos_])) ++pos_;
  }
  bool RequireSpace() {
    if (AtEnd() || !IsSpace(text_[pos_])) return Fail("White space expected");
    SkipSpace();
    return true;
  }

  bool ParseName(std::string& out) {
    out.clear();
    size_t length = 0;
    uint32_t cp = DecodeAt(text_, pos_, length);
    if (length == 0 || !IsNameStart(cp)) return Fail("Name expected");
    while (length != 0 && IsNameChar(cp)) {
      out.append(text_, pos_, length);
      pos_ += length;
      cp = DecodeAt(text_, pos_, length);
    }
    return true;
  }

  // ---- The prolog, and what follows the root ----

  bool ParseProlog() {
    if (Looking("<?xml") && pos_ == (text_.starts_with("\xEF\xBB\xBF") ? 3u : 0u) && (IsSpace(Peek(5)) || Peek(5) == '?')) {
      const size_t end = text_.find("?>", pos_);
      if (end == std::string::npos) return Fail("Unterminated XML declaration");
      const std::string declaration = text_.substr(pos_ + 5, end - pos_ - 5);
      if (declaration.find("version") == std::string::npos) return Fail("The XML declaration needs a version");
      pos_ = end + 2;
    }
    bool sawDoctype = false, sawRoot = false;
    for (;;) {
      SkipSpace();
      if (AtEnd()) break;
      if (Looking("<!--")) {
        if (!ParseComment(document_)) return false;
      } else if (Looking("<?")) {
        if (!ParseProcessingInstruction(document_)) return false;
      } else if (Looking("<!DOCTYPE")) {
        if (sawDoctype || sawRoot) return Fail("Unexpected DOCTYPE");
        sawDoctype = true;
        if (!ParseDoctype()) return false;
      } else if (Peek() == '<' && !sawRoot) {
        sawRoot = true;
        if (!ParseNodes(document_, 0, true)) return false;
      } else {
        return Fail(sawRoot ? "Extra content at the end of the document" : "Start tag expected");
      }
    }
    if (!sawRoot) return Fail("Document is empty");
    return true;
  }

  bool ParseComment(Node* parent) {
    const size_t start = pos_ + 4;
    const size_t end = text_.find("--", start);
    if (end == std::string::npos) return Fail("Unterminated comment");
    if (text_.compare(end, 3, "-->") != 0) {
      pos_ = end;
      return Fail("'--' is not allowed in a comment");
    }
    const std::string data = text_.substr(start, end - start);
    if (!AllXmlChars(data)) return Fail("Invalid character in a comment");
    pos_ = end + 3;
    FlushText(parent);
    dom::InsertUnchecked(dom::NewComment(ctx_, document_, data), parent, nullptr);
    Retain();
    return true;
  }

  bool ParseProcessingInstruction(Node* parent) {
    pos_ += 2;
    std::string target;
    if (!ParseName(target)) return false;
    std::string lower = target;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "xml") return Fail("The XML declaration is allowed only at the start of the document");
    if (target.find(':') != std::string::npos) return Fail("A processing instruction target cannot have a colon");
    std::string data;
    if (!Looking("?>")) {
      if (!RequireSpace()) return false;
      const size_t end = text_.find("?>", pos_);
      if (end == std::string::npos) return Fail("Unterminated processing instruction");
      data = text_.substr(pos_, end - pos_);
      if (!AllXmlChars(data)) return Fail("Invalid character in a processing instruction");
      pos_ = end;
    }
    pos_ += 2;
    FlushText(parent);
    dom::InsertUnchecked(dom::NewProcessingInstruction(ctx_, document_, target, data), parent, nullptr);
    Retain();
    return true;
  }

  bool ParseQuoted(std::string& out) {
    const char quote = Peek();
    if (quote != '"' && quote != '\'') return Fail("A quoted string is expected");
    const size_t end = text_.find(quote, pos_ + 1);
    if (end == std::string::npos) return Fail("Unterminated quoted string");
    out = text_.substr(pos_ + 1, end - pos_ - 1);
    pos_ = end + 1;
    return true;
  }

  bool ParseDoctype() {
    pos_ += 9;
    if (!RequireSpace()) return false;
    std::string name;
    if (!ParseName(name)) return false;
    SkipSpace();
    std::string publicId, systemId;
    if (Looking("PUBLIC")) {
      pos_ += 6;
      if (!RequireSpace() || !ParseQuoted(publicId)) return false;
      if (!RequireSpace() || !ParseQuoted(systemId)) return false;
    } else if (Looking("SYSTEM")) {
      pos_ += 6;
      if (!RequireSpace() || !ParseQuoted(systemId)) return false;
    }
    SkipSpace();
    if (Peek() == '[') {
      ++pos_;
      if (!ParseInternalSubset()) return false;
      SkipSpace();
    }
    if (Peek() != '>') return Fail("'>' expected at the end of the DOCTYPE");
    ++pos_;
    dom::InsertUnchecked(dom::NewDocumentType(ctx_, document_, name, publicId, systemId), document_, nullptr);
    doctypePublic_ = publicId;
    return true;
  }

  // The declarations of the internal subset: the general entities are kept, the rest is passed over.
  bool ParseInternalSubset() {
    for (;;) {
      SkipSpace();
      if (AtEnd()) return Fail("Unterminated DOCTYPE");
      if (Peek() == ']') {
        ++pos_;
        return true;
      }
      if (Looking("<!--")) {
        const size_t end = text_.find("-->", pos_ + 4);
        if (end == std::string::npos) return Fail("Unterminated comment");
        pos_ = end + 3;
      } else if (Looking("<?")) {
        const size_t end = text_.find("?>", pos_ + 2);
        if (end == std::string::npos) return Fail("Unterminated processing instruction");
        pos_ = end + 2;
      } else if (Looking("<!ENTITY")) {
        pos_ += 8;
        if (!RequireSpace()) return false;
        const bool parameter = Peek() == '%';
        if (parameter) {
          ++pos_;
          if (!RequireSpace()) return false;
        }
        std::string name;
        if (!ParseName(name)) return false;
        if (!RequireSpace()) return false;
        std::string value;
        if (Peek() == '"' || Peek() == '\'') {
          if (!ParseQuoted(value)) return false;
          if (!parameter && entities_.find(name) == entities_.end()) entities_[name] = ExpandCharacterReferences(value);
        } else if (!SkipDeclaration()) {
          return false;
        }
        if (parameter) {
          SkipSpace();
          if (Peek() != '>') return Fail("'>' expected in an entity declaration");
          ++pos_;
          continue;
        }
        if (value.empty() && entities_.find(name) == entities_.end()) {
          // An external entity: it has nothing to give.
          entities_[name] = "";
        }
        SkipSpace();
        if (Peek() != '>') return Fail("'>' expected in an entity declaration");
        ++pos_;
      } else if (Looking("<!")) {
        if (!SkipDeclaration()) return false;
        if (Peek() != '>') return Fail("'>' expected");
        ++pos_;
      } else if (Peek() == '%') {
        const size_t end = text_.find(';', pos_);
        if (end == std::string::npos) return Fail("Unterminated parameter entity reference");
        pos_ = end + 1;
      } else {
        return Fail("Unexpected content in the DOCTYPE");
      }
    }
  }

  // To the '>' that ends the declaration, which it leaves, not looking into quoted strings.
  bool SkipDeclaration() {
    while (!AtEnd() && Peek() != '>') {
      if (Peek() == '"' || Peek() == '\'') {
        const size_t end = text_.find(Peek(), pos_ + 1);
        if (end == std::string::npos) return Fail("Unterminated quoted string");
        pos_ = end + 1;
      } else {
        ++pos_;
      }
    }
    return !AtEnd() || Fail("Unterminated declaration");
  }

  // The character references of an entity's literal value are replaced when it is declared.
  std::string ExpandCharacterReferences(const std::string& value) {
    std::string out;
    for (size_t i = 0; i < value.size(); ++i) {
      if (value[i] == '&' && i + 1 < value.size() && value[i + 1] == '#') {
        const size_t end = value.find(';', i);
        if (end != std::string::npos) {
          uint32_t cp = 0;
          if (CharacterReference(std::string_view(value).substr(i + 2, end - i - 2), cp)) {
            AppendUtf8(out, cp);
            i = end;
            continue;
          }
        }
      }
      out += value[i];
    }
    return out;
  }

  static bool CharacterReference(std::string_view digits, uint32_t& cp) {
    if (digits.empty()) return false;
    const bool hex = digits[0] == 'x';
    if (hex) digits.remove_prefix(1);
    if (digits.empty() || digits.size() > 8) return false;
    cp = 0;
    for (char c : digits) {
      uint32_t d;
      if (c >= '0' && c <= '9') d = c - '0';
      else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
      else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
      else return false;
      cp = cp * (hex ? 16 : 10) + d;
    }
    return IsXmlChar(cp);
  }

  // ---- Namespaces ----

  void Bind(const std::string& prefix, const std::string& ns) { bindings_.emplace_back(prefix, ns); }

  // The namespace a prefix is bound to; none (empty) for the default one if it is not bound.
  std::optional<std::string> Lookup(const std::string& prefix) const {
    for (auto it = bindings_.rbegin(); it != bindings_.rend(); ++it) {
      if (it->first == prefix) return it->second;
    }
    if (prefix.empty()) return std::string();
    return std::nullopt;
  }

  // ---- Content ----

  void Retain() {}

  void FlushText(Node* parent) {
    if (pendingText_.empty()) return;
    dom::InsertUnchecked(dom::NewText(ctx_, document_, std::move(pendingText_)), parent, nullptr);
    pendingText_.clear();
  }

  // The text and the nodes of `parent`, to the end of the text or, for a document's root, of its element.
  bool ParseNodes(Node* parent, int depth, bool singleElement) {
    if (depth > 20) return Fail("Entity nesting is too deep");
    std::vector<Open> stack;
    Node* container = parent;
    while (true) {
      if (AtEnd()) {
        if (!stack.empty()) return Fail("Premature end of data in tag " + stack.back().qualified);
        FlushText(container);
        return true;
      }
      const char c = Peek();
      if (c == '<') {
        if (Looking("</")) {
          if (stack.empty()) return Fail("Unexpected end tag");
          pos_ += 2;
          std::string name;
          if (!ParseName(name)) return false;
          SkipSpace();
          if (Peek() != '>') return Fail("'>' expected at the end of a tag");
          ++pos_;
          if (name != stack.back().qualified) return Fail("Opening and ending tag mismatch: " + stack.back().qualified + " and " + name);
          FlushText(container);
          Element* element = stack.back().element;
          bindings_.resize(stack.back().bindings);
          stack.pop_back();
          container = stack.empty() ? parent : stack.back().element;
          RunScript(element);
          if (stack.empty() && singleElement) return true;
        } else if (Looking("<!--")) {
          if (!ParseComment(container)) return false;
        } else if (Looking("<![CDATA[")) {
          const size_t end = text_.find("]]>", pos_ + 9);
          if (end == std::string::npos) return Fail("Unterminated CDATA section");
          const std::string data = text_.substr(pos_ + 9, end - pos_ - 9);
          if (!AllXmlChars(data)) return Fail("Invalid character in a CDATA section");
          if (stack.empty() && container->IsDocument()) return Fail("CDATA is not allowed outside the document element");
          pos_ = end + 3;
          FlushText(container);
          dom::InsertUnchecked(dom::NewCdataSection(ctx_, document_, data), container, nullptr);
        } else if (Looking("<?")) {
          if (!ParseProcessingInstruction(container)) return false;
        } else if (Looking("<!")) {
          return Fail("Unexpected markup declaration");
        } else {
          FlushText(container);
          if (stack.size() >= static_cast<size_t>(kMaxDepth)) return Fail("The document is nested too deeply");
          bool empty = false;
          Element* element = ParseStartTag(container, empty);
          if (!element) return false;
          if (empty) {
            bindings_.resize(pendingBindings_);
            RunScript(element);
            if (stack.empty() && singleElement) return true;
          } else {
            stack.push_back({element, openName_, pendingBindings_});
            container = element;
          }
        }
      } else if (c == '&') {
        if (stack.empty() && container->IsDocument()) return Fail("A reference is not allowed outside the document element");
        if (!ParseReference(container, depth)) return false;
      } else {
        // Text, to the next markup or reference.
        size_t end = pos_;
        while (end < text_.size() && text_[end] != '<' && text_[end] != '&') ++end;
        const std::string_view run = std::string_view(text_).substr(pos_, end - pos_);
        if (stack.empty() && container->IsDocument() ) {
          if (run.find_first_not_of(" \t\n") != std::string_view::npos) return Fail(singleElement ? "Extra content at the end of the document" : "Text is not allowed here");
        }
        if (run.find("]]>") != std::string_view::npos) return Fail("']]>' is not allowed in text");
        if (!AllXmlChars(run)) return Fail("Invalid character");
        pendingText_.append(run);
        pos_ = end;
      }
    }
  }

  void RunScript(Element* element) {
    if (!scripts_ || !element->IsHtml("script")) return;
    scripts_(element);
  }

  // A start tag: the element is made and put in `parent`; `empty` says it was an empty-element tag.
  Element* ParseStartTag(Node* parent, bool& empty) {
    ++pos_;
    std::string qualified;
    if (!ParseName(qualified)) return nullptr;
    struct RawAttribute {
      std::string name, value;
    };
    std::vector<RawAttribute> raw;
    for (;;) {
      const bool spaced = !AtEnd() && IsSpace(Peek());
      SkipSpace();
      if (Peek() == '>' || Looking("/>")) break;
      if (!spaced) {
        Fail("White space expected between attributes");
        return nullptr;
      }
      RawAttribute attribute;
      if (!ParseName(attribute.name)) return nullptr;
      SkipSpace();
      if (Peek() != '=') {
        Fail("Attribute " + attribute.name + " has no value");
        return nullptr;
      }
      ++pos_;
      SkipSpace();
      if (!ParseAttributeValue(attribute.value)) return nullptr;
      for (const RawAttribute& other : raw) {
        if (other.name == attribute.name) {
          Fail("Attribute " + attribute.name + " redefined");
          return nullptr;
        }
      }
      raw.push_back(std::move(attribute));
    }
    empty = Looking("/>");
    pos_ += empty ? 2 : 1;

    const size_t before = bindings_.size();
    pendingBindings_ = before;
    // The declarations first: they are in scope for the element itself.
    for (const RawAttribute& attribute : raw) {
      if (attribute.name == "xmlns") {
        if (attribute.value == dom::kXmlNamespace || attribute.value == dom::kXmlnsNamespace) {
          Fail("The namespace " + attribute.value + " cannot be the default namespace");
          return nullptr;
        }
        Bind("", attribute.value);
      } else if (attribute.name.starts_with("xmlns:")) {
        const std::string prefix = attribute.name.substr(6);
        if (prefix.empty() || prefix.find(':') != std::string::npos) {
          Fail("Invalid namespace declaration " + attribute.name);
          return nullptr;
        }
        if (prefix == "xmlns" || (prefix == "xml") != (attribute.value == dom::kXmlNamespace) || attribute.value == dom::kXmlnsNamespace) {
          Fail("The prefix " + prefix + " cannot be bound to " + attribute.value);
          return nullptr;
        }
        if (attribute.value.empty()) {
          Fail("A namespace prefix cannot be undeclared");
          return nullptr;
        }
        Bind(prefix, attribute.value);
      }
    }
    std::string prefix, local = qualified;
    if (const size_t colon = qualified.find(':'); colon != std::string::npos) {
      prefix = qualified.substr(0, colon);
      local = qualified.substr(colon + 1);
      if (prefix.empty() || local.empty() || local.find(':') != std::string::npos) {
        Fail("Invalid qualified name " + qualified);
        return nullptr;
      }
    }
    const std::optional<std::string> ns = Lookup(prefix);
    if (!ns) {
      Fail("Namespace prefix " + prefix + " on " + local + " is not defined");
      return nullptr;
    }
    Element* element = dom::NewElement(ctx_, document_, local, *ns, prefix);
    if (scripts_) retained_.Append(Quanta::Embed::FromObject(element));
    std::vector<std::pair<std::string, std::string>> seen;  // (namespace, local name) of those with a namespace
    for (const RawAttribute& attribute : raw) {
      std::string attributePrefix, attributeLocal = attribute.name, attributeNs;
      if (attribute.name == "xmlns") {
        attributeNs = std::string(dom::kXmlnsNamespace);
      } else if (const size_t colon = attribute.name.find(':'); colon != std::string::npos) {
        attributePrefix = attribute.name.substr(0, colon);
        attributeLocal = attribute.name.substr(colon + 1);
        if (attributePrefix.empty() || attributeLocal.empty() || attributeLocal.find(':') != std::string::npos) {
          Fail("Invalid qualified name " + attribute.name);
          return nullptr;
        }
        if (attributePrefix == "xmlns") {
          attributeNs = std::string(dom::kXmlnsNamespace);
        } else {
          const std::optional<std::string> resolved = Lookup(attributePrefix);
          if (!resolved) {
            Fail("Namespace prefix " + attributePrefix + " for " + attributeLocal + " is not defined");
            return nullptr;
          }
          attributeNs = *resolved;
        }
      }
      if (!attributeNs.empty()) {
        const auto key = std::make_pair(attributeNs, attributeLocal);
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
          Fail("Attribute " + attribute.name + " redefined");
          return nullptr;
        }
        seen.push_back(key);
      }
      dom::AppendAttr(element, dom::NewAttr(ctx_, document_, attributeNs, attributePrefix, attributeLocal, attribute.value));
    }
    openName_ = qualified;
    dom::InsertUnchecked(element, parent, nullptr);
    return element;
  }

  bool ParseAttributeValue(std::string& out) {
    const char quote = Peek();
    if (quote != '"' && quote != '\'') return Fail("Attribute value must be quoted");
    ++pos_;
    out.clear();
    while (true) {
      if (AtEnd()) return Fail("Unterminated attribute value");
      const char c = Peek();
      if (c == quote) {
        ++pos_;
        return true;
      }
      if (c == '<') return Fail("'<' is not allowed in an attribute value");
      if (c == '&') {
        std::string replaced;
        if (!ReadReference(replaced, true, 0)) return false;
        out += replaced;
        continue;
      }
      size_t length = 0;
      const uint32_t cp = DecodeAt(text_, pos_, length);
      if (!IsXmlChar(cp)) return Fail("Invalid character in an attribute value");
      // Whitespace is a space.
      if (c == '\n' || c == '\t') out += ' ';
      else out.append(text_, pos_, length);
      pos_ += length;
    }
  }

  // After "&": the reference's text, for an attribute value (`attribute`) or for content that is only text.
  bool ReadReference(std::string& out, bool attribute, int depth) {
    ++pos_;
    if (Peek() == '#') {
      const size_t end = text_.find(';', pos_);
      if (end == std::string::npos) return Fail("Unterminated character reference");
      uint32_t cp = 0;
      if (!CharacterReference(std::string_view(text_).substr(pos_ + 1, end - pos_ - 1), cp)) return Fail("Invalid character reference");
      AppendUtf8(out, cp);
      pos_ = end + 1;
      return true;
    }
    std::string name;
    if (!ParseName(name)) return false;
    if (Peek() != ';') return Fail("Entity reference to " + name + " is not terminated");
    ++pos_;
    if (name == "lt") out += '<';
    else if (name == "gt") out += '>';
    else if (name == "amp") out += '&';
    else if (name == "quot") out += '"';
    else if (name == "apos") out += '\'';
    else {
      std::string replacement;
      if (!Entity(name, replacement)) return false;
      if (!attribute) {
        out += replacement;
        return true;
      }
      // An entity in an attribute value is expanded where it is, and may not hold markup.
      if (replacement.find('<') != std::string::npos) return Fail("'<' is not allowed in an attribute value");
      if (depth > 10 || expanded_ > kMaxExpansion) return Fail("Entity expansion is too large");
      expanded_ += replacement.size();
      std::string expanded;
      XmlParser inner(ctx_, document_, nullptr);
      inner.entities_ = entities_;
      inner.text_ = replacement;
      inner.expanded_ = expanded_;
      while (!inner.AtEnd()) {
        if (inner.Peek() == '&') {
          std::string piece;
          if (!inner.ReadReference(piece, true, depth + 1)) return Fail(inner.error_);
          expanded += piece;
        } else {
          const char c = inner.Peek();
          expanded += (c == '\n' || c == '\t') ? ' ' : c;
          ++inner.pos_;
        }
      }
      out += expanded;
    }
    return true;
  }

  bool Entity(const std::string& name, std::string& replacement) {
    const auto found = entities_.find(name);
    if (found != entities_.end()) {
      replacement = found->second;
      return true;
    }
    // The XHTML doctypes define the named entities of HTML.
    if (doctypePublic_.find("XHTML") != std::string::npos || doctypePublic_.find("xhtml") != std::string::npos) {
      if (const NamedEntity* entity = FindEntity(name + ";")) {
        AppendUtf8(replacement, entity->first);
        if (entity->second) AppendUtf8(replacement, entity->second);
        return true;
      }
    }
    return Fail("Entity '" + name + "' not defined");
  }

  bool ParseReference(Node* container, int depth) {
    const size_t at = pos_;
    ++pos_;
    if (Peek() != '#') {
      std::string name;
      if (!ParseName(name)) return false;
      const auto found = entities_.find(name);
      if (Peek() == ';' && found != entities_.end() && found->second.find_first_of("<&") != std::string::npos) {
        // An entity with markup in it: its replacement text is content.
        ++pos_;
        if (depth > 10 || expanded_ > kMaxExpansion) return Fail("Entity expansion is too large");
        expanded_ += found->second.size();
        std::string saved = std::move(text_);
        const size_t savedPos = pos_;
        text_ = found->second;
        pos_ = 0;
        const bool ok = ParseNodes(container, depth + 1, false);
        text_ = std::move(saved);
        pos_ = savedPos;
        return ok;
      }
    }
    pos_ = at;
    std::string out;
    if (!ReadReference(out, false, depth)) return false;
    pendingText_ += out;
    return true;
  }

  Quanta::Context& ctx_;
  Document* document_;
  ScriptHandler scripts_;
  std::string text_;
  size_t pos_ = 0;
  std::string error_;
  std::string pendingText_;
  std::vector<std::pair<std::string, std::string>> bindings_;
  size_t pendingBindings_ = 0;
  std::string openName_;
  std::map<std::string, std::string> entities_;
  std::string doctypePublic_;
  size_t expanded_ = 0;
  Quanta::Embed::ValueList retained_;
};

// ---- The serializer ----

struct Serialization {
  using PrefixMap = std::map<std::string, std::vector<std::string>>;
  static std::string Key(const std::optional<std::string>& ns) { return ns ? "+" + *ns : "-"; }

  static bool Has(const PrefixMap& map, const std::optional<std::string>& ns) { return map.find(Key(ns)) != map.end(); }
  static void Add(PrefixMap& map, const std::string& prefix, const std::optional<std::string>& ns) { map[Key(ns)].push_back(prefix); }
  static bool Found(const PrefixMap& map, const std::string& prefix, const std::optional<std::string>& ns) {
    const auto it = map.find(Key(ns));
    return it != map.end() && std::find(it->second.begin(), it->second.end(), prefix) != it->second.end();
  }
  static std::optional<std::string> Preferred(const PrefixMap& map, const std::optional<std::string>& preferred, const std::optional<std::string>& ns) {
    const auto it = map.find(Key(ns));
    if (it == map.end()) return std::nullopt;
    if (preferred) {
      for (const std::string& candidate : it->second) {
        if (candidate == *preferred) return candidate;
      }
    }
    return it->second.back();
  }
  static std::string Generate(PrefixMap& map, const std::optional<std::string>& ns, int& index) {
    const std::string generated = "ns" + std::to_string(index++);
    Add(map, generated, ns);
    return generated;
  }

  bool requireWellFormed = false;
  bool failed = false;

  void Fail() { failed = true; }

  static std::string EscapeText(const std::string& text) {
    std::string out;
    for (char c : text) {
      if (c == '&') out += "&amp;";
      else if (c == '<') out += "&lt;";
      else if (c == '>') out += "&gt;";
      else out += c;
    }
    return out;
  }

  std::string AttributeValue(const std::optional<std::string>& value) {
    if (!value) return "";
    if (requireWellFormed && !AllXmlChars(*value)) Fail();
    std::string out;
    for (char c : *value) {
      if (c == '&') out += "&amp;";
      else if (c == '"') out += "&quot;";
      else if (c == '<') out += "&lt;";
      else if (c == '\t') out += "&#9;";
      else if (c == '\n') out += "&#xA;";
      else if (c == '\r') out += "&#xD;";
      else out += c;
    }
    return out;
  }

  static std::optional<std::string> Ns(const std::string& ns) { return ns.empty() ? std::nullopt : std::optional<std::string>(ns); }

  std::string Quote(const std::string& id) { return (id.find('"') != std::string::npos ? "'" : "\"") + id + (id.find('"') != std::string::npos ? "'" : "\""); }

  std::string Node(const dom::Node* node, std::optional<std::string> ns, PrefixMap& map, int& index) {
    switch (node->nodeType) {
      case dom::NodeType::Element: return ElementNode(static_cast<const Element*>(node), ns, map, index);
      case dom::NodeType::Document:
        if (requireWellFormed && !static_cast<const Document*>(node)->DocumentElement()) Fail();
        return Children(node, ns, map, index);
      case dom::NodeType::DocumentFragment: return Children(node, ns, map, index);
      case dom::NodeType::Comment: {
        const std::string& data = static_cast<const dom::CharacterData*>(node)->data;
        if (requireWellFormed && (!AllXmlChars(data) || data.find("--") != std::string::npos || data.ends_with("-"))) Fail();
        return "<!--" + data + "-->";
      }
      case dom::NodeType::CdataSection: return "<![CDATA[" + static_cast<const dom::CharacterData*>(node)->data + "]]>";
      case dom::NodeType::Text: {
        const std::string& data = static_cast<const dom::CharacterData*>(node)->data;
        if (requireWellFormed && !AllXmlChars(data)) Fail();
        return EscapeText(data);
      }
      case dom::NodeType::DocumentType: {
        const auto* doctype = static_cast<const dom::DocumentType*>(node);
        if (requireWellFormed) {
          for (char c : doctype->publicId) {
            const bool pubid = c == ' ' || c == '\r' || c == '\n' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || std::string_view("-'()+,./:=?;!*#@$_%").find(c) != std::string_view::npos;
            if (!pubid) Fail();
          }
          if (!AllXmlChars(doctype->systemId) || (doctype->systemId.find('"') != std::string::npos && doctype->systemId.find('\'') != std::string::npos)) Fail();
        }
        std::string markup = "<!DOCTYPE " + doctype->name;
        if (!doctype->publicId.empty()) markup += " PUBLIC " + Quote(doctype->publicId);
        if (!doctype->systemId.empty() && doctype->publicId.empty()) markup += " SYSTEM";
        if (!doctype->systemId.empty()) markup += " " + Quote(doctype->systemId);
        return markup + ">";
      }
      case dom::NodeType::ProcessingInstruction: {
        const auto* pi = static_cast<const dom::CharacterData*>(node);
        if (requireWellFormed) {
          std::string lower = pi->target;
          for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
          if (pi->target.find(':') != std::string::npos || lower == "xml") Fail();
          if (!AllXmlChars(pi->data) || pi->data.find("?>") != std::string::npos) Fail();
        }
        return "<?" + pi->target + " " + pi->data + "?>";
      }
      default: return "";
    }
  }

  std::string Children(const dom::Node* node, std::optional<std::string> ns, PrefixMap& map, int& index) {
    std::string out;
    for (const dom::Node* child = node->firstChild; child; child = child->nextSibling) out += Node(child, ns, map, index);
    return out;
  }

  std::string ElementNode(const Element* element, std::optional<std::string> ns, PrefixMap& parentMap, int& index) {
    if (requireWellFormed && (element->localName.find(':') != std::string::npos || !IsXmlNameString(element->localName))) Fail();
    std::string markup = "<";
    std::string qualified;
    bool skipEndTag = false, ignoreNamespaceDefinition = false;
    PrefixMap map = parentMap;
    std::map<std::string, std::string> localPrefixes;
    // Recording the namespace information.
    std::optional<std::string> localDefault;
    for (const dom::Attr* attribute : element->attributes) {
      if (attribute->namespaceUri != dom::kXmlnsNamespace) continue;
      if (attribute->prefix.empty()) {
        localDefault = attribute->value;
        continue;
      }
      const std::string prefixDefinition = attribute->localName;
      std::optional<std::string> namespaceDefinition = attribute->value;
      if (*namespaceDefinition == dom::kXmlNamespace) continue;
      if (namespaceDefinition->empty()) namespaceDefinition = std::nullopt;
      if (Found(map, prefixDefinition, namespaceDefinition)) continue;
      Add(map, prefixDefinition, namespaceDefinition);
      localPrefixes[prefixDefinition] = namespaceDefinition ? *namespaceDefinition : "";
    }
    std::optional<std::string> inherited = ns;
    const std::optional<std::string> elementNs = Ns(element->namespaceUri);
    if (inherited == elementNs) {
      if (localDefault) ignoreNamespaceDefinition = true;
      if (elementNs && *elementNs == dom::kXmlNamespace) qualified += "xml:";
      qualified += element->localName;
      markup += qualified;
    } else {
      std::optional<std::string> prefix = element->prefix.empty() ? std::nullopt : std::optional<std::string>(element->prefix);
      std::optional<std::string> candidate = (!prefix && localDefault && elementNs && *localDefault == *elementNs) ? std::nullopt : Preferred(map, prefix, elementNs);
      if (prefix && *prefix == "xmlns") {
        if (requireWellFormed) Fail();
        candidate = prefix;
      }
      if (candidate) {
        qualified += *candidate + ":" + element->localName;
        if (localDefault && *localDefault != dom::kXmlNamespace) inherited = localDefault->empty() ? std::nullopt : localDefault;
        markup += qualified;
      } else if (prefix) {
        std::string use = *prefix;
        if (localPrefixes.find(use) != localPrefixes.end()) use = Generate(map, elementNs, index);
        else Add(map, use, elementNs);
        qualified += use + ":" + element->localName;
        markup += qualified;
        markup += " xmlns:" + use + "=\"" + AttributeValue(elementNs ? *elementNs : std::string()) + "\"";
        if (localDefault) inherited = localDefault->empty() ? std::nullopt : localDefault;
      } else if (!localDefault || (elementNs ? *localDefault != *elementNs : !localDefault->empty())) {
        ignoreNamespaceDefinition = true;
        qualified += element->localName;
        inherited = elementNs;
        markup += qualified;
        markup += " xmlns=\"" + AttributeValue(elementNs ? *elementNs : std::string()) + "\"";
      } else {
        qualified += element->localName;
        inherited = elementNs;
        markup += qualified;
      }
    }
    markup += Attributes(element, map, index, localPrefixes, ignoreNamespaceDefinition);
    static const char* const kVoid[] = {"area", "base", "basefont", "bgsound", "br", "col", "embed", "frame", "hr", "img", "input", "keygen", "link", "menuitem", "meta", "param", "source", "track", "wbr"};
    const bool html = elementNs && *elementNs == dom::kHtmlNamespace;
    if (html && !element->firstChild && std::find_if(std::begin(kVoid), std::end(kVoid), [&](const char* name) { return element->localName == name; }) != std::end(kVoid)) {
      markup += " /";
      skipEndTag = true;
    } else if (!html && !element->firstChild) {
      markup += "/";
      skipEndTag = true;
    }
    markup += ">";
    if (skipEndTag) return markup;
    if (html && element->localName == "template" && element->templateContents) {
      markup += Children(element->templateContents, inherited, map, index);
    } else {
      markup += Children(element, inherited, map, index);
    }
    return markup + "</" + qualified + ">";
  }

  std::string Attributes(const Element* element, PrefixMap& map, int& index, std::map<std::string, std::string>& localPrefixes, bool ignoreNamespaceDefinition) {
    std::string result;
    std::vector<std::pair<std::string, std::string>> localNameSet;
    for (const dom::Attr* attribute : element->attributes) {
      const std::optional<std::string> attributeNs = Ns(attribute->namespaceUri);
      const auto name = std::make_pair(attribute->namespaceUri, attribute->localName);
      if (requireWellFormed && std::find(localNameSet.begin(), localNameSet.end(), name) != localNameSet.end()) Fail();
      localNameSet.push_back(name);
      std::optional<std::string> candidate;
      const std::optional<std::string> prefix = attribute->prefix.empty() ? std::nullopt : std::optional<std::string>(attribute->prefix);
      if (attributeNs) {
        candidate = Preferred(map, prefix, attributeNs);
        if (*attributeNs == dom::kXmlnsNamespace) {
          const auto local = localPrefixes.find(attribute->localName);
          if (attribute->value == dom::kXmlNamespace) continue;
          if (!prefix && ignoreNamespaceDefinition) continue;
          if (prefix && (local == localPrefixes.end() || local->second != attribute->value) && Found(map, attribute->localName, attribute->value.empty() ? std::nullopt : std::optional<std::string>(attribute->value))) continue;
          if (requireWellFormed && attribute->value == dom::kXmlnsNamespace) Fail();
          if (requireWellFormed && attribute->value.empty()) Fail();
          if (prefix && *prefix == "xmlns") candidate = prefix;
        } else if (!candidate) {
          if (prefix && localPrefixes.find(*prefix) == localPrefixes.end()) candidate = prefix;
          else candidate = Generate(map, attributeNs, index);
          if (prefix && candidate == prefix) Add(map, *candidate, attributeNs);
          localPrefixes[*candidate] = *attributeNs;
          result += " xmlns:" + *candidate + "=\"" + AttributeValue(*attributeNs) + "\"";
        }
      }
      result += " ";
      if (candidate) result += *candidate + ":";
      if (requireWellFormed && (attribute->localName.find(':') != std::string::npos || !IsXmlNameString(attribute->localName) || (attribute->localName == "xmlns" && !attributeNs))) Fail();
      result += attribute->localName + "=\"" + AttributeValue(attribute->value) + "\"";
    }
    return result;
  }
};

}  // namespace

XmlResult ParseXmlDocument(Quanta::Context& ctx, dom::Document* document, std::string_view markup, ScriptHandler scripts) {
  XmlParser parser(ctx, document, std::move(scripts));
  return parser.ParseDocument(markup);
}

XmlResult ParseXmlFragment(Quanta::Context& ctx, dom::Element* context, std::string_view markup, dom::DocumentFragment* fragment) {
  XmlParser parser(ctx, context->nodeDocument, nullptr);
  return parser.ParseFragment(context, markup, fragment);
}

std::optional<std::string> SerializeXml(const dom::Node* node, bool requireWellFormed) {
  Serialization serialization;
  serialization.requireWellFormed = requireWellFormed;
  Serialization::PrefixMap map;
  Serialization::Add(map, "xml", std::string(dom::kXmlNamespace));
  int index = 1;
  std::string out = serialization.Node(node, std::nullopt, map, index);
  if (serialization.failed) return std::nullopt;
  return out;
}

std::optional<std::string> SerializeXmlChildren(const dom::Node* node, bool requireWellFormed) {
  Serialization serialization;
  serialization.requireWellFormed = requireWellFormed;
  Serialization::PrefixMap map;
  Serialization::Add(map, "xml", std::string(dom::kXmlNamespace));
  int index = 1;
  std::string out = serialization.Children(node, std::nullopt, map, index);
  if (serialization.failed) return std::nullopt;
  return out;
}

}  // namespace solar::html
