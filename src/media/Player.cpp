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

namespace {
constexpr uint64_t kChunk = uint64_t{4} << 20;  // what one Range request asks for
constexpr size_t kKeep = size_t{96} << 20;      // what is kept, at most (the part nearest the reader stays)
}  // namespace

void ByteStream::Append(const uint8_t* data, size_t size) {
  {
    std::lock_guard lock(mutex_);
    std::string& chunk = chunks_[0];
    chunk.append(reinterpret_cast<const char*>(data), size);
    appended_ += size;
    kept_ += size;
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

void ByteStream::SetFetcher(Fetcher fetcher) {
  std::lock_guard lock(mutex_);
  fetcher_ = std::move(fetcher);
}

void ByteStream::SetTotal(uint64_t total) {
  {
    std::lock_guard lock(mutex_);
    total_ = total;
  }
  cv_.notify_all();
}

void ByteStream::SetSequential() {
  std::lock_guard lock(mutex_);
  sequential_ = true;
}

void ByteStream::SetAhead(uint64_t bytes) {
  {
    std::unique_lock lock(mutex_);
    ahead_ = bytes;
    PlanLocked(lock);
  }
  cv_.notify_all();
}

void ByteStream::Stop() {
  {
    std::lock_guard lock(mutex_);
    stopped_ = true;
    fetcher_ = nullptr;
  }
  cv_.notify_all();
}

void ByteStream::Poke() { cv_.notify_all(); }

// The first offset at or after `from` that is not kept.
uint64_t ByteStream::CoveredFromLocked(uint64_t from) const {
  uint64_t at = from;
  for (auto it = chunks_.upper_bound(at); ; ) {
    if (it == chunks_.begin()) break;
    auto before = std::prev(it);
    const uint64_t end = before->first + before->second.size();
    if (end > at && before->first <= at) {
      at = end;
      it = chunks_.upper_bound(at);
      continue;
    }
    break;
  }
  return at;
}

// Starts a fetch of [from, from + 4 MB) (the one that is going is let go of): the fetcher is called without the lock.
void ByteStream::Request(std::unique_lock<std::mutex>& lock, uint64_t from) {
  if (!fetcher_ || stopped_) return;
  uint64_t to = from + kChunk;
  if (total_ && to > total_) to = total_;
  if (to <= from) return;
  fetching_ = true;
  fetchStart_ = from;
  fetchEnd_ = to;
  const uint64_t generation = ++generation_;
  Fetcher fetcher = fetcher_;
  lock.unlock();
  fetcher(from, to, generation);
  lock.lock();
}

// With nothing going: the next piece is fetched if the reader has less than 16 MB ahead.
void ByteStream::PlanLocked(std::unique_lock<std::mutex>& lock) {
  if (!fetcher_ || stopped_ || sequential_ || fetching_ || failed_) return;
  const uint64_t gap = CoveredFromLocked(readPos_);
  if (total_ && gap >= total_) return;
  if (gap - readPos_ >= ahead_) return;
  Request(lock, gap);
}

void ByteStream::EvictLocked() {
  if (sequential_ || !fetcher_) return;
  while (kept_ > kKeep) {
    // The chunk farthest from the reader, but not the one it is in, nor the one that is coming.
    auto far = chunks_.end();
    uint64_t farthest = 0;
    for (auto it = chunks_.begin(); it != chunks_.end(); ++it) {
      const uint64_t start = it->first, end = it->first + it->second.size();
      if ((readPos_ >= start && readPos_ < end) || (fetching_ && start == fetchStart_)) continue;
      const uint64_t distance = readPos_ < start ? start - readPos_ : readPos_ - end;
      if (far == chunks_.end() || distance > farthest) {
        far = it;
        farthest = distance;
      }
    }
    if (far == chunks_.end()) return;
    kept_ -= far->second.size();
    chunks_.erase(far);
  }
}

void ByteStream::Put(uint64_t generation, uint64_t offset, const uint8_t* data, size_t size) {
  {
    std::unique_lock lock(mutex_);
    if (generation != generation_ || stopped_) return;
    // The bytes join the chunk that begins where this fetch began (made if there is none).
    std::string& chunk = chunks_[fetchStart_];
    if (offset != fetchStart_ + chunk.size()) return;  // (out of place: dropped)
    chunk.append(reinterpret_cast<const char*>(data), size);
    kept_ += size;
    if (sequential_) appended_ += size;
    EvictLocked();
  }
  cv_.notify_all();
}

void ByteStream::FetchDone(uint64_t generation, bool ok, bool endOfResource) {
  {
    std::unique_lock lock(mutex_);
    if (generation != generation_ || stopped_) return;
    fetching_ = false;
    if (sequential_) {
      finished_ = true;
      failed_ = !ok;
    } else if (endOfResource) {
      if (fetchStart_ > 0 && (!total_ || total_ > fetchStart_)) total_ = fetchStart_;
    } else if (!ok) {
      if (++errors_ >= 3) {
        failed_ = true;
        finished_ = true;
      }
    } else {
      errors_ = 0;
      // The chunk is joined with the one that follows if they meet.
      auto it = chunks_.find(fetchStart_);
      if (it != chunks_.end()) {
        auto next = std::next(it);
        while (next != chunks_.end() && next->first <= it->first + it->second.size()) {
          const uint64_t end = it->first + it->second.size();
          if (next->first + next->second.size() > end) it->second.append(next->second, end - next->first, std::string::npos);
          kept_ -= std::min<size_t>(kept_, next->second.size());
          kept_ += 0;
          next = chunks_.erase(next);
        }
        // (What was counted for the joined chunks is the size of what is there now.)
        kept_ = 0;
        for (const auto& [start, bytes] : chunks_) kept_ += bytes.size();
      }
    }
    PlanLocked(lock);
  }
  cv_.notify_all();
}

int64_t ByteStream::Read(uint64_t offset, uint8_t* out, size_t size, const std::function<bool()>& interrupted, int timeoutMs) {
  std::unique_lock lock(mutex_);
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
  readPos_ = offset;
  for (;;) {
    auto it = chunks_.upper_bound(offset);
    if (it != chunks_.begin()) {
      --it;
      const uint64_t end = it->first + it->second.size();
      if (it->first <= offset && offset < end) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(size, end - offset));
        std::memcpy(out, it->second.data() + (offset - it->first), n);
        readPos_ = offset + n;
        // (The reader has moved on: more is fetched if there is room ahead.)
        if (fetcher_ && !fetching_) PlanLocked(lock);
        return static_cast<int64_t>(n);
      }
    }
    if (failed_ && (finished_ || !fetcher_)) return -3;
    if (fetcher_) {
      if (total_ && offset >= total_) return 0;
      if (!sequential_) {
        // Wanted: a fetch that will bring it. One that is going is left to if it is about to (the offset is at or just ahead of what it has brought).
        const uint64_t brought = fetchStart_ + (chunks_.count(fetchStart_) ? chunks_[fetchStart_].size() : 0);
        const bool coming = fetching_ && offset >= fetchStart_ && offset < std::max(brought, fetchStart_) + (uint64_t{256} << 10) && offset < fetchEnd_;
        if (!coming) Request(lock, offset);
      }
    } else if (finished_) {
      return failed_ ? -3 : 0;
    }
    if (interrupted && interrupted()) return -1;
    if (stopped_) return -1;
    if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
      // (Looked at again, then given up.)
      auto again = chunks_.upper_bound(offset);
      bool there = false;
      if (again != chunks_.begin()) {
        --again;
        there = again->first <= offset && offset < again->first + again->second.size();
      }
      if (!there && !(finished_ && !fetcher_)) return -2;
    }
  }
}

