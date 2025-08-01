#ifndef CSS_PARSER_H
#define CSS_PARSER_H

#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <variant>
#include <optional>
#include <functional>
#include <regex>

namespace CSS3Parser {

// Forward declarations
class CSSValue;
class CSSRule;
class CSSStyleSheet;

// CSS Token Types
enum class TokenType {
    // Basic tokens
    Ident,          // identifiers
    Function,       // function(
    AtKeyword,      // @media, @import, etc.
    Hash,           // #id, #color
    String,         // "string" or 'string'
    BadString,      // unterminated string
    Url,            // url(...)
    BadUrl,         // malformed url
    Delim,          // single character delimiter
    Number,         // 123, 12.34
    Percentage,     // 50%
    Dimension,      // 10px, 2em, etc.
    UnicodeRange,   // U+0100-02FF
    IncludeMatch,   // ~=
    DashMatch,      // |=
    PrefixMatch,    // ^=
    SuffixMatch,    // $=
    SubstringMatch, // *=
    Column,         // ||
    Whitespace,     // spaces, tabs, newlines
    CDO,            // <!--
    CDC,            // -->
    Colon,          // :
    Semicolon,      // ;
    Comma,          // ,
    LeftSquare,     // [
    RightSquare,    // ]
    LeftParen,      // (
    RightParen,     // )
    LeftBrace,      // {
    RightBrace,     // }
    EOF_TOKEN,      // end of file
    
    // CSS3 specific
    Important,      // !important
    Comment,        // /* comment */
    BadComment      // unterminated comment
};

struct Token {
    TokenType type;
    std::string value;
    std::string unit;           // for dimensions (px, em, %, etc.)
    double numeric_value = 0.0; // for numbers, percentages, dimensions
    size_t start_pos = 0;
    size_t end_pos = 0;
    size_t line = 1;
    size_t column = 1;
    
    Token(TokenType t = TokenType::EOF_TOKEN) : type(t) {}
    Token(TokenType t, const std::string& v) : type(t), value(v) {}
    Token(TokenType t, double num) : type(t), numeric_value(num) {}
    Token(TokenType t, const std::string& v, double num, const std::string& u = "") 
        : type(t), value(v), unit(u), numeric_value(num) {}
};

// CSS Value Types
enum class ValueType {
    Keyword,        // auto, inherit, initial
    Number,         // 123
    Percentage,     // 50%
    Length,         // 10px, 2em
    Angle,          // 45deg, 1.5rad
    Time,           // 2s, 500ms
    Frequency,      // 44khz
    Resolution,     // 300dpi, 2dppx
    Color,          // #fff, rgb(255,0,0), hsl(120,50%,50%)
    String,         // "Arial", 'Times'
    Url,            // url("image.png")
    Function,       // calc(), var(), etc.
    List,           // comma or space separated values
    Custom          // CSS custom properties
};

struct CSSColor {
    enum Type { RGB, HSL, HWB, LAB, LCH, Named, Hex, Current, Transparent };
    
    Type type = RGB;
    double values[4] = {0, 0, 0, 1}; // r,g,b,a or h,s,l,a etc.
    std::string name; // for named colors
    
    CSSColor() = default;
    CSSColor(Type t, double v1, double v2, double v3, double alpha = 1.0) 
        : type(t) { values[0] = v1; values[1] = v2; values[2] = v3; values[3] = alpha; }
    
    std::string to_string() const;
    static CSSColor from_hex(const std::string& hex);
    static CSSColor from_name(const std::string& name);
};

class CSSValue {
public:
    ValueType type;
    std::string string_value;
    double numeric_value = 0.0;
    std::string unit;
    CSSColor color_value;
    std::vector<CSSValue> list_values;
    std::map<std::string, CSSValue> function_args;
    
