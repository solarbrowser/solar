#include "solar/html/TreeBuilder.h"

#include <algorithm>

#include "solar/html/Tables.h"

namespace solar::html {

using dom::Element;
using dom::Node;
namespace qe = Quanta::Embed;

namespace {

constexpr std::string_view kHtml = dom::kHtmlNamespace;
constexpr std::string_view kSvg = dom::kSvgNamespace;
constexpr std::string_view kMathMl = dom::kMathMlNamespace;

bool IsOneOf(std::string_view name, std::initializer_list<std::string_view> names) { return std::find(names.begin(), names.end(), name) != names.end(); }

bool IsHtmlElement(const Element* element, std::string_view name) { return element && element->namespaceUri == kHtml && element->localName == name; }
bool IsHtmlElementOneOf(const Element* element, std::initializer_list<std::string_view> names) {
  return element && element->namespaceUri == kHtml && IsOneOf(element->localName, names);
}

bool IsWhitespaceCharacter(const Token& token) {
  return token.type == Token::Type::Character && token.data.size() == 1 && (token.data[0] == '\t' || token.data[0] == '\n' || token.data[0] == '\f' || token.data[0] == '\r' || token.data[0] == ' ');
}
bool IsNullCharacter(const Token& token) { return token.type == Token::Type::Character && token.data.size() == 1 && token.data[0] == '\0'; }
bool IsStart(const Token& token, std::string_view name) { return token.type == Token::Type::StartTag && token.name == name; }
bool IsEnd(const Token& token, std::string_view name) { return token.type == Token::Type::EndTag && token.name == name; }
bool IsStartOneOf(const Token& token, std::initializer_list<std::string_view> names) { return token.type == Token::Type::StartTag && IsOneOf(token.name, names); }
bool IsEndOneOf(const Token& token, std::initializer_list<std::string_view> names) { return token.type == Token::Type::EndTag && IsOneOf(token.name, names); }

std::string LowerAscii(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 0x20);
  }
  return out;
}

bool StartsWithLowered(const std::string& value, const char* prefix) { return value.starts_with(LowerAscii(prefix)); }

Token FakeStartTag(std::string_view name) {
  Token token;
  token.type = Token::Type::StartTag;
  token.name = name;
  return token;
}

// The mode a DOCTYPE token puts the document in.
dom::Document::Mode ModeForDoctype(const Token& token) {
  using Mode = dom::Document::Mode;
  const std::string name = token.doctypeName.value_or("");
  const bool hasPublic = token.publicId.has_value();
  const bool hasSystem = token.systemId.has_value();
  const std::string publicId = LowerAscii(token.publicId.value_or(""));
  const std::string systemId = LowerAscii(token.systemId.value_or(""));
  if (token.forceQuirks || !token.doctypeName || name != "html") return Mode::Quirks;
  for (const char* exact : kQuirksPublicExact) {
    if (hasPublic && publicId == LowerAscii(exact)) return Mode::Quirks;
  }
  for (const char* exact : kQuirksSystemExact) {
    if (hasSystem && systemId == LowerAscii(exact)) return Mode::Quirks;
  }
  for (const char* prefix : kQuirksPublicPrefixes) {
    if (hasPublic && StartsWithLowered(publicId, prefix)) return Mode::Quirks;
  }
  if (!hasSystem || systemId.empty()) {
    for (const char* prefix : kQuirksPublicPrefixesWithoutSystem) {
      if (hasPublic && StartsWithLowered(publicId, prefix)) return Mode::Quirks;
    }
  }
  for (const char* prefix : kLimitedQuirksPublicPrefixes) {
    if (hasPublic && StartsWithLowered(publicId, prefix)) return Mode::LimitedQuirks;
  }
  if (hasSystem && !systemId.empty()) {
    for (const char* prefix : kLimitedQuirksPublicPrefixesWithSystem) {
      if (hasPublic && StartsWithLowered(publicId, prefix)) return Mode::LimitedQuirks;
    }
  }
  return Mode::NoQuirks;
}

}  // namespace

TreeBuilder::TreeBuilder(Quanta::Context& ctx, dom::Document* document, std::string_view markup, ScriptingMode scripting)
    : ctx_(ctx), document_(document), tokenizer_(markup), scripting_(scripting) {}

void TreeBuilder::SetScriptHandler(ScriptHandler handler) { scriptHandler_ = std::move(handler); }

std::vector<std::string> TreeBuilder::Errors() const {
  std::vector<std::string> all = tokenizer_.errors();
  all.insert(all.end(), errors_.begin(), errors_.end());
  return all;
}

// ---- The loop and the dispatcher ----

void TreeBuilder::Run() {
  while (!done_) {
    const Element* adjusted = AdjustedCurrentNode();
    tokenizer_.SetCdataAllowed(adjusted && adjusted->namespaceUri != kHtml);
    Token token = tokenizer_.Next();
    const bool end = token.type == Token::Type::EndOfFile;
    Process(token);
    if (end) done_ = true;
  }
}

void TreeBuilder::Process(Token& token) {
  if (skipNextLineFeed_) {
    skipNextLineFeed_ = false;
    if (token.type == Token::Type::Character && token.data == "\n") return;
  }
  const Element* adjusted = AdjustedCurrentNode();
  bool html = open_.empty() || adjusted->namespaceUri == kHtml || token.type == Token::Type::EndOfFile;
  if (!html) {
    const bool start = token.type == Token::Type::StartTag;
    const bool character = token.type == Token::Type::Character;
    if (IsMathTextIntegrationPoint(adjusted)) {
      if (start && token.name != "mglyph" && token.name != "malignmark") html = true;
      if (character) html = true;
    }
    if (adjusted->namespaceUri == kMathMl && adjusted->localName == "annotation-xml" && IsStart(token, "svg")) html = true;
    if (IsHtmlIntegrationPoint(adjusted) && (start || character)) html = true;
  }
  if (html) ProcessUsing(mode_, token);
  else ProcessForeign(token);
}

void TreeBuilder::ProcessUsing(Mode mode, Token& token) {
  switch (mode) {
    case Mode::Initial: Initial(token); break;
    case Mode::BeforeHtml: BeforeHtml(token); break;
    case Mode::BeforeHead: BeforeHead(token); break;
    case Mode::InHead: InHead(token); break;
    case Mode::InHeadNoscript: InHeadNoscript(token); break;
    case Mode::AfterHead: AfterHead(token); break;
    case Mode::InBody: InBody(token); break;
    case Mode::Text: TextMode(token); break;
    case Mode::InTable: InTable(token); break;
    case Mode::InTableText: InTableText(token); break;
    case Mode::InCaption: InCaption(token); break;
    case Mode::InColumnGroup: InColumnGroup(token); break;
    case Mode::InTableBody: InTableBody(token); break;
    case Mode::InRow: InRow(token); break;
    case Mode::InCell: InCell(token); break;
    case Mode::InTemplate: InTemplate(token); break;
    case Mode::AfterBody: AfterBody(token); break;
    case Mode::InFrameset: InFrameset(token); break;
    case Mode::AfterFrameset: AfterFrameset(token); break;
    case Mode::AfterAfterBody: AfterAfterBody(token); break;
    case Mode::AfterAfterFrameset: AfterAfterFrameset(token); break;
  }
}

// ---- The stack of open elements ----

Element* TreeBuilder::AdjustedCurrentNode() const {
  if (fragmentContext_ && open_.size() == 1) return fragmentContext_;
  return CurrentNode();
}

void TreeBuilder::PopUntilHtml(std::string_view name) {
  while (!open_.empty()) {
    Element* popped = open_.back();
    open_.pop_back();
    if (IsHtmlElement(popped, name)) return;
  }
}

void TreeBuilder::PopUntilOneOf(std::initializer_list<std::string_view> names) {
  while (!open_.empty()) {
    Element* popped = open_.back();
    open_.pop_back();
    if (IsHtmlElementOneOf(popped, names)) return;
  }
}

void TreeBuilder::RemoveFromStack(Element* element) {
  const auto found = std::find(open_.begin(), open_.end(), element);
  if (found != open_.end()) open_.erase(found);
}

bool TreeBuilder::InStack(const Element* element) const { return std::find(open_.begin(), open_.end(), element) != open_.end(); }

bool TreeBuilder::IsScopeBoundary(const Element* element, Scope scope) const {
  if (scope == Scope::Table) return IsHtmlElementOneOf(element, {"html", "table", "template"});
  if (element->namespaceUri == kHtml) {
    if (IsOneOf(element->localName, {"applet", "caption", "html", "table", "td", "th", "marquee", "object", "select", "template"})) return true;
    if (scope == Scope::ListItem && IsOneOf(element->localName, {"ol", "ul"})) return true;
    if (scope == Scope::Button && element->localName == "button") return true;
    return false;
  }
  if (element->namespaceUri == kMathMl) return IsOneOf(element->localName, {"mi", "mo", "mn", "ms", "mtext", "annotation-xml"});
  if (element->namespaceUri == kSvg) return IsOneOf(element->localName, {"foreignObject", "desc", "title"});
  return false;
}

