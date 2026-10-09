#include <algorithm>
#include <cctype>

#include "solar/css/Selectors.h"

namespace solar::css {

namespace {

using dom::Element;
using dom::Node;

std::string LowerAscii(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 0x20);
  }
  return out;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return (x >= 'A' && x <= 'Z' ? x + 0x20 : x) == (y >= 'A' && y <= 'Z' ? y + 0x20 : y);
         });
}

// An HTML element of an HTML document: where names compare without regard to case.
bool InHtmlDocument(const Element* element) { return element->IsHtml() && element->nodeDocument && element->nodeDocument->isHtml; }

const Element* ParentElement(const Element* element) { return dom::AsElement(element->parentNode); }

// The parent, and for the child of a shadow root whose host the rules are of, that host.
const Element* ParentOrHost(const Element* element, const MatchContext& context) {
  if (const Element* parent = ParentElement(element)) return parent;
  const dom::Node* node = element->parentNode;
  if (context.host && node && node->IsFragment() && static_cast<const dom::DocumentFragment*>(node)->isShadowRoot && static_cast<const dom::DocumentFragment*>(node)->host == context.host) return context.host;
  return nullptr;
}

const Element* PreviousElement(const Element* element) {
  for (const Node* node = element->previousSibling; node; node = node->previousSibling) {
    if (node->IsElement()) return static_cast<const Element*>(node);
  }
  return nullptr;
}

const Element* NextElement(const Element* element) {
  for (const Node* node = element->nextSibling; node; node = node->nextSibling) {
    if (node->IsElement()) return static_cast<const Element*>(node);
  }
  return nullptr;
}

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }

bool HasClass(const Element* element, const std::string& name, bool quirks) {
  const dom::Attr* attribute = element->FindAttribute("", "class");
  if (!attribute) return false;
  const std::string& value = attribute->value;
  for (size_t i = 0; i < value.size();) {
    while (i < value.size() && IsSpace(value[i])) ++i;
    const size_t start = i;
    while (i < value.size() && !IsSpace(value[i])) ++i;
    if (i > start) {
      const std::string_view token(value.data() + start, i - start);
      if (quirks ? EqualsIgnoreCase(token, name) : token == name) return true;
    }
  }
  return false;
}

// The attributes whose values an HTML element of an HTML document matches without regard to case.
bool IsCaseInsensitiveAttribute(const std::string& name) {
  static const char* const kNames[] = {"accept", "accept-charset", "align", "alink", "axis", "bgcolor", "charset", "checked", "clear", "codetype", "color", "compact", "declare",
                                       "defer", "dir", "direction", "disabled", "enctype", "face", "frame", "hreflang", "http-equiv", "lang", "language", "link", "media",
                                       "method", "multiple", "nohref", "noresize", "noshade", "nowrap", "readonly", "rel", "rev", "rules", "scope", "scrolling", "selected",
                                       "shape", "target", "text", "type", "valign", "valuetype", "vlink"};
  return std::find(std::begin(kNames), std::end(kNames), name) != std::end(kNames);
}

