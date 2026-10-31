/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/gui/SdlAudio.h"

#include <SDL3/SDL.h>

namespace solar::gui {

namespace {

// The sound of the media elements, through SDL's audio stream: the player gives float samples as it decodes, and SDL mixes and plays them.
class SdlAudioOutput final : public media::AudioOutput {
 public:
  ~SdlAudioOutput() override {
    if (stream_) SDL_DestroyAudioStream(stream_);
  }
  bool Start(int sampleRate, int channels) override {
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) return false;
    SDL_AudioSpec spec;
    SDL_zero(spec);
    spec.format = SDL_AUDIO_F32;
    spec.channels = channels;
    spec.freq = sampleRate;
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    channels_ = channels;
    return stream_ != nullptr;
  }
  void Queue(const float* samples, size_t frames) override {
    if (stream_) SDL_PutAudioStreamData(stream_, samples, static_cast<int>(frames * channels_ * sizeof(float)));
  }
  size_t QueuedFrames() override {
    if (!stream_) return 0;
    return static_cast<size_t>(SDL_GetAudioStreamQueued(stream_)) / (channels_ * sizeof(float));
  }
  void Pause(bool paused) override {
    if (!stream_) return;
    if (paused) SDL_PauseAudioStreamDevice(stream_);
    else SDL_ResumeAudioStreamDevice(stream_);
  }
  void Clear() override {
    if (stream_) SDL_ClearAudioStream(stream_);
  }

 private:
  SDL_AudioStream* stream_ = nullptr;
  int channels_ = 2;
};

}  // namespace

void InstallSdlAudio() {
  media::SetAudioOutputFactory([]() -> std::unique_ptr<media::AudioOutput> { return std::make_unique<SdlAudioOutput>(); });
}

}  // namespace solar::gui
