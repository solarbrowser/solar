#include "BrowserParser.h"
#include <fstream>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <regex>

namespace BrowserParser {

WebPageParser::WebPageParser() : options_({}) {}

WebPageParser::WebPageParser(const ParseOptions& options) : options_(options) {}

ParsedDocument WebPageParser::parse_html_with_css(const std::string& html_content) {
    auto start_time = std::chrono::high_resolution_clock::now();
    
    ParsedDocument document;
    
    // Parse HTML
    HTML5Parser::Parser html_parser(html_content, options_.html_options.strict_mode);
    html_parser.set_options(options_.html_options);
    document.html_document = html_parser.parse();
    
    // Collect HTML parse errors
    for (const auto& error : html_parser.get_errors()) {
        document.parse_errors.push_back(std::string("HTML: ") + error.what());
    }
    
    if (!document.html_document) {
        add_error("Failed to parse HTML document");
        return document;
    }
    
    // Extract CSS from HTML
    if (options_.extract_style_elements || options_.extract_inline_styles) {
        auto css_sources = extract_css_from_html(*document.html_document);
        
        // Parse each CSS source
        for (const auto& css_content : css_sources) {
            if (!css_content.empty()) {
                CSS3Parser::CSSParser css_parser(css_content, options_.css_options);
                auto stylesheet = css_parser.parse_stylesheet();
                
                // Collect CSS parse errors
                for (const auto& error : css_parser.get_errors()) {
                    document.parse_errors.push_back("CSS: " + error.message);
                }
                
                if (stylesheet) {
                    document.stylesheets.push_back(std::move(stylesheet));
                }
            }
        }
        
        // Extract inline styles
        if (options_.extract_inline_styles) {
            document.inline_styles = extract_inline_styles(*document.html_document);
        }
    }
    
    // Validation
    if (options_.validate_css_against_html) {
        for (const auto& stylesheet : document.stylesheets) {
            auto validation_errors = validate_css_selectors_against_html(*stylesheet, *document.html_document);
            for (const auto& error : validation_errors) {
                document.parse_errors.push_back("Validation: " + error);
            }
        }
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
    
    // Compute statistics
    document.stats = compute_statistics(document);
    document.stats.parse_time_us = duration.count();
    document.stats.total_size = html_content.length();
    
    return document;
}

ParsedDocument WebPageParser::parse_html_file(const std::string& file_path) {
    std::ifstream file(file_path);
    if (!file.is_open()) {
        add_error("Could not open file: " + file_path);
        return ParsedDocument{};
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    return parse_html_with_css(buffer.str());
}

std::unique_ptr<HTML5Parser::Node> WebPageParser::parse_html(const std::string& html) {
    HTML5Parser::Parser parser(html, options_.html_options.strict_mode);
    parser.set_options(options_.html_options);
    return parser.parse();
}

std::unique_ptr<CSS3Parser::CSSStyleSheet> WebPageParser::parse_css(const std::string& css) {
    CSS3Parser::CSSParser parser(css, options_.css_options);
    return parser.parse_stylesheet();
}

std::vector<std::string> WebPageParser::extract_css_from_html(const HTML5Parser::Node& html_doc) {
    std::vector<std::string> css_sources;
    std::map<std::string, std::string> inline_styles; // Not used here but required for function signature
    
    extract_styles_recursive(html_doc, css_sources, inline_styles);
    return css_sources;
}

std::map<std::string, std::string> WebPageParser::extract_inline_styles(const HTML5Parser::Node& html_doc) {
    std::vector<std::string> css_sources; // Not used here but required for function signature
    std::map<std::string, std::string> inline_styles;
    
    extract_styles_recursive(html_doc, css_sources, inline_styles);
    return inline_styles;
}

void WebPageParser::extract_styles_recursive(const HTML5Parser::Node& node, 
                                             std::vector<std::string>& styles,
                                             std::map<std::string, std::string>& inline_styles) {
    
    if (node.type == HTML5Parser::NodeType::Element) {
        // Extract <style> element content
        if (options_.extract_style_elements && node.tag_name == "style") {
            for (const auto& child : node.children) {
                if (child->type == HTML5Parser::NodeType::Text) {
                    styles.push_back(child->text_content);
                }
            }
        }
        
        // Extract inline styles from style attribute
        if (options_.extract_inline_styles) {
            auto style_attr = node.attributes.find("style");
            if (style_attr != node.attributes.end()) {
                std::string element_id = node.tag_name;
                
                // Try to get a more specific identifier
                auto id_attr = node.attributes.find("id");
                auto class_attr = node.attributes.find("class");
                
                if (id_attr != node.attributes.end()) {
                    element_id = "#" + id_attr->second;
                } else if (class_attr != node.attributes.end()) {
                    element_id = "." + class_attr->second;
                } else {
                    element_id = node.tag_name + "[" + std::to_string(inline_styles.size()) + "]";
                }
                
                inline_styles[element_id] = style_attr->second;
            }
        }
        
        // Recursively process children
        for (const auto& child : node.children) {
            extract_styles_recursive(*child, styles, inline_styles);
        }
    }
}

std::vector<std::string> WebPageParser::validate_css_selectors_against_html(
    const CSS3Parser::CSSStyleSheet& stylesheet, 
    const HTML5Parser::Node& html_doc) {
    
    std::vector<std::string> errors;
    
    for (const auto& rule : stylesheet.rules) {
        if (rule->type == CSS3Parser::RuleType::Style) {
            auto style_rule = static_cast<const CSS3Parser::StyleRule*>(rule.get());
            
            for (const auto& complex_selector : style_rule->selectors.selectors) {
                auto matching_elements = CSSMatcher::find_matching_elements(
                    CSS3Parser::SelectorList{}, html_doc);
                
                if (matching_elements.empty()) {
                    errors.push_back("Selector '" + complex_selector.to_string() + 
                                   "' does not match any elements");
                }
            }
        }
    }
    
    return errors;
}

std::map<std::string, int> WebPageParser::compute_selector_specificity(const CSS3Parser::CSSStyleSheet& stylesheet) {
    std::map<std::string, int> specificity_map;
    
    for (const auto& rule : stylesheet.rules) {
        if (rule->type == CSS3Parser::RuleType::Style) {
            auto style_rule = static_cast<const CSS3Parser::StyleRule*>(rule.get());
            
            for (const auto& complex_selector : style_rule->selectors.selectors) {
                specificity_map[complex_selector.to_string()] = complex_selector.specificity();
            }
        }
    }
    
    return specificity_map;
}

void WebPageParser::add_error(const std::string& message) {
    errors_.push_back(message);
}

ParsedDocument::Stats WebPageParser::compute_statistics(const ParsedDocument& document) {
    ParsedDocument::Stats stats;
    
    // Count HTML elements
    std::function<void(const HTML5Parser::Node&)> count_elements = 
        [&](const HTML5Parser::Node& node) {
            if (node.type == HTML5Parser::NodeType::Element) {
                stats.html_elements++;
            }
            for (const auto& child : node.children) {
                count_elements(*child);
            }
        };
    
    if (document.html_document) {
        count_elements(*document.html_document);
    }
    
    // Count CSS rules and declarations
    for (const auto& stylesheet : document.stylesheets) {
        stats.css_rules += stylesheet->rules.size();
        
        for (const auto& rule : stylesheet->rules) {
            if (rule->type == CSS3Parser::RuleType::Style) {
                auto style_rule = static_cast<const CSS3Parser::StyleRule*>(rule.get());
                stats.css_declarations += style_rule->declarations.size();
            } else if (rule->type == CSS3Parser::RuleType::AtRule) {
                auto at_rule = static_cast<const CSS3Parser::AtRule*>(rule.get());
                stats.css_declarations += at_rule->declarations.size();
            }
        }
    }
    
    return stats;
}

// CSSMatcher implementation
CSSMatcher::MatchResult CSSMatcher::matches_selector(const CSS3Parser::ComplexSelector& selector, 
                                                     const HTML5Parser::Node& element,
                                                     const HTML5Parser::Node& document_root) {
    MatchResult result;
    
    if (selector.components.empty()) {
        return result;
    }
    
    // Start from the rightmost component (the element we're testing)
    const auto& last_component = selector.components.back();
    auto compound_result = matches_compound_selector(last_component.selector, element);
    
    if (!compound_result.matches) {
        return result;
    }
    
    result.specificity += compound_result.specificity;
    result.matched_selector = last_component.selector.to_string();
    
    // If this is the only component, we have a match
    if (selector.components.size() == 1) {
        result.matches = true;
        return result;
    }
    
    // Check remaining components with combinators
    // This is a simplified implementation
    result.matches = true;
    for (size_t i = 0; i < selector.components.size() - 1; ++i) {
        auto comp_result = matches_compound_selector(selector.components[i].selector, element);
        result.specificity += comp_result.specificity;
    }
    
    return result;
}

CSSMatcher::MatchResult CSSMatcher::matches_compound_selector(const CSS3Parser::CompoundSelector& selector,
                                                             const HTML5Parser::Node& element) {
    MatchResult result;
    result.matches = true;
    
    for (const auto& simple_sel : selector.selectors) {
        auto simple_result = matches_simple_selector(simple_sel, element);
        if (!simple_result.matches) {
            result.matches = false;
            return result;
        }
        result.specificity += simple_result.specificity;
    }
    
    result.matched_selector = selector.to_string();
    return result;
}

CSSMatcher::MatchResult CSSMatcher::matches_simple_selector(const CSS3Parser::SimpleSelector& selector,
                                                           const HTML5Parser::Node& element) {
    MatchResult result;
    result.specificity = selector.specificity();
    
    if (element.type != HTML5Parser::NodeType::Element) {
        return result;
    }
    
    switch (selector.type) {
        case CSS3Parser::SelectorType::Universal:
            result.matches = true;
            break;
            
        case CSS3Parser::SelectorType::Type:
            result.matches = (element.tag_name == selector.name);
            break;
            
        case CSS3Parser::SelectorType::Class: {
            auto class_attr = element.attributes.find("class");
            if (class_attr != element.attributes.end()) {
                // Simple class matching (space-separated)
                std::string class_list = " " + class_attr->second + " ";
                std::string target_class = " " + selector.name + " ";
                result.matches = (class_list.find(target_class) != std::string::npos);
            }
            break;
        }
        
        case CSS3Parser::SelectorType::Id: {
            auto id_attr = element.attributes.find("id");
            result.matches = (id_attr != element.attributes.end() && 
                             id_attr->second == selector.name);
            break;
        }
        
        case CSS3Parser::SelectorType::Attribute:
            result.matches = matches_attribute_selector(selector.attribute, element);
            break;
            
        case CSS3Parser::SelectorType::Pseudo:
        case CSS3Parser::SelectorType::PseudoElement:
            // Simplified pseudo-selector matching
            result.matches = true; // Would need more complex logic for real pseudo-selectors
            break;
    }
    
    if (result.matches) {
        result.matched_selector = selector.to_string();
    }
    
    return result;
}

bool CSSMatcher::matches_attribute_selector(const CSS3Parser::AttributeSelector& attr_sel,
                                           const HTML5Parser::Node& element) {
    auto attr_it = element.attributes.find(attr_sel.name);
    
    if (attr_it == element.attributes.end()) {
        return false;
    }
    
    const std::string& attr_value = attr_it->second;
    
    switch (attr_sel.match_type) {
        case CSS3Parser::AttributeMatchType::Exists:
            return true;
            
        case CSS3Parser::AttributeMatchType::Exact:
            return attr_value == attr_sel.value;
            
        case CSS3Parser::AttributeMatchType::Include: {
            std::string value_list = " " + attr_value + " ";
            std::string target_value = " " + attr_sel.value + " ";
            return value_list.find(target_value) != std::string::npos;
        }
        
        case CSS3Parser::AttributeMatchType::Dash:
            return attr_value == attr_sel.value || 
                   attr_value.substr(0, attr_sel.value.length() + 1) == attr_sel.value + "-";
            
        case CSS3Parser::AttributeMatchType::Prefix:
            return attr_value.substr(0, attr_sel.value.length()) == attr_sel.value;
            
        case CSS3Parser::AttributeMatchType::Suffix:
            return attr_value.length() >= attr_sel.value.length() &&
                   attr_value.substr(attr_value.length() - attr_sel.value.length()) == attr_sel.value;
            
        case CSS3Parser::AttributeMatchType::Substring:
            return attr_value.find(attr_sel.value) != std::string::npos;
    }
    
    return false;
}

std::vector<const HTML5Parser::Node*> CSSMatcher::find_matching_elements(
    const CSS3Parser::SelectorList& selectors,
    const HTML5Parser::Node& document_root) {
    
    std::vector<const HTML5Parser::Node*> matching_elements;
    
    std::function<void(const HTML5Parser::Node&)> search_elements = 
        [&](const HTML5Parser::Node& node) {
            if (node.type == HTML5Parser::NodeType::Element) {
                for (const auto& complex_selector : selectors.selectors) {
                    auto result = matches_selector(complex_selector, node, document_root);
                    if (result.matches) {
                        matching_elements.push_back(&node);
                        break; // Don't add the same element multiple times
                    }
                }
            }
            
            for (const auto& child : node.children) {
                search_elements(*child);
            }
        };
    
    search_elements(document_root);
    return matching_elements;
}

// HTMLCSSAnalyzer implementation
HTMLCSSAnalyzer::AnalysisReport HTMLCSSAnalyzer::analyze(const ParsedDocument& document) {
    AnalysisReport report;
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // Analyze HTML structure
    if (document.html_document) {
        analyze_html_structure(*document.html_document, report);
    }
    
    // Analyze CSS usage
    for (const auto& stylesheet : document.stylesheets) {
        if (document.html_document) {
            analyze_css_usage(*stylesheet, *document.html_document, report);
        }
    }
    
    // Count inline styles
    report.inline_styles = document.inline_styles.size();
    report.internal_stylesheets = document.stylesheets.size();
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    report.parse_time_ms = duration.count();
    
    return report;
}

void HTMLCSSAnalyzer::analyze_html_structure(const HTML5Parser::Node& node, AnalysisReport& report) {
    if (node.type == HTML5Parser::NodeType::Element) {
        report.total_elements++;
        report.element_counts[node.tag_name]++;
        
        // Check for ID
        auto id_attr = node.attributes.find("id");
        if (id_attr != node.attributes.end()) {
            report.elements_with_ids++;
            report.id_usage[id_attr->second]++;
        }
        
        // Check for classes
        auto class_attr = node.attributes.find("class");
        if (class_attr != node.attributes.end()) {
            report.elements_with_classes++;
            
            // Split class names
            std::istringstream class_stream(class_attr->second);
            std::string class_name;
            while (class_stream >> class_name) {
                report.class_usage[class_name]++;
            }
        }
    }
    
    // Recursively analyze children
    for (const auto& child : node.children) {
        analyze_html_structure(*child, report);
    }
}

void HTMLCSSAnalyzer::analyze_css_usage(const CSS3Parser::CSSStyleSheet& stylesheet, 
                                       const HTML5Parser::Node& html_doc, 
                                       AnalysisReport& report) {
    for (const auto& rule : stylesheet.rules) {
        if (rule->type == CSS3Parser::RuleType::Style) {
            auto style_rule = static_cast<const CSS3Parser::StyleRule*>(rule.get());
            report.total_rules++;
            report.total_declarations += style_rule->declarations.size();
            
            // Analyze specificity
            int max_specificity = style_rule->selectors.max_specificity();
            report.specificity_distribution[max_specificity]++;
            
            // Check if selectors match any elements
            auto matching_elements = CSSMatcher::find_matching_elements(style_rule->selectors, html_doc);
            if (matching_elements.empty()) {
                report.unused_selectors++;
                report.unused_css_selectors.push_back(style_rule->selectors.to_string());
            }
            
            // Analyze property usage
            for (const auto& declaration : style_rule->declarations) {
                report.property_usage[declaration.property]++;
            }
        }
    }
}

std::string HTMLCSSAnalyzer::generate_report(const AnalysisReport& report) {
    std::ostringstream ss;
    
    ss << "=== HTML & CSS Analysis Report ===" << std::endl;
    ss << std::endl;
    
    // HTML Analysis
    ss << "HTML Structure:" << std::endl;
    ss << "  Total Elements: " << report.total_elements << std::endl;
    ss << "  Elements with IDs: " << report.elements_with_ids << std::endl;
    ss << "  Elements with Classes: " << report.elements_with_classes << std::endl;
    ss << std::endl;
    
    ss << "Most Common Elements:" << std::endl;
    auto sorted_elements = report.element_counts;
    // Sort would require converting to vector, simplified here
    for (const auto& [element, count] : sorted_elements) {
        if (count > 1) {
            ss << "  " << element << ": " << count << std::endl;
        }
    }
    ss << std::endl;
    
    // CSS Analysis
    ss << "CSS Analysis:" << std::endl;
    ss << "  Total Rules: " << report.total_rules << std::endl;
    ss << "  Total Declarations: " << report.total_declarations << std::endl;
    ss << "  Unused Selectors: " << report.unused_selectors << std::endl;
    ss << "  Invalid Properties: " << report.invalid_properties << std::endl;
    ss << std::endl;
    
    ss << "Style Sources:" << std::endl;
    ss << "  Inline Styles: " << report.inline_styles << std::endl;
    ss << "  Internal Stylesheets: " << report.internal_stylesheets << std::endl;
    ss << "  External Stylesheets: " << report.external_stylesheets << std::endl;
    ss << std::endl;
    
    // Performance
    ss << "Performance:" << std::endl;
    ss << "  Parse Time: " << report.parse_time_ms << " ms" << std::endl;
    ss << "  Memory Usage: " << report.memory_usage_kb << " KB" << std::endl;
    
    return ss.str();
}

std::string HTMLCSSAnalyzer::generate_json_report(const AnalysisReport& report) {
    std::ostringstream ss;
    
    ss << "{" << std::endl;
    ss << "  \"html\": {" << std::endl;
    ss << "    \"totalElements\": " << report.total_elements << "," << std::endl;
    ss << "    \"elementsWithIds\": " << report.elements_with_ids << "," << std::endl;
    ss << "    \"elementsWithClasses\": " << report.elements_with_classes << std::endl;
    ss << "  }," << std::endl;
    ss << "  \"css\": {" << std::endl;
    ss << "    \"totalRules\": " << report.total_rules << "," << std::endl;
    ss << "    \"totalDeclarations\": " << report.total_declarations << "," << std::endl;
    ss << "    \"unusedSelectors\": " << report.unused_selectors << std::endl;
    ss << "  }," << std::endl;
    ss << "  \"performance\": {" << std::endl;
    ss << "    \"parseTimeMs\": " << report.parse_time_ms << "," << std::endl;
    ss << "    \"memoryUsageKb\": " << report.memory_usage_kb << std::endl;
    ss << "  }" << std::endl;
    ss << "}" << std::endl;
    
    return ss.str();
}

} // namespace BrowserParser