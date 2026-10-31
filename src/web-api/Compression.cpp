/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/web-api/Compression.h"

#include <zlib.h>

#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace solar::webapi {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::Value;

namespace {

struct Z {
  z_stream stream{};
  bool compress = false;
  bool ended = false;  // an inflate stream that reached its end
  ~Z() {
    if (compress) deflateEnd(&stream);
    else inflateEnd(&stream);
  }
};

// (The page thread and the workers' realms share it; ids are never reused.)
std::map<int, std::unique_ptr<Z>>& Streams() {
  static std::map<int, std::unique_ptr<Z>> streams;
  return streams;
}
int g_next = 1;

Value Result(Context& ctx, const std::vector<uint8_t>& bytes) {
  Value buffer = qe::NewArrayBuffer(ctx, bytes.size());
  if (!bytes.empty()) {
    if (auto out = qe::MutableBytesOf(buffer)) std::memcpy(out->data(), bytes.data(), bytes.size());
  }
  return buffer;
}

Z* Find(Context& ctx, qe::Args args) {
  if (args.empty()) return nullptr;
  const auto found = Streams().find(static_cast<int>(qe::ToNumber(ctx, args[0])));
  return found == Streams().end() ? nullptr : found->second.get();
}

// __solarZNew(format, compress)
Value New(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 2) return qe::FromNumber(-1);
  const std::string format = qe::ToWtf8(ctx, args[0]);
  const bool compress = qe::ToNumber(ctx, args[1]) != 0;
  int bits;
  if (format == "gzip") bits = 15 + 16;
  else if (format == "deflate") bits = 15;
  else if (format == "deflate-raw") bits = -15;
  else return qe::FromNumber(-1);
  auto z = std::make_unique<Z>();
  z->compress = compress;
  const int status = compress ? deflateInit2(&z->stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, bits, 8, Z_DEFAULT_STRATEGY) : inflateInit2(&z->stream, bits);
  if (status != Z_OK) {
    z->compress = false;  // (nothing to end)
    return qe::FromNumber(-1);
  }
  const int id = g_next++;
  Streams()[id] = std::move(z);
  return qe::FromNumber(id);
}

// Runs the stream over `input` with `flush`; false where the data was not valid (or, for inflate, went on after the end).
bool Run(Z& z, const uint8_t* input, size_t size, int flush, std::vector<uint8_t>& out) {
  z.stream.next_in = const_cast<uint8_t*>(input);
  z.stream.avail_in = static_cast<uInt>(size);
  uint8_t buffer[16384];
  for (;;) {
    z.stream.next_out = buffer;
    z.stream.avail_out = sizeof buffer;
    int status;
    if (z.compress) {
      status = deflate(&z.stream, flush);
      if (status == Z_STREAM_ERROR) return false;
    } else {
      if (z.ended) return z.stream.avail_in == 0;
      status = inflate(&z.stream, flush);
      if (status == Z_STREAM_END) z.ended = true;
      else if (status != Z_OK && status != Z_BUF_ERROR) return false;
    }
    out.insert(out.end(), buffer, buffer + (sizeof buffer - z.stream.avail_out));
    if (z.ended) return z.stream.avail_in == 0;
    if (z.stream.avail_out != 0) return true;  // everything it had was written
  }
}

// __solarZWrite(id, bytes)
Value Write(Context& ctx, Value, qe::Args args, Value) {
  Z* z = Find(ctx, args);
  if (!z || args.size() < 2) return qe::Undefined();
  auto bytes = qe::BytesOf(args[1]);
  if (!bytes) return qe::Undefined();
  std::vector<uint8_t> out;
  if (!Run(*z, bytes->data(), bytes->size(), Z_NO_FLUSH, out)) return qe::Undefined();
  return Result(ctx, out);
}

// __solarZFinish(id): what is left; for inflate, undefined if the stream did not reach its end (it was cut short).
Value Finish(Context& ctx, Value, qe::Args args, Value) {
  Z* z = Find(ctx, args);
  if (!z) return qe::Undefined();
  std::vector<uint8_t> out;
  if (!Run(*z, nullptr, 0, z->compress ? Z_FINISH : Z_SYNC_FLUSH, out)) return qe::Undefined();
  if (!z->compress && !z->ended) return qe::Undefined();
  return Result(ctx, out);
}

Value Free(Context& ctx, Value, qe::Args args, Value) {
  if (!args.empty()) Streams().erase(static_cast<int>(qe::ToNumber(ctx, args[0])));
  return qe::Undefined();
}

}  // namespace

void InstallCompressionNatives(qe::Realm& realm) {
  Context& ctx = realm.GetContext();
  qe::DefineGlobalFunction(ctx, "__solarZNew", New, 2);
  qe::DefineGlobalFunction(ctx, "__solarZWrite", Write, 2);
  qe::DefineGlobalFunction(ctx, "__solarZFinish", Finish, 1);
  qe::DefineGlobalFunction(ctx, "__solarZFree", Free, 1);
}

}  // namespace solar::webapi
