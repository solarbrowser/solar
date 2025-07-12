#include "CSSParser.h"
#include <cctype>
#include <algorithm>
#include <sstream>

namespace CSS3Parser {

CSSTokenizer::CSSTokenizer(const std::string& input) : input_(input) {}

Token CSSTokenizer::next_token() {
    if (buffer_pos_ < token_buffer_.size()) {
        return token_buffer_[buffer_pos_++];
    }
    
    skip_whitespace();
    
    if (at_end()) {
        return Token(TokenType::EOF_TOKEN);
    }
    
    size_t start_pos = pos_;
    size_t start_line = line_;
    size_t start_column = column_;
    
    char c = peek();
    Token token;
    
    switch (c) {
        case '/':
            if (peek(1) == '*') {
                skip_comment();
                return next_token(); // Skip comments by default
            }
            token = Token(TokenType::Delim, std::string(1, consume()));
            break;
            
        case '@':
            consume(); // consume '@'
            if (is_identifier_start(peek())) {
                token = Token(TokenType::AtKeyword, consume_identifier());
            } else {
                token = Token(TokenType::Delim, "@");
            }
            break;
            
        case '#':
            token = read_hash();
            break;
            
        case '"':
        case '\'':
            token = read_string(c);
            break;
            
        case '(':
            token = Token(TokenType::LeftParen, std::string(1, consume()));
            break;
            
        case ')':
            token = Token(TokenType::RightParen, std::string(1, consume()));
            break;
            
        case '[':
            token = Token(TokenType::LeftSquare, std::string(1, consume()));
            break;
            
        case ']':
            token = Token(TokenType::RightSquare, std::string(1, consume()));
            break;
            
        case '{':
            token = Token(TokenType::LeftBrace, std::string(1, consume()));
            break;
            
        case '}':
            token = Token(TokenType::RightBrace, std::string(1, consume()));
            break;
            
        case ':':
            token = Token(TokenType::Colon, std::string(1, consume()));
            break;
            
        case ';':
            token = Token(TokenType::Semicolon, std::string(1, consume()));
            break;
            
        case ',':
            token = Token(TokenType::Comma, std::string(1, consume()));
            break;
            
        case '~':
            if (peek(1) == '=') {
                consume(); consume();
                token = Token(TokenType::IncludeMatch, "~=");
            } else {
                token = Token(TokenType::Delim, std::string(1, consume()));
            }
            break;
            
        case '|':
            if (peek(1) == '=') {
                consume(); consume();
                token = Token(TokenType::DashMatch, "|=");
            } else if (peek(1) == '|') {
                consume(); consume();
                token = Token(TokenType::Column, "||");
            } else {
                token = Token(TokenType::Delim, std::string(1, consume()));
            }
            break;
            
        case '^':
            if (peek(1) == '=') {
                consume(); consume();
                token = Token(TokenType::PrefixMatch, "^=");
            } else {
                token = Token(TokenType::Delim, std::string(1, consume()));
            }
            break;
            
        case '$':
            if (peek(1) == '=') {
                consume(); consume();
                token = Token(TokenType::SuffixMatch, "$=");
            } else {
                token = Token(TokenType::Delim, std::string(1, consume()));
            }
            break;
            
        case '*':
            if (peek(1) == '=') {
                consume(); consume();
                token = Token(TokenType::SubstringMatch, "*=");
            } else {
                token = Token(TokenType::Delim, std::string(1, consume()));
            }
            break;
            
        case '<':
            if (peek(1) == '!' && peek(2) == '-' && peek(3) == '-') {
                consume(); consume(); consume(); consume();
                token = Token(TokenType::CDO, "<!--");
            } else {
                token = Token(TokenType::Delim, std::string(1, consume()));
            }
            break;
            
        case '-':
            if (peek(1) == '-' && peek(2) == '>') {
                consume(); consume(); consume();
                token = Token(TokenType::CDC, "-->");
            } else if (is_digit(peek(1)) || (peek(1) == '.' && is_digit(peek(2)))) {
                token = read_number();
            } else if (is_identifier_start(c)) {
                token = read_identifier();
            } else {
                token = Token(TokenType::Delim, std::string(1, consume()));
            }
            break;
            
        case '.':
            if (is_digit(peek(1))) {
                token = read_number();
            } else {
                token = Token(TokenType::Delim, std::string(1, consume()));
            }
            break;
            
        case '+':
            if (is_digit(peek(1)) || (peek(1) == '.' && is_digit(peek(2)))) {
                token = read_number();
            } else {
                token = Token(TokenType::Delim, std::string(1, consume()));
            }
            break;
            
        case 'u':
        case 'U':
            if (peek(1) == '+') {
                token = read_unicode_range();
            } else {
                token = read_identifier();
            }
            break;
            
        default:
            if (is_digit(c)) {
                token = read_number();
            } else if (is_identifier_start(c)) {
                token = read_identifier();
            } else {
                token = Token(TokenType::Delim, std::string(1, consume()));
            }
            break;
    }
    
    token.start_pos = start_pos;
    token.end_pos = pos_;
    token.line = start_line;
    token.column = start_column;
    
    return token;
}

Token CSSTokenizer::peek_token(size_t offset) {
    // Build token buffer if needed
    while (buffer_pos_ + offset >= token_buffer_.size()) {
        Token token = next_token();
        token_buffer_.push_back(token);
        
        // If we hit EOF, stop buffering
        if (token.type == TokenType::EOF_TOKEN) {
            break;
        }
    }
    
    if (buffer_pos_ + offset < token_buffer_.size()) {
        return token_buffer_[buffer_pos_ + offset];
    }
    
    return Token(TokenType::EOF_TOKEN);
}

void CSSTokenizer::reset(size_t position) {
    pos_ = position;
    line_ = 1;
    column_ = 1;
    token_buffer_.clear();
    buffer_pos_ = 0;
    
    // Recalculate line and column
    for (size_t i = 0; i < position && i < input_.length(); ++i) {
        if (input_[i] == '\n') {
            line_++;
            column_ = 1;
        } else {
            column_++;
        }
    }
}

char CSSTokenizer::peek(size_t offset) const {
    size_t index = pos_ + offset;
    return (index < input_.length()) ? input_[index] : '\0';
}

char CSSTokenizer::consume() {
    if (pos_ >= input_.length()) return '\0';
    
    char c = input_[pos_++];
    if (c == '\n') {
        line_++;
        column_ = 1;
    } else {
        column_++;
    }
    return c;
}

void CSSTokenizer::skip_whitespace() {
    while (!at_end() && is_whitespace(peek())) {
        consume();
    }
}

void CSSTokenizer::skip_comment() {
    if (peek() == '/' && peek(1) == '*') {
        consume(); // '/'
        consume(); // '*'
        
        while (!at_end()) {
            if (peek() == '*' && peek(1) == '/') {
                consume(); // '*'
                consume(); // '/'
                break;
            }
            consume();
        }
    }
}

Token CSSTokenizer::read_identifier() {
    std::string value = consume_identifier();
    
    // Check if it's a function
    if (peek() == '(') {
        return Token(TokenType::Function, value);
    }
    
    return Token(TokenType::Ident, value);
}

Token CSSTokenizer::read_string(char quote) {
    consume(); // consume opening quote
    std::string value = consume_string(quote);
    
    if (at_end() || peek() != quote) {
        add_error("Unterminated string literal");
        return Token(TokenType::BadString, value);
    }
    
    consume(); // consume closing quote
    return Token(TokenType::String, value);
}

Token CSSTokenizer::read_number() {
    double number = consume_number();
    std::string repr = std::to_string(number);
    
    // Check for percentage
    if (peek() == '%') {
        consume();
        return Token(TokenType::Percentage, repr + "%", number, "%");
    }
    
    // Check for dimension (unit)
    if (is_identifier_start(peek())) {
        std::string unit = consume_identifier();
        return Token(TokenType::Dimension, repr + unit, number, unit);
    }
    
    return Token(TokenType::Number, repr, number);
}

Token CSSTokenizer::read_hash() {
    consume(); // consume '#'
    
    if (is_identifier_char(peek()) || is_hex_digit(peek())) {
        std::string value = consume_identifier();
        return Token(TokenType::Hash, "#" + value);
    }
    
    return Token(TokenType::Delim, "#");
}

Token CSSTokenizer::read_url() {
    std::string function_name = consume_identifier(); // "url"
    consume(); // consume '('
    
    skip_whitespace();
    
    std::string url_value;
    
    // Check if quoted string
    if (peek() == '"' || peek() == '\'') {
        char quote = peek();
        consume(); // consume quote
        url_value = consume_string(quote);
        if (peek() == quote) {
            consume(); // consume closing quote
        } else {
            consume_bad_url();
            return Token(TokenType::BadUrl, url_value);
        }
    } else {
        // Unquoted URL
        while (!at_end() && peek() != ')' && !is_whitespace(peek())) {
            if (peek() == '\\') {
                consume(); // backslash
                if (!at_end()) {
                    url_value += consume();
                }
            } else if (peek() == '(' || peek() == '"' || peek() == '\'' || 
                      is_whitespace(peek()) || peek() == '\t' || peek() == '\n') {
                consume_bad_url();
                return Token(TokenType::BadUrl, url_value);
            } else {
                url_value += consume();
            }
        }
    }
    
    skip_whitespace();
    
    if (peek() == ')') {
        consume();
        return Token(TokenType::Url, url_value);
    } else {
        consume_bad_url();
        return Token(TokenType::BadUrl, url_value);
    }
}

Token CSSTokenizer::read_unicode_range() {
    consume(); // 'U' or 'u'
    consume(); // '+'
    
    std::string range;
    
    // Read hex digits or '?' wildcards
    while (!at_end() && (is_hex_digit(peek()) || peek() == '?')) {
        range += consume();
    }
    
    // Check for range separator
    if (peek() == '-') {
        range += consume(); // '-'
        while (!at_end() && is_hex_digit(peek())) {
            range += consume();
        }
    }
    
    return Token(TokenType::UnicodeRange, "U+" + range);
}

bool CSSTokenizer::is_identifier_start(char c) const {
    return std::isalpha(c) || c == '_' || c == '-' || (unsigned char)c >= 0x80;
}

bool CSSTokenizer::is_identifier_char(char c) const {
    return is_identifier_start(c) || std::isdigit(c);
}

bool CSSTokenizer::is_digit(char c) const {
    return std::isdigit(c);
}

bool CSSTokenizer::is_hex_digit(char c) const {
    return std::isxdigit(c);
}

bool CSSTokenizer::is_whitespace(char c) const {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

bool CSSTokenizer::is_newline(char c) const {
    return c == '\n' || c == '\r' || c == '\f';
}

std::string CSSTokenizer::consume_identifier() {
    std::string result;
    
    while (!at_end() && is_identifier_char(peek())) {
        if (peek() == '\\') {
            consume(); // backslash
            if (!at_end()) {
                // Handle escape sequence
                char escaped = consume();
                if (is_hex_digit(escaped)) {
                    // Unicode escape sequence
                    std::string hex_digits(1, escaped);
                    for (int i = 0; i < 5 && !at_end() && is_hex_digit(peek()); ++i) {
                        hex_digits += consume();
                    }
                    if (!at_end() && is_whitespace(peek())) {
                        consume(); // consume optional whitespace
                    }
                    // Convert hex to character
                    unsigned int code_point = std::stoul(hex_digits, nullptr, 16);
                    if (code_point <= 0x10FFFF) {
                        // Convert Unicode code point to UTF-8
                        if (code_point < 0x80) {
                            result += static_cast<char>(code_point);
                        } else {
                            // Simplified UTF-8 encoding for demonstration
                            result += '?'; // Placeholder for non-ASCII
                        }
                    }
                } else {
                    result += escaped;
                }
            }
        } else {
            result += consume();
        }
    }
    
    return result;
}

std::string CSSTokenizer::consume_string(char quote) {
    std::string result;
    
    while (!at_end() && peek() != quote && !is_newline(peek())) {
        if (peek() == '\\') {
            consume(); // backslash
            if (!at_end()) {
                if (is_newline(peek())) {
                    consume(); // consume newline (escaped)
                } else {
                    result += consume(); // escaped character
                }
            }
        } else {
            result += consume();
        }
    }
    
    return result;
}

double CSSTokenizer::consume_number() {
    std::string number_str;
    
    if (peek() == '+' || peek() == '-') {
        number_str += consume();
    }
    
    while (!at_end() && is_digit(peek())) {
        number_str += consume();
    }
    
    if (peek() == '.') {
        number_str += consume();
        while (!at_end() && is_digit(peek())) {
            number_str += consume();
        }
    }
    
    // Scientific notation
    if (peek() == 'e' || peek() == 'E') {
        number_str += consume();
        if (peek() == '+' || peek() == '-') {
            number_str += consume();
        }
        while (!at_end() && is_digit(peek())) {
            number_str += consume();
        }
    }
    
    return std::stod(number_str);
}

void CSSTokenizer::consume_bad_url() {
    while (!at_end() && peek() != ')') {
        if (peek() == '\\') {
            consume(); // backslash
            if (!at_end()) {
                consume(); // escaped character
            }
        } else {
            consume();
        }
    }
    
    if (peek() == ')') {
        consume();
    }
}

void CSSTokenizer::add_error(const std::string& message) {
    errors_.emplace_back(message, pos_, line_, column_);
}

} // namespace CSS3Parser