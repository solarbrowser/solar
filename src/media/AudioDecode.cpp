/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/media/AudioDecode.h"

#include <algorithm>
#include <cstring>

#include "solar/media/FfmpegApi.h"

namespace solar::media::audio {

namespace {

struct Memory {
  const uint8_t* data;
  size_t size, pos = 0;
};

int Read(void* opaque, uint8_t* buffer, int size) {
  auto* m = static_cast<Memory*>(opaque);
  if (m->pos >= m->size) return AVERROR_EOF;
  const size_t n = std::min(static_cast<size_t>(size), m->size - m->pos);
  std::memcpy(buffer, m->data + m->pos, n);
  m->pos += n;
  return static_cast<int>(n);
}

int64_t Seek(void* opaque, int64_t offset, int whence) {
  auto* m = static_cast<Memory*>(opaque);
  if (whence == AVSEEK_SIZE) return static_cast<int64_t>(m->size);
  int64_t target;
  switch (whence & ~AVSEEK_FORCE) {
    case SEEK_SET: target = offset; break;
    case SEEK_CUR: target = static_cast<int64_t>(m->pos) + offset; break;
    case SEEK_END: target = static_cast<int64_t>(m->size) + offset; break;
    default: return -1;
  }
  if (target < 0 || target > static_cast<int64_t>(m->size)) return -1;
  m->pos = static_cast<size_t>(target);
  return target;
}

}  // namespace

bool DecodeAudio(const uint8_t* bytes, size_t size, double rate, Buffer& out) {
  if (!bytes || size == 0 || !LoadFfmpeg()) return false;
  Memory memory{bytes, size};
  AVFormatContext* fmt = g_api.avformat_alloc_context();
  uint8_t* ioBuffer = static_cast<uint8_t*>(g_api.av_malloc(1 << 16));
  AVIOContext* io = g_api.avio_alloc_context(ioBuffer, 1 << 16, 0, &memory, &Read, nullptr, &Seek);
  AVCodecContext* codec = nullptr;
  SwrContext* swr = nullptr;
  AVPacket* packet = nullptr;
  AVFrame* frame = nullptr;
  bool good = false;
  const auto cleanup = [&] {
    if (frame) g_api.av_frame_free(&frame);
    if (packet) g_api.av_packet_free(&packet);
    if (swr) g_api.swr_free(&swr);
    if (codec) g_api.avcodec_free_context(&codec);
    if (fmt) g_api.avformat_close_input(&fmt);
    if (io) {
      g_api.av_freep(&io->buffer);
      g_api.avio_context_free(&io);
    }
  };
  if (!fmt || !io) {
    if (fmt) g_api.avformat_close_input(&fmt);
    return false;
  }
  fmt->pb = io;
  fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
  if (g_api.avformat_open_input(&fmt, nullptr, nullptr, nullptr) < 0 || g_api.avformat_find_stream_info(fmt, nullptr) < 0) {
    cleanup();
    return false;
  }
  const AVCodec* decoder = nullptr;
  const int stream = g_api.av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
  if (stream < 0 || !decoder) {
    cleanup();
    return false;
  }
  codec = g_api.avcodec_alloc_context3(decoder);
  if (!codec || g_api.avcodec_parameters_to_context(codec, fmt->streams[stream]->codecpar) < 0 || g_api.avcodec_open2(codec, decoder, nullptr) < 0) {
    cleanup();
    return false;
  }
  const int channels = std::clamp(codec->ch_layout.nb_channels, 1, kMostChannels);
  AVChannelLayout layout;
  g_api.av_channel_layout_default(&layout, channels);
  const size_t most = static_cast<size_t>(rate * 600);  // (ten minutes: more is not a sound effect or a song to hold as samples)
  out.channels = channels;
  out.rate = rate;
  out.data.assign(static_cast<size_t>(channels), {});
  packet = g_api.av_packet_alloc();
  frame = g_api.av_frame_alloc();
  std::vector<std::vector<float>> block(static_cast<size_t>(channels));
  std::vector<float*> pointers(static_cast<size_t>(channels));
  const auto convert = [&](const uint8_t** input, int count) {
    const int room = g_api.swr_get_out_samples(swr, count) + 64;
    for (int c = 0; c < channels; ++c) {
      block[static_cast<size_t>(c)].resize(static_cast<size_t>(room));
      pointers[static_cast<size_t>(c)] = block[static_cast<size_t>(c)].data();
    }
    const int got = g_api.swr_convert(swr, reinterpret_cast<uint8_t**>(pointers.data()), room, input, count);
    if (got < 0) return false;
    for (int c = 0; c < channels; ++c) out.data[static_cast<size_t>(c)].insert(out.data[static_cast<size_t>(c)].end(), block[static_cast<size_t>(c)].begin(), block[static_cast<size_t>(c)].begin() + got);
    return true;
  };
  const auto drain = [&]() {
    for (;;) {
      const int status = g_api.avcodec_receive_frame(codec, frame);
      if (status < 0) return status == AVERROR(EAGAIN) || status == AVERROR_EOF;
      if (!swr) {
        if (g_api.swr_alloc_set_opts2(&swr, &layout, AV_SAMPLE_FMT_FLTP, static_cast<int>(rate), &frame->ch_layout, static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr) < 0 || g_api.swr_init(swr) < 0) return false;
      }
      const bool converted = convert(const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
      g_api.av_frame_unref(frame);
      if (!converted || out.data[0].size() > most) return false;
    }
  };
  bool failed = false;
  while (!failed && g_api.av_read_frame(fmt, packet) >= 0) {
    if (packet->stream_index == stream && g_api.avcodec_send_packet(codec, packet) >= 0) failed = !drain();
    g_api.av_packet_unref(packet);
  }
  if (!failed) {
    g_api.avcodec_send_packet(codec, nullptr);
    failed = !drain();
  }
  if (!failed && swr) convert(nullptr, 0);
  good = !failed && swr && !out.data[0].empty();
  g_api.av_channel_layout_uninit(&layout);
  cleanup();
  if (good) out.frames = static_cast<int>(out.data[0].size());
  return good;
}

}  // namespace solar::media::audio
