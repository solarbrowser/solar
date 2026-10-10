// Inline layout (https://www.w3.org/TR/CSS22/visuren.html#inline-formatting, https://www.w3.org/TR/css-text-3/): the text and inline boxes of a
// block container, broken into lines and placed on them.
#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

#include "Internal.h"
#include "solar/css/Fonts.h"
#include "solar/css/Tokenizer.h"
#include "solar/text/Unicode.h"

namespace solar::layout {

namespace {

constexpr double kEpsilon = 0.0005;

// ---- Text ----

char32_t DecodeAt(const std::string& s, size_t& i) {
  const unsigned char c = static_cast<unsigned char>(s[i]);
  char32_t cp;
  int extra;
  if (c < 0x80) { cp = c; extra = 0; }
  else if ((c >> 5) == 6) { cp = c & 0x1f; extra = 1; }
  else if ((c >> 4) == 14) { cp = c & 0x0f; extra = 2; }
  else if ((c >> 3) == 30) { cp = c & 0x07; extra = 3; }
  else { ++i; return 0xFFFD; }
  ++i;
  for (int k = 0; k < extra && i < s.size(); ++k, ++i) cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3f);
  return cp;
}

void AppendCp(std::string& out, char32_t c) { solar::css::AppendUtf8(out, c); }

bool IsCjk(char32_t c) {
  return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0x20000 && c <= 0x3FFFF) ||
         (c >= 0x3000 && c <= 0x30FF);
}
bool IsClosingPunct(char32_t c) {
  switch (c) {
    case 0x3001: case 0x3002: case 0xFF0C: case 0xFF0E: case 0xFF1A: case 0xFF1B: case 0xFF01: case 0xFF1F: case 0xFF09: case 0x300D: case 0x300F: case 0x3011:
    case 0x3009: case 0x300B: case 0x30FC: case 0x3005: return true;
    default: return false;
  }
}
bool IsOpeningPunct(char32_t c) {
  switch (c) { case 0xFF08: case 0x300C: case 0x300E: case 0x3010: case 0x3008: case 0x300A: return true; default: return false; }
}
bool IsCombining(char32_t c) {
  return (c >= 0x300 && c <= 0x36F) || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x1DC0 && c <= 0x1DFF) || (c >= 0x20D0 && c <= 0x20FF) || (c >= 0xFE00 && c <= 0xFE0F) ||
         (c >= 0xFE20 && c <= 0xFE2F) || c == 0x200D || (c >= 0x0900 && c <= 0x0903) || (c >= 0x093A && c <= 0x094F);
}
bool IsAlnum(char32_t c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0xC0; }
bool IsSpaceChar(char32_t c) { return c == ' ' || c == '\t' || c == 0x1680 || (c >= 0x2000 && c <= 0x2006) || (c >= 0x2008 && c <= 0x200A) || c == 0x205F || c == 0x3000; }

char32_t ToUpper(char32_t c) {
  if (c >= 'a' && c <= 'z') return c - 32;
  if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) return c - 32;
  if (c >= 0x100 && c <= 0x17F && (c & 1) && c != 0x131 && c != 0x138 && c != 0x149) return c - 1;
  if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;
  if (c >= 0x430 && c <= 0x44F) return c - 32;
  if (c >= 0x450 && c <= 0x45F) return c - 80;
  return c;
}
char32_t ToLower(char32_t c) {
  if (c >= 'A' && c <= 'Z') return c + 32;
  if ((c >= 0xC0 && c <= 0xDE && c != 0xD7)) return c + 32;
  if (c >= 0x100 && c <= 0x17F && !(c & 1) && c != 0x130 && c != 0x138) return c + 1;
  if (c >= 0x391 && c <= 0x3A9) return c + 32;
  if (c >= 0x410 && c <= 0x42F) return c + 32;
  if (c >= 0x400 && c <= 0x40F) return c + 80;
  return c;
}

std::string Transform(const std::string& text, TextTransform transform, const std::string& language) {
  switch (transform) {
    case TextTransform::Uppercase: return text::ChangeCase(text, text::CaseMode::Upper, language);
    case TextTransform::Lowercase: return text::ChangeCase(text, text::CaseMode::Lower, language);
    case TextTransform::Capitalize: return text::ChangeCase(text, text::CaseMode::Title, language);
    default: return text;
  }
}

// ---- Fonts ----

struct FontSet {
  std::vector<std::shared_ptr<font::Face>> faces;
};

struct FontKey {
  std::string family;
  int weight;
  bool italic;
  int stretch;
  bool operator<(const FontKey& o) const { return std::tie(family, weight, italic, stretch) < std::tie(o.family, o.weight, o.italic, o.stretch); }
};

struct Metrics {
  double ascent = 0, descent = 0, lineGap = 0, xHeight = 0;
};

struct FontCache {
  std::map<FontKey, FontSet> sets;
  std::map<std::pair<const font::Face*, int>, Metrics> metrics;
  dom::Document* document = nullptr;
  uint64_t version = 0;
};

FontCache& Cache() {
  static FontCache cache;
  return cache;
}

const FontSet& FontsOf(LayoutContext& lc, const BoxStyle& style) {
  FontCache& cache = Cache();
  dom::Document* document = lc.tree.root ? static_cast<dom::Document*>(lc.tree.root->node) : nullptr;
  const uint64_t version = dom::TreeVersion();
  if (cache.document != document || cache.version != version) {
    cache.sets.clear();
    cache.document = document;
    cache.version = version;
  }
  const FontKey key{style.fontFamily, style.fontWeight, style.italic, static_cast<int>(style.fontStretch)};
  auto found = cache.sets.find(key);
  if (found != cache.sets.end()) return found->second;
  FontSet set;
  set.faces = solar::css::FontsForRequest(document, solar::css::MakeFontRequest(style.fontFamily, style.fontWeight, style.fontStretch, style.italic));
  return cache.sets.emplace(key, std::move(set)).first->second;
}

// The metrics of the primary font at the style's size, with ascent and descent rounded as the engines that sized the tests do.
Metrics MetricsFor(const font::Face* face, double size) {
  Metrics m;
  if (!face) {
    m.ascent = std::round(size * 0.8);
    m.descent = std::round(size * 0.2);
    m.xHeight = size * 0.5;
    return m;
  }
  const font::Metrics& fm = face->metrics();
  m.ascent = std::round(fm.ascent * size);
  m.descent = std::round(fm.descent * size);
  m.lineGap = fm.lineGap * size;
  m.xHeight = fm.xHeight * size;
  return m;
}

// ---- Items ----

struct Run {
  size_t begin, end;  // byte range in the text box's processed text
  std::shared_ptr<font::Face> face;
  std::vector<font::Glyph> glyphs;  // clusters relative to the processed text
};

