#!/bin/sh
# The Pointer Lock test: a click locks the pointer, two relative moves are measured by the page, Escape unlocks. Run from the repository root, after make.
SDL_VIDEO_DRIVER=dummy SOLAR_GUI_CPU=1 SOLAR_GUI_SCRIPT="c:60,60;r:5,1;r:3,-4;k:escape" SOLAR_GUI_SETTLE=4000 timeout 40 ./solar "file://$PWD/tests/web-api/pointerlock.html" 2>&1 | grep -v "^loading\|^loaded"
