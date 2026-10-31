/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/media/Player.h"

#include "FfmpegApi.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cmath>
#include <optional>
#include <thread>

namespace solar::media {

namespace {

using Clock = std::chrono::steady_clock;

AudioOutputFactory& Factory() {
  static AudioOutputFactory factory;
  return factory;
}
std::atomic<int> g_playing{0};
std::atomic<uint64_t> g_frames{0};
constexpr int kOutRate = 48000;  // what the audio output is opened with (a playback rate other than 1 changes the rate the sound is resampled to)

}  // namespace

void SetAudioOutputFactory(AudioOutputFactory factory) { Factory() = std::move(factory); }
int PlayingCount() { return g_playing.load(); }
uint64_t FrameCounter() { return g_frames.load(); }

// ---- ByteStream ----

void ByteStream::Append(const uint8_t* data, size_t size) {
  {
    std::lock_guard lock(mutex_);
    bytes_.append(reinterpret_cast<const char*>(data), size);
  }
  cv_.notify_all();
}

void ByteStream::Finish(bool failed) {
  {
    std::lock_guard lock(mutex_);
    finished_ = true;
    failed_ = failed;
  }
  cv_.notify_all();
}

void ByteStream::SetTotal(uint64_t total) {
  std::lock_guard lock(mutex_);
  total_ = total;
}

void ByteStream::Poke() { cv_.notify_all(); }

int64_t ByteStream::Read(uint64_t offset, uint8_t* out, size_t size, const std::function<bool()>& interrupted, int timeoutMs) {
  std::unique_lock lock(mutex_);
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
  for (;;) {
    if (offset < bytes_.size()) {
      const size_t n = std::min<uint64_t>(size, bytes_.size() - offset);
      std::memcpy(out, bytes_.data() + offset, n);
      return static_cast<int64_t>(n);
    }
    if (finished_) return failed_ ? -3 : 0;
    if (interrupted && interrupted()) return -1;
    if (cv_.wait_until(lock, deadline) == std::cv_status::timeout && offset >= bytes_.size() && !finished_) return -2;
  }
}

int64_t ByteStream::Size() const {
  std::lock_guard lock(mutex_);
  if (finished_) return static_cast<int64_t>(bytes_.size());
  return total_ ? static_cast<int64_t>(total_) : -1;
}

bool ByteStream::WaitForEnd(const std::function<bool()>& interrupted) {
  std::unique_lock lock(mutex_);
  while (!finished_) {
    if (interrupted && interrupted()) return false;
    cv_.wait_for(lock, std::chrono::milliseconds(50));
  }
  return true;
}

uint64_t ByteStream::available() const { std::lock_guard lock(mutex_); return bytes_.size(); }
uint64_t ByteStream::total() const { std::lock_guard lock(mutex_); return finished_ ? bytes_.size() : total_; }
bool ByteStream::finished() const { std::lock_guard lock(mutex_); return finished_; }
bool ByteStream::failed() const { std::lock_guard lock(mutex_); return failed_; }

// ---- Player ----

struct Player::Impl {
  Player* owner = nullptr;
  std::shared_ptr<ByteStream> stream;
  std::function<void()> changed;
  std::thread thread;

  std::mutex m;
  std::condition_variable cv;
  bool stop = false;
  bool wantPlay = false;
  struct SeekRequest {
    double time = 0;
    bool silent = false;
  };
  std::optional<SeekRequest> seekTo;
  double volume = 1;
  bool muted = false;
  double rate = 1;
  bool loop = false;

  PlayerState st;
  std::vector<std::string> events;

  // The clock.
  double pausedTime = 0;
  bool clockRunning = false;
  Clock::time_point wallStart;
  double wallBase = 0;
  bool audioClock = false;
  double audioEnd = 0;  // the media time at the end of what has been given to the audio output
  std::unique_ptr<AudioOutput> audio;

