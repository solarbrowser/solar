#include "solar/font/Database.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>

namespace solar::font {

namespace {

std::string Lower(std::string text) {
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

bool FontFileName(const std::filesystem::path& path) {
  std::string extension = Lower(path.extension().string());
  return extension == ".ttf" || extension == ".otf" || extension == ".ttc" || extension == ".otc" || extension == ".woff";
}

// The generic families as the platform's own fonts spell them, best first.
const std::map<std::string, std::vector<std::string>>& Generics() {
  static const std::map<std::string, std::vector<std::string>> generics = {
      {"serif", {"Noto Serif", "DejaVu Serif", "Liberation Serif", "Times New Roman", "Times", "Tinos", "Georgia", "Source Serif Pro", "FreeSerif"}},
      {"sans-serif", {"Noto Sans", "DejaVu Sans", "Liberation Sans", "Arial", "Helvetica", "Segoe UI", "Roboto", "Arimo", "Cantarell", "Verdana", "FreeSans"}},
      {"monospace", {"Noto Sans Mono", "DejaVu Sans Mono", "Liberation Mono", "Courier New", "Menlo", "Consolas", "Cousine", "Courier", "FreeMono"}},
      {"cursive", {"Comic Sans MS", "Apple Chancery", "Z003", "URW Chancery L", "Noto Sans", "DejaVu Sans"}},
      {"fantasy", {"Impact", "Papyrus", "Noto Sans", "DejaVu Sans"}},
      {"system-ui", {"Cantarell", "Segoe UI", "SF Pro Text", "Helvetica Neue", "Noto Sans", "DejaVu Sans", "Roboto", "Liberation Sans"}},
      {"ui-serif", {"New York", "Noto Serif", "DejaVu Serif", "Liberation Serif", "Times New Roman"}},
      {"ui-sans-serif", {"SF Pro", "Noto Sans", "DejaVu Sans", "Liberation Sans", "Segoe UI"}},
      {"ui-monospace", {"SF Mono", "Noto Sans Mono", "DejaVu Sans Mono", "Liberation Mono", "Consolas"}},
      {"ui-rounded", {"SF Pro Rounded", "Noto Sans", "DejaVu Sans"}},
      {"math", {"STIX Two Math", "Noto Sans Math", "Latin Modern Math", "Cambria Math", "DejaVu Sans"}},
      {"emoji", {"Noto Color Emoji", "Apple Color Emoji", "Segoe UI Emoji", "Twemoji Mozilla", "Noto Emoji"}},
      {"fangsong", {"FangSong", "Noto Serif CJK SC", "Noto Serif CJK JP", "Noto Serif"}},
  };
  return generics;
}


// What is known of a face without opening its file, kept between runs (one line a face, tab-separated) so that the fonts of a
// machine are not all opened every time.
struct Record {
  long long size = 0, modified = 0;
  unsigned index = 0;
  std::string family, legacyFamily, fullName, postScriptName;
  double weightMin = 400, weightMax = 400, widthMin = 100, widthMax = 100, obliqueMin = 14, obliqueMax = 14;
  int style = 0;
};

std::string CachePath() {
  std::filesystem::path base;
#if defined(_WIN32)
  if (const char* local = std::getenv("LOCALAPPDATA")) base = local;
#elif defined(__APPLE__)
  if (const char* home = std::getenv("HOME")) base = std::filesystem::path(home) / "Library" / "Caches";
#else
  if (const char* xdg = std::getenv("XDG_CACHE_HOME")) base = xdg;
  else if (const char* home = std::getenv("HOME")) base = std::filesystem::path(home) / ".cache";
#endif
  if (base.empty()) return "";
  return (base / "solar" / "fonts-v1.cache").string();
}

std::map<std::string, std::vector<Record>> ReadCache() {
  std::map<std::string, std::vector<Record>> cache;
  const std::string file = CachePath();
  if (file.empty()) return cache;
  std::ifstream in(file);
  std::string line;
  while (std::getline(in, line)) {
    std::vector<std::string> f;
    size_t start = 0;
    for (;;) {
      const size_t tab = line.find('\t', start);
      f.push_back(line.substr(start, tab == std::string::npos ? tab : tab - start));
      if (tab == std::string::npos) break;
      start = tab + 1;
    }
    if (f.size() != 16) continue;
    Record r;
    r.size = std::atoll(f[1].c_str());
    r.modified = std::atoll(f[2].c_str());
    r.index = static_cast<unsigned>(std::atoi(f[3].c_str()));
    r.family = f[4];
    r.legacyFamily = f[5];
    r.fullName = f[6];
    r.postScriptName = f[7];
    r.weightMin = std::atof(f[8].c_str());
    r.weightMax = std::atof(f[9].c_str());
    r.widthMin = std::atof(f[10].c_str());
    r.widthMax = std::atof(f[11].c_str());
    r.style = std::atoi(f[12].c_str());
    r.obliqueMin = std::atof(f[13].c_str());
    r.obliqueMax = std::atof(f[14].c_str());
    cache[f[0]].push_back(std::move(r));
  }
  return cache;
}

void WriteCache(const std::map<std::string, std::vector<Record>>& cache) {
  const std::string file = CachePath();
  if (file.empty()) return;
  std::error_code error;
  std::filesystem::create_directories(std::filesystem::path(file).parent_path(), error);
  const std::string temporary = file + ".tmp";
  {
    std::ofstream out(temporary, std::ios::trunc);
    if (!out) return;
    const auto clean = [](std::string text) {
      for (char& c : text) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
      return text;
    };
    for (const auto& [path, records] : cache) {
      for (const Record& r : records) {
        out << clean(path) << '\t' << r.size << '\t' << r.modified << '\t' << r.index << '\t' << clean(r.family) << '\t' << clean(r.legacyFamily) << '\t'
            << clean(r.fullName) << '\t' << clean(r.postScriptName) << '\t' << r.weightMin << '\t' << r.weightMax << '\t' << r.widthMin << '\t' << r.widthMax
            << '\t' << r.style << '\t' << r.obliqueMin << '\t' << r.obliqueMax << '\t' << "end\n";
      }
    }
  }
  std::filesystem::rename(temporary, file, error);
}

double Distance(double a, double b) { return std::fabs(a - b); }

// The value within [lo, hi] closest to `wanted`.
double Clamp(double wanted, double lo, double hi) { return std::max(lo, std::min(hi, wanted)); }

}  // namespace

const std::vector<std::string>& Database::GenericCandidates(const std::string& generic) {
  static const std::vector<std::string> none;
  const auto found = Generics().find(Lower(generic));
  return found == Generics().end() ? none : found->second;
}

Database& Database::System() {
  static Database* database = [] {
    Database* d = new Database();
    d->systemDirectories_ = true;
    return d;
  }();
  return *database;
}

void Database::Add(Source source) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto entry = std::make_unique<Entry>();
  entry->source = std::move(source);
  Entry* raw = entry.get();
  byFamily_.emplace(Lower(raw->source.family), raw);
  entries_.push_back(std::move(entry));
}

void Database::AddDirectory(const std::string& directory) {
  std::lock_guard<std::mutex> lock(mutex_);
  directories_.push_back(directory);
  scanned_ = false;
}

void Database::EnsureScanned() {
  if (scanned_ || scanning_) return;
  scanning_ = true;
  std::vector<std::string> roots = directories_;
  if (systemDirectories_) {
#if defined(__APPLE__)
    roots.insert(roots.end(), {"/System/Library/Fonts", "/Library/Fonts", "/System/Library/Fonts/Supplemental"});
    if (const char* home = std::getenv("HOME")) roots.push_back(std::string(home) + "/Library/Fonts");
#elif defined(_WIN32)
    if (const char* windir = std::getenv("WINDIR")) roots.push_back(std::string(windir) + "\\Fonts");
    if (const char* local = std::getenv("LOCALAPPDATA")) roots.push_back(std::string(local) + "\\Microsoft\\Windows\\Fonts");
#else
    roots.insert(roots.end(), {"/usr/share/fonts", "/usr/local/share/fonts"});
    if (const char* home = std::getenv("HOME")) {
      roots.push_back(std::string(home) + "/.fonts");
      roots.push_back(std::string(home) + "/.local/share/fonts");
    }
    if (const char* data = std::getenv("XDG_DATA_HOME")) roots.push_back(std::string(data) + "/fonts");
#endif
    systemDirectories_ = false;  // once
    directories_.insert(directories_.end(), roots.begin(), roots.end());
  }
  std::set<std::string> done;
  std::map<std::string, std::vector<Record>> cache = ReadCache();
  std::map<std::string, std::vector<Record>> fresh;
  bool changed = false;
  for (const std::string& root : roots) {
    std::error_code error;
    if (!std::filesystem::exists(root, error)) continue;
    for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, error), end; !error && it != end; it.increment(error)) {
      if (!it->is_regular_file(error) || !FontFileName(it->path())) continue;
      const std::string path = it->path().string();
      if (!done.insert(path).second) continue;
      std::error_code stat;
      const long long size = static_cast<long long>(it->file_size(stat));
      const long long modified = static_cast<long long>(it->last_write_time(stat).time_since_epoch().count());
      std::vector<Record> records;
      const auto known = cache.find(path);
      if (known != cache.end() && !known->second.empty() && known->second[0].size == size && known->second[0].modified == modified) {
        records = known->second;
      } else {
        changed = true;
        // The names of the faces in the file; a collection is opened face by face until one is not.
        const std::string extension = Lower(it->path().extension().string());
        const bool collection = extension == ".ttc" || extension == ".otc";
        for (unsigned index = 0; index < (collection ? 64u : 1u); ++index) {
          std::shared_ptr<Face> face = Face::OpenFile(path, index);
          if (!face) break;
          const Description& d = face->description();
          Record r;
          r.size = size;
          r.modified = modified;
          r.index = index;
          r.family = d.family;
          r.legacyFamily = d.legacyFamily;
          r.fullName = d.fullName;
          r.postScriptName = d.postScriptName;
          r.weightMin = d.weightMin;
          r.weightMax = d.weightMax;
          r.widthMin = d.widthMin;
          r.widthMax = d.widthMax;
          r.style = d.italic ? 1 : d.slant != 0 ? 2 : 0;
          r.obliqueMin = d.slantMin;
          r.obliqueMax = d.slantMax;
          records.push_back(std::move(r));
        }
      }
      fresh[path] = records;
      for (const Record& r : records) {
        const unsigned index = r.index;
        const auto add = [&](const std::string& family) {
          if (family.empty()) return;
          auto entry = std::make_unique<Entry>();
          entry->source.family = family;
          Coverage& c = entry->source.coverage;
          c.weightMin = r.weightMin;
          c.weightMax = r.weightMax;
          c.widthMin = r.widthMin;
          c.widthMax = r.widthMax;
          c.style = r.style == 1 ? SlantStyle::Italic : r.style == 2 ? SlantStyle::Oblique : SlantStyle::Normal;
          c.obliqueMin = r.obliqueMin;
          c.obliqueMax = r.obliqueMax;
          entry->source.load = [path, index] { return Face::OpenFile(path, index); };
          entry->fullName = r.fullName;
          entry->postScriptName = r.postScriptName;
          Entry* raw = entry.get();
          byFamily_.emplace(Lower(family), raw);
          entries_.push_back(std::move(entry));
        };
        add(r.family);
        if (r.legacyFamily != r.family) add(r.legacyFamily);
      }
    }
  }
  if (changed || fresh.size() != cache.size()) WriteCache(fresh);
  scanned_ = true;
  scanning_ = false;
}

