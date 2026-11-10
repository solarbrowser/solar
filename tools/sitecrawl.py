#!/usr/bin/env python3
"""Opens a list of real sites in Solar, without a window, and writes down what went wrong: what the console said, what failed to load, what crashed or did not end, how long it took and
where the time went, and whether the page was left blank. The errors are grouped by what they say, and the ones that say something is missing (X is not a function, Y is not defined) are
counted over the sites: that list is what the engine lacks most often.

It is a tool for priorities and for seeing a change over time, not for exactness: advertisements, consent banners, bot walls and regions make a run differ from another.

usage: tools/sitecrawl.py run [--sites FILE] [--out DIR] [--only TEXT] [--limit N] [--jobs N] [--timeout S] [--settle MS] [--solar PATH]
       tools/sitecrawl.py report DIR            the summary again, from what a run wrote
       tools/sitecrawl.py compare OLD_DIR NEW_DIR   what changed between two runs

A run writes DIR/results.json, DIR/summary.md, DIR/logs/NAME.log and DIR/shots/NAME.png (build/crawl/<date> if there is no --out).
"""
import argparse
import collections
import concurrent.futures
import datetime
import json
import os
import re
import signal
import subprocess
import sys
import time
import urllib.parse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Lines that the program writes about itself (timings, and what it is doing): they are not what the page said.
DIAGNOSTIC = re.compile(
    r"^(\s*(fetch\s|waited |document |early frame|after load|step changed|version:|frame:|LoadPage|loading |loaded in|window:|vsync:|gpu:|history|\[\d+ ms\]|SDL|solar:|Fontconfig|libpng))"
    r"|^\s*$")
LOADED = re.compile(r"^loaded in (\d+) ms")
PAGE_TIME = re.compile(r"LoadPage took (\d+) ms \((\d+) ms of it waiting; scripts (\d+) ms, style sheets parsed (\d+) ms, layout (\d+) ms\)")
# What says that something went wrong.
ERROR = re.compile(
    r"(Uncaught|\b\w*Error\b|is not a function|is not defined|is not a constructor|Cannot read propert|cannot read propert|not implemented|Unsupported|unsupported|failed|Failed|cannot |Cannot |"
    r"^\s*\[[45]\d\d\]|sandbox:|panic|assert|Segmentation|terminate called|AddressSanitizer|stack overflow|out of memory)")
# What says that the engine lacks something.
MISSING = [
    (re.compile(r"([\w$.\[\]'\"]+) is not a function"), "not a function"),
    (re.compile(r"([\w$.]+) is not a constructor"), "not a constructor"),
    (re.compile(r"([\w$]+) is not defined"), "not defined"),
    (re.compile(r"Cannot read propert(?:y|ies) of (?:undefined|null)(?: \(reading '([^']*)'\))?"), "read of undefined"),
    (re.compile(r"([\w$.]+) (?:is )?not (?:supported|implemented)"), "not implemented"),
]


def name_of(url):
    parts = urllib.parse.urlparse(url)
    text = (parts.netloc + parts.path + ("_" + parts.query if parts.query else "")).strip("/")
    return re.sub(r"[^A-Za-z0-9._-]+", "_", text)[:90]


def read_sites(path):
    sites = []
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        sites.append({"url": fields[0], "kind": fields[1] if len(fields) > 1 else ""})
    return sites


def signature(line):
    """What a line says, with what differs from one site to another taken out, so that the same complaint of two sites is one."""
    text = line.strip()
    text = re.sub(r"https?://\S+", "URL", text)
    text = re.sub(r"\b[0-9a-f]{8,}\b", "HEX", text)
    text = re.sub(r"\d+(\.\d+)?", "N", text)
    text = re.sub(r"\s+", " ", text)
    return text[:140]


def blank_ratio(path):
    """The part of the picture that is of its commonest colour, or None where Pillow is not there."""
    try:
        from PIL import Image
    except ImportError:
        return None
    try:
        image = Image.open(path).convert("RGB")
    except Exception:
        return None
    image.thumbnail((160, 100))
    colours = image.getcolors(maxcolors=image.width * image.height + 1) or []
    if not colours:
        return None
    return max(count for count, _ in colours) / float(image.width * image.height)