  std::deque<std::shared_ptr<VideoFrame>> queue;
  std::shared_ptr<VideoFrame> current;
  uint64_t serial = 0;
  size_t maxFrames = 12;
  bool eof = false;
  double decodedEnd = 0;  // how far the decoding has got (media time)

  // The demuxer and the decoders (the thread's alone).
  AVFormatContext* fmt = nullptr;
  AVIOContext* io = nullptr;
  AVCodecContext* vctx = nullptr;
  AVCodecContext* actx = nullptr;
  int vIndex = -1, aIndex = -1;
  SwsContext* sws = nullptr;
  SwrContext* swr = nullptr;
  int swrInRate = 0, swrInFormat = -1, swrInChannels = 0, swrOutRate = 0;
  uint64_t ioPos = 0;
  double sar = 1;

  ~Impl() = default;

  void Notify() {
    if (changed) changed();
  }
  void Event(const char* name) {  // (with the lock held)
    events.emplace_back(name);
  }

  // ---- Clock (with the lock held) ----
  double ClockLocked() {
    if (!clockRunning) return pausedTime;
    if (audioClock && audio) return std::max(pausedTime, audioEnd - static_cast<double>(audio->QueuedFrames()) * rate / kOutRate);
    return wallBase + std::chrono::duration<double>(Clock::now() - wallStart).count() * rate;
  }
  void UpdateClockLocked() {
    const bool should = wantPlay && !st.seeking && st.readyState >= kHaveCurrentData && !st.stalled && !st.ended;
    if (should && !clockRunning) {
      clockRunning = true;
      wallStart = Clock::now();
      wallBase = pausedTime;
      if (audio) audio->Pause(false);
    } else if (!should && clockRunning) {
      pausedTime = ClockLocked();
      clockRunning = false;
      if (audio) audio->Pause(true);
    }
    const bool playing = wantPlay && clockRunning;
    if (playing != owner->playing_.load()) {
      owner->playing_.store(playing);
      g_playing += playing ? 1 : -1;
    }
  }

  void SetReadyLocked(int state) {
    if (state == st.readyState) return;
    const int before = st.readyState;
    st.readyState = state;
    if (before < kHaveMetadata && state >= kHaveMetadata) Event("loadedmetadata");
    if (before < kHaveCurrentData && state >= kHaveCurrentData) Event("loadeddata");
    if (before < kHaveFutureData && state >= kHaveFutureData) Event("canplay");
    if (before < kHaveEnoughData && state >= kHaveEnoughData) Event("canplaythrough");
  }

  void Fail(int code, const std::string& message) {
    std::lock_guard lock(m);
    st.error = code;
    st.errorMessage = message;
    Event("error");
    UpdateClockLocked();
    Notify();
  }

  // ---- Input ----
  bool Interrupted() {
    std::lock_guard lock(m);
    return stop || seekTo.has_value();
  }
  static int ReadCb(void* opaque, uint8_t* buffer, int size) {
    auto* self = static_cast<Impl*>(opaque);
    for (;;) {
      const int64_t n = self->stream->Read(self->ioPos, buffer, static_cast<size_t>(size), [self] { return self->Interrupted(); }, 150);
      if (n > 0) {
        self->ioPos += static_cast<uint64_t>(n);
        std::lock_guard lock(self->m);
        if (self->st.stalled) {
          self->st.stalled = false;
          self->UpdateClockLocked();
        }
        return static_cast<int>(n);
      }
      if (n == 0) return AVERROR_EOF;
      if (n == -1) return AVERROR_EXIT;
      if (n == -3) return AVERROR(EIO);
      // Waiting for the network: said once it has gone on for a while.
      bool tell = false;
      {
        std::lock_guard lock(self->m);
        if (self->stop) return AVERROR_EXIT;
        if (!self->st.stalled && self->st.readyState >= kHaveCurrentData) {
          self->st.stalled = true;
          self->SetReadyLockedStalled();
          tell = true;
        }
      }
      if (tell) self->Notify();
    }
  }
  void SetReadyLockedStalled() {  // (with the lock held)
    if (st.readyState > kHaveCurrentData) st.readyState = kHaveCurrentData;
    Event("waiting");
    UpdateClockLocked();
  }
  static int64_t SeekCb(void* opaque, int64_t offset, int whence) {
    auto* self = static_cast<Impl*>(opaque);
    if (whence == AVSEEK_SIZE) return self->stream->Size();
    int64_t target;
    switch (whence & ~AVSEEK_FORCE) {
      case SEEK_SET: target = offset; break;
      case SEEK_CUR: target = static_cast<int64_t>(self->ioPos) + offset; break;
      case SEEK_END: {
        if (!self->stream->WaitForEnd([self] { return self->Interrupted(); })) return AVERROR_EXIT;
        target = self->stream->Size() + offset;
        break;
      }
      default: return -1;
    }
    if (target < 0) return -1;
    self->ioPos = static_cast<uint64_t>(target);
    return target;
  }
  static int InterruptCb(void* opaque) { return static_cast<Impl*>(opaque)->Interrupted() ? 1 : 0; }

