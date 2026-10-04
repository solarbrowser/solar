#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "quanta/Embed.h"
#include "solar/dom/Node.h"
#include "solar/html/TreeBuilder.h"

// XML (https://www.w3.org/TR/xml/ and its namespaces): a parser from text to the DOM, and the serialization back
// (https://w3c.github.io/DOM-Parsing/#dfn-xml-serialization). A document that is not well-formed is not given.
namespace solar::html {

struct XmlResult {
  bool ok = true;
  std::string error;  // what was wrong, with where
};

// Parses `markup` into `document`, which is empty: its children, its doctype, and the scripts of an XHTML or SVG
// document, each given to `scripts` as soon as its end tag is read. Stops at the first error and says what it was.
XmlResult ParseXmlDocument(Quanta::Context& ctx, dom::Document* document, std::string_view markup, ScriptHandler scripts = nullptr);

// "parse an XML fragment": `markup` as the content of `context`, in the namespaces that are in scope there, into
// `fragment`.
XmlResult ParseXmlFragment(Quanta::Context& ctx, dom::Element* context, std::string_view markup, dom::DocumentFragment* fragment);

// "XML serialization" of a node; with `requireWellFormed`, nothing if it would not be well-formed.
std::optional<std::string> SerializeXml(const dom::Node* node, bool requireWellFormed);
// The same for the children of a node, which is what innerHTML is in an XML document.
std::optional<std::string> SerializeXmlChildren(const dom::Node* node, bool requireWellFormed);

}  // namespace solar::html
