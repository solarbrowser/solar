#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "solar/dom/Node.h"

// Selectors (https://drafts.csswg.org/selectors-4/): parsing a selector, matching an element against it, and
// its specificity. What querySelector, matches and closest are made of, and what a style rule is selected by.
namespace solar::css {

struct ComplexSelector;

// "a b", "a > b", "a + b", "a ~ b": how a compound selector is related to the one before it.
enum class Combinator { Descendant, Child, NextSibling, SubsequentSibling };

enum class AttributeOperator { Exists, Equals, Includes, DashMatch, Prefix, Suffix, Substring };
enum class CaseMode { Default, Insensitive, Sensitive };

// The pseudo-classes that need no arguments.
enum class PseudoClass {
  Root, Empty, FirstChild, LastChild, OnlyChild, FirstOfType, LastOfType, OnlyOfType,
  Scope, Link, AnyLink, Visited, Hover, Active, Focus, FocusVisible, FocusWithin, Target, TargetWithin,
  Checked, Disabled, Enabled, Required, Optional, ReadOnly, ReadWrite, PlaceholderShown, Default,
  Indeterminate, Valid, Invalid, InRange, OutOfRange, Defined, Fullscreen, Modal, PopoverOpen, UserInvalid, UserValid,
  Playing, Paused, Muted, Autofill, Past, Current, Future, LocalLink, Host, Open, Closed,
};

struct SimpleSelector {
  enum class Kind {
    Type,
    Universal,
    Class,
    Id,
    Attribute,
    Pseudo,        // a pseudo-class with no arguments
    Not,           // :not(), :is(), :where(): `list`
    Is,
    Where,
    Has,           // :has(): relative selectors in `list`
    Nth,           // :nth-child() and the like
    Lang,          // :lang(): `languages`
    Dir,           // :dir(): `name` is ltr or rtl
    PseudoElement, // never matches an element
    Nesting,       // &: `list` is the selector list of the rule it is nested in (none: the scoping root)
    Host,          // :host, :host(), :host-context(): `name` says which, `list` is the argument
    Heading,       // :heading and :heading(): `languages` holds the levels asked for
    Unknown,       // a pseudo-class this engine does not know: it matches nothing
  };
  Kind kind = Kind::Type;
  // The name of a type, class, id or attribute (case as written), or of a pseudo-element.
  std::string name;
  // A type's or an attribute's namespace: "*" any, "" none, or absent for "any, as no default is declared".
  std::optional<std::string> namespaceName;
  // The namespace the prefix (or the default namespace) stands for, once a style sheet has said which: what is matched against.
  std::optional<std::string> namespaceUri;
  // An attribute selector.
  AttributeOperator op = AttributeOperator::Exists;
  std::string value;
  CaseMode caseMode = CaseMode::Default;
  PseudoClass pseudo = PseudoClass::Root;
  // :nth-*: matches positions a*n + b (n >= 0).
  int a = 0;
  int b = 0;
  bool fromEnd = false;
  bool ofType = false;
  std::vector<std::string> languages;
  std::shared_ptr<std::vector<ComplexSelector>> list;  // the arguments of :not(), :is(), :where(), :has(), :nth-child(... of S)
};

struct CompoundSelector {
  std::vector<SimpleSelector> simples;
};

struct ComplexSelector {
  // Left to right; `combinators[i]` relates compounds[i] and compounds[i + 1]. For a relative selector (inside
  // :has()) `leading` is the combinator before the first compound.
  std::vector<CompoundSelector> compounds;
  std::vector<Combinator> combinators;
  Combinator leading = Combinator::Descendant;
  bool relative = false;
  // The first compound is the & that a nested selector gets without writing it: it is not written back.
  bool implicitNesting = false;
};

using SelectorList = std::vector<ComplexSelector>;

// Parses a selector list as querySelector does: nullopt if it is not valid (a SyntaxError). `forgiving`
// is for the lists of :is() and :where(), where what is invalid is dropped.
std::optional<SelectorList> ParseSelectorList(std::string_view text);
// The same for the selector of a style rule, which may use the prefixes of @namespace rules (checked by the sheet).
std::optional<SelectorList> ParseSelectorListForRule(std::string_view text);
// The selector of a style rule nested in another: relative selectors are allowed, and a selector without & is taken to have
// one in front of it. `parent` is what & stands for (the parent rule's list).
std::optional<SelectorList> ParseNestedSelectorList(std::string_view text, const std::shared_ptr<SelectorList>& parent);

// "serialize a group of selectors" (CSSOM): the text selectorText answers with.
std::string SerializeSelectorList(const SelectorList& list);
std::string SerializeComplexSelector(const ComplexSelector& selector);

// (a, b, c): the ids, the classes and attributes and pseudo-classes, and the types and pseudo-elements.
struct Specificity {
  uint32_t ids = 0;
  uint32_t classes = 0;
  uint32_t types = 0;
  bool operator<(const Specificity& o) const {
    if (ids != o.ids) return ids < o.ids;
    if (classes != o.classes) return classes < o.classes;
    return types < o.types;
  }
};
Specificity SpecificityOf(const ComplexSelector& selector);

// What a match needs to know that is not in the element.
struct MatchContext {
  const dom::Element* scope = nullptr;  // :scope
  bool quirks = false;                   // class and id names compare without regard to case
  // Matching for a pseudo-element ("before", "first-line"): only selectors that end in it match, and then its element.
  const std::string* pseudoElement = nullptr;
  // The shadow host whose tree the rule is of, for :host.
  const dom::Element* host = nullptr;
};

bool MatchesComplex(const ComplexSelector& selector, const dom::Element* element, const MatchContext& context);
bool MatchesAny(const SelectorList& list, const dom::Element* element, const MatchContext& context);

}  // namespace solar::css
