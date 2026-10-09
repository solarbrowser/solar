#pragma once

#include <string>
#include <vector>

// Media queries (https://drafts.csswg.org/mediaqueries/) against the environment this engine presents: a screen of
// 800 by 600 CSS pixels at 1 dppx, in the light scheme, with a mouse.
namespace solar::css {

struct MediaEnvironment {
  double width = 800;
  double height = 600;
  double resolution = 1;  // dppx
  std::string colorScheme = "light";
};

const MediaEnvironment& CurrentMediaEnvironment();

// Whether a serialized media query list matches: an empty list always does.
bool MediaListMatches(const std::vector<std::string>& queries, const MediaEnvironment& environment);
bool MediaQueryMatches(const std::string& query, const MediaEnvironment& environment);

}  // namespace solar::css
