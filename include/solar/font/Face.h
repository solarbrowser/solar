#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// A font file read and used (HarfBuzz does the reading and the shaping): its names and style, its vertical metrics, and
// text measured in it.
namespace solar::font {

// Vertical metrics, as fractions of the font size.
struct Metrics {
  double ascent = 0.8;
  double descent = 0.2;
  double lineGap = 0;
  double xHeight = 0.5;
  double capHeight = 0.7;
  double underlinePosition = -0.1;
  double underlineThickness = 0.05;
  double strikeoutPosition = 0.3;
  double strikeoutThickness = 0.05;
  double unitsPerEm = 1000;
  // The height of a line the font asks for: ascent, descent and gap.
  double LineHeight() const { return ascent + descent + lineGap; }
};

// What a face says of itself, for matching it to what a style asks for.
struct Description {
  std::string family;       // the family the font is part of (typographic when it has one)
  std::string legacyFamily; // name id 1: the family a style group of four goes by
  std::string fullName;
  std::string postScriptName;
  double weight = 400;      // 1 to 1000
  double width = 100;       // a percentage: 100 is normal
  bool italic = false;
  double slant = 0;         // degrees, when oblique
  bool variable = false;    // has axes
  double weightMin = 400, weightMax = 400;  // what a variable font spans on its wght axis (else the weight)
  double widthMin = 100, widthMax = 100;
  double slantMin = 0, slantMax = 0;        // slnt axis, in degrees
};

struct ShapeOptions {
  bool rightToLeft = false;
  std::string script;    // an ISO 15924 tag such as "Latn"; empty: guessed from the text
  std::string language;  // BCP 47; empty: none
  bool kerning = true;
  bool ligatures = true;
  struct Feature {
    std::string tag;
    uint32_t value = 1;
  };
  std::vector<Feature> features;
  double letterSpacing = 0;  // added after each glyph, in em
};

struct Glyph {
  uint32_t id = 0;
  double advance = 0;  // along the line, in em
  double xOffset = 0;
  double yOffset = 0;
  uint32_t cluster = 0;  // byte offset in the text of the first character it stands for
};

class Face : public std::enable_shared_from_this<Face> {
 public:
  // `data` is a font file: TrueType, OpenType, a collection (`index` picks the font in it), or WOFF / WOFF2. Nothing if it is not a font.
  static std::shared_ptr<Face> Open(std::string data, unsigned index = 0);
  ~Face();
  Face(const Face&) = delete;
  Face& operator=(const Face&) = delete;

  const Description& description() const { return description_; }
  const Metrics& metrics() const { return metrics_; }
  // How many fonts a collection holds.
  static unsigned CountIn(const std::string& data);

  bool HasGlyph(char32_t codePoint) const;
  uint32_t GlyphFor(char32_t codePoint) const;
  // The advance of the glyph a character is, in em.
  double Advance(char32_t codePoint) const;
  // The text, a UTF-8 string, as the glyphs it comes to.
  std::vector<Glyph> Shape(std::string_view text, const ShapeOptions& options = {}) const;
  // Its width, in em.
  double Measure(std::string_view text, const ShapeOptions& options = {}) const;

  // The outline of a glyph as path commands in em (y up): M x y, L x y, Q x1 y1 x y, C x1 y1 x2 y2 x y, Z.
  std::string Outline(uint32_t glyph) const;
  // The face at the given values of its variation axes (wght, wdth, slnt, ital...); the face itself when it has none to set.
  struct Axis {
    std::string tag;
    double value;
  };
  std::shared_ptr<Face> WithVariations(const std::vector<Axis>& axes) const;
  // A face of a file on disk (mapped, not copied).
  static std::shared_ptr<Face> OpenFile(const std::string& path, unsigned index = 0);

 private:
  Face() = default;
  struct Impl;
  static std::shared_ptr<Face> Finish(std::shared_ptr<Face> face);
  void ReadMetrics();
  std::shared_ptr<const Face> parent_;  // the face a variation is of
  std::string data_;
  Description description_;
  Metrics metrics_;
  std::unique_ptr<Impl> impl_;
};

}  // namespace solar::font
