/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <memory>
#include <string>

#include "solar/gui/Wire.h"
#include "solar/net/Ipc.h"

// What the browser process runs for the pages (the renderers): the network (cookies, cache, connections, the rules of fetching) on threads of its own, and for each tab its end of the
// network connection and the navigations it takes the page's word for (the page says where it wants to go; the browser fetches the document, and lets the page ask for the origin of what
// comes).
namespace solar::gui {

// The path of a file: address (the part after file://, without a query or a fragment, percent-decoded).
std::string FileOfAddress(const std::string& address);

class BrowserServices {
 public:
  BrowserServices();
  ~BrowserServices();
  BrowserServices(const BrowserServices&) = delete;
  BrowserServices& operator=(const BrowserServices&) = delete;

  class Tab {
   public:
    ~Tab();
    // From any thread: what the page asks of the browser (navigations that are not for it to have are answered with an error).
    void Navigate(wire::Navigate request);
    // The page says it is now on `address`: where that is an origin it was let have, the others are taken back.
    void Committed(const std::string& address);

    struct Impl;

   private:
    friend class BrowserServices;
    std::shared_ptr<Impl> impl_;
  };

  // A tab: its page asks for the network through the other end of `networkFd`; the answers to its navigations are sent on `link`, which stays until the tab is gone. `allowFiles`: the page
  // may be taken to the files of this machine (it was made for one: a page from the network is not).
  std::unique_ptr<Tab> OpenTab(int networkFd, net::ipc::Channel& link, bool allowFiles);

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace solar::gui
