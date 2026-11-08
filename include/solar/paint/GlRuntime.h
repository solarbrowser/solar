/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

// WebGL's context and its operations, with no script in them: the numbers, bytes and strings a call is given and what it gives back are plain values, so that what runs the calls can be in
// the GPU's process (the page's has no GPU) as well as where the script is. A context is an OpenGL ES 3 context of the machine's (or ANGLE's), made through EGL with no window, that draws
// into a framebuffer of its own (the drawing buffer).
namespace solar::paint::gl {

// What a call is given: a number (a boolean is one), bytes (a typed array: `writable` where the call writes into it), a string, or nothing.
struct Arg {
  enum class Kind : uint8_t { Undefined, Null, Number, Bytes, Text } kind = Kind::Undefined;
  double number = 0;
  uint8_t* data = nullptr;
  size_t size = 0;
  bool writable = false;
  std::string text;
};
using Args = std::span<const Arg>;

// What a call gives back: nothing, a number, a string, or an array of numbers.
struct Result {
  enum class Kind : uint8_t { Undefined, Number, Text, Numbers } kind = Kind::Undefined;
  double number = 0;
  std::string text;
  std::vector<double> numbers;
  static Result Nothing() { return {}; }
  static Result Of(double v) { Result r; r.kind = Kind::Number; r.number = v; return r; }
  static Result Text(std::string s) { Result r; r.kind = Kind::Text; r.text = std::move(s); return r; }
  static Result Numbers(std::vector<double> v) { Result r; r.kind = Kind::Numbers; r.numbers = std::move(v); return r; }
};

// Whether there is anything to make a context with (the libraries of OpenGL ES are opened when this is first asked).
bool Available();
// The names of the operations, whose numbers are their places, and whether an operation draws into the drawing buffer.
const std::vector<std::string>& OperationNames();
bool Draws(const std::string& name);
// Whether the call has a result the script is waiting for (a query, a creation): the others can be sent on and not waited for.
bool NeedsAnswer(const std::string& name);

class Context {
 public:
  // `attributes` is the JSON text of the context's attributes (antialias, alpha, depth, stencil, premultipliedAlpha, preserveDrawingBuffer). Null where one cannot be made.
  static std::unique_ptr<Context> Create(int version, const std::string& attributes, int width, int height);
  ~Context();
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;

  // JSON text of what the context is: the renderer, the version, the extensions it has, the number of samples and the name the drawing buffer's framebuffer has.
  std::string Info();
  void Resize(int width, int height);
  int width() const;
  int height() const;
  bool premultiplied() const;
  // One call of the standard's (the operation by its place in OperationNames()).
  Result Call(size_t operation, Args args);
  // What was drawn, as pixels (R G B A, the first row at the bottom as OpenGL has it); the drawing buffer is cleared after, unless the page keeps it.
  std::vector<uint8_t> Present();
  // Something was drawn since Present.
  bool dirty = false;

  // The drawing buffer, made a texture that is kept (the one this context last published), for what draws it where the contexts are shared with the drawing of the page (the GPU's
  // process): it is not read into memory. The drawing buffer is cleared after, unless the page keeps it. False where the context is not one that shares.
  struct Snapshot {
    uint32_t texture = 0;
    int width = 0, height = 0;
    bool premultiplied = true;
    uint64_t generation = 0;  // (it changes with the texture)
  };
  bool Publish(Snapshot& out);
  bool shares() const;
  // What was last published (its texture is valid until the next Publish at another size, a Resize or the end of the context: with SharedLock held).
  Snapshot LastSnapshot() const;

  struct Impl;

 private:
  Context();
  std::unique_ptr<Impl> impl_;
};

// Runs `work` on the thread that all the GPU's work is on, and waits for it: what is made on one EGL (ANGLE's) is used from one thread, as the GPU's process of a browser does, because its
// contexts do not take being used from the several threads of the connections.
void RunOnGlThread(const std::function<void()>& work);

// A context for the drawing of the page (Skia's), made on the EGL that WebGL's contexts are made on (ANGLE where there is one, else the machine's), and shared with them: the two draw
// into one another's textures only on one EGL. `makeCurrent(display, context)` makes it current in the calling thread (a null context lets go).
struct RasterEgl {
  void* display = nullptr;
  void* context = nullptr;
  void* (*getProc)(const char*) = nullptr;
  bool (*makeCurrent)(void* display, void* context) = nullptr;
};
bool CreateRasterContext(RasterEgl& out, std::string* why = nullptr);
void DestroyRasterContext(RasterEgl& egl);

// Contexts made after this share their textures with `context` of `display` (EGL's handles; the GPU's process gives the one Skia draws with), so that what a page's WebGL drew is
// drawn into the page without being read: a context that cannot share is made without it.
void SetShare(void* display, void* context);
// Held by whoever draws with the shared textures, and by whoever changes or ends them (a publish, a resize, a destroy): so that a texture is not deleted while it is being drawn.
std::recursive_mutex& SharedLock();
// The contexts that a page's connection (`token`) has, by number, for the drawing of the page to find the texture of one (with SharedLock held).
void RegisterShared(uint64_t token, uint32_t id, Context* context);
void UnregisterShared(uint64_t token, uint32_t id);
bool LookupSnapshot(uint64_t token, uint32_t id, Context::Snapshot& out);
// Whether the page's WebGL is drawn from textures (it is, unless the drawing could not be done by the GPU's process, or SOLAR_GL_READBACK says it is not to be).
bool CompositionEnabled();
void DisableComposition();

// Where the contexts are: in this process (the default), or in another (the GPU's), asked by number. What the script's natives call; contexts are numbered by the backend.
class Backend {
 public:
  virtual ~Backend() = default;
  // The number of a new context (0 where none could be made).
  virtual uint32_t Create(int version, const std::string& attributes, int width, int height) = 0;
  virtual std::string Info(uint32_t context) = 0;
  virtual void Resize(uint32_t context, int width, int height) = 0;
  virtual bool Premultiplied(uint32_t context) = 0;
  // The pixels of what was drawn (R G B A, the first row at the bottom), `width` x `height` of them; empty where there are none.
  virtual std::vector<uint8_t> Present(uint32_t context, int& width, int& height) = 0;
  virtual void Destroy(uint32_t context) = 0;
  // One call; `answer` is whether the script waits for what it gives back (the others may be sent on).
  virtual Result Call(uint32_t context, size_t operation, Args args, bool answer) = 0;
  // What was drawn is published as a texture in the process that draws the page (where that is another): true where it was, and the page then refers to the context in the drawing of its
  // canvas instead of giving the picture.
  virtual bool Publish(uint32_t context) { (void)context; return false; }
};

// The contexts of this process, made with EGL here.
Backend& LocalBackend();
// The backend the natives use (the local one unless another is set; not owned).
Backend& CurrentBackend();
void SetBackend(Backend* backend);

}  // namespace solar::paint::gl
