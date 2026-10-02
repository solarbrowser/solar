#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "solar/url/Parser.h"
#include "solar/url/Serializer.h"
#include "solar/url/Utf8.h"

namespace {

struct Json {
  enum class Type { Null, Bool, String, Array, Object } type = Type::Null;
  bool boolean = false;
  std::string string;
  std::vector<Json> array;
  std::map<std::string, Json> object;

  const Json* Find(const std::string& key) const {
    auto it = object.find(key);
    return it == object.end() ? nullptr : &it->second;
  }
};

class JsonReader {
 public:
  explicit JsonReader(const std::string& text) : text_(text) {}

  Json Read() {
    SkipSpace();
    char c = text_.at(pos_);
    Json value;
    if (c == '{') {
      value.type = Json::Type::Object;
      ++pos_;
      SkipSpace();
      while (text_[pos_] != '}') {
        std::string key = ReadString();
        SkipSpace();
        ++pos_;  // ':'
        value.object[key] = Read();
        SkipSpace();
        if (text_[pos_] == ',') ++pos_;
        SkipSpace();
      }
      ++pos_;
    } else if (c == '[') {
      value.type = Json::Type::Array;
      ++pos_;
      SkipSpace();
      while (text_[pos_] != ']') {
        value.array.push_back(Read());
        SkipSpace();
        if (text_[pos_] == ',') ++pos_;
        SkipSpace();
      }
      ++pos_;
    } else if (c == '"') {
      value.type = Json::Type::String;
      value.string = ReadString();
    } else if (text_.compare(pos_, 4, "true") == 0) {
      value.type = Json::Type::Bool;
      value.boolean = true;
      pos_ += 4;
    } else if (text_.compare(pos_, 5, "false") == 0) {
      value.type = Json::Type::Bool;
      pos_ += 5;
    } else {
      pos_ += 4;  // null
    }
    return value;
  }

 private:
  void SkipSpace() {
    while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\n' || text_[pos_] == '\r' ||
                                   text_[pos_] == '\t')) {
      ++pos_;
    }
  }

  unsigned ReadHex4() {
    unsigned value = std::stoul(text_.substr(pos_, 4), nullptr, 16);
    pos_ += 4;
    return value;
  }

  // Test inputs hold lone surrogates, which the standard's callers turn into U+FFFD.
  std::string ReadString() {
    std::string out;
    ++pos_;  // opening quote
    while (text_[pos_] != '"') {
      char c = text_[pos_++];
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      char escape = text_[pos_++];
      switch (escape) {
        case 'n': out.push_back('\n'); break;
        case 't': out.push_back('\t'); break;
        case 'r': out.push_back('\r'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'u': {
          unsigned unit = ReadHex4();
          char32_t codePoint = unit;
          if (unit >= 0xD800 && unit <= 0xDBFF && text_.compare(pos_, 2, "\\u") == 0) {
            size_t saved = pos_;
            pos_ += 2;
            unsigned low = ReadHex4();
            if (low >= 0xDC00 && low <= 0xDFFF) {
              codePoint = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
            } else {
              pos_ = saved;
              codePoint = 0xFFFD;
            }
          } else if (unit >= 0xD800 && unit <= 0xDFFF) {
            codePoint = 0xFFFD;
          }
          solar::url::AppendUtf8(out, codePoint);
          break;
        }
        default: out.push_back(escape);
      }
    }
    ++pos_;
    return out;
  }

  const std::string& text_;
  size_t pos_ = 0;
};


std::optional<Json> Load(const std::string& path) {
  std::ifstream file(path);
  if (!file) {
    std::fprintf(stderr, "cannot open %s\n", path.c_str());
    return std::nullopt;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string text = buffer.str();
  return JsonReader(text).Read();
}

void Report(const char* name, int total, int failed) {
  std::printf("%s: %d/%d passed\n", name, total - failed, total);
}

int RunUrlTests(const Json& root) {
  int total = 0;
  int failed = 0;
  for (const Json& test : root.array) {
    if (test.type != Json::Type::Object) continue;
    ++total;

    const std::string& input = test.Find("input")->string;
    const Json* baseJson = test.Find("base");
    bool hasBase = baseJson && baseJson->type == Json::Type::String;
    std::optional<solar::url::Url> base;
    if (hasBase) base = solar::url::Parse(baseJson->string);

    std::optional<solar::url::Url> url;
    if (!hasBase || base) url = solar::url::Parse(input, base ? &*base : nullptr);

    const Json* failure = test.Find("failure");
    std::string want = failure && failure->boolean ? "<failure>" : test.Find("href")->string;
    std::string got = url ? solar::url::Serialize(*url) : "<failure>";

    if (got != want) {
      ++failed;
      std::printf("FAIL input=%s base=%s\n  want %s\n  got  %s\n", input.c_str(),
                  hasBase ? baseJson->string.c_str() : "null", want.c_str(), got.c_str());
    }
  }
  Report("urltestdata", total, failed);
  return failed;
}

// toascii.json and IdnaTestV2.json are both exercised the way WPT does it: as the host of
// https://<input>/x, with a null output meaning the whole URL fails to parse.
int RunHostTests(const char* name, const Json& root) {
  int total = 0;
  int failed = 0;
  for (const Json& test : root.array) {
    if (test.type != Json::Type::Object) continue;

    const std::string& input = test.Find("input")->string;
    // WPT skips this too: https:///x is a different parse, not an empty host.
    if (input.empty()) continue;
    // Assigned in Unicode 18.0, while WPT's expectations predate it and still disallow them.
    bool newInUnicode18 = false;
    for (char32_t c : solar::url::DecodeUtf8(input)) newInUnicode18 = newInUnicode18 || (c >= 0x3D000 && c <= 0x3FC3F);
    if (newInUnicode18) continue;

    ++total;
    const Json* output = test.Find("output");
    std::string want = output->type == Json::Type::String ? "https://" + output->string + "/x" : "<failure>";

    std::optional<solar::url::Url> url = solar::url::Parse("https://" + input + "/x");
    std::string got = url ? solar::url::Serialize(*url) : "<failure>";

    if (got != want) {
      ++failed;
      std::printf("FAIL %s input=%s\n  want %s\n  got  %s\n", name, input.c_str(), want.c_str(), got.c_str());
    }
  }
  Report(name, total, failed);
  return failed;
}

}  // namespace

int main(int argc, char** argv) {
  std::string dir = argc > 1 ? argv[1] : "tests/data";
  int failed = 0;

  std::optional<Json> urls = Load(dir + "/urltestdata.json");
  std::optional<Json> toAscii = Load(dir + "/toascii.json");
  std::optional<Json> idna = Load(dir + "/IdnaTestV2.json");
  if (!urls || !toAscii || !idna) return 2;

  failed += RunUrlTests(*urls);
  failed += RunHostTests("toascii", *toAscii);
  failed += RunHostTests("IdnaTestV2", *idna);
  return failed == 0 ? 0 : 1;
}
