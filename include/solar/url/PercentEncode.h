#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace solar::url {

enum class EncodeSet {
  C0Control,
  Fragment,
  Query,
  SpecialQuery,
  Path,
  Userinfo,
};

// Operates on bytes: every byte of a multi-byte UTF-8 sequence is above 0x7E,
// so each one is in every set and the result equals per-code-point encoding.
void AppendPercentEncoded(std::string& out, uint8_t byte, EncodeSet set);
void AppendPercentEncoded(std::string& out, std::string_view bytes, EncodeSet set);

std::string PercentDecode(std::string_view input);

}  // namespace solar::url