  // ---- The thread ----
  void Close() {
    if (sws) g_api.sws_freeContext(sws);
    if (swr) g_api.swr_free(&swr);
    if (vctx) g_api.avcodec_free_context(&vctx);
    if (actx) g_api.avcodec_free_context(&actx);
    if (fmt) g_api.avformat_close_input(&fmt);
    if (io) {
      g_api.av_freep(&io->buffer);
      g_api.avio_context_free(&io);
    }
  }

  bool Open() {
    fmt = g_api.avformat_alloc_context();
    if (!fmt) return false;
    constexpr int kBuffer = 1 << 16;
    uint8_t* buffer = static_cast<uint8_t*>(g_api.av_malloc(kBuffer));
    io = g_api.avio_alloc_context(buffer, kBuffer, 0, this, &ReadCb, nullptr, &SeekCb);
    if (!io) return false;
    fmt->pb = io;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    fmt->interrupt_callback.callback = &InterruptCb;
    fmt->interrupt_callback.opaque = this;
    if (g_api.avformat_open_input(&fmt, nullptr, nullptr, nullptr) < 0) return false;
    if (g_api.avformat_find_stream_info(fmt, nullptr) < 0) return false;
    vIndex = g_api.av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    aIndex = g_api.av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (vIndex < 0 && aIndex < 0) return false;
    auto openCodec = [&](int index, AVCodecContext*& out) {
      AVStream* stream = fmt->streams[index];
      const AVCodec* codec = g_api.avcodec_find_decoder(stream->codecpar->codec_id);
      if (!codec) return false;
      out = g_api.avcodec_alloc_context3(codec);
      if (!out || g_api.avcodec_parameters_to_context(out, stream->codecpar) < 0) return false;
      out->pkt_timebase = stream->time_base;
      out->thread_count = 0;
      out->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
      return g_api.avcodec_open2(out, codec, nullptr) >= 0;
    };
    if (vIndex >= 0 && !openCodec(vIndex, vctx)) vIndex = -1;
    if (aIndex >= 0 && !openCodec(aIndex, actx)) aIndex = -1;
    if (vIndex < 0 && aIndex < 0) return false;
    // (Streams that are not played are not read.)
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
      if (static_cast<int>(i) != vIndex && static_cast<int>(i) != aIndex) fmt->streams[i]->discard = AVDISCARD_ALL;
    }
    return true;
  }

