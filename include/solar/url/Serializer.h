#pragma once

#include <string>

#include "solar/url/Url.h"

namespace solar::url {

std::string Serialize(const Url& url, bool excludeFragment = false);

std::string SerializePath(const Url& url);

}  // namespace solar::url
