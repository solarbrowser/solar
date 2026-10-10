#include "solar/css/Fonts.h"

#include <cmath>
#include <map>
#include <set>

#include "solar/css/Cssom.h"
#include "solar/css/Style.h"
#include "solar/css/Syntax.h"
#include "solar/url/Parser.h"
#include "solar/url/Serializer.h"

namespace solar::css {

namespace {

using T = Token::Type;

std::string Lower(std::string text) {
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

// A list of family names written as a value: unquoted names joined by spaces, strings as they are.
std::vector<std::string> FamilyNames(const std::string& text) {
  std::vector<std::string> names;
  for (const ComponentValues& part : SplitOnCommas(Trimmed(ParseComponentValues(text)))) {
    std::string name;
    for (const ComponentValue& v : Trimmed(part)) {
      if (v.IsToken(T::String)) {
        name = v.token.value;
        break;
      }
      if (v.IsIdent()) name += (name.empty() ? "" : " ") + v.token.value;
    }
    if (!name.empty()) names.push_back(name);
  }
  return names;
}

double NumberOf(const std::string& text, double fallback) {
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  return end == text.c_str() ? fallback : value;
}

// ---- The descriptors of an @font-face rule ----

struct FaceRule {
  std::string family;
  std::string src;
  std::string weight, style, stretch, unicodeRange;
  std::string base;
  std::shared_ptr<font::Face> face;  // a script's font, already made
  FaceRuleInfo info;
};

std::map<dom::Document*, std::vector<ScriptFace>>& ScriptFaces() {
  static std::map<dom::Document*, std::vector<ScriptFace>> faces;
  return faces;
}
uint64_t g_scriptVersion = 0;

void CollectFaceRules(const std::vector<CssRule*>& rules, const std::string& base, std::vector<FaceRule>& out, int depth) {
  if (depth > 8) return;
  for (const CssRule* rule : rules) {
    switch (rule->kind) {
      case RuleKind::FontFace: {
        if (!rule->style) break;
        FaceRule f;
        const auto get = [&](const char* name) {
          const DeclarationEntry* entry = rule->style->Find(name);
          return entry ? entry->value : std::string();
        };
        f.family = get("font-family");
        f.src = get("src");
        f.weight = get("font-weight");
        f.style = get("font-style");
        f.stretch = get("font-stretch");
        f.unicodeRange = get("unicode-range");
        f.base = base;
        f.info = {f.family, f.src, f.weight, f.style, f.stretch, f.unicodeRange, get("font-variant"), get("font-feature-settings"), get("font-variation-settings"), get("font-display"), get("ascent-override"), get("descent-override"), get("line-gap-override")};
        if (!f.family.empty() && !f.src.empty()) out.push_back(std::move(f));
        break;
      }
      case RuleKind::Media:
      case RuleKind::Supports:
      case RuleKind::LayerBlock:
        CollectFaceRules(rule->rules, base, out, depth + 1);
        break;
      case RuleKind::Import:
        if (rule->importedSheet) CollectFaceRules(rule->importedSheet->rules, rule->importedSheet->baseUrl, out, depth + 1);
        break;
      default: break;
    }
  }
}

double WeightKeyword(const std::string& word, double fallback) {
  if (word == "normal") return 400;
  if (word == "bold") return 700;
  return NumberOf(word, fallback);
}

std::vector<std::string> Words(const std::string& text) {
  std::vector<std::string> words;
  std::string current;
  for (char c : text) {
    if (c == ' ' || c == '\t' || c == '\n') {
      if (!current.empty()) words.push_back(current);
      current.clear();
    } else {
      current += c;
    }
  }
  if (!current.empty()) words.push_back(current);
  return words;
}

font::Coverage CoverageOf(const FaceRule& rule) {
  font::Coverage c;
  // weight: auto, a keyword, a number, or two of them
  {
    const std::vector<std::string> words = Words(Lower(rule.weight));
    if (words.empty() || words[0] == "auto") {
      c.weightMin = 1;
      c.weightMax = 1000;
    } else {
      c.weightMin = WeightKeyword(words[0], 400);
      c.weightMax = words.size() > 1 ? WeightKeyword(words[1], c.weightMin) : c.weightMin;
    }
  }
  {
    static const std::map<std::string, double> keywords = {{"ultra-condensed", 50}, {"extra-condensed", 62.5}, {"condensed", 75}, {"semi-condensed", 87.5}, {"normal", 100},
                                                           {"semi-expanded", 112.5}, {"expanded", 125}, {"extra-expanded", 150}, {"ultra-expanded", 200}};
    const std::vector<std::string> words = Words(Lower(rule.stretch));
    const auto value = [&](const std::string& word) {
      const auto found = keywords.find(word);
      return found != keywords.end() ? found->second : NumberOf(word, 100);
    };
    if (words.empty() || words[0] == "auto") {
      c.widthMin = 50;
      c.widthMax = 200;
    } else {
      c.widthMin = value(words[0]);
      c.widthMax = words.size() > 1 ? value(words[1]) : c.widthMin;
    }
  }
  {
    const std::vector<std::string> words = Words(Lower(rule.style));
    if (words.empty() || words[0] == "auto" || words[0] == "normal") {
      c.style = font::SlantStyle::Normal;
      if (!words.empty() && words[0] == "auto") c.style = font::SlantStyle::Normal;
    } else if (words[0] == "italic") {
      c.style = font::SlantStyle::Italic;
    } else {
      c.style = font::SlantStyle::Oblique;
      c.obliqueMin = words.size() > 1 ? NumberOf(words[1], 14) : 14;
      c.obliqueMax = words.size() > 2 ? NumberOf(words[2], c.obliqueMin) : c.obliqueMin;
    }
  }
  // unicode-range: U+26, U+0-7F, U+4??
  for (const std::string& part : [&] {
         std::vector<std::string> parts;
         std::string current;
         for (char ch : rule.unicodeRange) {
           if (ch == ',') {
             parts.push_back(current);
             current.clear();
           } else if (ch != ' ') {
             current += ch;
           }
         }
         if (!current.empty()) parts.push_back(current);
         return parts;
       }()) {
    if (part.size() < 3) continue;
    const std::string body = part.substr(2);
    const size_t dash = body.find('-');
    const auto hex = [](const std::string& text, char fill) {
      std::string filled = text;
      for (char& ch : filled) {
        if (ch == '?') ch = fill;
      }
      return static_cast<char32_t>(std::strtoul(filled.c_str(), nullptr, 16));
    };
    if (dash != std::string::npos) c.unicodeRanges.push_back({hex(body.substr(0, dash), '0'), hex(body.substr(dash + 1), 'F')});
    else c.unicodeRanges.push_back({hex(body, '0'), hex(body, 'F')});
  }
  return c;
}

// The sources of a src descriptor: loaded in order until one gives a font.
std::function<std::shared_ptr<font::Face>()> LoaderOf(const FaceRule& rule) {
  struct Source {
    bool local;
    std::string name;  // a local font's name, or a url's address
  };
  std::vector<Source> sources;
  for (const ComponentValues& part : SplitOnCommas(Trimmed(ParseComponentValues(rule.src)))) {
    const ComponentValues items = Trimmed(part);
    if (items.empty()) continue;
    const ComponentValue& first = items[0];
    if (first.kind == ComponentValue::Kind::Function && Lower(first.name) == "local") {
      std::string name;
      for (const ComponentValue& c : first.children) {
        if (c.IsToken(T::String)) name = c.token.value;
        else if (c.IsIdent()) name += (name.empty() ? "" : " ") + c.token.value;
      }
      sources.push_back({true, name});
    } else {
      std::string address;
      if (first.IsToken(T::Url)) address = first.token.value;
      else if (first.kind == ComponentValue::Kind::Function) {
        for (const ComponentValue& c : first.children) if (c.IsToken(T::String)) address = c.token.value;
      }
      if (address.empty()) continue;
      const std::optional<url::Url> base = url::Parse(rule.base);
      const std::optional<url::Url> parsed = url::Parse(address, base ? &*base : nullptr);
      if (parsed) sources.push_back({false, url::Serialize(*parsed)});
    }
  }
  return [sources]() -> std::shared_ptr<font::Face> {
    for (const Source& source : sources) {
      if (source.local) {
        if (std::shared_ptr<font::Face> face = font::Database::System().FindLocal(source.name)) return face;
        continue;
      }
      const SheetLoader& load = GetSheetLoader();
      if (!load) continue;
      if (std::optional<std::string> data = load(source.name)) {
        if (std::shared_ptr<font::Face> face = font::Face::Open(std::move(*data))) return face;
      }
    }
    return nullptr;
  };
}

struct DocumentEntry {
  uint64_t version = 0;
  std::string url;
  std::unique_ptr<font::Database> database;
  size_t ruleCount = 0;
};

std::map<dom::Document*, DocumentEntry>& Cache() {
  static std::map<dom::Document*, DocumentEntry> cache;
  return cache;
}

}  // namespace

font::Database& DocumentFonts(dom::Document* document) {
  DocumentEntry& entry = Cache()[document];
  const uint64_t version = (StyleVersion() * 1000003 + dom::TreeVersion()) * 1000003 + g_scriptVersion;
  if (entry.database && entry.version == version && entry.url == document->url) return *entry.database;
  entry.version = version;
  entry.url = document->url;
  entry.database = std::make_unique<font::Database>();
  std::vector<FaceRule> rules;
  for (const CssStyleSheet* sheet : SheetsOfTreeRoot(document)) {
    if (!sheet->disabled) CollectFaceRules(sheet->rules, sheet->baseUrl, rules, 0);
  }
  for (const FaceRule& rule : rules) {
    const std::vector<std::string> names = FamilyNames(rule.family);
    if (names.empty()) continue;
    entry.database->Add({names[0], CoverageOf(rule), LoaderOf(rule)});
  }
  entry.ruleCount = entry.database->Count();
  for (const ScriptFace& script : ScriptFaces()[document]) {
    if (!script.face) continue;
    FaceRule rule;
    rule.weight = script.weight;
    rule.style = script.style;
    rule.stretch = script.stretch;
    rule.unicodeRange = script.unicodeRange;
    const std::shared_ptr<font::Face> face = script.face;
    entry.database->Add({script.family, CoverageOf(rule), [face] { return face; }});
  }
  return *entry.database;
}

font::Request FontRequestFor(dom::Element* element, const std::string& pseudo) {
  font::Request request;
  Quanta::Context* context = element->nodeDocument ? element->nodeDocument->context : nullptr;
  if (!context) return request;
  request.families = FamilyNames(ComputedValue(*context, element, "font-family", pseudo));
  if (request.families.empty()) request.families = {"serif"};
  request.weight = NumberOf(ComputedValue(*context, element, "font-weight", pseudo), 400);
  request.width = NumberOf(ComputedValue(*context, element, "font-width", pseudo), 100);
  const std::vector<std::string> style = Words(ComputedValue(*context, element, "font-style", pseudo));
  if (!style.empty() && style[0] == "italic") request.style = font::SlantStyle::Italic;
  else if (!style.empty() && style[0] == "oblique") {
    request.style = font::SlantStyle::Oblique;
    request.obliqueAngle = style.size() > 1 ? NumberOf(style[1], 14) : 14;
  }
  return request;
}

std::vector<std::shared_ptr<font::Face>> FontsFor(dom::Element* element, const std::string& pseudo) {
  std::vector<std::shared_ptr<font::Face>> faces;
  const font::Request request = FontRequestFor(element, pseudo);
  font::Database& web = DocumentFonts(element->nodeDocument);
  font::Database& system = font::Database::System();
  for (const std::string& family : request.families) {
    font::Request one = request;
    one.families = {family};
    std::shared_ptr<font::Face> face = web.Match(one);
    if (!face) face = system.Match(one);
    if (face && std::find(faces.begin(), faces.end(), face) == faces.end()) faces.push_back(face);
  }
  if (faces.empty()) {
    font::Request fallback = request;
    fallback.families = {"serif", "sans-serif"};
    if (std::shared_ptr<font::Face> face = system.Match(fallback)) faces.push_back(face);
  }
  return faces;
}

std::shared_ptr<font::Face> PrimaryFont(dom::Element* element, const std::string& pseudo) {
  const std::vector<std::shared_ptr<font::Face>> faces = FontsFor(element, pseudo);
  return faces.empty() ? nullptr : faces[0];
}

std::shared_ptr<font::Face> FallbackFont(dom::Element* element, char32_t codePoint, const std::string& pseudo) {
  return font::Database::System().Fallback(codePoint, FontRequestFor(element, pseudo));
}

}  // namespace solar::css

namespace solar::css {

std::vector<FaceRuleInfo> DocumentFaceRules(dom::Document* document) {
  std::vector<FaceRule> rules;
  for (const CssStyleSheet* sheet : SheetsOfTreeRoot(document)) {
    if (!sheet->disabled) CollectFaceRules(sheet->rules, sheet->baseUrl, rules, 0);
  }
  std::vector<FaceRuleInfo> out;
  for (const FaceRule& rule : rules) {
    if (!FamilyNames(rule.family).empty()) out.push_back(rule.info);
  }
  return out;
}

bool LoadDocumentFace(dom::Document* document, size_t index) {
  font::Database& database = DocumentFonts(document);
  return index < Cache()[document].ruleCount && database.Load(index);
}

int DocumentFaceState(dom::Document* document, size_t index) {
  font::Database& database = DocumentFonts(document);
  return index < Cache()[document].ruleCount ? database.State(index) : 0;
}

void SetScriptFaces(dom::Document* document, std::vector<ScriptFace> faces) {
  ScriptFaces()[document] = std::move(faces);
  ++g_scriptVersion;
}

}  // namespace solar::css