  void PublishMetadata() {
    std::lock_guard lock(m);
    double duration = std::nan("");
    if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0) duration = static_cast<double>(fmt->duration) / AV_TIME_BASE;
    st.duration = duration;
    st.hasVideo = vIndex >= 0;
    st.hasAudio = aIndex >= 0;
    if (vIndex >= 0) {
      const AVRational ratio = g_api.av_guess_sample_aspect_ratio(fmt, fmt->streams[vIndex], nullptr);
      sar = ratio.num > 0 && ratio.den > 0 ? static_cast<double>(ratio.num) / ratio.den : 1;
      st.videoWidth = static_cast<int>(std::lround(vctx->width * sar));
      st.videoHeight = vctx->height;
      const size_t frameBytes = static_cast<size_t>(std::max(1, vctx->width)) * std::max(1, vctx->height) * 4;
      maxFrames = std::clamp<size_t>((size_t{96} << 20) / frameBytes, 3, 16);
    }
    if (aIndex >= 0 && Factory()) {
      audio = Factory()();
      if (audio && !audio->Start(kOutRate, 2)) audio.reset();
      if (audio) {
        audioClock = true;
        audio->Pause(true);
      }
    }
    Event("durationchange");
    SetReadyLocked(kHaveMetadata);
    Notify();
  }

  double SecondsOf(int64_t ts, const AVStream* stream) const {
    if (ts == AV_NOPTS_VALUE) return std::nan("");
    return static_cast<double>(ts) * av_q2d(stream->time_base);
  }

  // Whether the thread should wait before decoding more: what it has decoded is as much as it keeps.
  bool Full() {
    if (vIndex >= 0 && queue.size() >= maxFrames) return true;
    if (aIndex >= 0 && audio && static_cast<double>(audio->QueuedFrames()) > kOutRate * 0.6) return true;
    return false;
  }

  // ---- Seeking ----
  struct Seeking {
    bool active = false;
    double target = 0;
    bool silent = false;
    AVFrame* candidate = nullptr;  // the last picture at or before the target
    double candidatePts = 0;
    bool audioDone = false;
  } seeking;

  void BeginSeek(SeekRequest request) {
    double target = std::max(0.0, request.time);
    {
      std::lock_guard lock(m);
      if (!std::isnan(st.duration)) target = std::min(target, st.duration);
      if (!request.silent) {
        st.seeking = true;
        Event("seeking");
      }
      st.ended = false;
      queue.clear();
      current.reset();
      eof = false;
      pausedTime = target;
      audioEnd = target;
      if (audio) audio->Clear();
      if (st.readyState > kHaveMetadata) st.readyState = kHaveMetadata;
      UpdateClockLocked();
    }
    if (seeking.candidate) g_api.av_frame_free(&seeking.candidate);
    seeking.active = true;
    seeking.target = target;
    seeking.silent = request.silent;
    seeking.audioDone = aIndex < 0;
    seeking.candidate = nullptr;
    ioPos = ioPos;  // (the demuxer asks for where it wants to read)
    g_api.av_seek_frame(fmt, -1, static_cast<int64_t>(target * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
    if (vctx) g_api.avcodec_flush_buffers(vctx);
    if (actx) g_api.avcodec_flush_buffers(actx);
    if (swr) {
      // (the resampler keeps a tail of the sound that was before)
      g_api.swr_free(&swr);
      swrInRate = 0;
    }
    Notify();
  }
  void FinishSeek() {
    if (!seeking.active) return;
    seeking.active = false;
    if (seeking.candidate) g_api.av_frame_free(&seeking.candidate);
    std::lock_guard lock(m);
    st.seeking = false;
    if (!seeking.silent) Event("seeked");
    if (st.readyState < kHaveCurrentData) st.readyState = kHaveCurrentData;
    UpdateClockLocked();
  }

  // ---- Decoding ----
  std::shared_ptr<VideoFrame> Convert(const AVFrame* frame, double pts) {
    sws = g_api.sws_getCachedContext(sws, frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), frame->width, frame->height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) return nullptr;
    auto out = std::make_shared<VideoFrame>();
    out->width = frame->width;
    out->height = frame->height;
    out->bgra.resize(static_cast<size_t>(frame->width) * frame->height * 4);
    uint8_t* planes[4] = {out->bgra.data(), nullptr, nullptr, nullptr};
    int strides[4] = {frame->width * 4, 0, 0, 0};
    g_api.sws_scale(sws, frame->data, frame->linesize, 0, frame->height, planes, strides);
    out->pts = pts;
    return out;
  }

  void PushVideo(std::shared_ptr<VideoFrame> frame) {
    std::unique_lock lock(m);
    // Waits for room (the picture is decoded: it is kept until the window has taken the ones before it).
    while (queue.size() >= maxFrames && !stop && !seekTo.has_value()) cv.wait_for(lock, std::chrono::milliseconds(5));
    if (stop || seekTo.has_value()) return;
    frame->serial = ++serial;
    decodedEnd = std::max(decodedEnd, frame->pts);
    queue.push_back(std::move(frame));
    if (st.readyState < kHaveCurrentData) {
      SetReadyLocked(kHaveCurrentData);
      UpdateClockLocked();
    }
    ++g_frames;
    UpdateBufferedLocked();
  }

  // The state the buffer is in: how far ahead of the clock what is decoded goes, and how much of the resource has come.
  void UpdateBufferedLocked() {
    const double ahead = decodedEnd - ClockLocked();
    int ready = st.readyState;
    if (ready >= kHaveCurrentData && !st.stalled) {
      if (eof || stream->finished() || ahead >= 3) ready = kHaveEnoughData;
      else if (ahead >= 0.4 || queue.size() >= 3) ready = kHaveFutureData;
      else ready = std::max(ready, static_cast<int>(kHaveCurrentData));
      if (ready > st.readyState) SetReadyLocked(ready);
    }
    st.buffered.clear();
    if (!std::isnan(st.duration) && st.duration > 0) {
      const uint64_t total = stream->total();
      const double fraction = total ? std::min(1.0, static_cast<double>(stream->available()) / static_cast<double>(total)) : (stream->finished() ? 1.0 : 0.0);
      if (fraction > 0) st.buffered.push_back({0, st.duration * fraction});
    } else if (decodedEnd > 0) {
      st.buffered.push_back({0, decodedEnd});
    }
  }

  void HandleVideo(AVFrame* frame) {
    double pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? SecondsOf(frame->best_effort_timestamp, fmt->streams[vIndex]) : (frame->pts != AV_NOPTS_VALUE ? SecondsOf(frame->pts, fmt->streams[vIndex]) : decodedEnd);
    if (seeking.active) {
      if (pts < seeking.target - 0.0005) {
        if (seeking.candidate) g_api.av_frame_free(&seeking.candidate);
        seeking.candidate = g_api.av_frame_clone(frame);
        seeking.candidatePts = pts;
        return;
      }
      if (seeking.candidate) {
        auto picture = Convert(seeking.candidate, seeking.candidatePts);
        g_api.av_frame_free(&seeking.candidate);
        if (picture) {
          picture->pts = seeking.target;
          PushVideo(std::move(picture));
        }
      }
      if (auto picture = Convert(frame, pts)) PushVideo(std::move(picture));
      if (seeking.audioDone || aIndex < 0) FinishSeek();
      else videoDoneWaitingAudio = true;
      return;
    }
    if (auto picture = Convert(frame, pts)) PushVideo(std::move(picture));
  }
  bool videoDoneWaitingAudio = false;

  void HandleAudio(AVFrame* frame) {
    if (!audio) {
      // No sound is played (nowhere to play it); the sound still marks time for a seek and for the data being there.
      if (seeking.active) {
        seeking.audioDone = true;
        if (vIndex < 0 || videoDoneWaitingAudio) FinishSeek();
      }
      std::lock_guard lock(m);
      if (vIndex < 0 && st.readyState < kHaveCurrentData) {
        SetReadyLocked(kHaveCurrentData);
        UpdateClockLocked();
      }
      return;
    }
    const AVStream* stream = fmt->streams[aIndex];
    double pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? SecondsOf(frame->best_effort_timestamp, stream) : audioEnd;
    const double length = static_cast<double>(frame->nb_samples) / std::max(1, frame->sample_rate);
    double trim = 0;  // seconds of this frame that are before the point seeked to
    if (seeking.active) {
      if (pts + length <= seeking.target) return;
      trim = std::max(0.0, seeking.target - pts);
    }
    // The resampler: the input as the frame has it, the output as the device has it (a different rate of playing is a different rate to play at).
    double r;
    {
      std::lock_guard lock(m);
      r = rate;
    }
    const int outRate = std::max(8000, static_cast<int>(std::lround(kOutRate / std::clamp(r, 0.25, 4.0))));
    const int channels = frame->ch_layout.nb_channels;
    if (!swr || swrInRate != frame->sample_rate || swrInFormat != frame->format || swrInChannels != channels || swrOutRate != outRate) {
      if (swr) g_api.swr_free(&swr);
      AVChannelLayout out;
      g_api.av_channel_layout_default(&out, 2);
      AVChannelLayout in;
      if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) g_api.av_channel_layout_default(&in, std::max(1, channels));
      else g_api.av_channel_layout_copy(&in, &frame->ch_layout);
      if (g_api.swr_alloc_set_opts2(&swr, &out, AV_SAMPLE_FMT_FLT, outRate, &in, static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr) < 0 || g_api.swr_init(swr) < 0) {
        if (swr) g_api.swr_free(&swr);
        g_api.av_channel_layout_uninit(&in);
        g_api.av_channel_layout_uninit(&out);
        return;
      }
      g_api.av_channel_layout_uninit(&in);
      g_api.av_channel_layout_uninit(&out);
      swrInRate = frame->sample_rate;
      swrInFormat = frame->format;
      swrInChannels = channels;
      swrOutRate = outRate;
    }
    const int capacity = g_api.swr_get_out_samples(swr, frame->nb_samples);
    if (capacity <= 0) return;
    std::vector<float> samples(static_cast<size_t>(capacity) * 2);
    uint8_t* out[1] = {reinterpret_cast<uint8_t*>(samples.data())};
    const int got = g_api.swr_convert(swr, out, capacity, const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
    if (got <= 0) return;
    size_t first = 0;
    if (trim > 0) first = std::min<size_t>(got, static_cast<size_t>(trim * outRate));
    // The volume is applied here, to what goes to the device.
    float gain;
    {
      std::lock_guard lock(m);
      gain = muted ? 0.f : static_cast<float>(std::clamp(volume, 0.0, 1.0));
    }
    if (gain != 1.f) {
      for (size_t i = first * 2; i < static_cast<size_t>(got) * 2; ++i) samples[i] *= gain;
    }
    {
      std::unique_lock lock(m);
      while (static_cast<double>(audio->QueuedFrames()) > kOutRate * 0.6 && !stop && !seekTo.has_value()) cv.wait_for(lock, std::chrono::milliseconds(5));
      if (stop || seekTo.has_value()) return;
      const double start = pts + static_cast<double>(first) * r / kOutRate;
      if (seeking.active || std::abs(start - audioEnd) > 0.15 || audioEnd == 0) audioEnd = start;
      audio->Queue(samples.data() + first * 2, static_cast<size_t>(got) - first);
      audioEnd += static_cast<double>(got - first) * r / kOutRate;
      decodedEnd = std::max(decodedEnd, audioEnd);
      if (vIndex < 0) {
        if (st.readyState < kHaveCurrentData) {
          SetReadyLocked(kHaveCurrentData);
          UpdateClockLocked();
        }
        UpdateBufferedLocked();
      }
    }
    if (seeking.active) {
      seeking.audioDone = true;
      if (vIndex < 0 || videoDoneWaitingAudio) {
        videoDoneWaitingAudio = false;
        FinishSeek();
      }
    }
  }

  // Frames out of a decoder.
  void Drain(AVCodecContext* ctx, bool video) {
    AVFrame* frame = g_api.av_frame_alloc();
    for (;;) {
      const int r = g_api.avcodec_receive_frame(ctx, frame);
      if (r < 0) break;
      if (video) HandleVideo(frame);
      else HandleAudio(frame);
      g_api.av_frame_unref(frame);
      if (Stopping()) break;
    }
    g_api.av_frame_free(&frame);
  }
  bool Stopping() {
    std::lock_guard lock(m);
    return stop || seekTo.has_value();
  }

  void Run() {
    if (!LoadFfmpeg()) {
      Fail(4, "FFmpeg is not available");
      return;
    }
    if (!Open()) {
      Fail(stream->failed() ? 2 : 4, stream->failed() ? "network error" : "the media format is not supported");
      Close();
      return;
    }
    PublishMetadata();
    AVPacket* packet = g_api.av_packet_alloc();
    for (;;) {
      std::optional<SeekRequest> request;
      {
        std::unique_lock lock(m);
        if (stop) break;
        request = seekTo;
        seekTo.reset();
      }
      if (request) {
        BeginSeek(*request);
        videoDoneWaitingAudio = false;
      }
      {
        std::unique_lock lock(m);
        if (Full() || eof) {
          UpdateBufferedLocked();
          if (eof) CheckEndLocked();
          cv.wait_for(lock, std::chrono::milliseconds(eof ? 20 : 5));
          continue;
        }
      }
      const int r = g_api.av_read_frame(fmt, packet);
      if (r == AVERROR_EXIT) continue;  // (a seek or a stop wants this thread)
      if (r < 0) {
        if (r == AVERROR_EOF || fmt->pb->eof_reached) {
          if (vctx) { g_api.avcodec_send_packet(vctx, nullptr); Drain(vctx, true); }
          if (actx) { g_api.avcodec_send_packet(actx, nullptr); Drain(actx, false); }
          if (seeking.active) FinishSeek();
          std::lock_guard lock(m);
          eof = true;
          if (std::isnan(st.duration) && decodedEnd > 0) {
            st.duration = decodedEnd;
            Event("durationchange");
          }
          SetReadyLocked(std::max<int>(st.readyState, kHaveEnoughData));
          UpdateBufferedLocked();
          Notify();
        } else if (stream->failed()) {
          Fail(2, "network error");
          eof = true;
        } else {
          Fail(3, "the media could not be read");
          eof = true;
        }
        continue;
      }
      if (packet->stream_index == vIndex && vctx) {
        if (g_api.avcodec_send_packet(vctx, packet) >= 0) Drain(vctx, true);
      } else if (packet->stream_index == aIndex && actx) {
        if (g_api.avcodec_send_packet(actx, packet) >= 0) Drain(actx, false);
      }
      g_api.av_packet_unref(packet);
      Notify();
    }
    g_api.av_packet_free(&packet);
    if (seeking.candidate) g_api.av_frame_free(&seeking.candidate);
    Close();
  }

  // At the end of the file: when all that was decoded has been played, the playback has ended (or begins again).
  void CheckEndLocked() {
    if (st.ended || st.seeking) return;
    const bool playedOut = queue.empty() && (!audio || audio->QueuedFrames() == 0);
    const double now = ClockLocked();
    const double duration = std::isnan(st.duration) ? decodedEnd : st.duration;
    if (!playedOut) return;
    if (clockRunning && now < duration - 0.25 && !(audio && audio->QueuedFrames() == 0 && queue.empty())) return;
    if (!wantPlay && now < duration - 0.05) return;
    if (loop && wantPlay) {
      seekTo = SeekRequest{0, true};
      Event("loop");
      return;
    }
    st.ended = true;
    pausedTime = duration;
    UpdateClockLocked();
    Event("ended");
    Notify();
  }
};

