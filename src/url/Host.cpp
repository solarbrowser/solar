#include "solar/url/Host.h"

#include <array>
#include <cstdint>
#include <vector>

#include "solar/url/Idna.h"
#include "solar/url/PercentEncode.h"
#include "solar/url/Url.h"
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

struct Ipv4Number {
  uint64_t value;
  bool nonDecimal;
};

std::optional<Ipv4Number> ParseIpv4Number(std::string_view input) {
  if (input.empty()) return std::nullopt;

  unsigned radix = 10;
  bool nonDecimal = false;
  if (input.size() >= 2 && input[0] == '0' && (input[1] == 'x' || input[1] == 'X')) {
    input.remove_prefix(2);
    radix = 16;
    nonDecimal = true;
  } else if (input.size() >= 2 && input[0] == '0') {
    input.remove_prefix(1);
    radix = 8;
    nonDecimal = true;
  }
  if (input.empty()) return Ipv4Number{0, true};

  uint64_t value = 0;
  for (char c : input) {
    int digit = HexValue(c);
    if (digit < 0 || static_cast<unsigned>(digit) >= radix) return std::nullopt;
    value = value * radix + digit;
    if (value > kIpv4NumberCap) value = kIpv4NumberCap;
  }
  return Ipv4Number{value, nonDecimal};
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

std::optional<std::string> ParseIpv4(std::string_view input, ValidationErrors* errors) {
  std::vector<std::string_view> parts = Split(input, '.');
  if (parts.back().empty()) {
    Report(errors, ValidationError::Ipv4EmptyPart);
    if (parts.size() > 1) parts.pop_back();
  }
  if (parts.size() < 4) Report(errors, ValidationError::Ipv4TooFewParts);
  if (parts.size() > 4) {
    Report(errors, ValidationError::Ipv4TooManyParts);
    return std::nullopt;
  }

  std::vector<uint64_t> numbers;
  for (std::string_view part : parts) {
    std::optional<Ipv4Number> number = ParseIpv4Number(part);
    if (!number) {
      Report(errors, ValidationError::Ipv4NonNumericPart);
      return std::nullopt;
    }
    if (number->nonDecimal) Report(errors, ValidationError::Ipv4NonDecimalPart);
    numbers.push_back(number->value);
  }

  for (uint64_t number : numbers) {
    if (number > 255) {
      Report(errors, ValidationError::Ipv4OutOfRangePart);
      break;
    }
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

std::optional<std::string> ParseIpv6(std::string_view input, ValidationErrors* errors) {
  std::array<uint16_t, 8> address{};
  size_t pieceIndex = 0;
  std::optional<size_t> compress;
  size_t pointer = 0;

  auto at = [&](size_t i) -> int { return i < input.size() ? static_cast<unsigned char>(input[i]) : -1; };

  if (at(pointer) == ':') {
    if (at(pointer + 1) != ':') {
      Report(errors, ValidationError::Ipv6InvalidCompression);
      return std::nullopt;
    }
    pointer += 2;
    compress = ++pieceIndex;
  }

  while (at(pointer) != -1) {
    if (pieceIndex == 8) {
      Report(errors, ValidationError::Ipv6TooManyPieces);
      return std::nullopt;
    }

    if (at(pointer) == ':') {
      if (compress) {
        Report(errors, ValidationError::Ipv6MultipleCompression);
        return std::nullopt;
      }
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
      if (length == 0) {
        Report(errors, ValidationError::Ipv4InIpv6InvalidCodePoint);
        return std::nullopt;
      }
      pointer -= length;
      if (pieceIndex > 6) {
        Report(errors, ValidationError::Ipv4InIpv6TooManyPieces);
        return std::nullopt;
      }

      size_t numbersSeen = 0;
      while (at(pointer) != -1) {
        std::optional<uint32_t> ipv4Piece;
        if (numbersSeen > 0) {
          if (at(pointer) == '.' && numbersSeen < 4) {
            ++pointer;
          } else {
            Report(errors, ValidationError::Ipv4InIpv6InvalidCodePoint);
            return std::nullopt;
          }
        }
        if (at(pointer) == -1 || !IsAsciiDigit(static_cast<char>(at(pointer)))) {
          Report(errors, ValidationError::Ipv4InIpv6InvalidCodePoint);
          return std::nullopt;
        }

        while (at(pointer) != -1 && IsAsciiDigit(static_cast<char>(at(pointer)))) {
          uint32_t number = at(pointer) - '0';
          if (!ipv4Piece) {
            ipv4Piece = number;
          } else if (*ipv4Piece == 0) {
            Report(errors, ValidationError::Ipv4InIpv6InvalidCodePoint);
            return std::nullopt;
          } else {
            ipv4Piece = *ipv4Piece * 10 + number;
          }
          if (*ipv4Piece > 255) {
            Report(errors, ValidationError::Ipv4InIpv6OutOfRangePart);
            return std::nullopt;
          }
          ++pointer;
        }

        address[pieceIndex] = static_cast<uint16_t>(address[pieceIndex] * 0x100 + *ipv4Piece);
        ++numbersSeen;
        if (numbersSeen == 2 || numbersSeen == 4) ++pieceIndex;
      }
      if (numbersSeen != 4) {
        Report(errors, ValidationError::Ipv4InIpv6TooFewParts);
        return std::nullopt;
      }
      break;
    }

    if (at(pointer) == ':') {
      ++pointer;
      if (at(pointer) == -1) {
        Report(errors, ValidationError::Ipv6InvalidCodePoint);
        return std::nullopt;
      }
    } else if (at(pointer) != -1) {
      Report(errors, ValidationError::Ipv6InvalidCodePoint);
      return std::nullopt;
    }

    if (length > 1 && value < (uint32_t{1} << (4 * (length - 1)))) {
      Report(errors, ValidationError::Ipv6PieceLeadingZero);
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
    Report(errors, ValidationError::Ipv6TooFewPieces);
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

std::optional<std::string> ParseOpaqueHost(std::string_view input, ValidationErrors* errors) {
  for (char c : input) {
    if (IsForbiddenHostCodePoint(c)) {
      Report(errors, ValidationError::HostInvalidCodePoint);
      return std::nullopt;
    }
  }

  if (errors) {
    for (char32_t c : DecodeUtf8(input)) {
      if (c != '%' && !IsUrlCodePoint(c)) {
        Report(errors, ValidationError::InvalidUrlUnit);
        break;
      }
    }
    for (size_t i = input.find('%'); i != std::string_view::npos; i = input.find('%', i + 1)) {
      if (i + 2 >= input.size() || HexValue(input[i + 1]) < 0 || HexValue(input[i + 2]) < 0) {
        Report(errors, ValidationError::InvalidUrlUnit);
        break;
      }
    }
  }

  std::string out;
  AppendPercentEncoded(out, input, EncodeSet::C0Control);
  return out;
}

bool ContainsPercentEncodedByte(std::string_view input) {
  for (size_t i = 0; i + 2 < input.size(); ++i) {
    if (input[i] == '%' && HexValue(input[i + 1]) >= 0 && HexValue(input[i + 2]) >= 0) return true;
  }
  return false;
}

}  // namespace

std::optional<std::string> ParseHost(std::string_view input, bool isOpaque, ValidationErrors* errors) {
  if (!input.empty() && input.front() == '[') {
    if (input.back() != ']') {
      Report(errors, ValidationError::Ipv6Unclosed);
      return std::nullopt;
    }
    std::optional<std::string> ipv6 = ParseIpv6(input.substr(1, input.size() - 2), errors);
    if (!ipv6) return std::nullopt;
    return "[" + *ipv6 + "]";
  }

  if (isOpaque) return ParseOpaqueHost(input, errors);

  if (ContainsPercentEncodedByte(input)) Report(errors, ValidationError::DomainPercentEncoded);
  std::u32string domain = DecodeUtf8(PercentDecode(input));
  std::optional<std::string> asciiDomain = DomainToAscii(domain, errors);
  if (!asciiDomain) return std::nullopt;

  if (EndsInANumber(*asciiDomain)) {
    bool ascii = true;
    for (char32_t c : domain) ascii = ascii && c < 0x80;
    if (!ascii) Report(errors, ValidationError::Ipv4NonAsciiInput);
    return ParseIpv4(*asciiDomain, errors);
  }
  return asciiDomain;
}

}  // namespace solar::url
