#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

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

  // Runs the jobs script has queued: the microtask checkpoint, after a script has run.
  virtual void RunJobs() = 0;

  // Told of a script that threw, with what it said.
  virtual void ScriptFailed(const std::string&) {}
};

void SetFrameEnvironment(FrameEnvironment* environment);

// Host code that makes or touches the things of a realm has to run as that realm, or the prototypes and holders it
// finds are those of the realm that happens to be running. RunInRealm runs `work` that way; a new realm needs
// PrepareRealm first, which gives it what that takes.
void PrepareRealm(Quanta::Embed::Realm& realm);
void RunInRealm(Quanta::Embed::Realm& realm, const std::function<void()>& work);

// Parses `markup` into `document`, a document made in `realm` that has had InstallWindow, runs its scripts, and
// fires DOMContentLoaded and, once the frames in it have loaded, load. Returns whether every script ran.
bool LoadPage(Quanta::Embed::Realm& realm, dom::Document* document, std::string_view markup, std::string_view contentType = "text/html");

// The address that relative ones in a document are resolved against, and the loader's answer for an address (a blob:
// URL's bytes included).
std::string DocumentBaseUrl(const dom::Document* document);
std::optional<std::string> LoadResource(const std::string& url);

// Makes the tree hooks that give iframe elements their contexts. Called by the HTML bindings.
void InstallFrameHooks();

}  // namespace solar::html