def run_one(site, args, out):
    url = site["url"]
    name = name_of(url)
    log_path = os.path.join(out, "logs", name + ".log")
    shot_path = os.path.join(out, "shots", name + ".png")
    env = dict(os.environ)
    env.update({
        "SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy", "SOLAR_GUI_TIMES": "1", "SOLAR_GUI_SHOT": shot_path, "SOLAR_GUI_SETTLE": str(args.settle),
        "SOLAR_SETTINGS_FILE": os.path.join(out, "settings.conf"),
    })
    started = time.time()
    status = "ok"
    with open(log_path, "w", encoding="utf-8", errors="replace") as log:
        # (A group of its own: the browser starts processes, and all of them are to end when the time is up.)
        process = subprocess.Popen([args.solar, url], stdout=log, stderr=subprocess.STDOUT, env=env, cwd=ROOT, start_new_session=True)
        try:
            code = process.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            status = "timeout"
            code = None
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
    elapsed = time.time() - started
    text = open(log_path, encoding="utf-8", errors="replace").read()
    loaded = [int(m.group(1)) for line in text.splitlines() for m in [LOADED.match(line)] if m]
    times = [tuple(int(x) for x in m.groups()) for m in PAGE_TIME.finditer(text)]
    slowest = max(times, key=lambda t: t[0]) if times else None
    console = [line for line in text.splitlines() if not DIAGNOSTIC.match(line)]
    errors = [line for line in console if ERROR.search(line)]
    if code not in (0, None) and status == "ok":
        status = "crash" if code < 0 or code >= 128 or re.search(r"Segmentation|terminate called|AddressSanitizer|sandbox: system call", text) else "exit %d" % code
    if not os.path.exists(shot_path) and status == "ok":
        status = "no picture"
    blank = blank_ratio(shot_path) if os.path.exists(shot_path) else None
    if status == "ok" and blank is not None and blank > 0.985:
        status = "blank"
    record = {
        "url": url, "kind": site["kind"], "name": name, "status": status, "exit": code, "seconds": round(elapsed, 1),
        # (The program says "loaded in" twice for a page that has scripts: when its document is there, and when it has all it asked for. The last is the one that matters.)
        "loaded_ms": loaded[-1] if loaded else None,
        "page_ms": slowest[0] if slowest else None, "waiting_ms": slowest[1] if slowest else None, "scripts_ms": slowest[2] if slowest else None,
        "styles_ms": slowest[3] if slowest else None, "layout_ms": slowest[4] if slowest else None,
        "blank": round(blank, 3) if blank is not None else None,
        "console": len(console), "errors": [line.strip()[:300] for line in errors[:60]],
    }
    return record


def missing_of(line):
    for pattern, kind in MISSING:
        found = pattern.search(line)
        if found:
            return kind, (found.group(1) if found.groups() and found.group(1) else "")
    return None


