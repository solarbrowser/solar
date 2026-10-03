#include "solar/net/ContentDecoder.h"

#include <brotli/decode.h>
#include <zlib.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace solar::net {

namespace {

constexpr size_t kChunkSize = 64 * 1024;
const char kStopped[] = "decoding stopped";

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 0x20) : c; }

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return Lower(x) == Lower(y); });
}

// gzip, and deflate in either of its two spellings. deflate is meant to be zlib-wrapped, and some
// servers send the raw stream, so the first two bytes decide.
class ZlibDecoder final : public ContentDecoder {
 public:
  enum class Format { Gzip, Deflate };

  explicit ZlibDecoder(Format format) : format_(format) {}
  ~ZlibDecoder() override {
    if (initialized_) inflateEnd(&stream_);
  }

  std::optional<std::string> Decode(std::span<const uint8_t> input, const DecodedSink& output) override {
    if (ignoring_) return std::nullopt;

    if (!initialized_) {
      // gzip needs nothing but its first byte; deflate needs two to tell its spellings apart.
      const size_t needed = format_ == Format::Deflate ? 2 : 1;
      while (header_.size() < needed && !input.empty()) {
        header_.push_back(input.front());
        input = input.subspan(1);
      }
      if (header_.size() < needed) return std::nullopt;
      if (auto error = Initialize()) return error;
      std::vector<uint8_t> header = std::move(header_);
      if (auto error = Feed(header, output)) return error;
    }

    if (ended_) {
      // A gzip file may be several members in a row; anything else after the end is dropped.
      while (!input.empty() && trailing_.size() < 2) {
        trailing_.push_back(input.front());
        input = input.subspan(1);
      }
      if (trailing_.size() < 2) return std::nullopt;
      if (format_ == Format::Gzip && trailing_[0] == 0x1f && trailing_[1] == 0x8b) {
        inflateReset(&stream_);
        ended_ = false;
        std::vector<uint8_t> next = std::move(trailing_);
        trailing_.clear();
        if (auto error = Feed(next, output)) return error;
      } else {
        ignoring_ = true;
        return std::nullopt;
      }
    }
    return Feed(input, output);
  }

  std::optional<std::string> Finish(const DecodedSink&) override {
    if (!initialized_) return header_.empty() ? std::nullopt : std::optional<std::string>("truncated compressed body");
    return ended_ || ignoring_ ? std::nullopt : std::optional<std::string>("truncated compressed body");
  }

 private:
  std::optional<std::string> Initialize() {
    int windowBits;
    if (format_ == Format::Gzip) {
      windowBits = 15 + 16;
    } else {
      // A zlib header is a method nibble of 8 and a two-byte value divisible by 31.
      const bool wrapped = (header_[0] & 0x0F) == 8 && ((header_[0] << 8) | header_[1]) % 31 == 0;
      windowBits = wrapped ? 15 : -15;
    }
    std::memset(&stream_, 0, sizeof(stream_));
    if (inflateInit2(&stream_, windowBits) != Z_OK) return "cannot start the decompressor";
    initialized_ = true;
    return std::nullopt;
  }

  std::optional<std::string> Feed(std::span<const uint8_t> input, const DecodedSink& output) {
    stream_.next_in = const_cast<Bytef*>(input.data());
    stream_.avail_in = static_cast<uInt>(input.size());
    while (stream_.avail_in > 0 && !ended_) {
      std::array<uint8_t, kChunkSize> chunk;
      stream_.next_out = chunk.data();
      stream_.avail_out = static_cast<uInt>(chunk.size());
      const int result = inflate(&stream_, Z_NO_FLUSH);
      const size_t produced = chunk.size() - stream_.avail_out;
      if (produced > 0 && !output(std::span<const uint8_t>(chunk.data(), produced))) return kStopped;
      if (result == Z_STREAM_END) {
        ended_ = true;
        break;
      }
      if (result != Z_OK && result != Z_BUF_ERROR) return std::string("invalid compressed data: ") + (stream_.msg ? stream_.msg : "zlib error");
    }
    // What is left after the end of a stream is looked at as the next call's input.
    if (ended_ && stream_.avail_in > 0) {
      const std::vector<uint8_t> rest(stream_.next_in, stream_.next_in + stream_.avail_in);
      return Decode(rest, output);
    }
    return std::nullopt;
  }

