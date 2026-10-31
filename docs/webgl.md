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
Windows, Metal on macOS, OpenGL on Linux (Vulkan with the variable); `SOLAR_ANGLE_BACKEND=vulkan|gl|d3d11|metal` chooses, `SOLAR_ANGLE_SWIFTSHADER=1` uses its software Vulkan. Without ANGLE, EGL's own devices are
tried (a GPU with no window), then the default display (Mesa's llvmpipe works).

Tested on Linux with the system's NVIDIA and with ANGLE (OpenGL backend). Windows and macOS (D3D11, Metal) are not tested.

### Building ANGLE (`third_party/angle` submodule)

```
git clone --depth 1 https://chromium.googlesource.com/chromium/tools/depot_tools.git ~/depot_tools
cd third_party/angle      # .gclient: one unmanaged solution "." with url .../angle/angle.git
~/depot_tools/gclient sync --no-history --nohooks -D
buildtools/linux64/gn gen ~/angle-out --args='is_debug=false is_component_build=false angle_enable_swiftshader=false angle_enable_wgpu=false enable_rust=false use_custom_libcxx=false use_sysroot=false use_dummy_lastchange=true treat_warnings_as_errors=false is_clang=false extra_cflags=["-include","cstdint","-include","variant","-include","iostream"]'
ninja -C ~/angle-out libEGL libGLESv2
mkdir angle && cp ~/angle-out/libEGL.so ~/angle-out/libGLESv2.so angle/     # beside ./solar (not committed)
```

(`is_clang=false` and the `-include` flags are for the machine's gcc, whose clang has no compiler-rt. On Windows and macOS use Chromium's clang as ANGLE's own instructions say and copy
`libEGL.dll` + `libGLESv2.dll` (+ `d3dcompiler_47.dll`) or the `.dylib` files beside the program.)

## Known gaps

Works in workers (`OffscreenCanvas`) too; no `EXT_disjoint_timer_query`, no `WEBGL_multi_draw`, no video or `VideoFrame` textures through
`texImage2D` of an element not already decoded to a picture, formats the sources are converted to are 8-bit ones (and 4444/5551/565 and float).
