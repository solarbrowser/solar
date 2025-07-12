#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <numeric>
#include <algorithm>
#include "BrowserParser.h"

using namespace BrowserParser;

class ModernBrowserDemo {
public:
    static void run_comprehensive_demo(const std::string& html_file, const std::string& css_file = "") {
        std::cout << "\n" << std::string(80, '=') << std::endl;
        std::cout << "🌐 MODERN BROWSER PARSER - HTML5 & CSS3 ENGINE" << std::endl;
        std::cout << std::string(80, '=') << std::endl;
        
        // Configure parser options for browser environment
        WebPageParser::ParseOptions options;
        options.html_options.strict_mode = false;
        options.html_options.preserve_whitespace = false;
        options.html_options.validate_nesting = true;
        
        options.css_options.strict_mode = false;
        options.css_options.preserve_comments = true;
        options.css_options.validate_properties = true;
        options.css_options.allow_vendor_prefixes = true;
        
        options.extract_inline_styles = true;
        options.extract_style_elements = true;
        options.validate_css_against_html = true;
        options.compute_specificity = true;
        
        WebPageParser parser(options);
        
        // Parse HTML file
        std::cout << "\n📄 Parsing web document: " << html_file << std::endl;
        auto document = parser.parse_html_file(html_file);
        
        if (!document.html_document) {
            std::cerr << "❌ Failed to parse HTML document" << std::endl;
            return;
        }
        
        // Parse additional CSS file if provided
        if (!css_file.empty()) {
            std::cout << "🎨 Parsing external stylesheet: " << css_file << std::endl;
            std::string css_content = read_file(css_file);
            if (!css_content.empty()) {
                CSS3Parser::CSSParser css_parser(css_content, options.css_options);
                auto stylesheet = css_parser.parse_stylesheet();
                
                if (stylesheet) {
                    document.stylesheets.push_back(std::move(stylesheet));
                }
                
                // Collect external CSS parse errors
                for (const auto& error : css_parser.get_errors()) {
                    document.parse_errors.push_back("External CSS: " + error.message);
                }
            }
        }
        
        // Display comprehensive parsing results
        display_parsing_summary(document);
        display_html_analysis(document);
        display_css_analysis(document);
        display_integration_analysis(document);
        display_performance_metrics(document);
        
        // Generate comprehensive reports
        HTMLCSSAnalyzer analyzer;
        auto analysis_report = analyzer.analyze(document);
        
        std::cout << "\n" << analyzer.generate_report(analysis_report) << std::endl;
        
        // Save detailed reports
        save_reports(document, analysis_report);
        
        std::cout << "\n" << std::string(80, '=') << std::endl;
        std::cout << "✅ Browser parsing demo completed successfully!" << std::endl;
        std::cout << std::string(80, '=') << std::endl;
    }

private:
    static std::string read_file(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            std::cerr << "⚠️ Warning: Could not open file: " << filename << std::endl;
            return "";
        }
        
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }
    
    static void display_parsing_summary(const ParsedDocument& document) {
        std::cout << "\n📊 BROWSER PARSING SUMMARY" << std::endl;
        std::cout << "----------------------------------------" << std::endl;
        
        std::cout << (document.html_document ? "✅" : "❌") << " HTML Document: " 
                  << (document.html_document ? "Parsed" : "Failed") << std::endl;
        std::cout << "📊 HTML Elements: " << document.stats.html_elements << std::endl;
        std::cout << "🎨 CSS Stylesheets: " << document.stylesheets.size() << std::endl;
        std::cout << "📋 CSS Rules: " << document.stats.css_rules << std::endl;
        std::cout << "🎯 CSS Declarations: " << document.stats.css_declarations << std::endl;
        std::cout << "💄 Inline Styles: " << document.inline_styles.size() << std::endl;
        std::cout << "⚠️  Parse Errors: " << document.parse_errors.size() << std::endl;
        std::cout << "⏱️  Parse Time: " << document.stats.parse_time_us << " μs" << std::endl;
        
        // Format file size
        double size_kb = document.stats.total_size / 1024.0;
        std::cout << "📦 Total Size: " << std::fixed << std::setprecision(1) << size_kb << " KB" << std::endl;
    }
    
    static void display_html_analysis(const ParsedDocument& document) {
        std::cout << "\n🏗️  HTML STRUCTURE ANALYSIS" << std::endl;
        std::cout << "----------------------------------------" << std::endl;
        
        if (!document.html_document) return;
        
        // Count elements by type
        std::map<std::string, int> element_counts;
        std::map<std::string, int> attribute_counts;
        
        std::function<void(const HTML5Parser::Node&)> count_elements = 
            [&](const HTML5Parser::Node& node) {
                if (node.type == HTML5Parser::NodeType::Element) {
                    element_counts[node.tag_name]++;
                    for (const auto& attr : node.attributes) {
                        attribute_counts[attr.first]++;
                    }
                }
                for (const auto& child : node.children) {
                    count_elements(*child);
                }
            };
        
        count_elements(*document.html_document);
        
        std::cout << "📊 Total Elements: " << document.stats.html_elements << std::endl;
        std::cout << "🏷️  Element Types: " << element_counts.size() << std::endl;
        
        // Show most common elements
        std::cout << "\n🔝 Most Common Elements:" << std::endl;
        auto sorted_elements = get_top_items(element_counts, 5);
        for (const auto& [element, count] : sorted_elements) {
            std::cout << "    " << std::setw(12) << element << ": " << count << std::endl;
        }
        
        // Show most common attributes
        std::cout << "\n🏷️  Most Common Attributes:" << std::endl;
        auto sorted_attrs = get_top_items(attribute_counts, 5);
        for (const auto& [attr, count] : sorted_attrs) {
            std::cout << "    " << std::setw(12) << attr << ": " << count << std::endl;
        }
    }
    
    static void display_css_analysis(const ParsedDocument& document) {
        std::cout << "\n🎨 CSS ANALYSIS" << std::endl;
        std::cout << "----------------------------------------" << std::endl;
        
        // Analyze CSS properties and at-rules
        std::map<std::string, int> property_counts;
        std::map<std::string, int> at_rule_counts;
        
        for (const auto& stylesheet : document.stylesheets) {
            for (const auto& rule : stylesheet->rules) {
                if (rule->type == CSS3Parser::RuleType::Style) {
                    auto style_rule = static_cast<const CSS3Parser::StyleRule*>(rule.get());
                    for (const auto& decl : style_rule->declarations) {
                        property_counts[decl.property]++;
                    }
                } else if (rule->type == CSS3Parser::RuleType::AtRule) {
                    auto at_rule = static_cast<const CSS3Parser::AtRule*>(rule.get());
                    at_rule_counts[at_rule->name]++;
                }
            }
        }
        
        std::cout << "📊 Properties Used: " << property_counts.size() << std::endl;
        std::cout << "📋 At-Rules Used: " << at_rule_counts.size() << std::endl;
        
        // Show most used properties
        std::cout << "\n🔝 Most Used Properties:" << std::endl;
        auto sorted_props = get_top_items(property_counts, 5);
        for (const auto& [prop, count] : sorted_props) {
            std::cout << "    " << std::setw(20) << prop << ": " << count << std::endl;
        }
    }
    
    static void display_integration_analysis(const ParsedDocument& document) {
        std::cout << "\n🔗 INTEGRATION ANALYSIS" << std::endl;
        std::cout << "----------------------------------------" << std::endl;
        
        std::cout << "🎨 Style Sources:" << std::endl;
        std::cout << "   External Stylesheets: " << document.stylesheets.size() << std::endl;
        std::cout << "   Inline Styles: " << document.inline_styles.size() << std::endl;
        
        if (!document.parse_errors.empty()) {
            std::cout << "\n⚠️  Parse Errors:" << std::endl;
            for (size_t i = 0; i < std::min(document.parse_errors.size(), size_t(5)); ++i) {
                std::cout << "   " << document.parse_errors[i] << std::endl;
            }
            if (document.parse_errors.size() > 5) {
                std::cout << "   ... and " << (document.parse_errors.size() - 5) << " more errors" << std::endl;
            }
        }
    }
    
    static void display_performance_metrics(const ParsedDocument& document) {
        std::cout << "\n⚡ PERFORMANCE METRICS" << std::endl;
        std::cout << "----------------------------------------" << std::endl;
        
        double parse_time_ms = document.stats.parse_time_us / 1000.0;
        double size_kb = document.stats.total_size / 1024.0;
        double throughput_mb_s = (document.stats.total_size / 1024.0 / 1024.0) / (document.stats.parse_time_us / 1000000.0);
        double elements_per_ms = document.stats.html_elements / parse_time_ms;
        
        std::cout << "⏱️  Parse Time: " << std::fixed << std::setprecision(2) << parse_time_ms << " ms" << std::endl;
        std::cout << "📦 Document Size: " << std::fixed << std::setprecision(1) << size_kb << " KB" << std::endl;
        std::cout << "🚀 Throughput: " << std::fixed << std::setprecision(1) << throughput_mb_s << " MB/s" << std::endl;
        std::cout << "🏗️  Elements/ms: " << std::fixed << std::setprecision(0) << elements_per_ms << std::endl;
    }
    
    static void save_reports(const ParsedDocument& document, const HTMLCSSAnalyzer::AnalysisReport& report) {
        std::cout << "\n💾 SAVE DETAILED REPORTS" << std::endl;
        std::cout << "----------------------------------------" << std::endl;
        
        // Save JSON report
        std::ofstream json_file("browser_analysis.json");
        if (json_file.is_open()) {
            HTMLCSSAnalyzer analyzer;
            json_file << analyzer.generate_json_report(report);
            json_file.close();
            std::cout << "📄 JSON report saved: browser_analysis.json" << std::endl;
        }
        
        // Save detailed HTML structure
        if (document.html_document) {
            std::ofstream structure_file("browser_structure.txt");
            if (structure_file.is_open()) {
                structure_file << generate_html_tree(*document.html_document);
                structure_file.close();
                std::cout << "🏗️  HTML structure saved: browser_structure.txt" << std::endl;
            }
        }
    }
    
    static std::string generate_html_tree(const HTML5Parser::Node& node, int depth = 0) {
        std::ostringstream ss;
        std::string indent(depth * 2, ' ');
        
        if (node.type == HTML5Parser::NodeType::Document) {
            ss << "Document:" << std::endl;
        } else if (node.type == HTML5Parser::NodeType::Element) {
            ss << indent << "Element: " << node.tag_name;
            if (!node.attributes.empty()) {
                ss << " [";
                bool first = true;
                for (const auto& attr : node.attributes) {
                    if (!first) ss << ", ";
                    ss << attr.first << "=\"" << attr.second << "\"";
                    first = false;
                }
                ss << "]";
            }
            ss << std::endl;
        } else if (node.type == HTML5Parser::NodeType::Text && !node.text_content.empty()) {
            // Only show non-whitespace text
            std::string trimmed = node.text_content;
            trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r"));
            trimmed.erase(trimmed.find_last_not_of(" \t\n\r") + 1);
            if (!trimmed.empty()) {
                ss << indent << "Text: \"" << trimmed << "\"" << std::endl;
            }
        }
        
        for (const auto& child : node.children) {
            ss << generate_html_tree(*child, depth + 1);
        }
        
        return ss.str();
    }
    
    template<typename T>
    static std::vector<std::pair<std::string, T>> get_top_items(const std::map<std::string, T>& items, size_t count) {
        std::vector<std::pair<std::string, T>> sorted_items(items.begin(), items.end());
        std::sort(sorted_items.begin(), sorted_items.end(), 
                 [](const auto& a, const auto& b) { return a.second > b.second; });
        
        if (sorted_items.size() > count) {
            sorted_items.resize(count);
        }
        
        return sorted_items;
    }
};

