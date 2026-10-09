#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "solar/css/Properties.h"
#include "solar/css/Values.h"

// Shorthand properties (https://drafts.csswg.org/css-cascade/#shorthand): a declaration of one is the declarations of its
// longhands, and a block with all the longhands of a shorthand is written with the shorthand.
namespace solar::css {

// One longhand a declaration comes to. A shorthand whose value has a var() in it gives every longhand a "pending"
// value: the longhand reads as empty, and the shorthand's own text is what is kept.
struct Longhand {
  std::string name;
  std::string value;
  std::string pendingShorthand;  // the shorthand it is pending on, if any
  std::string pendingText;       // and the text of that shorthand's value
};

// "parse a CSS value" of the property for a declaration: the longhands `name: text` is, expanded through nested
// shorthands. False when the value is not valid for the property. A property with no definition (a vendor-prefixed one)
// is its own longhand.
bool ExpandDeclaration(const std::string& name, const std::string& text, std::vector<Longhand>& out);

// The canonical text of the initial value of a property, or "initial" if the specification gives it only in prose.
std::string InitialValueText(const PropertyDefinition& property);

// The shorthands that contain `longhand`, directly or through another shorthand, those with the most longhands first.
const std::vector<const PropertyDefinition*>& ShorthandsOf(const std::string& longhand);
// The longhands a property comes to: itself, if it is one; for a shorthand, the longhands of its longhands, in order.
std::vector<std::string> LeavesOf(const PropertyDefinition& property);
// The longhands a shorthand sets to their initial values besides its own (border resets border-image).
std::vector<std::string> ResetLeavesOf(const PropertyDefinition& property);
bool IsShorthandProperty(const PropertyDefinition& property);

// What a block holds for the longhands of a shorthand, to be written as one declaration: its values in the order of
// the shorthand's longhands (empty for a longhand the block does not have).
struct ShorthandInput {
  std::vector<std::string> values;
  std::vector<bool> pending;
};

// "serialize a CSS value" of a shorthand from its longhands' values: the text, or nothing when the values cannot be
// written as one declaration of the shorthand.
std::optional<std::string> SerializeShorthand(const PropertyDefinition& shorthand, const ShorthandInput& input);

}  // namespace solar::css
