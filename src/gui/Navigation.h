/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "solar/gui/Wire.h"
#include "solar/net/HttpClient.h"
#include "solar/net/Ipc.h"
#include "solar/net/Loop.h"

namespace solar::gui {

// The page's way to a document: it asks the browser, which fetches it (the cookies, the redirects, the cache and files are the browser's) and says what comes, which the handler is told of
// from the loop of the page. The same as a fetch of the network client for what a navigation is told of: the status, the type of the document, where it came from, the body, the end.
class RemoteNavigation {
 public:
  RemoteNavigation(net::Loop& pageLoop, net::ipc::Channel& link) : loop_(pageLoop), link_(link) {}

  // Asks for the document at `url` (the method and the body of `options` go with it; `kind` and `entry` say what it is for the history). `handler` is to be there until it ends. Called on the page's
  // thread. The number the browser knows the navigation by.
  uint32_t Fetch(const url::Url& url, net::FetchHandler& handler, const net::FetchOptions& options, wire::Navigate::Kind kind = wire::Navigate::Kind::New, int entry = -1);
  // The page is on `address` now, having come there by the navigation with that number (0: one the page did itself).
  void Committed(const std::string& address, uint32_t navigation);
  // A step back or forward along the history of the window, which the browser keeps.
  void HistoryGo(int delta);
  // The browser asks the page to go to an address (as a step along the history for an entry, or as a new navigation for -1), on the page's thread.
  void SetGoTo(std::function<void(std::string, int)> goTo) { goTo_ = std::move(goTo); }
  // What a navigation that went to another process ends with (the page stays where it is).
  static constexpr const char* kTransferred = "navigation moved to another process";
  // What the browser said (from any thread: it is put on the loop of the page).
  void Deliver(wire::ToRenderer type, std::string bytes);

 private:
  void Handle(wire::ToRenderer type, const std::string& bytes);

  net::Loop& loop_;
  net::ipc::Channel& link_;
  std::function<void(std::string, int)> goTo_;
  uint32_t next_ = 1;
  std::map<uint32_t, net::FetchHandler*> pending_;  // (the page's thread)
};

}  // namespace solar::gui
