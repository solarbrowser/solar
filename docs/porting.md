# Porting the browser to Windows and macOS

What builds and runs today: the URL library and the network layer on all three systems (CI runs `make test` on Linux, macOS with kqueue and Windows with IO completion ports).
**The browser** (the window, the pages' processes, the interface, the GPU's process) builds and runs on **Linux only**. This is the list of what stands between that and the other
two systems, found by looking at the code (not by building there: none of this has been tried on a Mac or a Windows machine).

## What is already portable

* The event loop has the three backends (`src/net/BackendUring.cpp`, `PollerKqueue.cpp`, `BackendIocp.cpp`, and the readiness one over epoll/kqueue that a shut-in page uses).
* The window, the keys, the pointer, the sound, the clipboard, the save dialog, the system's colours and `SDL_OpenURL`: SDL 3 (`src/gui/Window.cpp`, `SdlAudio.cpp`).
* The settings, the storage and the font cache already have a folder per system (`Settings::Path`, `StorageDirectory`, `CachePath`).
* FFmpeg is opened at run time by name (`src/media/Ffmpeg.cpp`, `docs/media.md` says what to ship).
* All of the interface: the pages in `src/gui/ui` are HTML, CSS and script of our own engine.

## What is Linux's (and what each system needs instead)

| What | Where | macOS | Windows |
|---|---|---|---|
| The two ends of a process's connection are a Unix socket pair that carries descriptors (`SCM_RIGHTS`) | `src/net/Ipc.cpp` | Same code (`SOCK_CLOEXEC` is not there: `fcntl`) | Named pipes, and `DuplicateHandle` for what is passed |
| Memory that two processes share (pictures, WebGL's) is a `memfd` | `src/gui/FrameBuffer.cpp`, `Sandbox.cpp` | `shm_open` + `shm_unlink` at once | `CreateFileMapping` + `DuplicateHandle` |
| A renderer is started by `posix_spawn` with its descriptors at 3 to 6, and the rest closed | `Main.cpp` (`SpawnRenderer`), `GpuProcess.cpp` | Same (`posix_spawn_file_actions_addclosefrom_np` is there) | `CreateProcess` with inherited handles whose numbers are on the command line |
| The process is shut in: user, network, IPC and UTS namespaces, Landlock, seccomp, no new privileges, caps dropped | `src/gui/Sandbox.cpp`, `GpuProcess.cpp` | Seatbelt (`sandbox_init` with a profile: what Chrome and Firefox use) | A restricted token, a job object, low integrity (or an AppContainer) |
| A process that goes with the browser | `prctl(PR_SET_PDEATHSIG)` | A pipe that is read: end of file is the browser gone | A job object with kill-on-close |
| The GPU's process: EGL on the machine's driver, ANGLE where it is; `/proc` to find drivers | `GpuProcess.cpp`, `GpuRaster.cpp`, `src/paint/GlRuntime.cpp` | ANGLE on Metal (the same EGL calls); no `/proc` | ANGLE on Direct3D 11 (the same EGL calls) |
| Skia is built with Vulkan and GL (`third_party/skia/out/StaticVk`) | `Makefile` | A Skia built for Metal/GL, per architecture | A Skia built for Direct3D/GL |
| `io_uring` in the page's loop when it is not shut in | `src/net/BackendUring.cpp` | kqueue (already there) | IOCP (already there) |
| `dlopen` of ANGLE and of FFmpeg by `.so` names | `GlRuntime.cpp`, `Ffmpeg.cpp` | `.dylib` names | `.dll` names |
| The JS engine: Quanta's Rust and C++, and a baseline JIT for x86-64 | `third_party/quanta` (it has a `build-windows.bat`) | Apple silicon is arm64: the interpreter alone until there is an arm64 JIT | x86-64 |

## An order that can be followed

1. **macOS first**, because it is POSIX: the sockets, the shared memory and the spawn are nearly the same code. The work is the Makefile (Skia, SDL, FFmpeg paths), Quanta on arm64
   (no JIT: the interpreter), ANGLE's `.dylib`, and then the Seatbelt profile. Run with `SOLAR_SANDBOX=off` until the profile is there.
2. **A shut-in page and a GPU's process on macOS**: Seatbelt for both, ANGLE's Metal for the GPU's process.
3. **Windows**: the channel and the shared memory are rewritten behind the same two classes (`net::ipc::Channel`, `gui::SharedMemory`), the spawn, the job object and the restricted
   token. The build is MSYS2 clang (the CI job that is there), Skia and Quanta built for it.
4. **Packaging**: an app bundle with FFmpeg's libraries and ANGLE (macOS), a folder with the DLLs (Windows), the icon, a signed binary where the system asks for it.

## What can only be done on the machine

Every step above needs the system to compile and to run: a Mac (Apple silicon, and an Intel one for the other architecture) and a Windows machine, or CI jobs that do the whole
build and keep the log. The CI file has the macOS and Windows jobs for the network layer; the next step for each is a job that builds Skia and Quanta and then `make solar`,
allowed to fail until it does, so that each push says what is left.