Player::Player() : impl_(std::make_unique<Impl>()) { impl_->owner = this; }

Player::~Player() {
  {
    std::lock_guard lock(impl_->m);
    impl_->stop = true;
  }
  impl_->cv.notify_all();
  impl_->stream->Poke();
  if (impl_->thread.joinable()) impl_->thread.join();
  if (playing_.exchange(false)) --g_playing;
}

std::shared_ptr<Player> Player::Create(std::shared_ptr<ByteStream> stream, std::function<void()> changed) {
  std::shared_ptr<Player> player(new Player());
  player->impl_->stream = std::move(stream);
  player->impl_->changed = std::move(changed);
  Impl* impl = player->impl_.get();
  impl->thread = std::thread([impl] { impl->Run(); });
  return player;
}

void Player::Play() {
  {
    std::lock_guard lock(impl_->m);
    if (impl_->st.ended) {
      // Playing what has ended begins it again.
      impl_->seekTo = Impl::SeekRequest{0, false};
      impl_->st.ended = false;
    }
    impl_->wantPlay = true;
    impl_->UpdateClockLocked();
  }
  impl_->cv.notify_all();
  impl_->stream->Poke();
  impl_->Notify();
}

void Player::Pause() {
  {
    std::lock_guard lock(impl_->m);
    impl_->wantPlay = false;
    impl_->UpdateClockLocked();
  }
  impl_->Notify();
}

