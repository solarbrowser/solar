/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <string>
#include <string_view>

namespace solar::web {

// Subresource Integrity (https://www.w3.org/TR/SRI/): whether `bytes` match the integrity metadata (a space separated list of
// "sha256-", "sha384-" or "sha512-" hashes in base64). Metadata with no hash that is understood matches anything; otherwise
// the hashes of the strongest algorithm named are the ones, and any of them matching is enough.
bool IntegrityMatches(std::string_view metadata, std::string_view bytes);

}  // namespace solar::web
