/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <string>
#include <string_view>

namespace solar::url {

// Turns what a person typed into an address bar into a string the URL parser
// accepts. The URL Standard leaves this out of scope, so it is kept apart from
// Parse and never needs to follow the standard.
std::string Normalize(std::string_view typed);

}  // namespace solar::url
