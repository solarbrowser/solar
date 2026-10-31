#!/bin/sh
# The service worker tests that need more than one page: registers a worker (and tries its features), then follows navigations the worker answers, then goes out of its
# scope for longer than it is kept and comes back. Prints the console lines.   Usage: tests/web-api/run-sw.sh   (from the repository root, after make)
set -e
PORT=8765
DIR=$(mktemp -d)
export SOLAR_IDB_DIR="$DIR/idb"
export SOLAR_SW_IDLE_MS=600
export SDL_VIDEO_DRIVER=dummy SOLAR_GUI_CPU=1
python3 tests/web-api/wsserver.py $PORT >/dev/null 2>&1 &
SERVER=$!
trap 'kill $SERVER 2>/dev/null; rm -rf "$DIR"' EXIT
sleep 1
# (The permission for notifications is granted by writing what the program keeps: one file for the origin, named by a hash of the key.)
python3 - "$SOLAR_IDB_DIR" "http://127.0.0.1:$PORT" <<'PY'
import os, sys
root, origin = sys.argv[1] + "-kv", sys.argv[2]
def fnv(text):
    h = 1469598103934665603
    for b in text.encode():
        h ^= b; h = (h * 1099511628211) % (1 << 64)
    return "%016x" % h
readable = "".join(c if c.isalnum() or c in ".-" else "_" for c in origin[:40])
d = os.path.join(root, fnv(origin) + "-" + readable)
os.makedirs(d, exist_ok=True)
open(os.path.join(d, fnv("permissions") + ".kv"), "w").write('{"notifications":"granted"}')
PY
run() { SOLAR_GUI_SETTLE=${2:-6000} SOLAR_GUI_SHOT="$DIR/x.png" timeout 60 ./solar "$1" 2>&1; }
echo "== register and features"; run "http://127.0.0.1:$PORT/sw-scope/register.html" 9000
echo "== navigations through the worker, with the worker running (one page after another)"; run "http://127.0.0.1:$PORT/sw-scope/virtual-start.html?next=%2Fsw-scope%2Fvirtual-next.html&wait=100" 4000
echo "== out of the scope for longer than the worker is kept, then back in"; run "http://127.0.0.1:$PORT/sw-scope/virtual-before.html?next=%2Foutside.html&wait=100" 6000
