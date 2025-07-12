#ifndef HTML_PARSER_H
#define HTML_PARSER_H

#include <string>
#include <vector>
#include <map>
#include <unordered_set>
#include <memory>
#include <optional>
#include <stdexcept>
#include <functional>

namespace HTML5Parser {

enum class NodeType {
    Document,
    Element,
    Text,
    Comment,
    Doctype,
    CData
};

enum class ElementCategory {
    Void,           // Self-closing elements
    Container,      // Regular container elements
    RawText,        // script, style
    EscapableRawText // textarea, title
};

struct ParseError : public std::runtime_error {
    size_t position;
    ParseError(const std::string& message, size_t pos) 
        : std::runtime_error(message), position(pos) {}
};

struct Node {
    NodeType type = NodeType::Element;
    std::string tag_name;
    std::map<std::string, std::string> attributes;
    std::vector<std::unique_ptr<Node>> children;
    std::string text_content;
    size_t start_pos = 0;
    size_t end_pos = 0;
    
    Node() = default;
    Node(NodeType t) : type(t) {}
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;
    Node(Node&&) = default;
    Node& operator=(Node&&) = default;
};

class Parser {
public:
    explicit Parser(const std::string& html, bool strict_mode = false);
    std::unique_ptr<Node> parse();
    
    struct ParseOptions {
        bool strict_mode = false;
        bool preserve_whitespace = false;
        bool case_sensitive = false;
        bool validate_nesting = true;
    };
    
    void set_options(const ParseOptions& options) { options_ = options; }
    const std::vector<ParseError>& get_errors() const { return errors_; }
    
private:
    std::string html_;
    size_t pos_ = 0;
    ParseOptions options_;
    std::vector<ParseError> errors_;
    
    static const std::unordered_set<std::string> void_elements_;
    static const std::unordered_set<std::string> raw_text_elements_;
    static const std::unordered_set<std::string> escapable_raw_text_elements_;
    static const std::map<std::string, std::unordered_set<std::string>> valid_children_;
    
    std::unique_ptr<Node> parse_document();
    std::unique_ptr<Node> parse_node();
    std::unique_ptr<Node> parse_element();
    std::unique_ptr<Node> parse_text();
    std::unique_ptr<Node> parse_comment();
    std::unique_ptr<Node> parse_doctype();
    std::unique_ptr<Node> parse_cdata();
    std::unique_ptr<Node> parse_raw_text(const std::string& end_tag);
    
    void parse_attributes(std::map<std::string, std::string>& attributes);
    std::string parse_attribute_value();
    std::string consume_while(const std::function<bool(char)>& predicate);
    void consume_whitespace();
    bool consume_string(const std::string& str);
    char peek(size_t offset = 0) const;
    bool at_end() const { return pos_ >= html_.length(); }
    
    ElementCategory get_element_category(const std::string& tag_name) const;
    bool is_valid_tag_name(const std::string& name) const;
    bool is_valid_child(const std::string& parent, const std::string& child) const;
    void add_error(const std::string& message);
    std::string normalize_tag_name(const std::string& name) const;
};

class PrettyPrinter {
public:
    static std::string print(const Node& node, int indent_size = 2);
    static std::string print_json(const Node& node, int indent_size = 2);
    
private:
    static void print_node(const Node& node, std::string& result, int indent, int indent_size);
    static void print_json_node(const Node& node, std::string& result, int indent, int indent_size);
    static std::string escape_json_string(const std::string& str);
};

} // namespace HTML5Parser

#endif // HTML_PARSER_H