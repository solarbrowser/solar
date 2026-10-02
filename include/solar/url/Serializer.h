#pragma once

#include <string>

#include "solar/url/Url.h"

namespace solar::url {

std::string Serialize(const Url& url, bool excludeFragment = false);

}  // namespace solar::url
