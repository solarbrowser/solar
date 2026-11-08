/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/media/AudioGraph.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstring>
#include <deque>

namespace solar::media::audio {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Block {
  int channels = 0;
  float d[kMostChannels][kQuantum];
  void Clear(int ch) {
    channels = std::clamp(ch, 0, kMostChannels);
    std::memset(d, 0, sizeof d);
  }
};

// Adds `src` to `dst` as the mixing rules of the standard say (a mono one to both sides of a stereo, a stereo one to a mono as its mean, else channel for channel).
void AddMixed(Block& dst, const Block& src) {
  if (src.channels == 0) return;
  if (src.channels == dst.channels) {
    for (int c = 0; c < dst.channels; ++c) for (int i = 0; i < kQuantum; ++i) dst.d[c][i] += src.d[c][i];
  } else if (src.channels == 1 && dst.channels >= 2) {
    for (int c = 0; c < std::min(dst.channels, 2); ++c) for (int i = 0; i < kQuantum; ++i) dst.d[c][i] += src.d[0][i];
  } else if (src.channels >= 2 && dst.channels == 1) {
    for (int i = 0; i < kQuantum; ++i) dst.d[0][i] += 0.5f * (src.d[0][i] + src.d[1][i]);
  } else {
    for (int c = 0; c < std::min(dst.channels, src.channels); ++c) for (int i = 0; i < kQuantum; ++i) dst.d[c][i] += src.d[c][i];
  }
}

// ---- A parameter: a value with what is scheduled for it ----
struct Event {
  ParamOp op;
  double value, time, extra;
};

struct Param {
  double base = 0;
  std::vector<Event> events;
  std::vector<uint32_t> inputs;

  double At(double t) const {
    double v = base, previousTime = 0;
    const Event* target = nullptr;
    double targetStart = 0;
    for (const Event& e : events) {
      switch (e.op) {
        case ParamOp::SetAt:
        case ParamOp::Set:
          if (t < e.time) goto done;
          v = e.value;
          previousTime = e.time;
          target = nullptr;
          break;
        case ParamOp::LinearRampTo:
        case ParamOp::ExponentialRampTo:
          if (t < e.time) {
            const double span = e.time - previousTime;
            if (span <= 0) goto done;
            const double fraction = (t - previousTime) / span;
            double from = v;
            if (target) from = target->value + (targetStart - target->value) * std::exp(-(previousTime - target->time) / std::max(1e-9, target->extra));
            if (e.op == ParamOp::LinearRampTo) return from + (e.value - from) * fraction;
            if (from <= 0 || e.value <= 0) return from;
            return from * std::pow(e.value / from, fraction);
          }
          v = e.value;
          previousTime = e.time;
          target = nullptr;
          break;
        case ParamOp::SetTarget:
          if (t < e.time) goto done;
          targetStart = target ? (target->value + (targetStart - target->value) * std::exp(-(e.time - target->time) / std::max(1e-9, target->extra))) : v;
          target = &e;
          previousTime = e.time;
          break;
        default: break;
      }
    }
  done:
    if (target) return target->value + (targetStart - target->value) * std::exp(-(t - target->time) / std::max(1e-9, target->extra));
    return v;
  }
};

struct Edge {
  uint32_t from;
  int output, input;
};

// ---- A node ----
struct Node {
  uint32_t id = 0;
  Kind kind = Kind::Gain;
  std::vector<Param> params;
  std::vector<Edge> inputs;
  int channelCount = 0;
  uint64_t cachedQuantum = UINT64_MAX;
  bool computing = false;
  Block out;

  // sources
  std::shared_ptr<const Buffer> buffer;
  bool loop = false;
  double loopStart = 0, loopEnd = 0;
  double playPos = 0, phase = 0;
  bool started = false, playing = false, ended = false, endedReported = false;
  uint64_t startFrame = 0, stopFrame = UINT64_MAX;
  double offset = 0, duration = -1;
  int wave = 0;
  // filter
  int filterType = 0;
  double z[kMostChannels][4] = {};
  // delay
  std::vector<std::vector<float>> delayLine;
  size_t delayPos = 0;
  double maxDelay = 1;
  // analyser
  std::vector<float> ring;
  size_t ringPos = 0;
  int fftSize = 2048;
  double smoothing = 0.8;
  std::vector<float> smoothed;
  // script processor
  int bufferSize = 4096, inChannels = 2, outChannels = 2;
  std::vector<std::vector<float>> accumulating;
  size_t filled = 0;
  bool pending = false;
  std::vector<std::vector<float>> pendingInput;
  std::deque<std::vector<std::vector<float>>> outputs;
  size_t outputAt = 0;
  std::vector<std::vector<float>> building;
  // splitter and merger
  int count = 2;
  // compressor
  double envelope = 0;
};

void MakeFft(std::vector<std::complex<double>>& a) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const double angle = -2 * kPi / static_cast<double>(len);
    const std::complex<double> w(std::cos(angle), std::sin(angle));
    for (size_t i = 0; i < n; i += len) {
      std::complex<double> factor(1);
      for (size_t j = 0; j < len / 2; ++j) {
        const std::complex<double> u = a[i + j], v = a[i + j + len / 2] * factor;
        a[i + j] = u + v;
        a[i + j + len / 2] = u - v;
        factor *= w;
      }
    }
  }
}

}  // namespace