    CSSValue(ValueType t = ValueType::Keyword) : type(t) {}
    CSSValue(const std::string& keyword) : type(ValueType::Keyword), string_value(keyword) {}
    CSSValue(ValueType t, const std::string& value) : type(t), string_value(value) {}
    CSSValue(double num, const std::string& u = "") 
        : type(u.empty() ? ValueType::Number : ValueType::Length), 
          numeric_value(num), unit(u) {}
    CSSValue(const CSSColor& color) : type(ValueType::Color), color_value(color) {}
    
    std::string to_string() const;
    bool is_length() const { return type == ValueType::Length; }
    bool is_percentage() const { return type == ValueType::Percentage; }
    bool is_number() const { return type == ValueType::Number; }
    bool is_color() const { return type == ValueType::Color; }
    bool is_keyword() const { return type == ValueType::Keyword; }
    bool is_function() const { return type == ValueType::Function; }
};

// CSS Selector Types
enum class SelectorType {
    Universal,      // *
    Type,           // div, p, h1
    Class,          // .classname
    Id,             // #idname
    Attribute,      // [attr], [attr=value]
    Pseudo,         // :hover, :first-child
    PseudoElement   // ::before, ::after
};

enum class SelectorCombinator {
    None,           // no combinator
    Descendant,     // space
    Child,          // >
    AdjacentSibling,// +
    GeneralSibling  // ~
};

enum class AttributeMatchType {
    Exists,         // [attr]
    Exact,          // [attr=value]
    Include,        // [attr~=value]
    Dash,           // [attr|=value]
    Prefix,         // [attr^=value]
    Suffix,         // [attr$=value]
    Substring       // [attr*=value]
};

struct AttributeSelector {
    std::string name;
    std::string value;
    AttributeMatchType match_type = AttributeMatchType::Exists;
    bool case_insensitive = false;
};

struct PseudoSelector {
    std::string name;
    std::string argument;
    bool is_function = false; // :nth-child(2n+1)
};

class SimpleSelector {
public:
    SelectorType type;
    std::string name;
    AttributeSelector attribute;
    PseudoSelector pseudo;
    
    SimpleSelector(SelectorType t, const std::string& n = "") : type(t), name(n) {}
    std::string to_string() const;
    int specificity() const;
};

class CompoundSelector {
public:
    std::vector<SimpleSelector> selectors;
    
    void add_selector(const SimpleSelector& selector) { selectors.push_back(selector); }
    std::string to_string() const;
    int specificity() const;
    bool empty() const { return selectors.empty(); }
};

class ComplexSelector {
public:
    struct Component {
        CompoundSelector selector;
        SelectorCombinator combinator = SelectorCombinator::None;
    };
    
    std::vector<Component> components;
    
    void add_component(const CompoundSelector& selector, SelectorCombinator combinator = SelectorCombinator::None);
    std::string to_string() const;
    int specificity() const;
    bool empty() const { return components.empty(); }
};

class SelectorList {
public:
    std::vector<ComplexSelector> selectors;
    
    void add_selector(const ComplexSelector& selector) { selectors.push_back(selector); }
    std::string to_string() const;
    int max_specificity() const;
    bool empty() const { return selectors.empty(); }
};

// CSS Declaration
struct CSSDeclaration {
    std::string property;
    CSSValue value;
    bool important = false;
    
    CSSDeclaration() = default;
    CSSDeclaration(const std::string& prop, const CSSValue& val, bool imp = false)
        : property(prop), value(val), important(imp) {}
    
    std::string to_string() const;
};

// CSS Rule Types
enum class RuleType {
    Style,          // selector { declarations }
    AtRule,         // @media, @import, @keyframes, etc.
    Comment         // /* comment */
};

class CSSRule {
public:
    RuleType type;
    size_t start_pos = 0;
    size_t end_pos = 0;
    
    CSSRule(RuleType t) : type(t) {}
    virtual ~CSSRule() = default;
    virtual std::string to_string() const = 0;
    virtual std::unique_ptr<CSSRule> clone() const = 0;
};

class StyleRule : public CSSRule {
public:
    SelectorList selectors;
    std::vector<CSSDeclaration> declarations;
    
    StyleRule() : CSSRule(RuleType::Style) {}
    
