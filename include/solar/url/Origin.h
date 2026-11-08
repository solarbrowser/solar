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

// What a process is made for: the scheme and the registrable domain (the host where it has none), so that two URLs have the same one just when IsSameSite says they are the same site; every
// file: address is one site. Empty for an opaque origin (which is a site of its own, and a process of its own).
std::string SiteOf(const Url& url);

}  // namespace solar::url
