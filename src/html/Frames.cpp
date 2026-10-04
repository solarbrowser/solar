#include "solar/html/Frames.h"

#include <algorithm>
#include <cstdio>

#include "solar/dom/CustomElements.h"
#include "solar/dom/NodeBindingsInternal.h"
#include "solar/html/HtmlBindings.h"
#include "solar/html/Parser.h"
#include "solar/html/Reflect.h"
#include "solar/html/Xml.h"
#include "solar/url/Parser.h"
#include "solar/url/Serializer.h"

namespace solar::html {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::Object;
using Quanta::Value;

namespace {

FrameEnvironment* g_environment = nullptr;

bool IsIframe(const dom::Node* node) {
  const dom::Element* element = dom::AsElement(node);
  return element && element->IsHtml("iframe");
}

// The address that relative ones in a document are made absolute against: its own, or for a document with none of
// its own (about:srcdoc), the one of the page it is in.
std::string BaseOf(const dom::Document* document) {
  if (document->url.starts_with("about:") && document->frameElement && document->frameElement->nodeDocument && document->frameElement->nodeDocument != document) {
    return BaseOf(document->frameElement->nodeDocument);
  }
  return document->url;
}

std::string Resolve(const std::string& relative, const std::string& base) {
  std::optional<url::Url> baseUrl = url::Parse(base);
  std::optional<url::Url> parsed = url::Parse(relative, baseUrl ? &*baseUrl : nullptr);
  return parsed ? url::Serialize(*parsed) : relative;
}

}  // namespace

namespace reflect {
std::string ResolveAgainst(const dom::Element* element, const std::string& value) {
  return element->nodeDocument ? Resolve(value, BaseOf(element->nodeDocument)) : value;
}
}  // namespace reflect

namespace {

// ---- Running host code as a realm ----

std::vector<const std::function<void()>*>& Pending() {
  static std::vector<const std::function<void()>*> pending;
  return pending;
}

Value RunHost(Context&, Value, qe::Args, Value) {
  if (Pending().empty()) return qe::Undefined();
  const std::function<void()>* work = Pending().back();
  Pending().pop_back();
  (*work)();
  return qe::Undefined();
}

// ---- Firing events and tasks, in script ----

// What the window script hands over: (target, type) => target.dispatchEvent(new Event(type)).
void FireEvent(Context& ctx, dom::Node* target, const char* type) {
  Object* holder = dom::RealmHolder(ctx);
  if (!holder) return;
  Value fire = qe::Get(ctx, qe::FromObject(holder), "fireEvent");
  if (!qe::IsCallable(fire)) return;
  Value arguments[] = {qe::FromObject(target), qe::FromUtf8(ctx, type)};
  qe::Call(ctx, fire, qe::Undefined(), qe::Args(arguments, 2));
  if (qe::HasException(ctx)) ctx.clear_exception();
}

// A task: the function run by a timer of no delay, with these arguments.
void QueueTask(Context& ctx, const char* function, std::vector<Value> arguments) {
  Object* holder = dom::RealmHolder(ctx);
  if (!holder) return;
  Value task = qe::Get(ctx, qe::FromObject(holder), function);
  Value setTimeout = qe::Get(ctx, qe::FromObject(ctx.get_global_object()), "setTimeout");
  if (!qe::IsCallable(task) || !qe::IsCallable(setTimeout)) return;
  std::vector<Value> all = {task, qe::FromUint32(0)};
  for (const Value& argument : arguments) all.push_back(argument);
  qe::Call(ctx, setTimeout, qe::Undefined(), qe::Args(all.data(), all.size()));
  if (qe::HasException(ctx)) ctx.clear_exception();
}

// ---- Loading a page ----

bool LoadPageIn(Quanta::Embed::Realm& realm, dom::Document* document, const std::string& markup, const std::string& contentType);

bool RunScript(Quanta::Embed::Realm& realm, dom::Document* document, dom::Element* script) {
  const dom::Attr* type = script->FindAttribute("", "type");
  if (type && !type->value.empty() && type->value != "text/javascript" && type->value != "application/javascript") return true;
  std::string code, filename = document->url;
  if (const dom::Attr* source = script->FindAttribute("", "src")) {
    const std::string address = Resolve(source->value, BaseOf(document));
    if (g_environment && g_environment->SkipScript(address)) return true;
    std::optional<std::string> loaded = g_environment ? g_environment->Load(address) : std::nullopt;
    if (!loaded) {
      if (g_environment) g_environment->ScriptFailed("cannot load script " + source->value);
      return false;
    }
    code = std::move(*loaded);
    filename = address;
  } else {
    code = script->DescendantText();
  }
  // A script in a shadow tree is not the document's current script.
  const dom::Node* root = script->Root();
  const bool inDocument = !(root->IsFragment() && static_cast<const dom::DocumentFragment*>(root)->isShadowRoot);
  // While it runs, the current script is it, or none if it is in a shadow tree; and then what it was.
  dom::Element* previous = document->currentScript;
  document->currentScript = inDocument ? script : nullptr;
  const qe::EvaluateResult result = realm.Evaluate(code, filename);
  document->currentScript = previous;
  if (g_environment) g_environment->RunJobs();
  if (!result.ok) {
    if (g_environment) g_environment->ScriptFailed(result.error);
    return false;
  }
  return true;
}

// A frame's page has finished loading, or has given up: the iframe fires load, and what waited for it goes on.
void FrameFinished(dom::Element* iframe, bool fire) {
  dom::Document* parent = iframe->nodeDocument;
  if (!parent) return;
  if (fire && parent->context) FireEvent(*parent->context, iframe, "load");
  if (parent->pendingFrameLoads > 0) --parent->pendingFrameLoads;
  if (parent->pendingFrameLoads == 0 && parent->whenFramesLoaded) {
    std::function<void()> next = std::move(parent->whenFramesLoaded);
    parent->whenFramesLoaded = nullptr;
    next();
  }
}

// ---- Nested browsing contexts ----

void RunAs(Quanta::Embed::Realm& realm, const std::function<void()>& work) {
  Pending().push_back(&work);
  const qe::EvaluateResult result = realm.Evaluate("__solarRunHost()", "host");
  if (!Pending().empty() && Pending().back() == &work) {
    std::fprintf(stderr, "RunAs: the host work was not run: %s\n", result.error.c_str());
    Pending().pop_back();
  }
}

struct FrameContext {
  Quanta::Embed::Realm* realm = nullptr;
  dom::Document* document = nullptr;
};

// A realm with a window and an (empty, so far) document for the iframe.
std::optional<FrameContext> MakeContext(dom::Element* iframe, const std::string& url) {
  if (!g_environment) return std::nullopt;
  Quanta::Embed::Realm* realm = g_environment->CreateRealm();
  if (!realm) return std::nullopt;
  dom::Document* document = nullptr;
  RunAs(*realm, [&] {
    Context& ctx = realm->GetContext();
    document = dom::NewDocument(ctx, true);
    document->url = url;
    document->frameElement = iframe;
    InstallWindow(*realm, document);
  });
  if (!document) return std::nullopt;
  return FrameContext{realm, document};
}

void Adopt(dom::Element* iframe, const FrameContext& context) {
  iframe->contentDocument = context.document;
  iframe->contentWindow = context.realm->GetContext().get_global_object();
  iframe->NoteWrite();
}

void StartLoad(dom::Element* iframe) {
  dom::Document* parent = iframe->nodeDocument;
  if (!parent || !parent->context) return;
  ++iframe->frameLoad;
  ++parent->pendingFrameLoads;
  QueueTask(*parent->context, "frameTask", {qe::FromObject(iframe), qe::FromUint32(static_cast<uint32_t>(iframe->frameLoad))});
}

// "create a new nested browsing context", and start what the iframe's attributes say to load in it.
void CreateContext(dom::Element* iframe) {
  dom::Document* parent = iframe->nodeDocument;
  if (!parent || !parent->window || iframe->contentDocument) return;
  std::optional<FrameContext> context = MakeContext(iframe, "about:blank");
  if (!context) return;
  // The initial about:blank document: an empty page, which is complete at once.
  RunAs(*context->realm, [&] {
    ParseDocument(context->realm->GetContext(), context->document, "", ScriptingMode::Normal);
    context->document->readyState = "complete";
  });
  Adopt(iframe, *context);
  StartLoad(iframe);
}

void DiscardContext(dom::Element* iframe) {
  if (!iframe->contentDocument) return;
  iframe->contentDocument->frameElement = nullptr;
  iframe->contentDocument = nullptr;
  iframe->contentWindow = nullptr;
  ++iframe->frameLoad;
  iframe->NoteWrite();
}

// The elements of a tree, shadow trees included, that are HTML elements of this name.
std::vector<dom::Element*> HtmlElementsIn(dom::Node* root, std::string_view name) {
  std::vector<dom::Element*> found;
  for (dom::Node* node = root; node; node = node->NextInTree(root)) {
    if (dom::Element* element = dom::AsElement(node)) {
      if (element->IsHtml(name)) found.push_back(element);
      if (element->shadowRoot) {
        for (dom::Element* inner : HtmlElementsIn(element->shadowRoot, name)) found.push_back(inner);
      }
    }
  }
  return found;
}

std::vector<dom::Element*> IframesIn(dom::Node* root) { return HtmlElementsIn(root, "iframe"); }

// "prepare the script element" for one that script made and put in a document: an inline script runs now, and one with
// a source, as a task of its own once it is loaded.
void PrepareScript(dom::Element* script) {
  if (script->scriptStarted || script->scriptParserInserted) return;
  dom::Document* document = script->nodeDocument;
  if (!document || !document->window || !document->realm || !document->context) return;
  const bool hasSource = script->FindAttribute("", "src") != nullptr;
  if (!hasSource && script->DescendantText().empty()) return;
  script->scriptStarted = true;
  if (hasSource) {
    QueueTask(*document->context, "scriptTask", {qe::FromObject(script)});
  } else {
    RunScript(*document->realm, document, script);
  }
}

// The task of a script with a source: __solarScriptTask(script).
Value ScriptTask(Context&, Value, qe::Args args, Value) {
  dom::Element* script = args.empty() ? nullptr : Quanta::DOMObject::Cast<dom::Element>(args[0]);
  if (!script || !script->nodeDocument || !script->nodeDocument->realm) return qe::Undefined();
  RunScript(*script->nodeDocument->realm, script->nodeDocument, script);
  return qe::Undefined();
}

void AfterInsert(dom::Node* node) {
  if (!node->IsElement() && !node->IsFragment()) return;
  dom::Node* root = dom::ShadowIncludingRoot(node);
  if (!root || !root->IsDocument()) return;
  for (dom::Element* iframe : IframesIn(node)) CreateContext(iframe);
  for (dom::Element* script : HtmlElementsIn(node, "script")) PrepareScript(script);
}

void AfterRemove(dom::Node* node, bool) {
  if (!node->IsElement()) return;
  for (dom::Element* iframe : IframesIn(node)) DiscardContext(iframe);
}

// An on<event> content attribute is a handler, which the window script compiles: (element, name, value or null).
void ContentHandlerChanged(dom::Element* element, const std::string& name) {
  if (!element->IsHtml() || !element->nodeDocument || !element->nodeDocument->context) return;
  Context& ctx = *element->nodeDocument->context;
  Object* holder = dom::RealmHolder(ctx);
  if (!holder) return;
  Value compile = qe::Get(ctx, qe::FromObject(holder), "contentHandler");
  if (!qe::IsCallable(compile)) return;
  const std::optional<std::string> value = dom::GetAttribute(element, name);
  Value arguments[] = {qe::FromObject(element), qe::FromWtf8(ctx, name), value ? qe::FromWtf8(ctx, *value) : qe::Null()};
  qe::Call(ctx, compile, qe::Undefined(), qe::Args(arguments, 3));
  if (qe::HasException(ctx)) ctx.clear_exception();
}

void AttributeChanged(dom::Element* element, const std::string& name) {
  if (name.size() > 2 && name.starts_with("on")) ContentHandlerChanged(element, name);
  if (!IsIframe(element) || (name != "src" && name != "srcdoc")) return;
  dom::Node* root = dom::ShadowIncludingRoot(element);
  if (!root || !root->IsDocument() || !element->contentDocument) return;
  StartLoad(element);
}

// The task that loads what the iframe asks for: __solarFrameTask(iframe, load).
Value FrameTask(Context& ctx, Value, qe::Args args, Value) {
  dom::Element* iframe = args.empty() ? nullptr : Quanta::DOMObject::Cast<dom::Element>(args[0]);
  if (!iframe || !g_environment) return qe::Undefined();
  const uint64_t load = args.size() > 1 ? qe::ToUint32(ctx, args[1]) : 0;
  if (load != iframe->frameLoad || !iframe->contentDocument) {
    // Not the load that counts any more: the count it was in goes down all the same.
    FrameFinished(iframe, false);
    return qe::Undefined();
  }
  dom::Document* parent = iframe->nodeDocument;
  std::string markup;
  std::string address = "about:blank";
  bool navigate = false;
  if (const dom::Attr* srcdoc = iframe->FindAttribute("", "srcdoc")) {
    markup = srcdoc->value;
    address = "about:srcdoc";
    navigate = true;
  } else if (const dom::Attr* src = iframe->FindAttribute("", "src"); src && !src->value.empty()) {
    const std::string resolved = Resolve(src->value, BaseOf(parent));
    if (resolved != "about:blank" && !resolved.starts_with("javascript:")) {
      address = resolved;
      navigate = true;
      if (resolved.starts_with("about:")) {
        markup = "";
      } else if (std::optional<std::string> loaded = g_environment->Load(resolved)) {
        markup = std::move(*loaded);
      }
    }
  }
  if (!navigate) {
    // About:blank, which is already there.
    FrameFinished(iframe, true);
    return qe::Undefined();
  }
  std::optional<FrameContext> context = MakeContext(iframe, address);
  if (!context) {
    FrameFinished(iframe, true);
    return qe::Undefined();
  }
  Adopt(iframe, *context);
  LoadPage(*context->realm, context->document, markup, address.starts_with("about:") ? "text/html" : g_environment->ContentType(address));
  return qe::Undefined();
}

}  // namespace

void SetFrameEnvironment(FrameEnvironment* environment) { g_environment = environment; }

void PrepareRealm(Quanta::Embed::Realm& realm) { qe::DefineGlobalFunction(realm.GetContext(), "__solarRunHost", RunHost, 0); }

void RunInRealm(Quanta::Embed::Realm& realm, const std::function<void()>& work) { RunAs(realm, work); }

bool LoadPage(Quanta::Embed::Realm& realm, dom::Document* document, std::string_view markup, std::string_view contentType) {
  bool ok = true;
  const std::string text(markup), type(contentType);
  RunAs(realm, [&] { ok = LoadPageIn(realm, document, text, type); });
  return ok;
}

namespace {

bool LoadPageIn(Quanta::Embed::Realm& realm, dom::Document* document, const std::string& markup, const std::string& contentType) {
  Context& ctx = realm.GetContext();
  document->readyState = "loading";
  bool ok = true;
  const auto run = [&](dom::Element* script) {
    script->scriptStarted = true;
    if (!RunScript(realm, document, script)) ok = false;
  };
  if (contentType == "text/html") {
    ParseDocument(ctx, document, markup, ScriptingMode::Normal, nullptr, run);
  } else {
    // An XML document: a page of its own kind of parser, which stops at the first thing wrong.
    document->isHtml = false;
    document->contentType = contentType;
    const XmlResult result = ParseXmlDocument(ctx, document, markup, run);
    if (!result.ok) {
      while (document->firstChild) dom::RemoveUnchecked(document->firstChild);
      dom::Element* error = dom::NewElement(ctx, document, "parsererror", "http://www.mozilla.org/newlayout/xml/parsererror.xml");
      dom::AppendChild(error, dom::NewText(ctx, document, result.error));
      dom::AppendChild(document, error);
    }
  }
  document->readyState = "interactive";
  realm.Evaluate("document.dispatchEvent(new Event('DOMContentLoaded', { bubbles: true }))", "page");
  const auto finish = [&realm, document] {
    document->readyState = "complete";
    realm.Evaluate("window.dispatchEvent(new Event('load'))", "page");
    // The load of a frame is done when its page's is.
    if (dom::Element* iframe = document->frameElement; iframe && iframe->contentDocument == document) FrameFinished(iframe, true);
  };
  if (document->pendingFrameLoads == 0) finish();
  else document->whenFramesLoaded = finish;
  return ok;
}

}  // namespace

void InstallFrameHooks() {
  dom::TreeHooks hooks;
  hooks.afterInsert = AfterInsert;
  hooks.afterRemove = AfterRemove;
  hooks.attributeChanged = AttributeChanged;
  dom::SetTreeHooks(hooks);
}

// ---- What window gets of its frames ----

namespace {

dom::Document* DocumentOfRealm(Context& ctx) { return dom::AssociatedDocument(ctx); }

// The windows of the iframes of a document, in tree order.
std::vector<Object*> ChildWindows(dom::Document* document) {
  std::vector<Object*> windows;
  if (!document) return windows;
  for (dom::Element* iframe : IframesIn(document)) {
    if (iframe->contentWindow) windows.push_back(iframe->contentWindow);
  }
  return windows;
}

// __solarParent(): the window this one is in, or null.
Value ParentWindow(Context& ctx, Value, qe::Args, Value) {
  dom::Document* document = DocumentOfRealm(ctx);
  dom::Element* iframe = document ? document->frameElement : nullptr;
  if (!iframe || iframe->contentDocument != document || !iframe->nodeDocument || !iframe->nodeDocument->globalObject) return qe::Null();
  return qe::FromObject(iframe->nodeDocument->globalObject);
}

Value FrameElement(Context& ctx, Value, qe::Args, Value) {
  dom::Document* document = DocumentOfRealm(ctx);
  dom::Element* iframe = document ? document->frameElement : nullptr;
  if (!iframe || iframe->contentDocument != document) return qe::Null();
  return qe::FromObject(iframe);
}

// __solarRegisterContentHandler(compile): the script's function that makes handlers of the on<event> attributes.
Value RegisterContentHandler(Context& ctx, Value, qe::Args args, Value) {
  if (Object* holder = dom::RealmHolder(ctx); holder && !args.empty()) qe::Set(ctx, qe::FromObject(holder), "contentHandler", args[0]);
  return qe::Undefined();
}

// __solarRegisterFire(fire): the window script's (target, type) => event function.
Value RegisterFire(Context& ctx, Value, qe::Args args, Value) {
  if (Object* holder = dom::RealmHolder(ctx); holder && !args.empty()) qe::Set(ctx, qe::FromObject(holder), "fireEvent", args[0]);
  return qe::Undefined();
}

Value ChildCount(Context& ctx, Value, qe::Args, Value) { return qe::FromUint32(static_cast<uint32_t>(ChildWindows(DocumentOfRealm(ctx)).size())); }

Value ChildWindow(Context& ctx, Value, qe::Args args, Value) {
  const std::vector<Object*> windows = ChildWindows(DocumentOfRealm(ctx));
  const uint32_t index = args.empty() ? 0 : qe::ToUint32(ctx, args[0]);
  return index < windows.size() ? qe::FromObject(windows[index]) : qe::Undefined();
}

// The iframe element members: the document and window of the context, and the attributes it is made from.
dom::Element* ThisIframe(Context& ctx, const Value& t) {
  dom::Element* self = dom::ThisElement(ctx, t);
  if (self && !self->IsHtml("iframe")) {
    qe::ThrowTypeError(ctx, "Illegal invocation");
    return nullptr;
  }
  return self;
}

Value GetContentDocument(Context& ctx, Value t, qe::Args, Value) {
  dom::Element* self = ThisIframe(ctx, t);
  return self ? (self->contentDocument ? qe::FromObject(self->contentDocument) : qe::Null()) : qe::Undefined();
}

Value GetContentWindow(Context& ctx, Value t, qe::Args, Value) {
  dom::Element* self = ThisIframe(ctx, t);
  return self ? (self->contentWindow ? qe::FromObject(self->contentWindow) : qe::Null()) : qe::Undefined();
}

// src: an address, so it reads back made absolute against the document it is in.
Value GetSrc(Context& ctx, Value t, qe::Args, Value) {
  dom::Element* self = ThisIframe(ctx, t);
  if (!self) return qe::Undefined();
  const std::optional<std::string> value = dom::GetAttribute(self, "src");
  if (!value) return qe::FromUtf8(ctx, "");
  return qe::FromWtf8(ctx, self->nodeDocument ? Resolve(*value, BaseOf(self->nodeDocument)) : *value);
}

template <const char* Attribute>
Value GetString(Context& ctx, Value t, qe::Args, Value) {
  dom::Element* self = ThisIframe(ctx, t);
  return self ? qe::FromWtf8(ctx, dom::GetAttribute(self, Attribute).value_or("")) : qe::Undefined();
}

template <const char* Attribute>
Value SetString(Context& ctx, Value t, qe::Args args, Value) {
  dom::Element* self = ThisIframe(ctx, t);
  if (!self || args.empty()) return qe::Undefined();
  std::string text = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  dom::SetAttribute(ctx, self, Attribute, std::move(text));
  return qe::Undefined();
}

template <const char* Attribute>
Value GetFlag(Context& ctx, Value t, qe::Args, Value) {
  dom::Element* self = ThisIframe(ctx, t);
  return self ? qe::FromBool(dom::GetAttribute(self, Attribute).has_value()) : qe::Undefined();
}

template <const char* Attribute>
Value SetFlag(Context& ctx, Value t, qe::Args args, Value) {
  dom::Element* self = ThisIframe(ctx, t);
  if (!self || args.empty()) return qe::Undefined();
  if (args[0].to_boolean()) dom::SetAttribute(ctx, self, Attribute, "");
  else dom::RemoveAttribute(self, Attribute);
  return qe::Undefined();
}

constexpr char kSrc[] = "src";
constexpr char kSrcdoc[] = "srcdoc";
constexpr char kName[] = "name";
constexpr char kAllow[] = "allow";
constexpr char kWidth[] = "width";
constexpr char kHeight[] = "height";
constexpr char kLoading[] = "loading";
constexpr char kReferrerPolicy[] = "referrerpolicy";
constexpr char kAllowFullscreen[] = "allowfullscreen";

}  // namespace

void DefineFrameNatives(Context& ctx) {
  qe::DefineGlobalFunction(ctx, "__solarParent", ParentWindow, 0);
  qe::DefineGlobalFunction(ctx, "__solarFrameElement", FrameElement, 0);
  qe::DefineGlobalFunction(ctx, "__solarChildCount", ChildCount, 0);
  qe::DefineGlobalFunction(ctx, "__solarChildWindow", ChildWindow, 1);
  qe::DefineGlobalFunction(ctx, "__solarFrameTask", FrameTask, 2);
  qe::DefineGlobalFunction(ctx, "__solarRegisterFire", RegisterFire, 1);
  qe::DefineGlobalFunction(ctx, "__solarScriptTask", ScriptTask, 1);
  qe::DefineGlobalFunction(ctx, "__solarRegisterContentHandler", RegisterContentHandler, 1);
  // The task function is kept for QueueTask, which finds it on the holder.
  if (Object* holder = dom::RealmHolder(ctx)) {
    qe::Set(ctx, qe::FromObject(holder), "frameTask", qe::Get(ctx, qe::FromObject(ctx.get_global_object()), "__solarFrameTask"));
    qe::Set(ctx, qe::FromObject(holder), "scriptTask", qe::Get(ctx, qe::FromObject(ctx.get_global_object()), "__solarScriptTask"));
  }
}

// HTMLScriptElement: the attributes, and text, which is the content.
Value GetScriptText(Context& ctx, Value t, qe::Args, Value) {
  dom::Element* self = reflect::ThisHtml(ctx, t);
  return self ? qe::FromWtf8(ctx, self->DescendantText()) : qe::Undefined();
}

Value SetScriptText(Context& ctx, Value t, qe::Args args, Value) {
  dom::Element* self = reflect::ThisHtml(ctx, t);
  if (!self || args.empty()) return qe::Undefined();
  std::string text = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  while (self->firstChild) dom::RemoveUnchecked(self->firstChild);
  if (!text.empty()) dom::AppendChild(self, dom::NewText(ctx, self->nodeDocument, std::move(text)));
  return qe::Undefined();
}

constexpr char kType[] = "type";
constexpr char kCharset[] = "charset";
constexpr char kAsync[] = "async";
constexpr char kDefer[] = "defer";
constexpr char kNoModule[] = "nomodule";
constexpr char kCrossOrigin[] = "crossorigin";
constexpr char kIntegrity[] = "integrity";
constexpr char kFetchPriority[] = "fetchpriority";

void DefineScriptMembers(Context&, Object* prototype) {
  qe::DefineAccessor(prototype, "src", reflect::GetUrl<kSrc>, dom::Reactions<reflect::SetString<kSrc>>);
  qe::DefineAccessor(prototype, "type", reflect::GetString<kType>, dom::Reactions<reflect::SetString<kType>>);
  qe::DefineAccessor(prototype, "charset", reflect::GetString<kCharset>, dom::Reactions<reflect::SetString<kCharset>>);
  qe::DefineAccessor(prototype, "defer", reflect::GetFlag<kDefer>, dom::Reactions<reflect::SetFlag<kDefer>>);
  qe::DefineAccessor(prototype, "noModule", reflect::GetFlag<kNoModule>, dom::Reactions<reflect::SetFlag<kNoModule>>);
  qe::DefineAccessor(prototype, "integrity", reflect::GetString<kIntegrity>, dom::Reactions<reflect::SetString<kIntegrity>>);
  qe::DefineAccessor(prototype, "fetchPriority", reflect::GetString<kFetchPriority>, dom::Reactions<reflect::SetString<kFetchPriority>>);
  qe::DefineAccessor(prototype, "text", GetScriptText, dom::Reactions<SetScriptText>);
}

void DefineIframeMembers(Context& ctx, Object* prototype) {
  qe::DefineAccessor(prototype, "contentDocument", GetContentDocument, nullptr);
  qe::DefineAccessor(prototype, "contentWindow", GetContentWindow, nullptr);
  qe::DefineAccessor(prototype, "src", GetSrc, dom::Reactions<SetString<kSrc>>);
  qe::DefineAccessor(prototype, "srcdoc", GetString<kSrcdoc>, dom::Reactions<SetString<kSrcdoc>>);
  qe::DefineAccessor(prototype, "name", GetString<kName>, dom::Reactions<SetString<kName>>);
  qe::DefineAccessor(prototype, "allow", GetString<kAllow>, dom::Reactions<SetString<kAllow>>);
  qe::DefineAccessor(prototype, "width", GetString<kWidth>, dom::Reactions<SetString<kWidth>>);
  qe::DefineAccessor(prototype, "height", GetString<kHeight>, dom::Reactions<SetString<kHeight>>);
  qe::DefineAccessor(prototype, "loading", GetString<kLoading>, dom::Reactions<SetString<kLoading>>);
  qe::DefineAccessor(prototype, "referrerPolicy", GetString<kReferrerPolicy>, dom::Reactions<SetString<kReferrerPolicy>>);
  qe::DefineAccessor(prototype, "allowFullscreen", GetFlag<kAllowFullscreen>, dom::Reactions<SetFlag<kAllowFullscreen>>);
}

}  // namespace solar::html