def summarise(results, out):
    lines = []
    total = len(results)
    by_status = collections.Counter(r["status"] for r in results)
    lines.append("# Crawl of %d sites\n" % total)
    lines.append("Status: " + ", ".join("%s %d" % (k, v) for k, v in sorted(by_status.items(), key=lambda kv: -kv[1])) + "\n")
    loaded = sorted(r["loaded_ms"] for r in results if r["loaded_ms"] is not None)
    if loaded:
        lines.append("Time to load (the sites that did): median %d ms, 90th percentile %d ms, slowest %d ms.\n" % (loaded[len(loaded) // 2], loaded[int(len(loaded) * 0.9)], loaded[-1]))

    # What the errors say, over the sites.
    sites_of = collections.defaultdict(set)
    count_of = collections.Counter()
    example = {}
    missing_sites = collections.defaultdict(set)
    for r in results:
        for line in r["errors"]:
            sig = signature(line)
            sites_of[sig].add(r["url"])
            count_of[sig] += 1
            example.setdefault(sig, line)
            miss = missing_of(line)
            if miss:
                missing_sites[miss].add(r["url"])
    lines.append("\n## What is missing most often (over sites)\n")
    lines.append("| Sites | What | Kind |\n|---:|---|---|")
    for (kind, what), sites in sorted(missing_sites.items(), key=lambda kv: -len(kv[1]))[:40]:
        lines.append("| %d | `%s` | %s |" % (len(sites), what or "(a read of undefined)", kind))
    lines.append("\n## The errors that come most often (over sites)\n")
    lines.append("| Sites | Times | Error |\n|---:|---:|---|")
    for sig, sites in sorted(sites_of.items(), key=lambda kv: (-len(kv[1]), -count_of[kv[0]]))[:40]:
        lines.append("| %d | %d | `%s` |" % (len(sites), count_of[sig], example[sig].strip()[:130].replace("|", "\\|")))

    lines.append("\n## Sites that did not work\n")
    bad = [r for r in results if r["status"] != "ok"]
    if not bad:
        lines.append("None.\n")
    for r in sorted(bad, key=lambda r: r["status"]):
        lines.append("* **%s** %s (%s): `logs/%s.log`" % (r["status"], r["url"], r["kind"], r["name"]))

    lines.append("\n## The slowest (where the time went, in ms)\n")
    lines.append("| Site | Loaded | Page | Waiting | Scripts | Styles | Layout |\n|---|---:|---:|---:|---:|---:|---:|")
    for r in sorted([r for r in results if r["loaded_ms"] is not None], key=lambda r: -r["loaded_ms"])[:20]:
        lines.append("| %s | %s | %s | %s | %s | %s | %s |" % (r["url"], r["loaded_ms"], r["page_ms"], r["waiting_ms"], r["scripts_ms"], r["styles_ms"], r["layout_ms"]))

    lines.append("\n## All\n")
    lines.append("| Site | Kind | Status | Loaded ms | Errors | Blank |\n|---|---|---|---:|---:|---:|")
    for r in results:
        lines.append("| %s | %s | %s | %s | %d | %s |" % (r["url"], r["kind"], r["status"], r["loaded_ms"] if r["loaded_ms"] is not None else "", len(r["errors"]), r["blank"] if r["blank"] is not None else ""))
    text = "\n".join(lines) + "\n"
    with open(os.path.join(out, "summary.md"), "w", encoding="utf-8") as f:
        f.write(text)
    return text


def command_run(args):
    sites = read_sites(args.sites)
    if args.only:
        sites = [s for s in sites if args.only in s["url"] or args.only == s["kind"]]
    if args.limit:
        sites = sites[: args.limit]
    out = args.out or os.path.join(ROOT, "build", "crawl", datetime.datetime.now().strftime("%Y-%m-%d-%H%M"))
    os.makedirs(os.path.join(out, "logs"), exist_ok=True)
    os.makedirs(os.path.join(out, "shots"), exist_ok=True)
    with open(os.path.join(out, "settings.conf"), "w") as f:
        f.write("tabs=top\ntheme=light\n")
    print("%d sites, %d at a time, %s" % (len(sites), args.jobs, out))
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(run_one, s, args, out): s for s in sites}
        for future in concurrent.futures.as_completed(futures):
            r = future.result()
            results.append(r)
            print("%3d/%d %-10s %6s ms  %2d errors  %s" % (len(results), len(sites), r["status"], r["loaded_ms"] if r["loaded_ms"] is not None else "-", len(r["errors"]), r["url"]), flush=True)
    order = {s["url"]: i for i, s in enumerate(sites)}
    results.sort(key=lambda r: order[r["url"]])
    with open(os.path.join(out, "results.json"), "w", encoding="utf-8") as f:
        json.dump(results, f, indent=1)
    summarise(results, out)
    print("\nwritten: %s/summary.md" % out)


def command_report(args):
    results = json.load(open(os.path.join(args.dir, "results.json"), encoding="utf-8"))
    print(summarise(results, args.dir))


def command_compare(args):
    old = {r["url"]: r for r in json.load(open(os.path.join(args.old, "results.json"), encoding="utf-8"))}
    new = {r["url"]: r for r in json.load(open(os.path.join(args.new, "results.json"), encoding="utf-8"))}
    print("# %s -> %s\n" % (args.old, args.new))
    fixed = [u for u in new if u in old and old[u]["status"] != "ok" and new[u]["status"] == "ok"]
    broke = [u for u in new if u in old and old[u]["status"] == "ok" and new[u]["status"] != "ok"]
    print("Now work: %d" % len(fixed))
    for u in fixed:
        print("  + %s (was %s)" % (u, old[u]["status"]))
    print("No longer work: %d" % len(broke))
    for u in broke:
        print("  - %s (now %s)" % (u, new[u]["status"]))
    faster, slower = [], []
    for u in new:
        if u in old and old[u]["loaded_ms"] and new[u]["loaded_ms"]:
            ratio = new[u]["loaded_ms"] / old[u]["loaded_ms"]
            (faster if ratio < 0.8 else slower if ratio > 1.25 else []).append((ratio, u, old[u]["loaded_ms"], new[u]["loaded_ms"]))
    print("Faster by a fifth or more: %d" % len(faster))
    for ratio, u, a, b in sorted(faster)[:15]:
        print("  %s %d -> %d ms" % (u, a, b))
    print("Slower by a quarter or more: %d" % len(slower))
    for ratio, u, a, b in sorted(slower, reverse=True)[:15]:
        print("  %s %d -> %d ms" % (u, a, b))
    old_sigs = {signature(l) for r in old.values() for l in r["errors"]}
    new_sigs = {signature(l) for r in new.values() for l in r["errors"]}
    print("Errors that are new: %d, gone: %d" % (len(new_sigs - old_sigs), len(old_sigs - new_sigs)))
    for sig in sorted(new_sigs - old_sigs)[:15]:
        print("  new: " + sig)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    run = sub.add_parser("run")
    run.add_argument("--sites", default=os.path.join(ROOT, "tools", "crawl", "sites.txt"))
    run.add_argument("--out")
    run.add_argument("--only", help="a part of an address, or a kind (news, dev, probe-video ...)")
    run.add_argument("--limit", type=int)
    run.add_argument("--jobs", type=int, default=3)
    run.add_argument("--timeout", type=int, default=90, help="seconds before a site is given up")
    run.add_argument("--settle", type=int, default=10000, help="ms the program waits before it takes the picture")
    run.add_argument("--solar", default=os.path.join(ROOT, "solar"))
    report = sub.add_parser("report")
    report.add_argument("dir")
    compare = sub.add_parser("compare")
    compare.add_argument("old")
    compare.add_argument("new")
    args = parser.parse_args()
    {"run": command_run, "report": command_report, "compare": command_compare}[args.command](args)


if __name__ == "__main__":
    main()
