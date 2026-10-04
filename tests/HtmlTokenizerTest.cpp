// Runs the html5lib tokenizer tests (tests/html5lib/tokenizer/*.test) through the tokenizer.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "support/Json.h"
#include "solar/html/Tokenizer.h"
#include "solar/url/Utf8.h"

namespace {

using solar::html::Token;
using solar::html::Tokenizer;
using solar::test::Json;

// "doubleEscaped" tests spell their text with \uXXXX sequences a second time, to be able to say a lone
// surrogate; the tokenizer takes UTF-8, which cannot, so those become U+FFFD on both sides.
std::string Unescape(const std::string& text) {
  std::string out;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\\' && i + 5 < text.size() + 0 && text[i + 1] == 'u') {
      unsigned unit = std::stoul(text.substr(i + 2, 4), nullptr, 16);
      i += 5;
      char32_t point = unit;
      if (unit >= 0xD800 && unit <= 0xDBFF && text.compare(i + 1, 2, "\\u") == 0) {
        const unsigned low = std::stoul(text.substr(i + 3, 4), nullptr, 16);
        if (low >= 0xDC00 && low <= 0xDFFF) {
          point = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
          i += 6;
        } else {
          point = 0xFFFD;
        }
      } else if (unit >= 0xD800 && unit <= 0xDFFF) {
        point = 0xFFFD;
      }
      solar::url::AppendUtf8(out, point);
    } else {
      out.push_back(text[i]);
    }
  }
  return out;
}

std::string Quote(const std::string& text) { return "\"" + text + "\""; }

// A token (or an expected one) as one line, to compare.
std::string Describe(const std::vector<std::string>& parts) {
  std::string out;
  for (const std::string& part : parts) out += part + "|";
  return out;
}

std::vector<std::string> Actual(Tokenizer& tokenizer) {
  std::vector<std::string> lines;
  std::string characters;
  const auto flush = [&] {
    if (!characters.empty()) lines.push_back(Describe({"Character", Quote(characters)}));
    characters.clear();
  };
  for (;;) {
    Token token = tokenizer.Next();
    if (token.type == Token::Type::Character) {
      characters += token.data;
      continue;
    }
    flush();
    if (token.type == Token::Type::EndOfFile) break;
    switch (token.type) {
      case Token::Type::StartTag: {
        std::vector<std::pair<std::string, std::string>> attributes;
        for (const auto& attribute : token.attributes) attributes.emplace_back(attribute.name, attribute.value);
        std::sort(attributes.begin(), attributes.end());
        std::string list;
        for (const auto& [name, value] : attributes) list += Quote(name) + "=" + Quote(value) + ",";
        lines.push_back(Describe({"StartTag", token.name, list, token.selfClosing ? "selfclosing" : ""}));
        break;
      }
      case Token::Type::EndTag: lines.push_back(Describe({"EndTag", token.name})); break;
      case Token::Type::Comment: lines.push_back(Describe({"Comment", Quote(token.data)})); break;
      case Token::Type::ProcessingInstruction: lines.push_back(Describe({"ProcessingInstruction", token.name, Quote(token.data)})); break;
      case Token::Type::Doctype:
        lines.push_back(Describe({"DOCTYPE", token.doctypeName ? Quote(*token.doctypeName) : "null", token.publicId ? Quote(*token.publicId) : "null",
                                  token.systemId ? Quote(*token.systemId) : "null", token.forceQuirks ? "quirks" : "correct"}));
        break;
      default: break;
    }
  }
  return lines;
}

std::vector<std::string> Expected(const Json& output, bool doubleEscaped) {
  const auto text = [&](const Json& value) { return doubleEscaped ? Unescape(value.string) : value.string; };
  std::vector<std::string> lines;
  std::string characters;
  const auto flush = [&] {
    if (!characters.empty()) lines.push_back(Describe({"Character", Quote(characters)}));
    characters.clear();
  };
  for (const Json& token : output.array) {
    const std::string& kind = token.array[0].string;
    if (kind == "Character") {
      characters += text(token.array[1]);
      continue;
    }
    flush();
    if (kind == "StartTag") {
      std::string list;
      for (const auto& [name, value] : token.array[2].object) list += Quote(doubleEscaped ? Unescape(name) : name) + "=" + Quote(text(value)) + ",";
      const bool selfClosing = token.array.size() > 3 && token.array[3].boolean;
      lines.push_back(Describe({"StartTag", text(token.array[1]), list, selfClosing ? "selfclosing" : ""}));
    } else if (kind == "EndTag") {
      lines.push_back(Describe({"EndTag", text(token.array[1])}));
    } else if (kind == "ProcessingInstruction") {
      lines.push_back(Describe({"ProcessingInstruction", text(token.array[1]), Quote(text(token.array[2]))}));
    } else if (kind == "Comment") {
      lines.push_back(Describe({"Comment", Quote(text(token.array[1]))}));
    } else if (kind == "DOCTYPE") {
      const auto field = [&](size_t i) { return token.array[i].type == Json::Type::Null ? std::string("null") : Quote(text(token.array[i])); };
      lines.push_back(Describe({"DOCTYPE", field(1), field(2), field(3), token.array[4].boolean ? "correct" : "quirks"}));
    }
  }
  flush();
  return lines;
}

