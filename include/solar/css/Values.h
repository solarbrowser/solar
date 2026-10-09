#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "solar/css/Properties.h"
#include "solar/css/Syntax.h"

// CSS Values and Units (https://drafts.csswg.org/css-values/): the value definition syntax, matching a value against
// it, and the canonical form a matched value is serialized in.
namespace solar::css {

// How a value matched a syntax. `normalized` is the value with keywords, units and numbers in their canonical form.
// For a shorthand, `assigned` says which of the top-level components of the value each of the `<'longhand'>`s of its
// syntax took: [begin, end) positions in `normalized` (whitespace not counted), in the order they were matched.
struct ValueMatch {
  ComponentValues normalized;
  struct Assignment {
    std::string property;
    size_t begin = 0;
    size_t end = 0;
  };
  std::vector<Assignment> assigned;
};

// Whether `values` (the value of a declaration, without !important) is in the language of the property's syntax.
bool MatchPropertyValue(const PropertyDefinition& property, const ComponentValues& values, ValueMatch& out);
// The same for a syntax given as text, such as a registered custom property's, a descriptor's or CSS.supports'.
bool MatchSyntax(std::string_view syntax, const ComponentValues& values, ValueMatch& out);

// The canonical text of a list of component values: single spaces, ", " after commas, " / " around slashes.
std::string SerializeValue(const ComponentValues& values);
// A number as CSS writes it: the shortest text that reads back as the same value, no exponent, no trailing zeros.
std::string FormatNumber(double number);

// The CSS-wide keywords (initial, inherit, unset, revert, revert-layer) a value can be on its own.
bool IsCssWideKeyword(const ComponentValues& values);
// Whether a value has a var(), env() or attr() in it, so that it is known only when it is used.
bool ContainsSubstitution(const ComponentValues& values);

}  // namespace solar::css
