#include "Compress.h"

#include <brotli/encode.h>
#include <zlib.h>
#include <zstd.h>

#include <vector>

namespace solar::test {

namespace {

std::string Deflate(std::string_view data, int windowBits) {
  z_stream stream{};
  deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, windowBits, 8, Z_DEFAULT_STRATEGY);
  std::string out;
  std::vector<unsigned char> chunk(64 * 1024);
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
  stream.avail_in = static_cast<uInt>(data.size());
  int result;
  do {
    stream.next_out = chunk.data();
    stream.avail_out = static_cast<uInt>(chunk.size());
    result = deflate(&stream, Z_FINISH);
    out.append(reinterpret_cast<char*>(chunk.data()), chunk.size() - stream.avail_out);
  } while (result != Z_STREAM_END);
  deflateEnd(&stream);
  return out;
}

}  // namespace

std::string Gzip(std::string_view data) { return Deflate(data, 15 + 16); }
std::string ZlibDeflate(std::string_view data) { return Deflate(data, 15); }
std::string RawDeflate(std::string_view data) { return Deflate(data, -15); }

std::string Brotli(std::string_view data) {
  std::string out(BrotliEncoderMaxCompressedSize(data.size()) + 16, '\0');
  size_t size = out.size();
  BrotliEncoderCompress(BROTLI_DEFAULT_QUALITY, BROTLI_DEFAULT_WINDOW, BROTLI_MODE_GENERIC, data.size(),
                        reinterpret_cast<const uint8_t*>(data.data()), &size, reinterpret_cast<uint8_t*>(out.data()));
  out.resize(size);
  return out;
}

std::string Zstd(std::string_view data) {
  std::string out(ZSTD_compressBound(data.size()), '\0');
  const size_t size = ZSTD_compress(out.data(), out.size(), data.data(), data.size(), 3);
  out.resize(size);
  return out;
}

}  // namespace solar::test
