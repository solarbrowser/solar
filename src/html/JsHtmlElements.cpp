#include <string>

#include "solar/dom/NodeBindingsInternal.h"
#include "solar/html/HtmlBindings.h"

namespace solar::html {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::Object;
using Quanta::Value;

namespace {

Value IllegalConstructor(Context& ctx, Value, qe::Args, Value) {
  qe::ThrowTypeError(ctx, "Illegal constructor");
  return qe::Undefined();
}

struct Interface {
  const char* name;
  std::initializer_list<const char*> tags;
};

// The interfaces the HTML Standard gives its elements. What each adds to HTMLElement is the standard's
// element-by-element work; here they are the objects script can test for and that carry that work.
const Interface kInterfaces[] = {
    {"HTMLAnchorElement", {"a"}},
    {"HTMLAreaElement", {"area"}},
    {"HTMLAudioElement", {"audio"}},
    {"HTMLBaseElement", {"base"}},
    {"HTMLBodyElement", {"body"}},
    {"HTMLBRElement", {"br"}},
    {"HTMLButtonElement", {"button"}},
    {"HTMLCanvasElement", {"canvas"}},
    {"HTMLDataElement", {"data"}},
    {"HTMLDataListElement", {"datalist"}},
    {"HTMLDetailsElement", {"details"}},
    {"HTMLDialogElement", {"dialog"}},
    {"HTMLDirectoryElement", {"dir"}},
    {"HTMLDivElement", {"div"}},
    {"HTMLDListElement", {"dl"}},
    {"HTMLEmbedElement", {"embed"}},
    {"HTMLFieldSetElement", {"fieldset"}},
    {"HTMLFontElement", {"font"}},
    {"HTMLFormElement", {"form"}},
    {"HTMLFrameElement", {"frame"}},
    {"HTMLFrameSetElement", {"frameset"}},
    {"HTMLHeadElement", {"head"}},
    {"HTMLHeadingElement", {"h1", "h2", "h3", "h4", "h5", "h6"}},
    {"HTMLHRElement", {"hr"}},
    {"HTMLHtmlElement", {"html"}},
    {"HTMLIFrameElement", {"iframe"}},
    {"HTMLImageElement", {"img"}},
    {"HTMLInputElement", {"input"}},
    {"HTMLLabelElement", {"label"}},
    {"HTMLLegendElement", {"legend"}},
    {"HTMLLIElement", {"li"}},
    {"HTMLLinkElement", {"link"}},
    {"HTMLMapElement", {"map"}},
    {"HTMLMarqueeElement", {"marquee"}},
    {"HTMLMenuElement", {"menu"}},
    {"HTMLMetaElement", {"meta"}},
    {"HTMLMeterElement", {"meter"}},
    {"HTMLModElement", {"ins", "del"}},
    {"HTMLObjectElement", {"object"}},
    {"HTMLOListElement", {"ol"}},
    {"HTMLOptGroupElement", {"optgroup"}},
    {"HTMLOptionElement", {"option"}},
    {"HTMLOutputElement", {"output"}},
    {"HTMLParagraphElement", {"p"}},
    {"HTMLParamElement", {"param"}},
    {"HTMLPictureElement", {"picture"}},
    {"HTMLPreElement", {"pre", "listing", "xmp"}},
    {"HTMLProgressElement", {"progress"}},
    {"HTMLQuoteElement", {"blockquote", "q"}},
    {"HTMLScriptElement", {"script"}},
    {"HTMLSelectElement", {"select"}},
    {"HTMLSlotElement", {"slot"}},
    {"HTMLSourceElement", {"source"}},
    {"HTMLSpanElement", {"span"}},
    {"HTMLStyleElement", {"style"}},
    {"HTMLTableCaptionElement", {"caption"}},
    {"HTMLTableCellElement", {"td", "th"}},
    {"HTMLTableColElement", {"col", "colgroup"}},
    {"HTMLTableElement", {"table"}},
    {"HTMLTableRowElement", {"tr"}},
    {"HTMLTableSectionElement", {"thead", "tbody", "tfoot"}},
    {"HTMLTemplateElement", {"template"}},
    {"HTMLTextAreaElement", {"textarea"}},
    {"HTMLTimeElement", {"time"}},
    {"HTMLTitleElement", {"title"}},
    {"HTMLTrackElement", {"track"}},
    {"HTMLUListElement", {"ul"}},
    {"HTMLVideoElement", {"video"}},
};

// The elements that have no interface of their own: HTMLElement is theirs.
const char* const kPlainTags[] = {"abbr", "acronym", "address", "article", "aside", "b", "basefont", "bdi", "bdo", "bgsound", "big", "center", "cite", "code", "dd", "dfn", "dt",
                                  "em", "figcaption", "figure", "footer", "header", "hgroup", "i", "kbd", "keygen", "main", "mark", "nav", "nobr", "noembed", "noframes",
                                  "noscript", "rb", "rp", "rt", "rtc", "ruby", "s", "samp", "search", "section", "small", "strike", "strong", "sub", "summary", "sup", "tt",
                                  "u", "var", "wbr"};

Value GetTemplateContent(Context& ctx, Value t, qe::Args, Value) {
  dom::Element* self = dom::ThisElement(ctx, t);
  if (!self) return qe::Undefined();
  if (!self->IsHtml("template") || !self->templateContents) {
    qe::ThrowTypeError(ctx, "Illegal invocation");
    return qe::Undefined();
  }
  return qe::FromObject(self->templateContents);
}

}  // namespace

void DefineHtmlElementInterfaces(Context& ctx) {
  Object* htmlElement = dom::InterfacePrototype(ctx, dom::Interface::HtmlElement);

  qe::ClassRef unknown = qe::DefineClass(ctx, "HTMLUnknownElement", IllegalConstructor, 0, htmlElement);
  qe::DefineGlobal(ctx, "HTMLUnknownElement", unknown.constructor);
  dom::SetUnknownHtmlElementInterface(ctx, unknown.prototype);

  for (const Interface& interface : kInterfaces) {
    qe::ClassRef definition = qe::DefineClass(ctx, interface.name, IllegalConstructor, 0, htmlElement);
    qe::DefineGlobal(ctx, interface.name, definition.constructor);
    for (const char* tag : interface.tags) dom::RegisterHtmlElementInterface(ctx, tag, definition.prototype);
    if (std::string_view(interface.name) == "HTMLTemplateElement") qe::DefineAccessor(definition.prototype, "content", GetTemplateContent, nullptr);
  }
  for (const char* tag : kPlainTags) dom::RegisterHtmlElementInterface(ctx, tag, htmlElement);
}

}  // namespace solar::html