bool TreeBuilder::HasInScope(std::string_view name, Scope scope) const {
  for (size_t i = open_.size(); i-- > 0;) {
    if (IsHtmlElement(open_[i], name)) return true;
    if (IsScopeBoundary(open_[i], scope)) return false;
  }
  return false;
}

bool TreeBuilder::HasInScopeOneOf(std::initializer_list<std::string_view> names, Scope scope) const {
  for (size_t i = open_.size(); i-- > 0;) {
    if (IsHtmlElementOneOf(open_[i], names)) return true;
    if (IsScopeBoundary(open_[i], scope)) return false;
  }
  return false;
}

bool TreeBuilder::HasInScopeElement(const Element* element) const {
  for (size_t i = open_.size(); i-- > 0;) {
    if (open_[i] == element) return true;
    if (IsScopeBoundary(open_[i], Scope::Default)) return false;
  }
  return false;
}

bool TreeBuilder::HasTemplateInStack() const {
  return std::any_of(open_.begin(), open_.end(), [](const Element* element) { return IsHtmlElement(element, "template"); });
}

bool TreeBuilder::ParsingTemplateContents() const { return HasTemplateInStack() || IsHtmlElement(fragmentContext_, "template"); }

void TreeBuilder::GenerateImpliedEndTags(std::string_view except) {
  while (CurrentNode() && IsHtmlElementOneOf(CurrentNode(), {"dd", "dt", "li", "optgroup", "option", "p", "rb", "rp", "rt", "rtc"}) && CurrentNode()->localName != except) Pop();
}

void TreeBuilder::GenerateAllImpliedEndTagsThoroughly() {
  while (CurrentNode() &&
         IsHtmlElementOneOf(CurrentNode(), {"caption", "colgroup", "dd", "dt", "li", "optgroup", "option", "p", "rb", "rp", "rt", "rtc", "tbody", "td", "tfoot", "th", "thead", "tr"})) {
    Pop();
  }
}

void TreeBuilder::ClosePElement() {
  GenerateImpliedEndTags("p");
  if (!IsHtmlElement(CurrentNode(), "p")) Error("end-tag-too-early");
  PopUntilHtml("p");
}

void TreeBuilder::ClearStackBackTo(std::initializer_list<std::string_view> names) {
  while (CurrentNode() && !IsHtmlElementOneOf(CurrentNode(), names) && !IsHtmlElement(CurrentNode(), "template") && !IsHtmlElement(CurrentNode(), "html")) Pop();
}

void TreeBuilder::ResetInsertionMode() {
  bool last = false;
  size_t index = open_.size() - 1;
  Element* node = open_[index];
  for (;;) {
    if (index == 0) {
      last = true;
      if (fragmentContext_) node = fragmentContext_;
    }
    if (IsHtmlElementOneOf(node, {"td", "th"}) && !last) { mode_ = Mode::InCell; return; }
    if (IsHtmlElement(node, "tr")) { mode_ = Mode::InRow; return; }
    if (IsHtmlElementOneOf(node, {"tbody", "thead", "tfoot"})) { mode_ = Mode::InTableBody; return; }
    if (IsHtmlElement(node, "caption")) { mode_ = Mode::InCaption; return; }
    if (IsHtmlElement(node, "colgroup")) { mode_ = Mode::InColumnGroup; return; }
    if (IsHtmlElement(node, "table")) { mode_ = Mode::InTable; return; }
    if (IsHtmlElement(node, "template")) { mode_ = templateModes_.back(); return; }
    if (IsHtmlElement(node, "head") && !last) { mode_ = Mode::InHead; return; }
    if (IsHtmlElement(node, "body")) { mode_ = Mode::InBody; return; }
    if (IsHtmlElement(node, "frameset")) { mode_ = Mode::InFrameset; return; }
    if (IsHtmlElement(node, "html")) {
      mode_ = head_ ? Mode::AfterHead : Mode::BeforeHead;
      return;
    }
    if (last) { mode_ = Mode::InBody; return; }
    node = open_[--index];
  }
}

// ---- Creating and inserting nodes ----

TreeBuilder::Location TreeBuilder::AppropriatePlace(Node* overrideTarget) const {
  Node* target = overrideTarget ? overrideTarget : CurrentNode();
  Node* reference = nullptr;
  Element* targetElement = AsElement(target);
  if (fosterParenting_ && IsHtmlElementOneOf(targetElement, {"table", "tbody", "tfoot", "thead", "tr"})) {
    int lastTemplateOrTable = -1;
    for (size_t i = open_.size(); i-- > 0;) {
      if (IsHtmlElementOneOf(open_[i], {"template", "table"})) {
        lastTemplateOrTable = static_cast<int>(i);
        break;
      }
    }
    if (lastTemplateOrTable < 0) return {open_[0], nullptr};
    Element* found = open_[lastTemplateOrTable];
    if (IsHtmlElement(found, "template")) {
      target = found;
    } else if (found->parentNode) {
      target = found->parentNode;
      reference = found;
    } else {
      target = open_[lastTemplateOrTable - 1];
    }
  }
  if (!IsHtmlElement(AsElement(target), "template")) return {target, reference};
  return {static_cast<Element*>(target)->templateContents, nullptr};
}

TreeBuilder::Location TreeBuilder::AdjustedInsertionLocation(Node* overrideTarget) const {
  Location location = AppropriatePlace(overrideTarget);
  if (!open_.empty() && location.parent == open_[0] && rootInsertionTarget_) return {rootInsertionTarget_, nullptr};
  return location;
}

Element* TreeBuilder::CreateElementForToken(const Token& token, std::string_view ns, Node* intendedParent) {
  dom::Document* document = intendedParent && intendedParent->IsDocument() ? static_cast<dom::Document*>(intendedParent) : intendedParent ? intendedParent->nodeDocument : document_;
  if (!document) document = document_;
  Element* element = dom::NewElement(ctx_, document, token.name, ns);
  for (const TokenAttribute& attribute : token.attributes) {
    dom::Attr* node = attribute.localName.empty() ? dom::NewAttr(ctx_, document, "", "", attribute.name, attribute.value)
                                                  : dom::NewAttr(ctx_, document, attribute.namespaceUri, attribute.prefix, attribute.localName, attribute.value);
    node->ownerElement = element;
    element->attributes.push_back(node);
  }
  element->NoteWrite();
  if (scriptHandler_) retained_.Append(qe::FromObject(element));
  return element;
}

void TreeBuilder::InsertElementAt(Element* element, Location location) {
  Node* parent = location.parent;
  if (element->parentNode || element->Contains(parent)) return;
  if (parent->IsDocument() && static_cast<dom::Document*>(parent)->DocumentElement()) return;
  dom::InsertUnchecked(element, parent, location.reference);
}

Element* TreeBuilder::InsertForeignElement(const Token& token, std::string_view ns, bool onlyAddToStack) {
  const Location location = AdjustedInsertionLocation();
  Element* element = CreateElementForToken(token, ns, location.parent);
  if (!onlyAddToStack) InsertElementAt(element, location);
  open_.push_back(element);
  return element;
}

Element* TreeBuilder::InsertHtmlElementNamed(std::string_view name) { return InsertHtmlElement(FakeStartTag(name)); }

void TreeBuilder::AddMissingAttributes(Element* element, const Token& token) {
  for (const TokenAttribute& attribute : token.attributes) {
    if (element->FindAttribute(attribute.namespaceUri, attribute.localName.empty() ? attribute.name : attribute.localName) && !attribute.localName.empty()) continue;
    if (attribute.localName.empty() && element->FindAttribute("", attribute.name)) continue;
    dom::Attr* node = attribute.localName.empty() ? dom::NewAttr(ctx_, element->nodeDocument, "", "", attribute.name, attribute.value)
                                                  : dom::NewAttr(ctx_, element->nodeDocument, attribute.namespaceUri, attribute.prefix, attribute.localName, attribute.value);
    node->ownerElement = element;
    element->attributes.push_back(node);
    element->NoteWrite();
  }
  dom::NoteTreeChange();
}

void TreeBuilder::InsertCharacter(const std::string& utf8) {
  const Location location = AdjustedInsertionLocation();
  if (location.parent->IsDocument()) return;
  Node* previous = location.reference ? location.reference->previousSibling : location.parent->lastChild;
  if (previous && previous->nodeType == dom::NodeType::Text) {
    static_cast<dom::CharacterData*>(previous)->data += utf8;
    return;
  }
  dom::Document* document = location.parent->nodeDocument ? location.parent->nodeDocument : document_;
  dom::InsertUnchecked(dom::NewText(ctx_, document, utf8), location.parent, location.reference);
}

void TreeBuilder::InsertComment(const Token& token, const Location* at) {
  const Location location = AdjustedInsertionLocation(at ? at->parent : nullptr);
  dom::Document* document = location.parent->IsDocument() ? static_cast<dom::Document*>(location.parent) : location.parent->nodeDocument;
  dom::InsertUnchecked(dom::NewComment(ctx_, document, token.data), location.parent, location.reference);
}

