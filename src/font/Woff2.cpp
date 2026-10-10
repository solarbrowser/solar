#include <brotli/decode.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "solar/font/Face.h"

// WOFF2 (https://www.w3.org/TR/WOFF2/): the tables Brotli-compressed together, glyf, loca and hmtx perhaps stored in a form that
// compresses better. What comes out is the sfnt the file was made from.
namespace solar::font {

namespace {

constexpr const char* kKnownTags[63] = {"cmap", "head", "hhea", "hmtx", "maxp", "name", "OS/2", "post", "cvt ", "fpgm", "glyf", "loca", "prep",
                                        "CFF ", "VORG", "EBDT", "EBLC", "gasp", "hdmx", "kern", "LTSH", "PCLT", "VDMX", "vhea", "vmtx", "BASE",
                                        "GDEF", "GPOS", "GSUB", "EBSC", "JSTF", "MATH", "CBDT", "CBLC", "COLR", "CPAL", "SVG ", "sbix", "acnt",
                                        "avar", "bdat", "bloc", "bsln", "cvar", "fdsc", "feat", "fmtx", "fvar", "gvar", "hsty", "just", "lcar",
                                        "mort", "morx", "opbd", "prop", "trak", "Zapf", "Silf", "Glat", "Gloc", "Feat", "Sill"};

struct Reader {
  const uint8_t* data;
  size_t size;
  size_t at = 0;
  bool bad = false;