bool MatchesAttribute(const SimpleSelector& selector, const Element* element) {
  const bool html = InHtmlDocument(element);
  const std::string name = html ? LowerAscii(selector.name) : selector.name;
  bool matched = false;
  for (const dom::Attr* attribute : element->attributes) {
    if (attribute->localName != name) continue;
    if (selector.namespaceUri) {
      if (attribute->namespaceUri != *selector.namespaceUri) continue;
    } else if (!selector.namespaceName) {
      if (!attribute->namespaceUri.empty()) continue;
    } else if (*selector.namespaceName == "") {
      if (!attribute->namespaceUri.empty()) continue;
    }  // "*": any namespace
    if (selector.op == AttributeOperator::Exists) return true;

    bool insensitive = selector.caseMode == CaseMode::Insensitive || (selector.caseMode == CaseMode::Default && html && attribute->namespaceUri.empty() && IsCaseInsensitiveAttribute(name));
    const std::string& have = attribute->value;
    const std::string& want = selector.value;
    const auto equal = [&](std::string_view a, std::string_view b) { return insensitive ? EqualsIgnoreCase(a, b) : a == b; };
    switch (selector.op) {
      case AttributeOperator::Equals:
        matched = equal(have, want);
        break;
      case AttributeOperator::Includes: {
        if (want.empty() || std::any_of(want.begin(), want.end(), IsSpace)) break;
        for (size_t i = 0; i < have.size() && !matched;) {
          while (i < have.size() && IsSpace(have[i])) ++i;
          const size_t start = i;
          while (i < have.size() && !IsSpace(have[i])) ++i;
          if (i > start && equal(std::string_view(have).substr(start, i - start), want)) matched = true;
        }
        break;
      }
      case AttributeOperator::DashMatch:
        matched = equal(have, want) || (have.size() > want.size() && have[want.size()] == '-' && equal(std::string_view(have).substr(0, want.size()), want));
        break;
      case AttributeOperator::Prefix:
        matched = !want.empty() && have.size() >= want.size() && equal(std::string_view(have).substr(0, want.size()), want);
        break;
      case AttributeOperator::Suffix:
        matched = !want.empty() && have.size() >= want.size() && equal(std::string_view(have).substr(have.size() - want.size()), want);
        break;
      case AttributeOperator::Substring: {
        if (want.empty()) break;
        if (insensitive) {
          const std::string a = LowerAscii(have), b = LowerAscii(want);
          matched = a.find(b) != std::string::npos;
        } else {
          matched = have.find(want) != std::string::npos;
        }
        break;
      }
      case AttributeOperator::Exists:
        break;
    }
    if (matched) return true;
  }
  return false;
}

bool MatchesCompound(const CompoundSelector& compound, const Element* element, const MatchContext& context);
bool MatchesSelectorListAt(const SelectorList& list, const Element* element, const MatchContext& context);

// ---- Pseudo-classes ----