  Format format_;
  z_stream stream_{};
  bool initialized_ = false;
  bool ended_ = false;
  bool ignoring_ = false;
  std::vector<uint8_t> header_;
  std::vector<uint8_t> trailing_;
};

class BrotliDecoder final : public ContentDecoder {
 public:
  BrotliDecoder() : state_(BrotliDecoderCreateInstance(nullptr, nullptr, nullptr)) {}
  ~BrotliDecoder() override { BrotliDecoderDestroyInstance(state_); }

  std::optional<std::string> Decode(std::span<const uint8_t> input, const DecodedSink& output) override {
    if (ended_) return std::nullopt;  // whatever follows the stream is dropped
    if (!input.empty()) sawInput_ = true;
    const uint8_t* next = input.data();
    size_t available = input.size();

    while (true) {
      std::array<uint8_t, kChunkSize> chunk;
      uint8_t* out = chunk.data();
      size_t outAvailable = chunk.size();
      const BrotliDecoderResult result = BrotliDecoderDecompressStream(state_, &available, &next, &outAvailable, &out, nullptr);
      const size_t produced = chunk.size() - outAvailable;
      if (produced > 0 && !output(std::span<const uint8_t>(chunk.data(), produced))) return kStopped;

      switch (result) {
        case BROTLI_DECODER_RESULT_ERROR:
          return std::string("invalid compressed data: ") + BrotliDecoderErrorString(BrotliDecoderGetErrorCode(state_));
        case BROTLI_DECODER_RESULT_SUCCESS:
          ended_ = true;
          return std::nullopt;
        case BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT:
          return std::nullopt;
        case BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT:
          break;
      }
    }
  }

  std::optional<std::string> Finish(const DecodedSink&) override {
    return !sawInput_ || ended_ ? std::nullopt : std::optional<std::string>("truncated compressed body");
  }

 private:
  BrotliDecoderState* state_;
  bool sawInput_ = false;
  bool ended_ = false;
};

class ZstdDecoder final : public ContentDecoder {
 public:
  ZstdDecoder() : stream_(ZSTD_createDStream()) {
    // RFC 8878 asks HTTP decoders to refuse windows over 8 MiB, which is what bounds the memory a
    // hostile frame can make this use.
    ZSTD_DCtx_setParameter(stream_, ZSTD_d_windowLogMax, 23);
  }
  ~ZstdDecoder() override { ZSTD_freeDStream(stream_); }

  std::optional<std::string> Decode(std::span<const uint8_t> input, const DecodedSink& output) override {
    if (!input.empty()) sawInput_ = true;
    ZSTD_inBuffer in{input.data(), input.size(), 0};
    while (true) {
      std::array<uint8_t, kChunkSize> chunk;
      ZSTD_outBuffer out{chunk.data(), chunk.size(), 0};
      const size_t inputBefore = in.pos;
      const size_t result = ZSTD_decompressStream(stream_, &out, &in);
      if (ZSTD_isError(result)) return std::string("invalid compressed data: ") + ZSTD_getErrorName(result);
      if (out.pos > 0 && !output(std::span<const uint8_t>(chunk.data(), out.pos))) return kStopped;
      // 0 means a frame is complete. After that a call that does nothing returns a hint for the
      // next frame's header, which is not an unfinished frame; only a call that made progress and
      // still has more to go is.
      if (result == 0) {
        frameComplete_ = true;
      } else if (in.pos > inputBefore || out.pos > 0) {
        frameComplete_ = false;
      }
      // Done once the input is used up and the output buffer was not left full.
      if (in.pos == in.size && out.pos < out.size) return std::nullopt;
    }
  }

