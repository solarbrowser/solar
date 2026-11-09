# Roadmap

Where Solar is going, in the order it is meant to be done. The order has one rule: **measure first, fix what breaks most often, make it fast, then add what a person needs every day**.
Security work runs along the way, and the heavy security and platform work waits for the gates below.

Written 9 November 2026, from a browser that is a month old: its own JavaScript engine (Quanta), its own style and layout, its own interface (written in HTML and CSS, drawn by the engine),
and a process model of its own (pages, network, storage, GPU), with the sandbox on Linux.

## Where it is

* Pages load: GitHub in about 5 seconds and YouTube in about 8 (the page, not yet its videos), through the interface, on Linux.
* Done and in use: tabs (top, side, sidebar), pinned tabs, find in the page, zoom, downloads (with the question of where to keep them), settings, light and dark colours that follow the system,
  favicons, WebGL, Web Audio, media through FFmpeg, IndexedDB, service workers, WebSocket, a native sandbox and a GPU's process on Linux.
* Builds and links on macOS (Apple silicon and Intel); not yet run there. Windows builds up to where the process model begins.
* Known and written down: `docs/porting.md` (what each system needs), `docs/wpt.md` (what conforms).

## 1. Measure

Nothing is fixed well that is not measured.

* **A crawler of the popular sites** (about 100, a frozen list so that runs can be compared): each is opened without a window, a picture is taken, and the console's errors, uncaught exceptions,
  failed requests, crashes, time to load and whether the page is blank are written to a log. Errors are grouped by what they say; the list that matters is **what is missing most often** across
  the sites. It builds on `tools/perfpage.py`. It is a tool for priorities and trends, not for exactness: advertisements, consent banners, bot walls and regions make it noisy.
* **WPT stays the guard against regressions** (its own workflow, apart from the build's): a page that fails there is something to fix in the engine, not a broken build.
* **Where the time goes** on GitHub and YouTube: network, script, style, layout, paint.
* A bug to start with: the right button on a tab (pin, close) does not work in the side layouts for the person who tried it.

## 2. Compatibility

* The missing APIs the crawler ranks first, a dozen or two at a time.
* **YouTube plays a video.** This is the first gate (below).
* The engine's known gaps: style sheets that paint SVG shapes, `outline: none` on a text field, and a change a script makes in the handler of a press of a button other than the left, which is not painted.
* The long tail of what a page needs: selection, focus, drag and drop, input methods, fonts.

## 3. Speed

* The DOM and the CSSOM live in the JavaScript engine's heap, and the full gain of that is still to take: no copying between script and page; style and layout reading a committed copy
  on a thread of their own while script writes the pending one (the double slot design in the notes); the collector held back while layout runs.
* Incremental style and layout (what changed, not everything), the engine's start-up, a cache of compiled scripts, and a JIT for the code that is hot.

## 4. What a person needs every day

History and the suggestions of the address bar, bookmarks, restoring the tabs of the last time, menus on the right button (a link, a picture, text), printing and PDF, the pages that ask for
permissions (camera, microphone, location, notifications) and a place to see and take them away, cookies and site data, groups of tabs and the column made wider or narrower, a console for
the person who builds a page.

## 5. Security

The shape is there (processes, a sandbox, the browser checks what a page asks). What is left is the part that needs the engine to be steady first.

* **Fuzzing** (the parsers of HTML, CSS, URLs and script, and everything the processes read from each other) **starts after the second gate**: when videos play on YouTube and most of the modern
  web works. Fuzzing an engine that still falls over on ordinary pages finds what the crawler finds sooner and cheaper.
* A frame of another site in a process of its own (site isolation, stage 2), the GPU's process hardened, a way to update the browser, the pages that tell a person a certificate is wrong.

## 6. Other systems

Costly in money and in time, so after fuzzing. macOS comes first (it builds and links; it has to be run, drawn on the GPU through ANGLE's Metal, and shut in with Seatbelt), then Windows (a
channel between processes, starting a process, a restricted token, DirectX through ANGLE). `docs/porting.md` has the list and `.github/workflows/browser-ports.yml` the builds.

## 7. Depth of the web platform

WebRTC, WebGPU, the system's own decoders for the formats FFmpeg cannot be shipped with, installable pages, worklets, and whatever the crawler shows people use.

## The gates

1. **The first:** a video plays on YouTube. Until then, compatibility and speed come before anything that is not an everyday need.
2. **The second:** most of the modern web works. Fuzzing starts here.
3. **The third:** fuzzing is steady and the sandbox is checked. Other systems start here.

Verified by Ata Türkçü.
