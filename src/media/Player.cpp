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

#include "PlayerImpl.h"

namespace solar::media {

AudioOutputFactory& Factory() {
  static AudioOutputFactory factory;
  return factory;
}
std::atomic<int> g_playing{0};
std::atomic<uint64_t> g_frames{0};

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

Player::Player() : impl_(std::make_unique<Impl>()) { impl_->owner = this; }

Player::~Player() {
  {
    std::lock_guard lock(impl_->m);
    impl_->stop = true;
  }
  impl_->cv.notify_all();
  if (impl_->stream) impl_->stream->Poke();
  if (impl_->mse) impl_->mse->SetWake(nullptr);
  if (impl_->thread.joinable()) impl_->thread.join();
  if (playing_.exchange(false)) --g_playing;
}

std::shared_ptr<Player> Player::CreateMse(std::shared_ptr<MseSource> source, std::function<void()> changed) {
  std::shared_ptr<Player> player(new Player());
  Impl* impl = player->impl_.get();
  impl->mse = std::move(source);
  impl->changed = std::move(changed);
  impl->mse->SetWake([impl] { impl->cv.notify_all(); });
  impl->thread = std::thread([impl] { impl->RunMse(); });
  return player;
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
  if (impl_->stream) impl_->stream->Poke();
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
  if (impl_->stream) impl_->stream->Poke();
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
  if (impl_->stream) impl_->stream->Poke();
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
