/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/gui/FrameBuffer.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <string>
#include <utility>

namespace solar::gui {

SharedMemory::~SharedMemory() { Reset(); }

SharedMemory::SharedMemory(SharedMemory&& other) noexcept : fd_(std::exchange(other.fd_, -1)), data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}

SharedMemory& SharedMemory::operator=(SharedMemory&& other) noexcept {
  if (this != &other) {
    Reset();
    fd_ = std::exchange(other.fd_, -1);
    data_ = std::exchange(other.data_, nullptr);
    size_ = std::exchange(other.size_, 0);
  }
  return *this;
}

void SharedMemory::Reset() {
  if (data_) ::munmap(data_, size_);
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
  data_ = nullptr;
  size_ = 0;
}

bool SharedMemory::Create(size_t size) {
  Reset();
  if (size == 0) return false;
#if defined(__linux__)
  const int fd = ::memfd_create("solar-frame", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd < 0) return false;
  // (Sealed: the memory cannot be made smaller by whoever else has it, which would fault the one reading it.)
  if (::ftruncate(fd, static_cast<off_t>(size)) != 0 || ::fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) != 0) {
    ::close(fd);
    return false;
  }
#else
  // (Elsewhere on POSIX: a named piece of shared memory that has its name taken away at once, so that only the descriptors reach it. It cannot be sealed: the reader maps
  // what it is told the size of and the sender is the browser's own process.)
  static std::atomic<unsigned> counter{0};
  const std::string name = "/solar-" + std::to_string(::getpid()) + "-" + std::to_string(counter.fetch_add(1));
  const int fd = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd < 0) return false;
  ::shm_unlink(name.c_str());
  ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
    ::close(fd);
    return false;
  }
#endif
  void* mapped = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapped == MAP_FAILED) {
    ::close(fd);
    return false;
  }
  fd_ = fd;
  data_ = static_cast<uint8_t*>(mapped);
  size_ = size;
  return true;
}

bool SharedMemory::Adopt(int fd, size_t size, bool writable) {
  Reset();
  // (The memory has to be as large as it was said to be: a smaller one would be read past its end.)
  struct stat info;
#if defined(__linux__)
  const int seals = fd >= 0 ? ::fcntl(fd, F_GET_SEALS) : -1;
  const int needed = F_SEAL_SHRINK | F_SEAL_GROW;
  const bool sealed = seals >= 0 && (seals & needed) == needed;
#else
  const bool sealed = fd >= 0;  // (Memory is not sealed elsewhere: the size is looked at, and the one that sends it is the browser's own process.)
#endif
  if (size == 0 || !sealed || ::fstat(fd, &info) != 0 || static_cast<size_t>(info.st_size) < size) {
    ::close(fd);
    return false;
  }
  void* mapped = ::mmap(nullptr, size, writable ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, fd, 0);
  if (mapped == MAP_FAILED) {
    ::close(fd);
    return false;
  }
  fd_ = fd;
  data_ = static_cast<uint8_t*>(mapped);
  size_ = size;
  return true;
}

}  // namespace solar::gui