// The element the address of the document points at: the one whose id is its fragment, which is percent-decoded.
bool IsTarget(const Element* element) {
  const dom::Document* document = element->nodeDocument;
  if (!document || const_cast<dom::Element*>(element)->Root() != document) return false;
  const size_t hash = document->url.find('#');
  if (hash == std::string::npos) return false;
  const std::string fragment = document->url.substr(hash + 1);
  std::string decoded;
  for (size_t i = 0; i < fragment.size(); ++i) {
    if (fragment[i] == '%' && i + 2 < fragment.size() + 0 && std::isxdigit(static_cast<unsigned char>(fragment[i + 1])) && std::isxdigit(static_cast<unsigned char>(fragment[i + 2]))) {
      decoded += static_cast<char>(std::stoi(fragment.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else {
      decoded += fragment[i];
    }
  }
  if (decoded.empty()) return false;
  const dom::Attr* id = element->FindAttribute("", "id");
  return id && id->value == decoded;
}

// :focus is the focused element and the hosts of the shadow trees it is in; :focus-within is also all that contain them.
bool FocusPath(const Element* element, bool within) {
  const dom::Document* document = element->nodeDocument;
  if (!document || !document->focusedElement) return false;
  const dom::Node* node = document->focusedElement;
  if (node == element) return true;
  for (;;) {
    const dom::Node* root = node;
    while (root->parentNode) {
      root = root->parentNode;
      if (within && root == element) return true;
    }
    if (!root->IsFragment() || !static_cast<const dom::DocumentFragment*>(root)->isShadowRoot) return false;
    node = static_cast<const dom::ShadowRoot*>(root)->host;
    if (node == element) return true;
  }
}

bool IsRoot(const Element* element) { return element->parentNode && element->parentNode->IsDocument(); }

bool IsEmpty(const Element* element) {
  for (const Node* child = element->firstChild; child; child = child->nextSibling) {
    if (child->IsElement()) return false;
    if (child->IsText() && !static_cast<const dom::CharacterData*>(child)->data.empty()) return false;
  }
  return true;
}

bool IsHtmlNamed(const Element* element, std::initializer_list<std::string_view> names) {
  if (!element->IsHtml()) return false;
  return std::find(names.begin(), names.end(), element->localName) != names.end();
}

bool IsFormControl(const Element* element) { return IsHtmlNamed(element, {"button", "input", "select", "textarea", "optgroup", "option", "fieldset"}); }

bool IsDisabled(const Element* element) {
  if (!IsFormControl(element)) return false;
  if (element->FindAttribute("", "disabled")) return true;
  if (element->localName == "option") {
    const Element* parent = ParentElement(element);
    return parent && parent->IsHtml() && parent->localName == "optgroup" && parent->FindAttribute("", "disabled");
  }
  return false;
}

bool SiblingIndex(const Element* element, const SimpleSelector& nth, const MatchContext& context, int& index) {
  const auto counts = [&](const Element* other) {
    if (nth.ofType) return other->localName == element->localName && other->namespaceUri == element->namespaceUri;
    if (nth.list) return MatchesSelectorListAt(*nth.list, other, context);
    return true;
  };
  if (!counts(element)) return false;
  index = 1;
  for (const Element* other = nth.fromEnd ? NextElement(element) : PreviousElement(element); other; other = nth.fromEnd ? NextElement(other) : PreviousElement(other)) {
    if (counts(other)) ++index;
  }
  return true;
}

bool MatchesNth(const SimpleSelector& selector, const Element* element, const MatchContext& context) {
  int index = 0;
  if (!SiblingIndex(element, selector, context, index)) return false;
  const long long a = selector.a, b = selector.b;
  if (a == 0) return index == b;
  const long long difference = index - b;
  return difference % a == 0 && difference / a >= 0;
}

bool OnlyOfKind(const Element* element, bool ofType) {
  for (const Element* other = PreviousElement(element); other; other = PreviousElement(other)) {
    if (!ofType || (other->localName == element->localName && other->namespaceUri == element->namespaceUri)) return false;
  }
  for (const Element* other = NextElement(element); other; other = NextElement(other)) {
    if (!ofType || (other->localName == element->localName && other->namespaceUri == element->namespaceUri)) return false;
  }
  return true;
}

bool FirstOfKind(const Element* element, bool ofType, bool last) {
  for (const Element* other = last ? NextElement(element) : PreviousElement(element); other; other = last ? NextElement(other) : PreviousElement(other)) {
    if (!ofType || (other->localName == element->localName && other->namespaceUri == element->namespaceUri)) return false;
  }
  return true;
}

// The language of an element: the lang of it or its nearest ancestor that has one.
std::optional<std::string> LanguageOf(const Element* element) {
  for (const Element* node = element; node; node = ParentElement(node)) {
    if (const dom::Attr* lang = node->FindAttribute("", "lang")) return lang->value;
    if (const dom::Attr* lang = node->FindAttribute(std::string(dom::kXmlNamespace), "lang")) return lang->value;
  }
  return std::nullopt;
}

bool MatchesLanguage(const std::string& range, const std::string& language) {
  if (range == "*") return !language.empty();
  const std::string r = LowerAscii(range), l = LowerAscii(language);
  if (r.empty()) return l.empty();
  if (l == r) return true;
  return l.size() > r.size() && l.starts_with(r) && l[r.size()] == '-';
}

std::string DirectionOf(const Element* element) {
  for (const Element* node = element; node; node = ParentElement(node)) {
    if (const dom::Attr* dir = node->FindAttribute("", "dir")) {
      const std::string value = LowerAscii(dir->value);
      if (value == "ltr" || value == "rtl") return value;
    }
  }
  return "ltr";
}

bool MatchesPseudo(PseudoClass pseudo, const Element* element, const MatchContext& context) {
  switch (pseudo) {
    case PseudoClass::Root: return IsRoot(element);
    case PseudoClass::Empty: return IsEmpty(element);
    case PseudoClass::Target: return IsTarget(element);
    case PseudoClass::Focus:
    case PseudoClass::FocusVisible: return FocusPath(element, false);
    case PseudoClass::FocusWithin: return FocusPath(element, true);
    case PseudoClass::FirstChild: return FirstOfKind(element, false, false);
    case PseudoClass::LastChild: return FirstOfKind(element, false, true);
    case PseudoClass::OnlyChild: return OnlyOfKind(element, false);
    case PseudoClass::FirstOfType: return FirstOfKind(element, true, false);
    case PseudoClass::LastOfType: return FirstOfKind(element, true, true);
    case PseudoClass::OnlyOfType: return OnlyOfKind(element, true);
    case PseudoClass::Scope: return context.scope ? element == context.scope : IsRoot(element);
    case PseudoClass::Link:
    case PseudoClass::AnyLink: return IsHtmlNamed(element, {"a", "area"}) && element->FindAttribute("", "href");
    case PseudoClass::Checked:
      if (IsHtmlNamed(element, {"input"})) {
        const dom::Attr* type = element->FindAttribute("", "type");
        const std::string value = type ? LowerAscii(type->value) : "";
        return (value == "checkbox" || value == "radio") && element->FindAttribute("", "checked");
      }
      return IsHtmlNamed(element, {"option"}) && element->FindAttribute("", "selected");
    case PseudoClass::Disabled: return IsDisabled(element);
    case PseudoClass::Enabled: return IsFormControl(element) && !IsDisabled(element);
    case PseudoClass::Required: return IsHtmlNamed(element, {"input", "select", "textarea"}) && element->FindAttribute("", "required");
    case PseudoClass::Optional: return IsHtmlNamed(element, {"input", "select", "textarea"}) && !element->FindAttribute("", "required");
    case PseudoClass::ReadWrite:
      return (IsHtmlNamed(element, {"input", "textarea"}) && !element->FindAttribute("", "readonly") && !IsDisabled(element)) ||
             (element->IsHtml() && element->FindAttribute("", "contenteditable"));
    case PseudoClass::ReadOnly:
      return element->IsHtml() && !MatchesPseudo(PseudoClass::ReadWrite, element, context);
    case PseudoClass::Defined: return element->customState == dom::CustomState::Uncustomized || element->customState == dom::CustomState::Custom;
    case PseudoClass::Open: return IsHtmlNamed(element, {"details", "dialog"}) && element->FindAttribute("", "open");
    case PseudoClass::Closed: return IsHtmlNamed(element, {"details", "dialog"}) && !element->FindAttribute("", "open");
    default:
      // The ones that depend on what the user is doing or on what is rendered: no element is one yet.
      return false;
  }
}

bool MatchesSimple(const SimpleSelector& selector, const Element* element, const MatchContext& context) {
  switch (selector.kind) {
    case SimpleSelector::Kind::Universal:
      if (selector.namespaceUri && element->namespaceUri != *selector.namespaceUri) return false;
      return !selector.namespaceName || *selector.namespaceName != "" || element->namespaceUri.empty();
    case SimpleSelector::Kind::Type: {
      if (selector.namespaceUri && element->namespaceUri != *selector.namespaceUri) return false;
      if (selector.namespaceName && *selector.namespaceName == "" && !element->namespaceUri.empty()) return false;
      if (InHtmlDocument(element)) return LowerAscii(selector.name) == element->localName;
      return selector.name == element->localName;
    }
    case SimpleSelector::Kind::Class: return HasClass(element, selector.name, context.quirks);
    case SimpleSelector::Kind::Id: {
      const dom::Attr* id = element->FindAttribute("", "id");
      return id && (context.quirks ? EqualsIgnoreCase(id->value, selector.name) : id->value == selector.name);
    }
    case SimpleSelector::Kind::Attribute: return MatchesAttribute(selector, element);
    case SimpleSelector::Kind::Pseudo: return MatchesPseudo(selector.pseudo, element, context);
    case SimpleSelector::Kind::Not: return !MatchesSelectorListAt(*selector.list, element, context);
    case SimpleSelector::Kind::Is:
    case SimpleSelector::Kind::Where: return MatchesSelectorListAt(*selector.list, element, context);
    case SimpleSelector::Kind::Has: {
      // The elements the relative selector can be about, which depend on how it starts.
      const auto matchesRelative = [&](const ComplexSelector& relative, const Element* anchor, auto&& self, size_t index, const Element* candidate) -> bool {
        if (!MatchesCompound(relative.compounds[index], candidate, context)) return false;
        const auto leading = [&](const Element* leftmost) {
          switch (relative.leading) {
            case Combinator::Descendant:
              for (const Element* up = ParentElement(leftmost); up; up = ParentElement(up)) {
                if (up == anchor) return true;
              }
              return false;
            case Combinator::Child: return ParentElement(leftmost) == anchor;
            case Combinator::NextSibling: return PreviousElement(leftmost) == anchor;
            case Combinator::SubsequentSibling:
              for (const Element* before = PreviousElement(leftmost); before; before = PreviousElement(before)) {
                if (before == anchor) return true;
              }
              return false;
          }
          return false;
        };
        if (index == 0) return leading(candidate);
        switch (relative.combinators[index - 1]) {
          case Combinator::Descendant:
            for (const Element* up = ParentElement(candidate); up; up = ParentElement(up)) {
              if (self(relative, anchor, self, index - 1, up)) return true;
            }
            return false;
          case Combinator::Child: {
            const Element* parent = ParentElement(candidate);
            return parent && self(relative, anchor, self, index - 1, parent);
          }
          case Combinator::NextSibling: {
            const Element* before = PreviousElement(candidate);
            return before && self(relative, anchor, self, index - 1, before);
          }
          case Combinator::SubsequentSibling:
            for (const Element* before = PreviousElement(candidate); before; before = PreviousElement(before)) {
              if (self(relative, anchor, self, index - 1, before)) return true;
            }
            return false;
        }
        return false;
      };
      for (const ComplexSelector& relative : *selector.list) {
        const auto tryTree = [&](const Node* root, bool includeRoot) {
          for (const Node* node = includeRoot ? root : const_cast<Node*>(root)->NextInTree(root); node; node = const_cast<Node*>(node)->NextInTree(root)) {
            if (node->IsElement() && matchesRelative(relative, element, matchesRelative, relative.compounds.size() - 1, static_cast<const Element*>(node))) return true;
          }
          return false;
        };
        if (relative.leading == Combinator::Descendant || relative.leading == Combinator::Child) {
          if (tryTree(element, false)) return true;
        } else {
          for (const Element* sibling = NextElement(element); sibling; sibling = NextElement(sibling)) {
            if (tryTree(sibling, true)) return true;
          }
        }
      }
      return false;
    }
    case SimpleSelector::Kind::Nth: return MatchesNth(selector, element, context);
    case SimpleSelector::Kind::Lang: {
      const std::optional<std::string> language = LanguageOf(element);
      if (!language) return false;
      return std::any_of(selector.languages.begin(), selector.languages.end(), [&](const std::string& range) { return MatchesLanguage(range, *language); });
    }
    case SimpleSelector::Kind::Dir: return DirectionOf(element) == selector.value;
    case SimpleSelector::Kind::Nesting: {
      if (selector.list) return MatchesSelectorListAt(*selector.list, element, context);
      // At the top level & is :scope.
      return context.scope ? element == context.scope : IsRoot(element);
    }
    case SimpleSelector::Kind::Host: {
      if (!context.host || element != context.host) return false;
      if (!selector.list) return true;
      // The argument is about the host as the tree outside sees it.
      MatchContext outer = context;
      outer.host = nullptr;
      if (selector.name == "host") return MatchesSelectorListAt(*selector.list, element, outer);
      // :host-context(): the host or one of the elements around it.
      for (const Element* up = element; up;) {
        if (MatchesSelectorListAt(*selector.list, up, outer)) return true;
        const dom::Node* parent = up->parentNode;
        if (!parent) break;
        if (const Element* e = dom::AsElement(parent)) up = e;
        else if (parent->IsFragment() && static_cast<const dom::DocumentFragment*>(parent)->isShadowRoot) up = static_cast<const dom::DocumentFragment*>(parent)->host;
        else break;
      }
      return false;
    }
    case SimpleSelector::Kind::Heading: {
      if (!element->IsHtml() || element->localName.size() != 2 || element->localName[0] != 'h' || element->localName[1] < '1' || element->localName[1] > '6') return false;
      if (selector.languages.empty()) return true;
      const int level = element->localName[1] - '0';
      for (const std::string& wanted : selector.languages) {
        if (std::atoi(wanted.c_str()) == level) return true;
      }
      return false;
    }
    case SimpleSelector::Kind::PseudoElement: return context.pseudoElement && selector.name == *context.pseudoElement;
    case SimpleSelector::Kind::Unknown:
      return false;
  }
  return false;
}

bool MatchesCompound(const CompoundSelector& compound, const Element* element, const MatchContext& context) {
  // The shadow host, seen from the rules of its shadow tree, is only :host.
  if (context.host && element == context.host) {
    bool isHost = false;
    for (const SimpleSelector& simple : compound.simples) isHost = isHost || simple.kind == SimpleSelector::Kind::Host;
    if (!isHost) return false;
  }
  for (const SimpleSelector& simple : compound.simples) {
    if (!MatchesSimple(simple, element, context)) return false;
  }
  return true;
}

// The compound at `index` matches `element`, and the ones before it match what the combinators lead to.
bool MatchesFrom(const ComplexSelector& selector, size_t index, const Element* element, const MatchContext& context) {
  if (!MatchesCompound(selector.compounds[index], element, context)) return false;
  if (index == 0) return true;
  switch (selector.combinators[index - 1]) {
    case Combinator::Descendant:
      for (const Element* up = ParentOrHost(element, context); up; up = ParentOrHost(up, context)) {
        if (MatchesFrom(selector, index - 1, up, context)) return true;
      }
      return false;
    case Combinator::Child: {
      const Element* parent = ParentOrHost(element, context);
      return parent && MatchesFrom(selector, index - 1, parent, context);
    }
    case Combinator::NextSibling: {
      const Element* before = PreviousElement(element);
      return before && MatchesFrom(selector, index - 1, before, context);
    }
    case Combinator::SubsequentSibling:
      for (const Element* before = PreviousElement(element); before; before = PreviousElement(before)) {
        if (MatchesFrom(selector, index - 1, before, context)) return true;
      }
      return false;
  }
  return false;
}

bool MatchesSelectorListAt(const SelectorList& list, const Element* element, const MatchContext& context) {
  for (const ComplexSelector& selector : list) {
    if (MatchesFrom(selector, selector.compounds.size() - 1, element, context)) return true;
  }
  return false;
}

}  // namespace

bool MatchesComplex(const ComplexSelector& selector, const Element* element, const MatchContext& context) {
  // A selector ends in a pseudo-element or it does not; it matches the pseudo-element asked for, or the element when none is.
  bool endsInPseudo = false;
  for (const SimpleSelector& simple : selector.compounds.back().simples) {
    if (simple.kind == SimpleSelector::Kind::PseudoElement) endsInPseudo = true;
  }
  if (endsInPseudo != (context.pseudoElement != nullptr)) return false;
  return MatchesFrom(selector, selector.compounds.size() - 1, element, context);
}

bool MatchesAny(const SelectorList& list, const Element* element, const MatchContext& context) { return MatchesSelectorListAt(list, element, context); }

}  // namespace solar::css
