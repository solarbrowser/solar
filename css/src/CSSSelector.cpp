#include "CSSParser.h"
#include <sstream>
#include <algorithm>

namespace CSS3Parser {

// SimpleSelector implementation
std::string SimpleSelector::to_string() const {
    std::ostringstream ss;
    
    switch (type) {
        case SelectorType::Universal:
            ss << "*";
            break;
            
        case SelectorType::Type:
            ss << name;
            break;
            
        case SelectorType::Class:
            ss << "." << name;
            break;
            
        case SelectorType::Id:
            ss << "#" << name;
            break;
            
        case SelectorType::Attribute:
            ss << "[" << attribute.name;
            switch (attribute.match_type) {
                case AttributeMatchType::Exists:
                    break;
                case AttributeMatchType::Exact:
                    ss << "=" << attribute.value;
                    break;
                case AttributeMatchType::Include:
                    ss << "~=" << attribute.value;
                    break;
                case AttributeMatchType::Dash:
                    ss << "|=" << attribute.value;
                    break;
                case AttributeMatchType::Prefix:
                    ss << "^=" << attribute.value;
                    break;
                case AttributeMatchType::Suffix:
                    ss << "$=" << attribute.value;
                    break;
                case AttributeMatchType::Substring:
                    ss << "*=" << attribute.value;
                    break;
            }
            if (attribute.case_insensitive) {
                ss << " i";
            }
            ss << "]";
            break;
            
        case SelectorType::Pseudo:
            ss << ":" << pseudo.name;
            if (pseudo.is_function) {
                ss << "(" << pseudo.argument << ")";
            }
            break;
            
        case SelectorType::PseudoElement:
            ss << "::" << pseudo.name;
            if (pseudo.is_function) {
                ss << "(" << pseudo.argument << ")";
            }
            break;
    }
    
    return ss.str();
}

int SimpleSelector::specificity() const {
    switch (type) {
        case SelectorType::Universal:
            return 0;
        case SelectorType::Type:
        case SelectorType::PseudoElement:
            return 1;
        case SelectorType::Class:
        case SelectorType::Attribute:
        case SelectorType::Pseudo:
            return 10;
        case SelectorType::Id:
            return 100;
    }
    return 0;
}

// CompoundSelector implementation
std::string CompoundSelector::to_string() const {
    std::ostringstream ss;
    for (const auto& selector : selectors) {
        ss << selector.to_string();
    }
    return ss.str();
}

int CompoundSelector::specificity() const {
    int total = 0;
    for (const auto& selector : selectors) {
        total += selector.specificity();
    }
    return total;
}

// ComplexSelector implementation
void ComplexSelector::add_component(const CompoundSelector& selector, SelectorCombinator combinator) {
    Component component;
    component.selector = selector;
    component.combinator = combinator;
    components.push_back(component);
}

std::string ComplexSelector::to_string() const {
    std::ostringstream ss;
    
    for (size_t i = 0; i < components.size(); ++i) {
        const auto& component = components[i];
        
        ss << component.selector.to_string();
        
        if (i < components.size() - 1) {
            switch (component.combinator) {
                case SelectorCombinator::None:
                case SelectorCombinator::Descendant:
                    ss << " ";
                    break;
                case SelectorCombinator::Child:
                    ss << " > ";
                    break;
                case SelectorCombinator::AdjacentSibling:
                    ss << " + ";
                    break;
                case SelectorCombinator::GeneralSibling:
                    ss << " ~ ";
                    break;
            }
        }
    }
    
    return ss.str();
}

int ComplexSelector::specificity() const {
    int total = 0;
    for (const auto& component : components) {
        total += component.selector.specificity();
    }
    return total;
}

// SelectorList implementation
std::string SelectorList::to_string() const {
    std::ostringstream ss;
    for (size_t i = 0; i < selectors.size(); ++i) {
        if (i > 0) ss << ", ";
        ss << selectors[i].to_string();
    }
    return ss.str();
}

int SelectorList::max_specificity() const {
    int max_spec = 0;
    for (const auto& selector : selectors) {
        max_spec = std::max(max_spec, selector.specificity());
    }
    return max_spec;
}

// CSSDeclaration implementation
std::string CSSDeclaration::to_string() const {
    std::ostringstream ss;
    ss << property << ": " << value.to_string();
    if (important) {
        ss << " !important";
    }
    return ss.str();
}

// StyleRule implementation
std::string StyleRule::to_string() const {
    std::ostringstream ss;
    ss << selectors.to_string() << " {\\n";
    
    for (const auto& decl : declarations) {
        ss << "  " << decl.to_string() << ";\\n";
    }
    
    ss << "}";
    return ss.str();
}

std::unique_ptr<CSSRule> StyleRule::clone() const {
    auto cloned = std::make_unique<StyleRule>();
    cloned->selectors = selectors;
    cloned->declarations = declarations;
    cloned->start_pos = start_pos;
    cloned->end_pos = end_pos;
    return std::move(cloned);
}

// AtRule implementation
bool AtRule::is_conditional() const {
    return name == "media" || name == "supports" || name == "document" || 
           name == "container" || name == "layer";
}

bool AtRule::is_descriptor() const {
    return name == "font-face" || name == "page" || name == "viewport" || 
           name == "counter-style" || name == "property";
}

bool AtRule::is_keyframes() const {
    return name == "keyframes" || name == "-webkit-keyframes" || 
           name == "-moz-keyframes" || name == "-ms-keyframes";
}

std::string AtRule::to_string() const {
    std::ostringstream ss;
    ss << "@" << name;
    
    if (!prelude.empty()) {
        ss << " " << prelude;
    }
    
    if (!rules.empty()) {
        ss << " {\\n";
        for (const auto& rule : rules) {
            ss << "  " << rule->to_string() << "\\n";
        }
        ss << "}";
    } else if (!declarations.empty()) {
        ss << " {\\n";
        for (const auto& decl : declarations) {
            ss << "  " << decl.to_string() << ";\\n";
        }
        ss << "}";
    } else {
        ss << ";";
    }
    
    return ss.str();
}

std::unique_ptr<CSSRule> AtRule::clone() const {
    auto cloned = std::make_unique<AtRule>(name);
    cloned->prelude = prelude;
    cloned->declarations = declarations;
    cloned->start_pos = start_pos;
    cloned->end_pos = end_pos;
    
    for (const auto& rule : rules) {
        cloned->rules.push_back(rule->clone());
    }
    
    return std::move(cloned);
}

// CommentRule implementation
std::unique_ptr<CSSRule> CommentRule::clone() const {
    auto cloned = std::make_unique<CommentRule>(content);
    cloned->start_pos = start_pos;
    cloned->end_pos = end_pos;
    return std::move(cloned);
}

// CSSStyleSheet implementation
std::string CSSStyleSheet::to_string() const {
    std::ostringstream ss;
    
    // Add imports first
    for (const auto& import_url : imports) {
        ss << "@import url(" << import_url << ");\\n";
    }
    
    if (!imports.empty()) {
        ss << "\\n";
    }
    
    // Add rules
    for (const auto& rule : rules) {
        ss << rule->to_string() << "\\n\\n";
    }
    
    return ss.str();
}

void CSSStyleSheet::insert_rule(const std::string& rule_text, size_t index) {
    // Parse the rule text and insert at specified index
    // This is a simplified implementation
    if (index <= rules.size()) {
        // For now, create a comment rule with the text
        auto comment_rule = std::make_unique<CommentRule>("Inserted rule: " + rule_text);
        rules.insert(rules.begin() + index, std::move(comment_rule));
    }
}

void CSSStyleSheet::delete_rule(size_t index) {
    if (index < rules.size()) {
        rules.erase(rules.begin() + index);
    }
}

std::vector<StyleRule*> CSSStyleSheet::get_style_rules() const {
    std::vector<StyleRule*> style_rules;
    for (const auto& rule : rules) {
        if (rule->type == RuleType::Style) {
            style_rules.push_back(static_cast<StyleRule*>(rule.get()));
        }
    }
    return style_rules;
}

std::vector<AtRule*> CSSStyleSheet::get_at_rules(const std::string& name) const {
    std::vector<AtRule*> at_rules;
    for (const auto& rule : rules) {
        if (rule->type == RuleType::AtRule) {
            AtRule* at_rule = static_cast<AtRule*>(rule.get());
            if (name.empty() || at_rule->name == name) {
                at_rules.push_back(at_rule);
            }
        }
    }
    return at_rules;
}

} // namespace CSS3Parser