  bool Has(size_t n) {
    if (at + n > size) bad = true;
    return !bad;
  }
  uint8_t U8() { return Has(1) ? data[at++] : 0; }
  uint16_t U16() {
    if (!Has(2)) return 0;
    const uint16_t v = static_cast<uint16_t>((data[at] << 8) | data[at + 1]);
    at += 2;
    return v;
  }
  uint32_t U32() {
    if (!Has(4)) return 0;
    const uint32_t v = (uint32_t(data[at]) << 24) | (uint32_t(data[at + 1]) << 16) | (uint32_t(data[at + 2]) << 8) | data[at + 3];
    at += 4;
    return v;
  }
  uint32_t Base128() {
    uint32_t value = 0;
    for (int i = 0; i < 5; ++i) {
      const uint8_t byte = U8();
      if (bad) return 0;
      if (i == 0 && byte == 0x80) {
        bad = true;
        return 0;
      }
      if (value & 0xfe000000u) {
        bad = true;
        return 0;
      }
      value = (value << 7) | (byte & 0x7f);
      if (!(byte & 0x80)) return value;
    }
    bad = true;
    return 0;
  }
  uint16_t Short255() {
    const uint8_t code = U8();
    if (code == 253) return U16();
    if (code == 255) return static_cast<uint16_t>(253 + U8());
    if (code == 254) return static_cast<uint16_t>(506 + U8());
    return code;
  }
  Reader Slice(size_t n) {
    if (!Has(n)) return {data, 0, 0, true};
    Reader r{data + at, n, 0, false};
    at += n;
    return r;
  }
};

void Put16(std::string& out, uint32_t v) {
  out.push_back(static_cast<char>(v >> 8));
  out.push_back(static_cast<char>(v));
}
void Put32(std::string& out, uint32_t v) {
  Put16(out, v >> 16);
  Put16(out, v & 0xffff);
}
int Signed(int flag, int value) { return (flag & 1) ? value : -value; }

struct Point {
  int x, y;
  bool onCurve;
};

bool Triplets(Reader& flags, Reader& data, size_t count, std::vector<Point>& points) {
  int x = 0, y = 0;
  for (size_t i = 0; i < count; ++i) {
    int flag = flags.U8();
    const bool on = !(flag >> 7);
    flag &= 0x7f;
    int dx, dy;
    if (flag < 10) {
      dx = 0;
      dy = Signed(flag, ((flag & 14) << 8) + data.U8());
    } else if (flag < 20) {
      dx = Signed(flag, (((flag - 10) & 14) << 8) + data.U8());
      dy = 0;
    } else if (flag < 84) {
      const int b0 = flag - 20, b1 = data.U8();
      dx = Signed(flag, 1 + (b0 & 0x30) + (b1 >> 4));
      dy = Signed(flag >> 1, 1 + ((b0 & 0x0c) << 2) + (b1 & 0x0f));
    } else if (flag < 120) {
      const int b0 = flag - 84, b1 = data.U8(), b2 = data.U8();
      dx = Signed(flag, 1 + ((b0 / 12) << 8) + b1);
      dy = Signed(flag >> 1, 1 + (((b0 % 12) >> 2) << 8) + b2);
    } else if (flag < 124) {
      const int b1 = data.U8(), b2 = data.U8(), b3 = data.U8();
      dx = Signed(flag, (b1 << 4) + (b2 >> 4));
      dy = Signed(flag >> 1, ((b2 & 0x0f) << 8) + b3);
    } else {
      const int b1 = data.U8(), b2 = data.U8(), b3 = data.U8(), b4 = data.U8();
      dx = Signed(flag, (b1 << 8) + b2);
      dy = Signed(flag >> 1, (b3 << 8) + b4);
    }
    if (flags.bad || data.bad) return false;
    x += dx;
    y += dy;
    points.push_back({x, y, on});
  }
  return true;
}

// The glyf and loca tables from the transformed glyf; `xMins` get each glyph's left edge for hmtx.
bool ReconstructGlyf(const std::string& transformed, std::string& glyf, std::string& loca, std::vector<int16_t>& xMins) {
  Reader in{reinterpret_cast<const uint8_t*>(transformed.data()), transformed.size()};
  in.U16();
  const uint16_t options = in.U16();
  const uint16_t glyphs = in.U16();
  const uint16_t indexFormat = in.U16();
  uint32_t sizes[7];
  for (uint32_t& size : sizes) size = in.U32();
  if (in.bad) return false;
  Reader contours = in.Slice(sizes[0]), pointCounts = in.Slice(sizes[1]), flags = in.Slice(sizes[2]), glyphData = in.Slice(sizes[3]);
  Reader composites = in.Slice(sizes[4]), boxes = in.Slice(sizes[5]), instructions = in.Slice(sizes[6]);
  if (in.bad) return false;
  Reader overlaps{nullptr, 0, 0, false};
  if (options & 1) overlaps = in.Slice((glyphs + 7) / 8);
  const size_t bitmapSize = ((glyphs + 31) >> 5) << 2;
  Reader bitmap = boxes.Slice(bitmapSize);
  if (boxes.bad) return false;

  std::vector<uint32_t> offsets;
  xMins.assign(glyphs, 0);
  const size_t pad = indexFormat == 0 ? 2 : 4;
  for (uint16_t g = 0; g < glyphs; ++g) {
    offsets.push_back(static_cast<uint32_t>(glyf.size()));
    const int16_t count = static_cast<int16_t>(contours.U16());
    if (contours.bad) return false;
    if (count == 0) continue;
    const bool explicitBox = bitmap.data[g >> 3] & (0x80 >> (g & 7));
    std::string glyph;
    int16_t box[4] = {0, 0, 0, 0};
    if (count > 0) {
      std::vector<uint16_t> ends;
      size_t total = 0;
      for (int c = 0; c < count; ++c) {
        total += pointCounts.Short255();
        ends.push_back(static_cast<uint16_t>(total - 1));
      }
      if (pointCounts.bad || total > 0xffff) return false;
      std::vector<Point> points;
      if (!Triplets(flags, glyphData, total, points)) return false;
      const uint16_t instructionLength = glyphData.Short255();
      Reader code = instructions.Slice(instructionLength);
      if (glyphData.bad || code.bad) return false;
      if (explicitBox) {
        for (int16_t& v : box) v = static_cast<int16_t>(boxes.U16());
      } else if (!points.empty()) {
        int xMin = points[0].x, xMax = xMin, yMin = points[0].y, yMax = yMin;
        for (const Point& p : points) {
          xMin = std::min(xMin, p.x);
          xMax = std::max(xMax, p.x);
          yMin = std::min(yMin, p.y);
          yMax = std::max(yMax, p.y);
        }
        box[0] = static_cast<int16_t>(xMin);
        box[1] = static_cast<int16_t>(yMin);
        box[2] = static_cast<int16_t>(xMax);
        box[3] = static_cast<int16_t>(yMax);
      }
      Put16(glyph, static_cast<uint16_t>(count));
      for (int16_t v : box) Put16(glyph, static_cast<uint16_t>(v));
      for (uint16_t end : ends) Put16(glyph, end);
      Put16(glyph, instructionLength);
      glyph.append(reinterpret_cast<const char*>(code.data), instructionLength);
      const bool overlap = options & 1 && (overlaps.data[g >> 3] & (0x80 >> (g & 7)));
      // Every point's flag, then x deltas and y deltas, all in full words.
      for (size_t i = 0; i < points.size(); ++i) glyph.push_back(static_cast<char>((points[i].onCurve ? 1 : 0) | (i == 0 && overlap ? 0x40 : 0)));
      int previous = 0;
      for (const Point& p : points) {
        Put16(glyph, static_cast<uint16_t>(p.x - previous));
        previous = p.x;
      }
      previous = 0;
      for (const Point& p : points) {
        Put16(glyph, static_cast<uint16_t>(p.y - previous));
        previous = p.y;
      }
    } else {
      // A composite: its components are copied; the box is given.
      if (!explicitBox) return false;
      for (int16_t& v : box) v = static_cast<int16_t>(boxes.U16());
      Put16(glyph, 0xffff);
      for (int16_t v : box) Put16(glyph, static_cast<uint16_t>(v));
      bool more = true, hasInstructions = false;
      while (more) {
        const uint16_t flag = composites.U16();
        const uint16_t index = composites.U16();
        Put16(glyph, flag);
        Put16(glyph, index);
        size_t extra = (flag & 1) ? 4 : 2;
        if (flag & 0x8) extra += 2;
        else if (flag & 0x40) extra += 4;
        else if (flag & 0x80) extra += 8;
        if (composites.bad || !composites.Has(extra)) return false;
        glyph.append(reinterpret_cast<const char*>(composites.data + composites.at), extra);
        composites.at += extra;
        more = flag & 0x20;
        if (flag & 0x100) hasInstructions = true;
      }
      if (hasInstructions) {
        const uint16_t length = glyphData.Short255();
        Reader code = instructions.Slice(length);
        if (glyphData.bad || code.bad) return false;
        Put16(glyph, length);
        glyph.append(reinterpret_cast<const char*>(code.data), length);
      }
    }
    xMins[g] = box[0];
    glyf += glyph;
    glyf.append((pad - glyf.size() % pad) % pad, '\0');
  }
  offsets.push_back(static_cast<uint32_t>(glyf.size()));
  for (uint32_t offset : offsets) {
    if (indexFormat == 0) {
      if (offset / 2 > 0xffff) return false;
      Put16(loca, offset / 2);
    } else {
      Put32(loca, offset);
    }
  }
  return true;
}

// hmtx with the left side bearings that follow from the glyphs left out.
bool ReconstructHmtx(const std::string& transformed, uint16_t glyphs, uint16_t metrics, const std::vector<int16_t>& xMins, std::string& hmtx) {
  Reader in{reinterpret_cast<const uint8_t*>(transformed.data()), transformed.size()};
  const uint8_t flags = in.U8();
  if (metrics == 0 || metrics > glyphs) return false;
  std::vector<uint16_t> advances(metrics);
  for (uint16_t& a : advances) a = in.U16();
  std::vector<int16_t> bearings(glyphs);
  for (uint16_t i = 0; i < metrics; ++i) bearings[i] = (flags & 1) ? xMins[i] : static_cast<int16_t>(in.U16());
  for (uint16_t i = metrics; i < glyphs; ++i) bearings[i] = (flags & 2) ? xMins[i] : static_cast<int16_t>(in.U16());
  if (in.bad) return false;
  for (uint16_t i = 0; i < glyphs; ++i) {
    if (i < metrics) Put16(hmtx, advances[i]);
    Put16(hmtx, static_cast<uint16_t>(bearings[i]));
  }
  return true;
}

uint32_t Checksum(const std::string& table) {
  uint32_t sum = 0;
  for (size_t i = 0; i < table.size(); i += 4) {
    uint32_t word = 0;
    for (size_t j = 0; j < 4; ++j) word = (word << 8) | (i + j < table.size() ? uint8_t(table[i + j]) : 0);
    sum += word;
  }
  return sum;
}

}  // namespace

std::string UnwrapWoff2(const std::string& woff) {
  Reader in{reinterpret_cast<const uint8_t*>(woff.data()), woff.size()};
  if (woff.size() < 48 || woff.compare(0, 4, "wOF2") != 0) return "";
  in.at = 4;
  const uint32_t flavor = in.U32();
  if (flavor == 0x74746366) return "";  // a collection
  in.U32();
  const uint16_t tableCount = in.U16();
  in.U16();
  in.U32();
  const uint32_t compressedSize = in.U32();
  in.at = 48;
  struct Table {
    std::string tag;
    bool transformed = false;
    uint32_t original = 0, stored = 0;
  };
  std::vector<Table> tables;
  size_t streamSize = 0;
  for (uint16_t i = 0; i < tableCount; ++i) {
    Table t;
    const uint8_t flags = in.U8();
    if ((flags & 0x3f) == 0x3f) {
      t.tag.assign(4, ' ');
      for (char& c : t.tag) c = static_cast<char>(in.U8());
    } else {
      t.tag = kKnownTags[flags & 0x3f];
    }
    const int version = flags >> 6;
    t.original = in.Base128();
    t.transformed = (t.tag == "glyf" || t.tag == "loca") ? version != 3 : version != 0;
    t.stored = t.original;
    if (t.transformed) t.stored = (t.tag == "loca") ? (in.Base128(), 0) : in.Base128();
    if (in.bad) return "";
    streamSize += t.stored;
    tables.push_back(std::move(t));
  }
  if (!in.Has(compressedSize)) return "";
  std::string stream(streamSize, '\0');
  size_t decoded = streamSize;
  if (BrotliDecoderDecompress(compressedSize, in.data + in.at, &decoded, reinterpret_cast<uint8_t*>(stream.data())) != BROTLI_DECODER_RESULT_SUCCESS || decoded != streamSize) return "";

  std::vector<std::string> bodies(tables.size());
  size_t at = 0;
  for (size_t i = 0; i < tables.size(); ++i) {
    bodies[i] = stream.substr(at, tables[i].stored);
    at += tables[i].stored;
  }
  const auto find = [&](const char* tag) -> int {
    for (size_t i = 0; i < tables.size(); ++i) if (tables[i].tag == tag) return static_cast<int>(i);
    return -1;
  };
  std::vector<int16_t> xMins;
  const int glyfIndex = find("glyf"), locaIndex = find("loca"), hmtxIndex = find("hmtx");
  if (glyfIndex >= 0 && tables[glyfIndex].transformed) {
    std::string glyf, loca;
    if (!ReconstructGlyf(bodies[glyfIndex], glyf, loca, xMins)) return "";
    bodies[glyfIndex] = std::move(glyf);
    if (locaIndex >= 0) bodies[locaIndex] = std::move(loca);
  }
  if (hmtxIndex >= 0 && tables[hmtxIndex].transformed) {
    const int head = find("hhea"), maxp = find("maxp");
    if (head < 0 || maxp < 0 || xMins.empty() || bodies[head].size() < 36 || bodies[maxp].size() < 6) return "";
    const uint16_t metrics = static_cast<uint16_t>((uint8_t(bodies[head][34]) << 8) | uint8_t(bodies[head][35]));
    const uint16_t glyphs = static_cast<uint16_t>((uint8_t(bodies[maxp][4]) << 8) | uint8_t(bodies[maxp][5]));
    std::string hmtx;
    if (!ReconstructHmtx(bodies[hmtxIndex], glyphs, metrics, xMins, hmtx)) return "";
    bodies[hmtxIndex] = std::move(hmtx);
  }

  // The sfnt: its tables in tag order.
  std::vector<size_t> order(tables.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return tables[a].tag < tables[b].tag; });
  std::string out;
  Put32(out, flavor);
  Put16(out, tableCount);
  uint16_t searchRange = 1, entrySelector = 0;
  while (searchRange * 2 <= tableCount) {
    searchRange = static_cast<uint16_t>(searchRange * 2);
    ++entrySelector;
  }
  Put16(out, static_cast<uint16_t>(searchRange * 16));
  Put16(out, entrySelector);
  Put16(out, static_cast<uint16_t>(tableCount * 16 - searchRange * 16));
  size_t offset = 12 + size_t(tableCount) * 16;
  for (size_t i : order) {
    for (char c : tables[i].tag) out.push_back(c);
    Put32(out, Checksum(bodies[i]));
    Put32(out, static_cast<uint32_t>(offset));
    Put32(out, static_cast<uint32_t>(bodies[i].size()));
    offset += (bodies[i].size() + 3) & ~size_t(3);
  }
  for (size_t i : order) {
    out += bodies[i];
    out.append((4 - bodies[i].size() % 4) % 4, '\0');
  }
  return out;
}

}  // namespace solar::font
