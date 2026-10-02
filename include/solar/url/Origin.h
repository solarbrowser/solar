#pragma once

#include <string>

#include "solar/url/Url.h"

namespace solar::url {

// The ASCII serialization of the URL's origin; "null" stands for an opaque origin.
std::string SerializeOrigin(const Url& url);

}  // namespace solar::url
