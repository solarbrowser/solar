#!/bin/sh
# The Pointer Lock test: a click locks the pointer, two relative moves are measured by the page, Escape unlocks. Run from the repository root, after make.
SDL_VIDEO_DRIVER=dummy SOLAR_GUI_CPU=1 SOLAR_GUI_SCRIPT="c:60,60;r:5,1;r:3,-4;k:escape" SOLAR_GUI_SETTLE=4000 timeout 15 ./solar "file://$PWD/tests/web-api/pointerlock.html" 2>&1 | grep -v "^loading\|^loaded"
# The window takes the pointer back from the page (f:): the page is told, the lock is gone.
SDL_VIDEO_DRIVER=dummy SOLAR_GUI_CPU=1 SOLAR_GUI_SCRIPT="c:60,60;r:5,1;f:1;r:3,-4" SOLAR_GUI_SETTLE=5000 timeout 15 ./solar "file://$PWD/tests/web-api/pointerlock-taken-back.html" 2>&1 | grep "^ok\|^FAIL"
