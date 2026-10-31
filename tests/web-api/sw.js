// The service worker of tests/web-api/serviceworker.html.
const VERSION = "v1";
const log = [];
self.addEventListener("install", (event) => {
  log.push("install");
  event.waitUntil(caches.open("sw-" + VERSION).then((c) => c.put("/precached", new Response("from the cache"))));
});
self.addEventListener("activate", (event) => {
  log.push("activate");
  event.waitUntil(self.clients.claim());
});
self.addEventListener("fetch", (event) => {
  const url = new URL(event.request.url);
  if (url.pathname === "/virtual") {
    event.respondWith(new Response("made by the worker " + event.request.method, { status: 201, statusText: "Made", headers: { "x-sw": "yes", "content-type": "text/plain" } }));
  } else if (url.pathname === "/precached") {
    event.respondWith(caches.match(event.request));
  } else if (url.pathname === "/echo-body") {
    event.respondWith(event.request.text().then((t) => new Response("got " + t)));
  } else if (url.pathname === "/slow") {
    event.respondWith(new Promise((resolve) => setTimeout(() => resolve(new Response("slow")), 50)));
  } else if (url.pathname === "/fail") {
    event.respondWith(Promise.reject(new Error("no")));
  } else if (url.pathname === "/throws-late") {
    setTimeout(() => { try { event.respondWith(new Response("late")); } catch (e) { self.lateError = e.name; } }, 0);
  } else if (url.pathname === "/via-network") {
    event.respondWith(fetch("/serviceworker.html", { cache: "no-store" }).then((r) => new Response("network said " + r.status)));
  }
  // anything else: not handled, so the page's fetch goes to the network
});
self.addEventListener("message", (event) => {
  if (event.data === "ping") event.source.postMessage({ pong: true, from: self.registration.scope, log: log.slice(), scope: self instanceof ServiceWorkerGlobalScope, win: typeof window, doc: typeof document });
  else if (event.data === "clients") self.clients.matchAll().then((list) => event.source.postMessage({ clients: list.length, type: list[0] && list[0].type, url: list[0] && list[0].url }));
  else if (event.data === "skip") self.skipWaiting().then(() => event.source.postMessage("skipped"));
  else if (event.data === "late") event.source.postMessage({ late: self.lateError });
  else if (event.data === "version") event.source.postMessage({ version: VERSION });
});
