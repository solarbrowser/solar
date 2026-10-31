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

## Download

A network resource is fetched in ranges (`ByteStream` in `include/solar/media/Player.h`, with `Fetcher` in `src/web-api/Media.cpp` doing the HTTP): 4 MB at a time, with a
`Range` header, as far ahead of what is decoded as 16 MB (2 MB for a video that is paused and only wanted its metadata: `preload` and `play()` change it), from where a seek
goes, and what is far from the reader is let go of past 96 MB. A file with its index at the end (an MP4 not made for the web) is read from its end first, as FFmpeg asks. A
server that does not do ranges (a 200 for the range request) is read whole, in one fetch, and all of it is kept. `buffered` is the byte ranges kept, taken as proportional to
time. Not done: the HTTP cache is not used for the pieces, and nothing is kept on disk.

## Controls

A video with `controls` has a bar drawn by the program over the bottom of the picture (`DrawControls` in `src/paint/SkiaPainter.cpp`: play or pause, the time in a small
pixel font, the position with what is buffered, the sound), shown while it is paused or the pointer has moved lately. The clicks are taken in `src/web-api/js/media.js`,
which has the same measures (a 36 px bar; the time takes 140 px when the video is 360 px wide or more; the sound button is the last 36 px). Not done: keyboard, full screen,
speed and captions menus, the volume as a slider.
