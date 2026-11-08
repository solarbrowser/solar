/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <functional>
#include <memory>
#include <string>

#include "solar/gui/Wire.h"
#include "solar/net/HttpClient.h"
#include "solar/net/Ipc.h"
#include "solar/url/Url.h"

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

  // The network and the storage of one page's process (a renderer): its end of the network connection, with the origins it may ask for, and the files it stores.
  class Tab {
   public:
    ~Tab();
    // The page may ask for (is on) `origin`: it is let have it as it navigates, and the others are taken back when it commits to one (it can only ask for less: the origin has to be one it
    // was let have). From any thread.
    void AllowOrigin(const std::string& origin);
    void KeepOnlyOrigin(const std::string& origin);
    // A file of what the page stores, asked for (answered on `link`): done for an origin the page was let have, and refused for another (a service worker's registrations of an origin
    // may be read: whether a navigation is for a worker is asked before the page is there).
    void Storage(wire::StorageRequest request);

    struct Impl;

   private:
    friend class BrowserServices;
    std::shared_ptr<Impl> impl_;
  };

  // A tab: its page asks for the network through the other end of `networkFd`; the answers to its storage are sent on `link`, which stays until the tab is gone.
  std::unique_ptr<Tab> OpenTab(int networkFd, net::ipc::Channel& link);

  // The browser's loop (where what the network tells is run), and a fetch of its own: a document of a navigation, with the cookies, the cache and the redirects, which `handler` hears of on
  // that loop. Both from any thread (the fetch returns at once; its handle is for the loop's thread to cancel).
  void Post(std::function<void()> task);
  net::FetchHandle Fetch(const url::Url& address, net::FetchHandler& handler, net::FetchOptions options);

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace solar::gui