  std::optional<std::string> Finish(const DecodedSink&) override {
    return !sawInput_ || frameComplete_ ? std::nullopt : std::optional<std::string>("truncated compressed body");
  }

 private:
  ZSTD_DStream* stream_;
  bool sawInput_ = false;
  bool frameComplete_ = true;
};

}  // namespace

std::unique_ptr<ContentDecoder> MakeContentDecoder(std::string_view coding) {
  if (EqualsIgnoreCase(coding, "gzip") || EqualsIgnoreCase(coding, "x-gzip")) return std::make_unique<ZlibDecoder>(ZlibDecoder::Format::Gzip);
  if (EqualsIgnoreCase(coding, "deflate")) return std::make_unique<ZlibDecoder>(ZlibDecoder::Format::Deflate);
  if (EqualsIgnoreCase(coding, "br")) return std::make_unique<BrotliDecoder>();
  if (EqualsIgnoreCase(coding, "zstd")) return std::make_unique<ZstdDecoder>();
  return nullptr;
}

std::unique_ptr<BodyDecoder> BodyDecoder::Create(const HttpResponseHead& head, uint64_t maxDecodedBytes) {
  std::vector<std::string> codings;
  for (const auto& [name, value] : head.headers) {
    if (!EqualsIgnoreCase(name, "content-encoding")) continue;
    size_t start = 0;
    while (start <= value.size()) {
      size_t end = value.find(',', start);
      if (end == std::string::npos) end = value.size();
      std::string_view token(value.data() + start, end - start);
      while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) token.remove_prefix(1);
      while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) token.remove_suffix(1);
      if (!token.empty()) codings.emplace_back(token);
      start = end + 1;
    }
  }
  if (codings.empty()) return nullptr;

  // The last coding listed was applied last, so it is undone first.
  std::vector<std::unique_ptr<ContentDecoder>> stages;
  for (auto it = codings.rbegin(); it != codings.rend(); ++it) {
    std::unique_ptr<ContentDecoder> decoder = MakeContentDecoder(*it);
    if (!decoder) return nullptr;  // one that is not understood leaves the whole body as it is
    stages.push_back(std::move(decoder));
  }
  return std::unique_ptr<BodyDecoder>(new BodyDecoder(std::move(stages), maxDecodedBytes));
}

// What stage `index` hands on goes to the next stage, and from the last one to `output` once it
// has been counted against the limit. A stage that stops because a later one failed reports only
// that it stopped, so the later one's error is kept and returned in its place.
DecodedSink BodyDecoder::SinkAfter(size_t index, const DecodedSink& output) {
  const bool last = index + 1 == stages_.size();
  return [this, index, last, &output](std::span<const uint8_t> decoded) {
    if (last) {
      produced_ += decoded.size();
      if (produced_ > limit_) {
        error_ = "decoded body exceeds the size limit";
        return false;
      }
      return output(decoded);
    }
    if (auto failure = Run(index + 1, decoded, output)) {
      if (!error_) error_ = failure;
      return false;
    }
    return true;
  };
}

std::optional<std::string> BodyDecoder::Run(size_t index, std::span<const uint8_t> input, const DecodedSink& output) {
  const DecodedSink next = SinkAfter(index, output);
  std::optional<std::string> failure = stages_[index]->Decode(input, next);
  return error_ ? error_ : failure;
}

std::optional<std::string> BodyDecoder::FinishFrom(size_t index, const DecodedSink& output) {
  const DecodedSink next = SinkAfter(index, output);
  if (auto failure = stages_[index]->Finish(next)) return error_ ? error_ : failure;
  if (error_) return error_;
  return index + 1 == stages_.size() ? std::nullopt : FinishFrom(index + 1, output);
}

std::optional<std::string> BodyDecoder::Decode(std::span<const uint8_t> input, const DecodedSink& output) { return Run(0, input, output); }

std::optional<std::string> BodyDecoder::Finish(const DecodedSink& output) { return FinishFrom(0, output); }

}  // namespace solar::net