struct TextData {
  Box* box = nullptr;
  std::vector<Run> runs;
  double size = 16;
  double letterSpacing = 0, wordSpacing = 0;
};

// How wide [b, e) of the box's text is.
double WidthOf(const TextData& data, size_t b, size_t e) {
  double width = 0;
  for (const Run& run : data.runs) {
    if (run.end <= b || run.begin >= e) continue;
    for (const font::Glyph& g : run.glyphs) {
      if (g.cluster >= b && g.cluster < e) width += g.advance * data.size + data.letterSpacing;
    }
  }
  return width;
}

struct Atom {
  enum class Kind { Text, Open, Close, Atomic, Break, OutOfFlow } kind = Kind::Text;
  Box* box = nullptr;
  TextData* data = nullptr;
  size_t begin = 0, end = 0;
  double width = 0;           // all of it
  double trailing = 0;        // the spaces at its end that go when it ends a line
  size_t trailingBytes = 0;
  bool breakAfter = false;    // a line may end after it
  bool forced = false;        // a line must end after it
  double leading = 0;         // (Open/Close) the edges
};

// What is known about a position in the text for breaking.
struct IfcText {
  std::string all;
  std::vector<char> breakAfter;  // by byte offset of the end of a character: a line may end there
};

struct Collector {
  LayoutContext& lc;
  Box& container;
  std::vector<std::unique_ptr<TextData>> texts;
  std::vector<Atom> atoms;
  // processing state
  bool lastWasSpace = true;
  std::string all;                 // the concatenation, for break opportunities
  struct Mark { size_t atomIndex; size_t offset; };
  std::vector<size_t> atomStart;   // offset in `all` of each atom's start
  std::vector<Box*> floatsAndOthers;

  Collector(LayoutContext& l, Box& c) : lc(l), container(c) {}

  // The text after white space processing.
  std::string Process(Box& text) {
    const BoxStyle& s = *text.style;
    std::string in = Transform(text.text, s.textTransform, s.lang);
    std::string out;
    switch (s.whiteSpaceCollapse) {
      case WhiteSpaceCollapse::Collapse:
        for (size_t i = 0; i < in.size();) {
          const char c = in[i];
          if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
            if (!lastWasSpace) out += ' ';
            lastWasSpace = true;
            ++i;
          } else {
            size_t j = i;
            const char32_t cp = DecodeAt(in, j);
            (void)cp;
            out.append(in, i, j - i);
            i = j;
            lastWasSpace = false;
          }
        }
        break;
      case WhiteSpaceCollapse::PreserveBreaks:
        for (size_t i = 0; i < in.size();) {
          const char c = in[i];
          if (c == '\n') {
            while (!out.empty() && out.back() == ' ') out.pop_back();
            out += '\n';
            lastWasSpace = true;
            ++i;
          } else if (c == ' ' || c == '\t' || c == '\r' || c == '\f') {
            if (!lastWasSpace) out += ' ';
            lastWasSpace = true;
            ++i;
          } else {
            size_t j = i;
            DecodeAt(in, j);
            out.append(in, i, j - i);
            i = j;
            lastWasSpace = false;
          }
        }
        break;
      case WhiteSpaceCollapse::PreserveSpaces:
        for (char c : in) out += (c == '\n' || c == '\r') ? ' ' : c;
        lastWasSpace = false;
        break;
      default:
        for (char c : in) if (c != '\r') out += c;
        lastWasSpace = false;
        break;
    }
    return out;
  }

  void Collect(Box& parent) {
    for (auto& childPtr : parent.children) {
      Box& child = *childPtr;
      if (child.outsideMarker) continue;
      if (child.IsOutOfFlow()) {
        Atom a;
        a.kind = Atom::Kind::OutOfFlow;
        a.box = &child;
        atomStart.push_back(all.size());
        atoms.push_back(a);
        continue;
      }
      switch (child.kind) {
        case Box::Kind::Text: AddText(child); break;
        case Box::Kind::LineBreak: {
          Atom a;
          a.kind = Atom::Kind::Break;
          a.box = &child;
          a.breakAfter = true;
          a.forced = true;
          atomStart.push_back(all.size());
          all += '\n';
          atoms.push_back(a);
          lastWasSpace = true;
          break;
        }
        case Box::Kind::Inline: {
          Atom open;
          open.kind = Atom::Kind::Open;
          open.box = &child;
          atomStart.push_back(all.size());
          atoms.push_back(open);
          Collect(child);
          Atom close;
          close.kind = Atom::Kind::Close;
          close.box = &child;
          atomStart.push_back(all.size());
          atoms.push_back(close);
          break;
        }
        case Box::Kind::Block: {
          // An atomic inline.
          Atom a;
          a.kind = Atom::Kind::Atomic;
          a.box = &child;
          atomStart.push_back(all.size());
          AppendCp(all, 0xFFFC);
          atoms.push_back(a);
          lastWasSpace = false;
          break;
        }
      }
    }
  }

  void AddText(Box& box) {
    box.processed = Process(box);
    if (box.processed.empty()) return;
    auto data = std::make_unique<TextData>();
    data->box = &box;
    data->size = box.style->fontSize;
    data->letterSpacing = box.style->letterSpacing;
    data->wordSpacing = box.style->wordSpacing;
    Shape(*data);
    // One atom for the whole text for now; it is split at break opportunities once those are known.
    Atom a;
    a.kind = Atom::Kind::Text;
    a.box = &box;
    a.data = data.get();
    a.begin = 0;
    a.end = box.processed.size();
    atomStart.push_back(all.size());
    all += box.processed;
    atoms.push_back(a);
    texts.push_back(std::move(data));
  }

  void Shape(TextData& data) {
    Box& box = *data.box;
    const FontSet& set = FontsOf(lc, *box.style);
    const std::string& text = box.processed;
    // Split into runs by the face that has the character.
    size_t runBegin = 0;
    std::shared_ptr<font::Face> runFace;
    const auto flush = [&](size_t end) {
      if (end <= runBegin || !runFace) return;
      font::ShapeOptions options;
      options.kerning = true;
      Run run;
      run.begin = runBegin;
      run.end = end;
      run.face = runFace;
      std::string slice = text.substr(runBegin, end - runBegin);
      run.glyphs = runFace->Shape(slice, options);
      for (font::Glyph& g : run.glyphs) g.cluster += static_cast<uint32_t>(runBegin);
      // tabs and word spacing
      data.runs.push_back(std::move(run));
    };
    for (size_t i = 0; i < text.size();) {
      const size_t start = i;
      const char32_t cp = DecodeAt(text, i);
      std::shared_ptr<font::Face> face;
      for (const auto& f : set.faces) {
        if (f->HasGlyph(cp) || IsCombining(cp) || cp == '\n') { face = f; break; }
      }
      if (!face) {
        face = font::Database::System().Fallback(cp, solar::css::MakeFontRequest(box.style->fontFamily, box.style->fontWeight, box.style->fontStretch, box.style->italic));
        if (!face && !set.faces.empty()) face = set.faces[0];
      }
      // Combining marks and joiners stay with the face of the character before them.
      if (IsCombining(cp) && runFace) face = runFace;
      if (face != runFace) {
        flush(start);
        runBegin = start;
        runFace = face;
      }
    }
    flush(text.size());
    // Word spacing goes on the spaces.
    if (data.wordSpacing != 0) {
      for (Run& run : data.runs) {
        for (font::Glyph& g : run.glyphs) {
          if (g.cluster < text.size() && (text[g.cluster] == ' ' || static_cast<unsigned char>(text[g.cluster]) == 0xA0)) g.advance += data.wordSpacing / data.size;
        }
      }
    }
  }
};

