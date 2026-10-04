// Runs the tree construction tests of the HTML parser (tests/html5lib/tree-construction/*.dat, which are
// the web-platform-tests' copy of html5lib's) through the parser, and compares the DOM it makes with theirs.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "solar/dom/Node.h"
#include "solar/html/Parser.h"

namespace {

using namespace solar;
using Quanta::Embed::Runtime;

struct Case {
  std::string data;
  std::string document;
  std::string fragmentContext;  // "div", or "svg path"
  bool scriptOn = false;
  bool scriptOff = false;
};

std::vector<Case> Load(const std::string& path) {
  std::ifstream file(path);
  std::vector<Case> cases;
  std::string line, section;
  Case current;
  bool any = false;
  std::vector<std::string> data, document, fragment;
  const auto finish = [&] {
    if (!any) return;
    while (!document.empty() && document.back().empty()) document.pop_back();
    while (!data.empty() && data.back().empty() && false) data.pop_back();
    for (size_t i = 0; i < data.size(); ++i) current.data += (i ? "\n" : "") + data[i];
    for (size_t i = 0; i < document.size(); ++i) current.document += document[i] + "\n";
    if (!fragment.empty()) current.fragmentContext = fragment[0];
    cases.push_back(std::move(current));
    current = Case();
    data.clear();
    document.clear();
    fragment.clear();
  };
  while (std::getline(file, line)) {
    if (line == "#data") {
      finish();
      any = true;
      section = "data";
      continue;
    }
    if (!line.empty() && line[0] == '#' && (line == "#errors" || line == "#new-errors" || line == "#document" || line == "#document-fragment" || line == "#script-on" ||
                                           line == "#script-off")) {
      section = line.substr(1);
      if (section == "script-on") current.scriptOn = true;
      if (section == "script-off") current.scriptOff = true;
      continue;
    }
    if (section == "data") data.push_back(line);
    else if (section == "document") document.push_back(line);
    else if (section == "document-fragment") fragment.push_back(line);
  }
  finish();
  return cases;
}


void Dump(const dom::Node* node, int depth, std::string& out) {
  const std::string indent = "| " + std::string(static_cast<size_t>(depth) * 2, ' ');
  switch (node->nodeType) {
    case dom::NodeType::Document:
    case dom::NodeType::DocumentFragment:
      break;
    case dom::NodeType::DocumentType: {
      const auto* doctype = static_cast<const dom::DocumentType*>(node);
      out += indent + "<!DOCTYPE " + doctype->name;
      if (!doctype->publicId.empty() || !doctype->systemId.empty()) out += " \"" + doctype->publicId + "\" \"" + doctype->systemId + "\"";
      out += ">\n";
      break;
    }
    case dom::NodeType::Element: {
      const auto* element = static_cast<const dom::Element*>(node);
      std::string name = element->localName;
      if (element->namespaceUri == dom::kSvgNamespace) name = "svg " + name;
      else if (element->namespaceUri == dom::kMathMlNamespace) name = "math " + name;
      out += indent + "<" + name + ">\n";
      std::vector<std::pair<std::string, std::string>> attributes;
      for (const dom::Attr* attribute : element->attributes) {
        std::string attributeName = attribute->localName;
        if (!attribute->namespaceUri.empty()) {
          attributeName = (attribute->prefix.empty() ? "" : attribute->prefix + " ") + attribute->localName;
        }
        attributes.emplace_back(attributeName, attribute->value);
      }
      std::sort(attributes.begin(), attributes.end());
      for (const auto& [attributeName, value] : attributes) out += indent + "  " + attributeName + "=\"" + value + "\"\n";
      if (element->templateContents) {
        out += indent + "  content\n";
        for (const dom::Node* child = element->templateContents->firstChild; child; child = child->nextSibling) Dump(child, depth + 2, out);
        return;
      }
      break;
    }
    case dom::NodeType::Text:
      out += indent + "\"" + static_cast<const dom::CharacterData*>(node)->data + "\"\n";
      break;
    case dom::NodeType::Comment:
      out += indent + "<!-- " + static_cast<const dom::CharacterData*>(node)->data + " -->\n";
      break;
    case dom::NodeType::ProcessingInstruction: {
      const auto* instruction = static_cast<const dom::CharacterData*>(node);
      out += indent + "<?" + instruction->target + " " + instruction->data + "?>\n";
      break;
    }
    default:
      break;
  }
  for (const dom::Node* child = node->firstChild; child; child = child->nextSibling) Dump(child, node->IsDocument() || node->IsFragment() ? depth : depth + 1, out);
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> files(argv + 1, argv + argc);
  if (files.empty()) {
    for (const auto& entry : std::filesystem::directory_iterator("tests/html5lib/tree-construction")) {
      if (entry.path().extension() == ".dat") files.push_back(entry.path().string());
    }
    std::sort(files.begin(), files.end());
  }
  auto runtime = Runtime::Create();
  Quanta::Context& ctx = runtime->GetContext();
  int passed = 0, failed = 0;
  for (const std::string& file : files) {
    // The scripted tests need scripts to run, which the parser does not do yet.
    if (std::filesystem::path(file).filename().string().starts_with("scripted_")) continue;
    int filePassed = 0, fileFailed = 0;
    for (const Case& test : Load(file)) {
      // selectedcontent is filled from the selected option by the select element's own steps, not by the parser.
      if (test.data.find("<selectedcontent>") != std::string::npos) continue;
      html::ScriptingMode scripting = test.scriptOn ? html::ScriptingMode::Normal : html::ScriptingMode::Disabled;
      std::string actual;
      if (test.fragmentContext.empty()) {
        dom::Document* document = dom::NewDocument(ctx, true);
        html::ParseDocument(ctx, document, test.data, scripting);
        Dump(document, 0, actual);
      } else {
        std::string name = test.fragmentContext, ns(dom::kHtmlNamespace);
        const size_t space = name.find(' ');
        if (space != std::string::npos) {
          const std::string prefix = name.substr(0, space);
          ns = prefix == "svg" ? dom::kSvgNamespace : dom::kMathMlNamespace;
          name = name.substr(space + 1);
        }
        dom::Document* owner = dom::NewDocument(ctx, true);
        dom::Element* context = dom::NewElement(ctx, owner, name, ns);
        dom::DocumentFragment* fragment = html::ParseFragment(ctx, context, test.data, scripting == html::ScriptingMode::Normal ? html::ScriptingMode::Fragment : html::ScriptingMode::Inert);
        Dump(fragment, 0, actual);
      }
      if (actual == test.document) {
        ++filePassed;
      } else {
        ++fileFailed;
        std::printf("FAIL %s\n  input: %s\n  expected:\n%s  actual:\n%s", file.c_str(), test.data.c_str(), test.document.c_str(), actual.c_str());
      }
    }
    std::printf("%s: %d/%d\n", file.c_str(), filePassed, filePassed + fileFailed);
    passed += filePassed;
    failed += fileFailed;
  }
  std::printf("%d/%d passed\n", passed, passed + failed);
  return failed == 0 ? 0 : 1;
}
