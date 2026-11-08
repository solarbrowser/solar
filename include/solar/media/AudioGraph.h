/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "solar/media/Player.h"

// Web Audio (https://webaudio.github.io/web-audio-api/): the graph of nodes that a page's AudioContext is, and what mixes it. The nodes are made and joined by number; the engine renders the
// graph a quantum (128 frames) at a time, on a thread of its own, into the program's audio output (or, for an OfflineAudioContext, into memory as fast as it can). The one thing that is the
// page's script is a ScriptProcessorNode's callback: the engine collects what it is to be given and the page's thread takes it, runs the callback and gives back what it made.
namespace solar::media::audio {

constexpr int kQuantum = 128;
constexpr int kMostChannels = 8;

// Samples of sound, a vector for each channel.
struct Buffer {
  int channels = 0;
  int frames = 0;
  double rate = 44100;
  std::vector<std::vector<float>> data;
};

enum class Kind : uint8_t {
  Gain, BufferSource, Oscillator, ScriptProcessor, StereoPanner, Biquad, Delay, Analyser, ConstantSource, ChannelSplitter, ChannelMerger, Compressor,
};

// What an operation on a parameter is (AudioParam's methods).
enum class ParamOp : uint8_t { Set, SetAt, LinearRampTo, ExponentialRampTo, SetTarget, Cancel, CancelAndHold };

// What a node is set by, besides its parameters (the number given with it).
enum class Setting : uint8_t {
  WaveType,       // oscillator: 0 sine, 1 square, 2 sawtooth, 3 triangle
  FilterType,     // biquad: 0 lowpass, 1 highpass, 2 bandpass, 3 lowshelf, 4 highshelf, 5 peaking, 6 notch, 7 allpass
  Loop,           // buffer source: 0 or 1
  LoopStart,      // buffer source: seconds
  LoopEnd,
  FftSize,        // analyser
  SmoothingTimeConstant,
  ChannelCount,   // the number of channels a node mixes its inputs to (0: as many as the inputs have)
};

class Engine {
 public:
  // `realtime`: the sound goes to the audio output as it is made; else it is made when RenderOffline is called.
  static std::shared_ptr<Engine> Create(double sampleRate, int channels, bool realtime);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  double sampleRate() const { return rate_; }
  int channels() const { return channels_; }
  // Seconds of sound that have been played (of an offline context: made).
  double currentTime();
  // The sound is made (and goes out) once this is called; Suspend stops it going on.
  void Start();
  void Suspend();
  void Close();
  bool running() const { return running_.load(); }
  // The most sound that is made ahead of what is played, in seconds (the lag a ScriptProcessorNode has besides its own).
  void SetAhead(double seconds) { ahead_ = seconds; }

  uint32_t CreateNode(Kind kind, int a = 0, int b = 0, int c = 0);
  void DeleteNode(uint32_t node);
  // Joins an output of a node to the input of another (a splitter has an output for each channel, a merger an input for each); `Disconnect` takes one join away or, with `to` 0, all of the node's.
  void Connect(uint32_t from, uint32_t to, int output = 0, int input = 0);
  void ConnectParam(uint32_t from, uint32_t to, int param);
  void ConnectDestination(uint32_t from);
  void Disconnect(uint32_t from, uint32_t to);

  void SetParam(uint32_t node, int param, ParamOp op, double value, double time = 0, double extra = 0);
  double GetParam(uint32_t node, int param);
  void Set(uint32_t node, Setting setting, double value);
  void SetBuffer(uint32_t node, std::shared_ptr<const Buffer> buffer);
  void StartSource(uint32_t node, double when, double offset, double duration);
  void StopSource(uint32_t node, double when);
  // The sources that have ended since the last call.
  std::vector<uint32_t> TakeEnded();

  // ScriptProcessorNode: the ones that have collected a buffer of input, what it was, and what the page made of it.
  std::vector<uint32_t> PendingProcessors();
  bool ProcessorInput(uint32_t node, int channel, float* out, size_t count);
  void ProcessorOutput(uint32_t node, int channel, const float* in, size_t count);
  void ProcessorDone(uint32_t node);

  // AnalyserNode: what was last played, and its spectrum in decibels.
  void AnalyserTimeData(uint32_t node, float* out, size_t count);
  void AnalyserFrequencyData(uint32_t node, float* out, size_t count);

  // An offline context: this much sound, made now.
  Buffer RenderOffline(int frames);

  struct Impl;

 private:
  Engine(double rate, int channels, bool realtime);
  void Loop();
  double rate_;
  int channels_;
  bool realtime_;
  double ahead_ = 0.08;
  std::atomic<bool> running_{false}, stop_{false};
  std::unique_ptr<Impl> impl_;
  std::thread thread_;
};

}  // namespace solar::media::audio
