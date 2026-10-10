#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "solar/font/Face.h"

// The fonts that can be used: those installed on the system, and those a document brings with @font-face, matched to what a
// style asks for as CSS Fonts 4 says (https://drafts.csswg.org/css-fonts-4/#font-matching-algorithm).
namespace solar::font {

enum class SlantStyle { Normal, Italic, Oblique };

struct Request {
  std::vector<std::string> families;  // family names and generic families, in order
  double weight = 400;
  double width = 100;  // a percentage
  SlantStyle style = SlantStyle::Normal;
  double obliqueAngle = 14;
};

// What a font says of the values it covers, which may be ranges (a variable font, an @font-face descriptor).
struct Coverage {
  double weightMin = 400, weightMax = 400;
  double widthMin = 100, widthMax = 100;
  SlantStyle style = SlantStyle::Normal;
  double obliqueMin = 14, obliqueMax = 14;
  // The code points it is for (unicode-range): empty is all of them.
  std::vector<std::pair<char32_t, char32_t>> unicodeRanges;
};

class Database {
 public:
  // A font of the system or of a document: the family it is called by, what it covers, and how to get at it (called once, when it is
  // first wanted; nothing means the font could not be loaded).
  struct Source {
    std::string family;
    Coverage coverage;
    std::function<std::shared_ptr<Face>()> load;
  };
  void Add(Source source);
  // The fonts in the directories the platform keeps its fonts in (and, with a directory, in that too), found when first asked.
  void AddDirectory(const std::string& directory);

  // The face for the request: the first family in it that has a font. `weight` and the rest then pick among that family's fonts.
  // For a variable font the face is at the values asked for.
  std::shared_ptr<Face> Match(const Request& request);
  // Every face that would do for the request, in order: each family's best, for the characters the first ones do not have.
  std::vector<std::shared_ptr<Face>> MatchAll(const Request& request);
  // A face with the character, looked for in all the fonts; the request's styles preferred.
  std::shared_ptr<Face> Fallback(char32_t codePoint, const Request& request);

  // A face by its full name or PostScript name, which local() in a src names.
  std::shared_ptr<Face> FindLocal(const std::string& name);
  // The names of the families, for tests.
  std::vector<std::string> Families();
  // The generic family's candidates, best first.
  static const std::vector<std::string>& GenericCandidates(const std::string& generic);

  // The database of the system's installed fonts (made on first use).
  static Database& System();

 private:
  struct Entry {
    Source source;
    std::shared_ptr<Face> face;
    bool loaded = false;
  };
  void EnsureScanned();
  std::shared_ptr<Face> FaceOf(Entry& entry);
  std::shared_ptr<Face> BestIn(const std::string& family, const Request& request);
  std::vector<Entry*> InFamily(const std::string& family);

  std::mutex mutex_;
  std::vector<std::unique_ptr<Entry>> entries_;
  std::multimap<std::string, Entry*> byFamily_;  // lowercased
  std::vector<std::string> directories_;
  bool scanned_ = false;
  bool scanning_ = false;
  bool systemDirectories_ = false;
};

}  // namespace solar::font
