#!/usr/bin/env python3
"""Makes an HTML report of a full WPT run: the directories as a tree with the subtests that pass, and the pages of each with the first failure.

usage: tools/wptreport.py [RESULTS [OUTPUT]]   (defaults: build/wpt-all.txt build/wpt-report.html; RESULTS is what tools/wptrun.py OUTPUT DIR... wrote)
"""
import json
import re
import sys

results = sys.argv[1] if len(sys.argv) > 1 else "build/wpt-all.txt"
output = sys.argv[2] if len(sys.argv) > 2 else "build/wpt-report.html"
pages = []
for line in open(results):
    m = re.match(r"(\S+) :: rc=(\d+) (.*)", line.rstrip("\n"))
    if not m:
        continue
    path, rc, rest = m.groups()
    s = re.match(r"(\d+)/(\d+) passed(?: \| (.*))?", rest)
    if s:
        passed, total, note = int(s[1]), int(s[2]), (s[3] or "")
    else:
        passed, total, note = 0, 0, "no summary" + (" (" + rest.split("|")[0].strip() + ")" if "|" in rest else "")
    note = re.sub(r"^FAIL\s*", "", note)[:160]
    pages.append([path.removeprefix("tests/wpt/"), int(rc), passed, total, note])

# [name, pages, whole, passed, total, children, files]; files are [name, rc, passed, total, note]
def node(name):
    return [name, 0, 0, 0, 0, {}, []]
root = node("wpt")
for path, rc, passed, total, note in pages:
    parts = path.split("/")
    chain = [root]
    for d in parts[:-1]:
        chain.append(chain[-1][5].setdefault(d, node(d)))
    whole = 1 if rc == 0 and total > 0 and passed == total else 0
    for n in chain:
        n[1] += 1; n[2] += whole; n[3] += passed; n[4] += total
    chain[-1][6].append([parts[-1], rc, passed, total, note])

def pack(n):
    return [n[0], n[1], n[2], n[3], n[4], [pack(c) for c in sorted(n[5].values(), key=lambda c: c[0])], sorted(n[6])]

html = open("tools/wptreport.template.html").read().replace("/*DATA*/null", json.dumps(pack(root), separators=(",", ":")).replace("<", "\\u003c").replace("\u2028", "\\u2028").replace("\u2029", "\\u2029"))
open(output, "w").write(html)
print(f"{len(pages)} pages -> {output}")