std::shared_ptr<Face> Database::FaceOf(Entry& entry) {
  if (!entry.loaded) {
    entry.face = entry.source.load ? entry.source.load() : nullptr;
    entry.loaded = true;
  }
  return entry.face;
}

std::vector<Database::Entry*> Database::InFamily(const std::string& family) {
  std::vector<Entry*> out;
  const auto range = byFamily_.equal_range(Lower(family));
  for (auto it = range.first; it != range.second; ++it) out.push_back(it->second);
  return out;
}

std::shared_ptr<Face> Database::BestIn(const std::string& family, const Request& request) {
  std::vector<Entry*> candidates = InFamily(family);
  if (candidates.empty()) return nullptr;
  // 1. The width: narrower first when wanting normal or narrower, else wider first.
  {
    double best = 1e9;
    std::vector<Entry*> kept;
    const auto score = [&](const Entry* e) {
      const Coverage& c = e->source.coverage;
      if (request.width >= c.widthMin && request.width <= c.widthMax) return 0.0;
      const double edge = request.width < c.widthMin ? c.widthMin : c.widthMax;
      const double gap = Distance(request.width, edge);
      const bool preferred = request.width <= 100 ? edge < request.width : edge > request.width;
      return preferred ? gap : 1000 + gap;
    };
    for (Entry* e : candidates) best = std::min(best, score(e));
    for (Entry* e : candidates) if (score(e) == best) kept.push_back(e);
    candidates = kept;
  }
  // 2. The style.
  {
    const auto rank = [&](const Entry* e) {
      const SlantStyle s = e->source.coverage.style;
      switch (request.style) {
        case SlantStyle::Normal: return s == SlantStyle::Normal ? 0 : s == SlantStyle::Oblique ? 1 : 2;
        case SlantStyle::Italic: return s == SlantStyle::Italic ? 0 : s == SlantStyle::Oblique ? 1 : 2;
        case SlantStyle::Oblique: return s == SlantStyle::Oblique ? 0 : s == SlantStyle::Italic ? 1 : 2;
      }
      return 3;
    };
    int best = 99;
    for (Entry* e : candidates) best = std::min(best, rank(e));
    std::vector<Entry*> kept;
    for (Entry* e : candidates) if (rank(e) == best) kept.push_back(e);
    candidates = kept;
    if (request.style == SlantStyle::Oblique && candidates.size() > 1) {
      // the angle closest, bigger preferred when it is 11 degrees or more
      const auto angleScore = [&](const Entry* e) {
        const Coverage& c = e->source.coverage;
        const double angle = Clamp(request.obliqueAngle, c.obliqueMin, c.obliqueMax);
        return Distance(angle, request.obliqueAngle);
      };
      double bestAngle = 1e9;
      for (Entry* e : candidates) bestAngle = std::min(bestAngle, angleScore(e));
      kept.clear();
      for (Entry* e : candidates) if (angleScore(e) == bestAngle) kept.push_back(e);
      candidates = kept;
    }
  }
  // 3. The weight.
  Entry* chosen = nullptr;
  {
    const double wanted = request.weight;
    const auto covers = [&](const Entry* e, double w) { return w >= e->source.coverage.weightMin && w <= e->source.coverage.weightMax; };
    const auto pick = [&](auto&& better, double from) -> Entry* {
      Entry* result = nullptr;
      double bestDistance = 1e9;
      for (Entry* e : candidates) {
        const Coverage& c = e->source.coverage;
        // the weight the font gives that is nearest `from`, going the way `better` says
        double w = Clamp(from, c.weightMin, c.weightMax);
        if (!better(w, from)) continue;
        const double distance = Distance(w, from);
        if (distance < bestDistance) {
          bestDistance = distance;
          result = e;
        }
      }
      return result;
    };
    const auto below = [](double w, double from) { return w <= from; };
    const auto above = [](double w, double from) { return w >= from; };
    for (Entry* e : candidates) {
      if (covers(e, wanted)) {
        chosen = e;
        break;
      }
    }
    if (!chosen) {
      if (wanted >= 400 && wanted <= 500) {
        chosen = pick(above, wanted);
        if (chosen && Clamp(wanted, chosen->source.coverage.weightMin, chosen->source.coverage.weightMax) > 500) chosen = nullptr;
        if (!chosen) chosen = pick(below, wanted);
        if (!chosen) chosen = pick(above, wanted);
        if (!chosen && wanted == 400) chosen = nullptr;
      } else if (wanted < 400) {
        chosen = pick(below, wanted);
        if (!chosen) chosen = pick(above, wanted);
      } else {
        chosen = pick(above, wanted);
        if (!chosen) chosen = pick(below, wanted);
      }
    }
    if (!chosen) chosen = candidates.front();
  }
  std::shared_ptr<Face> face = FaceOf(*chosen);
  if (!face) return nullptr;
  if (face->description().variable) {
    const Coverage& c = chosen->source.coverage;
    std::vector<Face::Axis> axes;
    axes.push_back({"wght", Clamp(request.weight, face->description().weightMin, face->description().weightMax)});
    if (face->description().widthMax > face->description().widthMin) axes.push_back({"wdth", Clamp(request.width, face->description().widthMin, face->description().widthMax)});
    if (request.style == SlantStyle::Oblique && face->description().slantMax > face->description().slantMin) axes.push_back({"slnt", -Clamp(request.obliqueAngle, face->description().slantMin, face->description().slantMax)});
    (void)c;
    return face->WithVariations(axes);
  }
  return face;
}

