#include <iostream>
#include <fstream>
#include <sstream>
#include "HTMLParser.h"

using namespace HTML5Parser;

void print_node(const Node& node, int depth = 0) {
    std::string indent(depth * 2, ' ');
    
    if (node.type == NodeType::Document) {
        std::cout << "Document" << std::endl;
    } else if (node.type == NodeType::Element) {
        std::cout << indent << "<" << node.tag_name;
        for (const auto& attr : node.attributes) {
            std::cout << " " << attr.first << "=\"" << attr.second << "\"";
        }
        std::cout << ">" << std::endl;
    } else if (node.type == NodeType::Text && !node.text_content.empty()) {
        std::string trimmed = node.text_content;
        trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r"));
        trimmed.erase(trimmed.find_last_not_of(" \t\n\r") + 1);
        if (!trimmed.empty()) {
            std::cout << indent << "\"" << trimmed << "\"" << std::endl;
        }
    }
    
    for (const auto& child : node.children) {
        print_node(*child, depth + 1);
    }
}

int main(int argc, char* argv[]) {
    std::cout << "HTML5 Parser v4.0 - Standalone HTML Parser" << std::endl;
    
    if (argc != 2) {
        std::cout << "Usage: " << argv[0] << " <html_file>" << std::endl;
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
    std::string html_content = buffer.str();
    
    std::cout << "\nParsing HTML file: " << filename << std::endl;
    std::cout << "File size: " << html_content.length() << " bytes" << std::endl;
    
    Parser parser(html_content, false);
    auto document = parser.parse();
    
    if (!document) {
        std::cerr << "Error: Failed to parse HTML document" << std::endl;
        return 1;
    }
    
    std::cout << "\n=== HTML Document Structure ===" << std::endl;
    print_node(*document);
    
    auto errors = parser.get_errors();
    if (!errors.empty()) {
        std::cout << "\n=== Parse Errors ===" << std::endl;
        for (const auto& error : errors) {
            std::cout << "Error: " << error.what() << std::endl;
        }
    }
    
    std::cout << "\n✅ HTML parsing completed successfully!" << std::endl;
    return 0;
}