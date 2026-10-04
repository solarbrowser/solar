#include "solar/dom/NodeBindings.h"

#include "solar/dom/NodeBindingsInternal.h"

namespace solar::dom {

namespace {

template <typename Host>
void Install(Host& host) {
  Quanta::Context& ctx = host.GetContext();
  DefineNodeClass(ctx);
  DefineCollectionClasses(ctx);
  DefineCharacterDataClasses(ctx);
  DefineElementClass(ctx);
  DefineDocumentClasses(ctx);

  // What Web IDL has and the embedding surface cannot say: the constants, which are on the interface
  // object and its prototype, and the static side of each interface inheriting its parent's.
  host.Evaluate(R"JS(
    (function () {
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
      define(Node, nodeTypes);
      define(Node.prototype, nodeTypes);
      for (const [child, parent] of [[CharacterData, Node], [Text, CharacterData], [CDATASection, Text], [ProcessingInstruction, CharacterData],
                                     [Comment, CharacterData], [DocumentType, Node], [DocumentFragment, Node], [Attr, Node], [Element, Node],
                                     [HTMLElement, Element], [Document, Node]]) {
        Object.setPrototypeOf(child, parent);
      }
    })();
  )JS");
}

}  // namespace

void InstallNodeApis(Quanta::Embed::Realm& realm) { Install(realm); }
void InstallNodeApis(Quanta::Embed::Runtime& runtime) { Install(runtime); }

}  // namespace solar::dom
