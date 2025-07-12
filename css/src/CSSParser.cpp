#include "CSSParser.h"
#include <algorithm>
#include <sstream>
#include <unordered_set>
#include <regex>

namespace CSS3Parser {

// Known CSS properties for validation
static const std::unordered_set<std::string> valid_properties = {
    // Layout properties
    "display", "position", "top", "right", "bottom", "left", "z-index",
    "float", "clear", "visibility", "overflow", "overflow-x", "overflow-y",
    "clip", "clip-path",
    
    // Box model
    "width", "height", "min-width", "min-height", "max-width", "max-height",
    "margin", "margin-top", "margin-right", "margin-bottom", "margin-left",
    "padding", "padding-top", "padding-right", "padding-bottom", "padding-left",
    "border", "border-width", "border-style", "border-color",
    "border-top", "border-right", "border-bottom", "border-left",
    "border-top-width", "border-right-width", "border-bottom-width", "border-left-width",
    "border-top-style", "border-right-style", "border-bottom-style", "border-left-style",
    "border-top-color", "border-right-color", "border-bottom-color", "border-left-color",
    "border-radius", "border-top-left-radius", "border-top-right-radius",
    "border-bottom-left-radius", "border-bottom-right-radius",
    "box-shadow", "box-sizing",
    
    // Background
    "background", "background-color", "background-image", "background-repeat",
    "background-position", "background-size", "background-attachment",
    "background-origin", "background-clip", "background-blend-mode",
    
    // Typography
    "font", "font-family", "font-size", "font-weight", "font-style",
    "font-variant", "font-stretch", "line-height", "letter-spacing",
    "word-spacing", "text-align", "text-decoration", "text-transform",
    "text-indent", "text-shadow", "white-space", "word-wrap", "word-break",
    "text-overflow", "vertical-align",
    
    // Color
    "color", "opacity",
    
    // Flexbox
    "flex", "flex-direction", "flex-wrap", "flex-flow", "justify-content",
    "align-items", "align-content", "align-self", "flex-grow", "flex-shrink",
    "flex-basis", "order",
    
    // Grid
    "grid", "grid-template", "grid-template-rows", "grid-template-columns",
    "grid-template-areas", "grid-auto-rows", "grid-auto-columns", "grid-auto-flow",
    "grid-row", "grid-column", "grid-area", "grid-row-start", "grid-row-end",
    "grid-column-start", "grid-column-end", "gap", "row-gap", "column-gap",
    "grid-gap", "grid-row-gap", "grid-column-gap",
    
    // Transforms
    "transform", "transform-origin", "transform-style", "perspective",
    "perspective-origin", "backface-visibility",
    
    // Transitions & Animations
    "transition", "transition-property", "transition-duration", "transition-timing-function",
    "transition-delay", "animation", "animation-name", "animation-duration",
    "animation-timing-function", "animation-delay", "animation-iteration-count",
    "animation-direction", "animation-fill-mode", "animation-play-state",
    
    // Filters & Effects
    "filter", "backdrop-filter", "mix-blend-mode", "isolation",
    
    // Table
    "table-layout", "border-collapse", "border-spacing", "caption-side",
    "empty-cells",
    
    // Lists
    "list-style", "list-style-type", "list-style-position", "list-style-image",
    
    // Content
    "content", "quotes", "counter-reset", "counter-increment",
    
    // User Interface
    "cursor", "outline", "outline-width", "outline-style", "outline-color",
    "outline-offset", "resize", "user-select", "pointer-events",
    
    // Multi-column
    "columns", "column-count", "column-width", "column-gap", "column-rule",
    "column-rule-width", "column-rule-style", "column-rule-color",
    "column-span", "column-fill", "break-before", "break-after", "break-inside",
    
    // CSS3 additions
    "border-image", "border-image-source", "border-image-slice", "border-image-width",
    "border-image-outset", "border-image-repeat", "mask", "mask-image", "mask-mode",
    "mask-repeat", "mask-position", "mask-clip", "mask-origin", "mask-size",
    "mask-composite", "object-fit", "object-position", "image-rendering",
    "shape-outside", "shape-margin", "shape-image-threshold", "scroll-behavior",
    "scroll-snap-type", "scroll-snap-align", "overscroll-behavior", "touch-action",
    
    // Custom properties
    "--*"
};

static const std::unordered_set<std::string> vendor_prefixes = {
    "-webkit-", "-moz-", "-ms-", "-o-"
};

CSSParser::CSSParser(const std::string& css, const ParseOptions& options)
    : tokenizer_(css), options_(options) {}

std::unique_ptr<CSSStyleSheet> CSSParser::parse_stylesheet() {
    auto stylesheet = std::make_unique<CSSStyleSheet>();
    
    while (!tokenizer_.at_end()) {
        skip_whitespace();
        
        if (tokenizer_.at_end()) break;
        
        Token token = peek_token();
        
        if (token.type == TokenType::AtKeyword) {
            if (token.value == "import") {
                // Handle @import specially
                consume_token(); // consume @import
                skip_whitespace();
                
                Token url_token = consume_token();
                if (url_token.type == TokenType::Url || url_token.type == TokenType::String) {
                    stylesheet->imports.push_back(url_token.value);
                    
                    // Consume until semicolon
                    while (!tokenizer_.at_end() && peek_token().type != TokenType::Semicolon) {
                        consume_token();
                    }
                    if (peek_token().type == TokenType::Semicolon) {
                        consume_token();
                    }
                }
            } else {
                auto rule = parse_at_rule();
                if (rule) {
                    stylesheet->add_rule(std::move(rule));
                }
            }
        } else if (token.type == TokenType::Comment && options_.preserve_comments) {
            auto comment_rule = std::make_unique<CommentRule>(token.value);
            stylesheet->add_rule(std::move(comment_rule));
            consume_token();
        } else if (token.type == TokenType::Comment) {
            consume_token(); // Skip comments
        } else {
            auto rule = parse_style_rule();
            if (rule) {
                stylesheet->add_rule(std::move(rule));
            }
        }
    }
    
    return stylesheet;
}

std::unique_ptr<CSSRule> CSSParser::parse_rule() {
    skip_whitespace();
    
    Token token = peek_token();
    if (token.type == TokenType::AtKeyword) {
        return parse_at_rule();
    } else {
        return parse_style_rule();
    }
}

std::unique_ptr<StyleRule> CSSParser::parse_style_rule() {
    auto rule = std::make_unique<StyleRule>();
    rule->start_pos = tokenizer_.position();
    
    // Parse selector list
    rule->selectors = parse_selector_list();
    
    if (rule->selectors.empty()) {
        add_error("Expected selector before '{'");
        return nullptr;
    }
    
    skip_whitespace();
    
    if (!consume_if_match(TokenType::LeftBrace)) {
        add_error("Expected '{' after selector");
        return nullptr;
    }
    
    // Parse declarations
    parse_declaration_list(rule->declarations);
    
    if (!consume_if_match(TokenType::RightBrace)) {
        add_error("Expected '}' after declarations");
    }
    
    rule->end_pos = tokenizer_.position();
    return rule;
}

std::unique_ptr<AtRule> CSSParser::parse_at_rule() {
    Token at_token = consume_token(); // consume @keyword
    
    if (at_token.type != TokenType::AtKeyword) {
        add_error("Expected at-rule keyword");
        return nullptr;
    }
    
    auto rule = std::make_unique<AtRule>(at_token.value);
    rule->start_pos = tokenizer_.position();
    
    if (!is_supported_at_rule(at_token.value)) {
        add_error("Unsupported at-rule: @" + at_token.value);
    }
    
    // Parse prelude (everything before { or ;)
    std::ostringstream prelude;
    while (!tokenizer_.at_end()) {
        Token token = peek_token();
        if (token.type == TokenType::LeftBrace || token.type == TokenType::Semicolon) {
            break;
        }
        prelude << consume_token().value << " ";
    }
    rule->prelude = prelude.str();
    
    // Trim trailing whitespace
    rule->prelude.erase(rule->prelude.find_last_not_of(" \\t\\n\\r") + 1);
    
    Token next = peek_token();
    if (next.type == TokenType::LeftBrace) {
        consume_token(); // consume {
        
        if (rule->is_conditional()) {
            // Parse nested rules
            parse_rule_list(rule->rules);
        } else if (rule->is_descriptor()) {
            // Parse declarations
            parse_declaration_list(rule->declarations);
        } else {
            // Generic block parsing - could be rules or declarations
            // Try to determine based on content
            while (!tokenizer_.at_end() && peek_token().type != TokenType::RightBrace) {
                Token token = peek_token();
                if (token.type == TokenType::AtKeyword || 
                    (token.type == TokenType::Ident && 
                     std::find_if(peek_token(5).value.begin(), peek_token(5).value.end(), 
                                  [](char c) { return c == '{'; }) != peek_token(5).value.end())) {
                    // Looks like a rule
                    auto nested_rule = parse_rule();
                    if (nested_rule) {
                        rule->rules.push_back(std::move(nested_rule));
                    }
                } else {
                    // Looks like a declaration
                    auto decl = parse_declaration();
                    if (!decl.property.empty()) {
                        rule->declarations.push_back(decl);
                    }
                    
                    if (peek_token().type == TokenType::Semicolon) {
                        consume_token();
                    }
                }
                skip_whitespace();
            }
        }
        
        if (!consume_if_match(TokenType::RightBrace)) {
            add_error("Expected '}' after at-rule block");
        }
    } else if (next.type == TokenType::Semicolon) {
        consume_token(); // consume ;
    }
    
    rule->end_pos = tokenizer_.position();
    return rule;
}

void CSSParser::parse_declaration_list(std::vector<CSSDeclaration>& declarations) {
    while (!tokenizer_.at_end() && peek_token().type != TokenType::RightBrace) {
        skip_whitespace();
        
        if (peek_token().type == TokenType::RightBrace) break;
        
        auto decl = parse_declaration();
        if (!decl.property.empty()) {
            if (options_.validate_properties && !is_valid_property(decl.property)) {
                add_error("Unknown property: " + decl.property);
            }
            
            declarations.push_back(decl);
        }
        
        // Consume semicolon if present
        if (peek_token().type == TokenType::Semicolon) {
            consume_token();
        }
        
        skip_whitespace();
    }
}

void CSSParser::parse_rule_list(std::vector<std::unique_ptr<CSSRule>>& rules) {
    while (!tokenizer_.at_end() && peek_token().type != TokenType::RightBrace) {
        skip_whitespace();
        
        if (peek_token().type == TokenType::RightBrace) break;
        
        auto rule = parse_rule();
        if (rule) {
            rules.push_back(std::move(rule));
        }
    }
}

CSSDeclaration CSSParser::parse_declaration() {
    CSSDeclaration decl;
    
    skip_whitespace();
    
    Token property_token = consume_token();
    if (property_token.type != TokenType::Ident) {
        add_error("Expected property name");
        return decl;
    }
    
    decl.property = property_token.value;
    
    skip_whitespace();
    
    if (!consume_if_match(TokenType::Colon)) {
        add_error("Expected ':' after property name");
        return decl;
    }
    
    skip_whitespace();
    
    decl.value = parse_value();
    
    // Check for !important
    skip_whitespace();
    Token next = peek_token();
    if (next.type == TokenType::Delim && next.value == "!") {
        consume_token(); // consume !
        Token important = consume_token();
        if (important.type == TokenType::Ident && important.value == "important") {
            decl.important = true;
        } else {
            add_error("Expected 'important' after '!'");
        }
    }
    
    return decl;
}

SelectorList CSSParser::parse_selector_list() {
    SelectorList list;
    
    do {
        auto selector = parse_complex_selector();
        if (!selector.empty()) {
            list.add_selector(selector);
        } else {
            break;
        }
        
        skip_whitespace();
        
        if (peek_token().type == TokenType::Comma) {
            consume_token(); // consume comma
            skip_whitespace();
        } else {
            break;
        }
    } while (!tokenizer_.at_end());
    
    return list;
}

ComplexSelector CSSParser::parse_complex_selector() {
    ComplexSelector complex;
    SelectorCombinator combinator = SelectorCombinator::None;
    
    while (!tokenizer_.at_end()) {
        auto compound = parse_compound_selector();
        if (compound.empty()) {
            break;
        }
        
        complex.add_component(compound, combinator);
        
        skip_whitespace();
        combinator = parse_combinator();
        
        if (combinator == SelectorCombinator::None) {
            // Check if there's a following selector without explicit combinator
            Token next = peek_token();
            if (next.type == TokenType::Ident || next.type == TokenType::Hash ||
                next.type == TokenType::Delim || next.type == TokenType::LeftSquare ||
                next.type == TokenType::Colon) {
                combinator = SelectorCombinator::Descendant;
            } else {
                break;
            }
        } else {
            skip_whitespace();
        }
    }
    
    return complex;
}

CompoundSelector CSSParser::parse_compound_selector() {
    CompoundSelector compound;
    
    while (!tokenizer_.at_end()) {
        auto simple = parse_simple_selector();
        if (simple.type == SelectorType::Universal && simple.name.empty()) {
            break; // No valid selector found
        }
        
        compound.add_selector(simple);
        
        // Check if next token can be part of compound selector
        Token next = peek_token();
        if (next.type != TokenType::Hash && next.type != TokenType::Delim &&
            next.type != TokenType::LeftSquare && next.type != TokenType::Colon) {
            break;
        }
    }
    
    return compound;
}

SimpleSelector CSSParser::parse_simple_selector() {
    Token token = peek_token();
    
    switch (token.type) {
        case TokenType::Delim:
            if (token.value == "*") {
                consume_token();
                return SimpleSelector(SelectorType::Universal, "*");
            } else if (token.value == ".") {
                consume_token(); // consume .
                Token class_name = consume_token();
                if (class_name.type == TokenType::Ident) {
                    return SimpleSelector(SelectorType::Class, class_name.value);
                }
                add_error("Expected class name after '.'");
            }
            break;
            
        case TokenType::Hash:
            consume_token();
            return SimpleSelector(SelectorType::Id, token.value.substr(1)); // Remove #
            
        case TokenType::Ident:
            consume_token();
            return SimpleSelector(SelectorType::Type, token.value);
            
        case TokenType::LeftSquare: {
            auto attr = parse_attribute_selector();
            SimpleSelector selector(SelectorType::Attribute);
            selector.attribute = attr;
            return selector;
        }
        
        case TokenType::Colon: {
            auto pseudo = parse_pseudo_selector();
            SelectorType type = pseudo.name.find("::") == 0 ? 
                              SelectorType::PseudoElement : SelectorType::Pseudo;
            SimpleSelector selector(type);
            selector.pseudo = pseudo;
            return selector;
        }
        
        default:
            break;
    }
    
    return SimpleSelector(SelectorType::Universal); // Invalid selector
}

AttributeSelector CSSParser::parse_attribute_selector() {
    AttributeSelector attr;
    
    if (!consume_if_match(TokenType::LeftSquare)) {
        add_error("Expected '[' for attribute selector");
        return attr;
    }
    
    skip_whitespace();
    
    Token name_token = consume_token();
    if (name_token.type != TokenType::Ident) {
        add_error("Expected attribute name");
        return attr;
    }
    
    attr.name = name_token.value;
    attr.match_type = AttributeMatchType::Exists;
    
    skip_whitespace();
    
    // Check for attribute matching operator
    Token op = peek_token();
    if (op.type == TokenType::Delim && op.value == "=") {
        consume_token();
        attr.match_type = AttributeMatchType::Exact;
    } else if (op.type == TokenType::IncludeMatch) {
        consume_token();
        attr.match_type = AttributeMatchType::Include;
    } else if (op.type == TokenType::DashMatch) {
        consume_token();
        attr.match_type = AttributeMatchType::Dash;
    } else if (op.type == TokenType::PrefixMatch) {
        consume_token();
        attr.match_type = AttributeMatchType::Prefix;
    } else if (op.type == TokenType::SuffixMatch) {
        consume_token();
        attr.match_type = AttributeMatchType::Suffix;
    } else if (op.type == TokenType::SubstringMatch) {
        consume_token();
        attr.match_type = AttributeMatchType::Substring;
    }
    
    if (attr.match_type != AttributeMatchType::Exists) {
        skip_whitespace();
        
        Token value_token = consume_token();
        if (value_token.type == TokenType::String || value_token.type == TokenType::Ident) {
            attr.value = value_token.value;
            
            skip_whitespace();
            
            // Check for case-insensitive flag
            Token flag = peek_token();
            if (flag.type == TokenType::Ident && (flag.value == "i" || flag.value == "I")) {
                consume_token();
                attr.case_insensitive = true;
            }
        } else {
            add_error("Expected attribute value");
        }
    }
    
    skip_whitespace();
    
    if (!consume_if_match(TokenType::RightSquare)) {
        add_error("Expected ']' after attribute selector");
    }
    
    return attr;
}

PseudoSelector CSSParser::parse_pseudo_selector() {
    PseudoSelector pseudo;
    
    if (!consume_if_match(TokenType::Colon)) {
        add_error("Expected ':' for pseudo selector");
        return pseudo;
    }
    
    // Check for double colon (pseudo-element)
    if (peek_token().type == TokenType::Colon) {
        consume_token();
        pseudo.name = "::";
    } else {
        pseudo.name = ":";
    }
    
    Token name_token = consume_token();
    if (name_token.type == TokenType::Ident) {
        pseudo.name += name_token.value;
    } else if (name_token.type == TokenType::Function) {
        pseudo.name += name_token.value;
        pseudo.is_function = true;
        
        // Parse function argument
        std::ostringstream arg;
        int paren_depth = 1;
        
        while (!tokenizer_.at_end() && paren_depth > 0) {
            Token token = consume_token();
            if (token.type == TokenType::LeftParen) {
                paren_depth++;
            } else if (token.type == TokenType::RightParen) {
                paren_depth--;
                if (paren_depth == 0) break;
            }
            arg << token.value;
        }
        
        pseudo.argument = arg.str();
    } else {
        add_error("Expected pseudo-class or pseudo-element name");
    }
    
    return pseudo;
}

SelectorCombinator CSSParser::parse_combinator() {
    Token token = peek_token();
    
    if (token.type == TokenType::Delim) {
        if (token.value == ">") {
            consume_token();
            return SelectorCombinator::Child;
        } else if (token.value == "+") {
            consume_token();
            return SelectorCombinator::AdjacentSibling;
        } else if (token.value == "~") {
            consume_token();
            return SelectorCombinator::GeneralSibling;
        }
    }
    
    return SelectorCombinator::None;
}

CSSValue CSSParser::parse_value() {
    std::vector<CSSValue> values;
    
    while (!tokenizer_.at_end()) {
        Token token = peek_token();
        
        if (token.type == TokenType::Semicolon || token.type == TokenType::RightBrace ||
            token.type == TokenType::RightParen || token.type == TokenType::Comma ||
            (token.type == TokenType::Delim && token.value == "!")) {
            break;
        }
        
        auto component = parse_component_value();
        if (component.type != ValueType::Keyword || !component.string_value.empty()) {
            values.push_back(component);
        } else {
            break;
        }
        
        skip_whitespace();
    }
    
    if (values.empty()) {
        return CSSValue(""); // Empty value
    } else if (values.size() == 1) {
        return values[0];
    } else {
        CSSValue list_value(ValueType::List);
        list_value.list_values = values;
        return list_value;
    }
}

CSSValue CSSParser::parse_component_value() {
    Token token = peek_token();
    
    switch (token.type) {
        case TokenType::Ident:
            consume_token();
            return CSSValue(token.value);
            
        case TokenType::Number:
            consume_token();
            return CSSValue(token.numeric_value);
            
        case TokenType::Percentage:
            consume_token();
            return CSSValue(token.numeric_value, "%");
            
        case TokenType::Dimension:
            consume_token();
            return CSSValue(token.numeric_value, token.unit);
            
        case TokenType::String:
            consume_token();
            return CSSValue(ValueType::String, token.value);
            
        case TokenType::Url:
            consume_token();
            return CSSValue(ValueType::Url, token.value);
            
        case TokenType::Hash:
            return parse_color();
            
        case TokenType::Function:
            return parse_function();
            
        default:
            consume_token(); // Skip unknown token
            return CSSValue("");
    }
}

CSSColor CSSParser::parse_color() {
    Token token = consume_token();
    
    if (token.type == TokenType::Hash) {
        return CSSColor::from_hex(token.value);
    } else if (token.type == TokenType::Ident) {
        return CSSColor::from_name(token.value);
    }
    
    return CSSColor(); // Default color (transparent)
}

CSSValue CSSParser::parse_function() {
    Token func_token = consume_token();
    
    if (func_token.type != TokenType::Function) {
        add_error("Expected function");
        return CSSValue("");
    }
    
    std::string func_name = func_token.value;
    
    // Handle specific function types
    if (func_name == "rgb" || func_name == "rgba" || 
        func_name == "hsl" || func_name == "hsla" ||
        func_name == "hwb" || func_name == "lab" || func_name == "lch") {
        return parse_color_function(func_name);
    } else if (func_name == "calc") {
        return parse_calc_expression();
    } else if (func_name == "var") {
        return parse_var_function();
    } else {
        return parse_generic_function(func_name);
    }
}

CSSValue CSSParser::parse_color_function(const std::string& func_name) {
    std::vector<double> values;
    
    // Parse function arguments
    while (!tokenizer_.at_end() && peek_token().type != TokenType::RightParen) {
        skip_whitespace();
        
        Token token = consume_token();
        if (token.type == TokenType::Number) {
            values.push_back(token.numeric_value);
        } else if (token.type == TokenType::Percentage) {
            values.push_back(token.numeric_value);
        }
        
        skip_whitespace();
        
        // Skip comma if present
        if (peek_token().type == TokenType::Comma) {
            consume_token();
        }
    }
    
    if (peek_token().type == TokenType::RightParen) {
        consume_token(); // consume )
    }
    
    // Create color based on function type
    CSSColor color;
    if (func_name == "rgb" || func_name == "rgba") {
        color.type = CSSColor::RGB;
        if (values.size() >= 3) {
            color.values[0] = values[0];
            color.values[1] = values[1];
            color.values[2] = values[2];
            color.values[3] = values.size() > 3 ? values[3] : 1.0;
        }
    } else if (func_name == "hsl" || func_name == "hsla") {
        color.type = CSSColor::HSL;
        if (values.size() >= 3) {
            color.values[0] = values[0];
            color.values[1] = values[1];
            color.values[2] = values[2];
            color.values[3] = values.size() > 3 ? values[3] : 1.0;
        }
    }
    
    return CSSValue(color);
}

CSSValue CSSParser::parse_var_function() {
    CSSValue var_value(ValueType::Function);
    var_value.string_value = "var";
    
    skip_whitespace();
    
    // Parse custom property name
    Token name_token = consume_token();
    if (name_token.type == TokenType::Ident) {
        var_value.function_args["name"] = CSSValue(name_token.value);
        
        skip_whitespace();
        
        // Parse fallback value if present
        if (peek_token().type == TokenType::Comma) {
            consume_token(); // consume comma
            skip_whitespace();
            
            auto fallback = parse_value();
            var_value.function_args["fallback"] = fallback;
        }
    }
    
    if (peek_token().type == TokenType::RightParen) {
        consume_token(); // consume )
    }
    
    return var_value;
}

CSSValue CSSParser::parse_generic_function(const std::string& func_name) {
    CSSValue func_value(ValueType::Function);
    func_value.string_value = func_name;
    
    std::vector<CSSValue> args;
    
    while (!tokenizer_.at_end() && peek_token().type != TokenType::RightParen) {
        skip_whitespace();
        
        auto arg = parse_component_value();
        args.push_back(arg);
        
        skip_whitespace();
        
        if (peek_token().type == TokenType::Comma) {
            consume_token();
        }
    }
    
    if (peek_token().type == TokenType::RightParen) {
        consume_token(); // consume )
    }
    
    // Store arguments as list
    CSSValue args_list(ValueType::List);
    args_list.list_values = args;
    func_value.function_args["args"] = args_list;
    
    return func_value;
}

CSSValue CSSParser::parse_calc_expression() {
    CSSValue calc_value(ValueType::Function);
    calc_value.string_value = "calc";
    
    std::ostringstream expr;
    int paren_depth = 1;
    
    while (!tokenizer_.at_end() && paren_depth > 0) {
        Token token = consume_token();
        if (token.type == TokenType::LeftParen) {
            paren_depth++;
        } else if (token.type == TokenType::RightParen) {
            paren_depth--;
            if (paren_depth == 0) break;
        }
        expr << token.value << " ";
    }
    
    calc_value.function_args["expression"] = CSSValue(expr.str());
    return calc_value;
}

Token CSSParser::consume_token() {
    return tokenizer_.next_token();
}

Token CSSParser::peek_token(size_t offset) {
    return tokenizer_.peek_token(offset);
}

bool CSSParser::consume_if_match(TokenType type) {
    if (peek_token().type == type) {
        consume_token();
        return true;
    }
    return false;
}

bool CSSParser::consume_if_match(const std::string& value) {
    if (peek_token().value == value) {
        consume_token();
        return true;
    }
    return false;
}

void CSSParser::skip_whitespace() {
    while (peek_token().type == TokenType::Whitespace) {
        consume_token();
    }
}

void CSSParser::add_error(const std::string& message) {
    errors_.emplace_back(message, tokenizer_.position());
}

bool CSSParser::is_valid_property(const std::string& property) {
    // Check for custom properties
    if (property.substr(0, 2) == "--") {
        return true;
    }
    
    // Check for vendor prefixes
    for (const auto& prefix : vendor_prefixes) {
        if (property.substr(0, prefix.length()) == prefix) {
            std::string unprefixed = property.substr(prefix.length());
            return valid_properties.count(unprefixed) > 0 || valid_properties.count(property) > 0;
        }
    }
    
    return valid_properties.count(property) > 0;
}

bool CSSParser::is_valid_value_for_property(const std::string& property, const CSSValue& value) {
    // This is a simplified validation - a complete implementation would be much more complex
    return true;
}

std::vector<std::string> CSSParser::get_vendor_prefixes() {
    return {"-webkit-", "-moz-", "-ms-", "-o-"};
}

bool CSSParser::is_supported_at_rule(const std::string& name) const {
    return options_.supported_at_rules.count(name) > 0;
}

} // namespace CSS3Parser