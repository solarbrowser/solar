/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

// FFmpeg is loaded when the first media is played, not when the program starts: the program is built against the headers of one version of FFmpeg and runs with the
// shared libraries of that major version if the machine has them (and plays no media if it has not). Everything of FFmpeg that the player calls is in the table `g_api`,
// which LoadFfmpeg fills from the libraries; what only reads the headers (av_q2d, the AV_ and AVERROR macros) is used as it is.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace solar::media {

// X(library, function): the libraries are avformat, avcodec, avutil, swscale and swresample.
#define SOLAR_FFMPEG_FUNCTIONS(X)                                                                                          \
  X(avformat, avformat_alloc_context) X(avformat, avformat_open_input) X(avformat, avformat_find_stream_info)             \
  X(avformat, av_find_best_stream) X(avformat, av_read_frame) X(avformat, av_seek_frame) X(avformat, avformat_close_input) \
  X(avformat, avio_alloc_context) X(avformat, avio_context_free) X(avformat, av_guess_sample_aspect_ratio)                \
  X(avformat, av_find_input_format)                                                                                        \
  X(avcodec, avcodec_find_decoder) X(avcodec, avcodec_alloc_context3) X(avcodec, avcodec_parameters_to_context)           \
  X(avcodec, avcodec_open2) X(avcodec, avcodec_free_context) X(avcodec, avcodec_send_packet)                              \
  X(avcodec, avcodec_receive_frame) X(avcodec, avcodec_flush_buffers) X(avcodec, av_packet_alloc)                         \
  X(avcodec, av_packet_free) X(avcodec, av_packet_unref)                                                                   \
  X(avutil, av_malloc) X(avutil, av_freep) X(avutil, av_frame_alloc) X(avutil, av_frame_free) X(avutil, av_frame_unref)  \
  X(avutil, av_frame_clone) X(avutil, av_channel_layout_default) X(avutil, av_channel_layout_copy)                        \
  X(avutil, av_channel_layout_uninit)                                                                                      \
  X(swscale, sws_getCachedContext) X(swscale, sws_scale) X(swscale, sws_freeContext)                                      \
  X(swresample, swr_alloc_set_opts2) X(swresample, swr_init) X(swresample, swr_free) X(swresample, swr_convert)           \
  X(swresample, swr_get_out_samples)

struct FfmpegApi {
#define X(lib, name) decltype(&::name) name = nullptr;
  SOLAR_FFMPEG_FUNCTIONS(X)
#undef X
};

extern FfmpegApi g_api;
// True once the libraries are loaded (they are loaded on the first call, and the answer is kept).
bool LoadFfmpeg();

}  // namespace solar::media