void TreeBuilder::InsertProcessingInstruction(const Token& token, const Location* at) {
  const Location location = AdjustedInsertionLocation(at ? at->parent : nullptr);
  dom::Document* document = location.parent->IsDocument() ? static_cast<dom::Document*>(location.parent) : location.parent->nodeDocument;
  dom::InsertUnchecked(dom::NewProcessingInstruction(ctx_, document, token.name, token.data), location.parent, location.reference);
}

void TreeBuilder::GenericTextElement(Token& token, Tokenizer::State state) {
  InsertHtmlElement(token);
  tokenizer_.SetState(state);
  originalMode_ = mode_;
  mode_ = Mode::Text;
}

// ---- The list of active formatting elements ----

void TreeBuilder::InsertMarker() {
  Formatting marker;
  marker.marker = true;
  formatting_.push_back(std::move(marker));
}

int TreeBuilder::FindFormatting(const Element* element) const {
  for (size_t i = formatting_.size(); i-- > 0;) {
    if (!formatting_[i].marker && formatting_[i].element == element) return static_cast<int>(i);
  }
  return -1;
}

void TreeBuilder::PushFormatting(Element* element, const Token& token) {
  // The Noah's Ark clause: of elements alike after the last marker, there are not more than three.
  int count = 0;
  int earliest = -1;
  for (size_t i = formatting_.size(); i-- > 0;) {
    const Formatting& entry = formatting_[i];
    if (entry.marker) break;
    if (entry.element->localName != element->localName || entry.element->namespaceUri != element->namespaceUri) continue;
    if (entry.token.attributes.size() != token.attributes.size()) continue;
    bool same = true;
    for (const TokenAttribute& a : token.attributes) {
      const std::string* other = entry.token.Attribute(a.name);
      if (!other || *other != a.value) same = false;
    }
    if (!same) continue;
    ++count;
    earliest = static_cast<int>(i);
  }
  if (count >= 3) formatting_.erase(formatting_.begin() + earliest);
  Formatting entry;
  entry.element = element;
  entry.token = token;
  formatting_.push_back(std::move(entry));
}

void TreeBuilder::ReconstructFormatting() {
  if (formatting_.empty()) return;
  if (formatting_.back().marker || InStack(formatting_.back().element)) return;
  size_t index = formatting_.size() - 1;
  // Rewind to the entry after the last one that is a marker or is open.
  while (index > 0) {
    --index;
    if (formatting_[index].marker || InStack(formatting_[index].element)) {
      ++index;
      break;
    }
  }
  for (; index < formatting_.size(); ++index) {
    Element* created = InsertHtmlElement(formatting_[index].token);
    formatting_[index].element = created;
  }
}

void TreeBuilder::ClearFormattingToMarker() {
  while (!formatting_.empty()) {
    const bool marker = formatting_.back().marker;
    formatting_.pop_back();
    if (marker) return;
  }
}

void TreeBuilder::AdoptionAgency(Token& token) {
  const std::string subject = token.name;
  if (IsHtmlElement(CurrentNode(), subject) && FindFormatting(CurrentNode()) < 0) {
    Pop();
    return;
  }
  for (int outer = 0; outer < 8; ++outer) {
    // The last element of the list, after its last marker, that has the tag name.
    int formattingIndex = -1;
    for (size_t i = formatting_.size(); i-- > 0;) {
      if (formatting_[i].marker) break;
      if (formatting_[i].element->localName == subject) {
        formattingIndex = static_cast<int>(i);
        break;
      }
    }
    if (formattingIndex < 0) {
      InBodyAnyOtherEndTag(token);
      return;
    }
    Element* formattingElement = formatting_[formattingIndex].element;
    if (!InStack(formattingElement)) {
      Error("adoption-agency-1.2");
      formatting_.erase(formatting_.begin() + formattingIndex);
      return;
    }
    if (!HasInScopeElement(formattingElement)) {
      Error("adoption-agency-4.4");
      return;
    }
    if (formattingElement != CurrentNode()) Error("adoption-agency-1.3");

    const auto formattingStackPosition = std::find(open_.begin(), open_.end(), formattingElement) - open_.begin();
    Element* furthestBlock = nullptr;
    for (size_t i = static_cast<size_t>(formattingStackPosition) + 1; i < open_.size(); ++i) {
      if (IsSpecial(open_[i])) {
        furthestBlock = open_[i];
        break;
      }
    }
    if (!furthestBlock) {
      while (CurrentNode() != formattingElement) Pop();
      Pop();
      formatting_.erase(formatting_.begin() + FindFormatting(formattingElement));
      return;
    }
    Element* commonAncestor = open_[static_cast<size_t>(formattingStackPosition) - 1];
    int bookmark = formattingIndex;

    Element* node = furthestBlock;
    Element* lastNode = furthestBlock;
    int index = static_cast<int>(std::find(open_.begin(), open_.end(), furthestBlock) - open_.begin());
    int inner = 0;
    for (;;) {
      ++inner;
      --index;
      node = open_[static_cast<size_t>(index)];
      if (node == formattingElement) break;
      int entry = FindFormatting(node);
      if (inner > 3 && entry >= 0) {
        formatting_.erase(formatting_.begin() + entry);
        if (entry < bookmark) --bookmark;
        entry = -1;
      }
      if (entry < 0) {
        open_.erase(open_.begin() + index);
        continue;
      }
      Element* replacement = CreateElementForToken(formatting_[static_cast<size_t>(entry)].token, kHtml, commonAncestor);
      formatting_[static_cast<size_t>(entry)].element = replacement;
      open_[static_cast<size_t>(index)] = replacement;
      node = replacement;
      if (lastNode == furthestBlock) bookmark = entry + 1;
      dom::InsertUnchecked(lastNode, node, nullptr);
      lastNode = node;
    }

    const Location location = AdjustedInsertionLocation(commonAncestor);
    if (lastNode->parentNode) dom::RemoveUnchecked(lastNode);
    if (!lastNode->parentNode && !lastNode->Contains(location.parent) && !(location.parent->IsDocument() && static_cast<dom::Document*>(location.parent)->DocumentElement()) &&
        (!location.reference || location.reference->parentNode == location.parent)) {
      dom::InsertUnchecked(lastNode, location.parent, location.reference);
    }

    Element* created = CreateElementForToken(formatting_[static_cast<size_t>(FindFormatting(formattingElement))].token, kHtml, furthestBlock);
    while (furthestBlock->firstChild) dom::InsertUnchecked(furthestBlock->firstChild, created, nullptr);
    dom::InsertUnchecked(created, furthestBlock, nullptr);

    Token formattingToken = formatting_[static_cast<size_t>(FindFormatting(formattingElement))].token;
    const int at = FindFormatting(formattingElement);
    formatting_.erase(formatting_.begin() + at);
    if (at < bookmark) --bookmark;
    Formatting entry;
    entry.element = created;
    entry.token = std::move(formattingToken);
    formatting_.insert(formatting_.begin() + bookmark, std::move(entry));

    RemoveFromStack(formattingElement);
    const auto below = std::find(open_.begin(), open_.end(), furthestBlock);
    open_.insert(below + 1, created);
  }
}

// ---- Odds and ends ----

void TreeBuilder::SetQuirks(dom::Document::Mode mode) { document_->mode = mode; }

bool TreeBuilder::IsMathTextIntegrationPoint(const Element* element) {
  return element->namespaceUri == kMathMl && IsOneOf(element->localName, {"mi", "mo", "mn", "ms", "mtext"});
}

bool TreeBuilder::IsHtmlIntegrationPoint(const Element* element) const {
  if (element->namespaceUri == kMathMl && element->localName == "annotation-xml") {
    const dom::Attr* encoding = element->FindAttribute("", "encoding");
    if (!encoding) return false;
    const std::string value = LowerAscii(encoding->value);
    return value == "text/html" || value == "application/xhtml+xml";
  }
  return element->namespaceUri == kSvg && IsOneOf(element->localName, {"foreignObject", "desc", "title"});
}

bool TreeBuilder::IsSpecial(const Element* element) {
  if (element->namespaceUri == kHtml) {
    return IsOneOf(element->localName, {"address", "applet", "area", "article", "aside", "base", "basefont", "bgsound", "blockquote", "body", "br", "button", "caption", "center",
                                         "col", "colgroup", "dd", "details", "dir", "div", "dl", "dt", "embed", "fieldset", "figcaption", "figure", "footer", "form", "frame",
                                         "frameset", "h1", "h2", "h3", "h4", "h5", "h6", "head", "header", "hgroup", "hr", "html", "iframe", "img", "input", "keygen", "li",
                                         "link", "listing", "main", "marquee", "menu", "meta", "nav", "noembed", "noframes", "noscript", "object", "ol", "p", "param",
                                         "plaintext", "pre", "script", "search", "section", "select", "source", "style", "summary", "table", "tbody", "td", "template",
                                         "textarea", "tfoot", "th", "thead", "title", "tr", "track", "ul", "wbr", "xmp"});
  }
  if (element->namespaceUri == kMathMl) return IsOneOf(element->localName, {"mi", "mo", "mn", "ms", "mtext", "annotation-xml"});
  if (element->namespaceUri == kSvg) return IsOneOf(element->localName, {"foreignObject", "desc", "title"});
  return false;
}

