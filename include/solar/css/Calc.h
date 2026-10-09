#pragma once

#include <optional>

#include "solar/css/Syntax.h"

// Math functions (https://drafts.csswg.org/css-values/#math-function) evaluated: calc(), min(), max(), clamp(), round(),
// the trigonometric and exponential ones, in the canonical unit of their type.
namespace solar::css {

enum class MathKind { Number, Percentage, Length, Angle, Time, Frequency, Resolution, Flex };

struct MathValue {
  double value = 0;
  MathKind kind = MathKind::Number;  // a length is in px, an angle in deg, a time in s, a frequency in Hz, a resolution in dppx
};

// The value of a numeric token (a number, a percentage, a dimension of a unit with an absolute canonical form) or of a
// math function. Nothing for what cannot be known without more context: em, vh and the like, a percentage mixed with
// a dimension, a function that is not valid.
std::optional<MathValue> EvaluateNumeric(const ComponentValue& value);

bool IsMathFunctionName(const std::string& name);
// Whether the function is a valid calculation: its operands combine in types that can be (a length and a percentage in
// a sum, a number times a length), without knowing what the em or the percentage is.
bool IsValidMathFunction(const ComponentValue& function);

// "simplify a calculation tree" and "serialize" it: the math function as the specified value of a property reads it.
// calc(1px + 2px) is calc(3px), calc(2 * 3) calc(6), and what cannot be known yet (em, var()) stays, its terms in the
// order the standard gives. Nothing if the function is not a valid calculation.
// `allowedIdents` are keywords that may stand as operands (the channel names of a relative color).
std::optional<ComponentValue> NormalizeMathFunction(const ComponentValue& function, const std::vector<std::string>* allowedIdents = nullptr);

}  // namespace solar::css
