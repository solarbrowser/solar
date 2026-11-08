/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <string_view>

namespace solar::net {

// Opaque Response Blocking (https://fetch.spec.whatwg.org/#orb-algorithm): what a cross-origin response to a no-cors request may
// be given to the page as. Block makes it a network error.
enum class OrbVerdict { Allow, Block, NeedBody };

// By the head alone: the status, the Content-Type (empty when there is none), whether X-Content-Type-Options says nosniff, and the
// Content-Range (empty when there is none).
OrbVerdict OrbCheckHead(int status, std::string_view contentType, bool nosniff, std::string_view contentRange);
// By the body, for a response the head could not decide: true where it is HTML, XML or JSON, which are blocked.
bool OrbBodyIsBlocked(std::string_view body);

}  // namespace solar::net
