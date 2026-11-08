/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/web-api/Storage.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace solar::webapi {

namespace fs = std::filesystem;

namespace {

std::string HexOf(const std::string& text) {
  uint64_t hash = 1469598103934665603ull;  // FNV-1a
  for (unsigned char c : text) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  char buffer[17];
  std::snprintf(buffer, sizeof buffer, "%016llx", static_cast<unsigned long long>(hash));
  return buffer;
}

const char* ExtensionOf(StorageArea area) {
  switch (area) {
    case StorageArea::IndexedDb: return ".idb";
    case StorageArea::Cache: return ".cache";
    case StorageArea::Kv: return ".kv";
  }
  return "";
}

// What names an origin as a directory: its hash, and what of the origin can be read in a name.
std::string OriginDirectory(const std::string& origin) {
  std::string readable;
  for (char c : origin) {
    if (readable.size() >= 40) break;
    readable += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-') ? c : '_';
  }
  return HexOf(origin) + "-" + readable;
}

std::atomic<StorageBackend*> g_backend{nullptr};

}  // namespace

std::string StorageId(StorageArea area, const std::string& name) { return HexOf(name) + ExtensionOf(area); }

bool ValidStorageId(StorageArea area, const std::string& id) {
  const std::string extension = ExtensionOf(area);
  if (id.size() != 16 + extension.size() || id.compare(16, std::string::npos, extension) != 0) return false;
  for (size_t i = 0; i < 16; ++i) {
    if (!std::isxdigit(static_cast<unsigned char>(id[i])) || std::isupper(static_cast<unsigned char>(id[i]))) return false;
  }
  return true;
}

std::string StorageDirectory() {
  if (const char* given = std::getenv("SOLAR_IDB_DIR")) return given;
  fs::path base;
#if defined(_WIN32)
  if (const char* local = std::getenv("LOCALAPPDATA")) base = local;
#elif defined(__APPLE__)
  if (const char* home = std::getenv("HOME")) base = fs::path(home) / "Library" / "Application Support";
#else
  if (const char* xdg = std::getenv("XDG_DATA_HOME")) base = xdg;
  else if (const char* home = std::getenv("HOME")) base = fs::path(home) / ".local" / "share";
#endif
  if (base.empty()) return "";
  return (base / "solar" / "indexeddb").string();
}

StorageBackend& CurrentStorage() {
  if (StorageBackend* set = g_backend.load()) return *set;
  static DiskStorage disk(StorageDirectory());
  return disk;
}

void SetStorage(StorageBackend* backend) { g_backend.store(backend); }

std::string DiskStorage::Directory(StorageArea area, const std::string& origin) const {
  if (base_.empty()) return "";
  const char* suffix = area == StorageArea::IndexedDb ? "" : area == StorageArea::Cache ? "-cache" : "-kv";
  return (fs::path(base_ + suffix) / OriginDirectory(origin)).string();
}

std::optional<std::string> DiskStorage::Read(StorageArea area, const std::string& origin, const std::string& id, size_t maxBytes) {
  const std::string directory = Directory(area, origin);
  if (directory.empty() || !ValidStorageId(area, id)) return std::nullopt;
  std::ifstream in(fs::path(directory) / id, std::ios::binary);
  if (!in) return std::nullopt;
  if (maxBytes == 0) {
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
  }
  std::string head(maxBytes, '\0');
  in.read(head.data(), static_cast<std::streamsize>(maxBytes));
  head.resize(static_cast<size_t>(in.gcount()));
  return head;
}

bool DiskStorage::Write(StorageArea area, const std::string& origin, const std::string& id, std::string_view bytes) {
  const std::string directory = Directory(area, origin);
  if (directory.empty() || !ValidStorageId(area, id) || bytes.size() > kLargestFile) return false;
  std::error_code error;
  fs::create_directories(directory, error);
  if (error) return false;
  // What the origin keeps is bounded: the files it has (less the one this replaces) and this one are not more than the most.
  uint64_t used = 0;
  for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
    if (it->path().filename() == id || it->path().extension() == ".tmp") continue;
    std::error_code size;
    const auto bytesOf = it->file_size(size);
    if (!size) used += bytesOf;
  }
  if (used + bytes.size() > quota_) return false;
  const fs::path path = fs::path(directory) / id;
  const fs::path temporary = path.string() + ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) return false;
  }
  fs::rename(temporary, path, error);
  return !error;
}

bool DiskStorage::Remove(StorageArea area, const std::string& origin, const std::string& id) {
  const std::string directory = Directory(area, origin);
  if (directory.empty() || !ValidStorageId(area, id)) return false;
  std::error_code error;
  fs::remove(fs::path(directory) / id, error);
  return !error;
}

std::vector<std::string> DiskStorage::List(StorageArea area, const std::string& origin) {
  std::vector<std::string> ids;
  const std::string directory = Directory(area, origin);
  if (directory.empty()) return ids;
  std::error_code error;
  for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
    const std::string name = it->path().filename().string();
    if (ValidStorageId(area, name)) ids.push_back(name);
  }
  return ids;
}

}  // namespace solar::webapi
