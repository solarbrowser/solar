#!/usr/bin/env python3
"""Runs WPT pages in parallel and writes one line per page: path, exit code, passed/total, the first failure.

usage: tools/wptrun.py OUTPUT DIR...   (runs every .html page below tests/wpt/DIR that is not a support file)
"""
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

SKIP = re.compile(r"/resources/|/support/|-support|-ref\.html|-frame|-iframe|\.part\.html|-subframe|/tmp/")


def run(path):
    try:
        out = subprocess.run(["bash", "-c", f"ulimit -v 4000000; timeout 60 ./build/WptTest {path}"], capture_output=True, text=True, timeout=90)
        text = out.stdout + out.stderr
        code = out.returncode
    except subprocess.TimeoutExpired:
        return f"{path} :: rc=124 NO SUMMARY"
    summary = re.findall(r"^(\d+)/(\d+) passed", text, re.M)
    first = next((l.strip() for l in text.splitlines() if l.strip().startswith("FAIL")), "")
    s = f"{summary[-1][0]}/{summary[-1][1]} passed" if summary else "NO SUMMARY"
    return f"{path} :: rc={code} {s}" + (f" | {first[:200]}" if first else "")


def main():
    output, dirs = sys.argv[1], sys.argv[2:]
    pages = []
    for d in dirs:
        for root, _, files in os.walk(f"tests/wpt/{d}"):
            for f in files:
                p = os.path.join(root, f)
                if f.endswith((".html", ".xhtml", ".svg")) and not SKIP.search(p):
                    pages.append(p)
    pages.sort()
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        lines = list(pool.map(run, pages))
    with open(output, "w") as out:
        out.write("\n".join(lines) + "\n")
    ok = sum(1 for l in lines if re.search(r"rc=0 (\d+)/\1 passed", l) and not re.search(r"rc=0 0/0", l))
    print(f"{len(pages)} pages, {ok} pass whole; written to {output}")


main()
