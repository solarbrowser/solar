/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace solar::net {

// A MIME type as the MIME Sniffing Standard has it (https://mimesniff.spec.whatwg.org/#mime-type-representation).
struct MimeType {
  std::string type, subtype;
  std::vector<std::pair<std::string, std::string>> parameters;  // in the order they were given, names unique and in lower case
};

// "Parse a MIME type": nothing where the text is none.
std::optional<MimeType> ParseMimeType(std::string_view text);
// "Serialize a MIME type".
std::string SerializeMimeType(const MimeType& mime);

}  // namespace solar::net
