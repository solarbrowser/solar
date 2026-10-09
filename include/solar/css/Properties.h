#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// What the specifications say about each property and each type that a value definition syntax refers to. The table is
// generated (tools/genprops.py) from W3C's webref, so it is the specifications' and not this engine's.
namespace solar::css {

struct PropertyDefinition {
  const char* name;
  const char* syntax;   // the value definition syntax of the property (of the whole value, for a shorthand)
  const char* initial;
  bool inherited;
  std::vector<const char*> longhands;       // for a shorthand
  std::vector<const char*> resetLonghands;  // what a shorthand sets to the initial value besides its own longhands
};

struct TypeDefinition {
  const char* name;    // "<length-percentage>", "rgb()"
  const char* syntax;
};

extern const PropertyDefinition kPropertyDefinitions[];
extern const size_t kPropertyDefinitionCount;
extern const TypeDefinition kTypeDefinitions[];
extern const size_t kTypeDefinitionCount;

// The property of this name (lowercase), or null; the syntax of a type by its name, or null.
const PropertyDefinition* FindProperty(std::string_view name);
const char* FindTypeSyntax(std::string_view name);
inline bool IsShorthand(const PropertyDefinition& property) { return !property.longhands.empty(); }

}  // namespace solar::css