struct Engine::Impl {
  std::mutex mutex;
  std::map<uint32_t, std::unique_ptr<Node>> nodes;
  uint32_t next = 1;
  std::vector<Edge> destination;  // what is joined to the destination (from, output)
  uint64_t quantum = 0;           // the quantum that is being made (its first frame is quantum * kQuantum)
  std::atomic<uint64_t> rendered{0};
  std::atomic<uint64_t> queuedAtLast{0};
  std::vector<uint32_t> ended;
  std::unique_ptr<AudioOutput> output;
  double rate = 44100;
  int channels = 2;

  Node* Find(uint32_t id) {
    const auto found = nodes.find(id);
    return found == nodes.end() ? nullptr : found->second.get();
  }

  // The value of a parameter for each frame of the quantum (and what is joined to it added).
  void FillParam(Node& n, int index, float* out, double* mean = nullptr);
  const Block& Render(Node& n);
  void Mix(Node& n, Block& in, int forcedChannels);
  void Process(Node& n);
  void RenderQuantum(Block& destinationBlock);
};

namespace {
double Frames(double seconds, double rate) { return seconds * rate; }
}  // namespace

void Engine::Impl::FillParam(Node& n, int index, float* out, double* mean) {
  Param& p = n.params[static_cast<size_t>(index)];
  const double t0 = static_cast<double>(quantum * kQuantum) / rate;
  const double dt = 1.0 / rate;
  if (p.events.empty()) {
    for (int i = 0; i < kQuantum; ++i) out[i] = static_cast<float>(p.base);
  } else {
    for (int i = 0; i < kQuantum; ++i) out[i] = static_cast<float>(p.At(t0 + i * dt));
  }
  for (uint32_t source : p.inputs) {
    Node* from = Find(source);
    if (!from) continue;
    const Block& b = Render(*from);
    for (int i = 0; i < kQuantum; ++i) out[i] += b.channels > 1 ? 0.5f * (b.d[0][i] + b.d[1][i]) : (b.channels == 1 ? b.d[0][i] : 0.f);
  }
  if (mean) {
    double sum = 0;
    for (int i = 0; i < kQuantum; ++i) sum += out[i];
    *mean = sum / kQuantum;
  }
}

void Engine::Impl::Mix(Node& n, Block& in, int forcedChannels) {
  int channels = forcedChannels > 0 ? forcedChannels : n.channelCount;
  if (channels <= 0) {
    channels = 1;
    for (const Edge& e : n.inputs) {
      Node* from = Find(e.from);
      if (!from) continue;
      const Block& b = Render(*from);
      channels = std::max(channels, n.kind == Kind::ChannelSplitter ? b.channels : (b.channels));
    }
  }
  in.Clear(channels);
  for (const Edge& e : n.inputs) {
    Node* from = Find(e.from);
    if (!from) continue;
    const Block& b = Render(*from);
    if (from->kind == Kind::ChannelSplitter) {
      // (An output of a splitter is one of its channels.)
      Block one;
      one.Clear(1);
      if (e.output < b.channels) std::memcpy(one.d[0], b.d[e.output], sizeof one.d[0]);
      AddMixed(in, one);
    } else {
      AddMixed(in, b);
    }
  }
}

const Block& Engine::Impl::Render(Node& n) {
  if (n.cachedQuantum == quantum) return n.out;
  if (n.computing) {
    static Block silence;
    silence.Clear(1);
    return silence;  // (A cycle is a delay of a quantum, which this makes of it: silence.)
  }
  n.computing = true;
  Process(n);
  n.computing = false;
  n.cachedQuantum = quantum;
  return n.out;
}

