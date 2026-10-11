#!/usr/bin/env python3
"""Compares the layout of a reftest page with that of its reference, without painting: the boxes with a background colour (where they are, how big, what colour) must be the same.

usage: tools/layoutref.py DIR_OR_TEST...        every reftest (a page with <link rel="match">) below the paths, PASS/FAIL for each
       tools/layoutref.py --diff TEST           the boxes only the reference has and the boxes only the page has

It is a stand-in for the pixel comparison until there is paint: text and borders are not compared.
"""
import hashlib
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

PAGE = '''<!DOCTYPE html>
<script src="/resources/testharness.js"></script><script src="/resources/testharnessreport.js"></script>
<iframe id=a src="{test}" style="width:800px;height:600px"></iframe><iframe id=b src="{ref}" style="width:800px;height:600px"></iframe>
<script>
function boxes(doc) {{
  const out = [];
  const w = doc.defaultView;
  for (const e of doc.querySelectorAll('body *')) {{
    const bg = w.getComputedStyle(e).backgroundColor;
    if (bg === 'rgba(0, 0, 0, 0)' || bg === 'transparent') continue;
    const r = e.getBoundingClientRect();
    if (r.width <= 0 || r.height <= 0) continue;
    out.push([Math.round(r.left * 10) / 10, Math.round(r.top * 10) / 10, Math.round(r.width * 10) / 10, Math.round(r.height * 10) / 10, bg].join(' '));
  }}
  return out.sort();
}}
const t = async_test("reftest geometry");
window.onload = () => setTimeout(() => t.step_func_done(() => {{
  assert_equals(boxes(document.getElementById('a').contentDocument).join("\\n"), boxes(document.getElementById('b').contentDocument).join("\\n"));
}})(), 300);
</script>'''


def reference_of(test):
    source = open(test, errors="replace").read()
    m = re.search(r'<link[^>]*rel="match"[^>]*href="([^"]+)"', source) or re.search(r'<link[^>]*href="([^"]+)"[^>]*rel="match"', source)
    return os.path.normpath(os.path.join(os.path.dirname(test), m.group(1))) if m else None


def run(test):
    ref = reference_of(test)
    if not ref:
        return "NOREF", ""
    name = "tests/wpt/css/layoutref_%s.html" % hashlib.md5(test.encode()).hexdigest()[:10]
    rel = lambda p: "/" + os.path.relpath(p, "tests/wpt")
    open(name, "w").write(PAGE.format(test=rel(test), ref=rel(ref)))
    try:
        out = subprocess.run(["bash", "-c", "ulimit -v 4000000; timeout 60 ./build/WptTest " + name], capture_output=True, text=True, errors="replace", timeout=90)
        text = out.stdout + out.stderr
    except subprocess.TimeoutExpired:
        text = "TIMEOUT"
    finally:
        os.remove(name)
    return ("PASS" if re.search(r"^1/1 passed", text, re.M) else "FAIL"), text


def main():
    if sys.argv[1] == "--diff":
        state, text = run(sys.argv[2])
        m = re.search(r'expected "(.*?)" but got "(.*?)"\n', text, re.S)
        if not m:
            print(state)
            return
        split = lambda s: s.replace("\\n", "\n").split("\n")
        ref, page = split(m[1]), split(m[2])
        print("only in the reference:")
        for x in ref:
            if x not in page:
                print("  ", x)
        print("only in the page:")
        for x in page:
            if x not in ref:
                print("  ", x)
        return
    tests = []
    for path in sys.argv[1:]:
        if os.path.isfile(path):
            tests.append(path)
            continue
        for d, _, files in os.walk(path):
            for f in files:
                p = os.path.join(d, f)
                if f.endswith(".html") and "-ref" not in f and not f.startswith("layoutref_") and re.search(r'rel="?match"?', open(p, errors="replace").read()):
                    tests.append(p)
    tests.sort()
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        results = list(pool.map(run, tests))
    for test, (state, _) in zip(tests, results):
        print(state, test)
    print("PASS %d of %d" % (sum(1 for r in results if r[0] == "PASS"), len(results)))


main()
