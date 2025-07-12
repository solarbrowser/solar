#ifndef BROWSER_PARSER_H
#define BROWSER_PARSER_H

#include "HTMLParser.h"
#include "CSSParser.h"
#include <memory>
#include <string>
#include <vector>
#include <map>

namespace BrowserParser {

// Forward declarations
class StyleEngine;
class CSSMatcher;

struct ParsedDocument {
    std::unique_ptr<HTML5Parser::Node> html_document;
    std::vector<std::unique_ptr<CSS3Parser::CSSStyleSheet>> stylesheets;
    std::map<std::string, std::string> inline_styles; // element id/class -> style
    std::vector<std::string> parse_errors;
    
    // Statistics
    struct Stats {
        size_t html_elements = 0;
        size_t css_rules = 0;
        size_t css_declarations = 0;
        size_t parse_time_us = 0;
        size_t total_size = 0;
    } stats;
};

class WebPageParser {
public:
    struct ParseOptions {
        // HTML options
        HTML5Parser::Parser::ParseOptions html_options;
        
        // CSS options
        CSS3Parser::CSSParser::ParseOptions css_options;
        
        // Integration options
        bool extract_inline_styles = true;
        bool extract_style_elements = true;
        bool extract_linked_styles = false; // Would require file system access
        bool validate_css_against_html = true;
        bool compute_specificity = true;
    };
    
    explicit WebPageParser(const ParseOptions& options);
    WebPageParser();
    
    ParsedDocument parse_html_with_css(const std::string& html_content);
    ParsedDocument parse_html_file(const std::string& file_path);
    
    // Individual parsing methods
    std::unique_ptr<HTML5Parser::Node> parse_html(const std::string& html);
    std::unique_ptr<CSS3Parser::CSSStyleSheet> parse_css(const std::string& css);
    
    // Style extraction
    std::vector<std::string> extract_css_from_html(const HTML5Parser::Node& html_doc);
    std::map<std::string, std::string> extract_inline_styles(const HTML5Parser::Node& html_doc);
    
    // Analysis methods
    std::vector<std::string> validate_css_selectors_against_html(
        const CSS3Parser::CSSStyleSheet& stylesheet, 
        const HTML5Parser::Node& html_doc);
    
    std::map<std::string, int> compute_selector_specificity(const CSS3Parser::CSSStyleSheet& stylesheet);
    
    // Error handling
    const std::vector<std::string>& get_errors() const { return errors_; }
    bool has_errors() const { return !errors_.empty(); }
    
private:
    ParseOptions options_;
    std::vector<std::string> errors_;
    
    void extract_styles_recursive(const HTML5Parser::Node& node, 
                                  std::vector<std::string>& styles,
                                  std::map<std::string, std::string>& inline_styles);
    
    void add_error(const std::string& message);
    ParsedDocument::Stats compute_statistics(const ParsedDocument& document);
};

class CSSMatcher {
public:
    struct MatchResult {
        bool matches = false;
        int specificity = 0;
        std::string matched_selector;
    };
    
    static MatchResult matches_selector(const CSS3Parser::ComplexSelector& selector, 
                                       const HTML5Parser::Node& element,
                                       const HTML5Parser::Node& document_root);
    
    static MatchResult matches_compound_selector(const CSS3Parser::CompoundSelector& selector,
                                                 const HTML5Parser::Node& element);
    
    static MatchResult matches_simple_selector(const CSS3Parser::SimpleSelector& selector,
                                              const HTML5Parser::Node& element);
    
    static std::vector<const HTML5Parser::Node*> find_matching_elements(
        const CSS3Parser::SelectorList& selectors,
        const HTML5Parser::Node& document_root);
    
private:
    static bool matches_attribute_selector(const CSS3Parser::AttributeSelector& attr_sel,
                                          const HTML5Parser::Node& element);
    
    static bool matches_pseudo_selector(const CSS3Parser::PseudoSelector& pseudo_sel,
                                       const HTML5Parser::Node& element,
                                       const HTML5Parser::Node& document_root);
    
    static std::vector<const HTML5Parser::Node*> get_ancestors(const HTML5Parser::Node& element,
                                                              const HTML5Parser::Node& document_root);
    