int64_t ByteStream::Size() const {
  std::lock_guard lock(mutex_);
  if (finished_ && !fetcher_) return static_cast<int64_t>(appended_);
  if (total_) return static_cast<int64_t>(total_);
  if (finished_) return static_cast<int64_t>(appended_);
  return -1;
}

bool ByteStream::WaitForSize(const std::function<bool()>& interrupted) {
  std::unique_lock lock(mutex_);
  while (!((finished_ && !fetcher_) || total_ || finished_)) {
    if (interrupted && interrupted()) return false;
    cv_.wait_for(lock, std::chrono::milliseconds(50));
  }
  return true;
}

uint64_t ByteStream::available() const { std::lock_guard lock(mutex_); return appended_; }

std::vector<std::pair<uint64_t, uint64_t>> ByteStream::Coverage() const {
  std::lock_guard lock(mutex_);
  std::vector<std::pair<uint64_t, uint64_t>> out;
  for (const auto& [start, bytes] : chunks_) {
    const uint64_t end = start + bytes.size();
    if (!out.empty() && start <= out.back().second) out.back().second = std::max(out.back().second, end);
    else out.push_back({start, end});
  }
  return out;
}

uint64_t ByteStream::total() const {
  std::lock_guard lock(mutex_);
  if (total_) return total_;
  return finished_ || sequential_ ? appended_ : 0;
}
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

void Player::SetBufferAhead(uint64_t bytes) {
  if (impl_->stream) impl_->stream->SetAhead(bytes);
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

void Player::SetControlsVisible(bool visible) {
  std::lock_guard lock(impl_->m);
  impl_->controlsVisible = visible;
}

void Player::SetLoop(bool loop) {
  std::lock_guard lock(impl_->m);
  impl_->loop = loop;
}

PlayerState Player::State() {
  std::lock_guard lock(impl_->m);
  PlayerState state = impl_->st;
  state.playing = impl_->wantPlay;
  state.volume = impl_->volume;
  state.muted = impl_->muted;
  state.controlsVisible = impl_->controlsVisible;
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
