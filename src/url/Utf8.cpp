#include "solar/url/Utf8.h"

#include <cstdint>

namespace solar::url {

std::u32string DecodeUtf8(std::string_view bytes) {
  std::u32string out;
  out.reserve(bytes.size());

  char32_t codePoint = 0;
  int needed = 0;
  uint8_t lower = 0x80;
  uint8_t upper = 0xBF;

  for (size_t i = 0; i < bytes.size();) {
    uint8_t byte = static_cast<uint8_t>(bytes[i]);

    if (needed == 0) {
      ++i;
      if (byte <= 0x7F) {
        out.push_back(byte);
      } else if (byte >= 0xC2 && byte <= 0xDF) {
        needed = 1;
        codePoint = byte & 0x1F;
      } else if (byte >= 0xE0 && byte <= 0xEF) {
        if (byte == 0xE0) lower = 0xA0;
        if (byte == 0xED) upper = 0x9F;
        needed = 2;
        codePoint = byte & 0x0F;
      } else if (byte >= 0xF0 && byte <= 0xF4) {
        if (byte == 0xF0) lower = 0x90;
        if (byte == 0xF4) upper = 0x8F;
        needed = 3;
        codePoint = byte & 0x07;
      } else {
        out.push_back(0xFFFD);
      }
      continue;
    }

    if (byte < lower || byte > upper) {
      codePoint = 0;
      needed = 0;
      lower = 0x80;
      upper = 0xBF;
      out.push_back(0xFFFD);
      continue;  // the offending byte starts a new sequence
    }

    lower = 0x80;
    upper = 0xBF;
    codePoint = (codePoint << 6) | (byte & 0x3F);
    ++i;
    if (--needed == 0) {
      out.push_back(codePoint);
      codePoint = 0;
    }
  }

  if (needed != 0) out.push_back(0xFFFD);
  return out;
}

void AppendUtf8(std::string& out, char32_t codePoint) {
  if (codePoint <= 0x7F) {
    out.push_back(static_cast<char>(codePoint));
  } else if (codePoint <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
    out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
  } else if (codePoint <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
    out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
    out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
  }
}

}  // namespace solar::url
