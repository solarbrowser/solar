#include "solar/url/Host.h"

#include <array>
#include <cstdint>
#include <vector>

#include "solar/url/Idna.h"
#include "solar/url/PercentEncode.h"
#include "solar/url/Utf8.h"

namespace solar::url {

namespace {

// Past this the value is out of range for any IPv4 part, and capping keeps long
// digit strings from overflowing.
constexpr uint64_t kIpv4NumberCap = uint64_t{1} << 40;

bool IsAsciiDigit(char c) { return c >= '0' && c <= '9'; }

int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

std::vector<std::string_view> Split(std::string_view s, char separator) {
  std::vector<std::string_view> parts;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == separator) {
      parts.push_back(s.substr(start, i - start));
      start = i + 1;
    }
  }
  return parts;
}

std::optional<uint64_t> ParseIpv4Number(std::string_view input) {
  if (input.empty()) return std::nullopt;

  unsigned radix = 10;
  if (input.size() >= 2 && input[0] == '0' && (input[1] == 'x' || input[1] == 'X')) {
    input.remove_prefix(2);
    radix = 16;
  } else if (input.size() >= 2 && input[0] == '0') {
    input.remove_prefix(1);
    radix = 8;
  }
  if (input.empty()) return 0;

  uint64_t value = 0;
  for (char c : input) {
    int digit = HexValue(c);
    if (digit < 0 || static_cast<unsigned>(digit) >= radix) return std::nullopt;
    value = value * radix + digit;
    if (value > kIpv4NumberCap) value = kIpv4NumberCap;
  }
  return value;
}

bool EndsInANumber(std::string_view input) {
  std::vector<std::string_view> parts = Split(input, '.');
  if (parts.back().empty()) parts.pop_back();
  std::string_view last = parts.back();

  bool allDigits = !last.empty();
  for (char c : last) allDigits = allDigits && IsAsciiDigit(c);
  if (allDigits) return true;

  return ParseIpv4Number(last).has_value();
}

std::optional<std::string> ParseIpv4(std::string_view input) {
  std::vector<std::string_view> parts = Split(input, '.');
  if (parts.back().empty() && parts.size() > 1) parts.pop_back();
  if (parts.size() > 4) return std::nullopt;

  std::vector<uint64_t> numbers;
  for (std::string_view part : parts) {
    std::optional<uint64_t> number = ParseIpv4Number(part);
    if (!number) return std::nullopt;
    numbers.push_back(*number);
  }

  for (size_t i = 0; i + 1 < numbers.size(); ++i) {
    if (numbers[i] > 255) return std::nullopt;
  }
  uint64_t lastLimit = uint64_t{1} << (8 * (5 - numbers.size()));
  if (numbers.back() >= lastLimit) return std::nullopt;

  uint64_t ipv4 = numbers.back();
  for (size_t i = 0; i + 1 < numbers.size(); ++i) {
    ipv4 += numbers[i] << (8 * (3 - i));
  }

  std::string out;
  for (int shift = 24; shift >= 0; shift -= 8) {
    out += std::to_string((ipv4 >> shift) & 0xFF);
    if (shift != 0) out.push_back('.');
  }
  return out;
}

std::optional<size_t> FindCompressedPieceIndex(const std::array<uint16_t, 8>& address) {
  std::optional<size_t> longestIndex;
  size_t longestSize = 1;
  std::optional<size_t> foundIndex;
  size_t foundSize = 0;

  for (size_t i = 0; i < address.size(); ++i) {
    if (address[i] != 0) {
      if (foundSize > longestSize) {
        longestIndex = foundIndex;
        longestSize = foundSize;
      }
      foundIndex.reset();
      foundSize = 0;
    } else {
      if (!foundIndex) foundIndex = i;
      ++foundSize;
    }
  }
  if (foundSize > longestSize) return foundIndex;
  return longestIndex;
}

std::string SerializeIpv6(const std::array<uint16_t, 8>& address) {
  static constexpr char kLowerHex[] = "0123456789abcdef";
  std::optional<size_t> compress = FindCompressedPieceIndex(address);
  std::string out;
  bool ignore0 = false;

  for (size_t i = 0; i < address.size(); ++i) {
    if (ignore0 && address[i] == 0) continue;
    ignore0 = false;

    if (compress == i) {
      out += i == 0 ? "::" : ":";
      ignore0 = true;
      continue;
    }

    bool leading = true;
    for (int shift = 12; shift >= 0; shift -= 4) {
      unsigned nibble = (address[i] >> shift) & 0xF;
      if (nibble == 0 && leading && shift != 0) continue;
      leading = false;
      out.push_back(kLowerHex[nibble]);
    }
    if (i != 7) out.push_back(':');
  }
  return out;
}

