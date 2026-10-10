#pragma once

#include <string>
#include <vector>

// Logical properties (https://drafts.csswg.org/css-logical/): which physical property a logical one stands for, in an element
// with a given writing mode and direction.
namespace solar::css {

struct WritingContext {
  std::string mode = "horizontal-tb";  // writing-mode
  std::string direction = "ltr";
};

// Whether the name is a logical longhand (margin-inline-start, inline-size, border-start-start-radius...).
bool IsLogicalProperty(const std::string& name);
// The physical longhand a logical one is in this context; empty if the name is not a logical longhand.
std::string PhysicalOf(const std::string& logical, const WritingContext& context);
// The logical longhands that are, in this context, the same property as this physical one.
std::vector<std::string> LogicalsOf(const std::string& physical, const WritingContext& context);
// Whether a physical property can have logical ones standing for it at all.
bool HasLogicalCounterparts(const std::string& physical);

}  // namespace solar::css
