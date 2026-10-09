#include <string>
#include <vector>

#include "solar/dom/CustomElements.h"
#include "solar/dom/NodeBindingsInternal.h"
#include "solar/html/HtmlBindings.h"
#include "solar/web/DomBindingsInternal.h"

// HTMLElement.dataset, a DOMStringMap (https://html.spec.whatwg.org/multipage/dom.html#domstringmap): the data-* attributes of an
// element as properties, data-foo-bar as fooBar.
namespace solar::html {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::DOMObject;
using Quanta::Heap;
using Quanta::Object;
using Quanta::Value;

struct JsDataset : DOMObject {
  dom::Element* element = nullptr;

  // data-foo-bar -> fooBar; nothing for an attribute that is not one (no prefix, or an uppercase letter in the name).
  static bool PropertyOf(const std::string& attribute, std::string& name) {
    if (attribute.size() < 5 || attribute.compare(0, 5, "data-") != 0) return false;
    for (char c : attribute) {
      if (c >= 'A' && c <= 'Z') return false;
    }
    name.clear();
    for (size_t i = 5; i < attribute.size(); ++i) {
      if (attribute[i] == '-' && i + 1 < attribute.size() && attribute[i + 1] >= 'a' && attribute[i + 1] <= 'z') {
        name += static_cast<char>(attribute[i + 1] - 0x20);
        ++i;
      } else {
        name += attribute[i];
      }
    }
    return true;
  }

  static std::string AttributeOf(const std::string& name) {
    std::string attribute = "data-";
    for (char c : name) {
      if (c >= 'A' && c <= 'Z') {
        attribute += '-';
        attribute += static_cast<char>(c + 0x20);
      } else {
        attribute += c;
      }
    }
    return attribute;
  }

  static bool NamedGetter(Context& ctx, JsDataset& self, const std::string& name, Value& out) {
    const std::string attribute = AttributeOf(name);
    std::string back;
    // A name that does not come back to itself (a "-" before a lowercase letter) is not one of the properties.
    if (!PropertyOf(attribute, back) || back != name) return false;
    const dom::Attr* found = self.element->FindAttribute("", attribute);
    if (!found) return false;
    out = qe::FromWtf8(ctx, found->value);
    return true;
  }

  static void NamedSetter(Context& ctx, JsDataset& self, const std::string& name, const Value& value) {
    dom::ReactionsScope reactions(ctx);
    const std::string text = qe::ToWtf8(ctx, value);
    if (qe::HasException(ctx)) return;
    for (size_t i = 0; i + 1 < name.size(); ++i) {
      if (name[i] == '-' && name[i + 1] >= 'a' && name[i + 1] <= 'z') {
        web::ThrowDomException(ctx, "'" + name + "' is not a valid property name.", "SyntaxError");
        return;
      }
    }
    if (auto error = dom::SetAttribute(ctx, self.element, AttributeOf(name), text)) dom::Throw(ctx, *error);
  }

  static bool NamedDeleter(Context&, JsDataset& self, const std::string& name) {
    dom::RemoveAttribute(self.element, AttributeOf(name));
    return true;
  }

  static std::vector<std::string> NamedKeys(Context&, JsDataset& self) {
    std::vector<std::string> keys;
    for (const dom::Attr* attribute : self.element->attributes) {
      std::string name;
      if (attribute->namespaceUri.empty() && PropertyOf(attribute->localName, name)) keys.push_back(name);
    }
    return keys;
  }

  static constexpr bool LegacyUnenumerableNamedProperties = false;

  void Visit(Quanta::Visitor& visitor) { visitor.Mark(element); }
};

namespace {

char g_key;

Value IllegalConstructor(Context& ctx, Value, qe::Args, Value) {
  qe::ThrowTypeError(ctx, "Illegal constructor");
  return qe::Undefined();
}

Value GetDataset(Context& ctx, Value t, qe::Args, Value) {
  dom::Element* self = dom::ThisElement(ctx, t);
  if (!self) return qe::Undefined();
  if (!self->dataset) {
    JsDataset* map = Heap::Allocate<JsDataset>();
    map->initialize_prototype(static_cast<Object*>(qe::GetRealmData(ctx, &g_key)));
    map->element = self;
    self->dataset = map;
    self->NoteWrite();
  }
  return qe::FromObject(self->dataset);
}

}  // namespace

void DefineDataset(Context& ctx) {
  qe::ClassRef map = qe::DefineClass(ctx, "DOMStringMap", IllegalConstructor, 0);
  qe::SetRealmData(ctx, &g_key, map.prototype);
  qe::DefineGlobal(ctx, "DOMStringMap", map.constructor);
  if (Object* element = dom::InterfacePrototype(ctx, dom::Interface::HtmlElement)) qe::DefineAccessor(element, "dataset", GetDataset, nullptr);
}

}  // namespace solar::html
