# Media (video and audio)

`<video>` and `<audio>` are played by `src/media` (a `Player` per element: a thread that demuxes and decodes with FFmpeg, a clock that the sound drives, and the latest
picture kept for whoever draws) and written as elements in `src/web-api/js/media.js` (the attributes, the resource selection, the events and promises), with the natives
of `src/web-api/Media.cpp` between the two. The window draws the current picture of a playing video each frame (`DrawVideo` in the display list asks the player for it); the
sound goes to SDL (`src/gui/SdlAudio.cpp`; programs without a window play without sound).

## FFmpeg

FFmpeg is **not linked**: `src/media/Ffmpeg.cpp` opens its shared libraries (`libavformat`, `libavcodec`, `libavutil`, `libswscale`, `libswresample`) the first time media
is played, by the sonames of the **major versions the program was built against** (the headers at build time decide). A machine without them plays no media
(`canPlayType` answers "", and an element gets `MEDIA_ERR_SRC_NOT_SUPPORTED`); everything else works.

* **Linux**: the system's libraries (`pkg-config` finds the headers; the package that has the development files of FFmpeg is needed to build).
* **Windows**: ship the DLLs of one LGPL "shared" build of FFmpeg beside the program: `avformat-N.dll`, `avcodec-N.dll`, `avutil-N.dll`, `swscale-N.dll`, `swresample-N.dll`
  with the N the build was made against (the loader looks for exactly these names, which are the DLLs' own). Not yet tried on Windows.
* **macOS**: put `libavformat.N.dylib` and the others in the app bundle's `Contents/Frameworks` (or next to the program) with an `@rpath` that reaches them. Not yet tried on macOS.

Build the headers' version and the shipped libraries from the same FFmpeg release. The codecs that matter for the web are the open ones (VP8, VP9, AV1, Opus, Vorbis, FLAC); H.264,
AAC and HEVC are patented formats: a build that includes them is the distributor's licence question (the operating system's own decoders — AVFoundation, Media Foundation —
are the later way round it, behind the same `Player`).