std::optional<std::string> ParseIpv6(std::string_view input) {
  std::array<uint16_t, 8> address{};
  size_t pieceIndex = 0;
  std::optional<size_t> compress;
  size_t pointer = 0;

  auto at = [&](size_t i) -> int { return i < input.size() ? static_cast<unsigned char>(input[i]) : -1; };

  if (at(pointer) == ':') {
    if (at(pointer + 1) != ':') return std::nullopt;
    pointer += 2;
    compress = ++pieceIndex;
  }

  while (at(pointer) != -1) {
    if (pieceIndex == 8) return std::nullopt;

    if (at(pointer) == ':') {
      if (compress) return std::nullopt;
      ++pointer;
      compress = ++pieceIndex;
      continue;
    }

    uint32_t value = 0;
    size_t length = 0;
    while (length < 4 && at(pointer) != -1 && HexValue(static_cast<char>(at(pointer))) >= 0) {
      value = value * 0x10 + HexValue(static_cast<char>(at(pointer)));
      ++pointer;
      ++length;
    }

    if (at(pointer) == '.') {
      if (length == 0) return std::nullopt;
      pointer -= length;
      if (pieceIndex > 6) return std::nullopt;

      size_t numbersSeen = 0;
      while (at(pointer) != -1) {
        std::optional<uint32_t> ipv4Piece;
        if (numbersSeen > 0) {
          if (at(pointer) == '.' && numbersSeen < 4) {
            ++pointer;
          } else {
            return std::nullopt;
          }
        }
        if (at(pointer) == -1 || !IsAsciiDigit(static_cast<char>(at(pointer)))) return std::nullopt;

        while (at(pointer) != -1 && IsAsciiDigit(static_cast<char>(at(pointer)))) {
          uint32_t number = at(pointer) - '0';
          if (!ipv4Piece) {
            ipv4Piece = number;
          } else if (*ipv4Piece == 0) {
            return std::nullopt;
          } else {
            ipv4Piece = *ipv4Piece * 10 + number;
          }
          if (*ipv4Piece > 255) return std::nullopt;
          ++pointer;
        }

        address[pieceIndex] = static_cast<uint16_t>(address[pieceIndex] * 0x100 + *ipv4Piece);
        ++numbersSeen;
        if (numbersSeen == 2 || numbersSeen == 4) ++pieceIndex;
      }
      if (numbersSeen != 4) return std::nullopt;
      break;
    }

    if (at(pointer) == ':') {
      ++pointer;
      if (at(pointer) == -1) return std::nullopt;
    } else if (at(pointer) != -1) {
      return std::nullopt;
    }

    address[pieceIndex] = static_cast<uint16_t>(value);
    ++pieceIndex;
  }

  if (compress) {
    size_t swaps = pieceIndex - *compress;
    pieceIndex = 7;
    while (pieceIndex != 0 && swaps > 0) {
      std::swap(address[pieceIndex], address[*compress + swaps - 1]);
      --pieceIndex;
      --swaps;
    }
  } else if (pieceIndex != 8) {
    return std::nullopt;
  }

  return SerializeIpv6(address);
}

bool IsForbiddenHostCodePoint(char c) {
  switch (c) {
    case '\0': case '\t': case '\n': case '\r': case ' ': case '#': case '/': case ':':
    case '<': case '>': case '?': case '@': case '[': case '\\': case ']': case '^':
    case '|':
      return true;
    default:
      return false;
  }
}

std::optional<std::string> ParseOpaqueHost(std::string_view input) {
  for (char c : input) {
    if (IsForbiddenHostCodePoint(c)) return std::nullopt;
  }
  std::string out;
  AppendPercentEncoded(out, input, EncodeSet::C0Control);
  return out;
}

}  // namespace

std::optional<std::string> ParseHost(std::string_view input, bool isOpaque) {
  if (!input.empty() && input.front() == '[') {
    if (input.back() != ']') return std::nullopt;
    std::optional<std::string> ipv6 = ParseIpv6(input.substr(1, input.size() - 2));
    if (!ipv6) return std::nullopt;
    return "[" + *ipv6 + "]";
  }

  if (isOpaque) return ParseOpaqueHost(input);

  std::u32string domain = DecodeUtf8(PercentDecode(input));
  std::optional<std::string> asciiDomain = DomainToAscii(domain);
  if (!asciiDomain) return std::nullopt;

  if (EndsInANumber(*asciiDomain)) return ParseIpv4(*asciiDomain);
  return asciiDomain;
}

}  // namespace solar::url