void FindBreaks(const std::string& all, const BoxStyle& containerStyle, std::vector<char>& breakAfter) {
  text::LineBreakOptions options;
  options.strictness = containerStyle.lineBreak == 1 ? text::LineBreakOptions::Strictness::Loose : containerStyle.lineBreak == 2 ? text::LineBreakOptions::Strictness::Strict :
                       containerStyle.lineBreak == 3 ? text::LineBreakOptions::Strictness::Anywhere : text::LineBreakOptions::Strictness::Normal;
  options.words = containerStyle.wordBreak == WordBreak::BreakAll ? text::LineBreakOptions::Words::BreakAll : containerStyle.wordBreak == WordBreak::KeepAll ? text::LineBreakOptions::Words::KeepAll :
                  text::LineBreakOptions::Words::Normal;
  options.japaneseOrChinese = containerStyle.lang.rfind("ja", 0) == 0 || containerStyle.lang.rfind("zh", 0) == 0;
  breakAfter = text::FindLineBreaks(all, options);
  // Spaces are where lines end when the style keeps them (break-spaces): the algorithm allows a break after the last of a run only.
  if (containerStyle.whiteSpaceCollapse == WhiteSpaceCollapse::BreakSpaces) {
    for (size_t i = 0; i < all.size(); ++i) if (all[i] == ' ') breakAfter[i + 1] = 1;
  }
  // A newline in the text ends the line whatever the algorithm says of it.
  for (size_t i = 0; i < all.size(); ++i) if (all[i] == '\n') breakAfter[i + 1] = 1;
}

// ---- Lines ----

struct StackEntry {
  Box* box;
  double startX;       // where this fragment of the box began on the line
  bool used = false;
  bool continued = false;  // began on an earlier line
};

struct LineBuilder {
  LayoutContext& lc;
  Box& container;
  const BoxStyle& style;
  double width;  // content width

  std::vector<Atom>& atoms;
  size_t next = 0;
  double y = 0;  // top of the next line, in the content box
  bool firstLine = true;

  LineBuilder(LayoutContext& l, Box& c, double w, std::vector<Atom>& a) : lc(l), container(c), style(*c.style), width(w), atoms(a) {}
};

double MarginPx(const BoxStyle& s, int side, double basis) { return s.margin[side].IsAuto() ? 0 : s.margin[side].Resolve(basis); }

// The vertical extents of one inline box or piece, from the line's baseline.
struct Extent {
  double above = 0, below = 0;
  bool top = false, bottom = false;
};

Metrics BoxMetrics(LayoutContext& lc, const BoxStyle& style, double& lineHeight) {
  const FontSet& set = FontsOf(lc, style);
  const Metrics m = MetricsFor(set.faces.empty() ? nullptr : set.faces[0].get(), style.fontSize);
  if (style.lineHeight.IsAuto()) lineHeight = m.ascent + m.descent + m.lineGap;
  else lineHeight = style.lineHeight.value;
  return m;
}

}  // namespace

std::shared_ptr<font::Face> PrimaryFace(LayoutContext& lc, const Box& box) {
  const FontSet& set = FontsOf(lc, *box.style);
  return set.faces.empty() ? nullptr : set.faces[0];
}

namespace {

// Splits each text atom at the break opportunities, makes chunks.
struct Chunk {
  std::vector<Atom> atoms;
  double width = 0;
  double trailing = 0;
  bool forced = false;
  bool hasContent = false;   // anything that makes a line non-empty
};

void MeasureAtom(LayoutContext& lc, Atom& a, double availableForAtomic) {
  switch (a.kind) {
    case Atom::Kind::Text: {
      a.width = WidthOf(*a.data, a.begin, a.end);
      // trailing collapsible spaces
      const std::string& t = a.box->processed;
      const WhiteSpaceCollapse mode = a.box->style->whiteSpaceCollapse;
      size_t e = a.end;
      double trailing = 0;
      size_t bytes = 0;
      if (mode == WhiteSpaceCollapse::Collapse || mode == WhiteSpaceCollapse::PreserveBreaks) {
        while (e > a.begin && t[e - 1] == ' ') { --e; ++bytes; }
        if (bytes) trailing = WidthOf(*a.data, e, a.end);
      } else if (mode == WhiteSpaceCollapse::Preserve || mode == WhiteSpaceCollapse::PreserveSpaces) {
        // Preserved spaces at the end of a line hang when wrapping, and are kept when not; both do not count for fit.
        if (a.box->style->wrap) {
          while (e > a.begin && t[e - 1] == ' ') { --e; ++bytes; }
          if (bytes) trailing = WidthOf(*a.data, e, a.end);
        }
      }
      a.trailing = trailing;
      a.trailingBytes = bytes;
      break;
    }
    case Atom::Kind::Open: {
      const BoxStyle& s = *a.box->style;
      Box& b = *a.box;
      const double cb = availableForAtomic;
      b.border = {s.border[0], s.border[1], s.border[2], s.border[3]};
      b.padding = {std::max(0.0, s.padding[0].Resolve(cb)), std::max(0.0, s.padding[1].Resolve(cb)), std::max(0.0, s.padding[2].Resolve(cb)), std::max(0.0, s.padding[3].Resolve(cb))};
      b.margin = {s.margin[0].IsAuto() ? 0 : s.margin[0].Resolve(cb), s.margin[1].IsAuto() ? 0 : s.margin[1].Resolve(cb), s.margin[2].IsAuto() ? 0 : s.margin[2].Resolve(cb),
                  s.margin[3].IsAuto() ? 0 : s.margin[3].Resolve(cb)};
      a.width = b.margin.left + b.border.left + b.padding.left;
      break;
    }
    case Atom::Kind::Close: {
      Box& b = *a.box;
      a.width = b.margin.right + b.border.right + b.padding.right;
      break;
    }
    case Atom::Kind::Atomic: {
      LayoutBlockLevel(lc, *a.box, availableForAtomic, std::numeric_limits<double>::quiet_NaN(), true);
      a.width = a.box->margin.left + a.box->width + a.box->margin.right;
      break;
    }
    case Atom::Kind::Break: a.width = 0; break;
    case Atom::Kind::OutOfFlow: a.width = 0; break;
  }
}

}  // namespace

