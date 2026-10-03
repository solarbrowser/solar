#pragma once

#include <string>
#include <string_view>

namespace solar::test {

// The encoding side, for servers in tests that answer with a compressed body.
std::string Gzip(std::string_view data);
std::string ZlibDeflate(std::string_view data);  // what Content-Encoding: deflate is meant to be
std::string RawDeflate(std::string_view data);   // what some servers send for it anyway
std::string Brotli(std::string_view data);
std::string Zstd(std::string_view data);

}  // namespace solar::test