Tokenizer::State StateNamed(const std::string& name) {
  if (name == "Data state") return Tokenizer::State::Data;
  if (name == "PLAINTEXT state") return Tokenizer::State::Plaintext;
  if (name == "RCDATA state") return Tokenizer::State::Rcdata;
  if (name == "RAWTEXT state") return Tokenizer::State::Rawtext;
  if (name == "Script data state") return Tokenizer::State::ScriptData;
  if (name == "CDATA section state") return Tokenizer::State::CdataSection;
  std::fprintf(stderr, "unknown state %s\n", name.c_str());
  std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
  // With no arguments, all the files of the html5lib tokenizer tests.
  std::vector<std::string> files(argv + 1, argv + argc);
  if (files.empty()) {
    for (const auto& entry : std::filesystem::directory_iterator("tests/html5lib/tokenizer")) {
      if (entry.path().extension() == ".test") files.push_back(entry.path().string());
    }
    std::sort(files.begin(), files.end());
  }
  int passed = 0, failed = 0;
  for (size_t i = 0; i < files.size(); ++i) {
    const auto file = solar::test::Load(files[i].c_str());
    if (!file) {
      std::fprintf(stderr, "cannot read %s\n", files[i].c_str());
      return 2;
    }
    // xmlViolationTests are for coercing to an infoset, which the standard no longer asks the tokenizer for.
    const Json* tests = file->Find("tests");
    if (!tests) continue;
    for (const Json& test : tests->array) {
      const bool doubleEscaped = test.Find("doubleEscaped") && test.Find("doubleEscaped")->boolean;
      // A lone surrogate in the input cannot be said in UTF-8: the test of it is for another kind of input.
      if (test.Find("errors") && test.Find("errors")->type == Json::Type::Array) {
        bool needsSurrogate = false;
        for (const Json& error : test.Find("errors")->array) needsSurrogate |= error.type == Json::Type::Object && error.Find("code")->string == "surrogate-in-input-stream";
        if (needsSurrogate) continue;
      }
      // domjs.test still has the tests of how "<?" was a bogus comment before processing instructions.
      if (files[i].ends_with("domjs.test") && test.Find("description")->string.find("in bogus comment state") != std::string::npos) continue;
      std::string input = test.Find("input")->string;
      if (doubleEscaped) input = Unescape(input);
      std::vector<std::string> states = {"Data state"};
      if (const Json* initial = test.Find("initialStates")) {
        states.clear();
        for (const Json& state : initial->array) states.push_back(state.string);
      }
      for (const std::string& stateName : states) {
        Tokenizer tokenizer(input);
        tokenizer.SetState(StateNamed(stateName));
        if (const Json* last = test.Find("lastStartTag")) tokenizer.SetLastStartTag(last->string);
        if (stateName == "CDATA section state") tokenizer.SetCdataAllowed(true);
        const std::vector<std::string> actual = Actual(tokenizer);
        const std::vector<std::string> expected = Expected(*test.Find("output"), doubleEscaped);

        std::vector<std::string> expectedErrors;
        if (const Json* errors = test.Find("errors")) {
          for (const Json& error : errors->array) expectedErrors.push_back(error.type == Json::Type::Object ? error.Find("code")->string : error.string);
        }
        bool ok = actual == expected;
        // Only the new format names its errors; the old one is a bare text, which is not compared.
        const bool namedErrors = !expectedErrors.empty() ? expectedErrors[0].find(' ') == std::string::npos : true;
        if (namedErrors && tokenizer.errors() != expectedErrors) ok = false;
        if (ok) {
          ++passed;
        } else {
          ++failed;
          std::printf("FAIL %s: %s [%s]\n  input: %s\n", files[i].c_str(), test.Find("description")->string.c_str(), stateName.c_str(), input.c_str());
          if (actual != expected) {
            for (const std::string& line : expected) std::printf("  expected  %s\n", line.c_str());
            for (const std::string& line : actual) std::printf("  actual    %s\n", line.c_str());
          }
          if (namedErrors && tokenizer.errors() != expectedErrors) {
            std::string a, e;
            for (const std::string& error : tokenizer.errors()) a += error + " ";
            for (const std::string& error : expectedErrors) e += error + " ";
            std::printf("  errors expected: %s\n  errors actual:   %s\n", e.c_str(), a.c_str());
          }
        }
      }
    }
  }
  std::printf("%d/%d passed\n", passed, passed + failed);
  return failed == 0 ? 0 : 1;
}
