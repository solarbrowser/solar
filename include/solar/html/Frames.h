#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "quanta/Embed.h"
#include "solar/dom/Node.h"

// Pages and their frames: running a document in a realm, and the nested browsing contexts that iframe elements make,
// each of which is a realm and a document of its own.
namespace solar::html {

// What the program around the pages gives them: realms to make frames in, and the text of what a page or frame asks
// for. One is set for the thread, and is there for as long as any page is.
class FrameEnvironment {
 public:
  virtual ~FrameEnvironment() = default;

  // A realm that has every API a window has, but is not yet one: the Frame sets it up. The environment owns it, and
  // keeps it for as long as the page that made it.
  virtual Quanta::Embed::Realm* CreateRealm() = 0;

  // The text of the resource at an absolute URL, or nothing if there is none to be had.
  virtual std::optional<std::string> Load(const std::string& url) = 0;

  // What the resource at the URL is, as the Content-Type header would say: text/html, or an XML type.
  virtual std::string ContentType(const std::string& url) {
    const std::string path = url.substr(0, url.find_first_of("?#"));
    if (path.ends_with(".xhtml") || path.ends_with(".xht")) return "application/xhtml+xml";
    if (path.ends_with(".svg")) return "image/svg+xml";
    if (path.ends_with(".xml")) return "application/xml";
    return "text/html";
  }

  // Whether the script at this URL is one the page is to go without (the test harness, which is already in place).
  virtual bool SkipScript(const std::string&) { return false; }

  // The script at `address` compiled, if the environment keeps compiled scripts (one parse for every realm that runs it).
  // Null: the page compiles it itself.
  virtual std::shared_ptr<Quanta::Embed::Script> CompileScript(const std::string&, const std::string&) { return nullptr; }

  // Runs the jobs script has queued: the microtask checkpoint, after a script has run.
  virtual void RunJobs() = 0;

  // Told of a script that threw, with what it said.
  virtual void ScriptFailed(const std::string&) {}

  // The page of `document` goes to another address (a script set location, or a form was submitted). Only the top document of a window asks;
  // `method` is GET or POST, and a POST has its body and type. The program does it when what asked has returned.
  virtual void Navigate(dom::Document*, const std::string& /*url*/, bool /*replace*/, const std::string& /*method*/, const std::string& /*body*/, const std::string& /*contentType*/) {}
};

void SetFrameEnvironment(FrameEnvironment* environment);
// What the page's script asks for with location and forms: it goes to the environment.
void RequestNavigation(dom::Document* document, const std::string& url, bool replace, const std::string& method = "GET", const std::string& body = "", const std::string& contentType = "");
// How long scripts have run (for looking at where the time of a page goes).
double ScriptMillis();

// Host code that makes or touches the things of a realm has to run as that realm, or the prototypes and holders it
// finds are those of the realm that happens to be running. RunInRealm runs `work` that way.
void RunInRealm(Quanta::Embed::Realm& realm, const std::function<void()>& work);

// What the user does to a page (Interaction.cpp). Each runs script, so it is run in the realm of the document. DispatchPointer fires a
// mouse event at `target` (a press also moves the focus); DispatchClick fires a click and does what the target does for it (toggles,
// submits, follows a label). Both say whether no script stopped the event. TypeInto types `text` into a text control, or edits it
// (kind: "text", "backspace", "delete", "left", "right", "home", "end"); it says whether the control was one that took it.
bool DispatchPointer(dom::Document* document, dom::Element* target, const char* type, double x, double y, int button, int buttons);
bool DispatchClick(dom::Document* document, dom::Element* target, double x, double y, int button);
// A key (type: keydown, keypress, keyup; modifiers: 1 ctrl, 2 shift, 4 alt, 8 meta) goes to `target`; false when a script cancelled it.
bool DispatchKey(dom::Document* document, dom::Element* target, const char* type, const std::string& key, const std::string& code, int keyCode, bool repeat, int modifiers);
// The pointer went from one element to another (either may be null): mouseout/leave and mouseover/enter.
void DispatchHover(dom::Document* document, dom::Element* from, dom::Element* to, double x, double y);
bool TypeInto(dom::Document* document, dom::Element* target, const std::string& text, const char* kind);
void FireChange(dom::Document* document, dom::Element* target);
// The caret (or the selection, when the two differ) of a text control, in UTF-16 units of its value.
void SetCaret(dom::Document* document, dom::Element* target, size_t start, size_t end);

// What a form control shows (its value, whether it is checked, what it says when it is a button or a list), and where the caret and the
// selection of a text control are. False for an element that is not a control.
struct ControlView {
  std::string kind, type, value, placeholder, label;
  bool checked = false, indeterminate = false, disabled = false, readOnly = false;
  size_t start = 0, end = 0;  // in UTF-16 units of the value
};
bool ReadControl(dom::Element* element, ControlView& out);
// Asks the script of the window for the state of the controls that changed since, and keeps it for ReadControl (which gives it as it was committed).
// Called before a frame is made.
void RefreshControls(dom::Document* document);

// Parses `markup` into `document`, a document made in `realm` that has had InstallWindow, runs its scripts, and
// fires DOMContentLoaded and, once the frames in it have loaded, load. Returns whether every script ran.
bool LoadPage(Quanta::Embed::Realm& realm, dom::Document* document, std::string_view markup, std::string_view contentType = "text/html");

// The address that relative ones in a document are resolved against, and the loader's answer for an address (a blob:
// URL's bytes included).
std::string DocumentBaseUrl(const dom::Document* document);
std::optional<std::string> LoadResource(const std::string& url);
// Whether what is being loaded is a style sheet (the page is not to be drawn while it is waited for).
bool LoadingStyleSheet();
// What a data: URL holds.
std::optional<std::string> DecodeDataUrlText(const std::string& address);

// For the cross-origin view of a window: a document's origin, and the windows of its frames, by index or by name.
std::string OriginOfDocument(const dom::Document* document);
std::vector<Quanta::Object*> ChildWindowsOf(dom::Document* document);
Quanta::Object* ChildWindowNamed(dom::Document* document, const std::string& name);

// Makes the tree hooks that give iframe elements their contexts. Called by the HTML bindings.
void InstallFrameHooks();

}  // namespace solar::html