bool TreeBuilder::IsFormatting(std::string_view name) {
  return IsOneOf(name, {"a", "b", "big", "code", "em", "font", "i", "nobr", "s", "small", "strike", "strong", "tt", "u"});
}

void TreeBuilder::AdjustMathAttributes(Token& token) {
  for (TokenAttribute& attribute : token.attributes) {
    if (attribute.name == "definitionurl") attribute.name = "definitionURL";
  }
}

void TreeBuilder::AdjustSvgAttributes(Token& token) {
  for (TokenAttribute& attribute : token.attributes) {
    for (size_t i = 0; i < kSvgAttributeAdjustmentCount; ++i) {
      if (attribute.name == kSvgAttributeAdjustments[i].from) attribute.name = kSvgAttributeAdjustments[i].to;
    }
  }
}

void TreeBuilder::AdjustForeignAttributes(Token& token) {
  struct Foreign {
    const char* name;
    const char* prefix;
    const char* local;
    std::string_view ns;
  };
  static const Foreign kForeign[] = {
      {"xlink:actuate", "xlink", "actuate", "http://www.w3.org/1999/xlink"}, {"xlink:arcrole", "xlink", "arcrole", "http://www.w3.org/1999/xlink"},
      {"xlink:href", "xlink", "href", "http://www.w3.org/1999/xlink"},       {"xlink:role", "xlink", "role", "http://www.w3.org/1999/xlink"},
      {"xlink:show", "xlink", "show", "http://www.w3.org/1999/xlink"},       {"xlink:title", "xlink", "title", "http://www.w3.org/1999/xlink"},
      {"xlink:type", "xlink", "type", "http://www.w3.org/1999/xlink"},       {"xml:lang", "xml", "lang", dom::kXmlNamespace},
      {"xml:space", "xml", "space", dom::kXmlNamespace},                     {"xmlns", "", "xmlns", dom::kXmlnsNamespace},
      {"xmlns:xlink", "xmlns", "xlink", dom::kXmlnsNamespace},
  };
  for (TokenAttribute& attribute : token.attributes) {
    for (const Foreign& foreign : kForeign) {
      if (attribute.name == foreign.name) {
        attribute.namespaceUri = foreign.ns;
        attribute.prefix = foreign.prefix;
        attribute.localName = foreign.local;
      }
    }
  }
}

// ---- Insertion modes ----

void TreeBuilder::Initial(Token& token) {
  if (IsWhitespaceCharacter(token)) return;
  if (token.type == Token::Type::Comment) { Location at{document_, nullptr}; InsertComment(token, &at); return; }
  if (token.type == Token::Type::ProcessingInstruction) { Location at{document_, nullptr}; InsertProcessingInstruction(token, &at); return; }
  if (token.type == Token::Type::Doctype) {
    if (token.doctypeName != "html" || token.publicId || (token.systemId && *token.systemId != "about:legacy-compat")) Error("unexpected-doctype");
    dom::DocumentType* doctype = dom::NewDocumentType(ctx_, document_, token.doctypeName.value_or(""), token.publicId.value_or(""), token.systemId.value_or(""));
    if (!document_->Doctype() && !document_->DocumentElement()) dom::InsertUnchecked(doctype, document_, nullptr);
    SetQuirks(ModeForDoctype(token));
    mode_ = Mode::BeforeHtml;
    return;
  }
  Error("expected-doctype-but-got-something");
  SetQuirks(dom::Document::Mode::Quirks);
  mode_ = Mode::BeforeHtml;
  Process(token);
}

void TreeBuilder::BeforeHtml(Token& token) {
  if (token.type == Token::Type::Doctype) return;
  if (token.type == Token::Type::Comment) { Location at{document_, nullptr}; InsertComment(token, &at); return; }
  if (token.type == Token::Type::ProcessingInstruction) { Location at{document_, nullptr}; InsertProcessingInstruction(token, &at); return; }
  if (IsWhitespaceCharacter(token)) return;
  if (IsStart(token, "html")) {
    Element* html = CreateElementForToken(token, kHtml, document_);
    InsertElementAt(html, {document_, nullptr});
    open_.push_back(html);
    mode_ = Mode::BeforeHead;
    return;
  }
  if (token.type == Token::Type::EndTag && !IsEndOneOf(token, {"head", "body", "html", "br"})) return;
  Element* html = dom::NewElement(ctx_, document_, "html", kHtml);
  dom::InsertUnchecked(html, document_, nullptr);
  open_.push_back(html);
  mode_ = Mode::BeforeHead;
  Process(token);
}

void TreeBuilder::BeforeHead(Token& token) {
  if (IsWhitespaceCharacter(token)) return;
  if (token.type == Token::Type::Comment) { InsertComment(token); return; }
  if (token.type == Token::Type::ProcessingInstruction) { InsertProcessingInstruction(token); return; }
  if (token.type == Token::Type::Doctype) return;
  if (IsStart(token, "html")) { InBody(token); return; }
  if (IsStart(token, "head")) {
    head_ = InsertHtmlElement(token);
    mode_ = Mode::InHead;
    return;
  }
  if (token.type == Token::Type::EndTag && !IsEndOneOf(token, {"head", "body", "html", "br"})) return;
  head_ = InsertHtmlElementNamed("head");
  mode_ = Mode::InHead;
  Process(token);
}

void TreeBuilder::InHead(Token& token) {
  if (IsWhitespaceCharacter(token)) { InsertCharacter(token.data); return; }
  if (token.type == Token::Type::Comment) { InsertComment(token); return; }
  if (token.type == Token::Type::ProcessingInstruction) { InsertProcessingInstruction(token); return; }
  if (token.type == Token::Type::Doctype) return;
  if (IsStart(token, "html")) { InBody(token); return; }
  if (IsStartOneOf(token, {"base", "basefont", "bgsound", "link", "meta"})) {
    InsertHtmlElement(token);
    Pop();
    return;
  }
  if (IsStart(token, "title")) { GenericTextElement(token, Tokenizer::State::Rcdata); return; }
  if ((IsStart(token, "noscript") && scripting_ != ScriptingMode::Disabled) || IsStartOneOf(token, {"noframes", "style"})) {
    GenericTextElement(token, Tokenizer::State::Rawtext);
    return;
  }
  if (IsStart(token, "noscript")) {
    InsertHtmlElement(token);
    mode_ = Mode::InHeadNoscript;
    return;
  }
  if (IsStart(token, "script")) {
    const Location location = AdjustedInsertionLocation();
    Element* script = CreateElementForToken(token, kHtml, location.parent);
    InsertElementAt(script, location);
    open_.push_back(script);
    tokenizer_.SetState(Tokenizer::State::ScriptData);
    originalMode_ = mode_;
    mode_ = Mode::Text;
    return;
  }
  if (IsEnd(token, "head")) {
    Pop();
    mode_ = Mode::AfterHead;
    return;
  }
  if (IsStart(token, "template")) {
    InsertMarker();
    framesetOk_ = false;
    mode_ = Mode::InTemplate;
    templateModes_.push_back(Mode::InTemplate);
    InsertHtmlElement(token);
    return;
  }
  if (IsEnd(token, "template")) {
    if (!HasTemplateInStack()) return;
    GenerateAllImpliedEndTagsThoroughly();
    if (!IsHtmlElement(CurrentNode(), "template")) Error("end-tag-too-early");
    PopUntilHtml("template");
    ClearFormattingToMarker();
    templateModes_.pop_back();
    ResetInsertionMode();
    return;
  }
  if (IsStart(token, "head") || (token.type == Token::Type::EndTag && !IsEndOneOf(token, {"body", "html", "br"}))) return;
  Pop();
  mode_ = Mode::AfterHead;
  Process(token);
}

void TreeBuilder::InHeadNoscript(Token& token) {
  if (token.type == Token::Type::Doctype) return;
  if (IsStart(token, "html")) { InBody(token); return; }
  if (IsEnd(token, "noscript")) {
    Pop();
    mode_ = Mode::InHead;
    return;
  }
  if (IsWhitespaceCharacter(token) || token.type == Token::Type::Comment || token.type == Token::Type::ProcessingInstruction ||
      IsStartOneOf(token, {"basefont", "bgsound", "link", "meta", "noframes", "style"})) {
    InHead(token);
    return;
  }
  if (IsStartOneOf(token, {"head", "noscript"}) || (token.type == Token::Type::EndTag && token.name != "br")) return;
  Error("unexpected-token-in-noscript");
  Pop();
  mode_ = Mode::InHead;
  Process(token);
}

