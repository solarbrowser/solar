/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Services.h"
#include "solar/gui/Wire.h"

// The browser's side of a window's pages: which pages' processes (renderers) there are, which one the window shows, the history of the window (the browser keeps it: the pages' processes
// come and go), and where a navigation goes. A renderer is made for one site: a navigation to another site is not its to do, so another renderer is started for it and takes the window when
// it has committed to the document (the old one is ended then); a page that took the window over on its own could take another site's account with it.
namespace solar::gui {

// What starts and ends the renderers (processes): kept apart so that a test can have the page's end be threads.
struct Launcher {
  // Starts a renderer for `url` with its two connections (the window's and the network's: the renderer's ends, which this takes over), and returns what it is called by (-1: it could not).
  std::function<long(int windowFd, int networkFd, const std::string& url)> start;
  // Waits for the renderer to end for a moment, ends it if it has not, and returns its exit code.
  std::function<int(long)> finish;
};

class Session {
 public:
  struct Events {
    // A message of a renderer, from a thread of the session's: the window takes it on its own. `fd` came with it (the window owns it then).
    std::function<void(uint32_t host, uint8_t type, std::string bytes, int fd)> message;
    // A renderer is gone, from a thread of the session's. `fatal`: it was the one the window showed, or the window shows none and there is none to come.
    std::function<void(uint32_t host, bool fatal)> gone;
    // The window shows this renderer now (0: a tab that has no page yet, or none).
    std::function<void(uint32_t host)> shown;
  };

  // `allowFiles`: the pages may be taken to files of this machine (the window was started with one).
  Session(BrowserServices& services, Launcher launcher, Events events, bool allowFiles);
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  // The first page, for the address the window was started with.
  void Open(const std::string& url);
  // The user asks for an address, or a step along the history (the page's own back and forward come to the same).
  void Go(const std::string& url);
  void Back();
  void Forward();
  void Reload();
  // The tabs: a new one (for an address, or the browser's page for a new tab when it is empty) which is shown at once, the one with this number ended (the last one ends the window: `gone` is told
  // with `fatal`), the one with this number shown, and the next or the one before it shown (`delta` 1 or -1).
  void NewTab(const std::string& address);
  void CloseTab(uint32_t tab);
  // The number of the tab that is shown (0: none).
  uint32_t activeTab() const;
  void Activate(uint32_t tab);
  void CycleTab(int delta);
  // Whether the interface draws the window's own buttons (the system's bar is not there), and whether the window is maximized: the interface is told.
  void SetWindowFlags(bool controls, bool maximized);
  // The browser's interface: its renderer is started (`geometry` is the size it is told first, a Resize message's bytes) and told the state of the browser as it changes. It is not a page of
  // the window's history; what it says (Handle) is the window's to act on. 0 while there is none.
  void OpenChrome(const std::string& geometry);
  uint32_t chrome() const;
  void SendChrome(wire::ToRenderer type, const std::string& bytes);
  // The title the shown page has (for the interface).
  void SetTitle(uint32_t host, const std::string& title);
  // The interface asks to be told the state again (it has just started).
  void PushState();

  // The renderer the window shows (0: none yet).
  uint32_t current() const;
  // To one renderer, or to all (the size of the window, which is kept for the ones that come).
  bool Send(uint32_t host, wire::ToRenderer type, const std::string& bytes);
  void Broadcast(wire::ToRenderer type, const std::string& bytes);
  // What a renderer says that is the session's to answer (a navigation, a commit, a step along the history, its storage): from the window's thread. False for the rest.
  bool Handle(uint32_t host, uint8_t type, const std::string& bytes);

  // Ends every renderer; the exit code of the one that was shown last.
  int Close();

  // The history, for looking at (and for tests).
  std::vector<std::string> Entries() const;
  int Index() const;

  struct Impl;

 private:
  std::shared_ptr<Impl> impl_;
};

}  // namespace solar::gui
