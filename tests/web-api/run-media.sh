#!/bin/sh
# The media tests: makes the big files the download test wants (26 MB each, not kept in the repository; needs ffmpeg), starts the test server, and runs the pages that check
# playing, MediaSource, the controls and the download. Prints the console lines (ok / FAIL).   Usage: tests/web-api/run-media.sh   (from the repository root, after make)
set -e
cd tests/web-api/media
if [ ! -f big.mp4 ]; then
  ffmpeg -hide_banner -loglevel error -y -f lavfi -i "testsrc2=duration=40:size=640x360:rate=25,noise=alls=40:allf=t" -f lavfi -i "sine=frequency=330:duration=40" \
    -c:v libx264 -b:v 5M -maxrate 5M -bufsize 10M -pix_fmt yuv420p -g 50 -c:a aac -b:a 96k -movflags +faststart big.mp4
  ffmpeg -hide_banner -loglevel error -y -i big.mp4 -c copy big-tail.mp4   # (the index at the end: the muxer's default)
fi
cd ../../..
PORT=8765
export SDL_AUDIODRIVER=dummy SDL_VIDEO_DRIVER=dummy SOLAR_GUI_CPU=1
python3 tests/web-api/wsserver.py $PORT >/dev/null 2>&1 &
SERVER=$!
trap 'kill $SERVER 2>/dev/null' EXIT
sleep 1
for page in media mediasource media-controls media-range; do
  echo "== $page"
  SOLAR_GUI_SETTLE=60000 SOLAR_GUI_SHOT=/tmp/solar-media-$page.png timeout 120 ./solar "http://127.0.0.1:$PORT/$page.html" 2>&1 | grep -v "^loading\|^loaded"
done
