#include <iostream>
#include <fstream>
#include <sstream>
#include "CSSParser.h"

using namespace CSS3Parser;

void print_stylesheet(const CSSStyleSheet& stylesheet) {
    std::cout << "=== CSS Stylesheet ===" << std::endl;
    std::cout << "Rules: " << stylesheet.rules.size() << std::endl;
    
    for (const auto& rule : stylesheet.rules) {
        switch (rule->type) {
            case RuleType::Style: {
                auto style_rule = static_cast<const StyleRule*>(rule.get());
                std::cout << "\nStyle Rule:" << std::endl;
                std::cout << "  Selectors: " << style_rule->selectors.to_string() << std::endl;
                std::cout << "  Declarations: " << style_rule->declarations.size() << std::endl;
                for (const auto& decl : style_rule->declarations) {
                    std::cout << "    " << decl.property << ": " << decl.value.to_string();
                    if (decl.important) std::cout << " !important";
                    std::cout << std::endl;
                }
                break;
            }
            case RuleType::AtRule: {
                auto at_rule = static_cast<const AtRule*>(rule.get());
                std::cout << "\nAt-Rule: @" << at_rule->name << std::endl;
                std::cout << "  Prelude: " << at_rule->prelude << std::endl;
                if (!at_rule->declarations.empty()) {
                    std::cout << "  Declarations: " << at_rule->declarations.size() << std::endl;
                    for (const auto& decl : at_rule->declarations) {
                        std::cout << "    " << decl.property << ": " << decl.value.to_string() << std::endl;
                    }
                }
                if (!at_rule->rules.empty()) {
                    std::cout << "  Nested Rules: " << at_rule->rules.size() << std::endl;
                }
                break;
            }
            case RuleType::Comment: {
                auto comment_rule = static_cast<const CommentRule*>(rule.get());
                std::cout << "\nComment: " << comment_rule->content << std::endl;
                break;
            }
        }
    }
}

int main(int argc, char* argv[]) {
    std::cout << "CSS3 Parser v4.0 - Standalone CSS Parser" << std::endl;
    
    if (argc != 2) {
        std::cout << "Usage: " << argv[0] << " <css_file>" << std::endl;
        return 1;
    }
    
    std::string filename = argv[1];
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open file " << filename << std::endl;
        return 1;
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string css_content = buffer.str();
    
    std::cout << "\nParsing CSS file: " << filename << std::endl;
    std::cout << "File size: " << css_content.length() << " bytes" << std::endl;
    
    CSSParser parser(css_content);
    auto stylesheet = parser.parse_stylesheet();
    
    if (!stylesheet) {
        std::cerr << "Error: Failed to parse CSS stylesheet" << std::endl;
        return 1;
    }
    
    print_stylesheet(*stylesheet);
    
    auto errors = parser.get_errors();
    if (!errors.empty()) {
        std::cout << "\n=== Parse Errors ===" << std::endl;
        for (const auto& error : errors) {
            std::cout << "Error: " << error.message << " at line " << error.line << std::endl;
        }
    }
    
    std::cout << "\n=== CSS Pretty Print ===" << std::endl;
    std::cout << stylesheet->to_string() << std::endl;
    
    std::cout << "\n✅ CSS parsing completed successfully!" << std::endl;
    return 0;
}