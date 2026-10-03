#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "solar/net/ContentDecoder.h"
#include "support/Compress.h"

namespace {

using solar::net::BodyDecoder;
using solar::net::HttpResponseHead;
using namespace solar::test;

int total = 0;
int failed = 0;

void Check(const std::string& name, bool ok, const std::string& detail = "") {
  ++total;
  if (ok) return;
  ++failed;
  std::printf("FAIL %s %s\n", name.c_str(), detail.c_str());
}

std::span<const uint8_t> Bytes(const std::string& s, size_t from, size_t to) {
  return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(s.data()) + from, to - from);
}

struct Outcome {
  std::string output;
  bool failed = false;
  std::string error;
  bool operator==(const Outcome& o) const { return output == o.output && failed == o.failed; }
};

// Decodes `input` with `coding`, given in the pieces cut at `cuts`, then ends the stream.
Outcome Decode(const std::string& coding, const std::string& input, const std::vector<size_t>& cuts, uint64_t limit = ~uint64_t{0}) {
  HttpResponseHead head;
  head.headers.emplace_back("Content-Encoding", coding);
  auto decoder = BodyDecoder::Create(head, limit);
  Outcome outcome;
  if (!decoder) {
    outcome.failed = true;
    outcome.error = "no decoder";
    return outcome;
  }
  auto sink = [&](std::span<const uint8_t> data) {
    outcome.output.append(reinterpret_cast<const char*>(data.data()), data.size());
    return true;
  };
  size_t from = 0;
  std::vector<size_t> ends = cuts;
  ends.push_back(input.size());
  for (size_t end : ends) {
    if (auto error = decoder->Decode(Bytes(input, from, end), sink)) {
      outcome.failed = true;
      outcome.error = *error;
      return outcome;
    }
    from = end;
  }
  if (auto error = decoder->Finish(sink)) {
    outcome.failed = true;
    outcome.error = *error;
  }
  return outcome;
}

std::string Sample(size_t size, unsigned seed) {
  std::string s(size, '\0');
  unsigned x = seed;
  for (size_t i = 0; i < size; ++i) {
    x = x * 1664525u + 1013904223u;
    s[i] = static_cast<char>((x >> 24) % 7 == 0 ? (x >> 16) : 'a' + (i % 13));
  }
  return s;
}

using Encoder = std::string (*)(std::string_view);

struct Coding {
  const char* name;
  Encoder encode;
};

const Coding kCodings[] = {{"gzip", Gzip}, {"deflate", ZlibDeflate}, {"br", Brotli}, {"zstd", Zstd}};

// The output is the same whatever the cuts, and is the original.
void RoundTrips(const std::string& label, const Coding& coding, const std::string& original) {
  const std::string packed = coding.encode(original);
  const std::string name = label + " " + coding.name;
  Outcome whole = Decode(coding.name, packed, {});
  Check(name + ": whole", !whole.failed && whole.output == original, whole.error);

  if (packed.size() <= 400) {
    bool everywhere = true;
    for (size_t cut = 1; cut < packed.size(); ++cut) everywhere = everywhere && Decode(coding.name, packed, {cut}) == whole;
    Check(name + ": cut at every byte", everywhere);
  }
  std::vector<size_t> every;
  for (size_t i = 1; i < packed.size() && i < 3000; ++i) every.push_back(i);
  Check(name + ": byte by byte", Decode(coding.name, packed, every) == whole);
  std::vector<size_t> sevens;
  for (size_t i = 7; i < packed.size(); i += 7) sevens.push_back(i);
  Check(name + ": in sevens", Decode(coding.name, packed, sevens) == whole);
}

}  // namespace

