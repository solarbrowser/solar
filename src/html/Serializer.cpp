#include "solar/html/Serializer.h"

#include <algorithm>
#include <string_view>

namespace solar::html {

namespace {

using dom::Element;
using dom::Node;

bool IsVoid(const Element* element) {
  if (element->namespaceUri != dom::kHtmlNamespace) return false;
  static const std::string_view kVoid[] = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "source", "track", "wbr", "basefont", "bgsound", "frame", "keygen", "param"};
  return std::find(std::begin(kVoid), std::end(kVoid), element->localName) != std::end(kVoid);
}

// Escaping a string: & and the no-break space always, < and > in text, and " in an attribute's value.
void Escape(const std::string& text, bool attributeMode, std::string& out) {
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '&') out += "&amp;";
    else if (c == '<' && !attributeMode) out += "&lt;";
    else if (c == '>' && !attributeMode) out += "&gt;";
    else if (c == '"' && attributeMode) out += "&quot;";
    else if (static_cast<unsigned char>(c) == 0xC2 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0xA0) {
      out += "&nbsp;";
      ++i;
    } else out += c;
  }
}

std::string AttributeName(const dom::Attr* attribute) {
  if (attribute->namespaceUri.empty()) return attribute->localName;
  if (attribute->namespaceUri == dom::kXmlNamespace) return "xml:" + attribute->localName;
  if (attribute->namespaceUri == dom::kXmlnsNamespace) return attribute->localName == "xmlns" ? "xmlns" : "xmlns:" + attribute->localName;
  if (attribute->namespaceUri == "http://www.w3.org/1999/xlink") return "xlink:" + attribute->localName;
  return attribute->QualifiedName();
}

struct Options {
  bool scripting = false;
  bool serializableShadowRoots = false;
  const std::vector<const dom::ShadowRoot*>* shadowRoots = nullptr;
};

void SerializeInto(const Node* node, const Options& options, std::string& out);

bool TextIsLiteral(const Node* text, bool scripting) {
  const Element* parent = dom::AsElement(text->parentNode);
  return parent && parent->namespaceUri == dom::kHtmlNamespace &&
         (parent->localName == "style" || parent->localName == "script" || parent->localName == "xmp" || parent->localName == "iframe" || parent->localName == "noembed" ||
          parent->localName == "noframes" || parent->localName == "plaintext" || (parent->localName == "noscript" && scripting));
}

// A node's own markup and what is under it.
void SerializeOne(const Node* node, const Options& options, std::string& out) {
  const bool scripting = options.scripting;
  switch (node->nodeType) {
    case dom::NodeType::Element: {
      const Element* element = static_cast<const Element*>(node);
      const bool known = element->namespaceUri == dom::kHtmlNamespace || element->namespaceUri == dom::kMathMlNamespace || element->namespaceUri == dom::kSvgNamespace;
      const std::string tagName = known ? element->localName : element->QualifiedName();
      out += "<" + tagName;
      // The is value of a customized built-in element, if it is not an attribute it already has.
      if (element->isValue && !element->FindAttribute("", "is")) {
        out += " is=\"";
        Escape(*element->isValue, true, out);
        out += "\"";
      }
      for (const dom::Attr* attribute : element->attributes) {
        out += " " + AttributeName(attribute) + "=\"";
        Escape(attribute->value, true, out);
        out += "\"";
      }
      out += ">";
      if (IsVoid(element)) return;
      SerializeInto(element, options, out);
      out += "</" + tagName + ">";
      return;
    }
    case dom::NodeType::Text:
    case dom::NodeType::CdataSection: {
      const std::string& data = static_cast<const dom::CharacterData*>(node)->data;
      if (TextIsLiteral(node, scripting)) out += data;
      else Escape(data, false, out);
      return;
    }
    case dom::NodeType::Comment:
      out += "<!--" + static_cast<const dom::CharacterData*>(node)->data + "-->";
      return;
    case dom::NodeType::ProcessingInstruction:
      out += "<?" + static_cast<const dom::CharacterData*>(node)->target + " " + static_cast<const dom::CharacterData*>(node)->data + "?>";
      return;
    case dom::NodeType::DocumentType:
      out += "<!DOCTYPE " + static_cast<const dom::DocumentType*>(node)->name + ">";
      return;
    default:
      return;
  }
}

void SerializeInto(const Node* node, const Options& options, std::string& out) {
  // An element that serializes as void has no children to give; a template's are its contents'.
  if (const Element* element = dom::AsElement(node)) {
    if (IsVoid(element)) return;
    if (element->namespaceUri == dom::kHtmlNamespace && element->localName == "template" && element->templateContents) node = element->templateContents;
    if (const dom::ShadowRoot* shadow = element->shadowRoot) {
      const bool listed = options.shadowRoots && std::find(options.shadowRoots->begin(), options.shadowRoots->end(), shadow) != options.shadowRoots->end();
      if ((options.serializableShadowRoots && shadow->serializable) || listed) {
        out += std::string("<template shadowrootmode=\"") + (shadow->mode == dom::ShadowMode::Open ? "open" : "closed") + "\"";
        if (shadow->delegatesFocus) out += " shadowrootdelegatesfocus=\"\"";
        if (shadow->serializable) out += " shadowrootserializable=\"\"";
        if (shadow->slotAssignment == dom::SlotAssignment::Manual) out += " shadowrootslotassignment=\"manual\"";
        if (shadow->clonable) out += " shadowrootclonable=\"\"";
        out += ">";
        SerializeInto(shadow, options, out);
        out += "</template>";
      }
    }
  }
  for (const Node* child = node->firstChild; child; child = child->nextSibling) SerializeOne(child, options, out);
}

}  // namespace

std::string SerializeChildren(const Node* node, bool scripting) {
  std::string out;
  Options options;
  options.scripting = scripting;
  SerializeInto(node, options, out);
  return out;
}

std::string SerializeChildrenWithShadowRoots(const Node* node, bool serializableShadowRoots, const std::vector<const dom::ShadowRoot*>& shadowRoots, bool scripting) {
  std::string out;
  Options options;
  options.scripting = scripting;
  options.serializableShadowRoots = serializableShadowRoots;
  options.shadowRoots = &shadowRoots;
  SerializeInto(node, options, out);
  return out;
}

std::string SerializeNode(const Node* node, bool scripting) {
  std::string out;
  Options options;
  options.scripting = scripting;
  if (node->IsDocument() || node->IsFragment()) SerializeInto(node, options, out);
  else SerializeOne(node, options, out);
  return out;
}

}  // namespace solar::html
