// The service worker of tests/web-api/sw-scope/register.html and of the navigations the program asks it for.
const bootId = Math.random().toString(36).slice(2, 8);
self.addEventListener("install", (event) => event.waitUntil(self.skipWaiting()));
self.addEventListener("activate", (event) => event.waitUntil(Promise.all([self.clients.claim(), self.registration.navigationPreload.enable()])));
self.addEventListener("fetch", (event) => {
  const url = new URL(event.request.url);
  const virtual = url.pathname.match(/\/virtual-([a-z0-9]+)\.html$/);
  if (virtual) {
    const next = url.searchParams.get("next") || "";
    const wait = Number(url.searchParams.get("wait") || 200);
    event.respondWith(event.preloadResponse.then((preload) => new Response(
      `<!doctype html><meta charset="utf-8"><title>virtual ${virtual[1]}</title><script>
        console.log("navigation ${virtual[1]} boot=${bootId} mode=${event.request.mode} dest=${event.request.destination} preload=${preload ? preload.status : "none"} client=${event.clientId === "" ? "none" : "some"}");
        ${next ? `setTimeout(() => { location.href = ${JSON.stringify(next)}; }, ${wait});` : ""}
      </script>`,
      { headers: { "content-type": "text/html; charset=utf-8" } })));
  } else if (url.pathname.endsWith("/preload-target.html")) {
    event.respondWith(event.preloadResponse.then((r) => new Response(r ? "preload header ok " + r.status : "no preload")));
  }
});
const log = [];
self.addEventListener("sync", (event) => {
  event.waitUntil(caches.open("sync-log").then((c) => c.put("/" + event.tag + "-" + event.lastChance, new Response("fired"))));
});
self.addEventListener("push", (event) => {
  const data = event.data ? event.data.text() : "none";
  event.waitUntil(self.registration.showNotification("Push: " + data, { body: "got " + data, tag: "pushed", data: { n: 1 } }));
});
self.addEventListener("notificationclick", (event) => {
  event.waitUntil(caches.open("sync-log").then((c) => c.put("/clicked-" + event.notification.tag + "-" + event.action, new Response(JSON.stringify(event.notification.data)))));
});
self.addEventListener("message", (event) => {
  if (event.data === "boot") event.source.postMessage({ boot: bootId });
  else if (event.data === "push") self.dispatchEvent(new PushEvent("push", { data: "hello push" }));
  else if (event.data === "click") {
    self.registration.getNotifications({ tag: "pushed" }).then((list) => { const n = list[0]; self.dispatchEvent(new NotificationEvent("notificationclick", { notification: n, action: "open" })); event.source.postMessage({ clicked: !!n }); });
  }
  else if (event.data === "periodic") event.source.postMessage("periodic-registered");
});
self.addEventListener("periodicsync", (event) => log.push(event.tag));