void Player::Seek(double seconds) {
  {
    std::lock_guard lock(impl_->m);
    impl_->seekTo = Impl::SeekRequest{seconds, false};
  }
  impl_->cv.notify_all();
  impl_->stream->Poke();
}

void Player::SetVolume(double volume) {
  std::lock_guard lock(impl_->m);
  impl_->volume = volume;
}

void Player::SetMuted(bool muted) {
  std::lock_guard lock(impl_->m);
  impl_->muted = muted;
}

void Player::SetRate(double rate) {
  {
    std::lock_guard lock(impl_->m);
    if (rate == impl_->rate) return;
    const double now = impl_->ClockLocked();
    impl_->pausedTime = now;
    impl_->wallBase = now;
    impl_->wallStart = Clock::now();
    impl_->rate = rate;
    // What the sound has queued was made for the old rate: it is made again from here.
    if (impl_->st.hasAudio && impl_->audio && !impl_->st.ended) impl_->seekTo = Impl::SeekRequest{now, true};
  }
  impl_->cv.notify_all();
  impl_->stream->Poke();
}

void Player::SetLoop(bool loop) {
  std::lock_guard lock(impl_->m);
  impl_->loop = loop;
}

PlayerState Player::State() {
  std::lock_guard lock(impl_->m);
  PlayerState state = impl_->st;
  state.currentTime = impl_->st.seeking && impl_->seekTo ? impl_->seekTo->time : impl_->ClockLocked();
  if (!std::isnan(state.duration)) state.currentTime = std::min(state.currentTime, state.duration);
  return state;
}

std::vector<std::string> Player::TakeEvents() {
  std::lock_guard lock(impl_->m);
  std::vector<std::string> out = std::move(impl_->events);
  impl_->events.clear();
  return out;
}

std::shared_ptr<const VideoFrame> Player::Frame() {
  std::lock_guard lock(impl_->m);
  const double now = impl_->ClockLocked();
  bool advanced = false;
  while (!impl_->queue.empty() && impl_->queue.front()->pts <= now + 0.002) {
    impl_->current = std::move(impl_->queue.front());
    impl_->queue.pop_front();
    advanced = true;
  }
  if (!impl_->current && !impl_->queue.empty() && impl_->st.readyState >= kHaveCurrentData) {
    impl_->current = std::move(impl_->queue.front());
    impl_->queue.pop_front();
    advanced = true;
  }
  if (advanced) {
    ++g_frames;
    impl_->cv.notify_all();
  }
  return impl_->current;
}

}  // namespace solar::media
