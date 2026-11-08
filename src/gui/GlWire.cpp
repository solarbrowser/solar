/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/gui/GlWire.h"

#include <cmath>
#include <cstring>

namespace solar::gui::glwire {

using net::ipc::Reader;
using net::ipc::Writer;
using paint::gl::Arg;
using paint::gl::Result;

int OutputArgument(const std::string& operation) {
  if (operation == "readPixels") return 6;
  if (operation == "getBufferSubData") return 2;
  return -1;
}

namespace {
void WriteDouble(Writer& out, double v) {
  uint64_t bits;
  std::memcpy(&bits, &v, 8);
  out.U64(bits);
}
double ReadDouble(Reader& in) {
  const uint64_t bits = in.U64();
  double v;
  std::memcpy(&v, &bits, 8);
  return std::isfinite(v) ? v : 0.0;
}
}  // namespace

void WriteCall(Writer& out, const Call& call, const std::string& operationName) {
  out.U32(call.context);
  out.U32(call.operation);
  out.U64(call.request);
  const int output = OutputArgument(operationName);
  out.U32(static_cast<uint32_t>(call.args.size()));
  for (size_t i = 0; i < call.args.size(); ++i) {
    const Arg& a = call.args[i];
    out.U8(static_cast<uint8_t>(a.kind));
    switch (a.kind) {
      case Arg::Kind::Number: WriteDouble(out, a.number); break;
      case Arg::Kind::Text: out.String(a.text); break;
      case Arg::Kind::Bytes:
        if (static_cast<int>(i) == output) {
          out.Bool(true);
          out.U64(a.size);  // (only how much is wanted)
        } else {
          out.Bool(false);
          out.String(std::string_view(reinterpret_cast<const char*>(a.data), a.size));
        }
        break;
      default: break;
    }
  }
}

bool ReadCall(Reader& in, Call& call, size_t operationCount, const std::vector<std::string>& names) {
  call.context = in.U32();
  call.operation = in.U32();
  call.request = in.U64();
  const uint32_t count = in.U32();
  if (!in.ok() || call.operation >= operationCount || count > 64) return false;
  const int output = OutputArgument(names[call.operation]);
  call.args.clear();
  call.blobs.clear();
  call.blobs.reserve(count);  // (the arguments point into these: they are not to move)
  for (uint32_t i = 0; i < count && in.ok(); ++i) {
    Arg a;
    a.kind = in.EnumUpTo(Arg::Kind::Text);
    switch (a.kind) {
      case Arg::Kind::Number: a.number = ReadDouble(in); break;
      case Arg::Kind::Text: a.text = in.String(size_t{1} << 24); break;
      case Arg::Kind::Bytes: {
        const bool isOutput = in.Bool();
        if (isOutput) {
          const uint64_t size = in.U64();
          if (static_cast<int>(i) != output || size > (size_t{256} << 20)) return false;
          call.blobs.emplace_back(static_cast<size_t>(size), '\0');
          a.writable = true;
        } else {
          call.blobs.push_back(in.String(kMostCallsBytes));
        }
        std::string& blob = call.blobs.back();
        a.data = reinterpret_cast<uint8_t*>(blob.data());
        a.size = blob.size();
        break;
      }
      default: break;
    }
    call.args.push_back(std::move(a));
  }
  return in.ok();
}

void WriteResult(Writer& out, const Result& r, const std::string& outBytes) {
  out.U8(static_cast<uint8_t>(r.kind));
  switch (r.kind) {
    case Result::Kind::Number: WriteDouble(out, r.number); break;
    case Result::Kind::Text: out.String(r.text); break;
    case Result::Kind::Numbers:
      out.U32(static_cast<uint32_t>(r.numbers.size()));
      for (double v : r.numbers) WriteDouble(out, v);
      break;
    default: break;
  }
  out.String(outBytes);
}

bool ReadResult(Reader& in, Result& r, std::string& outBytes) {
  r.kind = in.EnumUpTo(Result::Kind::Numbers);
  switch (r.kind) {
    case Result::Kind::Number: r.number = ReadDouble(in); break;
    case Result::Kind::Text: r.text = in.String(size_t{1} << 24); break;
    case Result::Kind::Numbers: {
      const uint32_t n = in.U32();
      if (!in.ok() || n > (1u << 20)) return false;
      for (uint32_t i = 0; i < n && in.ok(); ++i) r.numbers.push_back(ReadDouble(in));
      break;
    }
    default: break;
  }
  outBytes = in.String(size_t{256} << 20);
  return in.ok();
}

}  // namespace solar::gui::glwire