std::shared_ptr<Face> Database::Match(const Request& request) {
  std::lock_guard<std::mutex> lock(mutex_);
  EnsureScanned();
  for (const std::string& family : request.families) {
    const std::string lower = Lower(family);
    const auto generic = Generics().find(lower);
    if (generic != Generics().end()) {
      for (const std::string& candidate : generic->second) {
        if (std::shared_ptr<Face> face = BestIn(candidate, request)) return face;
      }
      continue;
    }
    if (std::shared_ptr<Face> face = BestIn(family, request)) return face;
  }
  return nullptr;
}

std::vector<std::shared_ptr<Face>> Database::MatchAll(const Request& request) {
  std::lock_guard<std::mutex> lock(mutex_);
  EnsureScanned();
  std::vector<std::shared_ptr<Face>> faces;
  const auto add = [&](const std::shared_ptr<Face>& face) {
    if (face && std::find(faces.begin(), faces.end(), face) == faces.end()) faces.push_back(face);
  };
  for (const std::string& family : request.families) {
    const std::string lower = Lower(family);
    const auto generic = Generics().find(lower);
    if (generic != Generics().end()) {
      for (const std::string& candidate : generic->second) {
        if (std::shared_ptr<Face> face = BestIn(candidate, request)) {
          add(face);
          break;
        }
      }
      continue;
    }
    add(BestIn(family, request));
  }
  return faces;
}

