#pragma once

#include <string>
#include <string_view>

namespace solar::url {

// Ill-formed sequences become U+FFFD following the WHATWG Encoding decoder,
// which differs from other decoders in how many bytes one replacement consumes.
std::u32string DecodeUtf8(std::string_view bytes);

void AppendUtf8(std::string& out, char32_t codePoint);

}  // namespace solar::url
