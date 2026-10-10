#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Text algorithms from the Unicode standard that layout needs: character properties, breaking text into lines (UAX #14), grapheme clusters
// (UAX #29), the bidirectional algorithm (UAX #9) and case mapping. The property tables are generated (tools/gentext.py) from the Unicode
// Character Database.
namespace solar::text {

// ---- Tables ----

struct Range {
  uint32_t first, last;
  uint8_t value;
};
struct Table {
  const Range* ranges;
  size_t count;
  uint8_t Lookup(char32_t c, uint8_t fallback = 0) const;
};
struct Bracket {
  uint32_t code, pair;
  uint8_t type;  // 1 opening, 2 closing
};
struct BracketTable {
  const Bracket* entries;
  size_t count;
  const Bracket* Find(char32_t c) const;
};
struct Mapping {
  uint32_t from, to;
};
struct MappingTable {
  const Mapping* entries;
  size_t count;
  char32_t Find(char32_t c) const;  // 0 if there is none
};
struct Special {
  uint32_t code;
  uint32_t lower[3], title[3], upper[3];  // zero ends
};
struct SpecialTable {
  const Special* entries;
  size_t count;
  const Special* Find(char32_t c) const;
};

const Table& LineBreakTable();
const Table& EastAsianWidthTable();
const Table& ExtendedPictographicTable();
const Table& GraphemeBreakTable();
const Table& IndicConjunctBreakTable();
const Table& WordBreakTable();
const Table& BidiClassTable();
const Table& GeneralCategoryTable();
const BracketTable& Brackets();
const MappingTable& MirrorMap();
const MappingTable& UpperMap();
const MappingTable& LowerMap();
const MappingTable& TitleMap();
const SpecialTable& SpecialCasing();

// ---- Properties ----

enum class LineBreakClass : uint8_t {
  XX, AI, AL, B2, BA, BB, BK, CB, CJ, CL, CM, CP, CR, EB, EM, EX, GL, H2, H3, HL, HY, ID, IN, IS, JL, JT, JV, LF, NL, NS, NU, OP, PO, PR, QU, RI, SA, SG, SP, SY, WJ, ZW, ZWJ,
  AK, AP, AS, VF, VI, HH,
};
enum class EastAsianWidth : uint8_t { N, A, F, H, Na, W };
enum class GraphemeBreak : uint8_t { Other, CR, LF, Control, Extend, ZWJ, RegionalIndicator, Prepend, SpacingMark, L, V, T, LV, LVT };
enum class WordBreak : uint8_t { Other, CR, LF, Newline, Extend, ZWJ, RegionalIndicator, Format, Katakana, HebrewLetter, ALetter, SingleQuote, DoubleQuote, MidNumLet, MidLetter, MidNum, Numeric, ExtendNumLet, WSegSpace };
enum class BidiClass : uint8_t { L, R, AL, EN, ES, ET, AN, CS, NSM, BN, B, S, WS, ON, LRE, LRO, RLE, RLO, PDF, LRI, RLI, FSI, PDI };
enum class GeneralCategory : uint8_t { Cn, Lu, Ll, Lt, Lm, Lo, Mn, Mc, Me, Nd, Nl, No, Pc, Pd, Ps, Pe, Pi, Pf, Po, Sm, Sc, Sk, So, Zs, Zl, Zp, Cc, Cf, Cs, Co };

LineBreakClass LineBreakOf(char32_t c);
EastAsianWidth EastAsianWidthOf(char32_t c);
bool IsExtendedPictographic(char32_t c);
GraphemeBreak GraphemeBreakOf(char32_t c);
WordBreak WordBreakOf(char32_t c);
BidiClass BidiClassOf(char32_t c);
GeneralCategory GeneralCategoryOf(char32_t c);
inline bool IsLetter(char32_t c) { const GeneralCategory g = GeneralCategoryOf(c); return g >= GeneralCategory::Lu && g <= GeneralCategory::Lo; }
inline bool IsMark(char32_t c) { const GeneralCategory g = GeneralCategoryOf(c); return g >= GeneralCategory::Mn && g <= GeneralCategory::Me; }
inline bool IsNumber(char32_t c) { const GeneralCategory g = GeneralCategoryOf(c); return g >= GeneralCategory::Nd && g <= GeneralCategory::No; }
inline bool IsPunctuation(char32_t c) { const GeneralCategory g = GeneralCategoryOf(c); return g >= GeneralCategory::Pc && g <= GeneralCategory::Po; }
inline bool IsSymbol(char32_t c) { const GeneralCategory g = GeneralCategoryOf(c); return g >= GeneralCategory::Sm && g <= GeneralCategory::So; }
char32_t MirrorOf(char32_t c);  // the mirrored character, or the character itself

// ---- UTF-8 ----

// Decodes the code point at `at` and moves it past; U+FFFD for what is not valid.
char32_t DecodeUtf8(std::string_view s, size_t& at);
void AppendUtf8(std::string& out, char32_t c);
std::u32string FromUtf8(std::string_view s);
std::string ToUtf8(std::u32string_view s);

// ---- Case ----

enum class CaseMode { Upper, Lower, Title };
// The text in the case, with the full mappings (ß is SS) and the contextual ones (final sigma; Turkish, Azeri and Lithuanian by language).
// Title makes the first letter of each word title case, the rest as it was.
std::string ChangeCase(std::string_view text, CaseMode mode, std::string_view language = "");

// ---- Grapheme clusters ----

// The byte offset of the end of the grapheme cluster that starts at `at`.
size_t NextGraphemeBoundary(std::string_view text, size_t at);
// The byte offsets where a grapheme cluster ends, one for each.
std::vector<size_t> GraphemeBoundaries(std::string_view text);

// ---- Line breaking ----

struct LineBreakOptions {
  enum class Strictness { Normal, Loose, Strict, Anywhere } strictness = Strictness::Normal;
  enum class Words { Normal, BreakAll, KeepAll } words = Words::Normal;
  bool japaneseOrChinese = false;  // the language: CSS tailors the breaking of Japanese and Chinese text more than of any other
};
// result[offset] is set where a line may end after the character that ends at byte `offset` (the text's own end included).
std::vector<char> FindLineBreaks(std::string_view text, const LineBreakOptions& options = {});

// ---- Bidirectional text ----

struct BidiResult {
  std::vector<uint8_t> levels;       // the embedding level of each code point
  uint8_t paragraphLevel = 0;
};
// Resolves the levels of a paragraph (by code points). `baseLevel` is 0 or 1 to force the direction, 2 to find it from the text (the
// first strong character).
BidiResult ResolveBidi(const std::u32string& text, int baseLevel);
// The visual order of a line given by levels, as indices (UAX #9 rule L2): runs of the same level are reversed as the algorithm says.
std::vector<size_t> VisualOrder(const std::vector<uint8_t>& levels);

}  // namespace solar::text