double LayoutInlineContent(LayoutContext& lc, Box& container, double width, double& baselineOut) {
  container.lines.clear();
  baselineOut = -1;
  Collector collector(lc, container);
  collector.Collect(container);
  if (collector.atoms.empty()) return 0;
  std::vector<char> breaks;
  FindBreaks(collector.all, *container.style, breaks);

  // Split text atoms at break opportunities, and measure.
  std::vector<Atom> atoms;
  for (size_t i = 0; i < collector.atoms.size(); ++i) {
    Atom a = collector.atoms[i];
    if (a.kind == Atom::Kind::Text) {
      const size_t base = collector.atomStart[i];
      size_t from = a.begin;
      const std::string& t = a.box->processed;
      for (size_t k = a.begin; k < a.end; ++k) {
        // k is a byte; the opportunity is at the end of a character.
        const bool atCharEnd = k + 1 >= a.end || (static_cast<unsigned char>(t[k + 1]) & 0xC0) != 0x80;
        if (!atCharEnd) continue;
        const bool brk = breaks[base + (k + 1 - a.begin)];
        const bool newline = t[k] == '\n';
        if (brk || newline) {
          Atom piece = a;
          piece.begin = from;
          piece.end = k + 1;
          piece.breakAfter = brk;
          // a newline in preserved text is a forced break, not a glyph
          if (newline) {
            piece.end = k;
            piece.forced = true;
            piece.breakAfter = true;
          }
          atoms.push_back(piece);
          from = k + 1;
        }
      }
      if (from < a.end) {
        Atom piece = a;
        piece.begin = from;
        piece.end = a.end;
        piece.breakAfter = false;
        atoms.push_back(piece);
      }
    } else {
      // A break opportunity after an atomic inline and before one, at the Open/Close markers it is carried on the text atoms.
      Atom piece = a;
      if (a.kind == Atom::Kind::Atomic || a.kind == Atom::Kind::Break) {
        const size_t at = collector.atomStart[i] + (a.kind == Atom::Kind::Atomic ? 3 : 1);
        piece.breakAfter = a.kind == Atom::Kind::Break || (at < breaks.size() && breaks[at]);
        if (a.kind == Atom::Kind::Break) piece.forced = true;
      }
      atoms.push_back(piece);
    }
  }
  // Text pieces whose break comes just before an Open/Close marker: the opportunity belongs to the piece, as markers are zero width in the text.
  for (Atom& a : atoms) MeasureAtom(lc, a, width);

  // ---- Chunks: runs of atoms up to a break opportunity ----
  std::vector<Chunk> chunks;
  {
    Chunk current;
    for (Atom& a : atoms) {
      current.atoms.push_back(a);
      if (a.kind == Atom::Kind::Text || a.kind == Atom::Kind::Atomic) current.hasContent = true;
      if (a.kind == Atom::Kind::Open || a.kind == Atom::Kind::Close) {
        const Box& b = *a.box;
        if (a.width > 0 || b.border.Horizontal() > 0 || b.padding.Horizontal() > 0) current.hasContent = true;
      }
      if (a.kind == Atom::Kind::Break) current.hasContent = true;
      const bool wraps = a.box && a.box->style->wrap;
      if ((a.breakAfter && (wraps || a.forced)) || a.forced) {
        current.forced = a.forced;
        chunks.push_back(std::move(current));
        current = Chunk();
      }
    }
    if (!current.atoms.empty()) chunks.push_back(std::move(current));
    for (Chunk& c : chunks) {
      c.width = 0;
      for (const Atom& a : c.atoms) c.width += a.width;
      // trailing spaces: those at the end of the last text atom (or of several, when spaces span boxes)
      c.trailing = 0;
      for (size_t k = c.atoms.size(); k-- > 0;) {
        const Atom& a = c.atoms[k];
        if (a.kind == Atom::Kind::OutOfFlow) continue;
        if (a.kind == Atom::Kind::Close || a.kind == Atom::Kind::Open) {
          if (a.width == 0) continue;
          break;
        }
        if (a.kind == Atom::Kind::Text) {
          c.trailing += a.trailing;
          // When the whole atom is spaces the spaces before it count too.
          if (a.trailingBytes == a.end - a.begin) continue;
        }
        break;
      }
    }
  }

  // ---- Lines ----
  const BoxStyle& cs = *container.style;
  double lineTop = 0;       // in the content box
  double lineHeightStrut = 0;
  double strutAbove = 0, strutBelow = 0;
  Metrics rootMetrics = BoxMetrics(lc, cs, lineHeightStrut);
  {
    const double halfLeading = (lineHeightStrut - (rootMetrics.ascent + rootMetrics.descent)) / 2;
    strutAbove = rootMetrics.ascent + halfLeading;
    strutBelow = rootMetrics.descent + halfLeading;
  }
  const double indent = cs.textIndent.Resolve(width);
  std::vector<StackEntry> stack;   // inline boxes open at the current point
  bool first = true;
  size_t ci = 0;
  double lastBaselineFromTop = -1;
  std::vector<Box*> deferredFloats;
  const double strutHeight = std::max(1.0, strutAbove + strutBelow);
  const auto placeFloat = [&](Box* f) {
    const FloatRecord rec = PlaceFloat(lc, *f, lc.containerX, width, lc.containerY + lineTop);
    f->x = rec.left - lc.containerX + f->margin.left + container.ContentLeft();
    f->y = rec.top - lc.containerY + f->margin.top + container.ContentTop();
  };
  const auto isFloatBox = [](const Box* b) { return b->style->IsFloating() && !b->style->IsOutOfFlow(); };
  while (ci < chunks.size()) {
    const bool isFirstLine = first;
    first = false;
    const double indentHere = (isFirstLine != cs.textIndentHanging) ? indent : 0;
    // Floats that waited for this line go first.
    for (Box* f : deferredFloats) placeFloat(f);
    deferredFloats.clear();
    // The room beside floats.
    double lineOffsetX = 0, availableLine = width;
    const auto updateBand = [&] {
      if (lc.bfc->floats.empty()) return;
      double l, r;
      lc.bfc->Band(lc.containerY + lineTop, strutHeight, l, r);
      ++lc.floatEvents;
      const double left = std::max(lc.containerX, l), right = std::min(lc.containerX + width, r);
      lineOffsetX = left - lc.containerX;
      availableLine = std::max(0.0, right - left);
    };
    updateBand();
    // A line whose first piece does not fit beside the floats goes below the next one's bottom edge.
    for (int tries = 0; tries < 200 && !lc.bfc->floats.empty() && ci < chunks.size(); ++tries) {
      const double w1 = chunks[ci].width - chunks[ci].trailing + indentHere;
      if (w1 <= availableLine + kEpsilon || availableLine >= width - kEpsilon) break;
      const double y = lc.containerY + lineTop;
      const double next = lc.bfc->NextEdge(y, strutHeight);
      if (next <= y + kEpsilon) break;
      lineTop += next - y;
      updateBand();
    }
    double available = std::max(0.0, availableLine - indentHere);
    // Gather chunks for this line.
    std::vector<Chunk*> onLine;
    double used = 0, trailing = 0;
    bool forced = false;
    while (ci < chunks.size()) {
      Chunk& c = chunks[ci];
      const double fit = used + c.width - c.trailing;
      const bool wrapAllowed = c.atoms.empty() || !c.atoms[0].box || c.atoms[0].box->style->wrap;
      if (!onLine.empty() && wrapAllowed && fit > available + kEpsilon) break;
      onLine.push_back(&c);
      used += c.width;
      trailing = c.trailing;
      ++ci;
      // Floats in the chunk: beside the line if there is room, else below it.
      bool placedFloat = false;
      for (const Atom& a : c.atoms) {
        if (a.kind != Atom::Kind::OutOfFlow || !isFloatBox(a.box)) continue;
        ComputeContentSizes(lc, *a.box);
        const BoxStyle& fs = *a.box->style;
        const double approx = std::min(std::max(a.box->minContent, width), a.box->maxContent) + MarginPx(fs, 1, width) + MarginPx(fs, 3, width);
        if (used - c.width > 0 && approx > available - (used - c.width) + kEpsilon) {
          deferredFloats.push_back(a.box);
        } else {
          placeFloat(a.box);
          placedFloat = true;
        }
      }
      if (placedFloat) {
        updateBand();
        available = std::max(0.0, availableLine - indentHere);
      }
      if (c.forced) { forced = true; break; }
    }
    // A chunk that is too wide with the line empty: broken inside if the style allows.
    bool splitLast = false;
    if (onLine.size() == 1 && used - trailing > available + kEpsilon) {
      const Chunk& c = *onLine[0];
      bool emergency = false;
      for (const Atom& a : c.atoms) {
        if (a.kind == Atom::Kind::Text && (a.box->style->overflowWrap != OverflowWrap::Normal || a.box->style->wordBreak == WordBreak::BreakWord) && a.box->style->wrap) emergency = true;
      }
      splitLast = emergency;
    }
    if (splitLast) {
      // Cut the chunk at the last character that fits, and put the rest back as the next chunk.
      Chunk& c = *onLine[0];
      double acc = 0;
      std::vector<Atom> head, tail;
      bool cut = false;
      for (Atom& a : c.atoms) {
        if (cut) { tail.push_back(a); continue; }
        if (a.kind != Atom::Kind::Text || acc + a.width - a.trailing <= available + kEpsilon) {
          head.push_back(a);
          acc += a.width;
          continue;
        }
        // Walk the characters of the atom.
        size_t pos = a.begin;
        double w = 0;
        size_t lastFit = a.begin;
        const std::string& t = a.box->processed;
        while (pos < a.end) {
          size_t next = pos;
          DecodeAt(t, next);
          while (next < a.end && IsCombining([&] { size_t j = next; return DecodeAt(t, j); }())) { DecodeAt(t, next); }
          const double cw = WidthOf(*a.data, pos, next);
          if (acc + w + cw > available + kEpsilon && lastFit > a.begin) break;
          if (acc + w + cw > available + kEpsilon && lastFit == a.begin && head.empty() && w == 0) { w += cw; lastFit = next; pos = next; break; }
          w += cw;
          lastFit = next;
          pos = next;
        }
        Atom h = a, rest = a;
        h.end = lastFit;
        h.breakAfter = true;
        h.width = WidthOf(*a.data, h.begin, h.end);
        h.trailing = 0;
        h.trailingBytes = 0;
        rest.begin = lastFit;
        rest.width = WidthOf(*a.data, rest.begin, rest.end);
        head.push_back(h);
        acc += h.width;
        if (rest.begin < rest.end) tail.push_back(rest);
        cut = true;
      }
      if (!tail.empty()) {
        Chunk rest;
        rest.atoms = std::move(tail);
        rest.forced = c.forced;
        rest.hasContent = true;
        for (const Atom& a : rest.atoms) rest.width += a.width;
        for (size_t k = rest.atoms.size(); k-- > 0;) {
          if (rest.atoms[k].kind == Atom::Kind::Text) { rest.trailing = rest.atoms[k].trailing; break; }
          if (rest.atoms[k].width > 0) break;
        }
        c.atoms = std::move(head);
        c.forced = false;
        c.width = acc;
        c.trailing = 0;
        used = acc;
        trailing = 0;
        forced = false;
        chunks.insert(chunks.begin() + ci, std::move(rest));
        onLine[0] = &chunks[ci - 1];
      }
    }

    // ---- Place the line's atoms ----
    // Trailing spaces that go, and the leading ones of a line (collapsible spaces at its start were removed in processing, except after a break).
    struct Placed {
      Atom atom;
      double x = 0;
      double width = 0;
    };
    std::vector<Placed> placed;
    for (Chunk* c : onLine) {
      for (const Atom& a : c->atoms) placed.push_back({a, 0, a.width});
    }
    // Trim the trailing collapsible spaces of the last text pieces (and any spaces-only pieces before it).
    {
      bool trimming = true;
      for (size_t k = placed.size(); k-- > 0 && trimming;) {
        Placed& p = placed[k];
        if (p.atom.kind == Atom::Kind::OutOfFlow) continue;
        if (p.atom.kind == Atom::Kind::Close || p.atom.kind == Atom::Kind::Open) {
          if (p.atom.width == 0) continue;
          break;
        }
        if (p.atom.kind == Atom::Kind::Text) {
          if (p.atom.trailingBytes > 0 && p.atom.box->style->wrap) {
            p.atom.end -= p.atom.trailingBytes;
            p.width -= p.atom.trailing;
            const bool wholeAtom = p.atom.end == p.atom.begin;
            p.atom.trailingBytes = 0;
            p.atom.trailing = 0;
            if (!wholeAtom) trimming = false;
            continue;
          }
          trimming = false;
          continue;
        }
        trimming = false;
      }
    }
    // Is anything on the line?
    bool significant = forced;
    for (const Placed& p : placed) {
      if (p.atom.kind == Atom::Kind::Text && p.atom.end > p.atom.begin) significant = true;
      else if (p.atom.kind == Atom::Kind::Atomic || p.atom.kind == Atom::Kind::Break) significant = true;
      else if ((p.atom.kind == Atom::Kind::Open || p.atom.kind == Atom::Kind::Close) && p.atom.width != 0) significant = true;
    }

    // Open boxes continue on the line: they are on the stack already. Now walk the atoms.
    Line line;
    double lineWidth = 0;
    for (const Placed& p : placed) lineWidth += p.width;
    // Alignment.
    TextAlign align = cs.textAlign;
    const bool lastOfBlock = ci >= chunks.size();
    if (align == TextAlign::MatchParent) align = TextAlign::Start;
    if (align == TextAlign::Justify && (lastOfBlock || forced)) {
      align = cs.textAlignLastAuto ? TextAlign::Start : cs.textAlignLast;
      if (align == TextAlign::MatchParent) align = TextAlign::Start;
    } else if ((lastOfBlock || forced) && !cs.textAlignLastAuto) {
      align = cs.textAlignLast;
    }
    const bool rtl = cs.direction == Direction::Rtl;
    if (align == TextAlign::Start) align = rtl ? TextAlign::Right : TextAlign::Left;
    if (align == TextAlign::End) align = rtl ? TextAlign::Left : TextAlign::Right;
    const double freeSpace = std::max(0.0, available - lineWidth);
    double offset = 0;
    if (align == TextAlign::Right) offset = freeSpace;
    else if (align == TextAlign::Center) offset = freeSpace / 2;
    // With indentation in rtl the indent is from the right.
    double startX = lineOffsetX + (rtl ? 0 : indentHere) + offset;
    double justifyExtra = 0;
    if (align == TextAlign::Justify && significant) {
      int spaces = 0;
      for (const Placed& p : placed) {
        if (p.atom.kind != Atom::Kind::Text) continue;
        for (size_t k = p.atom.begin; k < p.atom.end; ++k) if (p.atom.box->processed[k] == ' ') ++spaces;
      }
      if (spaces > 0) justifyExtra = (available - lineWidth) / spaces;
      if (justifyExtra < 0) justifyExtra = 0;
    }

    // Positions, and what each box looks like.
    struct Piece {
      LineItem item;
      Box* inlineParent;
      double above = 0, below = 0;
      double shift = 0;  // from the line's baseline, up positive
      bool top = false, bottom = false;
    };
    std::vector<Piece> pieces;
    struct BoxExtent {
      Box* box;
      double xStart, xEnd;
      double shift;
      double above, below;  // of the inline box (line-height)
      double contentAscent, contentDescent;
      bool top, bottom;
      bool used;
    };
    std::vector<BoxExtent> extents;
    // The boxes open at the start of the line continue.
    struct OpenBox {
      Box* box;
      size_t extentIndex;
      double shift;
    };
    std::vector<OpenBox> open;
    const auto metricsOfBox = [&](Box* b, double& lineHeight) { return BoxMetrics(lc, *b->style, lineHeight); };
    const auto shiftOf = [&](Box* b, double parentShift, Box* parentBox, const Metrics& m, double aboveOfBox, double belowOfBox, bool& top, bool& bottom) {
      const BoxStyle& s = *b->style;
      top = bottom = false;
      double lh = 0;
      Metrics pm = metricsOfBox(parentBox, lh);
      switch (s.verticalAlign) {
        case VerticalAlign::Baseline: return parentShift;
        case VerticalAlign::Sub: return parentShift - (parentBox->style->fontSize / 5 + 1);
        case VerticalAlign::Super: return parentShift + (parentBox->style->fontSize / 3 + 1);
        case VerticalAlign::TextTop: return parentShift + pm.ascent - m.ascent;
        case VerticalAlign::TextBottom: return parentShift - pm.descent + m.descent;
        case VerticalAlign::Middle: return parentShift + pm.xHeight / 2 - (aboveOfBox - belowOfBox) / 2;
        case VerticalAlign::Top: top = true; return parentShift;
        case VerticalAlign::Bottom: bottom = true; return parentShift;
        case VerticalAlign::Length: {
          const Length& l = s.verticalAlignLength;
          double v = 0;
          if (l.kind == Length::Kind::Percent) {
            double lhOwn = 0;
            metricsOfBox(b, lhOwn);
            v = lhOwn * l.value / 100;
          } else {
            v = l.Resolve(0);
          }
          return parentShift + v;
        }
      }
      return parentShift;
    };
    // The root inline box: the container itself.
    const double rootShift = 0;
    // Re-open the boxes that continue from the previous line.
    double x = startX;
    for (const StackEntry& e : stack) {
      BoxExtent be;
      be.box = e.box;
      be.xStart = x;
      be.xEnd = x;
      double lh = 0;
      const Metrics m = metricsOfBox(e.box, lh);
      const double half = (lh - (m.ascent + m.descent)) / 2;
      be.above = m.ascent + half;
      be.below = m.descent + half;
      be.contentAscent = m.ascent;
      be.contentDescent = m.descent;
      Box* parentBox = open.empty() ? &container : open.back().box;
      const double parentShift = open.empty() ? rootShift : open.back().shift;
      be.shift = shiftOf(e.box, parentShift, parentBox, m, be.above, be.below, be.top, be.bottom);
      be.used = false;
      extents.push_back(be);
      open.push_back({e.box, extents.size() - 1, be.shift});
    }
    stack.clear();
    const auto markUsed = [&] {
      for (OpenBox& o : open) extents[o.extentIndex].used = true;
    };
    for (Placed& p : placed) {
      const Atom& a = p.atom;
      switch (a.kind) {
        case Atom::Kind::Open: {
          BoxExtent be;
          be.box = a.box;
          be.xStart = x;
          be.xEnd = x;
          double lh = 0;
          const Metrics m = metricsOfBox(a.box, lh);
          const double half = (lh - (m.ascent + m.descent)) / 2;
          be.above = m.ascent + half;
          be.below = m.descent + half;
          be.contentAscent = m.ascent;
          be.contentDescent = m.descent;
          Box* parentBox = open.empty() ? &container : open.back().box;
          const double parentShift = open.empty() ? rootShift : open.back().shift;
          be.shift = shiftOf(a.box, parentShift, parentBox, m, be.above, be.below, be.top, be.bottom);
          be.used = p.width != 0 || a.box->border.Horizontal() > 0 || a.box->padding.Horizontal() > 0;
          extents.push_back(be);
          open.push_back({a.box, extents.size() - 1, be.shift});
          x += p.width;
          break;
        }
        case Atom::Kind::Close: {
          x += p.width;
          if (!open.empty() && open.back().box == a.box) {
            BoxExtent& be = extents[open.back().extentIndex];
            be.xEnd = x;
            if (p.width != 0) be.used = true;
            open.pop_back();
          } else {
            // closing a box opened on an earlier line that is not on top: find it
            for (size_t k = open.size(); k-- > 0;) {
              if (open[k].box == a.box) {
                extents[open[k].extentIndex].xEnd = x;
                open.erase(open.begin() + k);
                break;
              }
            }
          }
          break;
        }
        case Atom::Kind::Text: {
          if (a.end <= a.begin) break;
          markUsed();
          Box* parentBox = open.empty() ? &container : open.back().box;
          const double shift = open.empty() ? rootShift : open.back().shift;
          Piece piece;
          piece.item.kind = LineItem::Kind::Text;
          piece.item.box = a.box;
          TextPiece& tp = piece.item.text;
          tp.box = a.box;
          tp.begin = a.begin;
          tp.end = a.end;
          tp.x = x;
          tp.fontSize = a.box->style->fontSize;
          const FontSet& set = FontsOf(lc, *a.box->style);
          tp.face = a.data->runs.empty() ? (set.faces.empty() ? nullptr : set.faces[0]) : a.data->runs[0].face;
          for (const Run& run : a.data->runs) {
            if (run.end <= a.begin || run.begin >= a.end) continue;
            for (font::Glyph g : run.glyphs) {
              if (g.cluster >= a.begin && g.cluster < a.end) {
                g.cluster -= static_cast<uint32_t>(a.begin);
                tp.glyphs.push_back(g);
              }
            }
          }
          double w = p.width;
          if (justifyExtra > 0) {
            int spaces = 0;
            for (size_t k = a.begin; k < a.end; ++k) if (a.box->processed[k] == ' ') ++spaces;
            w += spaces * justifyExtra;
          }
          tp.width = w;
          piece.item.rect = {x, 0, w, 0};
          piece.inlineParent = parentBox;
          piece.shift = shift;
          double lh = 0;
          const Metrics m = metricsOfBox(parentBox, lh);
          const double half = (lh - (m.ascent + m.descent)) / 2;
          piece.above = m.ascent + half;
          piece.below = m.descent + half;
          pieces.push_back(std::move(piece));
          x += w;
          break;
        }
        case Atom::Kind::Atomic: {
          markUsed();
          Box& b = *a.box;
          Piece piece;
          piece.item.kind = LineItem::Kind::Atomic;
          piece.item.box = &b;
          const double ml = b.margin.left;
          piece.item.rect = {x + ml, 0, b.width, b.height};
          piece.inlineParent = open.empty() ? &container : open.back().box;
          const double parentShift = open.empty() ? rootShift : open.back().shift;
          // The baseline of the atomic inline: its own, or the bottom margin edge.
          const double baselineInBox = b.baseline >= 0 && b.style->overflowY == Overflow::Visible && !b.style->containLayout ? b.baseline : b.height;
          const double aboveBase = b.margin.top + baselineInBox;
          const double belowBase = b.height - baselineInBox + b.margin.bottom;
          double shift = parentShift;
          const BoxStyle& s = *b.style;
          double lh = 0;
          Metrics pm = metricsOfBox(piece.inlineParent, lh);
          switch (s.verticalAlign) {
            case VerticalAlign::Baseline: break;
            case VerticalAlign::Sub: shift -= piece.inlineParent->style->fontSize / 5 + 1; break;
            case VerticalAlign::Super: shift += piece.inlineParent->style->fontSize / 3 + 1; break;
            case VerticalAlign::TextTop: shift += pm.ascent - aboveBase; break;
            case VerticalAlign::TextBottom: shift += -pm.descent + belowBase; break;
            case VerticalAlign::Middle: shift += pm.xHeight / 2 - (aboveBase - belowBase) / 2; break;
            case VerticalAlign::Top: piece.top = true; break;
            case VerticalAlign::Bottom: piece.bottom = true; break;
            case VerticalAlign::Length: {
              const Length& l = s.verticalAlignLength;
              shift += l.kind == Length::Kind::Percent ? (s.lineHeight.IsAuto() ? (pm.ascent + pm.descent + pm.lineGap) : s.lineHeight.value) * l.value / 100 : l.Resolve(0);
              break;
            }
          }
          piece.shift = shift;
          piece.above = aboveBase;
          piece.below = belowBase;
          pieces.push_back(std::move(piece));
          x += p.width;
          break;
        }
        case Atom::Kind::Break: {
          markUsed();
          break;
        }
        case Atom::Kind::OutOfFlow: {
          if (a.box->style->IsOutOfFlow()) {
            a.box->staticX = container.ContentLeft() + x;
            a.box->staticY = container.ContentTop() + lineTop;
          }
          break;
        }
      }
    }
    // Boxes still open at the end of the line continue on the next.
    for (OpenBox& o : open) {
      extents[o.extentIndex].xEnd = x;
      stack.push_back({o.box, 0, false, true});
    }
    // When a line was trimmed, the pieces end where the last one does.
    const double endX = x;

    if (!significant) {
      // A line with nothing on it takes no height; boxes open across it stay open.
      continue;
    }

    // ---- Vertical ----
    double above = strutAbove, below = strutBelow;
    for (const BoxExtent& be : extents) {
      if (!be.used || be.top || be.bottom) continue;
      above = std::max(above, be.shift + be.above);
      below = std::max(below, -be.shift + be.below);
    }
    for (const Piece& p : pieces) {
      if (p.top || p.bottom) continue;
      if (p.item.kind == LineItem::Kind::Text) continue;  // (the box that holds the text has been counted)
      above = std::max(above, p.shift + p.above);
      below = std::max(below, -p.shift + p.below);
    }
    double alignedTall = 0;
    for (const BoxExtent& be : extents) if (be.used && (be.top || be.bottom)) alignedTall = std::max(alignedTall, be.above + be.below);
    for (const Piece& p : pieces) if ((p.top || p.bottom) && p.item.kind == LineItem::Kind::Atomic) alignedTall = std::max(alignedTall, p.above + p.below);
    if (alignedTall > above + below) {
      // top-aligned things extend the bottom; bottom-aligned ones the top (when both, the line is as tall as the taller).
      below = alignedTall - above;
    }
    const double lineHeight = above + below;
    const double baselineY = above;

    line.rect = {container.ContentLeft() + lineOffsetX, container.ContentTop() + lineTop, availableLine, lineHeight};
    line.baseline = baselineY;
    const double originX = container.ContentLeft(), originY = container.ContentTop() + lineTop;
    // Fragments of the inline boxes.
    for (BoxExtent& be : extents) {
      if (!be.used) continue;
      Box& b = *be.box;
      double top;
      double heightOfContent = be.contentAscent + be.contentDescent;
      if (be.top) top = 0 + 0;
      else if (be.bottom) top = lineHeight - be.above - be.below;
      else top = baselineY - be.shift - be.contentAscent;
      if (be.top) top = (be.above + be.below - heightOfContent) / 2;
      if (be.bottom) top = lineHeight - (be.above + be.below) + (be.above + be.below - heightOfContent) / 2;
      // The border box includes the padding and border of the box on the ends that this fragment has.
      Rect r;
      r.x = originX + be.xStart + b.margin.left * 0;
      r.width = be.xEnd - be.xStart;
      // the margin is outside the border box: drop it from the ends this fragment has
      r.y = originY + top - b.padding.top - b.border.top;
      r.height = heightOfContent + b.padding.Vertical() + b.border.Vertical();
      // (Open's width included the left margin; Close's the right.)
      b.fragments.push_back(r);
    }
    // Items.
    for (Piece& p : pieces) {
      LineItem item = std::move(p.item);
      if (item.kind == LineItem::Kind::Text) {
        double lh = 0;
        const Metrics m = metricsOfBox(p.inlineParent, lh);
        double top;
        if (p.top) top = 0;
        else if (p.bottom) top = lineHeight - (m.ascent + m.descent);
        else top = baselineY - p.shift - m.ascent;
        item.rect.x += originX;
        item.rect.y = originY + top;
        item.rect.height = m.ascent + m.descent;
        item.text.x = item.rect.x;
        item.text.baselineShift = p.shift;
        item.box->fragments.push_back(item.rect);
      } else {
        double top;
        if (p.top) top = 0;
        else if (p.bottom) top = lineHeight - (p.above + p.below);
        else top = baselineY - p.shift - p.above;
        // `above` holds the margin top plus baseline offset; the border box starts after the margin.
        item.rect.x += originX;
        item.rect.y = originY + top + item.box->margin.top;
        item.box->x = item.rect.x;
        item.box->y = item.rect.y;
        // (relative to the container's border box)
      }
      line.items.push_back(std::move(item));
    }
    (void)endX;
    // Strut-only lines (a line with just a <br>) are as tall as the strut.
    lastBaselineFromTop = lineTop + baselineY;
    container.lines.push_back(std::move(line));
    lineTop += lineHeight;
  }
  for (Box* f : deferredFloats) placeFloat(f);
  baselineOut = lastBaselineFromTop;
  // The lines' boxes: floats and the like (placed by the caller).
  for (Box* b : collector.floatsAndOthers) (void)b;
  return lineTop;
}

