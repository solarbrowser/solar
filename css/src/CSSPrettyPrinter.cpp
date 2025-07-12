#include "CSSParser.h"
#include <sstream>
#include <algorithm>

namespace CSS3Parser {

std::string CSSPrettyPrinter::format(const CSSStyleSheet& stylesheet) {
    return format(stylesheet, FormatOptions{});
}

std::string CSSPrettyPrinter::format(const CSSStyleSheet& stylesheet, const FormatOptions& options) {
    std::ostringstream ss;
    
    // Add imports
    for (const auto& import_url : stylesheet.imports) {
        ss << "@import url(" << import_url << ")";
        if (!options.minify) ss << ";\n";
        else ss << ";";
    }
    
    if (!stylesheet.imports.empty() && !options.minify) {
        ss << "\n";
    }
    
    // Format rules
    for (size_t i = 0; i < stylesheet.rules.size(); ++i) {
        ss << format_rule(*stylesheet.rules[i], options);
        
        if (!options.minify && i < stylesheet.rules.size() - 1) {
            ss << "\n\n";
        }
    }
    
    return ss.str();
}

std::string CSSPrettyPrinter::format_rule(const CSSRule& rule) {
    return format_rule(rule, FormatOptions{});
}

std::string CSSPrettyPrinter::format_rule(const CSSRule& rule, const FormatOptions& options) {
    switch (rule.type) {
        case RuleType::Style:
            return format_style_rule(static_cast<const StyleRule&>(rule), options);
        case RuleType::AtRule:
            return format_at_rule(static_cast<const AtRule&>(rule), options);
        case RuleType::Comment:
            if (options.preserve_comments) {
                return static_cast<const CommentRule&>(rule).to_string();
            }
            return "";
    }
    return "";
}

std::string CSSPrettyPrinter::format_selector(const SelectorList& selectors) {
    return selectors.to_string();
}

std::string CSSPrettyPrinter::format_declaration(const CSSDeclaration& decl) {
    std::ostringstream ss;
    ss << decl.property << ": " << format_value(decl.value);
    if (decl.important) {
        ss << " !important";
    }
    return ss.str();
}

std::string CSSPrettyPrinter::format_value(const CSSValue& value) {
    return value.to_string();
}

std::string CSSPrettyPrinter::indent(int level, int size) {
    return std::string(level * size, ' ');
}

std::string CSSPrettyPrinter::format_style_rule(const StyleRule& rule, const FormatOptions& options, int indent_level) {
    std::ostringstream ss;
    std::string indent_str = indent(indent_level, options.indent_size);
    
    // Format selector
    ss << indent_str << format_selector(rule.selectors);
    
    if (options.minify) {
        ss << "{";
    } else {
        ss << " {\n";
    }
    
    // Prepare declarations for sorting if needed
    std::vector<CSSDeclaration> declarations = rule.declarations;
    if (options.sort_declarations) {
        std::sort(declarations.begin(), declarations.end(), 
                  [&options](const CSSDeclaration& a, const CSSDeclaration& b) {
                      // Vendor prefixes last if specified
                      if (options.vendor_prefix_last) {
                          bool a_vendor = a.property[0] == '-';
                          bool b_vendor = b.property[0] == '-';
                          if (a_vendor != b_vendor) {
                              return !a_vendor; // non-vendor properties first
                          }
                      }
                      return a.property < b.property;
                  });
    }
    
    // Format declarations
    for (size_t i = 0; i < declarations.size(); ++i) {
        if (!options.minify) {
            ss << indent_str << std::string(options.indent_size, ' ');
        }
        ss << format_declaration(declarations[i]);
        
        if (options.minify) {
            ss << ";";
        } else {
            ss << ";\n";
        }
    }
    
    if (options.minify) {
        ss << "}";
    } else {
        ss << indent_str << "}";
    }
    
    return ss.str();
}

std::string CSSPrettyPrinter::format_at_rule(const AtRule& rule, const FormatOptions& options, int indent_level) {
    std::ostringstream ss;
    std::string indent_str = indent(indent_level, options.indent_size);
    
    ss << indent_str << "@" << rule.name;
    
    if (!rule.prelude.empty()) {
        ss << " " << rule.prelude;
    }
    
    if (!rule.rules.empty()) {
        if (options.minify) {
            ss << "{";
        } else {
            ss << " {\n";
        }
        
        for (size_t i = 0; i < rule.rules.size(); ++i) {
            if (rule.rules[i]->type == RuleType::Style) {
                ss << format_style_rule(static_cast<const StyleRule&>(*rule.rules[i]), 
                                       options, indent_level + 1);
            } else if (rule.rules[i]->type == RuleType::AtRule) {
                ss << format_at_rule(static_cast<const AtRule&>(*rule.rules[i]), 
                                    options, indent_level + 1);
            }
            
            if (!options.minify && i < rule.rules.size() - 1) {
                ss << "\n";
            }
        }
        
        if (options.minify) {
            ss << "}";
        } else {
            ss << "\n" << indent_str << "}";
        }
    } else if (!rule.declarations.empty()) {
        if (options.minify) {
            ss << "{";
        } else {
            ss << " {\n";
        }
        
        for (size_t i = 0; i < rule.declarations.size(); ++i) {
            if (!options.minify) {
                ss << indent_str << std::string(options.indent_size, ' ');
            }
            ss << format_declaration(rule.declarations[i]);
            
            if (options.minify) {
                ss << ";";
            } else {
                ss << ";\n";
            }
        }
        
        if (options.minify) {
            ss << "}";
        } else {
            ss << indent_str << "}";
        }
    } else {
        ss << ";";
    }
    
    return ss.str();
}

} // namespace CSS3Parser