/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <mutex>
#include <unordered_map>

#include "solar/dom/MediaHook.h"
#include "solar/media/Player.h"

#include "FfmpegApi.h"

namespace solar::media {

namespace {
std::mutex g_mutex;
std::unordered_map<const void*, std::shared_ptr<Player>>& Players() {
  static std::unordered_map<const void*, std::shared_ptr<Player>> players;
  return players;
}
}  // namespace

namespace {
std::unordered_map<const void*, std::string>& Captions() {
  static std::unordered_map<const void*, std::string> captions;
  return captions;
}
}  // namespace

void AttachPlayer(const void* element, std::shared_ptr<Player> player) {
  std::shared_ptr<Player> old;  // (let go outside the lock: a player's destructor waits for its thread)
  {
    std::lock_guard lock(g_mutex);
    const auto found = Players().find(element);
    if (player) {
      if (found == Players().end()) {
        ++dom::g_mediaAttached;
        dom::g_mediaDetach = &DetachPlayer;
        Players()[element] = std::move(player);
      } else {
        old = std::move(found->second);
        found->second = std::move(player);
      }
    } else if (found != Players().end()) {
      old = std::move(found->second);
      Players().erase(found);
      --dom::g_mediaAttached;
    }
  }
}

void DetachPlayer(const void* element) {
  std::shared_ptr<Player> old;
  {
    std::lock_guard lock(g_mutex);
    Captions().erase(element);
    const auto found = Players().find(element);
    if (found == Players().end()) return;
    old = std::move(found->second);
    Players().erase(found);
    --dom::g_mediaAttached;
  }
}

std::shared_ptr<Player> PlayerOf(const void* element) {
  std::lock_guard lock(g_mutex);
  const auto found = Players().find(element);
  return found == Players().end() ? nullptr : found->second;
}

// ---- captions ----

void SetCaptions(const void* element, std::string text) {
  std::lock_guard lock(g_mutex);
  if (text.empty()) Captions().erase(element);
  else Captions()[element] = std::move(text);
}

std::string CaptionsOf(const void* element) {
  std::lock_guard lock(g_mutex);
  const auto found = Captions().find(element);
  return found == Captions().end() ? std::string() : found->second;
}

// ---- canPlayType ----

namespace {

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// The demuxers that play the type, by FFmpeg's names (empty for a type that is not one).
std::vector<const char*> ContainersOf(const std::string& mime) {
  if (mime == "video/mp4" || mime == "audio/mp4" || mime == "video/x-m4v" || mime == "audio/x-m4a" || mime == "audio/m4a") return {"mov"};
  if (mime == "video/webm" || mime == "audio/webm" || mime == "video/x-matroska" || mime == "audio/x-matroska") return {"matroska"};
  if (mime == "video/ogg" || mime == "audio/ogg" || mime == "application/ogg" || mime == "audio/opus") return {"ogg"};
  if (mime == "audio/mpeg" || mime == "audio/mp3" || mime == "audio/x-mp3") return {"mp3"};
  if (mime == "audio/wav" || mime == "audio/wave" || mime == "audio/x-wav" || mime == "audio/vnd.wave") return {"wav"};
  if (mime == "audio/flac" || mime == "audio/x-flac") return {"flac"};
  if (mime == "audio/aac" || mime == "audio/x-aac") return {"aac"};
  if (mime == "video/quicktime") return {"mov"};
  return {};
}

// The codec a codecs= token names, or AV_CODEC_ID_NONE.
AVCodecID CodecOf(const std::string& token) {
  auto starts = [&](const char* prefix) { return token.rfind(prefix, 0) == 0; };
  if (starts("avc1") || starts("avc3")) return AV_CODEC_ID_H264;
  if (starts("hev1") || starts("hvc1")) return AV_CODEC_ID_HEVC;
  if (token == "vp8") return AV_CODEC_ID_VP8;
  if (token == "vp9" || starts("vp09")) return AV_CODEC_ID_VP9;
  if (starts("av01")) return AV_CODEC_ID_AV1;
  if (token == "mp4a.40.2" || token == "mp4a.40.5" || token == "mp4a.40.29" || starts("mp4a.40") || token == "mp4a.67") return AV_CODEC_ID_AAC;
  if (token == "mp4a.69" || token == "mp4a.6b" || token == "mp3") return AV_CODEC_ID_MP3;
  if (token == "opus") return AV_CODEC_ID_OPUS;
  if (token == "vorbis") return AV_CODEC_ID_VORBIS;
  if (token == "flac") return AV_CODEC_ID_FLAC;
  if (token == "theora") return AV_CODEC_ID_THEORA;
  if (token == "ac-3") return AV_CODEC_ID_AC3;
  if (token == "ec-3") return AV_CODEC_ID_EAC3;
  return AV_CODEC_ID_NONE;
}

}  // namespace

int CanPlayType(const std::string& typeIn, const std::string& codecsIn) {
  if (!LoadFfmpeg()) return 0;
  const std::string mime = Lower(typeIn);
  const auto containers = ContainersOf(mime);
  if (containers.empty()) return 0;
  bool haveContainer = false;
  for (const char* name : containers) if (g_api.av_find_input_format(name)) haveContainer = true;
  if (!haveContainer) return 0;
  const std::string codecs = Lower(codecsIn);
  if (codecs.empty()) return 1;
  size_t at = 0;
  while (at < codecs.size()) {
    size_t end = codecs.find(',', at);
    if (end == std::string::npos) end = codecs.size();
    std::string token = codecs.substr(at, end - at);
    while (!token.empty() && token.front() == ' ') token.erase(token.begin());
    while (!token.empty() && token.back() == ' ') token.pop_back();
    at = end + 1;
    if (token.empty()) continue;
    const AVCodecID id = CodecOf(token);
    if (id == AV_CODEC_ID_NONE || !g_api.avcodec_find_decoder(id)) return 0;
  }
  return 2;
}

}  // namespace solar::media