void TreeBuilder::AfterHead(Token& token) {
  if (IsWhitespaceCharacter(token)) { InsertCharacter(token.data); return; }
  if (token.type == Token::Type::Comment) { InsertComment(token); return; }
  if (token.type == Token::Type::ProcessingInstruction) { InsertProcessingInstruction(token); return; }
  if (token.type == Token::Type::Doctype) return;
  if (IsStart(token, "html")) { InBody(token); return; }
  if (IsStart(token, "body")) {
    InsertHtmlElement(token);
    framesetOk_ = false;
    mode_ = Mode::InBody;
    return;
  }
  if (IsStart(token, "frameset")) {
    InsertHtmlElement(token);
    mode_ = Mode::InFrameset;
    return;
  }
  if (IsStartOneOf(token, {"base", "basefont", "bgsound", "link", "meta", "noframes", "script", "style", "template", "title"})) {
    Error("unexpected-start-tag-out-of-my-head");
    open_.push_back(head_);
    InHead(token);
    RemoveFromStack(head_);
    return;
  }
  if (IsEnd(token, "template")) { InHead(token); return; }
  if (IsStart(token, "head") || (token.type == Token::Type::EndTag && !IsEndOneOf(token, {"body", "html", "br"}))) return;
  InsertHtmlElementNamed("body");
  framesetOk_ = true;
  mode_ = Mode::InBody;
  Process(token);
}

void TreeBuilder::InBody(Token& token) {
  switch (token.type) {
    case Token::Type::Character:
      if (IsNullCharacter(token)) return;
      ReconstructFormatting();
      InsertCharacter(token.data);
      if (!IsWhitespaceCharacter(token)) framesetOk_ = false;
      return;
    case Token::Type::Comment:
      InsertComment(token);
      return;
    case Token::Type::ProcessingInstruction:
      InsertProcessingInstruction(token);
      return;
    case Token::Type::Doctype:
      return;
    case Token::Type::EndOfFile:
      if (!templateModes_.empty()) {
        InTemplate(token);
        return;
      }
      StopParsing();
      return;
    case Token::Type::StartTag:
    case Token::Type::EndTag:
      break;
  }

  const std::string& name = token.name;
  if (token.type == Token::Type::StartTag) {
    if (name == "html") {
      if (HasTemplateInStack()) return;
      AddMissingAttributes(open_[0], token);
      return;
    }
    if (IsOneOf(name, {"base", "basefont", "bgsound", "link", "meta", "noframes", "script", "style", "template", "title"})) {
      InHead(token);
      return;
    }
    if (name == "body") {
      if (open_.size() == 1 || !IsHtmlElement(open_[1], "body") || HasTemplateInStack()) return;
      framesetOk_ = false;
      AddMissingAttributes(open_[1], token);
      return;
    }
    if (name == "frameset") {
      if (open_.size() == 1 || !IsHtmlElement(open_[1], "body")) return;
      if (!framesetOk_) return;
      if (open_[1]->parentNode) dom::RemoveUnchecked(open_[1]);
      while (open_.size() > 1) Pop();
      InsertHtmlElement(token);
      mode_ = Mode::InFrameset;
      return;
    }
    if (IsOneOf(name, {"address", "article", "aside", "blockquote", "center", "details", "dialog", "dir", "div", "dl", "fieldset", "figcaption", "figure", "footer", "header",
                       "hgroup", "main", "menu", "nav", "ol", "p", "search", "section", "summary", "ul"})) {
      if (HasInScope("p", Scope::Button)) ClosePElement();
      InsertHtmlElement(token);
      return;
    }
    if (IsOneOf(name, {"h1", "h2", "h3", "h4", "h5", "h6"})) {
      if (HasInScope("p", Scope::Button)) ClosePElement();
      if (IsHtmlElementOneOf(CurrentNode(), {"h1", "h2", "h3", "h4", "h5", "h6"})) {
        Error("unexpected-start-tag");
        Pop();
      }
      InsertHtmlElement(token);
      return;
    }
    if (name == "pre" || name == "listing") {
      if (HasInScope("p", Scope::Button)) ClosePElement();
      InsertHtmlElement(token);
      skipNextLineFeed_ = true;
      framesetOk_ = false;
      return;
    }
    if (name == "form") {
      if (form_ && !ParsingTemplateContents()) return;
      if (HasInScope("p", Scope::Button)) ClosePElement();
      Element* form = InsertHtmlElement(token);
      if (!ParsingTemplateContents()) form_ = form;
      return;
    }
    if (name == "li" || name == "dd" || name == "dt") {
      framesetOk_ = false;
      for (size_t i = open_.size(); i-- > 0;) {
        Element* node = open_[i];
        if (name == "li" && IsHtmlElement(node, "li")) {
          GenerateImpliedEndTags("li");
          if (!IsHtmlElement(CurrentNode(), "li")) Error("end-tag-too-early");
          PopUntilHtml("li");
          break;
        }
        if (name != "li" && IsHtmlElementOneOf(node, {"dd", "dt"}) && (node->localName == "dd" || node->localName == "dt")) {
          const std::string found = node->localName;
          GenerateImpliedEndTags(found);
          if (!IsHtmlElement(CurrentNode(), found)) Error("end-tag-too-early");
          PopUntilHtml(found);
          break;
        }
        if (IsSpecial(node) && !IsHtmlElementOneOf(node, {"address", "div", "p"})) break;
      }
      if (HasInScope("p", Scope::Button)) ClosePElement();
      InsertHtmlElement(token);
      return;
    }
    if (name == "plaintext") {
      if (HasInScope("p", Scope::Button)) ClosePElement();
      InsertHtmlElement(token);
      tokenizer_.SetState(Tokenizer::State::Plaintext);
      return;
    }
    if (name == "button") {
      if (HasInScope("button")) {
        Error("unexpected-start-tag-implies-end-tag");
        GenerateImpliedEndTags();
        PopUntilHtml("button");
      }
      ReconstructFormatting();
      InsertHtmlElement(token);
      framesetOk_ = false;
      return;
    }
    if (name == "a") {
      for (size_t i = formatting_.size(); i-- > 0;) {
        if (formatting_[i].marker) break;
        if (IsHtmlElement(formatting_[i].element, "a")) {
          Error("unexpected-start-tag-implies-end-tag");
          Element* existing = formatting_[i].element;
          Token end = token;
          end.type = Token::Type::EndTag;
          AdoptionAgency(end);
          const int at = FindFormatting(existing);
          if (at >= 0) formatting_.erase(formatting_.begin() + at);
          RemoveFromStack(existing);
          break;
        }
      }
      ReconstructFormatting();
      Element* element = InsertHtmlElement(token);
      PushFormatting(element, token);
      return;
    }
    if (IsFormatting(name) && name != "nobr") {
      ReconstructFormatting();
      Element* element = InsertHtmlElement(token);
      PushFormatting(element, token);
      return;
    }
    if (name == "nobr") {
      ReconstructFormatting();
      if (HasInScope("nobr")) {
        Error("unexpected-start-tag-implies-end-tag");
        Token end = token;
        end.type = Token::Type::EndTag;
        AdoptionAgency(end);
        ReconstructFormatting();
      }
      Element* element = InsertHtmlElement(token);
      PushFormatting(element, token);
      return;
    }
    if (name == "applet" || name == "marquee" || name == "object") {
      ReconstructFormatting();
      InsertHtmlElement(token);
      InsertMarker();
      framesetOk_ = false;
      return;
    }
    if (name == "table") {
      if (document_->mode != dom::Document::Mode::Quirks && HasInScope("p", Scope::Button)) ClosePElement();
      InsertHtmlElement(token);
      framesetOk_ = false;
      mode_ = Mode::InTable;
      return;
    }
    if (IsOneOf(name, {"area", "br", "embed", "img", "keygen", "wbr"})) {
      ReconstructFormatting();
      InsertHtmlElement(token);
      Pop();
      framesetOk_ = false;
      return;
    }
    if (name == "input") {
      if (IsHtmlElement(fragmentContext_, "select")) return;
      if (HasInScope("select")) {
        Error("unexpected-input-in-select");
        PopUntilHtml("select");
      }
      ReconstructFormatting();
      InsertHtmlElement(token);
      Pop();
      const std::string* type = token.Attribute("type");
      if (!type || LowerAscii(*type) != "hidden") framesetOk_ = false;
      return;
    }
    if (name == "param" || name == "source" || name == "track") {
      InsertHtmlElement(token);
      Pop();
      return;
    }
    if (name == "hr") {
      if (HasInScope("p", Scope::Button)) ClosePElement();
      if (HasInScope("select")) {
        GenerateImpliedEndTags();
        if (HasInScope("option") || HasInScope("optgroup")) Error("unexpected-hr-in-select");
      }
      InsertHtmlElement(token);
      Pop();
      framesetOk_ = false;
      return;
    }
    if (name == "image") {
      Error("unexpected-start-tag-treated-as");
      token.name = "img";
      InBody(token);
      return;
    }
    if (name == "textarea") {
      InsertHtmlElement(token);
      skipNextLineFeed_ = true;
      tokenizer_.SetState(Tokenizer::State::Rcdata);
      originalMode_ = mode_;
      framesetOk_ = false;
      mode_ = Mode::Text;
      return;
    }
    if (name == "xmp") {
      if (HasInScope("p", Scope::Button)) ClosePElement();
      ReconstructFormatting();
      framesetOk_ = false;
      GenericTextElement(token, Tokenizer::State::Rawtext);
      return;
    }
    if (name == "iframe") {
      framesetOk_ = false;
      GenericTextElement(token, Tokenizer::State::Rawtext);
      return;
    }
    if (name == "noembed" || (name == "noscript" && scripting_ != ScriptingMode::Disabled)) {
      GenericTextElement(token, Tokenizer::State::Rawtext);
      return;
    }
    if (name == "select") {
      if (IsHtmlElement(fragmentContext_, "select")) return;
      if (HasInScope("select")) {
        PopUntilHtml("select");
        return;
      }
      ReconstructFormatting();
      InsertHtmlElement(token);
      framesetOk_ = false;
      return;
    }
    if (name == "option") {
      if (HasInScope("select")) {
        GenerateImpliedEndTags("optgroup");
        if (HasInScope("option")) Error("unexpected-option-in-select");
      } else if (IsHtmlElement(CurrentNode(), "option")) {
        Pop();
      }
      ReconstructFormatting();
      InsertHtmlElement(token);
      return;
    }
    if (name == "optgroup") {
      if (HasInScope("select")) {
        GenerateImpliedEndTags();
        if (HasInScope("option") || HasInScope("optgroup")) Error("unexpected-optgroup-in-select");
      } else if (IsHtmlElement(CurrentNode(), "option")) {
        Pop();
      }
      ReconstructFormatting();
      InsertHtmlElement(token);
      return;
    }
    if (name == "rb" || name == "rtc") {
      if (HasInScope("ruby")) {
        GenerateImpliedEndTags();
        if (!IsHtmlElement(CurrentNode(), "ruby")) Error("unexpected-ruby-start-tag");
      }
      InsertHtmlElement(token);
      return;
    }
    if (name == "rp" || name == "rt") {
      if (HasInScope("ruby")) {
        GenerateImpliedEndTags("rtc");
        if (!IsHtmlElement(CurrentNode(), "rtc") && !IsHtmlElement(CurrentNode(), "ruby")) Error("unexpected-ruby-start-tag");
      }
      InsertHtmlElement(token);
      return;
    }
    if (name == "math") {
      ReconstructFormatting();
      AdjustMathAttributes(token);
      AdjustForeignAttributes(token);
      InsertForeignElement(token, kMathMl, false);
      if (token.selfClosing) Pop();
      return;
    }
    if (name == "svg") {
      ReconstructFormatting();
      AdjustSvgAttributes(token);
      AdjustForeignAttributes(token);
      InsertForeignElement(token, kSvg, false);
      if (token.selfClosing) Pop();
      return;
    }
    if (IsOneOf(name, {"caption", "col", "colgroup", "frame", "head", "tbody", "td", "tfoot", "th", "thead", "tr"})) return;
    ReconstructFormatting();
    InsertHtmlElement(token);
    return;
  }

  // End tags.
  if (name == "template") {
    InHead(token);
    return;
  }
  if (name == "body") {
    if (!HasInScope("body")) return;
    mode_ = Mode::AfterBody;
    return;
  }
  if (name == "html") {
    if (!HasInScope("body")) return;
    mode_ = Mode::AfterBody;
    Process(token);
    return;
  }
  if (IsOneOf(name, {"address", "article", "aside", "blockquote", "button", "center", "details", "dialog", "dir", "div", "dl", "fieldset", "figcaption", "figure", "footer",
                     "header", "hgroup", "listing", "main", "menu", "nav", "ol", "pre", "search", "section", "select", "summary", "ul"})) {
    if (!HasInScope(name)) return;
    GenerateImpliedEndTags();
    if (!IsHtmlElement(CurrentNode(), name)) Error("end-tag-too-early");
    PopUntilHtml(name);
    return;
  }
  if (name == "form") {
    if (!ParsingTemplateContents()) {
      Element* node = form_;
      form_ = nullptr;
      if (!node || !HasInScopeElement(node)) return;
      GenerateImpliedEndTags();
      if (CurrentNode() != node) Error("end-tag-too-early");
      RemoveFromStack(node);
    } else {
      if (!HasInScope("form")) return;
      GenerateImpliedEndTags();
      if (!IsHtmlElement(CurrentNode(), "form")) Error("end-tag-too-early");
      PopUntilHtml("form");
    }
    return;
  }
  if (name == "p") {
    if (!HasInScope("p", Scope::Button)) {
      Error("unexpected-end-tag");
      InsertHtmlElementNamed("p");
    }
    ClosePElement();
    return;
  }
  if (name == "li") {
    if (!HasInScope("li", Scope::ListItem)) return;
    GenerateImpliedEndTags("li");
    if (!IsHtmlElement(CurrentNode(), "li")) Error("end-tag-too-early");
    PopUntilHtml("li");
    return;
  }
  if (name == "dd" || name == "dt") {
    if (!HasInScope(name)) return;
    GenerateImpliedEndTags(name);
    if (!IsHtmlElement(CurrentNode(), name)) Error("end-tag-too-early");
    PopUntilHtml(name);
    return;
  }
  if (IsOneOf(name, {"h1", "h2", "h3", "h4", "h5", "h6"})) {
    if (!HasInScopeOneOf({"h1", "h2", "h3", "h4", "h5", "h6"})) return;
    GenerateImpliedEndTags();
    if (!IsHtmlElement(CurrentNode(), name)) Error("end-tag-too-early");
    PopUntilOneOf({"h1", "h2", "h3", "h4", "h5", "h6"});
    return;
  }
  if (name == "sarcasm") {
    InBodyAnyOtherEndTag(token);
    return;
  }
  if (IsFormatting(name)) {
    AdoptionAgency(token);
    return;
  }
  if (name == "applet" || name == "marquee" || name == "object") {
    if (!HasInScope(name)) return;
    GenerateImpliedEndTags();
    if (!IsHtmlElement(CurrentNode(), name)) Error("end-tag-too-early");
    PopUntilHtml(name);
    ClearFormattingToMarker();
    return;
  }
  if (name == "br") {
    Error("unexpected-end-tag-treated-as-start-tag");
    token.type = Token::Type::StartTag;
    token.attributes.clear();
    token.selfClosing = false;
    InBody(token);
    return;
  }
  InBodyAnyOtherEndTag(token);
}

