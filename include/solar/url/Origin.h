/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <string>

#include "solar/url/Url.h"

namespace solar::url {

// The ASCII serialization of the URL's origin; "null" stands for an opaque origin.
std::string SerializeOrigin(const Url& url);

// Whether two URLs are schemeful same-site (HTML Standard, "same site"): their origins are tuples with
// the same scheme and the same registrable domain, or the same host where a host has none, as an
// IP address or "localhost" has not. Ports do not matter. An opaque origin is same-site only with
// itself, which two URLs cannot tell.
bool IsSameSite(const Url& a, const Url& b);

}  // namespace solar::url
