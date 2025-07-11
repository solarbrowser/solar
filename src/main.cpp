#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include "parser/Parser.h"

using namespace HTML5Parser;

class HTMLParserDemo {
public:
    static void run_demo(const std::string& filename) {
        std::cout << "\n" << std::string(60, '=') << std::endl;
        std::cout << "HTML5 Parser Demo - Processing: " << filename << std::endl;
        std::cout << std::string(60, '=') << std::endl;
        
        std::string html = read_file(filename);
        if (html.empty()) {
            std::cerr << "Error: Could not read file " << filename << std::endl;
            return;
        }
        
        std::cout << "\nInput HTML (first 200 chars):\n";
        std::cout << std::string(40, '-') << std::endl;
        std::cout << html.substr(0, 200);
        if (html.length() > 200) std::cout << "...";
        std::cout << "\n" << std::string(40, '-') << std::endl;
        
        auto start = std::chrono::high_resolution_clock::now();
        
        Parser parser(html);
        Parser::ParseOptions options;
        options.strict_mode = false;
        options.preserve_whitespace = false;
        options.validate_nesting = true;
        parser.set_options(options);
        
        auto document = parser.parse();
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        std::cout << "\nParsing completed in " << duration.count() << " microseconds\n";
        
        const auto& errors = parser.get_errors();
        if (!errors.empty()) {
            std::cout << "\nParse Errors (" << errors.size() << "):" << std::endl;
            std::cout << std::string(30, '-') << std::endl;
            for (size_t i = 0; i < std::min(errors.size(), size_t(5)); ++i) {
                std::cout << "Position " << errors[i].position << ": " 
                         << errors[i].what() << std::endl;
            }
            if (errors.size() > 5) {
                std::cout << "... and " << (errors.size() - 5) << " more errors\n";
            }
        }
        
        std::cout << "\nParsed Tree Structure:\n";
        std::cout << std::string(30, '-') << std::endl;
        std::cout << PrettyPrinter::print(*document, 2);
        
        std::cout << "\nJSON Output:\n";
        std::cout << std::string(30, '-') << std::endl;
        std::cout << PrettyPrinter::print_json(*document, 2) << std::endl;
        
        print_statistics(*document);
    }
    
private:
    static std::string read_file(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            return "";
        }
        
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }
    
    struct Stats {
        int elements = 0;
        int text_nodes = 0;
        int comments = 0;
        int total_nodes = 0;
        int max_depth = 0;
    };
    
    static void print_statistics(const Node& document) {
        Stats stats;
        
        count_nodes(document, stats, 0);
        
        std::cout << "\nDocument Statistics:\n";
        std::cout << std::string(30, '-') << std::endl;
        std::cout << "Total Nodes: " << stats.total_nodes << std::endl;
        std::cout << "Elements: " << stats.elements << std::endl;
        std::cout << "Text Nodes: " << stats.text_nodes << std::endl;
        std::cout << "Comments: " << stats.comments << std::endl;
        std::cout << "Max Depth: " << stats.max_depth << std::endl;
    }
    
    static void count_nodes(const Node& node, Stats& stats, int depth) {
        stats.total_nodes++;
        stats.max_depth = std::max(stats.max_depth, depth);
        
        switch (node.type) {
            case NodeType::Element:
                stats.elements++;
                break;
            case NodeType::Text:
                if (!node.text_content.empty()) {
                    stats.text_nodes++;
                }
                break;
            case NodeType::Comment:
                stats.comments++;
                break;
            default:
                break;
        }
        
        for (const auto& child : node.children) {
            count_nodes(*child, stats, depth + 1);
        }
    }
};

void print_usage(const char* program_name) {
    std::cout << "\nUsage: " << program_name << " [html_file]\n";
    std::cout << "\nOptions:\n";
    std::cout << "  html_file    HTML file to parse (default: test.html)\n";
    std::cout << "\nExample:\n";
    std::cout << "  " << program_name << " test.html\n";
    std::cout << "  " << program_name << " index.html\n";
}

int main(int argc, char* argv[]) {
    std::cout << "HTML5 Parser v2.0 - Enhanced C++ HTML Parser\n";
    std::cout << "Supports all modern HTML5 elements and features\n";
    
    std::string filename = "test.html";
    
    if (argc > 1) {
        if (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        filename = argv[1];
    }
    
    try {
        HTMLParserDemo::run_demo(filename);
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << std::endl;
        return 1;
    }
    
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "Demo completed successfully!" << std::endl;
    std::cout << std::string(60, '=') << std::endl;
    
    return 0;
}
