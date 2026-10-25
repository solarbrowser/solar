/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <string>
#include <vector>

// Media queries (https://drafts.csswg.org/mediaqueries/) against the environment this engine presents: a screen of
// 800 by 600 CSS pixels at 1 dppx, in the light scheme, with a mouse.
namespace solar::dom {
struct Document;
}

namespace solar::css {

struct MediaEnvironment {
  double width = 800;
  double height = 600;
  double deviceWidth = 800;   // the screen
  double deviceHeight = 600;
  double resolution = 1;  // dppx
  std::string colorScheme = "light";
};

const MediaEnvironment& CurrentMediaEnvironment();
// The window the pages are in: its size (and the pixels it has for each CSS pixel). What depends on it is worked out again when it changes.
void SetCurrentMediaEnvironment(const MediaEnvironment& environment);
// The environment of a document: the viewport of a document in a frame is the frame's size.
MediaEnvironment EnvironmentFor(const dom::Document* document);

// Whether a serialized media query list matches: an empty list always does.
bool MediaListMatches(const std::vector<std::string>& queries, const MediaEnvironment& environment);
bool MediaQueryMatches(const std::string& query, const MediaEnvironment& environment);

}  // namespace solar::css