int main(int argc, char* argv[]) {
    std::cout << "Modern Browser Parser v4.0 - HTML5 & CSS3 Engine" << std::endl;
    std::cout << "Supports complete HTML5 specification and modern CSS3 features" << std::endl;
    
    if (argc < 2) {
        std::cout << "\nUsage: " << argv[0] << " <html_file> [css_file]" << std::endl;
        std::cout << "       " << argv[0] << " --help" << std::endl;
        return 1;
    }
    
    std::string arg1 = argv[1];
    if (arg1 == "--help" || arg1 == "-h") {
        std::cout << "\nModern Browser Parser - HTML5 & CSS3 Engine" << std::endl;
        std::cout << "============================================" << std::endl;
        std::cout << "\nUsage:" << std::endl;
        std::cout << "  " << argv[0] << " <html_file>              Parse HTML file with embedded CSS" << std::endl;
        std::cout << "  " << argv[0] << " <html_file> <css_file>   Parse HTML file with external CSS" << std::endl;
        std::cout << "\nFeatures:" << std::endl;
        std::cout << "  • Complete HTML5 parsing with semantic validation" << std::endl;
        std::cout << "  • Full CSS3 support including Grid, Flexbox, animations" << std::endl;
        std::cout << "  • CSS custom properties (variables) and modern functions" << std::endl;
        std::cout << "  • Performance analysis and detailed reporting" << std::endl;
        std::cout << "  • Browser-grade parsing accuracy" << std::endl;
        return 0;
    }
    
    std::string html_file = argv[1];
    std::string css_file = (argc > 2) ? argv[2] : "";
    
    try {
        ModernBrowserDemo::run_comprehensive_demo(html_file, css_file);
    } catch (const std::exception& e) {
        std::cerr << "❌ Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}