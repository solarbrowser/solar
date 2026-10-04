#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "quanta/Embed.h"
#include "solar/dom/Node.h"

// Custom elements (https://html.spec.whatwg.org/#custom-elements): the registry, the definitions in it, and the
// reactions (upgrades and callbacks) that the tree operations queue and that run when the operation that caused them
// returns to script.
namespace solar::dom {

enum class Callback { Connected, Disconnected, Adopted, AttributeChanged, ConnectedMove };

// A custom element definition. It keeps the values script gave it, so it is a cell of its own.
struct CustomDefinition : Quanta::DOMObject {
  std::string name;
  std::string localName;  // the same as name for an autonomous element, the element it extends otherwise
  Quanta::Value constructor;
  Quanta::Value callbacks[5];  // by Callback; undefined for none
  std::vector<std::string> observedAttributes;
  bool disableShadow = false;
  bool disableInternals = false;
  // The elements being upgraded or constructed by this definition; null is "already constructed".
  std::vector<Element*> constructionStack;

  void Visit(Quanta::Visitor& visitor);
};

// Whether any definition exists, which is what the tree operations ask before they look for custom elements.
bool HasCustomDefinitions();

// "look up a custom element definition". Null in a document that has no window, which is no browsing context.
CustomDefinition* LookupDefinition(Quanta::Context& ctx, const Document* document, std::string_view ns, std::string_view localName, const std::optional<std::string>& is);

// "create an element": the element for this name, which is a custom element if there is a definition for it. With
// `synchronous` the constructor of a definition is run now (and a failure is reported and gives an HTMLUnknownElement);
// without, an upgrade is queued.
Element* CreateElement(Quanta::Context& ctx, Document* document, std::string_view localName, std::string_view ns, std::string_view prefix = "", const std::optional<std::string>& is = std::nullopt,
                       bool synchronous = false);

// "try to upgrade an element", and "upgrade an element".
void TryUpgrade(Quanta::Context& ctx, Element* element);
void Upgrade(Quanta::Context& ctx, Element* element, CustomDefinition* definition);

// "enqueue a custom element callback reaction" for the callback of the element's definition, if it has one.
void EnqueueCallback(Quanta::Context& ctx, Element* element, Callback callback, std::vector<Quanta::Value> args = {});

// The steps the tree algorithms run for the nodes they put in, take out and move to another document.
void CustomAfterInsert(Node* node);
void CustomAfterRemove(Node* node, bool parentWasConnected);
void CustomAfterAdopt(Node* node, Document* oldDocument);
void CustomAfterMove(Node* node);
void CustomAttributeChanged(Element* element, const std::string& name, const std::string& ns, const std::optional<std::string>& oldValue,
                            const std::optional<std::string>& newValue);

// [CEReactions]: reactions queued while one of these is alive run when it goes, before the call returns to script.
class ReactionsScope {
 public:
  explicit ReactionsScope(Quanta::Context& ctx);
  ~ReactionsScope();
  ReactionsScope(const ReactionsScope&) = delete;
  ReactionsScope& operator=(const ReactionsScope&) = delete;

 private:
  Quanta::Context& ctx_;
};

// A native function that has [CEReactions]: the same function, with the reactions it queued run before it returns.
template <Quanta::Embed::NativeFn Fn>
Quanta::Value Reactions(Quanta::Context& ctx, Quanta::Value thisValue, Quanta::Embed::Args args, Quanta::Value newTarget) {
  ReactionsScope scope(ctx);
  return Fn(ctx, thisValue, args, newTarget);
}

// The [HTMLConstructor] steps for the interface constructor `active`, which makes the elements with one of
// `localNames` (any, for HTMLElement itself, which is given no names).
Quanta::Value ConstructHtmlElement(Quanta::Context& ctx, const Quanta::Value& newTarget, Quanta::Object* active, const std::vector<std::string_view>& localNames);
// HTMLElement's constructor.
Quanta::Value HtmlElementConstructor(Quanta::Context& ctx, Quanta::Value thisValue, Quanta::Embed::Args args, Quanta::Value newTarget);

// Reports an exception the way a window does for one that nothing caught.
void ReportException(Quanta::Context& ctx);

void DefineCustomElementClasses(Quanta::Context& ctx);

}  // namespace solar::dom
