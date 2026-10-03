#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "solar/net/Http1Parser.h"

namespace solar::net {

// Where decoded bytes go. Returning false stops the decoder, which then reports that it was stopped.
using DecodedSink = std::function<bool(std::span<const uint8_t>)>;

// One content coding, decoded as a stream: it gives the same output however the input is cut into
// Decode calls, and hands the output on in pieces so a large body is never held whole.
class ContentDecoder {
 public:
  virtual ~ContentDecoder() = default;

  // Returns an error message when the input is not valid for the coding.
  virtual std::optional<std::string> Decode(std::span<const uint8_t> input, const DecodedSink& output) = 0;
  // The input has ended: a stream that was started and is not complete is an error.
  virtual std::optional<std::string> Finish(const DecodedSink& output) = 0;
};

// gzip (and x-gzip), deflate, br or zstd, without regard to case; null for anything else.
std::unique_ptr<ContentDecoder> MakeContentDecoder(std::string_view coding);

// The decoding a response's Content-Encoding calls for, as the Fetch Standard describes it: the
// codings are undone last-applied first, and if any of them is not one this decodes, the body is
// left as it arrived. The decoded size is capped, which is what stops a few kilobytes of input
// from becoming gigabytes of output.
class BodyDecoder {
 public:
  // Null when the body is to be passed on as it is.
  static std::unique_ptr<BodyDecoder> Create(const HttpResponseHead& head, uint64_t maxDecodedBytes);

  std::optional<std::string> Decode(std::span<const uint8_t> input, const DecodedSink& output);
  std::optional<std::string> Finish(const DecodedSink& output);

 private:
  BodyDecoder(std::vector<std::unique_ptr<ContentDecoder>> stages, uint64_t limit) : stages_(std::move(stages)), limit_(limit) {}

  // Runs `input` through stage `index` and the ones after it.
  std::optional<std::string> Run(size_t index, std::span<const uint8_t> input, const DecodedSink& output);
  std::optional<std::string> FinishFrom(size_t index, const DecodedSink& output);
  DecodedSink SinkAfter(size_t index, const DecodedSink& output);

  std::vector<std::unique_ptr<ContentDecoder>> stages_;  // in the order they are applied to the input
  uint64_t limit_;
  uint64_t produced_ = 0;
  std::optional<std::string> error_;  // the first failure of any stage, or the limit being hit
};

}  // namespace solar::net