void TreeBuilder::InBodyAnyOtherEndTag(Token& token) {
  for (size_t i = open_.size(); i-- > 0;) {
    Element* node = open_[i];
    if (IsHtmlElement(node, token.name)) {
      GenerateImpliedEndTags(token.name);
      if (node != CurrentNode()) Error("end-tag-too-early");
      while (CurrentNode() != node) Pop();
      Pop();
      return;
    }
    if (IsSpecial(node)) {
      Error("unexpected-end-tag");
      return;
    }
  }
}

void TreeBuilder::TextMode(Token& token) {
  if (token.type == Token::Type::Character) {
    InsertCharacter(token.data);
    return;
  }
  if (token.type == Token::Type::EndOfFile) {
    Error("expected-named-closing-tag-but-got-eof");
    Pop();
    mode_ = originalMode_;
    Process(token);
    return;
  }
  if (token.type == Token::Type::EndTag) {
    Element* current = CurrentNode();
    Pop();
    mode_ = originalMode_;
    // A script's end tag is where the script runs, and the parser waits for it.
    if (token.name == "script" && scriptHandler_ && IsHtmlElement(current, "script")) scriptHandler_(current);
  }
}

void TreeBuilder::InTable(Token& token) {
  if (token.type == Token::Type::Character && IsHtmlElementOneOf(CurrentNode(), {"table", "tbody", "template", "tfoot", "thead", "tr"})) {
    pendingTableCharacters_.clear();
    originalMode_ = mode_;
    mode_ = Mode::InTableText;
    Process(token);
    return;
  }
  if (token.type == Token::Type::Comment) { InsertComment(token); return; }
  if (token.type == Token::Type::ProcessingInstruction) { InsertProcessingInstruction(token); return; }
  if (token.type == Token::Type::Doctype) return;
  if (IsStart(token, "caption")) {
    ClearStackBackTo({"table"});
    InsertMarker();
    InsertHtmlElement(token);
    mode_ = Mode::InCaption;
    return;
  }
  if (IsStart(token, "colgroup")) {
    ClearStackBackTo({"table"});
    InsertHtmlElement(token);
    mode_ = Mode::InColumnGroup;
    return;
  }
  if (IsStart(token, "col")) {
    ClearStackBackTo({"table"});
    InsertHtmlElementNamed("colgroup");
    mode_ = Mode::InColumnGroup;
    Process(token);
    return;
  }
  if (IsStartOneOf(token, {"tbody", "tfoot", "thead"})) {
    ClearStackBackTo({"table"});
    InsertHtmlElement(token);
    mode_ = Mode::InTableBody;
    return;
  }
  if (IsStartOneOf(token, {"td", "th", "tr"})) {
    ClearStackBackTo({"table"});
    InsertHtmlElementNamed("tbody");
    mode_ = Mode::InTableBody;
    Process(token);
    return;
  }
  if (IsStart(token, "table")) {
    if (!HasInScope("table", Scope::Table)) return;
    PopUntilHtml("table");
    ResetInsertionMode();
    Process(token);
    return;
  }
  if (IsEnd(token, "table")) {
    if (!HasInScope("table", Scope::Table)) return;
    PopUntilHtml("table");
    ResetInsertionMode();
    return;
  }
  if (IsEndOneOf(token, {"body", "caption", "col", "colgroup", "html", "tbody", "td", "tfoot", "th", "thead", "tr"})) return;
  if (IsStartOneOf(token, {"style", "script", "template"}) || IsEnd(token, "template")) {
    InHead(token);
    return;
  }
  if (IsStart(token, "input")) {
    const std::string* type = token.Attribute("type");
    if (type && LowerAscii(*type) == "hidden") {
      InsertHtmlElement(token);
      Pop();
      return;
    }
  } else if (IsStart(token, "form")) {
    if (form_ && !ParsingTemplateContents()) return;
    Element* form = InsertHtmlElement(token);
    if (!ParsingTemplateContents()) form_ = form;
    Pop();
    return;
  } else if (token.type == Token::Type::EndOfFile) {
    InBody(token);
    return;
  }
  Error("unexpected-token-in-table");
  fosterParenting_ = true;
  InBody(token);
  fosterParenting_ = false;
}