    void add_declaration(const CSSDeclaration& decl) { declarations.push_back(decl); }
    std::string to_string() const override;
    std::unique_ptr<CSSRule> clone() const override;
};

class AtRule : public CSSRule {
public:
    std::string name;           // media, import, keyframes, etc.
    std::string prelude;        // everything before { or ;
    std::vector<std::unique_ptr<CSSRule>> rules; // nested rules (for @media, @supports, etc.)
    std::vector<CSSDeclaration> declarations;    // declarations (for @page, @font-face, etc.)
    
    AtRule(const std::string& n) : CSSRule(RuleType::AtRule), name(n) {}
    
    std::string to_string() const override;
    std::unique_ptr<CSSRule> clone() const override;
    bool is_conditional() const; // @media, @supports, @document
    bool is_descriptor() const;  // @font-face, @page, @viewport
    bool is_keyframes() const;   // @keyframes
};

class CommentRule : public CSSRule {
public:
    std::string content;
    
    CommentRule(const std::string& c) : CSSRule(RuleType::Comment), content(c) {}
    
    std::string to_string() const override { return "/* " + content + " */"; }
    std::unique_ptr<CSSRule> clone() const override;
};

// CSS Style Sheet
class CSSStyleSheet {
public:
    std::vector<std::unique_ptr<CSSRule>> rules;
    std::vector<std::string> imports;
    std::string href;
    std::string media;
    bool disabled = false;
    
    void add_rule(std::unique_ptr<CSSRule> rule) { rules.push_back(std::move(rule)); }
    std::string to_string() const;
    size_t rule_count() const { return rules.size(); }
    
    // CSS Object Model methods
    void insert_rule(const std::string& rule_text, size_t index = 0);
    void delete_rule(size_t index);
    std::vector<StyleRule*> get_style_rules() const;
    std::vector<AtRule*> get_at_rules(const std::string& name = "") const;
};

// CSS Parser Error
struct CSSParseError {
    std::string message;
    size_t position;
    size_t line;
    size_t column;
    std::string severity; // "error", "warning", "info"
    
    CSSParseError(const std::string& msg, size_t pos, size_t ln = 0, size_t col = 0, 
                  const std::string& sev = "error")
        : message(msg), position(pos), line(ln), column(col), severity(sev) {}
};

// CSS Tokenizer
class CSSTokenizer {
public:
    explicit CSSTokenizer(const std::string& input);
    
    Token next_token();
    Token peek_token(size_t offset = 0);
    bool at_end() const { return pos_ >= input_.length(); }
    size_t position() const { return pos_; }
    void reset(size_t position = 0);
    
    // Error handling
    const std::vector<CSSParseError>& get_errors() const { return errors_; }
    void add_error(const std::string& message);
    
private:
    std::string input_;
    size_t pos_ = 0;
    size_t line_ = 1;
    size_t column_ = 1;
    std::vector<Token> token_buffer_;
    size_t buffer_pos_ = 0;
    std::vector<CSSParseError> errors_;
    
    char peek(size_t offset = 0) const;
    char consume();
    void skip_whitespace();
    void skip_comment();
    
    Token read_identifier();
    Token read_string(char quote);
    Token read_number();
    Token read_hash();
    Token read_url();
    Token read_unicode_range();
    
    bool is_identifier_start(char c) const;
    bool is_identifier_char(char c) const;
    bool is_digit(char c) const;
    bool is_hex_digit(char c) const;
    bool is_whitespace(char c) const;
    bool is_newline(char c) const;
    
    std::string consume_identifier();
    std::string consume_string(char quote);
    double consume_number();
    void consume_bad_url();
};

// CSS Parser
class CSSParser {
public:
    struct ParseOptions {
        bool strict_mode = false;
        bool preserve_comments = false;
        bool validate_properties = true;
        bool allow_vendor_prefixes = true;
        std::unordered_set<std::string> supported_at_rules;
        
        ParseOptions() {
            // Default supported at-rules
            supported_at_rules = {
                "media", "import", "charset", "namespace", "supports", "page",
                "font-face", "keyframes", "counter-style", "viewport",
                "document", "layer", "container", "scope"
            };
        }
    };
    
