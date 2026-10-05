#include "solar/html/Frames.h"

#include <algorithm>
#include <cstdio>

#include "solar/dom/CustomElements.h"
#include "solar/dom/NodeBindingsInternal.h"
#include "solar/html/HtmlBindings.h"
#include "solar/html/Parser.h"
#include "solar/html/Errors.h"
#include "solar/html/Modules.h"
#include "solar/html/Reflect.h"
#include "solar/html/TreeBuilder.h"
#include "solar/html/Xml.h"
#include "solar/web/BlobUrls.h"
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

std::string DocumentBaseUrl(const dom::Document* document) { return BaseOf(document); }

std::optional<std::string> LoadResource(const std::string& url) {
  if (url.starts_with("blob:")) {
    if (auto blob = web::LookupBlobUrl(url)) return *blob->data;
    return std::nullopt;
  }
  return g_environment ? g_environment->Load(url) : std::nullopt;
}

namespace reflect {
std::string ResolveAgainst(const dom::Element* element, const std::string& value) {
  return element->nodeDocument ? Resolve(value, BaseOf(element->nodeDocument)) : value;
}
}  // namespace reflect

namespace {

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

// A task of the realm the document is in, which keeps `element` alive until it has run.
void QueueTask(dom::Document* document, const char* label, dom::Element* element, std::function<void()> work) {
  if (!document || !document->realm || !document->context) return;
  auto keep = std::make_shared<qe::Persistent>(*document->context, qe::FromObject(element));
  document->realm->EnqueueTask(label, [keep, work = std::move(work)] { work(); });
}

// ---- Loading a page ----

bool LoadPageIn(Quanta::Embed::Realm& realm, dom::Document* document, const std::string& markup, const std::string& contentType);
void CompleteLoad(Quanta::Embed::Realm& realm, dom::Document* document, bool notifyFrame);

std::string LowerType(const dom::Element* script) {
  const dom::Attr* type = script->FindAttribute("", "type");
  std::string value = type ? type->value : "";
  for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value;
}

// A module script: an inline one is the module of the document at an address of its own, one with a source is fetched
// with the modules it imports. What stopped it, if anything, is told as a script that failed.
bool StartModuleScript(Quanta::Embed::Realm& realm, dom::Document* document, dom::Element* script) {
  Context& ctx = realm.GetContext();
  Value promise;
  if (const dom::Attr* source = script->FindAttribute("", "src")) {
    const std::string address = Resolve(source->value, BaseOf(document));
    if (g_environment && g_environment->SkipScript(address)) return true;
    promise = realm.ImportModule(address, "", "");
  } else {
    promise = realm.EvaluateModule(script->DescendantText(), InlineModuleUrl(document));
  }
  if (qe::HasException(ctx)) {
    const Value exception = ctx.get_exception();
    const qe::ErrorInfo info = qe::InspectError(ctx, exception);
    ctx.clear_exception();
    const bool handled = ReportError(ctx, exception, info);
    if (!handled && g_environment) g_environment->ScriptFailed(info.name + ": " + info.message);
    return handled;
  }
  if (g_environment) g_environment->RunJobs();
  const qe::ObjectInfo state = qe::Inspect(ctx, promise);
  const bool external = script->FindAttribute("", "src") != nullptr;
  if (state.is_object && state.kind == qe::ObjectKind::Promise && state.promise_state == qe::PromiseState::Rejected) {
    if (external) FireEvent(ctx, script, "error");
    const qe::ErrorInfo info = qe::InspectError(ctx, state.promise_result);
    const bool handled = ReportError(ctx, state.promise_result, info);
    if (!handled && g_environment) {
      const std::string where = info.filename.empty() ? "" : " (" + VisibleModuleUrl(info.filename) + (info.line ? ":" + std::to_string(info.line) : "") + ")";
      g_environment->ScriptFailed((info.name.empty() ? "Error" : info.name) + ": " + info.message + where);
    }
    return handled;
  }
  if (external) FireEvent(ctx, script, "load");
  return true;
}

bool RunScript(Quanta::Embed::Realm& realm, dom::Document* document, dom::Element* script) {
  const std::string kind = LowerType(script);
  // An import map is read, not run.
  if (kind == "importmap") {
    std::string error;
    if (!RegisterImportMap(realm, document, script->DescendantText(), error)) {
      if (g_environment) g_environment->ScriptFailed("import map: " + error);
      return false;
    }
    return true;
  }
  if (kind == "module") return StartModuleScript(realm, document, script);
  // A script for browsers without modules is not for this one.
  if (script->FindAttribute("", "nomodule")) return true;
  // Anything else that has a type is data, which the page reads itself.
  const dom::Attr* type = script->FindAttribute("", "type");
  if (type && !type->value.empty() && kind != "text/javascript" && kind != "application/javascript") return true;
  std::string code, filename = document->url;
  if (const dom::Attr* source = script->FindAttribute("", "src")) {
    const std::string address = Resolve(source->value, BaseOf(document));
    if (g_environment && g_environment->SkipScript(address)) return true;
    std::optional<std::string> loaded = g_environment ? g_environment->Load(address) : std::nullopt;
    if (!loaded) {
      if (g_environment) g_environment->ScriptFailed("cannot load script " + source->value);
      FireEvent(realm.GetContext(), script, "error");
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
  bool failed = !result.ok;
  if (failed) {
    // The exception is reported to the window, which a page may be handling; only one nobody handles fails the page.
    qe::ErrorInfo info = qe::InspectError(realm.GetContext(), result.exception);
    if (info.filename.empty()) info.filename = result.filename;
    if (info.line == 0) {
      info.line = result.line;
      info.column = result.column;
    }
    const bool handled = ReportError(realm.GetContext(), result.exception, info);
    if (handled) failed = false;
    else if (g_environment) g_environment->ScriptFailed(result.error);
  }
  // A script with a source tells its element that it is done.
  if (script->FindAttribute("", "src")) FireEvent(realm.GetContext(), script, "load");
  return !failed;
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

void RunAs(Quanta::Embed::Realm& realm, const std::function<void()>& work) { realm.Run(work); }

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

void RunFrameLoad(dom::Element* iframe, uint64_t load);

void StartLoad(dom::Element* iframe) {
  dom::Document* parent = iframe->nodeDocument;
  if (!parent || !parent->context) return;
  ++iframe->frameLoad;
  ++parent->pendingFrameLoads;
  QueueTask(parent, "iframe load", iframe, [iframe, load = iframe->frameLoad] { RunFrameLoad(iframe, load); });
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
  // A module script, and one with a source, wait for a task of their own.
  if (hasSource || LowerType(script) == "module") {
    QueueTask(document, "script", script, [script] {
      if (script->nodeDocument && script->nodeDocument->realm) RunScript(*script->nodeDocument->realm, script->nodeDocument, script);
    });
  } else {
    RunScript(*document->realm, document, script);
  }
}

void AfterInsert(dom::Node* node) {
  if (!node->IsElement() && !node->IsFragment()) return;
  dom::Node* root = dom::ShadowIncludingRoot(node);
  if (!root || !root->IsDocument()) return;
  for (dom::Element* iframe : IframesIn(node)) CreateContext(iframe);
  for (dom::Element* script : HtmlElementsIn(node, "script")) PrepareScript(script);
}

void AfterRemove(dom::Node* node, bool) {
  FocusAfterRemove(node);
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

// The task that loads what the iframe asks for.
void RunFrameLoad(dom::Element* iframe, uint64_t load) {
  if (!iframe || !g_environment) return;
  if (load != iframe->frameLoad || !iframe->contentDocument) {
    // Not the load that counts any more: the count it was in goes down all the same.
    FrameFinished(iframe, false);
    return;
  }
  dom::Document* parent = iframe->nodeDocument;
  std::string markup, blobType;
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
      } else if (resolved.starts_with("blob:")) {
        if (auto blob = web::LookupBlobUrl(resolved)) {
          markup = *blob->data;
          blobType = blob->type;
        }
      } else if (std::optional<std::string> loaded = g_environment->Load(resolved)) {
        markup = std::move(*loaded);
      }
    }
  }
  if (!navigate) {
    // About:blank, which is already there.
    FrameFinished(iframe, true);
    return;
  }
  std::optional<FrameContext> context = MakeContext(iframe, address);
  if (!context) {
    FrameFinished(iframe, true);
    return;
  }
  Adopt(iframe, *context);
  LoadPage(*context->realm, context->document, markup,
           address.starts_with("about:") ? "text/html" : address.starts_with("blob:") ? (blobType.empty() ? "text/html" : blobType) : g_environment->ContentType(address));
}

}  // namespace

void SetFrameEnvironment(FrameEnvironment* environment) { g_environment = environment; }

void RunInRealm(Quanta::Embed::Realm& realm, const std::function<void()>& work) { RunAs(realm, work); }

bool LoadPage(Quanta::Embed::Realm& realm, dom::Document* document, std::string_view markup, std::string_view contentType) {
  bool ok = true;
  std::string type(contentType);
  // The parameters of a Content-Type are not the type.
  type = type.substr(0, type.find(';'));
  while (!type.empty() && type.back() == ' ') type.pop_back();
  const std::string text(markup);
  RunAs(realm, [&] { ok = LoadPageIn(realm, document, text, type); });
  return ok;
}

namespace {

// The end of parsing: the document is interactive, DOMContentLoaded, and load once the frames in it have loaded.
void CompleteLoad(Quanta::Embed::Realm& realm, dom::Document* document, bool notifyFrame) {
  document->readyState = "interactive";
  realm.Evaluate("document.dispatchEvent(new Event('DOMContentLoaded', { bubbles: true }))", "page");
  const auto finish = [&realm, document, notifyFrame] {
    document->readyState = "complete";
    realm.Evaluate("window.dispatchEvent(new Event('load'))", "page");
    // The load of a frame is done when its page's is.
    if (notifyFrame) {
      if (dom::Element* iframe = document->frameElement; iframe && iframe->contentDocument == document) FrameFinished(iframe, true);
    }
  };
  if (document->pendingFrameLoads == 0) finish();
  else document->whenFramesLoaded = finish;
}

bool LoadPageIn(Quanta::Embed::Realm& realm, dom::Document* document, const std::string& markup, const std::string& contentType) {
  Context& ctx = realm.GetContext();
  document->readyState = "loading";
  bool ok = true;
  // Module scripts the parser meets wait until it is done, in order, unless they are async.
  std::vector<dom::Element*> deferred;
  Quanta::Embed::ValueList keep;
  const auto run = [&](dom::Element* script) {
    script->scriptStarted = true;
    if (LowerType(script) == "module" && !script->FindAttribute("", "async")) {
      deferred.push_back(script);
      keep.Append(qe::FromObject(script));
      return;
    }
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
  for (dom::Element* script : deferred) {
    if (!RunScript(realm, document, script)) ok = false;
  }
  CompleteLoad(realm, document, true);
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

// ---- document.open, write and close ----

namespace {

struct ScriptParser {
  std::unique_ptr<TreeBuilder> builder;
};

// "document open": the document is emptied and a parser is made for what is written to it, which has no end until
// close().
void OpenDocument(dom::Document* document) {
  document->scriptParser.reset();
  // The listeners and handlers of the document, of what is in it and of its window are erased.
  for (dom::Node* node = document; node; node = node->NextInTree(document)) {
    node->listeners.clear();
    node->eventHandlers.clear();
    if (dom::Element* element = dom::AsElement(node); element && element->shadowRoot) {
      for (dom::Node* inner = element->shadowRoot; inner; inner = inner->NextInTree(element->shadowRoot)) {
        inner->listeners.clear();
        inner->eventHandlers.clear();
      }
    }
  }
  if (document->window) {
    document->window->listeners.clear();
    document->window->eventHandlers.clear();
  }
  if (document->context) {
    Context& ctx = *document->context;
    for (const char* helper : {"eraseWindowHandlers", "eraseElementHandlers"}) {
      Object* holder = dom::RealmHolder(ctx);
      Value erase = holder ? qe::Get(ctx, qe::FromObject(holder), helper) : qe::Undefined();
      if (qe::IsCallable(erase)) qe::Call(ctx, erase, qe::Undefined());
      if (qe::HasException(ctx)) ctx.clear_exception();
    }
  }
  while (document->firstChild) dom::RemoveUnchecked(document->firstChild);
  document->readyState = "loading";
  auto parser = std::make_shared<ScriptParser>();
  parser->builder = std::make_unique<TreeBuilder>(*document->context, document, "", ScriptingMode::Normal);
  parser->builder->SetStreaming();
  if (document->realm) {
    Quanta::Embed::Realm* realm = document->realm;
    parser->builder->SetScriptHandler([realm, document](dom::Element* script) {
      script->scriptStarted = true;
      RunScript(*realm, document, script);
    });
  }
  parser->builder->Run();
  document->scriptParser = parser;
}

dom::Document* DocumentForMarkup(Context& ctx, const Value& t, const char* member) {
  dom::Document* self = dom::ThisDocument(ctx, t);
  if (!self) return nullptr;
  if (!self->isHtml) {
    dom::Throw(ctx, {"InvalidStateError", std::string("Failed to execute '") + member + "' on 'Document': This method is not supported for XML documents."});
    return nullptr;
  }
  if (self->throwOnDynamicMarkup > 0) {
    dom::Throw(ctx, {"InvalidStateError", std::string("Failed to execute '") + member + "' on 'Document': Custom element constructors and reactions cannot call it."});
    return nullptr;
  }
  return self;
}

Value DocumentOpen(Context& ctx, Value t, qe::Args args, Value) {
  dom::Document* self = DocumentForMarkup(ctx, t, "open");
  if (!self) return qe::Undefined();
  // open(url, name, features) is window.open.
  if (args.size() >= 3) return qe::Null();
  // A script the parser is running does not get to start over.
  if (self->parserInsert) return qe::FromObject(self);
  OpenDocument(self);
  return qe::FromObject(self);
}

Value Write(Context& ctx, Value t, qe::Args args, bool newline, const char* member) {
  dom::Document* self = DocumentForMarkup(ctx, t, member);
  if (!self) return qe::Undefined();
  std::string text;
  for (const Value& arg : args) {
    text += qe::ToWtf8(ctx, arg);
    if (qe::HasException(ctx)) return qe::Undefined();
  }
  if (newline) text += "\n";
  if (self->parserInsert) {
    self->parserInsert(text);
    return qe::Undefined();
  }
  if (!self->scriptParser) OpenDocument(self);
  ScriptParser* parser = static_cast<ScriptParser*>(self->scriptParser.get());
  parser->builder->Append(text);
  parser->builder->Run();
  return qe::Undefined();
}

Value DocumentWrite(Context& ctx, Value t, qe::Args args, Value) { return Write(ctx, t, args, false, "write"); }
Value DocumentWriteln(Context& ctx, Value t, qe::Args args, Value) { return Write(ctx, t, args, true, "writeln"); }

Value DocumentClose(Context& ctx, Value t, qe::Args, Value) {
  dom::Document* self = DocumentForMarkup(ctx, t, "close");
  if (!self || !self->scriptParser) return qe::Undefined();
  std::shared_ptr<void> keep = self->scriptParser;
  ScriptParser* parser = static_cast<ScriptParser*>(keep.get());
  parser->builder->Close();
  parser->builder->Run();
  self->scriptParser.reset();
  if (self->realm) CompleteLoad(*self->realm, self, false);
  return qe::Undefined();
}

}  // namespace

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

// __solarLoadText(url): what the program's loader has at the address, as text, or nothing.
Value LoadText(Context& ctx, Value, qe::Args args, Value) {
  if (args.empty() || !g_environment) return qe::Undefined();
  const std::string address = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  if (address.starts_with("blob:")) {
    if (auto blob = web::LookupBlobUrl(address)) return qe::FromWtf8(ctx, *blob->data);
    return qe::Undefined();
  }
  const std::optional<std::string> text = g_environment->Load(address);
  return text ? qe::FromWtf8(ctx, *text) : qe::Undefined();
}

Value ContentTypeOf(Context& ctx, Value, qe::Args args, Value) {
  if (args.empty() || !g_environment) return qe::FromUtf8(ctx, "text/plain");
  const std::string address = qe::ToWtf8(ctx, args[0]);
  if (qe::HasException(ctx)) return qe::Undefined();
  if (address.starts_with("blob:")) {
    if (auto blob = web::LookupBlobUrl(address)) return qe::FromWtf8(ctx, blob->type);
  }
  return qe::FromWtf8(ctx, g_environment->ContentType(address));
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

void DefineDocumentWriting(Context& ctx) {
  Object* document = dom::InterfacePrototype(ctx, dom::Interface::Document);
  qe::DefineMethod(document, "open", dom::Reactions<DocumentOpen>, 0);
  qe::DefineMethod(document, "write", dom::Reactions<DocumentWrite>, 1);
  qe::DefineMethod(document, "writeln", dom::Reactions<DocumentWriteln>, 1);
  qe::DefineMethod(document, "close", dom::Reactions<DocumentClose>, 0);
}

void DefineFrameNatives(Context& ctx) {
  qe::DefineGlobalFunction(ctx, "__solarParent", ParentWindow, 0);
  qe::DefineGlobalFunction(ctx, "__solarFrameElement", FrameElement, 0);
  qe::DefineGlobalFunction(ctx, "__solarChildCount", ChildCount, 0);
  qe::DefineGlobalFunction(ctx, "__solarChildWindow", ChildWindow, 1);
  qe::DefineGlobalFunction(ctx, "__solarLoadText", LoadText, 1);
  qe::DefineGlobalFunction(ctx, "__solarContentTypeOf", ContentTypeOf, 1);
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
