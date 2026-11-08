/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/web-api/WebAudio.h"

#include <cstring>
#include <map>
#include <memory>
#include <mutex>

#include "solar/media/AudioDecode.h"
#include "solar/media/AudioGraph.h"

namespace solar::webapi {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::Value;
namespace audio = media::audio;

namespace {

// (The page's realm and its workers' share the table; a context is found by its number, which is never used again. At most 6 realtime contexts are open at once, as in a browser.)
std::mutex g_mutex;
std::map<int, std::shared_ptr<audio::Engine>> g_engines;
int g_next = 1;

std::shared_ptr<audio::Engine> Find(int id) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto found = g_engines.find(id);
  return found == g_engines.end() ? nullptr : found->second;
}

enum Op {
  New = 0, Close, Start, Suspend, CurrentTime, CreateNode, DeleteNode, Connect, ConnectParam, ConnectDestination, Disconnect, SetParam, GetParam, Set,
  StartSource, StopSource, Poll, ProcessorInput, ProcessorOutput, ProcessorDone, AnalyserTime, AnalyserFrequency, SetBuffer, RenderOffline,
};

// __solarAudio(op, context, ...)
Value Call(Context& ctx, Value, qe::Args args, Value) {
  if (args.empty()) return qe::FromNumber(-1);
  const auto num = [&](size_t i) { return i < args.size() ? qe::ToNumber(ctx, args[i]) : 0.0; };
  const auto id = [&](size_t i) { return static_cast<uint32_t>(num(i)); };
  const int op = static_cast<int>(num(0));
  if (op == New) {
    const bool realtime = num(4) != 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (realtime) {
      int open = 0;
      for (const auto& [number, engine] : g_engines) if (engine->running() || true) ++open;
      if (open >= 64) return qe::FromNumber(-1);
    }
    const int number = g_next++;
    g_engines[number] = audio::Engine::Create(num(2), static_cast<int>(num(3)), realtime);
    return qe::FromNumber(number);
  }
  const int context = static_cast<int>(num(1));
  if (op == Close) {
    std::shared_ptr<audio::Engine> engine;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      const auto found = g_engines.find(context);
      if (found == g_engines.end()) return qe::FromNumber(0);
      engine = std::move(found->second);
      g_engines.erase(found);
    }
    engine->Close();
    return qe::FromNumber(0);
  }
  const std::shared_ptr<audio::Engine> engine = Find(context);
  if (!engine) return qe::FromNumber(-1);
  switch (op) {
    case Start: engine->Start(); break;
    case Suspend: engine->Suspend(); break;
    case CurrentTime: return qe::FromNumber(engine->currentTime());
    case CreateNode: {
      const int kind = static_cast<int>(num(2));
      if (kind < 0 || kind > static_cast<int>(audio::Kind::Compressor)) return qe::FromNumber(-1);
      return qe::FromNumber(engine->CreateNode(static_cast<audio::Kind>(kind), static_cast<int>(num(3)), static_cast<int>(num(4)), static_cast<int>(num(5))));
    }
    case DeleteNode: engine->DeleteNode(id(2)); break;
    case Connect: engine->Connect(id(2), id(3), static_cast<int>(num(4)), static_cast<int>(num(5))); break;
    case ConnectParam: engine->ConnectParam(id(2), id(3), static_cast<int>(num(4))); break;
    case ConnectDestination: engine->ConnectDestination(id(2)); break;
    case Disconnect: engine->Disconnect(id(2), id(3)); break;
    case SetParam: engine->SetParam(id(2), static_cast<int>(num(3)), static_cast<audio::ParamOp>(static_cast<int>(num(4))), num(5), num(6), num(7)); break;
    case GetParam: return qe::FromNumber(engine->GetParam(id(2), static_cast<int>(num(3))));
    case Set: engine->Set(id(2), static_cast<audio::Setting>(static_cast<int>(num(3))), num(4)); break;
    case StartSource: engine->StartSource(id(2), num(3), num(4), num(5)); break;
    case StopSource: engine->StopSource(id(2), num(3)); break;
    case Poll: {
      std::vector<Value> out;
      for (uint32_t p : engine->PendingProcessors()) out.push_back(qe::FromNumber(p));
      out.push_back(qe::FromNumber(-1));
      for (uint32_t e : engine->TakeEnded()) out.push_back(qe::FromNumber(e));
      return qe::NewArray(ctx, qe::Args(out.data(), out.size()));
    }
    case ProcessorInput:
    case ProcessorOutput:
    case AnalyserTime:
    case AnalyserFrequency: {
      const bool channelled = op == ProcessorInput || op == ProcessorOutput;
      const size_t arrayAt = channelled ? 4 : 3;
      if (args.size() <= arrayAt) return qe::FromNumber(0);
      const auto bytes = qe::MutableBytesOf(args[arrayAt]);
      if (!bytes) return qe::FromNumber(0);
      float* data = reinterpret_cast<float*>(bytes->data());
      const size_t count = bytes->size() / sizeof(float);
      if (reinterpret_cast<uintptr_t>(data) % alignof(float) != 0) return qe::FromNumber(0);
      if (op == ProcessorInput) return qe::FromNumber(engine->ProcessorInput(id(2), static_cast<int>(num(3)), data, count) ? 1 : 0);
      if (op == ProcessorOutput) engine->ProcessorOutput(id(2), static_cast<int>(num(3)), data, count);
      else if (op == AnalyserTime) engine->AnalyserTimeData(id(2), data, count);
      else engine->AnalyserFrequencyData(id(2), data, count);
      break;
    }
    case ProcessorDone: engine->ProcessorDone(id(2)); break;
    case SetBuffer: {
      // (context, node, rate, frames, [Float32Array...]) — no array: no buffer.
      if (args.size() < 6) {
        engine->SetBuffer(id(2), nullptr);
        break;
      }
      const double length = qe::ToNumber(ctx, qe::Get(ctx, args[5], "length"));
      auto buffer = std::make_shared<audio::Buffer>();
      buffer->rate = num(3);
      buffer->frames = static_cast<int>(num(4));
      const int channels = static_cast<int>(std::min<double>(length, audio::kMostChannels));
      buffer->channels = channels;
      for (int c = 0; c < channels; ++c) {
        std::vector<float> samples(static_cast<size_t>(std::max(0, buffer->frames)), 0.f);
        const Value channel = qe::GetIndex(ctx, args[5], static_cast<uint32_t>(c));
        if (const auto bytes = qe::BytesOf(channel)) std::memcpy(samples.data(), bytes->data(), std::min(bytes->size(), samples.size() * sizeof(float)));
        buffer->data.push_back(std::move(samples));
      }
      engine->SetBuffer(id(2), buffer);
      break;
    }
    case RenderOffline: {
      // (context, frames, [Float32Array...]): the sound is made and copied into the arrays.
      if (args.size() < 4) return qe::FromNumber(0);
      const int frames = static_cast<int>(std::clamp(num(2), 0.0, 4.0e7));
      const audio::Buffer rendered = engine->RenderOffline(frames);
      for (int c = 0; c < rendered.channels; ++c) {
        const Value channel = qe::GetIndex(ctx, args[3], static_cast<uint32_t>(c));
        if (const auto bytes = qe::MutableBytesOf(channel)) std::memcpy(bytes->data(), rendered.data[static_cast<size_t>(c)].data(), std::min(bytes->size(), rendered.data[static_cast<size_t>(c)].size() * sizeof(float)));
      }
      return qe::FromNumber(rendered.frames);
    }
    default: break;
  }
  return qe::FromNumber(0);
}

// __solarAudioDecode(bytes, rate): [ArrayBuffer of floats for each channel] or null.
Value Decode(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 2) return qe::Null();
  const auto bytes = qe::BytesOf(args[0]);
  if (!bytes) return qe::Null();
  audio::Buffer buffer;
  if (!audio::DecodeAudio(bytes->data(), bytes->size(), qe::ToNumber(ctx, args[1]), buffer)) return qe::Null();
  std::vector<Value> channels;
  for (const auto& data : buffer.data) {
    Value array = qe::NewArrayBuffer(ctx, data.size() * sizeof(float));
    if (auto out = qe::MutableBytesOf(array)) std::memcpy(out->data(), data.data(), data.size() * sizeof(float));
    channels.push_back(array);
  }
  return qe::NewArray(ctx, qe::Args(channels.data(), channels.size()));
}

}  // namespace

void InstallWebAudioNatives(qe::Realm& realm) {
  qe::DefineGlobalFunction(realm.GetContext(), "__solarAudio", Call, 8);
  qe::DefineGlobalFunction(realm.GetContext(), "__solarAudioDecode", Decode, 2);
}

}  // namespace solar::webapi
