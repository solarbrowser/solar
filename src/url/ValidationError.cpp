#include "solar/url/ValidationError.h"

namespace solar::url {

const char* ValidationErrorName(ValidationError error) {
  switch (error) {
    case ValidationError::DomainToAscii: return "domain-to-ASCII";
    case ValidationError::DomainPercentEncoded: return "domain-percent-encoded";
    case ValidationError::HostInvalidCodePoint: return "host-invalid-code-point";
    case ValidationError::Ipv4EmptyPart: return "IPv4-empty-part";
    case ValidationError::Ipv4TooFewParts: return "IPv4-too-few-parts";
    case ValidationError::Ipv4TooManyParts: return "IPv4-too-many-parts";
    case ValidationError::Ipv4NonNumericPart: return "IPv4-non-numeric-part";
    case ValidationError::Ipv4NonDecimalPart: return "IPv4-non-decimal-part";
    case ValidationError::Ipv4OutOfRangePart: return "IPv4-out-of-range-part";
    case ValidationError::Ipv4NonAsciiInput: return "IPv4-non-ASCII-input";
    case ValidationError::Ipv6Unclosed: return "IPv6-unclosed";
    case ValidationError::Ipv6InvalidCompression: return "IPv6-invalid-compression";
    case ValidationError::Ipv6TooManyPieces: return "IPv6-too-many-pieces";
    case ValidationError::Ipv6MultipleCompression: return "IPv6-multiple-compression";
    case ValidationError::Ipv6InvalidCodePoint: return "IPv6-invalid-code-point";
    case ValidationError::Ipv6TooFewPieces: return "IPv6-too-few-pieces";
    case ValidationError::Ipv6PieceLeadingZero: return "IPv6-piece-leading-zero";
    case ValidationError::Ipv4InIpv6TooManyPieces: return "IPv4-in-IPv6-too-many-pieces";
    case ValidationError::Ipv4InIpv6InvalidCodePoint: return "IPv4-in-IPv6-invalid-code-point";
    case ValidationError::Ipv4InIpv6OutOfRangePart: return "IPv4-in-IPv6-out-of-range-part";
    case ValidationError::Ipv4InIpv6TooFewParts: return "IPv4-in-IPv6-too-few-parts";
    case ValidationError::InvalidUrlUnit: return "invalid-URL-unit";
    case ValidationError::SpecialSchemeMissingFollowingSolidus: return "special-scheme-missing-following-solidus";
    case ValidationError::MissingSchemeNonRelativeUrl: return "missing-scheme-non-relative-URL";
    case ValidationError::InvalidReverseSolidus: return "invalid-reverse-solidus";
    case ValidationError::InvalidCredentials: return "invalid-credentials";
    case ValidationError::HostMissing: return "host-missing";
    case ValidationError::PortOutOfRange: return "port-out-of-range";
    case ValidationError::PortInvalid: return "port-invalid";
    case ValidationError::FileInvalidWindowsDriveLetter: return "file-invalid-Windows-drive-letter";
    case ValidationError::FileInvalidWindowsDriveLetterHost: return "file-invalid-Windows-drive-letter-host";
  }
  return "";
}

}  // namespace solar::url
