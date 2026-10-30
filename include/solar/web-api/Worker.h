/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <optional>
#include <string>

#include "quanta/Embed.h"

// Dedicated workers (https://html.spec.whatwg.org/multipage/workers.html). A worker is a realm of its own in the page's Isolate, with the global
// scope a worker has (no window, no document), that runs on the thread of the page: its tasks are taken in turn with the page's, and a long one
// holds the page up, as it would not elsewhere. What it says and is told goes over a MessageChannel, so the messages are as they are for any port:
// cloned, in order, and in tasks.
namespace solar::webapi {

// What the program gives the workers to be made of.
class WorkerHost {
 public:
  virtual ~WorkerHost() = default;
  // A realm for the global scope of a worker whose script is at `scriptUrl`, with the APIs a worker has installed (the ones a window shares:
  // events, URLs, fetch, messaging, console). The program keeps it until ReleaseWorkerRealm.
  virtual Quanta::Embed::Realm* CreateWorkerRealm(const std::string& scriptUrl) = 0;
  virtual void ReleaseWorkerRealm(Quanta::Embed::Realm* realm) = 0;
  // The text of the script at an address (importScripts waits for it), or nothing if it cannot be had.
  virtual std::optional<std::string> LoadScript(const std::string& url) = 0;
};

// The program's host; the one that is not set makes `new Worker` fail with an error event.
void SetWorkerHost(WorkerHost* host);

// Defines in a window's realm the natives that its Worker class is made on (the class itself is in script: src/web-api/js/worker.js).
void InstallWorkerApis(Quanta::Embed::Realm& realm);

// What an exception that no script of a worker caught comes to: an error event at its Worker (and what the page does of that), told by the program
// where it reports exceptions. False where the realm is not a worker's.
bool ReportWorkerError(Quanta::Context& ctx, const Quanta::Value& exception, const Quanta::Embed::ErrorInfo& info);

// What a worker realm is given besides what the host installs: its natives, and the script that makes its global scope. The program calls this
// in the realm made by CreateWorkerRealm, last.
void InstallWorkerScope(Quanta::Embed::Realm& realm);

}  // namespace solar::webapi