std::shared_ptr<Face> Database::Fallback(char32_t codePoint, const Request& request) {
  std::lock_guard<std::mutex> lock(mutex_);
  EnsureScanned();
  // The generic sans-serif and serif first, then any font with the character.
  for (const char* generic : {"sans-serif", "serif", "system-ui"}) {
    for (const std::string& candidate : Generics().at(generic)) {
      std::shared_ptr<Face> face = BestIn(candidate, request);
      if (face && face->HasGlyph(codePoint)) return face;
    }
  }
  for (const auto& entry : entries_) {
    std::shared_ptr<Face> face = FaceOf(*entry);
    if (face && face->HasGlyph(codePoint)) return face;
  }
  return nullptr;
}

std::shared_ptr<Face> Database::FindLocal(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  EnsureScanned();
  const std::string wanted = Lower(name);
  for (const auto& entry : entries_) {
    if (Lower(entry->fullName) == wanted || Lower(entry->postScriptName) == wanted) {
      if (std::shared_ptr<Face> face = FaceOf(*entry)) return face;
    }
  }
  return nullptr;
}

size_t Database::Count() {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.size();
}

int Database::State(size_t index) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (index >= entries_.size() || !entries_[index]->loaded) return 0;
  return entries_[index]->face ? 1 : 2;
}

bool Database::Load(size_t index) {
  std::lock_guard<std::mutex> lock(mutex_);
  return index < entries_.size() && FaceOf(*entries_[index]) != nullptr;
}

std::vector<std::string> Database::Families() {
  std::lock_guard<std::mutex> lock(mutex_);
  EnsureScanned();
  std::set<std::string> names;
  for (const auto& entry : entries_) names.insert(entry->source.family);
  return {names.begin(), names.end()};
}

}  // namespace solar::font