void Engine::Impl::Process(Node& n) {
  const uint64_t first = quantum * kQuantum;
  switch (n.kind) {
    case Kind::Gain: {
      Block in;
      Mix(n, in, 0);
      float gain[kQuantum];
      FillParam(n, 0, gain);
      n.out.Clear(in.channels);
      for (int c = 0; c < in.channels; ++c) for (int i = 0; i < kQuantum; ++i) n.out.d[c][i] = in.d[c][i] * gain[i];
      break;
    }
    case Kind::ConstantSource: {
      float offset[kQuantum];
      FillParam(n, 0, offset);
      n.out.Clear(1);
      for (int i = 0; i < kQuantum; ++i) {
        const uint64_t frame = first + static_cast<uint64_t>(i);
        n.out.d[0][i] = (n.started && frame >= n.startFrame && frame < n.stopFrame) ? offset[i] : 0.f;
      }
      if (n.started && first + kQuantum > n.stopFrame && !n.ended) {
        n.ended = true;
      }
      break;
    }
    case Kind::Oscillator: {
      float frequency[kQuantum], detune[kQuantum];
      FillParam(n, 0, frequency);
      FillParam(n, 1, detune);
      n.out.Clear(1);
      for (int i = 0; i < kQuantum; ++i) {
        const uint64_t frame = first + static_cast<uint64_t>(i);
        if (!n.started || frame < n.startFrame || frame >= n.stopFrame) continue;
        const double f = frequency[i] * std::pow(2.0, detune[i] / 1200.0);
        const double p = n.phase;
        float v = 0;
        switch (n.wave) {
          case 0: v = static_cast<float>(std::sin(2 * kPi * p)); break;
          case 1: v = p < 0.5 ? 1.f : -1.f; break;
          case 2: v = static_cast<float>(2 * p - 1); break;
          default: v = static_cast<float>(p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4); break;
        }
        n.out.d[0][i] = v;
        n.phase += f / rate;
        n.phase -= std::floor(n.phase);
      }
      if (n.started && first + kQuantum > n.stopFrame) n.ended = true;
      break;
    }
    case Kind::BufferSource: {
      float rateParam[kQuantum], detune[kQuantum];
      FillParam(n, 0, rateParam);
      FillParam(n, 1, detune);
      const int channels = n.buffer ? std::min(n.buffer->channels, kMostChannels) : 1;
      n.out.Clear(channels);
      if (!n.buffer || n.buffer->frames <= 0) break;
      const Buffer& b = *n.buffer;
      const double loopEnd = (n.loopEnd > 0 && n.loopEnd * b.rate <= b.frames) ? n.loopEnd * b.rate : static_cast<double>(b.frames);
      const double loopStart = std::clamp(n.loopStart * b.rate, 0.0, loopEnd);
      for (int i = 0; i < kQuantum; ++i) {
        const uint64_t frame = first + static_cast<uint64_t>(i);
        if (!n.started || n.ended || frame < n.startFrame) continue;
        if (frame >= n.stopFrame) {
          n.ended = true;
          break;
        }
        if (!n.playing) {
          n.playing = true;
          n.playPos = n.offset * b.rate;
        }
        if (n.loop && loopEnd > loopStart && n.playPos >= loopEnd) n.playPos = loopStart + std::fmod(n.playPos - loopStart, loopEnd - loopStart);
        if (n.playPos >= b.frames) {
          n.ended = true;
          break;
        }
        const size_t at = static_cast<size_t>(n.playPos);
        const double frac = n.playPos - static_cast<double>(at);
        for (int c = 0; c < channels; ++c) {
          const std::vector<float>& d = b.data[static_cast<size_t>(c)];
          const float a0 = d[at];
          const float a1 = at + 1 < d.size() ? d[at + 1] : (n.loop ? d[static_cast<size_t>(loopStart)] : 0.f);
          n.out.d[c][i] = static_cast<float>(a0 + (a1 - a0) * frac);
        }
        n.playPos += (b.rate / rate) * rateParam[i] * std::pow(2.0, detune[i] / 1200.0);
      }
      break;
    }
    case Kind::StereoPanner: {
      Block in;
      Mix(n, in, 0);
      float pan[kQuantum];
      FillParam(n, 0, pan);
      n.out.Clear(2);
      for (int i = 0; i < kQuantum; ++i) {
        const double x = std::clamp(static_cast<double>(pan[i]), -1.0, 1.0);
        float l, r;
        if (in.channels == 1) {
          const double angle = (x + 1) * kPi / 4;
          l = in.d[0][i] * static_cast<float>(std::cos(angle));
          r = in.d[0][i] * static_cast<float>(std::sin(angle));
        } else {
          const double angle = (x <= 0 ? x + 1 : x) * kPi / 2;
          if (x <= 0) {
            l = in.d[0][i] + in.d[1][i] * static_cast<float>(std::cos(angle));
            r = in.d[1][i] * static_cast<float>(std::sin(angle));
          } else {
            l = in.d[0][i] * static_cast<float>(std::cos(angle));
            r = in.d[1][i] + in.d[0][i] * static_cast<float>(std::sin(angle));
          }
        }
        n.out.d[0][i] = l;
        n.out.d[1][i] = r;
      }
      break;
    }
    case Kind::Biquad: {
      Block in;
      Mix(n, in, 0);
      float fq[kQuantum], dt[kQuantum], qq[kQuantum], gg[kQuantum];
      FillParam(n, 0, fq);
      FillParam(n, 1, dt);
      FillParam(n, 2, qq);
      FillParam(n, 3, gg);
      const double f = std::clamp(static_cast<double>(fq[0]) * std::pow(2.0, dt[0] / 1200.0), 1.0, rate / 2 - 1);
      const double Q = std::max(1e-4, static_cast<double>(qq[0])), gainDb = gg[0];
      const double w0 = 2 * kPi * f / rate, cw = std::cos(w0), sw = std::sin(w0), A = std::pow(10.0, gainDb / 40.0);
      const double alpha = sw / (2 * Q), shelf = 2 * std::sqrt(A) * alpha;
      double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
      switch (n.filterType) {
        case 0: b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
        case 1: b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
        case 2: b0 = alpha; b1 = 0; b2 = -alpha; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
        case 3: b0 = A * ((A + 1) - (A - 1) * cw + shelf); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - shelf);
                a0 = (A + 1) + (A - 1) * cw + shelf; a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - shelf; break;
        case 4: b0 = A * ((A + 1) + (A - 1) * cw + shelf); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - shelf);
                a0 = (A + 1) - (A - 1) * cw + shelf; a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - shelf; break;
        case 5: b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A; break;
        case 6: b0 = 1; b1 = -2 * cw; b2 = 1; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
        default: b0 = 1 - alpha; b1 = -2 * cw; b2 = 1 + alpha; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
      }
      b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
      n.out.Clear(in.channels);
      for (int c = 0; c < in.channels; ++c) {
        double* z = n.z[c];
        for (int i = 0; i < kQuantum; ++i) {
          const double x = in.d[c][i];
          const double y = b0 * x + z[0];
          z[0] = b1 * x - a1 * y + z[1];
          z[1] = b2 * x - a2 * y;
          n.out.d[c][i] = static_cast<float>(y);
        }
      }
      break;
    }
    case Kind::Delay: {
      Block in;
      Mix(n, in, 0);
      float delayTime[kQuantum];
      FillParam(n, 0, delayTime);
      const size_t size = static_cast<size_t>(std::ceil(n.maxDelay * rate)) + kQuantum + 2;
      if (n.delayLine.size() != static_cast<size_t>(in.channels) || (n.delayLine.size() && n.delayLine[0].size() != size)) {
        n.delayLine.assign(static_cast<size_t>(in.channels), std::vector<float>(size, 0.f));
        n.delayPos = 0;
      }
      n.out.Clear(in.channels);
      for (int i = 0; i < kQuantum; ++i) {
        const double d = std::clamp(static_cast<double>(delayTime[i]) * rate, 0.0, static_cast<double>(size - kQuantum - 2));
        for (int c = 0; c < in.channels; ++c) {
          n.delayLine[static_cast<size_t>(c)][n.delayPos] = in.d[c][i];
          const double readAt = static_cast<double>(n.delayPos) - d;
          const double wrapped = readAt < 0 ? readAt + static_cast<double>(size) : readAt;
          const size_t i0 = static_cast<size_t>(wrapped) % size, i1 = (i0 + 1) % size;
          const double frac = wrapped - std::floor(wrapped);
          const std::vector<float>& line = n.delayLine[static_cast<size_t>(c)];
          n.out.d[c][i] = static_cast<float>(line[i0] + (line[i1] - line[i0]) * frac);
        }
        n.delayPos = (n.delayPos + 1) % size;
      }
      break;
    }
    case Kind::Analyser: {
      Block in;
      Mix(n, in, 0);
      n.out = in;
      if (n.ring.empty()) n.ring.assign(32768, 0.f);
      for (int i = 0; i < kQuantum; ++i) {
        float mono = 0;
        for (int c = 0; c < in.channels; ++c) mono += in.d[c][i];
        n.ring[n.ringPos] = in.channels ? mono / static_cast<float>(in.channels) : 0.f;
        n.ringPos = (n.ringPos + 1) % n.ring.size();
      }
      break;
    }
    case Kind::ChannelSplitter: {
      Block in;
      Mix(n, in, std::clamp(n.count, 1, kMostChannels));
      n.out = in;
      break;
    }
    case Kind::ChannelMerger: {
      const int count = std::clamp(n.count, 1, kMostChannels);
      n.out.Clear(count);
      for (const Edge& e : n.inputs) {
        Node* from = Find(e.from);
        if (!from || e.input < 0 || e.input >= count) continue;
        const Block& b = Render(*from);
        Block mono;
        mono.Clear(1);
        if (from->kind == Kind::ChannelSplitter) {
          if (e.output < b.channels) std::memcpy(mono.d[0], b.d[e.output], sizeof mono.d[0]);
        } else {
          Block one;
          one.Clear(1);
          AddMixed(one, b);
          std::memcpy(mono.d[0], one.d[0], sizeof mono.d[0]);
        }
        for (int i = 0; i < kQuantum; ++i) n.out.d[e.input][i] += mono.d[0][i];
      }
      break;
    }
    case Kind::Compressor: {
      Block in;
      Mix(n, in, 0);
      float threshold, knee, ratio, attack, release;
      float tmp[kQuantum];
      FillParam(n, 0, tmp); threshold = tmp[0];
      FillParam(n, 1, tmp); knee = tmp[0];
      FillParam(n, 2, tmp); ratio = std::max(1.f, tmp[0]);
      FillParam(n, 3, tmp); attack = std::max(1e-4f, tmp[0]);
      FillParam(n, 4, tmp); release = std::max(1e-4f, tmp[0]);
      n.out.Clear(in.channels);
      const double a = std::exp(-1.0 / (attack * rate)), r = std::exp(-1.0 / (release * rate));
      for (int i = 0; i < kQuantum; ++i) {
        float peak = 0;
        for (int c = 0; c < in.channels; ++c) peak = std::max(peak, std::fabs(in.d[c][i]));
        const double level = peak > 1e-9f ? 20 * std::log10(static_cast<double>(peak)) : -180;
        n.envelope = level > n.envelope ? a * n.envelope + (1 - a) * level : r * n.envelope + (1 - r) * level;
        double over = n.envelope - threshold, reduction = 0;
        if (2 * over > knee) reduction = over - over / ratio;
        else if (2 * over > -knee) { const double x = over + knee / 2; reduction = (1 - 1 / ratio) * x * x / (2 * std::max(1e-6, static_cast<double>(knee))); }
        const float gain = static_cast<float>(std::pow(10.0, -reduction / 20));
        for (int c = 0; c < in.channels; ++c) n.out.d[c][i] = in.d[c][i] * gain;
      }
      break;
    }
    case Kind::ScriptProcessor: {
      Block in;
      Mix(n, in, std::clamp(n.inChannels, 1, kMostChannels));
      const size_t size = static_cast<size_t>(n.bufferSize);
      if (n.accumulating.size() != static_cast<size_t>(in.channels) || (n.accumulating.size() && n.accumulating[0].size() != size)) {
        n.accumulating.assign(static_cast<size_t>(in.channels), std::vector<float>(size, 0.f));
        n.filled = 0;
      }
      for (int c = 0; c < in.channels; ++c) std::memcpy(n.accumulating[static_cast<size_t>(c)].data() + n.filled, in.d[c], sizeof(float) * kQuantum);
      n.filled += kQuantum;
      if (n.filled >= size) {
        // (A buffer the page has not taken is replaced by the newer: it is late.)
        n.pendingInput = n.accumulating;
        n.pending = true;
        n.filled = 0;
      }
      const int channels = std::clamp(n.outChannels, 1, kMostChannels);
      n.out.Clear(channels);
      if (!n.outputs.empty()) {
        const std::vector<std::vector<float>>& current = n.outputs.front();
        for (int c = 0; c < channels && c < static_cast<int>(current.size()); ++c) {
          for (int i = 0; i < kQuantum && n.outputAt + static_cast<size_t>(i) < current[static_cast<size_t>(c)].size(); ++i) n.out.d[c][i] = current[static_cast<size_t>(c)][n.outputAt + static_cast<size_t>(i)];
        }
        n.outputAt += kQuantum;
        if (n.outputAt >= size) {
          n.outputs.pop_front();
          n.outputAt = 0;
        }
      }
      break;
    }
  }
  if ((n.kind == Kind::BufferSource || n.kind == Kind::Oscillator || n.kind == Kind::ConstantSource) && n.ended && !n.endedReported) {
    n.endedReported = true;
    ended.push_back(n.id);
  }
}