void InlineContentSizes(LayoutContext& lc, Box& container, double& minContent, double& maxContent) {
  Collector collector(lc, container);
  collector.Collect(container);
  minContent = maxContent = 0;
  if (collector.atoms.empty()) return;
  std::vector<char> breaks;
  FindBreaks(collector.all, *container.style, breaks);
  double line = 0, word = 0;  // the max-content line so far, the min-content unbreakable piece so far
  for (size_t i = 0; i < collector.atoms.size(); ++i) {
    Atom a = collector.atoms[i];
    const size_t base = collector.atomStart[i];
    if (a.kind == Atom::Kind::Text) {
      const std::string& t = a.box->processed;
      size_t from = a.begin;
      for (size_t k = a.begin; k < a.end; ++k) {
        const bool charEnd = k + 1 >= a.end || (static_cast<unsigned char>(t[k + 1]) & 0xC0) != 0x80;
        if (!charEnd) continue;
        const bool brk = breaks[base + (k + 1 - a.begin)];
        const bool newline = t[k] == '\n';
        const bool wraps = a.box->style->wrap;
        if (brk || newline || k + 1 == a.end) {
          size_t end = newline ? k : k + 1;
          double w = WidthOf(*a.data, from, end);
          // trailing spaces do not count at the end of a word
          size_t e2 = end;
          double trailing = 0;
          const WhiteSpaceCollapse mode = a.box->style->whiteSpaceCollapse;
          if (mode != WhiteSpaceCollapse::BreakSpaces) {
            while (e2 > from && t[e2 - 1] == ' ' && (mode == WhiteSpaceCollapse::Collapse || mode == WhiteSpaceCollapse::PreserveBreaks || wraps)) --e2;
            trailing = e2 < end ? WidthOf(*a.data, e2, end) : 0;
          }
          line += w;
          word += w - trailing;
          if (newline) {
            maxContent = std::max(maxContent, line);
            minContent = std::max(minContent, word);
            line = word = 0;
          } else if ((brk && wraps) || (brk && false)) {
            minContent = std::max(minContent, word);
            word = 0;
          } else if (!wraps) {
            // not breakable at all
          }
          from = k + 1;
        }
      }
    } else if (a.kind == Atom::Kind::Open || a.kind == Atom::Kind::Close) {
      MeasureAtom(lc, a, 0);
      line += a.width;
      word += a.width;
    } else if (a.kind == Atom::Kind::Atomic) {
      ComputeContentSizes(lc, *a.box);
      const BoxStyle& s = *a.box->style;
      Box& b = *a.box;
      b.padding = {std::max(0.0, s.padding[0].Resolve(0)), std::max(0.0, s.padding[1].Resolve(0)), std::max(0.0, s.padding[2].Resolve(0)), std::max(0.0, s.padding[3].Resolve(0))};
      b.border = {s.border[0], s.border[1], s.border[2], s.border[3]};
      const double extras = b.padding.Horizontal() + b.border.Horizontal() + (s.margin[1].IsAuto() ? 0 : s.margin[1].Resolve(0)) + (s.margin[3].IsAuto() ? 0 : s.margin[3].Resolve(0));
      double mn = b.minContent, mx = b.maxContent;
      if (s.width.kind == Length::Kind::Px) mn = mx = s.boxSizing == BoxSizing::BorderBox ? std::max(0.0, s.width.value - b.padding.Horizontal() - b.border.Horizontal()) : s.width.value;
      line += mx + extras;
      word += mn + extras;
      const size_t at = base + 3;
      if (at < breaks.size() && breaks[at]) {
        minContent = std::max(minContent, word);
        word = 0;
      }
    } else if (a.kind == Atom::Kind::Break) {
      maxContent = std::max(maxContent, line);
      minContent = std::max(minContent, word);
      line = word = 0;
    }
  }
  maxContent = std::max(maxContent, line);
  minContent = std::max(minContent, word);
  if (!container.style->wrap) minContent = maxContent;
  // overflow-wrap: anywhere breaks everywhere for the minimum.
  (void)lc;
}

}  // namespace solar::layout
