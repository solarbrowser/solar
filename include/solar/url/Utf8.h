#pragma once

#include <string>
#include <string_view>

namespace solar::url {

// Ill-formed sequences become U+FFFD following the WHATWG Encoding decoder,
// which differs from other decoders in how many bytes one replacement consumes.
std::u32string DecodeUtf8(std::string_view bytes);

void AppendUtf8(std::string& out, char32_t codePoint);

// The USVString conversion: valid UTF-8 comes back unchanged, ill-formed bytes become U+FFFD.
std::string ScrubUtf8(std::string_view bytes);

}  // namespace solar::url