void Engine::Impl::RenderQuantum(Block& destinationBlock) {
  destinationBlock.Clear(channels);
  Node fake;  // (the destination: a mixer of what is joined to it)
  for (const Edge& e : destination) {
    Node* from = Find(e.from);
    if (!from) continue;
    const Block& b = Render(*from);
    if (from->kind == Kind::ChannelSplitter) {
      Block one;
      one.Clear(1);
      if (e.output < b.channels) std::memcpy(one.d[0], b.d[e.output], sizeof one.d[0]);
      AddMixed(destinationBlock, one);
    } else {
      AddMixed(destinationBlock, b);
    }
  }
  // (Sound that is too loud is cut at the edge of what can be played.)
  for (int c = 0; c < destinationBlock.channels; ++c) for (int i = 0; i < kQuantum; ++i) destinationBlock.d[c][i] = std::clamp(destinationBlock.d[c][i], -1.f, 1.f);
  // (Nodes that nothing is joined to still run: a script processor has to hear what it is given, an analyser what it is shown; a source that is not heard still plays on.)
  for (auto& [id, node] : nodes) {
    if (node->cachedQuantum != quantum && (node->kind == Kind::ScriptProcessor || node->kind == Kind::Analyser || node->kind == Kind::BufferSource || node->kind == Kind::Oscillator || node->kind == Kind::ConstantSource)) Render(*node);
  }
  ++quantum;
  rendered.fetch_add(kQuantum);
}

