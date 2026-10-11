#!/usr/bin/env python3
"""Compares the layout of a reftest page with that of its reference, without painting: a picture made of the backgrounds and the borders of the boxes (tree order, z-index, overflow clips) must be the same.

usage: tools/layoutref.py DIR_OR_TEST...        every reftest (a page with <link rel="match">) below the paths, PASS/FAIL for each
       tools/layoutref.py --diff TEST           where the pictures differ

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
// The backgrounds and the borders of the boxes painted into a picture of 800 by 600 (in tree order, within stacking contexts by z-index, clipped by overflow).
const W = 800, H = 600;
function parseColor(text) {{
  const m = /^rgba?\\(([^)]*)\\)$/.exec(text);
  if (!m) return null;
  const p = m[1].split(/[ ,\\/]+/).filter(x => x).map(Number);
  return [p[0], p[1], p[2], p.length > 3 ? p[3] : 1];
}}
function paintPicture(doc) {{
  const w = doc.defaultView;
  const pic = new Int32Array(W * H * 3).fill(255);
  const all = [...doc.querySelectorAll('html, body, body *')];
  const entries = [];
  const zOf = e => {{
    for (let n = e; n && n.nodeType === 1; n = n.parentElement) {{
      const cs = w.getComputedStyle(n);
      if (cs.position !== 'static' && cs.zIndex !== 'auto') return parseInt(cs.zIndex) || 0;
    }}
    return 0;
  }};
  all.forEach((e, i) => entries.push([zOf(e), i, e]));
  entries.sort((x, y) => x[0] - y[0] || x[1] - y[1]);
  const clipOf = e => {{
    let x0 = 0, y0 = 0, x1 = W, y1 = H;
    for (let n = e.parentElement; n; n = n.parentElement) {{
      const cs = w.getComputedStyle(n);
      if (cs.overflowX !== 'visible' || cs.overflowY !== 'visible') {{
        const r = n.getBoundingClientRect();
        x0 = Math.max(x0, r.left + n.clientLeft); y0 = Math.max(y0, r.top + n.clientTop);
        x1 = Math.min(x1, r.left + n.clientLeft + n.clientWidth); y1 = Math.min(y1, r.top + n.clientTop + n.clientHeight);
      }}
    }}
    return [x0, y0, x1, y1];
  }};
  const fill = (x0, y0, x1, y1, c, clip, opacity) => {{
    const a = c[3] * opacity;
    if (a <= 0) return;
    x0 = Math.max(Math.round(x0), Math.round(clip[0]), 0); y0 = Math.max(Math.round(y0), Math.round(clip[1]), 0);
    x1 = Math.min(Math.round(x1), Math.round(clip[2]), W); y1 = Math.min(Math.round(y1), Math.round(clip[3]), H);
    for (let y = y0; y < y1; y++) for (let x = x0; x < x1; x++) {{
      const o = (y * W + x) * 3;
      pic[o] = Math.round(c[0] * a + pic[o] * (1 - a)); pic[o + 1] = Math.round(c[1] * a + pic[o + 1] * (1 - a)); pic[o + 2] = Math.round(c[2] * a + pic[o + 2] * (1 - a));
    }}
  }};
  for (const [, , e] of entries) {{
    const cs = w.getComputedStyle(e);
    if (cs.visibility !== 'visible' || cs.display === 'none' || cs.display === 'contents') continue;
    let opacity = parseFloat(cs.opacity);
    for (let n = e.parentElement; n; n = n.parentElement) opacity *= parseFloat(w.getComputedStyle(n).opacity);
    const r = e.getBoundingClientRect();
    if (r.width <= 0 && r.height <= 0) continue;
    const clip = clipOf(e);
    const bg = parseColor(cs.backgroundColor);
    if (bg) fill(r.left, r.top, r.right, r.bottom, bg, clip, opacity);
    const bt = parseFloat(cs.borderTopWidth), br = parseFloat(cs.borderRightWidth), bb = parseFloat(cs.borderBottomWidth), bl = parseFloat(cs.borderLeftWidth);
    const sides = [['Top', bt, r.left, r.top, r.right, r.top + bt], ['Bottom', bb, r.left, r.bottom - bb, r.right, r.bottom], ['Left', bl, r.left, r.top, r.left + bl, r.bottom], ['Right', br, r.right - br, r.top, r.right, r.bottom]];
    for (const [name, width, x0, y0, x1, y1] of sides) {{
      const style = cs['border' + name + 'Style'];
      if (width > 0 && style !== 'none' && style !== 'hidden') {{
        const c = parseColor(cs['border' + name + 'Color']);
        if (c) fill(x0, y0, x1, y1, c, clip, opacity);
      }}
    }}
  }}
  return pic;
}}
const t = async_test("reftest picture");
window.onload = () => setTimeout(() => t.step_func_done(() => {{
  const A = paintPicture(document.getElementById('a').contentDocument), B = paintPicture(document.getElementById('b').contentDocument);
  let different = 0, x0 = W, y0 = H, x1 = -1, y1 = -1, sample = '';
  for (let i = 0; i < W * H; i++) {{
    if (A[i * 3] !== B[i * 3] || A[i * 3 + 1] !== B[i * 3 + 1] || A[i * 3 + 2] !== B[i * 3 + 2]) {{
      different++;
      const x = i % W, y = Math.floor(i / W);
      x0 = Math.min(x0, x); y0 = Math.min(y0, y); x1 = Math.max(x1, x); y1 = Math.max(y1, y);
      if (!sample) sample = 'page ' + A.slice(i * 3, i * 3 + 3).join(',') + ' reference ' + B.slice(i * 3, i * 3 + 3).join(',') + ' at ' + x + ',' + y;
    }}
  }}
  assert_equals(different, 0, 'pixels differ in the box ' + x0 + ',' + y0 + ' - ' + x1 + ',' + y1 + ': ' + sample);
}})(), 300);
</script>'''


def reference_of(test):
    source = open(test, errors="replace").read()
    m = re.search(r'<link[^>]*rel="match"[^>]*href="([^"]+)"', source) or re.search(r'<link[^>]*href="([^"]+)"[^>]*rel="match"', source)
    if not m:
        return None
    ref = os.path.normpath(os.path.join(os.path.dirname(test), m.group(1)))
    if not os.path.exists(ref):
        # (The references the tests share are in tests/wpt/css/reference.)
        shared = os.path.join("tests/wpt/css/reference", os.path.basename(ref))
        if os.path.exists(shared):
            return shared
    return ref if os.path.exists(ref) else None


def run(test):
    ref = reference_of(test)
    if not ref:
        return "NOREF", ""  # (the reference is not here, or the page has none)
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
        print(state)
        for line in text.splitlines():
            if "FAIL" in line:
                print(line.strip())
        return
    tests = []
    for path in sys.argv[1:]:
        if os.path.isfile(path):
            tests.append(path)
            continue
        for d, _, files in os.walk(path):
            for f in files:
                p = os.path.join(d, f)
                if f.endswith((".html", ".htm", ".xht", ".xhtml")) and "-ref" not in f and not f.startswith("layoutref_") and re.search(r'rel="?match"?', open(p, errors="replace").read()):
                    tests.append(p)
    tests.sort()
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        results = list(pool.map(run, tests))
    for test, (state, _) in zip(tests, results):
        print(state, test)
    print("PASS %d of %d" % (sum(1 for r in results if r[0] == "PASS"), len(results)))


main()
