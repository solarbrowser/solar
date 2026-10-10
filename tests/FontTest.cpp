// The font reader: Ahem, whose every glyph is a square of one em and whose ascent and descent are 0.8 and 0.2.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "solar/font/Database.h"
#include "solar/font/Face.h"

namespace {

int g_failed = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL %s\n", what);
    ++g_failed;
  }
}

bool Near(double a, double b) { return std::fabs(a - b) < 1e-6; }

}  // namespace

int main() {
  std::ifstream file("tests/wpt/fonts/Ahem.ttf", std::ios::binary);
  const std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  Check(!data.empty(), "Ahem.ttf is there");
  const auto face = solar::font::Face::Open(data);
  Check(face != nullptr, "Ahem opens");
  if (!face) return 1;
  Check(face->description().family == "Ahem", "family");
  Check(face->description().weight == 400 && !face->description().italic, "weight and style");
  Check(Near(face->metrics().ascent, 0.8) && Near(face->metrics().descent, 0.2), "ascent and descent");
  Check(face->HasGlyph('X') && Near(face->Advance('X'), 1), "X is a square");
  Check(Near(face->Measure("Xp x"), 4), "four glyphs, four em");
  const auto glyphs = face->Shape("ab");
  Check(glyphs.size() == 2 && glyphs[1].cluster == 1, "two glyphs, the second from byte 1");
  Check(!face->Outline(face->GlyphFor('X')).empty(), "X has an outline");
  Check(solar::font::Face::Open("not a font at all, really not") == nullptr, "text is not a font");
  // WOFF2: Red Hat Text (SIL Open Font License), with glyf and hmtx stored transformed.
  {
    std::string data;
    if (FILE* file = std::fopen("tests/wpt/fonts/RedHatText-Regular.woff2", "rb")) {
      char buffer[4096];
      size_t n;
      while ((n = std::fread(buffer, 1, sizeof buffer, file)) > 0) data.append(buffer, n);
      std::fclose(file);
    }
    const auto web = solar::font::Face::Open(data);
    Check(web != nullptr, "the WOFF2 file opens");
    if (web) {
      Check(web->description().family == "Red Hat Text", "WOFF2 family");
      Check(web->HasGlyph('H') && web->Advance('H') > 0.5 && web->Advance('H') < 0.9, "WOFF2 H has a sensible advance");
      Check(!web->Outline(web->GlyphFor('g')).empty(), "WOFF2 g has an outline");
      Check(web->Measure("Hello") > 2 && web->Measure("Hello") < 4, "WOFF2 shapes text");
    }
  }
  // A database of Ahem and the system: Ahem is found by its name, and a generic family finds something.
  solar::font::Database database;
  database.Add({"Ahem", {}, [face] { return face; }});
  solar::font::Request request;
  request.families = {"Nothing Like This", "ahem"};
  const auto matched = database.Match(request);
  Check(matched == face, "the second family is Ahem, whatever the case");
  request.families = {"Ahem"};
  request.weight = 700;
  Check(database.Match(request) == face, "the only font of the family is the best for any weight");
  solar::font::Database& system = solar::font::Database::System();
  solar::font::Request sans;
  sans.families = {"sans-serif"};
  const auto found = system.Match(sans);
  std::printf("sans-serif on this machine: %s\n", found ? found->description().family.c_str() : "(none)");
  sans.weight = 700;
  const auto bold = system.Match(sans);
  Check(!found || (bold && bold->description().weight >= 600), "bold sans-serif is bold when there is one");
  if (g_failed == 0) std::printf("font: all passed\n");
  return g_failed == 0 ? 0 : 1;
}