void TreeBuilder::InTableText(Token& token) {
  if (IsNullCharacter(token)) return;
  if (token.type == Token::Type::Character) {
    pendingTableCharacters_.push_back(token.data);
    return;
  }
  bool nonWhitespace = false;
  for (const std::string& piece : pendingTableCharacters_) {
    if (!(piece.size() == 1 && (piece[0] == '\t' || piece[0] == '\n' || piece[0] == '\f' || piece[0] == '\r' || piece[0] == ' '))) nonWhitespace = true;
  }
  if (nonWhitespace) {
    Error("foster-parenting-character");
    for (const std::string& piece : pendingTableCharacters_) {
      Token character;
      character.type = Token::Type::Character;
      character.data = piece;
      fosterParenting_ = true;
      InBody(character);
      fosterParenting_ = false;
    }
  } else {
    for (const std::string& piece : pendingTableCharacters_) InsertCharacter(piece);
  }
  pendingTableCharacters_.clear();
  mode_ = originalMode_;
  Process(token);
}

void TreeBuilder::InCaption(Token& token) {
  if (IsEnd(token, "caption")) {
    if (!HasInScope("caption", Scope::Table)) return;
    GenerateImpliedEndTags();
    if (!IsHtmlElement(CurrentNode(), "caption")) Error("end-tag-too-early");
    PopUntilHtml("caption");
    ClearFormattingToMarker();
    mode_ = Mode::InTable;
    return;
  }
  if (IsStartOneOf(token, {"caption", "col", "colgroup", "tbody", "td", "tfoot", "th", "thead", "tr"}) || IsEnd(token, "table")) {
    if (!HasInScope("caption", Scope::Table)) return;
    GenerateImpliedEndTags();
    if (!IsHtmlElement(CurrentNode(), "caption")) Error("end-tag-too-early");
    PopUntilHtml("caption");
    ClearFormattingToMarker();
    mode_ = Mode::InTable;
    Process(token);
    return;
  }
  if (IsEndOneOf(token, {"body", "col", "colgroup", "html", "tbody", "td", "tfoot", "th", "thead", "tr"})) return;
  InBody(token);
}

void TreeBuilder::InColumnGroup(Token& token) {
  if (IsWhitespaceCharacter(token)) { InsertCharacter(token.data); return; }
  if (token.type == Token::Type::Comment) { InsertComment(token); return; }
  if (token.type == Token::Type::ProcessingInstruction) { InsertProcessingInstruction(token); return; }
  if (token.type == Token::Type::Doctype) return;
  if (IsStart(token, "html")) { InBody(token); return; }
  if (IsStart(token, "col")) {
    InsertHtmlElement(token);
    Pop();
    return;
  }
  if (IsEnd(token, "colgroup")) {
    if (!IsHtmlElement(CurrentNode(), "colgroup")) return;
    Pop();
    mode_ = Mode::InTable;
    return;
  }
  if (IsEnd(token, "col")) return;
  if (IsStart(token, "template") || IsEnd(token, "template")) { InHead(token); return; }
  if (token.type == Token::Type::EndOfFile) { InBody(token); return; }
  if (!IsHtmlElement(CurrentNode(), "colgroup")) return;
  Pop();
  mode_ = Mode::InTable;
  Process(token);
}

void TreeBuilder::InTableBody(Token& token) {
  if (IsStart(token, "tr")) {
    ClearStackBackTo({"tbody", "tfoot", "thead"});
    InsertHtmlElement(token);
    mode_ = Mode::InRow;
    return;
  }
  if (IsStartOneOf(token, {"th", "td"})) {
    Error("unexpected-cell-in-table-body");
    ClearStackBackTo({"tbody", "tfoot", "thead"});
    InsertHtmlElementNamed("tr");
    mode_ = Mode::InRow;
    Process(token);
    return;
  }
  if (IsEndOneOf(token, {"tbody", "tfoot", "thead"})) {
    if (!HasInScope(token.name, Scope::Table)) return;
    ClearStackBackTo({"tbody", "tfoot", "thead"});
    Pop();
    mode_ = Mode::InTable;
    return;
  }
  if (IsStartOneOf(token, {"caption", "col", "colgroup", "tbody", "tfoot", "thead"}) || IsEnd(token, "table")) {
    if (!HasInScopeOneOf({"tbody", "thead", "tfoot"}, Scope::Table)) return;
    ClearStackBackTo({"tbody", "tfoot", "thead"});
    Pop();
    mode_ = Mode::InTable;
    Process(token);
    return;
  }
  if (IsEndOneOf(token, {"body", "caption", "col", "colgroup", "html", "td", "th", "tr"})) return;
  InTable(token);
}

void TreeBuilder::InRow(Token& token) {
  if (IsStartOneOf(token, {"th", "td"})) {
    ClearStackBackTo({"tr"});
    InsertHtmlElement(token);
    mode_ = Mode::InCell;
    InsertMarker();
    return;
  }
  if (IsEnd(token, "tr")) {
    if (!HasInScope("tr", Scope::Table)) return;
    ClearStackBackTo({"tr"});
    Pop();
    mode_ = Mode::InTableBody;
    return;
  }
  if (IsStartOneOf(token, {"caption", "col", "colgroup", "tbody", "tfoot", "thead", "tr"}) || IsEnd(token, "table")) {
    if (!HasInScope("tr", Scope::Table)) return;
    ClearStackBackTo({"tr"});
    Pop();
    mode_ = Mode::InTableBody;
    Process(token);
    return;
  }
  if (IsEndOneOf(token, {"tbody", "tfoot", "thead"})) {
    if (!HasInScope(token.name, Scope::Table)) return;
    if (!HasInScope("tr", Scope::Table)) return;
    ClearStackBackTo({"tr"});
    Pop();
    mode_ = Mode::InTableBody;
    Process(token);
    return;
  }
  if (IsEndOneOf(token, {"body", "caption", "col", "colgroup", "html", "td", "th"})) return;
  InTable(token);
}

void TreeBuilder::InCell(Token& token) {
  const auto closeTheCell = [this] {
    GenerateImpliedEndTags();
    if (!IsHtmlElementOneOf(CurrentNode(), {"td", "th"})) Error("end-tag-too-early");
    PopUntilOneOf({"td", "th"});
    ClearFormattingToMarker();
    mode_ = Mode::InRow;
  };
  if (IsEndOneOf(token, {"td", "th"})) {
    if (!HasInScope(token.name, Scope::Table)) return;
    GenerateImpliedEndTags();
    if (!IsHtmlElement(CurrentNode(), token.name)) Error("end-tag-too-early");
    PopUntilHtml(token.name);
    ClearFormattingToMarker();
    mode_ = Mode::InRow;
    return;
  }
  if (IsStartOneOf(token, {"caption", "col", "colgroup", "tbody", "td", "tfoot", "th", "thead", "tr"})) {
    closeTheCell();
    Process(token);
    return;
  }
  if (IsEndOneOf(token, {"body", "caption", "col", "colgroup", "html"})) return;
  if (IsEndOneOf(token, {"table", "tbody", "tfoot", "thead", "tr"})) {
    if (!HasInScope(token.name, Scope::Table)) return;
    closeTheCell();
    Process(token);
    return;
  }
  InBody(token);
}

void TreeBuilder::InTemplate(Token& token) {
  if (token.type == Token::Type::Character || token.type == Token::Type::Comment || token.type == Token::Type::ProcessingInstruction || token.type == Token::Type::Doctype) {
    InBody(token);
    return;
  }
  if (IsStartOneOf(token, {"base", "basefont", "bgsound", "link", "meta", "noframes", "script", "style", "template", "title"}) || IsEnd(token, "template")) {
    InHead(token);
    return;
  }
  const auto switchTo = [&](Mode mode) {
    templateModes_.pop_back();
    templateModes_.push_back(mode);
    mode_ = mode;
    Process(token);
  };
  if (IsStartOneOf(token, {"caption", "colgroup", "tbody", "tfoot", "thead"})) { switchTo(Mode::InTable); return; }
  if (IsStart(token, "col")) { switchTo(Mode::InColumnGroup); return; }
  if (IsStart(token, "tr")) { switchTo(Mode::InTableBody); return; }
  if (IsStartOneOf(token, {"td", "th"})) { switchTo(Mode::InRow); return; }
  if (token.type == Token::Type::StartTag) { switchTo(Mode::InBody); return; }
  if (token.type == Token::Type::EndTag) return;
  // The end of the file.
  if (!HasTemplateInStack()) {
    StopParsing();
    return;
  }
  Error("eof-in-template");
  PopUntilHtml("template");
  ClearFormattingToMarker();
  templateModes_.pop_back();
  ResetInsertionMode();
  Process(token);
}

