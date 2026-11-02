/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/web-api/WebApis.h"

#include <cstdlib>
#include <sstream>

#include "solar/web/Scripts.h"

namespace solar::webapi {

namespace {

// SOLAR_DISABLE=worker,serviceworker,indexeddb,...: web APIs that are made not to be there, to find which one a site is stopped by (a site that tests for an API and
// finds a half of one goes the wrong way, which one that does not find it does not). What is named is taken away from the window once the APIs are made.
std::string DisableEpilogue() {
  const char* named = std::getenv("SOLAR_DISABLE");
  if (!named || !*named) return "";
  std::string script = "\n;(() => {\n  const gone = (...names) => { for (const n of names) { try { delete globalThis[n]; } catch (e) {} } };\n";
  std::stringstream list(named);
  std::string item;
  while (std::getline(list, item, ',')) {
    if (item == "worker") script += "  gone('Worker', 'SharedWorker');\n";
    else if (item == "serviceworker") script += "  try { delete Navigator.prototype.serviceWorker; } catch (e) {} gone('ServiceWorker', 'ServiceWorkerContainer', 'ServiceWorkerRegistration');\n";
    else if (item == "indexeddb") script += "  gone('indexedDB', 'IDBFactory', 'IDBDatabase', 'IDBTransaction', 'IDBObjectStore', 'IDBRequest', 'IDBOpenDBRequest', 'IDBKeyRange', 'IDBCursor', 'IDBCursorWithValue', 'IDBIndex', 'IDBVersionChangeEvent');\n";
    else if (item == "cache") script += "  gone('caches', 'Cache', 'CacheStorage');\n";
    else if (item == "websocket") script += "  gone('WebSocket');\n";
    else if (item == "eventsource") script += "  gone('EventSource');\n";
    else if (item == "mediasource") script += "  gone('MediaSource', 'SourceBuffer', 'SourceBufferList', 'ManagedMediaSource');\n";
    else if (item == "webrtc") script += "  gone('RTCPeerConnection', 'webkitRTCPeerConnection', 'RTCSessionDescription', 'RTCIceCandidate', 'RTCDataChannel');\n";
    else if (item == "webgpu") script += "  try { delete Navigator.prototype.gpu; } catch (e) {} gone('GPU');\n";
    else if (item == "crypto") script += "  gone('crypto', 'Crypto', 'CryptoKey', 'SubtleCrypto');\n";
    else if (item == "locks") script += "  try { delete Navigator.prototype.locks; } catch (e) {} gone('LockManager');\n";
    else if (item == "storage") script += "  gone('StorageManager');\n  try { delete Navigator.prototype.storage; } catch (e) {}\n";
    else if (item == "permissions") script += "  try { delete Navigator.prototype.permissions; } catch (e) {} gone('Permissions', 'PermissionStatus');\n";
    else if (item == "push") script += "  gone('PushManager', 'PushSubscription', 'Notification');\n";
  }
  return script + "})();\n";
}

// SOLAR_TRACE=postmessage: each message a window sends and receives is written to the console, with whose it is (to see what two frames say to each other).
std::string TraceEpilogue() {
  const char* named = std::getenv("SOLAR_TRACE");
  if (!named) return "";
  std::string script;
  if (std::string(named).find("errors") != std::string::npos) {
    script += R"JS(
;(() => {
  addEventListener("error", (e) => { console.log("[error] " + e.message + " @ " + e.filename + ":" + e.lineno + ":" + e.colno + (e.error && e.error.stack ? "\n" + String(e.error.stack).split("\n").slice(0, 6).join("\n") : "")); }, true);
  addEventListener("unhandledrejection", (e) => { const r = e.reason; console.log("[rejection] " + (r && r.stack ? String(r.stack).split("\n").slice(0, 6).join("\n") : String(r))); }, true);
})();
)JS";
  }
  if (std::string(named).find("postmessage") == std::string::npos) return script;
  return script + R"JS(
;(() => {
  const who = () => { try { return (window.name || (window.top === window ? "top" : "frame")) + "@" + location.pathname.slice(0, 40); } catch (e) { return "?"; } };
  const show = (v) => { try { return typeof v === "string" ? v.slice(0, 160) : JSON.stringify(v).slice(0, 160); } catch (e) { return "[" + typeof v + "]"; } };
  const native = Window.prototype.postMessage || globalThis.postMessage;
  if (typeof native === "function") {
    const traced = { postMessage(message, ...rest) { console.log("[msg] " + who() + " sends to " + (this === window ? "self" : (this.name || "?")) + ": " + show(message)); return native.call(this, message, ...rest); } }.postMessage;
    try { Object.defineProperty(Window.prototype, "postMessage", { value: traced, writable: true, configurable: true, enumerable: true }); } catch (e) {}
  }
  addEventListener("message", (e) => { console.log("[msg] " + who() + " gets from " + e.origin + ": " + show(e.data) + (e.ports.length ? " (+" + e.ports.length + " ports)" : "")); }, true);
  if (typeof MessagePort === "function") {
    const portNative = MessagePort.prototype.postMessage;
    const tracedPort = { postMessage(message, ...rest) { console.log("[port] " + who() + " posts: " + show(message)); return portNative.call(this, message, ...rest); } }.postMessage;
    try { Object.defineProperty(MessagePort.prototype, "postMessage", { value: tracedPort, writable: true, configurable: true, enumerable: true }); } catch (e) {}
  }
})();
)JS";
}

}  // namespace

std::string Script() {
  std::string all;
  for (const char* const* piece = web::kScriptWebApi; *piece; ++piece) all += *piece;
  return all + DisableEpilogue() + TraceEpilogue();
}

std::string WorkerScopeScript() {
  std::string all;
  for (const char* const* piece = web::kScriptWebApiWorker; *piece; ++piece) all += *piece;
  return all;
}

}  // namespace solar::webapi
