#include "Lexer.h"
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <iostream>

namespace Quanta {

// Static keyword mapping
const std::unordered_map<std::string, TokenType> Lexer::keywords_ = {
    {"break", TokenType::BREAK},
    {"case", TokenType::CASE},
    {"catch", TokenType::CATCH},
    {"class", TokenType::CLASS},
    {"const", TokenType::CONST},
    {"continue", TokenType::CONTINUE},
    {"debugger", TokenType::DEBUGGER},
    {"default", TokenType::DEFAULT},
    {"delete", TokenType::DELETE},
    {"do", TokenType::DO},
    {"else", TokenType::ELSE},
    {"export", TokenType::EXPORT},
    {"extends", TokenType::EXTENDS},
    {"finally", TokenType::FINALLY},
    {"for", TokenType::FOR},
    {"function", TokenType::FUNCTION},
    {"if", TokenType::IF},
    {"import", TokenType::IMPORT},
    {"in", TokenType::IN},
    {"instanceof", TokenType::INSTANCEOF},
    {"let", TokenType::LET},
    {"new", TokenType::NEW},
    {"return", TokenType::RETURN},
    {"super", TokenType::SUPER},
    {"switch", TokenType::SWITCH},
    {"this", TokenType::THIS},
    {"throw", TokenType::THROW},
    {"try", TokenType::TRY},
    {"typeof", TokenType::TYPEOF},
    {"var", TokenType::VAR},
    {"void", TokenType::VOID},
    {"while", TokenType::WHILE},
    {"with", TokenType::WITH},
    {"yield", TokenType::YIELD},
    {"async", TokenType::ASYNC},
    {"await", TokenType::AWAIT},
    {"from", TokenType::FROM},
    {"of", TokenType::OF},
    {"static", TokenType::STATIC},
    {"true", TokenType::BOOLEAN},
    {"false", TokenType::BOOLEAN},
    {"null", TokenType::NULL_LITERAL},
    {"undefined", TokenType::UNDEFINED}
};

// Single character tokens
const std::unordered_map<char, TokenType> Lexer::single_char_tokens_ = {
    {'(', TokenType::LEFT_PAREN},
    {')', TokenType::RIGHT_PAREN},
    {'{', TokenType::LEFT_BRACE},
    {'}', TokenType::RIGHT_BRACE},
    {'[', TokenType::LEFT_BRACKET},
    {']', TokenType::RIGHT_BRACKET},
    {';', TokenType::SEMICOLON},
    {',', TokenType::COMMA},
    {':', TokenType::COLON},
    {'?', TokenType::QUESTION},
    {'~', TokenType::BITWISE_NOT}
};

//=============================================================================
// Lexer Implementation
//=============================================================================

Lexer::Lexer(const std::string& source)
    : source_(source), position_(0), current_position_(1, 1, 0) {
    options_.skip_whitespace = true;
    options_.skip_comments = true;
    options_.track_positions = true;
    options_.allow_reserved_words = false;
    options_.strict_mode = false;
}

Lexer::Lexer(const std::string& source, const LexerOptions& options)
    : source_(source), position_(0), current_position_(1, 1, 0), options_(options) {
}

TokenSequence Lexer::tokenize() {
    std::vector<Token> tokens;
    
    while (!at_end()) {
        Token token = next_token();
        
        // Skip whitespace and comments if requested
        if ((options_.skip_whitespace && token.get_type() == TokenType::WHITESPACE) ||
            (options_.skip_comments && token.get_type() == TokenType::COMMENT)) {
            continue;
        }
        
        tokens.push_back(token);
        
        if (token.get_type() == TokenType::EOF_TOKEN) {
            break;
        }
    }
    
    // Ensure we have an EOF token
    if (tokens.empty() || tokens.back().get_type() != TokenType::EOF_TOKEN) {
        tokens.emplace_back(TokenType::EOF_TOKEN, current_position_);
    }
    
    return TokenSequence(std::move(tokens));
}

Token Lexer::next_token() {
    if (at_end()) {
        return Token(TokenType::EOF_TOKEN, current_position_);
    }
    
    Position start = current_position_;
    char ch = current_char();
    
    // Skip whitespace if not tracking it
    if (is_whitespace(ch)) {
        if (options_.skip_whitespace) {
            skip_whitespace();
            return next_token();
        } else {
            skip_whitespace();
            return create_token(TokenType::WHITESPACE, start);
        }
    }
    
    // Line terminators
    if (is_line_terminator(ch)) {
        advance();
        return create_token(TokenType::NEWLINE, start);
    }
    
    // Comments and regex literals
    if (ch == '/') {
        char next = peek_char();
        if (next == '/') {
            return read_single_line_comment();
        } else if (next == '*') {
            return read_multi_line_comment();
        } else if (can_be_regex_literal()) {
            return read_regex();
        }
        // Fall through to operator parsing
    }
    
    // Numbers
    if (is_digit(ch) || (ch == '.' && is_digit(peek_char()))) {
        return read_number();
    }
    
    // Strings
    if (ch == '"' || ch == '\'') {
        return read_string(ch);
    }
    
    // Template literals
    if (ch == '`') {
        return read_template_literal();
    }
    
    // Identifiers and keywords
    if (is_identifier_start(ch)) {
        return read_identifier();
    }
    
    // Single character tokens
    auto single_it = single_char_tokens_.find(ch);
    if (single_it != single_char_tokens_.end()) {
        advance();
        return create_token(single_it->second, start);
    }
    
    // Operators
    return read_operator();
}

void Lexer::reset(size_t position) {
    position_ = std::min(position, source_.length());
    current_position_ = Position(1, 1, position_);
    
    // Recalculate line and column
    for (size_t i = 0; i < position_; ++i) {
        if (source_[i] == '\n') {
            current_position_.line++;
            current_position_.column = 1;
        } else {
            current_position_.column++;
        }
    }
}

char Lexer::current_char() const {
    if (at_end()) return '\0';
    return source_[position_];
}

char Lexer::peek_char(size_t offset) const {
    size_t peek_pos = position_ + offset;
    if (peek_pos >= source_.length()) return '\0';
    return source_[peek_pos];
}

char Lexer::advance() {
    if (at_end()) return '\0';
    
    char ch = source_[position_++];
    advance_position(ch);
    return ch;
}

void Lexer::skip_whitespace() {
    while (!at_end() && is_whitespace(current_char())) {
        advance();
    }
}

void Lexer::advance_position(char ch) {
    current_position_.offset = position_;
    
    if (ch == '\n') {
        current_position_.line++;
        current_position_.column = 1;
    } else {
        current_position_.column++;
    }
}

Token Lexer::create_token(TokenType type, const Position& start) const {
    return Token(type, start);
}

Token Lexer::create_token(TokenType type, const std::string& value, const Position& start) const {
    return Token(type, value, start, current_position_);
}

Token Lexer::create_token(TokenType type, double numeric_value, const Position& start) const {
    return Token(type, numeric_value, start, current_position_);
}

Token Lexer::read_identifier() {
    Position start = current_position_;
    std::string value;
    
    while (!at_end() && is_identifier_part(current_char())) {
        value += advance();
    }
    
    TokenType type = lookup_keyword(value);
    return create_token(type, value, start);
}

Token Lexer::read_number() {
    Position start = current_position_;
    double value = 0.0;
    
    // Handle different number formats
    if (current_char() == '0') {
        char next = peek_char();
        if (next == 'x' || next == 'X') {
            advance(); // '0'
            advance(); // 'x'
            value = parse_hex_literal();
        } else if (next == 'b' || next == 'B') {
            advance(); // '0'
            advance(); // 'b'
            value = parse_binary_literal();
        } else if (next == 'o' || next == 'O') {
            advance(); // '0'
            advance(); // 'o'
            value = parse_octal_literal();
        } else {
            value = parse_decimal_literal();
        }
    } else {
        value = parse_decimal_literal();
    }
    
    return create_token(TokenType::NUMBER, value, start);
}

Token Lexer::read_string(char quote) {
    Position start = current_position_;
    advance(); // Skip opening quote
    
    std::string value = parse_string_literal(quote);
    
    if (at_end() || current_char() != quote) {
        add_error("Unterminated string literal");
        return create_token(TokenType::INVALID, start);
    }
    
    advance(); // Skip closing quote
    return create_token(TokenType::STRING, value, start);
}

Token Lexer::read_template_literal() {
    Position start = current_position_;
    advance(); // Skip opening `
    
    std::string value;
    while (!at_end() && current_char() != '`') {
        if (current_char() == '\\') {
            value += parse_escape_sequence();
        } else {
            value += advance();
        }
    }
    
    if (at_end()) {
        add_error("Unterminated template literal");
        return create_token(TokenType::INVALID, start);
    }
    
    advance(); // Skip closing `
    return create_token(TokenType::TEMPLATE_LITERAL, value, start);
}

Token Lexer::read_single_line_comment() {
    Position start = current_position_;
    advance(); // '/'
    advance(); // '/'
    
    std::string value;
    while (!at_end() && !is_line_terminator(current_char())) {
        value += advance();
    }
    
    return create_token(TokenType::COMMENT, value, start);
}

Token Lexer::read_multi_line_comment() {
    Position start = current_position_;
    advance(); // '/'
    advance(); // '*'
    
    std::string value;
    while (!at_end()) {
        if (current_char() == '*' && peek_char() == '/') {
            advance(); // '*'
            advance(); // '/'
            break;
        }
        value += advance();
    }
    
    return create_token(TokenType::COMMENT, value, start);
}

Token Lexer::read_operator() {
    Position start = current_position_;
    char ch = current_char();
    
    switch (ch) {
        case '+':
            advance();
            if (current_char() == '+') {
                advance();
                return create_token(TokenType::INCREMENT, start);
            } else if (current_char() == '=') {
                advance();
                return create_token(TokenType::PLUS_ASSIGN, start);
            }
            return create_token(TokenType::PLUS, start);
            
        case '-':
            advance();
            if (current_char() == '-') {
                advance();
                return create_token(TokenType::DECREMENT, start);
            } else if (current_char() == '=') {
                advance();
                return create_token(TokenType::MINUS_ASSIGN, start);
            }
            return create_token(TokenType::MINUS, start);
            
        case '*':
            advance();
            if (current_char() == '*') {
                advance();
                if (current_char() == '=') {
                    advance();
                    return create_token(TokenType::EXPONENT_ASSIGN, start);
                }
                return create_token(TokenType::EXPONENT, start);
            } else if (current_char() == '=') {
                advance();
                return create_token(TokenType::MULTIPLY_ASSIGN, start);
            }
            return create_token(TokenType::MULTIPLY, start);
            
        case '/':
            advance();
            if (current_char() == '=') {
                advance();
                return create_token(TokenType::DIVIDE_ASSIGN, start);
            }
            return create_token(TokenType::DIVIDE, start);
            
        case '%':
            advance();
            if (current_char() == '=') {
                advance();
                return create_token(TokenType::MODULO_ASSIGN, start);
            }
            return create_token(TokenType::MODULO, start);
            
        case '=':
            advance();
            if (current_char() == '=') {
                advance();
                if (current_char() == '=') {
                    advance();
                    return create_token(TokenType::STRICT_EQUAL, start);
                }
                return create_token(TokenType::EQUAL, start);
            } else if (current_char() == '>') {
                advance();
                return create_token(TokenType::ARROW, start);
            }
            return create_token(TokenType::ASSIGN, start);
            
        case '!':
            advance();
            if (current_char() == '=') {
                advance();
                if (current_char() == '=') {
                    advance();
                    return create_token(TokenType::STRICT_NOT_EQUAL, start);
                }
                return create_token(TokenType::NOT_EQUAL, start);
            }
            return create_token(TokenType::LOGICAL_NOT, start);
            
        case '<':
            advance();
            if (current_char() == '=') {
                advance();
                return create_token(TokenType::LESS_EQUAL, start);
            } else if (current_char() == '<') {
                advance();
                if (current_char() == '=') {
                    advance();
                    return create_token(TokenType::LEFT_SHIFT_ASSIGN, start);
                }
                return create_token(TokenType::LEFT_SHIFT, start);
            }
            return create_token(TokenType::LESS_THAN, start);
            
        case '>':
            advance();
            if (current_char() == '=') {
                advance();
                return create_token(TokenType::GREATER_EQUAL, start);
            } else if (current_char() == '>') {
                advance();
                if (current_char() == '>') {
                    advance();
                    if (current_char() == '=') {
                        advance();
                        return create_token(TokenType::UNSIGNED_RIGHT_SHIFT_ASSIGN, start);
                    }
                    return create_token(TokenType::UNSIGNED_RIGHT_SHIFT, start);
                } else if (current_char() == '=') {
                    advance();
                    return create_token(TokenType::RIGHT_SHIFT_ASSIGN, start);
                }
                return create_token(TokenType::RIGHT_SHIFT, start);
            }
            return create_token(TokenType::GREATER_THAN, start);
            
        case '&':
            advance();
            if (current_char() == '&') {
                advance();
                if (current_char() == '=') {
                    advance();
                    return create_token(TokenType::LOGICAL_AND_ASSIGN, start);
                }
                return create_token(TokenType::LOGICAL_AND, start);
            } else if (current_char() == '=') {
                advance();
                return create_token(TokenType::BITWISE_AND_ASSIGN, start);
            }
            return create_token(TokenType::BITWISE_AND, start);
            
        case '|':
            advance();
            if (current_char() == '|') {
                advance();
                if (current_char() == '=') {
                    advance();
                    return create_token(TokenType::LOGICAL_OR_ASSIGN, start);
                }
                return create_token(TokenType::LOGICAL_OR, start);
            } else if (current_char() == '=') {
                advance();
                return create_token(TokenType::BITWISE_OR_ASSIGN, start);
            }
            return create_token(TokenType::BITWISE_OR, start);
            
        case '^':
            advance();
            if (current_char() == '=') {
                advance();
                return create_token(TokenType::BITWISE_XOR_ASSIGN, start);
            }
            return create_token(TokenType::BITWISE_XOR, start);
            
        case '.':
            advance();
            if (current_char() == '.' && peek_char() == '.') {
                advance();
                advance();
                return create_token(TokenType::ELLIPSIS, start);
            }
            return create_token(TokenType::DOT, start);
            
        default:
            advance();
            add_error("Unexpected character: " + std::string(1, ch));
            return create_token(TokenType::INVALID, start);
    }
}

bool Lexer::is_identifier_start(char ch) const {
    return std::isalpha(ch) || ch == '_' || ch == '$';
}

bool Lexer::is_identifier_part(char ch) const {
    return std::isalnum(ch) || ch == '_' || ch == '$';
}

bool Lexer::is_digit(char ch) const {
    return ch >= '0' && ch <= '9';
}

bool Lexer::is_hex_digit(char ch) const {
    return is_digit(ch) || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

bool Lexer::is_binary_digit(char ch) const {
    return ch == '0' || ch == '1';
}

bool Lexer::is_octal_digit(char ch) const {
    return ch >= '0' && ch <= '7';
}

bool Lexer::is_whitespace(char ch) const {
    return ch == ' ' || ch == '\t' || ch == '\v' || ch == '\f' || ch == '\r';
}

bool Lexer::is_line_terminator(char ch) const {
    return ch == '\n';
}

double Lexer::parse_decimal_literal() {
    std::string number_str;
    
    // Parse integer part
    while (!at_end() && is_digit(current_char())) {
        number_str += advance();
    }
    
    // Parse decimal part
    if (!at_end() && current_char() == '.') {
        number_str += advance();
        while (!at_end() && is_digit(current_char())) {
            number_str += advance();
        }
    }
    
    // Parse exponent
    if (!at_end() && (current_char() == 'e' || current_char() == 'E')) {
        number_str += advance();
        if (!at_end() && (current_char() == '+' || current_char() == '-')) {
            number_str += advance();
        }
        while (!at_end() && is_digit(current_char())) {
            number_str += advance();
        }
    }
    
    return std::strtod(number_str.c_str(), nullptr);
}

double Lexer::parse_hex_literal() {
    double value = 0.0;
    while (!at_end() && is_hex_digit(current_char())) {
        char ch = advance();
        int digit_value;
        if (is_digit(ch)) {
            digit_value = ch - '0';
        } else if (ch >= 'a' && ch <= 'f') {
            digit_value = ch - 'a' + 10;
        } else {
            digit_value = ch - 'A' + 10;
        }
        value = value * 16 + digit_value;
    }
    return value;
}

double Lexer::parse_binary_literal() {
    double value = 0.0;
    while (!at_end() && is_binary_digit(current_char())) {
        char ch = advance();
        value = value * 2 + (ch - '0');
    }
    return value;
}

double Lexer::parse_octal_literal() {
    double value = 0.0;
    while (!at_end() && is_octal_digit(current_char())) {
        char ch = advance();
        value = value * 8 + (ch - '0');
    }
    return value;
}

std::string Lexer::parse_string_literal(char quote) {
    std::string value;
    
    while (!at_end() && current_char() != quote) {
        if (current_char() == '\\') {
            value += parse_escape_sequence();
        } else {
            value += advance();
        }
    }
    
    return value;
}

std::string Lexer::parse_escape_sequence() {
    advance(); // Skip backslash
    
    if (at_end()) {
        add_error("Unexpected end of input in escape sequence");
        return "\\";
    }
    
    char ch = advance();
    switch (ch) {
        case 'n': return "\n";
        case 't': return "\t";
        case 'r': return "\r";
        case 'b': return "\b";
        case 'f': return "\f";
        case 'v': return "\v";
        case '0': return "\0";
        case '\\': return "\\";
        case '\'': return "'";
        case '"': return "\"";
        case 'x': return parse_hex_escape();
        case 'u': return parse_unicode_escape();
        default: return std::string(1, ch);
    }
}

std::string Lexer::parse_hex_escape() {
    // \xHH format
    if (remaining() < 2) {
        add_error("Invalid hex escape sequence");
        return "";
    }
    
    char high = advance();
    char low = advance();
    
    if (!is_hex_digit(high) || !is_hex_digit(low)) {
        add_error("Invalid hex escape sequence");
        return "";
    }
    
    int value = 0;
    if (is_digit(high)) value += (high - '0') * 16;
    else if (high >= 'a' && high <= 'f') value += (high - 'a' + 10) * 16;
    else value += (high - 'A' + 10) * 16;
    
    if (is_digit(low)) value += low - '0';
    else if (low >= 'a' && low <= 'f') value += low - 'a' + 10;
    else value += low - 'A' + 10;
    
    return std::string(1, static_cast<char>(value));
}

std::string Lexer::parse_unicode_escape() {
    // \uHHHH format (simplified)
    if (remaining() < 4) {
        add_error("Invalid unicode escape sequence");
        return "";
    }
    
    // For now, just skip the unicode characters
    advance(); advance(); advance(); advance();
    return "?"; // Placeholder
}

void Lexer::add_error(const std::string& message) {
    std::string error = "Lexer error at " + current_position_.to_string() + ": " + message;
    errors_.push_back(error);
}

TokenType Lexer::lookup_keyword(const std::string& identifier) const {
    auto it = keywords_.find(identifier);
    return (it != keywords_.end()) ? it->second : TokenType::IDENTIFIER;
}

bool Lexer::is_reserved_word(const std::string& word) const {
    return keywords_.find(word) != keywords_.end();
}

bool Lexer::can_be_regex_literal() const {
    // Simple heuristic: regex literals can appear after:
    // - assignment operators (=, +=, -=, etc.)
    // - comparison operators (==, !=, <, >, etc.)
    // - logical operators (&&, ||, !)
    // - control flow keywords (if, for, while, return, etc.)
    // - opening parentheses, brackets, braces
    // - commas, semicolons
    // - beginning of input
    
    if (position_ == 0) return true;
    
    // Look backwards to find the last non-whitespace character
    size_t pos = position_ - 1;
    while (pos > 0 && is_whitespace(source_[pos])) {
        pos--;
    }
    
    if (pos == 0) return true;
    
    char prev_char = source_[pos];
    
    // Check for characters that typically precede regex literals
    return prev_char == '=' || prev_char == '(' || prev_char == '[' || 
           prev_char == '{' || prev_char == ',' || prev_char == ';' || 
           prev_char == ':' || prev_char == '!' || prev_char == '&' || 
           prev_char == '|' || prev_char == '?' || prev_char == '+' || 
           prev_char == '-' || prev_char == '*' || prev_char == '%' || 
           prev_char == '<' || prev_char == '>' || prev_char == '^' || 
           prev_char == '~';
}

Token Lexer::read_regex() {
    Position start = current_position_;
    advance(); // consume initial '/'
    
    std::string pattern;
    
    // Read the pattern until we find the closing '/'
    while (!at_end() && current_char() != '/') {
        char ch = current_char();
        
        if (ch == '\\') {
            // Handle escape sequences
            pattern += ch;
            advance();
            if (!at_end()) {
                pattern += current_char();
                advance();
            }
        } else if (ch == '\n' || ch == '\r') {
            // Regex literals cannot contain unescaped newlines
            add_error("Unterminated regex literal");
            return create_token(TokenType::INVALID, start);
        } else {
            pattern += ch;
            advance();
        }
    }
    
    if (at_end()) {
        add_error("Unterminated regex literal");
        return create_token(TokenType::INVALID, start);
    }
    
    advance(); // consume closing '/'
    
    // Read flags
    std::string flags;
    while (!at_end() && is_identifier_part(current_char())) {
        char flag = current_char();
        // Valid regex flags: g, i, m, s, u, y
        if (flag == 'g' || flag == 'i' || flag == 'm' || 
            flag == 's' || flag == 'u' || flag == 'y') {
            flags += flag;
            advance();
        } else {
            break;
        }
    }
    
    // Create the regex token with the full regex string
    std::string regex_value = "/" + pattern + "/" + flags;
    return create_token(TokenType::REGEX, regex_value, start);
}

} // namespace Quanta