int main() {
  for (const Coding& coding : kCodings) {
    RoundTrips("empty", coding, "");
    RoundTrips("one byte", coding, "x");
    RoundTrips("text", coding, "The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog.");
    RoundTrips("1 MiB", coding, Sample(1 << 20, 1));
    RoundTrips("8 MiB of zeros", coding, std::string(8 << 20, '\0'));
  }

  // Both spellings of deflate.
  {
    const std::string text = Sample(5000, 3);
    Check("deflate: zlib wrapper", Decode("deflate", ZlibDeflate(text), {}).output == text);
    Check("deflate: raw stream", Decode("deflate", RawDeflate(text), {}).output == text);
    Check("deflate: raw stream, byte by byte", [&] {
      const std::string packed = RawDeflate(text);
      std::vector<size_t> every;
      for (size_t i = 1; i < packed.size(); ++i) every.push_back(i);
      return Decode("deflate", packed, every).output == text;
    }());
  }

  // Names and case.
  {
    const std::string text = "names";
    Check("GZIP in capitals", Decode("GZIP", Gzip(text), {}).output == text);
    Check("x-gzip", Decode("x-gzip", Gzip(text), {}).output == text);
    Check("Br", Decode("Br", Brotli(text), {}).output == text);
  }

  // A chain: Content-Encoding lists codings in the order they were applied.
  {
    const std::string text = Sample(100000, 9);
    HttpResponseHead head;
    head.headers.emplace_back("Content-Encoding", "br, gzip");  // brotli first, then gzip over it
    auto decoder = BodyDecoder::Create(head, ~uint64_t{0});
    std::string out;
    const std::string packed = Gzip(Brotli(text));
    std::vector<size_t> cuts;
    for (size_t i = 11; i < packed.size(); i += 11) cuts.push_back(i);
    cuts.push_back(packed.size());
    size_t from = 0;
    bool ok = decoder != nullptr;
    auto sink = [&](std::span<const uint8_t> d) { out.append(reinterpret_cast<const char*>(d.data()), d.size()); return true; };
    for (size_t end : cuts) {
      ok = ok && !decoder->Decode(Bytes(packed, from, end), sink);
      from = end;
    }
    ok = ok && !decoder->Finish(sink);
    Check("two codings are undone in reverse", ok && out == text);

    HttpResponseHead split;
    split.headers.emplace_back("Content-Encoding", "br");
    split.headers.emplace_back("content-encoding", "gzip");
    Check("Content-Encoding in two headers is the same list", BodyDecoder::Create(split, 100) != nullptr);
  }

  // Bodies that are not decoded at all.
  {
    HttpResponseHead none;
    Check("no Content-Encoding: no decoder", BodyDecoder::Create(none, 100) == nullptr);
    HttpResponseHead identity;
    identity.headers.emplace_back("Content-Encoding", "identity");
    Check("identity passes through", BodyDecoder::Create(identity, 100) == nullptr);
    HttpResponseHead unknown;
    unknown.headers.emplace_back("Content-Encoding", "gzip, snappy");
    Check("one unknown coding leaves the whole body alone", BodyDecoder::Create(unknown, 100) == nullptr);
  }

  // Input that must be refused.
  for (const Coding& coding : kCodings) {
    const std::string text = Sample(20000, 5);
    const std::string packed = coding.encode(text);
    const std::string name = coding.name;

    Check(name + ": garbage", Decode(coding.name, std::string("this is not compressed data at all, not even close"), {}).failed);
    std::string corrupt = packed;
    for (size_t i = corrupt.size() / 2; i < corrupt.size() / 2 + 8 && i < corrupt.size(); ++i) corrupt[i] = static_cast<char>(corrupt[i] ^ 0x5A);
    Outcome bad = Decode(coding.name, corrupt, {});
    Check(name + ": corrupted in the middle", bad.failed || bad.output != text);
    Check(name + ": truncated", Decode(coding.name, packed.substr(0, packed.size() - 5), {}).failed);
    Check(name + ": cut off after the first byte", Decode(coding.name, packed.substr(0, 1), {}).failed);
  }

  // What follows a complete stream.
  {
    const std::string text = "first member";
    const std::string second = "second member";
    Outcome joined = Decode("gzip", Gzip(text) + Gzip(second), {});
    Check("gzip: members in a row are joined", !joined.failed && joined.output == text + second, joined.error);
    Outcome trailing = Decode("gzip", Gzip(text) + std::string(10, '\0'), {});
    Check("gzip: trailing padding is dropped", !trailing.failed && trailing.output == text, trailing.error);
    std::vector<size_t> every;
    const std::string both = Gzip(text) + Gzip(second);
    for (size_t i = 1; i < both.size(); ++i) every.push_back(i);
    Check("gzip: members in a row, byte by byte", Decode("gzip", both, every).output == text + second);
    Outcome frames = Decode("zstd", Zstd(text) + Zstd(second), {});
    Check("zstd: frames in a row are joined", !frames.failed && frames.output == text + second, frames.error);
    Outcome brotliTrailing = Decode("br", Brotli(text) + "junk", {});
    Check("br: bytes after the stream are dropped", !brotliTrailing.failed && brotliTrailing.output == text, brotliTrailing.error);
  }

  // A bomb: a few kilobytes that would decode to a great deal.
  for (const Coding& coding : kCodings) {
    const std::string packed = coding.encode(std::string(512 << 20, '\0'));
    const auto start = std::chrono::steady_clock::now();
    Outcome limited = Decode(coding.name, packed, {}, 4 << 20);
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    Check(std::string(coding.name) + ": a bomb is cut off at the limit", limited.failed && limited.error.find("size limit") != std::string::npos && limited.output.size() <= (4u << 20),
          limited.error + " (" + std::to_string(limited.output.size()) + " bytes)");
    Check(std::string(coding.name) + ": and not decoded to the end", elapsed < 2.0, std::to_string(elapsed) + " s for " + std::to_string(packed.size()) + " bytes");
  }
  {
    // The limit is on what comes out of the last coding, not on a stage in between.
    const std::string text(1 << 20, 'a');
    HttpResponseHead head;
    head.headers.emplace_back("Content-Encoding", "br, gzip");
    auto decoder = BodyDecoder::Create(head, 2 << 20);
    const std::string packed = Gzip(Brotli(text));
    std::string out;
    auto sink = [&](std::span<const uint8_t> d) { out.append(reinterpret_cast<const char*>(d.data()), d.size()); return true; };
    const bool ok = !decoder->Decode(Bytes(packed, 0, packed.size()), sink) && !decoder->Finish(sink);
    Check("a limit above the output does not interfere", ok && out == text);
  }
  {
    // An error in the second coding is reported as it is, not as a stop.
    HttpResponseHead head;
    head.headers.emplace_back("Content-Encoding", "br, gzip");
    auto decoder = BodyDecoder::Create(head, ~uint64_t{0});
    const std::string packed = Gzip("this is not brotli, it is just text wrapped in gzip");
    auto sink = [](std::span<const uint8_t>) { return true; };
    auto error = decoder->Decode(Bytes(packed, 0, packed.size()), sink);
    if (!error) error = decoder->Finish(sink);
    Check("an error in an inner coding keeps its message", error && error->find("invalid compressed data") != std::string::npos, error.value_or("no error"));
  }

  std::printf("content decoder: %d/%d passed\n", total - failed, total);
  return failed == 0 ? 0 : 1;
}
