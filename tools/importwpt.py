#!/usr/bin/env python3
"""Copies the testharness pages (and the scripts and styles they use) of WPT directories into tests/wpt.

usage: tools/importwpt.py WPT_CHECKOUT DIR...      (DIR relative to the checkout, such as css/css-variables)
"""
import os
import shutil
import sys

root = sys.argv[1]
for rel in sys.argv[2:]:
    source = os.path.join(root, rel)
    target = os.path.join("tests/wpt", rel)
    copied = 0
    for directory, _, files in os.walk(source):
        sub = os.path.relpath(directory, source)
        for name in files:
            path = os.path.join(directory, name)
            keep = False
            in_support = any(part in ("support", "resources") for part in sub.split(os.sep))
            if in_support and name.endswith((".js", ".css", ".html", ".htm", ".xhtml", ".xht", ".xml", ".svg")):
                keep = True
            elif name.endswith((".html", ".htm", ".xhtml", ".svg")):
                text = open(path, errors="ignore").read()
                keep = "testharness.js" in text
            elif name.endswith((".js", ".css", ".html", ".htm", ".xhtml", ".xht", ".xml", ".svg")) and any(part in ("support", "resources") for part in sub.split(os.sep)):
                keep = True
            elif name.endswith(".any.js") or name.endswith(".window.js"):
                keep = True
            if keep:
                os.makedirs(os.path.join(target, sub), exist_ok=True)
                shutil.copy(path, os.path.join(target, sub, name))
                copied += 1
    print(rel, copied)
