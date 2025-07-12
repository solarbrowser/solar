#include "HTMLParser.h"
#include <cctype>
#include <algorithm>
#include <functional>
#include <sstream>
#include <iomanip>

namespace HTML5Parser {

const std::unordered_set<std::string> Parser::void_elements_ = {
    "area", "base", "br", "col", "embed", "hr", "img", "input", 
    "link", "meta", "param", "source", "track", "wbr"
};

const std::unordered_set<std::string> Parser::raw_text_elements_ = {
    "script", "style"
};

const std::unordered_set<std::string> Parser::escapable_raw_text_elements_ = {
    "textarea", "title"
};

const std::map<std::string, std::unordered_set<std::string>> Parser::valid_children_ = {
    {"html", {"head", "body"}},
    {"head", {"title", "meta", "link", "style", "script", "base", "noscript"}},
    {"body", {"div", "p", "h1", "h2", "h3", "h4", "h5", "h6", "section", "article", "aside", "nav", "header", "footer", "main"}},
    {"table", {"caption", "colgroup", "thead", "tbody", "tfoot", "tr"}},
    {"tr", {"td", "th"}},
    {"ul", {"li"}},
    {"ol", {"li"}},
    {"dl", {"dt", "dd"}},
    {"select", {"option", "optgroup"}},
    {"optgroup", {"option"}}
};

Parser::Parser(const std::string& html, bool strict_mode) 
    : html_(html), options_{strict_mode} {}

std::unique_ptr<Node> Parser::parse() {
    pos_ = 0;
    errors_.clear();
    return parse_document();
}

std::unique_ptr<Node> Parser::parse_document() {
    auto document = std::make_unique<Node>(NodeType::Document);
    
    while (!at_end()) {
        try {
            auto node = parse_node();
            if (node) {
                document->children.push_back(std::move(node));
            }
        } catch (const ParseError& e) {
            errors_.push_back(e);
            if (options_.strict_mode) {
                throw;
            }
            pos_++; // Skip problematic character
        }
    }
    
    return document;
}

std::unique_ptr<Node> Parser::parse_node() {
    if (at_end()) return nullptr;
    
    consume_whitespace();
    if (at_end()) return nullptr;
    
    if (consume_string("<!DOCTYPE") || consume_string("<!doctype")) {
        return parse_doctype();
    } else if (consume_string("<!--")) {
        return parse_comment();
    } else if (consume_string("<![CDATA[")) {
        return parse_cdata();
    } else if (peek() == '<') {
        return parse_element();
    }
    
    return parse_text();
}

std::unique_ptr<Node> Parser::parse_element() {
    size_t start_pos = pos_;
    
    if (!consume_string("<")) {
        add_error("Expected '<' at start of element");
        return nullptr;
    }
    
    bool is_closing = consume_string("/");
    
    std::string tag_name = consume_while([](char c) {
        return std::isalnum(c) || c == '-' || c == '_' || c == ':';
    });
    
    if (tag_name.empty()) {
        add_error("Empty tag name");
        return nullptr;
    }
    
    tag_name = normalize_tag_name(tag_name);
    
    if (is_closing) {
        consume_whitespace();
        if (!consume_string(">")) {
            add_error("Expected '>' after closing tag");
        }
        return nullptr; // Closing tags are handled by parent
    }
    
    auto node = std::make_unique<Node>(NodeType::Element);
    node->tag_name = tag_name;
    node->start_pos = start_pos;
    
    parse_attributes(node->attributes);
    
    bool self_closing = consume_string("/");
    
    if (!consume_string(">")) {
        add_error("Expected '>' after element opening");
        return node;
    }
    
    ElementCategory category = get_element_category(tag_name);
    
    if (category == ElementCategory::Void || self_closing) {
        node->end_pos = pos_;
        return node;
    }
    
    if (category == ElementCategory::RawText || category == ElementCategory::EscapableRawText) {
        auto text_node = parse_raw_text("</" + tag_name + ">");
        if (text_node) {
            node->children.push_back(std::move(text_node));
        }
    } else {
        while (!at_end()) {
            size_t before_parse = pos_;
            
            if (consume_string("</" + tag_name)) {
                consume_whitespace();
                if (consume_string(">")) {
                    break;
                }
                pos_ = before_parse; // Backtrack
            }
            
            auto child = parse_node();
            if (child) {
                if (options_.validate_nesting && 
                    !is_valid_child(tag_name, child->tag_name)) {
                    add_error("Invalid child '" + child->tag_name + "' in '" + tag_name + "'");
                }
                node->children.push_back(std::move(child));
            }
            
            if (pos_ == before_parse) {
                pos_++; // Avoid infinite loop
            }
        }
    }
    
    node->end_pos = pos_;
    return node;
}

std::unique_ptr<Node> Parser::parse_text() {
    size_t start_pos = pos_;
    
    std::string text = consume_while([](char c) { return c != '<'; });
    
    if (text.empty()) return nullptr;
    
    if (!options_.preserve_whitespace) {
        // Trim and normalize whitespace
        text.erase(0, text.find_first_not_of(" \t\n\r"));
        text.erase(text.find_last_not_of(" \t\n\r") + 1);
        if (text.empty()) return nullptr;
    }
    
    auto node = std::make_unique<Node>(NodeType::Text);
    node->text_content = text;
    node->start_pos = start_pos;
    node->end_pos = pos_;
    
    return node;
}

std::unique_ptr<Node> Parser::parse_comment() {
    size_t start_pos = pos_ - 4; // Account for already consumed "<!--"
    
    std::string comment_text;
    while (!at_end() && !consume_string("-->")) {
        comment_text += peek();
        pos_++;
    }
    
    auto node = std::make_unique<Node>(NodeType::Comment);
    node->text_content = comment_text;
    node->start_pos = start_pos;
    node->end_pos = pos_;
    
    return node;
}

std::unique_ptr<Node> Parser::parse_doctype() {
    size_t start_pos = pos_ - 9; // Account for already consumed "<!DOCTYPE"
    
    consume_whitespace();
    
    std::string doctype_content = consume_while([](char c) { return c != '>'; });
    
    if (!consume_string(">")) {
        add_error("Expected '>' after DOCTYPE");
    }
    
    auto node = std::make_unique<Node>(NodeType::Doctype);
    node->text_content = doctype_content;
    node->start_pos = start_pos;
    node->end_pos = pos_;
    
    return node;
}

std::unique_ptr<Node> Parser::parse_cdata() {
    size_t start_pos = pos_ - 9; // Account for already consumed "<![CDATA["
    
    std::string cdata_content;
    while (!at_end() && !consume_string("]]")) {
        if (peek() == '>') {
            pos_++;
            break;
        }
        cdata_content += peek();
        pos_++;
    }
    
    auto node = std::make_unique<Node>(NodeType::CData);
    node->text_content = cdata_content;
    node->start_pos = start_pos;
    node->end_pos = pos_;
    
    return node;
}

std::unique_ptr<Node> Parser::parse_raw_text(const std::string& end_tag) {
    size_t start_pos = pos_;
    std::string content;
    
    while (!at_end() && !consume_string(end_tag)) {
        content += peek();
        pos_++;
    }
    
    if (content.empty()) return nullptr;
    
    auto node = std::make_unique<Node>(NodeType::Text);
    node->text_content = content;
    node->start_pos = start_pos;
    node->end_pos = pos_;
    
    return node;
}

void Parser::parse_attributes(std::map<std::string, std::string>& attributes) {
    while (!at_end()) {
        consume_whitespace();
        
        if (peek() == '>' || peek() == '/') break;
        
        std::string name = consume_while([](char c) {
            return std::isalnum(c) || c == '-' || c == '_' || c == ':';
        });
        
        if (name.empty()) {
            pos_++; // Skip invalid character
            continue;
        }
        
        name = options_.case_sensitive ? name : 
               std::string(name.begin(), name.end()); // Copy for transform
        if (!options_.case_sensitive) {
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        }
        
        consume_whitespace();
        
        if (consume_string("=")) {
            std::string value = parse_attribute_value();
            attributes[name] = value;
        } else {
            attributes[name] = "";
        }
    }
}

std::string Parser::parse_attribute_value() {
    consume_whitespace();
    
    if (peek() == '"') {
        pos_++; // Consume opening quote
        std::string value = consume_while([](char c) { return c != '"'; });
        if (consume_string("\"")) {
            // Successfully consumed closing quote
        } else {
            add_error("Unterminated quoted attribute value");
        }
        return value;
    } else if (peek() == '\'') {
        pos_++; // Consume opening quote
        std::string value = consume_while([](char c) { return c != '\''; });
        if (consume_string("'")) {
            // Successfully consumed closing quote
        } else {
            add_error("Unterminated quoted attribute value");
        }
        return value;
    } else {
        // Unquoted attribute value
        return consume_while([](char c) {
            return !std::isspace(c) && c != '>' && c != '/';
        });
    }
}

std::string Parser::consume_while(const std::function<bool(char)>& predicate) {
    size_t start_pos = pos_;
    while (!at_end() && predicate(peek())) {
        pos_++;
    }
    return html_.substr(start_pos, pos_ - start_pos);
}

void Parser::consume_whitespace() {
    consume_while([](char c) { return std::isspace(c); });
}

bool Parser::consume_string(const std::string& str) {
    if (pos_ + str.length() > html_.length()) return false;
    
    std::string substr = html_.substr(pos_, str.length());
    if (!options_.case_sensitive) {
        std::transform(substr.begin(), substr.end(), substr.begin(), ::tolower);
        std::string lower_str = str;
        std::transform(lower_str.begin(), lower_str.end(), lower_str.begin(), ::tolower);
        if (substr == lower_str) {
            pos_ += str.length();
            return true;
        }
    } else if (substr == str) {
        pos_ += str.length();
        return true;
    }
    
    return false;
}

char Parser::peek(size_t offset) const {
    size_t index = pos_ + offset;
    return (index < html_.length()) ? html_[index] : '\0';
}

ElementCategory Parser::get_element_category(const std::string& tag_name) const {
    if (void_elements_.count(tag_name)) {
        return ElementCategory::Void;
    } else if (raw_text_elements_.count(tag_name)) {
        return ElementCategory::RawText;
    } else if (escapable_raw_text_elements_.count(tag_name)) {
        return ElementCategory::EscapableRawText;
    }
    return ElementCategory::Container;
}

bool Parser::is_valid_child(const std::string& parent, const std::string& child) const {
    auto it = valid_children_.find(parent);
    if (it == valid_children_.end()) {
        return true; // Allow if no rules defined
    }
    return it->second.count(child) > 0;
}

void Parser::add_error(const std::string& message) {
    errors_.emplace_back(message, pos_);
}

std::string Parser::normalize_tag_name(const std::string& name) const {
    if (options_.case_sensitive) return name;
    
    std::string result = name;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

std::string PrettyPrinter::print(const Node& node, int indent_size) {
    std::string result;
    print_node(node, result, 0, indent_size);
    return result;
}

std::string PrettyPrinter::print_json(const Node& node, int indent_size) {
    std::string result;
    print_json_node(node, result, 0, indent_size);
    return result;
}

void PrettyPrinter::print_node(const Node& node, std::string& result, int indent, int indent_size) {
    std::string indent_str(indent * indent_size, ' ');
    
    switch (node.type) {
        case NodeType::Document:
            result += indent_str + "Document:\n";
            break;
        case NodeType::Element:
            result += indent_str + "Element: " + node.tag_name;
            if (!node.attributes.empty()) {
                result += " [";
                bool first = true;
                for (const auto& attr : node.attributes) {
                    if (!first) result += ", ";
                    result += attr.first + "=\"" + attr.second + "\"";
                    first = false;
                }
                result += "]";
            }
            result += "\n";
            break;
        case NodeType::Text:
            if (!node.text_content.empty()) {
                result += indent_str + "Text: \"" + node.text_content + "\"\n";
            }
            break;
        case NodeType::Comment:
            result += indent_str + "Comment: \"" + node.text_content + "\"\n";
            break;
        case NodeType::Doctype:
            result += indent_str + "DOCTYPE: \"" + node.text_content + "\"\n";
            break;
        case NodeType::CData:
            result += indent_str + "CDATA: \"" + node.text_content + "\"\n";
            break;
    }
    
    for (const auto& child : node.children) {
        print_node(*child, result, indent + 1, indent_size);
    }
}

void PrettyPrinter::print_json_node(const Node& node, std::string& result, int indent, int indent_size) {
    std::string indent_str(indent * indent_size, ' ');
    
    result += indent_str + "{\n";
    result += indent_str + "  \"type\": \"";
    
    switch (node.type) {
        case NodeType::Document: result += "document"; break;
        case NodeType::Element: result += "element"; break;
        case NodeType::Text: result += "text"; break;
        case NodeType::Comment: result += "comment"; break;
        case NodeType::Doctype: result += "doctype"; break;
        case NodeType::CData: result += "cdata"; break;
    }
    
    result += "\",\n";
    
    if (!node.tag_name.empty()) {
        result += indent_str + "  \"tagName\": \"" + escape_json_string(node.tag_name) + "\",\n";
    }
    
    if (!node.text_content.empty()) {
        result += indent_str + "  \"textContent\": \"" + escape_json_string(node.text_content) + "\",\n";
    }
    
    if (!node.attributes.empty()) {
        result += indent_str + "  \"attributes\": {\n";
        bool first = true;
        for (const auto& attr : node.attributes) {
            if (!first) result += ",\n";
            result += indent_str + "    \"" + escape_json_string(attr.first) + 
                     "\": \"" + escape_json_string(attr.second) + "\"";
            first = false;
        }
        result += "\n" + indent_str + "  },\n";
    }
    
    if (!node.children.empty()) {
        result += indent_str + "  \"children\": [\n";
        for (size_t i = 0; i < node.children.size(); ++i) {
            if (i > 0) result += ",\n";
            print_json_node(*node.children[i], result, indent + 1, indent_size);
        }
        result += "\n" + indent_str + "  ]\n";
    } else {
        result.pop_back(); result.pop_back(); // Remove trailing ",\n"
        result += "\n";
    }
    
    result += indent_str + "}";
}

std::string PrettyPrinter::escape_json_string(const std::string& str) {
    std::string result;
    for (char c : str) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result += c; break;
        }
    }
    return result;
}

} // namespace HTML5Parser
