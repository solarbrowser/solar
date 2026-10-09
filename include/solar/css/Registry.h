#pragma once

#include <optional>
#include <string>
#include <vector>

#include "solar/css/Syntax.h"
#include "solar/dom/Node.h"

// Registered custom properties (https://drafts.css-houdini.org/css-properties-values-api/): what @property and
// CSS.registerProperty say a custom property is: its syntax, whether it inherits, and its initial value.
namespace solar::css {

void NoteStyleChangeForRegistry();

struct RegisteredProperty {
  std::string name;
  std::string syntax;         // as written ("<length>+", "*")
  std::string matcherSyntax;  // in the language the value matcher reads
  bool inherits = false;
  std::optional<std::string> initialValue;  // as written
  bool universal() const { return matcherSyntax == "*"; }
};

// "parse a syntax definition": the registration's syntax in the matcher's language, or nothing if it is not one.
std::optional<std::string> ParseSyntaxDefinition(const std::string& text);

// Checks and builds a registration; nothing (and `error` the DOMException name: SyntaxError, TypeError) if it cannot be.
std::optional<RegisteredProperty> MakeRegistration(const std::string& name, const std::string& syntax, bool inherits, const std::optional<std::string>& initialValue, std::string& error);

// CSS.registerProperty: false (error "InvalidModificationError") if the name is registered already.
bool RegisterProperty(dom::Document* document, const RegisteredProperty& property, std::string& error);

// The registration in effect for a name in a document: one made with CSS.registerProperty, or else the last valid @property
// rule of the document's sheets.
std::optional<RegisteredProperty> LookupRegistered(dom::Document* document, const std::string& name);

// The names of the registered properties of a document.
std::vector<std::string> RegisteredNames(dom::Document* document);

}  // namespace solar::css
