/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>
#include <thread>

#include "PlayerImpl.h"

namespace solar::media {

// Playing from a MediaSource: the same decoding and clock as for a file (Player.cpp), with the packets taken from the buffers of the page by time, and a stall where
// the page has not appended yet.

bool Player::Impl::OpenDecoder(int kind, const std::shared_ptr<MseCodec>& codec) {
  AVCodecContext*& ctx = kind == 0 ? vctx : actx;
  if (ctx) g_api.avcodec_free_context(&ctx);
  AVCodecParameters* parameters = kind == 0 ? codec->vpar : codec->apar;
  if (!parameters) return false;
  const AVCodec* decoder = g_api.avcodec_find_decoder(parameters->codec_id);
  if (!decoder) return false;
  ctx = g_api.avcodec_alloc_context3(decoder);
  if (!ctx || g_api.avcodec_parameters_to_context(ctx, parameters) < 0) return false;
  ctx->pkt_timebase = kind == 0 ? codec->vtb : codec->atb;
  ctx->thread_count = 0;
  ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
  if (g_api.avcodec_open2(ctx, decoder, nullptr) < 0) {
    g_api.avcodec_free_context(&ctx);
    return false;
  }
  (kind == 0 ? vTb : aTb) = kind == 0 ? codec->vtb : codec->atb;
  return true;
}

void Player::Impl::MseSetStalled(bool stalled) {
  bool tell = false;
  {
    std::lock_guard lock(m);
    if (st.stalled == stalled) return;
    st.stalled = stalled;
    if (stalled) {
      if (st.readyState > kHaveCurrentData) st.readyState = kHaveCurrentData;
      if (wantPlay) Event("waiting");
    }
    UpdateClockLocked();
    tell = true;
  }
  if (tell) Notify();
}

void Player::Impl::BeginSeekMse(SeekRequest request) {
  double target = std::max(0.0, request.time);
  {
    std::lock_guard lock(m);
    const double duration = mse->duration();
    if (!std::isnan(duration) && duration > 0) target = std::min(target, duration);
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
  videoDoneWaitingAudio = false;
  if (vctx) g_api.avcodec_flush_buffers(vctx);
  if (actx) g_api.avcodec_flush_buffers(actx);
  if (swr) {
    g_api.swr_free(&swr);
    swrInRate = 0;
  }
  // Where the decoding begins: the picture to start from and the sound at or before the target. If the page has not put them there yet, it is waited for.
  bool ok = true;
  double start[2] = {target, target};
  for (int kind = 0; kind < 2; ++kind) {
    if ((kind == 0 && vIndex < 0) || (kind == 1 && aIndex < 0)) continue;
    if (!mse->FindStart(kind, target, start[kind])) ok = false;
  }
  if (ok) {
    for (int kind = 0; kind < 2; ++kind) cursor[kind] = start[kind] - 1e-6;
    mseSeekPending.reset();
  } else {
    mseSeekPending = request;
  }
  Notify();
}

void Player::Impl::RunMse() {
  if (!LoadFfmpeg()) {
    Fail(4, "FFmpeg is not available");
    return;
  }
  // The tracks are known when the page has appended an init segment.
  for (;;) {
    {
      std::unique_lock lock(m);
      if (stop) return;
    }
    if (mse->HasTrack(0) || mse->HasTrack(1)) break;
    std::unique_lock lock(m);
    cv.wait_for(lock, std::chrono::milliseconds(30));
  }
  vIndex = mse->HasTrack(0) ? 0 : -1;
  aIndex = mse->HasTrack(1) ? 0 : -1;
  {
    std::lock_guard lock(m);
    st.hasVideo = vIndex >= 0;
    st.hasAudio = aIndex >= 0;
    if (vIndex >= 0) {
      auto codec = mse->CodecOf(0);
      if (codec && codec->vpar) {
        sar = codec->sar;
        st.videoWidth = static_cast<int>(std::lround(codec->vpar->width * sar));
        st.videoHeight = codec->vpar->height;
        const size_t frameBytes = static_cast<size_t>(std::max(1, codec->vpar->width)) * std::max(1, codec->vpar->height) * 4;
        maxFrames = std::clamp<size_t>((size_t{96} << 20) / frameBytes, 3, 16);
      }
    }
    if (aIndex >= 0 && Factory()) {
      audio = Factory()();
      if (audio && !audio->Start(kOutRate, 2)) audio.reset();
      if (audio) {
        audioClock = true;
        audio->Pause(true);
      }
    }
    st.duration = mse->duration();
    Event("durationchange");
    SetReadyLocked(kHaveMetadata);
  }
  Notify();

  uint64_t seenVersion = 0;
  BeginSeekMse(SeekRequest{0, true});
  double lastDuration = st.duration;
  for (;;) {
    std::optional<SeekRequest> request;
    {
      std::unique_lock lock(m);
      if (stop) break;
      request = seekTo;
      seekTo.reset();
    }
    if (request) BeginSeekMse(*request);
    // The page may have changed the duration.
    {
      const double duration = mse->duration();
      std::lock_guard lock(m);
      if (!(std::isnan(duration) && std::isnan(lastDuration)) && duration != lastDuration) {
        lastDuration = duration;
        st.duration = duration;
        Event("durationchange");
      }
    }
    // A seek that waits for data: tried again when there is some.
    if (mseSeekPending) {
      if (mse->version() != seenVersion) {
        seenVersion = mse->version();
        BeginSeekMse(*mseSeekPending);
      }
      if (mseSeekPending) {
        std::unique_lock lock(m);
        UpdateBufferedLocked();
        cv.wait_for(lock, std::chrono::milliseconds(20));
        continue;
      }
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
    MsePacket pv, pa;
    bool gapV = false, gapA = false;
    const bool hv = vIndex >= 0 && mse->Peek(0, cursor[0], pv, gapV);
    const bool ha = aIndex >= 0 && mse->Peek(1, cursor[1], pa, gapA);
    if (!hv && !ha) {
      const bool noMore = !gapV && !gapA;
      if (mse->ended() && noMore) {
        // The page said the end: what is decoded is played out.
        if (vctx) {
          g_api.avcodec_send_packet(vctx, nullptr);
          Drain(vctx, true);
        }
        if (actx) {
          g_api.avcodec_send_packet(actx, nullptr);
          Drain(actx, false);
        }
        if (seeking.active) FinishSeek();
        std::lock_guard lock(m);
        eof = true;
        SetReadyLocked(std::max<int>(st.readyState, kHaveEnoughData));
        UpdateBufferedLocked();
        Notify();
        continue;
      }
      // Nothing to decode, and the page has more to give: it is waited for (a stall if what plays is about to run out).
      bool starved;
      {
        std::lock_guard lock(m);
        starved = queue.empty() && (!audio || audio->QueuedFrames() < static_cast<size_t>(kOutRate * 0.05));
        if (!starved) UpdateBufferedLocked();
      }
      if (seeking.active && starved) {
        // (The seek is waiting for the first picture; there is none to come.)
      }
      MseSetStalled(starved);
      std::unique_lock lock(m);
      cv.wait_for(lock, std::chrono::milliseconds(20));
      continue;
    }
    MseSetStalled(false);
    const int kind = (hv && (!ha || pv.dts <= pa.dts)) ? 0 : 1;
    MsePacket& packet = kind == 0 ? pv : pa;
    if (curCodec[kind] != packet.codec) {
      if (!OpenDecoder(kind, packet.codec)) {
        Fail(4, "the media cannot be decoded");
        cursor[kind] = packet.dts;
        continue;
      }
      curCodec[kind] = packet.codec;
    }
    AVCodecContext* ctx = kind == 0 ? vctx : actx;
    if (ctx && g_api.avcodec_send_packet(ctx, packet.packet.get()) >= 0) Drain(ctx, kind == 0);
    cursor[kind] = packet.dts;
    Notify();
  }
  if (seeking.candidate) g_api.av_frame_free(&seeking.candidate);
  Close();
}

}  // namespace solar::media
