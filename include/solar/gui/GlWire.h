/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "solar/net/Ipc.h"
#include "solar/paint/GlRuntime.h"

// WebGL's contexts in the GPU's process: what a page's process (a renderer) says to it, and it back. A page asks for a context, and sends the calls of the script in batches (the ones the
// script waits for the answer to are marked, and flush what came before them); what was drawn is made into pixels in memory the page made, which the GPU's process writes. It is not trusted:
// every number, size and string that is read is checked before it reaches the driver.
namespace solar::gui::glwire {

// Renderer to GPU.
enum class ToGl : uint8_t {
  Create = 1,   // request, version, attributes, width, height
  Resize = 2,   // context, width, height (the memory for its pictures follows as a Buffer)
  Calls = 3,    // a batch of calls
  Present = 4,  // request, context: the picture of what was drawn goes in the memory of the context
  Destroy = 5,  // context
  Buffer = 6,   // context, width, height (sent with the descriptor of the memory): where its pictures go
  Info = 7,     // request, context
  Publish = 8,  // request, context: what was drawn is made a texture, drawn from by the drawing of the page (the contexts share with it)
};
// GPU to renderer: the answer to a request (the number that was given).
enum class FromGl : uint8_t { Answer = 32 };

constexpr size_t kMostCallsBytes = size_t{32} << 20;  // (a batch)
constexpr int kMostSide = 8192;                        // (a context's drawing buffer)

// One call in a batch: the context, the operation, whether the script waits for what it gives, and what it is given. A call that writes into one of its arguments (a read of pixels) is
// given only how large it is.
struct Call {
  uint32_t context = 0;
  uint32_t operation = 0;
  uint64_t request = 0;  // (the number the answer has; 0 where none is wanted)
  std::vector<paint::gl::Arg> args;
  std::vector<std::string> blobs;  // what the bytes and texts of the arguments are held in (after a read)
};

// The argument of an operation that is written into, or -1 (what it is given is its size only).
int OutputArgument(const std::string& operation);

void WriteCall(net::ipc::Writer& out, const Call& call, const std::string& operationName);
// False where it is not a call (an operation that is none, a size or count that cannot be, a text too long).
bool ReadCall(net::ipc::Reader& in, Call& call, size_t operationCount, const std::vector<std::string>& names);

void WriteResult(net::ipc::Writer& out, const paint::gl::Result& result, const std::string& outBytes);
bool ReadResult(net::ipc::Reader& in, paint::gl::Result& result, std::string& outBytes);

}  // namespace solar::gui::glwire
