/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <chrono>
#include <optional>
#include <string_view>

namespace solar::net {

// The date in a Date, Expires, Last-Modified or Set-Cookie header. It reads RFC 6265's cookie-date,
// which is lenient: "Sun, 06 Nov 1994 08:49:37 GMT" and every form HTTP has used for it, and a good
// many others. None if no date can be made of the text.
std::optional<std::chrono::system_clock::time_point> ParseHttpDate(std::string_view text);

}  // namespace solar::net
