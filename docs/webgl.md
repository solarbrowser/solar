# WebGL

`getContext("webgl" | "webgl2" | "experimental-webgl")` on a canvas gives WebGL 1 or 2. The context is an OpenGL ES 3 context made through EGL with no window; what the page draws is read
from its drawing buffer a moment later (one task of the loop for all the drawing there was) into the canvas's surface, which the window shows like a 2D canvas.

- `src/paint/WebGl.cpp`: the natives (`__solarGlCreate`, `__solarGl`, ...): the libraries, the display, the contexts, the drawing buffer (multisampled and resolved when `antialias`),
  what is read and what is cleared (`preserveDrawingBuffer`), each function of the standard.
- `src/paint/GlApi.h`: the table of OpenGL ES functions, loaded with `dlopen` (`libEGL`, `libGLESv2`), so the program does not link them and a machine with none just has no WebGL.
- `src/web-api/js/webgl.js`, `webgl-constants.js`: the interface (objects, argument checks, `getParameter`, pixel storage of WebGL's own, textures from images/canvases/ImageData,
  extensions, context loss).

## Libraries

The libraries are looked for beside the program (`angle/` or the same directory) first, then in the system. With ANGLE (`libEGL` + `libGLESv2` of ANGLE) the display is ANGLE's: Direct3D 11 on
Windows, Metal on macOS, Vulkan on Linux; `SOLAR_ANGLE_BACKEND=vulkan|gl|d3d11|metal` chooses, `SOLAR_ANGLE_SWIFTSHADER=1` uses its software Vulkan. Without ANGLE, EGL's own devices are
tried (a GPU with no window), then the default display (Mesa's llvmpipe works).

Tested on Linux with the system's NVIDIA and Mesa. ANGLE builds and the Windows and macOS packaging are not tested.

## Known gaps

No workers (`OffscreenCanvas.getContext("webgl")` is null there), no `EXT_disjoint_timer_query`, no `WEBGL_multi_draw`, no video or `VideoFrame` textures through
`texImage2D` of an element not already decoded to a picture, formats the sources are converted to are 8-bit ones (and 4444/5551/565 and float).
