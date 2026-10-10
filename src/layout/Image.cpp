// The size of an image, read from its header: layout needs how big a picture is, not what is in it.
#include <cmath>
#include <cstdlib>

#include "Internal.h"
#include "solar/css/Cssom.h"
#include "solar/url/Parser.h"
#include "solar/url/Serializer.h"

namespace solar::layout {

namespace {

uint32_t Be32(const std::string& d, size_t at) {
  return (uint32_t(uint8_t(d[at])) << 24) | (uint32_t(uint8_t(d[at + 1])) << 16) | (uint32_t(uint8_t(d[at + 2])) << 8) | uint8_t(d[at + 3]);
}
uint32_t Le16(const std::string& d, size_t at) { return uint8_t(d[at]) | (uint32_t(uint8_t(d[at + 1])) << 8); }
uint32_t Le32(const std::string& d, size_t at) { return Le16(d, at) | (Le16(d, at + 2) << 16); }

bool StartsWith(const std::string& d, const char* s) { return d.compare(0, std::string(s).size(), s) == 0; }

// The value of an attribute in the text of a tag, or empty.
std::string AttributeOf(const std::string& tag, const std::string& name) {
  size_t at = 0;
  while ((at = tag.find(name, at)) != std::string::npos) {
    const bool startOk = at == 0 || std::isspace(static_cast<unsigned char>(tag[at - 1]));
    size_t e = at + name.size();
    while (e < tag.size() && std::isspace(static_cast<unsigned char>(tag[e]))) ++e;
    if (startOk && e < tag.size() && tag[e] == '=') {
      ++e;
      while (e < tag.size() && std::isspace(static_cast<unsigned char>(tag[e]))) ++e;
      if (e < tag.size() && (tag[e] == '"' || tag[e] == '\'')) {
        const char q = tag[e++];
        const size_t end = tag.find(q, e);
        return tag.substr(e, end == std::string::npos ? std::string::npos : end - e);
      }
    }
    at += name.size();
  }
  return "";
}

bool LengthPx(const std::string& text, double& out) {
  if (text.empty()) return false;
  char* end = nullptr;
  const double v = std::strtod(text.c_str(), &end);
  if (end == text.c_str()) return false;
  const std::string unit = end;
  if (unit.empty() || unit == "px") { out = v; return true; }
  if (unit == "pt") { out = v * 4 / 3; return true; }
  if (unit == "in") { out = v * 96; return true; }
  if (unit == "cm") { out = v * 96 / 2.54; return true; }
  if (unit == "mm") { out = v * 96 / 25.4; return true; }
  if (unit == "pc") { out = v * 16; return true; }
  return false;  // %, em...
}

}  // namespace

// Fills what the image says of its size: width and height where it gives them (negative where not) and the ratio (0 where none).
bool ReadImageMetrics(const std::string& d, double& w, double& h, double& ratio) {
  w = h = -1;
  ratio = 0;
  if (d.size() >= 24 && StartsWith(d, "\x89PNG")) {
    w = Be32(d, 16);
    h = Be32(d, 20);
  } else if (d.size() >= 10 && (StartsWith(d, "GIF87a") || StartsWith(d, "GIF89a"))) {
    w = Le16(d, 6);
    h = Le16(d, 8);
  } else if (d.size() >= 4 && uint8_t(d[0]) == 0xFF && uint8_t(d[1]) == 0xD8) {
    size_t i = 2;
    while (i + 9 < d.size()) {
      if (uint8_t(d[i]) != 0xFF) { ++i; continue; }
      const uint8_t marker = uint8_t(d[i + 1]);
      if (marker == 0xFF) { ++i; continue; }
      if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) { i += 2; continue; }
      const size_t length = (uint8_t(d[i + 2]) << 8) | uint8_t(d[i + 3]);
      if ((marker >= 0xC0 && marker <= 0xCF) && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
        h = (uint8_t(d[i + 5]) << 8) | uint8_t(d[i + 6]);
        w = (uint8_t(d[i + 7]) << 8) | uint8_t(d[i + 8]);
        break;
      }
      i += 2 + length;
    }
  } else if (d.size() >= 30 && StartsWith(d, "RIFF") && d.compare(8, 4, "WEBP") == 0) {
    if (d.compare(12, 4, "VP8 ") == 0) {
      w = Le16(d, 26) & 0x3fff;
      h = Le16(d, 28) & 0x3fff;
    } else if (d.compare(12, 4, "VP8L") == 0) {
      const uint32_t bits = Le32(d, 21);
      w = (bits & 0x3fff) + 1;
      h = ((bits >> 14) & 0x3fff) + 1;
    } else if (d.compare(12, 4, "VP8X") == 0) {
      w = (uint8_t(d[24]) | (uint8_t(d[25]) << 8) | (uint8_t(d[26]) << 16)) + 1;
      h = (uint8_t(d[27]) | (uint8_t(d[28]) << 8) | (uint8_t(d[29]) << 16)) + 1;
    }
  } else if (d.size() >= 26 && StartsWith(d, "BM")) {
    w = static_cast<int32_t>(Le32(d, 18));
    h = std::abs(static_cast<int32_t>(Le32(d, 22)));
  } else if (d.size() >= 6 && d[0] == 0 && d[1] == 0 && d[2] == 1 && d[3] == 0) {
    // ICO: the largest image
    const size_t count = Le16(d, 4);
    for (size_t k = 0; k < count && 6 + k * 16 + 16 <= d.size(); ++k) {
      double iw = uint8_t(d[6 + k * 16]), ih = uint8_t(d[7 + k * 16]);
      if (iw == 0) iw = 256;
      if (ih == 0) ih = 256;
      if (iw * ih > std::max(0.0, w) * std::max(0.0, h)) { w = iw; h = ih; }
    }
  } else {
    // SVG
    const size_t at = d.find("<svg");
    if (at == std::string::npos) return false;
    const size_t end = d.find('>', at);
    const std::string tag = d.substr(at, end == std::string::npos ? std::string::npos : end - at);
    double sw = -1, sh = -1;
    LengthPx(AttributeOf(tag, "width"), sw);
    LengthPx(AttributeOf(tag, "height"), sh);
    const std::string box = AttributeOf(tag, "viewBox");
    double vb[4] = {0, 0, 0, 0};
    bool haveBox = false;
    if (!box.empty()) {
      const char* p = box.c_str();
      int n = 0;
      while (n < 4) {
        char* e = nullptr;
        while (*p == ' ' || *p == ',') ++p;
        vb[n] = std::strtod(p, &e);
        if (e == p) break;
        p = e;
        ++n;
      }
      haveBox = n == 4 && vb[2] > 0 && vb[3] > 0;
    }
    w = sw;
    h = sh;
    if (haveBox) ratio = vb[2] / vb[3];
    if (w >= 0 && h > 0 && !haveBox) ratio = w / h;
    if (w < 0 && h < 0 && haveBox) { /* only the ratio */ }
    return true;
  }
  if (w <= 0 || h <= 0) { w = h = -1; return false; }
  ratio = w / h;
  return true;
}

