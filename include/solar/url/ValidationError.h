#pragma once

#include <cstdint>
#include <vector>

namespace solar::url {

// The error types of the standard's validation error table. They never stop a parse on
// their own; a parse that fails is told apart by its return value.
enum class ValidationError : uint8_t {
  DomainToAscii,
  DomainPercentEncoded,
  HostInvalidCodePoint,
  Ipv4EmptyPart,
  Ipv4TooFewParts,
  Ipv4TooManyParts,
  Ipv4NonNumericPart,
  Ipv4NonDecimalPart,
  Ipv4OutOfRangePart,
  Ipv4NonAsciiInput,
  Ipv6Unclosed,
  Ipv6InvalidCompression,
  Ipv6TooManyPieces,
  Ipv6MultipleCompression,
  Ipv6InvalidCodePoint,
  Ipv6TooFewPieces,
  Ipv6PieceLeadingZero,
  Ipv4InIpv6TooManyPieces,
  Ipv4InIpv6InvalidCodePoint,
  Ipv4InIpv6OutOfRangePart,
  Ipv4InIpv6TooFewParts,
  InvalidUrlUnit,
  SpecialSchemeMissingFollowingSolidus,
  MissingSchemeNonRelativeUrl,
  InvalidReverseSolidus,
  InvalidCredentials,
  HostMissing,
  PortOutOfRange,
  PortInvalid,
  FileInvalidWindowsDriveLetter,
  FileInvalidWindowsDriveLetterHost,
};

using ValidationErrors = std::vector<ValidationError>;

// The standard's name for the error, such as "IPv4-empty-part".
const char* ValidationErrorName(ValidationError error);

inline void Report(ValidationErrors* errors, ValidationError error) {
  if (errors) errors->push_back(error);
}

}  // namespace solar::url
