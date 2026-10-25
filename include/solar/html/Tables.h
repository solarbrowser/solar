/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <cstddef>

// What the tree builder's standard has as lists: the case fixes of SVG attributes and tag names, and the
// document types that decide a document's mode. Generated from the standard's text: see src/html/Tables.cpp.
namespace solar::html {

struct NameMapping {
  const char* from;
  const char* to;
};

extern const NameMapping kSvgAttributeAdjustments[];
extern const size_t kSvgAttributeAdjustmentCount;
extern const NameMapping kSvgTagAdjustments[];
extern const size_t kSvgTagAdjustmentCount;

extern const char* const kQuirksPublicExact[3];
extern const char* const kQuirksSystemExact[1];
extern const char* const kQuirksPublicPrefixes[55];
extern const char* const kQuirksPublicPrefixesWithoutSystem[2];
extern const char* const kLimitedQuirksPublicPrefixes[2];
extern const char* const kLimitedQuirksPublicPrefixesWithSystem[2];

}  // namespace solar::html
