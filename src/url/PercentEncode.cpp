#include "solar/url/PercentEncode.h"

namespace solar::url {

namespace {

bool InSet(uint8_t byte, EncodeSet set) {
  if (byte <= 0x1F || byte > 0x7E) return true;
  if (set == EncodeSet::C0Control) return false;

  if (set == EncodeSet::Fragment) {
    return byte == ' ' || byte == '"' || byte == '<' || byte == '>' || byte == '`';
  }

  bool inQuery = byte == ' ' || byte == '"' || byte == '#' || byte == '<' || byte == '>';
  if (set == EncodeSet::Query) return inQuery;
  if (set == EncodeSet::SpecialQuery) return inQuery || byte == '\'';

  bool inPath = inQuery || byte == '?' || byte == '^' || byte == '`' || byte == '{' || byte == '}';
  if (set == EncodeSet::Path) return inPath;

  return inPath || byte == '/' || byte == ':' || byte == ';' || byte == '=' || byte == '@' ||
         (byte >= '[' && byte <= ']') || byte == '|';
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

}  // namespace

void AppendPercentEncoded(std::string& out, uint8_t byte, EncodeSet set) {
  if (!InSet(byte, set)) {
    out.push_back(static_cast<char>(byte));
    return;
  }
  static constexpr char kUpperHex[] = "0123456789ABCDEF";
  out.push_back('%');
  out.push_back(kUpperHex[byte >> 4]);
  out.push_back(kUpperHex[byte & 0x0F]);
}

void AppendPercentEncoded(std::string& out, std::string_view bytes, EncodeSet set) {
  for (char byte : bytes) AppendPercentEncoded(out, static_cast<uint8_t>(byte), set);
}

std::string PercentDecode(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '%' && i + 2 < input.size()) {
      int high = HexValue(input[i + 1]);
      int low = HexValue(input[i + 2]);
      if (high >= 0 && low >= 0) {
        out.push_back(static_cast<char>(high * 16 + low));
        i += 2;
        continue;
      }
    }
    out.push_back(input[i]);
  }
  return out;
}

}  // namespace solar::url