void TreeBuilder::AfterBody(Token& token) {
  if (IsWhitespaceCharacter(token)) { InBody(token); return; }
  if (token.type == Token::Type::Comment) { Location at{open_[0], nullptr}; InsertComment(token, &at); return; }
  if (token.type == Token::Type::ProcessingInstruction) { Location at{open_[0], nullptr}; InsertProcessingInstruction(token, &at); return; }
  if (token.type == Token::Type::Doctype) return;
  if (IsStart(token, "html")) { InBody(token); return; }
  if (IsEnd(token, "html")) {
    if (fragmentContext_) return;
    mode_ = Mode::AfterAfterBody;
    return;
  }
  if (token.type == Token::Type::EndOfFile) { StopParsing(); return; }
  Error("unexpected-token-after-body");
  mode_ = Mode::InBody;
  Process(token);
}

void TreeBuilder::InFrameset(Token& token) {
  if (IsWhitespaceCharacter(token)) { InsertCharacter(token.data); return; }
  if (token.type == Token::Type::Comment) { InsertComment(token); return; }
  if (token.type == Token::Type::ProcessingInstruction) { InsertProcessingInstruction(token); return; }
  if (token.type == Token::Type::Doctype) return;
  if (IsStart(token, "html")) { InBody(token); return; }
  if (IsStart(token, "frameset")) { InsertHtmlElement(token); return; }
  if (IsEnd(token, "frameset")) {
    if (CurrentNode() == open_[0]) return;
    Pop();
    if (!fragmentContext_ && !IsHtmlElement(CurrentNode(), "frameset")) mode_ = Mode::AfterFrameset;
    return;
  }
  if (IsStart(token, "frame")) {
    InsertHtmlElement(token);
    Pop();
    return;
  }
  if (IsStart(token, "noframes")) { InHead(token); return; }
  if (token.type == Token::Type::EndOfFile) { StopParsing(); return; }
}

void TreeBuilder::AfterFrameset(Token& token) {
  if (IsWhitespaceCharacter(token)) { InsertCharacter(token.data); return; }
  if (token.type == Token::Type::Comment) { InsertComment(token); return; }
  if (token.type == Token::Type::ProcessingInstruction) { InsertProcessingInstruction(token); return; }
  if (token.type == Token::Type::Doctype) return;
  if (IsStart(token, "html")) { InBody(token); return; }
  if (IsEnd(token, "html")) { mode_ = Mode::AfterAfterFrameset; return; }
  if (IsStart(token, "noframes")) { InHead(token); return; }
  if (token.type == Token::Type::EndOfFile) { StopParsing(); return; }
}

void TreeBuilder::AfterAfterBody(Token& token) {
  if (token.type == Token::Type::Comment) { Location at{document_, nullptr}; InsertComment(token, &at); return; }
  if (token.type == Token::Type::ProcessingInstruction) { Location at{document_, nullptr}; InsertProcessingInstruction(token, &at); return; }
  if (token.type == Token::Type::Doctype || IsWhitespaceCharacter(token) || IsStart(token, "html")) { InBody(token); return; }
  if (token.type == Token::Type::EndOfFile) { StopParsing(); return; }
  Error("unexpected-token-after-after-body");
  mode_ = Mode::InBody;
  Process(token);
}

void TreeBuilder::AfterAfterFrameset(Token& token) {
  if (token.type == Token::Type::Comment) { Location at{document_, nullptr}; InsertComment(token, &at); return; }
  if (token.type == Token::Type::ProcessingInstruction) { Location at{document_, nullptr}; InsertProcessingInstruction(token, &at); return; }
  if (token.type == Token::Type::Doctype || IsWhitespaceCharacter(token) || IsStart(token, "html")) { InBody(token); return; }
  if (token.type == Token::Type::EndOfFile) { StopParsing(); return; }
  if (IsStart(token, "noframes")) { InHead(token); return; }
}

// ---- Foreign content ----

void TreeBuilder::ProcessForeign(Token& token) {
  switch (token.type) {
    case Token::Type::Character:
      if (IsNullCharacter(token)) {
        InsertCharacter("\xEF\xBF\xBD");
        return;
      }
      InsertCharacter(token.data);
      if (!IsWhitespaceCharacter(token)) framesetOk_ = false;
      return;
    case Token::Type::Comment:
      InsertComment(token);
      return;
    case Token::Type::ProcessingInstruction:
      InsertProcessingInstruction(token);
      return;
    case Token::Type::Doctype:
    case Token::Type::EndOfFile:
      return;
    default:
      break;
  }

  if (token.type == Token::Type::StartTag) {
    const bool breaksOut =
        IsOneOf(token.name, {"b", "big", "blockquote", "body", "br", "center", "code", "dd", "div", "dl", "dt", "em", "embed", "h1", "h2", "h3", "h4", "h5", "h6", "head", "hr", "i",
                             "img", "li", "listing", "menu", "meta", "nobr", "ol", "p", "pre", "ruby", "s", "small", "span", "strong", "strike", "sub", "sup", "table", "tt", "u",
                             "ul", "var"}) ||
        (token.name == "font" && (token.Attribute("color") || token.Attribute("face") || token.Attribute("size")));
    if (breaksOut) {
      Error("unexpected-html-element-in-foreign-content");
      while (CurrentNode() && !IsMathTextIntegrationPoint(CurrentNode()) && !IsHtmlIntegrationPoint(CurrentNode()) && CurrentNode()->namespaceUri != kHtml) Pop();
      ProcessUsing(mode_, token);
      return;
    }
    const Element* adjusted = AdjustedCurrentNode();
    if (adjusted->namespaceUri == kMathMl) AdjustMathAttributes(token);
    if (adjusted->namespaceUri == kSvg) {
      for (size_t i = 0; i < kSvgTagAdjustmentCount; ++i) {
        if (token.name == kSvgTagAdjustments[i].from) token.name = kSvgTagAdjustments[i].to;
      }
      AdjustSvgAttributes(token);
    }
    AdjustForeignAttributes(token);
    const std::string ns = adjusted->namespaceUri;
    InsertForeignElement(token, ns, false);
    if (token.selfClosing) Pop();
    return;
  }

  // End tags.
  if (IsEndOneOf(token, {"br", "p"})) {
    Error("unexpected-html-element-in-foreign-content");
    while (CurrentNode() && !IsMathTextIntegrationPoint(CurrentNode()) && !IsHtmlIntegrationPoint(CurrentNode()) && CurrentNode()->namespaceUri != kHtml) Pop();
    ProcessUsing(mode_, token);
    return;
  }
  if (token.name == "script" && CurrentNode()->namespaceUri == kSvg && CurrentNode()->localName == "script") {
    Pop();
    return;
  }
  size_t index = open_.size() - 1;
  Element* node = open_[index];
  if (LowerAscii(node->localName) != token.name) Error("unexpected-end-tag");
  for (;;) {
    if (index == 0) return;
    if (LowerAscii(node->localName) == token.name) {
      while (CurrentNode() != node) Pop();
      Pop();
      return;
    }
    node = open_[--index];
    if (node->namespaceUri == kHtml) {
      ProcessUsing(mode_, token);
      return;
    }
  }
}

// ---- Fragments ----

void TreeBuilder::SetUpFragment(Element* context, dom::DocumentFragment* fragment) {
  fragmentContext_ = context;
  rootInsertionTarget_ = fragment;
  if (context->namespaceUri == kHtml) {
    const std::string& name = context->localName;
    if (name == "title" || name == "textarea") tokenizer_.SetState(Tokenizer::State::Rcdata);
    else if (IsOneOf(name, {"style", "xmp", "iframe", "noembed", "noframes"})) tokenizer_.SetState(Tokenizer::State::Rawtext);
    else if (name == "script") tokenizer_.SetState(Tokenizer::State::ScriptData);
    else if (name == "noscript" && scripting_ != ScriptingMode::Disabled) tokenizer_.SetState(Tokenizer::State::Rawtext);
    else if (name == "plaintext") tokenizer_.SetState(Tokenizer::State::Plaintext);
  }
  Element* root = dom::NewElement(ctx_, document_, "html", kHtml);
  dom::InsertUnchecked(root, document_, nullptr);
  open_.push_back(root);
  if (IsHtmlElement(context, "template")) templateModes_.push_back(Mode::InTemplate);
  ResetInsertionMode();
  for (Node* node = context; node; node = node->parentNode) {
    if (IsHtmlElement(AsElement(node), "form")) {
      form_ = static_cast<Element*>(node);
      break;
    }
  }
}

}  // namespace solar::html