// ---- The engine ----

Engine::Engine(double rate, int channels, bool realtime) : rate_(rate), channels_(channels), realtime_(realtime), impl_(std::make_unique<Impl>()) {
  impl_->rate = rate;
  impl_->channels = channels;
}

std::shared_ptr<Engine> Engine::Create(double sampleRate, int channels, bool realtime) {
  std::shared_ptr<Engine> engine(new Engine(std::clamp(sampleRate, 3000.0, 768000.0), std::clamp(channels, 1, kMostChannels), realtime));
  if (realtime) engine->thread_ = std::thread([raw = engine.get()] { raw->Loop(); });
  return engine;
}

Engine::~Engine() { Close(); }

void Engine::Start() { running_ = true; }
void Engine::Suspend() { running_ = false; }

void Engine::Close() {
  stop_ = true;
  running_ = false;
  if (thread_.joinable()) thread_.join();
}

double Engine::currentTime() {
  const uint64_t rendered = impl_->rendered.load();
  if (!realtime_) return static_cast<double>(rendered) / rate_;
  const uint64_t queued = impl_->queuedAtLast.load();
  return rendered > queued ? static_cast<double>(rendered - queued) / rate_ : 0.0;
}

void Engine::Loop() {
  std::unique_ptr<AudioOutput> output;
  bool opened = false;
  auto lastTick = std::chrono::steady_clock::now();
  double owed = 0;  // (frames that time has passed and that are to be made, when there is no output to say how many)
  std::vector<float> interleaved;
  while (!stop_) {
    if (!running_) {
      if (output && opened) output->Pause(true);
      opened = false;
      std::this_thread::sleep_for(std::chrono::milliseconds(4));
      lastTick = std::chrono::steady_clock::now();
      continue;
    }
    if (!output) {
      output = MakeAudioOutput();
      if (output && !output->Start(static_cast<int>(rate_), channels_)) output.reset();
    }
    if (output && !opened) {
      output->Pause(false);
      opened = true;
    }
    size_t wanted = 0;
    const size_t target = static_cast<size_t>(rate_ * ahead_);
    if (output) {
      const size_t queued = output->QueuedFrames();
      impl_->queuedAtLast = queued;
      if (queued < target) wanted = target - queued;
    } else {
      const auto now = std::chrono::steady_clock::now();
      owed += std::chrono::duration<double>(now - lastTick).count() * rate_;
      lastTick = now;
      wanted = static_cast<size_t>(owed);
      impl_->queuedAtLast = 0;
    }
    if (wanted >= kQuantum) {
      const size_t quanta = std::min<size_t>(wanted / kQuantum, 16);
      interleaved.assign(quanta * kQuantum * static_cast<size_t>(channels_), 0.f);
      {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        Block block;
        for (size_t q = 0; q < quanta; ++q) {
          impl_->RenderQuantum(block);
          for (int i = 0; i < kQuantum; ++i) for (int c = 0; c < channels_; ++c) interleaved[(q * kQuantum + static_cast<size_t>(i)) * static_cast<size_t>(channels_) + static_cast<size_t>(c)] = block.d[c][i];
        }
      }
      if (output) output->Queue(interleaved.data(), quanta * kQuantum);
      else owed -= static_cast<double>(quanta * kQuantum);
      continue;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

Buffer Engine::RenderOffline(int frames) {
  Buffer out;
  out.channels = channels_;
  out.rate = rate_;
  out.frames = std::max(0, frames);
  out.data.assign(static_cast<size_t>(channels_), std::vector<float>(static_cast<size_t>(out.frames), 0.f));
  std::lock_guard<std::mutex> lock(impl_->mutex);
  Block block;
  for (int done = 0; done < out.frames; done += kQuantum) {
    impl_->RenderQuantum(block);
    const int n = std::min(kQuantum, out.frames - done);
    for (int c = 0; c < channels_; ++c) std::memcpy(out.data[static_cast<size_t>(c)].data() + done, block.d[c], sizeof(float) * static_cast<size_t>(n));
  }
  return out;
}

uint32_t Engine::CreateNode(Kind kind, int a, int b, int c) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto node = std::make_unique<Node>();
  node->id = impl_->next++;
  node->kind = kind;
  const auto param = [&](double base) { Param p; p.base = base; node->params.push_back(p); };
  switch (kind) {
    case Kind::Gain: param(1); break;
    case Kind::BufferSource: param(1); param(0); break;
    case Kind::Oscillator: param(440); param(0); break;
    case Kind::StereoPanner: param(0); break;
    case Kind::Biquad: param(350); param(0); param(1); param(0); break;
    case Kind::Delay: param(0); node->maxDelay = std::clamp(static_cast<double>(a) / 1000.0, 0.001, 180.0); break;
    case Kind::ConstantSource: param(1); break;
    case Kind::Compressor: param(-24); param(30); param(12); param(0.003); param(0.25); break;
    case Kind::ScriptProcessor:
      node->bufferSize = a > 0 ? a : 4096;
      node->inChannels = std::clamp(b, 1, kMostChannels);
      node->outChannels = std::clamp(c, 1, kMostChannels);
      break;
    case Kind::ChannelSplitter:
    case Kind::ChannelMerger: node->count = std::clamp(a, 1, kMostChannels); break;
    case Kind::Analyser: node->smoothed.assign(16384, 0.f); break;
  }
  const uint32_t id = node->id;
  impl_->nodes[id] = std::move(node);
  return id;
}

void Engine::DeleteNode(uint32_t id) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->nodes.erase(id);
  for (auto& [other, node] : impl_->nodes) {
    node->inputs.erase(std::remove_if(node->inputs.begin(), node->inputs.end(), [&](const Edge& e) { return e.from == id; }), node->inputs.end());
    for (Param& p : node->params) p.inputs.erase(std::remove(p.inputs.begin(), p.inputs.end(), id), p.inputs.end());
  }
  impl_->destination.erase(std::remove_if(impl_->destination.begin(), impl_->destination.end(), [&](const Edge& e) { return e.from == id; }), impl_->destination.end());
}

void Engine::Connect(uint32_t from, uint32_t to, int output, int input) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  Node* target = impl_->Find(to);
  if (!target || !impl_->Find(from)) return;
  for (const Edge& e : target->inputs) if (e.from == from && e.output == output && e.input == input) return;
  target->inputs.push_back({from, output, input});
}