    static std::vector<const HTML5Parser::Node*> get_siblings(const HTML5Parser::Node& element,
                                                             const HTML5Parser::Node& document_root);
};

class StyleEngine {
public:
    struct ComputedStyle {
        std::map<std::string, CSS3Parser::CSSValue> properties;
        std::map<std::string, int> specificity; // property -> specificity
        std::map<std::string, std::string> source; // property -> source rule
    };
    
    explicit StyleEngine(const ParsedDocument& document);
    
    ComputedStyle compute_style(const HTML5Parser::Node& element);
    std::map<const HTML5Parser::Node*, ComputedStyle> compute_all_styles();
    
    // Cascade resolution
    CSS3Parser::CSSValue resolve_property(const std::string& property, 
                                         const HTML5Parser::Node& element);
    
    // Inheritance
    CSS3Parser::CSSValue inherit_property(const std::string& property,
                                         const HTML5Parser::Node& element,
                                         const HTML5Parser::Node& parent);
    
    // Value computation
    CSS3Parser::CSSValue compute_value(const CSS3Parser::CSSValue& specified_value,
                                      const std::string& property,
                                      const HTML5Parser::Node& element);
    
private:
    const ParsedDocument& document_;
    std::map<std::string, bool> inheritable_properties_;
    
    void initialize_inheritable_properties();
    std::vector<std::pair<CSS3Parser::CSSDeclaration, int>> get_matching_declarations(
        const std::string& property, const HTML5Parser::Node& element);
};

class HTMLCSSAnalyzer {
public:
    struct AnalysisReport {
        // HTML analysis
        size_t total_elements = 0;
        size_t elements_with_ids = 0;
        size_t elements_with_classes = 0;
        std::map<std::string, size_t> element_counts;
        std::map<std::string, size_t> class_usage;
        std::map<std::string, size_t> id_usage;
        
        // CSS analysis
        size_t total_rules = 0;
        size_t total_declarations = 0;
        size_t unused_selectors = 0;
        size_t invalid_properties = 0;
        std::map<std::string, size_t> property_usage;
        std::map<int, size_t> specificity_distribution;
        
        // Integration analysis
        size_t inline_styles = 0;
        size_t internal_stylesheets = 0;
        size_t external_stylesheets = 0;
        std::vector<std::string> unused_css_selectors;
        std::vector<std::string> missing_css_targets;
        
        // Performance metrics
        size_t parse_time_ms = 0;
        size_t memory_usage_kb = 0;
    };
    
    static AnalysisReport analyze(const ParsedDocument& document);
    static std::string generate_report(const AnalysisReport& report);
    static std::string generate_json_report(const AnalysisReport& report);
    
private:
    static void analyze_html_structure(const HTML5Parser::Node& node, AnalysisReport& report);
    static void analyze_css_usage(const CSS3Parser::CSSStyleSheet& stylesheet, 
                                 const HTML5Parser::Node& html_doc, 
                                 AnalysisReport& report);
};

class WebPageRenderer {
public:
    struct RenderOptions {
        bool include_computed_styles = true;
        bool include_layout_info = false;
        bool minify_output = false;
        std::string output_format = "html"; // html, json, xml
    };
    
    static std::string render_with_styles(const ParsedDocument& document, 
                                         const RenderOptions& options);
    static std::string render_with_styles(const ParsedDocument& document);
    
    static std::string render_html_with_computed_styles(const HTML5Parser::Node& html_doc,
                                                       const std::map<const HTML5Parser::Node*, 
                                                                     StyleEngine::ComputedStyle>& styles);
    
    static std::string render_css_summary(const std::vector<std::unique_ptr<CSS3Parser::CSSStyleSheet>>& stylesheets);
    
private:
    static void render_node_with_styles(const HTML5Parser::Node& node,
                                       const std::map<const HTML5Parser::Node*, 
                                                     StyleEngine::ComputedStyle>& styles,
                                       std::ostringstream& output,
                                       int indent_level = 0);
};

} // namespace BrowserParser

#endif // BROWSER_PARSER_H