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

// What turning a specified value into a computed one needs to know: the sizes the relative units are relative to.
struct ComputeContext {
  double fontSize = 16;        // of the element, in px
  double rootFontSize = 16;
  double lineHeight = 18.4;    // of the element (what 1lh is), in px
  double rootLineHeight = 18.4;
  // The font-relative units, in px, from the element's font: negative where it is not known (a fixed fraction of the size is used).
  double exHeight = -1, chWidth = -1, capHeight = -1, icWidth = -1;
  double rootExHeight = -1, rootChWidth = -1, rootCapHeight = -1, rootIcWidth = -1;
  double viewportWidth = 800;
  double viewportHeight = 600;
  // The nearest size container, for the container query units (negative where there is none: they are then the small viewport's).
  double containerWidth = -1;
  double containerHeight = -1;
  bool containerVertical = false;
  std::string currentColor = "rgb(0, 0, 0)";  // what currentcolor stands for
  std::string colorScheme = "light";
};

// The component value with its relative lengths (em, rem, vw...) worked out against the context, everything else as it was.
ComponentValue ResolveRelativeLengths(const ComponentValue& value, const ComputeContext& context);

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
// With a ComputeContext the matched value comes out as the computed value: lengths in px, times in s, angles in deg,
// colors resolved, calculations resolved as far as they can be.
bool MatchPropertyValue(const PropertyDefinition& property, const ComponentValues& values, ValueMatch& out, const ComputeContext* compute = nullptr);
// The same for a syntax given as text, such as a registered custom property's, a descriptor's or CSS.supports'.
bool MatchSyntax(std::string_view syntax, const ComponentValues& values, ValueMatch& out, const ComputeContext* compute = nullptr);

// The canonical text of a list of component values: single spaces, ", " after commas, " / " around slashes.
std::string SerializeValue(const ComponentValues& values);
// A number as CSS writes it: the shortest text that reads back as the same value, no exponent, no trailing zeros.
std::string FormatNumber(double number);

// attr( <attr-name> <attr-type>? , <declaration-value>? ), taken apart.
struct AttrCall {
  enum class Type { Raw, Number, Unit, Syntax };
  bool anyNamespace = false;  // *|name
  bool hasNamespace = false;  // prefix|name (the prefix is in `prefix`)
  std::string prefix;
  std::string name;
  Type type = Type::Raw;
  bool explicitType = false;
  std::string unit;    // for Unit: "%" or the unit
  std::string syntax;  // for Syntax
  ComponentValues fallback;
  bool hasFallback = false;
};
bool ParseAttrCall(const ComponentValue& function, AttrCall& out);

// The address that a relative url() in a value being matched is taken against; empty leaves them as written.
void SetValueBaseUrl(const std::string& base);
const std::string& ValueBaseUrl();

// The keywords a value definition syntax names (through the types it uses, except the ones for names and colors).
std::vector<std::string> KeywordsOfSyntax(std::string_view syntax);

// The CSS-wide keywords (initial, inherit, unset, revert, revert-layer) a value can be on its own.
bool IsCssWideKeyword(const ComponentValues& values);
// Whether a value has a var(), env() or attr() in it, so that it is known only when it is used.
bool ContainsSubstitution(const ComponentValues& values);
// Whether the var() functions in a value are well formed: a custom property name, then the end or a comma.
bool ValidSubstitutions(const ComponentValues& values);

}  // namespace solar::css