void Engine::ConnectParam(uint32_t from, uint32_t to, int param) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  Node* target = impl_->Find(to);
  if (!target || !impl_->Find(from) || param < 0 || static_cast<size_t>(param) >= target->params.size()) return;
  auto& inputs = target->params[static_cast<size_t>(param)].inputs;
  if (std::find(inputs.begin(), inputs.end(), from) == inputs.end()) inputs.push_back(from);
}

void Engine::ConnectDestination(uint32_t from) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  for (const Edge& e : impl_->destination) if (e.from == from) return;
  if (impl_->Find(from)) impl_->destination.push_back({from, 0, 0});
}

void Engine::Disconnect(uint32_t from, uint32_t to) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto drop = [&](std::vector<Edge>& edges) { edges.erase(std::remove_if(edges.begin(), edges.end(), [&](const Edge& e) { return e.from == from; }), edges.end()); };
  if (to == 0) {
    for (auto& [id, node] : impl_->nodes) {
      drop(node->inputs);
      for (Param& p : node->params) p.inputs.erase(std::remove(p.inputs.begin(), p.inputs.end(), from), p.inputs.end());
    }
    drop(impl_->destination);
  } else if (to == UINT32_MAX) {
    drop(impl_->destination);
  } else if (Node* target = impl_->Find(to)) {
    drop(target->inputs);
    for (Param& p : target->params) p.inputs.erase(std::remove(p.inputs.begin(), p.inputs.end(), from), p.inputs.end());
  }
}

