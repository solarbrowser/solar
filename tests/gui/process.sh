#!/usr/bin/env bash
# The page is a process of its own: it paints what the window shows, it has only what it was given (its two connections), it ends when the browser does and the browser ends when it does.
# Run from the repository root with ./solar built. No window is opened (SDL's dummy driver).
export SDL_VIDEODRIVER=dummy SOLAR_GUI_CPU=1
pass=0; fail=0
check() { if [ "$2" = "ok" ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL $1"; fi; }
dir=$(mktemp -d); trap 'rm -rf "$dir"' EXIT
printf '<body style="margin:0;background:#c00"><input style="width:300px;height:40px;margin:10px"><p>process test</p>' > "$dir/page.html"

# It paints (a picture of the window, made by the page's process and shown by the browser's).
SOLAR_GUI_SHOT="$dir/shot.png" SOLAR_GUI_SETTLE=1500 timeout 60 ./solar "file://$dir/page.html" > "$dir/out.txt" 2>&1
check "the program ends well" "$([ $? -eq 0 ] && echo ok)"
check "a picture was made" "$([ "$(head -c 4 "$dir/shot.png" 2>/dev/null | tail -c 3)" = "PNG" ] && echo ok)"
check "of the window's size" "$(python3 - "$dir/shot.png" <<'P'
import sys, zlib, struct
d = open(sys.argv[1], 'rb').read()
pos = 8; idat = b''; w = h = 0
while pos < len(d):
    n, t = struct.unpack('>I4s', d[pos:pos+8])
    if t == b'IHDR': w, h = struct.unpack('>II', d[pos+8:pos+16])
    if t == b'IDAT': idat += d[pos+8:pos+8+n]
    pos += 12 + n
raw = zlib.decompress(idat)
bpp = 4
stride = w * bpp
print("ok" if (w, h) == (1280, 800) and len(raw) == h * (stride + 1) else "no")
P
)"

# It is a process of its own, with no more descriptors than it needs.
./solar "file://$dir/page.html" > "$dir/out2.txt" 2>&1 &
browser=$!
sleep 2.5
renderer=$(pgrep -P $browser | head -1)
check "the page is another process" "$([ -n "$renderer" ] && [ "$renderer" != "$browser" ] && echo ok)"
fds=$(ls /proc/$renderer/fd 2>/dev/null | wc -l)
check "it has few descriptors ($fds)" "$([ "$fds" -le 12 ] && echo ok)"
leaked=$(ls -l /proc/$renderer/fd 2>/dev/null | grep -c -E 'x11|wayland|/dev/dri|X11-unix')
check "none of the window's" "$([ "$leaked" = 0 ] && echo ok)"

# A page that goes ends the browser.
kill -9 $renderer
for i in $(seq 1 20); do kill -0 $browser 2>/dev/null || break; sleep 0.25; done
check "the browser ends when the page's process does" "$(kill -0 $browser 2>/dev/null || echo ok)"
wait $browser 2>/dev/null

# A browser that goes ends the page.
./solar "file://$dir/page.html" > "$dir/out3.txt" 2>&1 &
browser=$!
sleep 2.5
renderer=$(pgrep -P $browser | head -1)
kill -9 $browser
sleep 1
check "the page's process ends when the browser does" "$(kill -0 $renderer 2>/dev/null || echo ok)"
wait $browser 2>/dev/null

# Closing the window ends both.
./solar "file://$dir/page.html" > "$dir/out4.txt" 2>&1 &
browser=$!
sleep 2.5
renderer=$(pgrep -P $browser | head -1)
kill -TERM $browser
for i in $(seq 1 20); do kill -0 $browser 2>/dev/null || break; sleep 0.25; done
check "closing the window ends the browser" "$(kill -0 $browser 2>/dev/null || echo ok)"
check "and the page" "$(kill -0 $renderer 2>/dev/null || echo ok)"
wait $browser 2>/dev/null

echo "gui process: $pass/$((pass+fail)) passed"
[ $fail -eq 0 ]
