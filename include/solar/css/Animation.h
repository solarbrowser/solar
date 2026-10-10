#pragma once

#include <map>
#include <optional>
#include <string>

#include "solar/dom/Node.h"

// Animation (https://drafts.csswg.org/css-values/#combining-values, web-animations-1): the values animations give to properties,
// which come over the declared ones in the cascade, and how two values of a property are combined.
namespace solar::css {

using AnimatedValues = std::map<std::string, std::string>;

// The values (longhand → text) animations give the element's pseudo-element ("" for the element). Replaces what was there;
// an empty map takes them away. Computed styles are made again.
void SetAnimatedValues(dom::Element* element, const std::string& pseudo, AnimatedValues values);
// The value of the property that animations give, or null; always null while suppressed.
const std::string* AnimatedValue(dom::Element* element, const std::string& pseudo, const std::string& property);
// While suppressed computed styles are the ones without animations (the value an effect adds to or interpolates from).
void SuppressAnimatedValues(bool suppressed);
bool AnimatedValuesSuppressed();

// While one of these is alive, reading a computed style does not bring what animations give up to date first (the animations are
// themselves reading styles).
struct NoStyleFlush {
  NoStyleFlush();
  ~NoStyleFlush();
};
bool StyleFlushSuppressed();

// The value between two computed values of the property: `from` at 0, `to` at 1 (beyond that too, for easing that overshoots).
// Nothing if the two cannot be interpolated (the property is not animatable, or the values are of shapes that do not match).
std::optional<std::string> InterpolateValues(const std::string& property, const std::string& from, const std::string& to, double progress);
// `a` plus `b` (composite add), and `a` accumulated onto `b` (b is the underlying value, a the effect's, scaled by `count` times).
std::optional<std::string> AddValues(const std::string& property, const std::string& a, const std::string& b);
std::optional<std::string> ScaleValue(const std::string& property, const std::string& value, double factor);
// Whether values of the property interpolate (not only flip), and the distance between two values (for paced timing).
bool IsInterpolableProperty(const std::string& property);
// The value with its numbers clamped to what the property allows.
std::string ClampValue(const std::string& property, const std::string& value);

}  // namespace solar::css
