#include "solar/dom/NodeBindings.h"

#include <cstdio>
#include <string>

#include "solar/dom/Mutation.h"
#include "solar/dom/CustomElements.h"
#include "solar/dom/NodeBindingsInternal.h"
#include "solar/dom/Range.h"
#include "solar/dom/Traversal.h"
#include "solar/web/Scripts.h"

namespace solar::dom {

namespace {

template <typename Host>
void Install(Host& host) {
  Quanta::Context& ctx = host.GetContext();
  DefineNodeClass(ctx);
  DefineCollectionClasses(ctx);
  DefineCharacterDataClasses(ctx);
  DefineAttributeClasses(ctx);
  DefineElementClass(ctx);
  DefineDocumentClasses(ctx);
  DefineShadowClasses(ctx);
  DefineCustomElementClasses(ctx);
  DefineMutationClasses(ctx);
  DefineRangeClasses(ctx);
  DefineSelectionClass(ctx);
  DefineTraversalClasses(ctx);

  // The event interfaces written in script, and Document.createEvent, which needs the Document.
  std::string events;
  for (const char* const* piece = web::kScriptUiEvents; *piece; ++piece) events += *piece;
  const auto eventsResult = host.Evaluate(events, "events.js");
  if (!eventsResult.ok) std::fprintf(stderr, "events.js: %s\n", eventsResult.error.c_str());

  // customElements, and the functions of script that the custom element algorithms are written in terms of.
  host.Evaluate(R"JS(
    (function () {
      const init = __solarCustomElementsInit;
      const registry = __solarCustomElementsRegistry();
      delete globalThis.__solarCustomElementsInit;
      delete globalThis.__solarCustomElementsRegistry;
      delete globalThis.__solarProcessCustomElements;
      const construct = Reflect.construct;
      init({
        construct: (constructor) => construct(constructor, []),
        isConstructor(value) {
          try { new (new Proxy(value, { construct() { return {}; } }))(); return true; } catch (e) { return false; }
        },
        toStrings(value) {
          if ((typeof value !== "object" && typeof value !== "function") || value === null) throw new TypeError("The provided value is not a sequence.");
          const out = [];
          for (const item of value) out.push(`${item}`);
          return out;
        },
        report() {},
      });
      Object.defineProperty(globalThis, "customElements", { get() { return registry; }, set: undefined, enumerable: true, configurable: true });
    })();
  )JS");

  // What Web IDL has and the embedding surface cannot say: the constants, which are on the interface
  // object and its prototype, and the static side of each interface inheriting its parent's.
  host.Evaluate(R"JS(
    (function () {
      delete globalThis.__solarNotifyObservers;
      const define = (target, constants) => {
        for (const [name, value] of Object.entries(constants)) {
          Object.defineProperty(target, name, { value, writable: false, enumerable: true, configurable: false });
        }
      };
      const nodeTypes = {
        ELEMENT_NODE: 1, ATTRIBUTE_NODE: 2, TEXT_NODE: 3, CDATA_SECTION_NODE: 4, ENTITY_REFERENCE_NODE: 5, ENTITY_NODE: 6,
        PROCESSING_INSTRUCTION_NODE: 7, COMMENT_NODE: 8, DOCUMENT_NODE: 9, DOCUMENT_TYPE_NODE: 10, DOCUMENT_FRAGMENT_NODE: 11,
        NOTATION_NODE: 12,
        DOCUMENT_POSITION_DISCONNECTED: 1, DOCUMENT_POSITION_PRECEDING: 2, DOCUMENT_POSITION_FOLLOWING: 4,
        DOCUMENT_POSITION_CONTAINS: 8, DOCUMENT_POSITION_CONTAINED_BY: 16, DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC: 32,
      };
      define(Range, { START_TO_START: 0, START_TO_END: 1, END_TO_END: 2, END_TO_START: 3 });
      define(Range.prototype, { START_TO_START: 0, START_TO_END: 1, END_TO_END: 2, END_TO_START: 3 });
      Object.setPrototypeOf(Range, AbstractRange);
      Object.setPrototypeOf(StaticRange, AbstractRange);
      const filterConstants = {
        FILTER_ACCEPT: 1, FILTER_REJECT: 2, FILTER_SKIP: 3, SHOW_ALL: 0xFFFFFFFF, SHOW_ELEMENT: 0x1, SHOW_ATTRIBUTE: 0x2, SHOW_TEXT: 0x4,
        SHOW_CDATA_SECTION: 0x8, SHOW_ENTITY_REFERENCE: 0x10, SHOW_ENTITY: 0x20, SHOW_PROCESSING_INSTRUCTION: 0x40, SHOW_COMMENT: 0x80,
        SHOW_DOCUMENT: 0x100, SHOW_DOCUMENT_TYPE: 0x200, SHOW_DOCUMENT_FRAGMENT: 0x400, SHOW_NOTATION: 0x800,
      };
      define(NodeFilter, filterConstants);
      define(Node, nodeTypes);
      define(Node.prototype, nodeTypes);
      for (const [child, parent] of [[CharacterData, Node], [Text, CharacterData], [CDATASection, Text], [ProcessingInstruction, CharacterData],
                                     [Comment, CharacterData], [DocumentType, Node], [DocumentFragment, Node], [Attr, Node], [Element, Node],
                                     [HTMLElement, Element], [Document, Node], [ShadowRoot, DocumentFragment]]) {
        Object.setPrototypeOf(child, parent);
      }
      // What Web IDL gives an interface with an indexed getter that is iterable: the iteration methods
      // of arrays, which work on anything that has a length and indexed elements.
      const iterate = (target, names) => {
        for (const name of names) {
          Object.defineProperty(target, name, { value: Array.prototype[name], writable: true, enumerable: true, configurable: true });
        }
        Object.defineProperty(target, Symbol.iterator, { value: Array.prototype.values, writable: true, enumerable: false, configurable: true });
      };
      iterate(NodeList.prototype, ["entries", "keys", "values", "forEach"]);
      const listValue = Object.getOwnPropertyDescriptor(DOMTokenList.prototype, "value").get;
      Object.defineProperty(DOMTokenList.prototype, "toString", { value: function toString() { return listValue.call(this); }, writable: true, enumerable: true, configurable: true });
      iterate(DOMTokenList.prototype, ["entries", "keys", "values", "forEach"]);
      Object.defineProperty(NamedNodeMap.prototype, Symbol.iterator, { value: Array.prototype.values, writable: true, enumerable: false, configurable: true });
      Object.defineProperty(HTMLCollection.prototype, Symbol.iterator, { value: Array.prototype.values, writable: true, enumerable: false, configurable: true });
    })();
  )JS");
}

}  // namespace

void InstallNodeApis(Quanta::Embed::Realm& realm) { Install(realm); }
void InstallNodeApis(Quanta::Embed::Runtime& runtime) { Install(runtime); }

}  // namespace solar::dom