// The picture at an address of the document, if it can be had: the loader the style sheets use brings it.
bool ImageMetricsOf(const std::string& address, const std::string& base, double& w, double& h, double& ratio) {
  static std::unordered_map<std::string, std::array<double, 3>> cache;
  std::string absolute = address;
  const std::optional<url::Url> baseUrl = url::Parse(base);
  const std::optional<url::Url> parsed = url::Parse(address, baseUrl ? &*baseUrl : nullptr);
  if (!parsed) return false;
  absolute = url::Serialize(*parsed);
  const auto found = cache.find(absolute);
  if (found != cache.end()) {
    w = found->second[0];
    h = found->second[1];
    ratio = found->second[2];
    return w >= 0 || ratio > 0;
  }
  std::string data;
  if (absolute.rfind("data:", 0) == 0) {
    const size_t comma = absolute.find(',');
    if (comma == std::string::npos) return false;
    const std::string meta = absolute.substr(5, comma - 5);
    const std::string body = absolute.substr(comma + 1);
    if (meta.size() >= 7 && meta.compare(meta.size() - 7, 7, ";base64") == 0) {
      int bits = 0, value = 0;
      for (char c : body) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else continue;
        value = (value << 6) | v;
        bits += 6;
        if (bits >= 8) { bits -= 8; data += static_cast<char>((value >> bits) & 0xff); }
      }
    } else {
      for (size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '%' && i + 2 < body.size()) { data += static_cast<char>(std::strtol(body.substr(i + 1, 2).c_str(), nullptr, 16)); i += 2; }
        else data += body[i];
      }
    }
  } else {
    const css::SheetLoader& load = css::GetSheetLoader();
    if (!load) return false;
    std::optional<std::string> loaded = load(absolute);
    if (!loaded) { cache[absolute] = {-1, -1, 0}; return false; }
    data = std::move(*loaded);
  }
  const bool ok = ReadImageMetrics(data, w, h, ratio);
  cache[absolute] = {w, h, ratio};
  return ok;
}

}  // namespace solar::layout