    explicit CSSParser(const std::string& css, const ParseOptions& options = ParseOptions());
    
    std::unique_ptr<CSSStyleSheet> parse_stylesheet();
    std::unique_ptr<CSSRule> parse_rule();
    SelectorList parse_selector_list();
    ComplexSelector parse_complex_selector();
    CompoundSelector parse_compound_selector();
    SimpleSelector parse_simple_selector();
    CSSDeclaration parse_declaration();
    CSSValue parse_value();
    CSSValue parse_component_value();
    
    // Specific value parsers
    CSSColor parse_color();
    CSSValue parse_length();
    CSSValue parse_function();
    CSSValue parse_calc_expression();
    CSSValue parse_color_function(const std::string& func_name);
    CSSValue parse_var_function();
    CSSValue parse_generic_function(const std::string& func_name);
    
    // Error handling
    const std::vector<CSSParseError>& get_errors() const { return errors_; }
    bool has_errors() const { return !errors_.empty(); }
    
    // Utility methods
    static bool is_valid_property(const std::string& property);
    static bool is_valid_value_for_property(const std::string& property, const CSSValue& value);
    static std::vector<std::string> get_vendor_prefixes();
    
private:
    CSSTokenizer tokenizer_;
    ParseOptions options_;
    std::vector<CSSParseError> errors_;
    
    // Parsing helpers
    Token consume_token();
    Token peek_token(size_t offset = 0);
    bool consume_if_match(TokenType type);
    bool consume_if_match(const std::string& value);
    void skip_whitespace();
    void add_error(const std::string& message);
    
    // Parse specific constructs
    std::unique_ptr<StyleRule> parse_style_rule();
    std::unique_ptr<AtRule> parse_at_rule();
    void parse_declaration_list(std::vector<CSSDeclaration>& declarations);
    void parse_rule_list(std::vector<std::unique_ptr<CSSRule>>& rules);
    void parse_keyframes_rules(std::vector<std::unique_ptr<CSSRule>>& rules);
    
    // Selector parsing helpers
    AttributeSelector parse_attribute_selector();
    PseudoSelector parse_pseudo_selector();
    SelectorCombinator parse_combinator();
    
    // Value parsing helpers
    CSSValue parse_primitive_value();
    CSSValue parse_numeric_value();
    CSSValue parse_color_value();
    CSSValue parse_string_value();
    CSSValue parse_url_value();
    CSSValue parse_list_value(char separator = ' ');
    
    // CSS3 specific parsers
    CSSValue parse_gradient();
    CSSValue parse_transform();
    CSSValue parse_filter();
    CSSValue parse_animation();
    CSSValue parse_flex_value();
    CSSValue parse_grid_value();
    
    // Validation
    bool validate_declaration(const CSSDeclaration& decl);
    bool is_supported_at_rule(const std::string& name) const;
};

// CSS Pretty Printer
class CSSPrettyPrinter {
public:
    struct FormatOptions {
        int indent_size = 2;
        bool minify = false;
        bool preserve_comments = true;
        bool sort_declarations = false;
        bool vendor_prefix_last = true;
    };
    
    static std::string format(const CSSStyleSheet& stylesheet, const FormatOptions& options);
    static std::string format(const CSSStyleSheet& stylesheet);
    static std::string format_rule(const CSSRule& rule, const FormatOptions& options);
    static std::string format_rule(const CSSRule& rule);
    static std::string format_selector(const SelectorList& selectors);
    static std::string format_declaration(const CSSDeclaration& decl);
    static std::string format_value(const CSSValue& value);
    
private:
    static std::string indent(int level, int size);
    static std::string format_style_rule(const StyleRule& rule, const FormatOptions& options, int indent_level = 0);
    static std::string format_at_rule(const AtRule& rule, const FormatOptions& options, int indent_level = 0);
};

} // namespace CSS3Parser

#endif // CSS_PARSER_H