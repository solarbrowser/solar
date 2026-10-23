#!/usr/bin/env python3
"""Makes an offline page for timing the style and layout of a real site: the page and its style sheets are fetched once, the style sheets are put in
the page, the scripts taken out, and a script that prints how long the first style, the first layout and a layout after a small change take.

usage: tools/perfpage.py fetch URL NAME   fetches URL into build/perf/NAME.html (needs the network)
       tools/perfpage.py run NAME...      times the pages (build/WptTest must be built)
"""
import html
import os
import re
import subprocess
import sys
import urllib.parse

SOLAR = "./solar"
DIR = "build/perf"
# The page has to be inside tests/wpt for the test runner to find /resources.
PAGE_DIR = "tests/wpt/css"

TIMER = """<script src="/resources/testharness.js"></script><script src="/resources/testharnessreport.js"></script>
<script>
test(() => {
  const t0 = performance.now();
  getComputedStyle(document.body).color;
  const t1 = performance.now();
  document.body.offsetHeight;
  const t2 = performance.now();
  document.body.style.zoom = 1;
  document.body.offsetHeight;
  const t3 = performance.now();
  const n = document.querySelectorAll("*").length;
  const h = document.documentElement.scrollHeight;
  assert_equals(JSON.stringify({elements: n, scrollHeight: h, style_ms: Math.round(t1 - t0), layout_ms: Math.round(t2 - t1), relayout_ms: Math.round(t3 - t2)}), "x");
});
</script>"""


def fetch(url):
    out = subprocess.run([SOLAR, "fetch", url], capture_output=True, text=True, errors="replace", timeout=120).stdout
    cut = out.find("\n\n")
    return out[cut + 2:] if cut >= 0 else out


def make(url, name):
    raw = fetch(url)
    start = raw.lower().find("<!doctype")
    start = start if start >= 0 else raw.lower().find("<html")
    page = raw[start:]

    def inline(match):
        href = re.search(r'href="([^"]+)"', match.group(0))
        if not href:
            return ""
        css = fetch(urllib.parse.urljoin(url, html.unescape(href.group(1))))
        return "<style>" + css + "</style>"

    page = re.sub(r'<link[^>]*rel="stylesheet"[^>]*>', inline, page)
    page = re.sub(r"<script\b.*?</script>", "", page, flags=re.S)
    page = page.replace("</body>", TIMER + "</body>", 1) if "</body>" in page else page + TIMER
    os.makedirs(DIR, exist_ok=True)
    path = os.path.join(DIR, name + ".html")
    open(path, "w").write(page)
    print(path, len(page), "bytes")


def run(names):
    for name in names:
        src = os.path.join(DIR, name + ".html")
        tmp = os.path.join(PAGE_DIR, "zz-perf-%s.html" % name)
        open(tmp, "w").write(open(src).read())
        try:
            out = subprocess.run(["./build/WptTest", tmp], capture_output=True, text=True, errors="replace", timeout=600)
            line = next((l for l in (out.stdout + out.stderr).splitlines() if "got" in l), "no result")
            print(name, line.strip()[:300])
        finally:
            os.remove(tmp)


if __name__ == "__main__":
    if sys.argv[1] == "fetch":
        make(sys.argv[2], sys.argv[3])
    else:
        run(sys.argv[2:])