void Engine::SetParam(uint32_t id, int param, ParamOp op, double value, double time, double extra) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  Node* n = impl_->Find(id);
  if (!n || param < 0 || static_cast<size_t>(param) >= n->params.size()) return;
  Param& p = n->params[static_cast<size_t>(param)];
  if (op == ParamOp::Cancel) {
    p.events.erase(std::remove_if(p.events.begin(), p.events.end(), [&](const Event& e) { return e.time >= time; }), p.events.end());
    return;
  }
  if (op == ParamOp::CancelAndHold) {
    const double held = p.At(time);
    p.events.erase(std::remove_if(p.events.begin(), p.events.end(), [&](const Event& e) { return e.time >= time; }), p.events.end());
    p.events.push_back({ParamOp::SetAt, held, time, 0});
    return;
  }
  if (op == ParamOp::Set) {
    // (The value is set at once: what was scheduled to come before is over, what is to come stays.)
    p.base = value;
    p.events.erase(std::remove_if(p.events.begin(), p.events.end(), [&](const Event& e) { return e.time <= time; }), p.events.end());
    return;
  }
  Event e{op, value, time, extra};
  auto at = std::upper_bound(p.events.begin(), p.events.end(), e, [](const Event& a, const Event& b) { return a.time < b.time; });
  p.events.insert(at, e);
}

double Engine::GetParam(uint32_t id, int param) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  Node* n = impl_->Find(id);
  if (!n || param < 0 || static_cast<size_t>(param) >= n->params.size()) return 0;
  return n->params[static_cast<size_t>(param)].At(static_cast<double>(impl_->quantum * kQuantum) / rate_);
}

void Engine::Set(uint32_t id, Setting setting, double value) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  Node* n = impl_->Find(id);
  if (!n) return;
  switch (setting) {
    case Setting::WaveType: n->wave = static_cast<int>(value); break;
    case Setting::FilterType: n->filterType = static_cast<int>(value); break;
    case Setting::Loop: n->loop = value != 0; break;
    case Setting::LoopStart: n->loopStart = value; break;
    case Setting::LoopEnd: n->loopEnd = value; break;
    case Setting::FftSize: n->fftSize = std::clamp(static_cast<int>(value), 32, 32768); break;
    case Setting::SmoothingTimeConstant: n->smoothing = std::clamp(value, 0.0, 1.0); break;
    case Setting::ChannelCount: n->channelCount = std::clamp(static_cast<int>(value), 0, kMostChannels); break;
  }
}

