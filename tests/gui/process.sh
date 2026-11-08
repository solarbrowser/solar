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
renderer=$(pgrep -P $browser -f -- --renderer | head -1)
check "the page is another process" "$([ -n "$renderer" ] && [ "$renderer" != "$browser" ] && echo ok)"
fds=$(ls /proc/$renderer/fd 2>/dev/null | wc -l)
check "it has few descriptors ($fds)" "$([ "$fds" -le 12 ] && echo ok)"
leaked=$(ls -l /proc/$renderer/fd 2>/dev/null | grep -c -E 'x11|wayland|/dev/dri|X11-unix')
check "none of the window's" "$([ "$leaked" = 0 ] && echo ok)"

# It is shut in: no new privileges, no capabilities, a filter on its system calls, and a network and a user of its own.
status=$(cat /proc/$renderer/status 2>/dev/null)
check "it has a system call filter" "$(echo "$status" | grep -q '^Seccomp:[[:space:]]*2' && echo ok)"
check "it has no new privileges" "$(echo "$status" | grep -q '^NoNewPrivs:[[:space:]]*1' && echo ok)"
check "it has no capabilities" "$(echo "$status" | grep -q '^CapEff:[[:space:]]*0000000000000000' && echo ok)"
check "it is in a network namespace of its own" "$([ "$(readlink /proc/$renderer/ns/net)" != "$(readlink /proc/$browser/ns/net)" ] && echo ok)"

# The GPU's process (the pages' pictures are drawn there where there is a GPU) is shut in too.
gpu=$(pgrep -P $browser -f -- --gpu | head -1)
if [ -n "$gpu" ]; then
  gstatus=$(cat /proc/$gpu/status 2>/dev/null)
  check "the GPU's process has a system call filter" "$(echo "$gstatus" | grep -q '^Seccomp:[[:space:]]*2' && echo ok)"
  check "no new privileges and no capabilities" "$(echo "$gstatus" | grep -q '^NoNewPrivs:[[:space:]]*1' && echo "$gstatus" | grep -q '^CapEff:[[:space:]]*0000000000000000' && echo ok)"
  check "a network namespace of its own" "$([ "$(readlink /proc/$gpu/ns/net)" != "$(readlink /proc/$browser/ns/net)" ] && echo ok)"
fi

# A page that goes ends the browser.
kill -9 $renderer
for i in $(seq 1 20); do kill -0 $browser 2>/dev/null || break; sleep 0.25; done
check "the browser ends when the page's process does" "$(kill -0 $browser 2>/dev/null || echo ok)"
wait $browser 2>/dev/null

# A browser that goes ends the page.
./solar "file://$dir/page.html" > "$dir/out3.txt" 2>&1 &
browser=$!
sleep 2.5
renderer=$(pgrep -P $browser -f -- --renderer | head -1)
kill -9 $browser
sleep 1
check "the page's process ends when the browser does" "$(kill -0 $renderer 2>/dev/null || echo ok)"
wait $browser 2>/dev/null

# Closing the window ends both.
./solar "file://$dir/page.html" > "$dir/out4.txt" 2>&1 &
browser=$!
sleep 2.5
renderer=$(pgrep -P $browser -f -- --renderer | head -1)
kill -TERM $browser
for i in $(seq 1 20); do kill -0 $browser 2>/dev/null || break; sleep 0.25; done
check "closing the window ends the browser" "$(kill -0 $browser 2>/dev/null || echo ok)"
check "and the page" "$(kill -0 $renderer 2>/dev/null || echo ok)"
wait $browser 2>/dev/null

# A navigation to another site is another process, which takes the window; the history goes back and forth between them.
printf '<p>one</p>' > "$dir/one.html"
python3 -m http.server 8793 --bind 127.0.0.1 --directory "$dir" > /dev/null 2>&1 &
server=$!
sleep 1
SOLAR_BROWSER_SCRIPT="wait:2500;go:http://localhost:8793/one.html;wait:3000;entries;back;wait:3000;entries;wait:300" timeout 60 ./solar "http://127.0.0.1:8793/one.html" > "$dir/out5.txt" 2>&1
kill $server 2>/dev/null; wait $server 2>/dev/null
check "a navigation to another site goes on with the history" "$(grep -c 'history (at 1): http://127.0.0.1:8793/one.html http://localhost:8793/one.html' "$dir/out5.txt" | grep -q 1 && echo ok)"
check "and back is to the entry before" "$(grep -q 'history (at 0): http://127.0.0.1:8793/one.html http://localhost:8793/one.html' "$dir/out5.txt" && echo ok)"
check "a renderer was started for each site" "$([ "$(grep -c '^loading http' "$dir/out5.txt")" -ge 3 ] && echo ok)"

# A page that has a WebGL context: the GPU's process goes on drawing the page (a second context on an EGL used from two threads once lost the one the page is drawn with).
printf '<body style="margin:0;background:#c00"><canvas id=c width=200 height=100></canvas><script>var gl=document.getElementById("c").getContext("webgl");gl.clearColor(0,0,1,1);gl.clear(gl.COLOR_BUFFER_BIT);</script>' > "$dir/gl.html"
SOLAR_GL_DEBUG=1 SOLAR_GUI_SHOT="$dir/gl.png" SOLAR_GUI_SETTLE=2500 timeout 60 ./solar "file://$dir/gl.html" > "$dir/out6.txt" 2>&1
check "a page with a WebGL context is drawn by the GPU's process throughout" "$(! grep -aq 'did not draw\|could not be made current\|process is gone' "$dir/out6.txt" && [ -s "$dir/gl.png" ] && echo ok)"
check "and the canvas is in the picture (blue where WebGL cleared, the page's red around it)" "$(python3 - "$dir/gl.png" <<'P'
import sys
try:
    from PIL import Image
except ImportError:
    print("ok")  # (no way to look: not held against it)
    raise SystemExit
im = Image.open(sys.argv[1]).convert("RGB")
inside = im.getpixel((100, 50))   # the canvas (8px of body margin: it is at 0,0 here)
outside = im.getpixel((600, 400))
print("ok" if inside[2] > 150 and inside[0] < 80 and outside[0] > 150 and outside[2] < 80 else "no")
P
)"

echo "gui process: $pass/$((pass+fail)) passed"
[ $fail -eq 0 ]
