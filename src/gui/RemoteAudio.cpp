/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "RemoteAudio.h"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>

#include "solar/gui/Wire.h"
#include "solar/media/Player.h"

namespace solar::gui {

namespace {

using Clock = std::chrono::steady_clock;

struct Registry {
  std::mutex mutex;
  net::ipc::Channel* link = nullptr;
  struct State {
    int rate = 48000;
    uint64_t sent = 0;          // frames given
    uint64_t sentAtReport = 0;  // frames given when the window last said
    uint64_t reported = 0;      // frames the window then had not played
    Clock::time_point reportedAt = Clock::now();
    bool paused = false;
  };
  std::map<uint32_t, State> streams;
  std::atomic<uint32_t> next{1};
};

Registry& Get() {
  static Registry registry;
  return registry;
}

class RemoteAudioOutput final : public media::AudioOutput {
 public:
  ~RemoteAudioOutput() override {
    if (!opened_) return;
    Registry& r = Get();
    {
      std::lock_guard<std::mutex> lock(r.mutex);
      r.streams.erase(id_);
    }
    net::ipc::Writer out;
    out.U32(id_);
    Send(wire::ToBrowser::AudioClose, out);
  }

  bool Start(int sampleRate, int channels) override {
    Registry& r = Get();
    if (!r.link || sampleRate < 8000 || sampleRate > 192000 || channels < 1 || channels > 8) return false;
    id_ = r.next++;
    channels_ = channels;
    {
      std::lock_guard<std::mutex> lock(r.mutex);
      Registry::State state;
      state.rate = sampleRate;
      r.streams[id_] = state;
    }
    net::ipc::Writer out;
    out.U32(id_);
    out.I32(sampleRate);
    out.I32(channels);
    opened_ = Send(wire::ToBrowser::AudioOpen, out);
    return opened_;
  }

  void Queue(const float* samples, size_t frames) override {
    if (!opened_ || frames == 0) return;
    // (A long run is sent in pieces: a message has a largest size.)
    const size_t perPiece = (size_t{1} << 20) / (sizeof(float) * static_cast<size_t>(channels_));
    for (size_t done = 0; done < frames; done += perPiece) {
      const size_t count = std::min(perPiece, frames - done);
      net::ipc::Writer out;
      out.U32(id_);
      out.String(std::string_view(reinterpret_cast<const char*>(samples + done * channels_), count * channels_ * sizeof(float)));
      Send(wire::ToBrowser::AudioData, out);
      Registry& r = Get();
      std::lock_guard<std::mutex> lock(r.mutex);
      r.streams[id_].sent += count;
    }
  }

  size_t QueuedFrames() override {
    Registry& r = Get();
    std::lock_guard<std::mutex> lock(r.mutex);
    const auto found = r.streams.find(id_);
    if (found == r.streams.end()) return 0;
    const Registry::State& s = found->second;
    double queued = static_cast<double>(s.reported) + static_cast<double>(s.sent - s.sentAtReport);
    if (!s.paused) queued -= std::chrono::duration<double>(Clock::now() - s.reportedAt).count() * s.rate;
    return queued > 0 ? static_cast<size_t>(queued) : 0;
  }

  void Pause(bool paused) override {
    if (!opened_) return;
    {
      Registry& r = Get();
      std::lock_guard<std::mutex> lock(r.mutex);
      Registry::State& s = r.streams[id_];
      // (What has played up to now is counted before it stops.)
      if (paused && !s.paused) {
        const double played = std::chrono::duration<double>(Clock::now() - s.reportedAt).count() * s.rate;
        s.reported = s.reported + (s.sent - s.sentAtReport) > played ? static_cast<uint64_t>(static_cast<double>(s.reported + (s.sent - s.sentAtReport)) - played) : 0;
        s.sentAtReport = s.sent;
      }
      s.paused = paused;
      s.reportedAt = Clock::now();
    }
    net::ipc::Writer out;
    out.U32(id_);
    out.Bool(paused);
    Send(wire::ToBrowser::AudioPause, out);
  }

  void Clear() override {
    if (!opened_) return;
    {
      Registry& r = Get();
      std::lock_guard<std::mutex> lock(r.mutex);
      Registry::State& s = r.streams[id_];
      s.reported = 0;
      s.sentAtReport = s.sent;
      s.reportedAt = Clock::now();
    }
    net::ipc::Writer out;
    out.U32(id_);
    Send(wire::ToBrowser::AudioClear, out);
  }

 private:
  static bool Send(wire::ToBrowser type, const net::ipc::Writer& out) {
    Registry& r = Get();
    return r.link && r.link->SendRaw(static_cast<uint8_t>(type), out.bytes());
  }

  uint32_t id_ = 0;
  int channels_ = 2;
  bool opened_ = false;
};

}  // namespace

void InstallRemoteAudio(net::ipc::Channel& link) {
  Get().link = &link;
  media::SetAudioOutputFactory([]() -> std::unique_ptr<media::AudioOutput> { return std::make_unique<RemoteAudioOutput>(); });
}

void DeliverAudioQueued(uint32_t id, uint64_t frames) {
  Registry& r = Get();
  std::lock_guard<std::mutex> lock(r.mutex);
  const auto found = r.streams.find(id);
  if (found == r.streams.end()) return;
  Registry::State& s = found->second;
  s.reported = frames;
  s.sentAtReport = s.sent;
  s.reportedAt = Clock::now();
}

}  // namespace solar::gui
