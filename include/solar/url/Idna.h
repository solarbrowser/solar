/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "solar/url/ValidationError.h"

namespace solar::url {

// The URL Standard's "domain parser" with beStrict false, the only mode the host parser uses.
// `errors` receives domain-to-ASCII when the strict run, which is only made then, fails.
std::optional<std::string> DomainToAscii(std::u32string_view domain, ValidationErrors* errors = nullptr);

}  // namespace solar::url
