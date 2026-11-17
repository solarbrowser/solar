#!/usr/bin/env python3
"""Runs the performance benchmarks of tests/perf (a window-less Solar loads bench.html) and prints what each takes.

usage: tests/perf/run.py [--suite layout|style] [--cards 200,1000] [--feed 300] [--save] [--compare]
  --save     writes the numbers to tests/perf/baseline.json
  --compare  prints the change against baseline.json and marks what is more than 25% slower (and more than 0.05 ms)
Run from the top of the repository, with ./solar built.
"""
import argparse
import http.server
import json
import os
import re
import subprocess
import sys
import threading

ROOT = os.path.dirname(os.path.abspath(__file__))
BASE = os.path.join(ROOT, "baseline.json")  # (numbers are named by the cards and not by the layout: save and compare with the same --layout)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cards", default="200,1000")
    ap.add_argument("--save", action="store_true")
    ap.add_argument("--compare", action="store_true")
    ap.add_argument("--suite", default="layout", help="layout (bench.html: DOM, layout, animations) or style (style.html: a big style sheet and document)")
    ap.add_argument("--feed", default="300", help="the style suite: posts in the feed (the size of the document)")
    ap.add_argument("--layout", default="flex", help="how the cards are laid out: flex, grid, float or block")
    ap.add_argument("--only", default="", help="a regular expression: only the benchmarks whose names match")
    ap.add_argument("--runs", type=int, default=0, help="runs of each (more of them for a profile)")
    ap.add_argument("--timeout", type=int, default=240)
    args = ap.parse_args()

    base_file = BASE if args.suite == "layout" else os.path.join(ROOT, "baseline-style.json")
    handler = lambda *a, **k: http.server.SimpleHTTPRequestHandler(*a, directory=ROOT, **k)
    handler_cls = type("H", (http.server.SimpleHTTPRequestHandler,), {"log_message": lambda *a: None})
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), lambda *a, **k: handler_cls(*a, directory=ROOT, **k))
    port = server.server_address[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()

    env = dict(os.environ, SOLAR_SANDBOX="off", SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SOLAR_GUI_CPU="1", SOLAR_GUI_SETTLE="600000", SOLAR_GUI_SHOT="/tmp/solar_bench.png")
    import urllib.parse
    if args.suite == "style":
        url = "http://127.0.0.1:%d/style.html?feed=%s&only=%s&runs=%d" % (port, args.feed, urllib.parse.quote(args.only), args.runs)
    else:
        url = "http://127.0.0.1:%d/bench.html?cards=%s&layout=%s&only=%s&runs=%d" % (port, args.cards, args.layout, urllib.parse.quote(args.only), args.runs)
    proc = subprocess.Popen(["./solar", url], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
    rows, info = [], []
    try:
        for line in proc.stdout:
            line = line.rstrip()
            m = re.match(r"BENCH (.+) ([\d.]+) (\d+)$", line)
            if m:
                rows.append((m.group(1), float(m.group(2)), int(m.group(3))))
            elif line.startswith("INFO "):
                info.append(line[5:])
            elif line.startswith("BENCHDONE"):
                break
    finally:
        proc.kill()
        server.shutdown()
    if not rows:
        print("no results: is ./solar built, and does the page load?")
        return 1
    base = json.load(open(base_file)) if args.compare and os.path.exists(base_file) else {}
    print("\n".join(info))
    width = max(len(r[0]) for r in rows)
    slower = 0
    for name, ms, ops in rows:
        mark = ""
        if name in base:
            was = base[name]
            if ms > was * 1.25 and ms - was > 0.05:
                mark = "  SLOWER (was %.4f)" % was
                slower += 1
            elif ms < was * 0.8 and was - ms > 0.05:
                mark = "  faster (was %.4f)" % was
        print("%-*s  %10.4f ms%s" % (width, name, ms, mark))
    if args.save:
        json.dump({n: m for n, m, _ in rows}, open(base_file, "w"), indent=1, sort_keys=True)
        print("saved %d numbers to %s" % (len(rows), base_file))
    if args.compare and slower:
        print("%d slower than the baseline" % slower)
    return 0


if __name__ == "__main__":
    sys.exit(main())