void Engine::SetBuffer(uint32_t id, std::shared_ptr<const Buffer> buffer) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (Node* n = impl_->Find(id)) n->buffer = std::move(buffer);
}

void Engine::StartSource(uint32_t id, double when, double offset, double duration) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  Node* n = impl_->Find(id);
  if (!n || n->started) return;
  n->started = true;
  n->startFrame = static_cast<uint64_t>(std::max(0.0, Frames(when, rate_)) + 0.5);
  n->offset = std::max(0.0, offset);
  n->duration = duration;
  if (duration >= 0) n->stopFrame = n->startFrame + static_cast<uint64_t>(Frames(duration, rate_));
}

void Engine::StopSource(uint32_t id, double when) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  Node* n = impl_->Find(id);
  if (!n || !n->started) return;
  n->stopFrame = std::min(n->stopFrame, static_cast<uint64_t>(std::max(0.0, Frames(when, rate_)) + 0.5));
}

std::vector<uint32_t> Engine::TakeEnded() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<uint32_t> out;
  out.swap(impl_->ended);
  return out;
}

std::vector<uint32_t> Engine::PendingProcessors() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<uint32_t> out;
  for (auto& [id, node] : impl_->nodes) if (node->kind == Kind::ScriptProcessor && node->pending) out.push_back(id);
  return out;
}

bool Engine::ProcessorInput(uint32_t id, int channel, float* out, size_t count) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  Node* n = impl_->Find(id);
  if (!n || channel < 0 || static_cast<size_t>(channel) >= n->pendingInput.size()) return false;
  const std::vector<float>& d = n->pendingInput[static_cast<size_t>(channel)];
  std::memcpy(out, d.data(), sizeof(float) * std::min(count, d.size()));
  return true;
}

void Engine::ProcessorOutput(uint32_t id, int channel, const float* in, size_t count) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  Node* n = impl_->Find(id);
  if (!n || channel < 0 || channel >= n->outChannels) return;
  if (n->building.size() != static_cast<size_t>(n->outChannels)) n->building.assign(static_cast<size_t>(n->outChannels), std::vector<float>(static_cast<size_t>(n->bufferSize), 0.f));
  std::vector<float>& d = n->building[static_cast<size_t>(channel)];
  std::memcpy(d.data(), in, sizeof(float) * std::min(count, d.size()));
}

void Engine::ProcessorDone(uint32_t id) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  Node* n = impl_->Find(id);
  if (!n) return;
  n->pending = false;
  if (n->building.size() == static_cast<size_t>(n->outChannels)) {
    n->outputs.push_back(std::move(n->building));
    n->building.clear();
    // (More than a few buffers ahead is more than anyone listens to: the oldest go.)
    while (n->outputs.size() > 6) n->outputs.pop_front();
  }
}

void Engine::AnalyserTimeData(uint32_t id, float* out, size_t count) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  Node* n = impl_->Find(id);
  if (!n || n->ring.empty()) {
    std::fill(out, out + count, 0.f);
    return;
  }
  for (size_t i = 0; i < count; ++i) {
    const size_t back = count - i;
    n->ringPos >= back ? out[i] = n->ring[n->ringPos - back] : out[i] = n->ring[n->ring.size() + n->ringPos - back];
  }
}

void Engine::AnalyserFrequencyData(uint32_t id, float* out, size_t count) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  Node* n = impl_->Find(id);
  if (!n || n->ring.empty()) {
    std::fill(out, out + count, -100.f);
    return;
  }
  const size_t size = static_cast<size_t>(n->fftSize);
  std::vector<std::complex<double>> buffer(size);
  for (size_t i = 0; i < size; ++i) {
    const size_t back = size - i;
    const float sample = n->ringPos >= back ? n->ring[n->ringPos - back] : n->ring[n->ring.size() + n->ringPos - back];
    const double window = 0.42 - 0.5 * std::cos(2 * kPi * static_cast<double>(i) / static_cast<double>(size)) + 0.08 * std::cos(4 * kPi * static_cast<double>(i) / static_cast<double>(size));
    buffer[i] = sample * window;
  }
  MakeFft(buffer);
  if (n->smoothed.size() < size / 2) n->smoothed.assign(size / 2, 0.f);
  for (size_t k = 0; k < count && k < size / 2; ++k) {
    const double magnitude = std::abs(buffer[k]) / static_cast<double>(size);
    n->smoothed[k] = static_cast<float>(n->smoothing * n->smoothed[k] + (1 - n->smoothing) * magnitude);
    out[k] = n->smoothed[k] > 1e-12f ? static_cast<float>(20 * std::log10(static_cast<double>(n->smoothed[k]))) : -240.f;
  }
}

}  // namespace solar::media::audio
