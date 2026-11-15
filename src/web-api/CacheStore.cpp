/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/web-api/CacheStore.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "solar/web-api/IndexedDb.h"
#include "solar/web-api/Storage.h"

namespace solar::webapi {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::Value;

namespace {

struct Entry {
  std::string meta;  // JSON, made and read by the script
  // The body is in memory (an entry put since the file was written) or is in the file of the cache, from `offset` for `length` bytes, and is read when asked for: a service worker's cache
  // holds megabytes of scripts (YouTube's: 55 MB, and a script is kept in it more than once), which were all in memory as soon as the page touched caches.
  std::string body;
  bool onDisk = false;
  uint64_t offset = 0, length = 0;
};

struct Cache {
  std::vector<Entry> entries;
  bool dirty = false;
  std::string fileId;  // of the file the entries that are onDisk are in
};

struct Origin {
  bool loaded = false;
  std::vector<std::string> order;  // names, as they were made
  std::map<std::string, Cache> caches;
  std::set<std::string> removed;   // files to delete at the next flush
};

std::mutex g_mutex;
std::map<std::string, Origin>& Origins() {
  static std::map<std::string, Origin> origins;
  return origins;
}

constexpr StorageArea kCache = StorageArea::Cache;
constexpr StorageArea kKv = StorageArea::Kv;

bool Persistent(const std::string& origin) { return origin != "null" && !origin.empty() && !StorageDirectory().empty(); }

void PutU32(std::string& out, uint32_t v) { out.append(reinterpret_cast<const char*>(&v), 4); }
void PutU64(std::string& out, uint64_t v) { out.append(reinterpret_cast<const char*>(&v), 8); }

// Reads a file in pieces, so that the bodies in it, which are what it is made of, are not read when only the places of them are wanted.
class FileReader {
 public:
  FileReader(const std::string& origin, const std::string& id) : origin_(origin), id_(id) {}
  // `n` bytes at `at`; false if the file ends before.
  bool Bytes(uint64_t at, size_t n, std::string& out) {
    if (at >= start_ && at + n <= start_ + window_.size()) {
      out.assign(window_, static_cast<size_t>(at - start_), n);
      return true;
    }
    constexpr size_t kPiece = 64 * 1024;
    std::optional<std::string> got = CurrentStorage().ReadAt(kCache, origin_, id_, at, std::max(n, kPiece));
    if (!got || got->size() < n) return false;
    start_ = at;
    window_ = std::move(*got);
    out.assign(window_, 0, n);
    return true;
  }
  bool U32(uint64_t& at, uint32_t& v) {
    std::string b;
    if (!Bytes(at, 4, b)) return false;
    std::memcpy(&v, b.data(), 4);
    at += 4;
    return true;
  }
  bool U64(uint64_t& at, uint64_t& v) {
    std::string b;
    if (!Bytes(at, 8, b)) return false;
    std::memcpy(&v, b.data(), 8);
    at += 8;
    return true;
  }

 private:
  std::string origin_, id_;
  std::string window_;
  uint64_t start_ = 0;
};

// One file of a cache: "SCH1", the name, the number of entries, and each entry's meta and body. Only the metas are read here; a body is read when it is asked for.
void Load(const std::string& origin, Origin& o) {
  if (o.loaded) return;
  o.loaded = true;
  if (!Persistent(origin)) return;
  std::vector<std::pair<uint64_t, std::pair<std::string, Cache>>> found;
  for (const std::string& id : CurrentStorage().List(kCache, origin)) {
    FileReader file(origin, id);
    std::string magic;
    if (!file.Bytes(0, 4, magic) || magic != "SCH1") continue;
    uint64_t at = 4;
    uint64_t made = 0;
    uint32_t nameLen = 0, count = 0;
    std::string name;
    if (!file.U64(at, made) || !file.U32(at, nameLen) || (nameLen > 0 && !file.Bytes(at, nameLen, name))) continue;
    at += nameLen;
    if (!file.U32(at, count)) continue;
    Cache cache;
    cache.fileId = id;
    bool good = true;
    for (uint32_t i = 0; i < count && good; ++i) {
      uint32_t metaLen = 0;
      uint64_t bodyLen = 0;
      good = file.U32(at, metaLen);
      if (!good) break;
      Entry e;
      good = metaLen == 0 || file.Bytes(at, metaLen, e.meta);
      if (!good) break;
      at += metaLen;
      good = file.U64(at, bodyLen);
      if (!good) break;
      e.onDisk = true;
      e.offset = at;
      e.length = bodyLen;
      at += bodyLen;
      cache.entries.push_back(std::move(e));
    }
    // (The file ends where the last body does, or it is cut short and is not taken.)
    std::string last;
    if (good && at > 0 && count > 0 && !file.Bytes(at - 1, 1, last)) good = false;
    if (good) found.push_back({made, {name, std::move(cache)}});
  }
  std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  for (auto& [made, named] : found) {
    o.order.push_back(named.first);
    o.caches[named.first] = std::move(named.second);
  }
}

Origin& Get(const std::string& origin) {
  Origin& o = Origins()[origin];
  Load(origin, o);
  return o;
}

std::string Escape(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
    else out += static_cast<char>(c);
  }
  return out + "\"";
}

// ---- natives ----

Value Names(Context& ctx, Value, qe::Args args, Value) {
  std::lock_guard lock(g_mutex);
  const std::string origin = args.empty() ? "" : qe::ToWtf8(ctx, args[0]);
  std::string json = "[";
  bool first = true;
  for (const std::string& name : Get(origin).order) {
    json += (first ? "" : ",") + Escape(name);
    first = false;
  }
  return qe::FromWtf8(ctx, json + "]");
}

// __solarCacheOpen(origin, name): makes the cache if there is none.
Value Open(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 2) return qe::Undefined();
  std::lock_guard lock(g_mutex);
  Origin& o = Get(qe::ToWtf8(ctx, args[0]));
  const std::string name = qe::ToWtf8(ctx, args[1]);
  if (!o.caches.count(name)) {
    o.order.push_back(name);
    o.caches[name].dirty = true;
    o.removed.erase(name);
  }
  return qe::Undefined();
}

Value Has(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 2) return qe::FromBool(false);
  std::lock_guard lock(g_mutex);
  return qe::FromBool(Get(qe::ToWtf8(ctx, args[0])).caches.count(qe::ToWtf8(ctx, args[1])) > 0);
}

Value Delete(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 2) return qe::FromBool(false);
  std::lock_guard lock(g_mutex);
  Origin& o = Get(qe::ToWtf8(ctx, args[0]));
  const std::string name = qe::ToWtf8(ctx, args[1]);
  if (!o.caches.erase(name)) return qe::FromBool(false);
  o.order.erase(std::find(o.order.begin(), o.order.end(), name));
  o.removed.insert(name);
  return qe::FromBool(true);
}

// __solarCacheMetas(origin, name): the metas of the entries, as a JSON array text (the metas are JSON themselves).
Value Metas(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 2) return qe::Undefined();
  std::lock_guard lock(g_mutex);
  Origin& o = Get(qe::ToWtf8(ctx, args[0]));
  const auto found = o.caches.find(qe::ToWtf8(ctx, args[1]));
  if (found == o.caches.end()) return qe::Undefined();
  std::string json = "[";
  for (size_t i = 0; i < found->second.entries.size(); ++i) json += (i ? "," : "") + found->second.entries[i].meta;
  return qe::FromWtf8(ctx, json + "]");
}

Value Body(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 3) return qe::Undefined();
  std::lock_guard lock(g_mutex);
  Origin& o = Get(qe::ToWtf8(ctx, args[0]));
  const auto found = o.caches.find(qe::ToWtf8(ctx, args[1]));
  const size_t index = static_cast<size_t>(qe::ToNumber(ctx, args[2]));
  if (found == o.caches.end() || index >= found->second.entries.size()) return qe::Undefined();
  const Entry& entry = found->second.entries[index];
  std::optional<std::string> read;
  if (entry.onDisk) read = CurrentStorage().ReadAt(kCache, qe::ToWtf8(ctx, args[0]), found->second.fileId, entry.offset, static_cast<size_t>(entry.length));
  const std::string& body = entry.onDisk ? (read ? *read : std::string()) : entry.body;
  Value buffer = qe::NewArrayBuffer(ctx, body.size());
  if (!body.empty()) {
    if (auto out = qe::MutableBytesOf(buffer)) std::memcpy(out->data(), body.data(), body.size());
  }
  return buffer;
}

// __solarCachePut(origin, name, meta, bytes, replace): `replace` is the index of the entry that this one takes the place of, or -1.
Value Put(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 5) return qe::Undefined();
  std::lock_guard lock(g_mutex);
  Origin& o = Get(qe::ToWtf8(ctx, args[0]));
  const auto found = o.caches.find(qe::ToWtf8(ctx, args[1]));
  if (found == o.caches.end()) return qe::FromBool(false);
  Entry e;
  e.meta = qe::ToWtf8(ctx, args[2]);
  if (auto bytes = qe::BytesOf(args[3])) e.body.assign(reinterpret_cast<const char*>(bytes->data()), bytes->size());
  const int replace = static_cast<int>(qe::ToNumber(ctx, args[4]));
  auto& entries = found->second.entries;
  if (replace >= 0 && static_cast<size_t>(replace) < entries.size()) entries.erase(entries.begin() + replace);
  entries.push_back(std::move(e));
  found->second.dirty = true;
  return qe::FromBool(true);
}

Value Remove(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 3) return qe::Undefined();
  std::lock_guard lock(g_mutex);
  Origin& o = Get(qe::ToWtf8(ctx, args[0]));
  const auto found = o.caches.find(qe::ToWtf8(ctx, args[1]));
  const size_t index = static_cast<size_t>(qe::ToNumber(ctx, args[2]));
  if (found == o.caches.end() || index >= found->second.entries.size()) return qe::Undefined();
  found->second.entries.erase(found->second.entries.begin() + index);
  found->second.dirty = true;
  return qe::Undefined();
}

// __solarCacheFlush(): what changed is written, and the files of what was deleted are removed.
Value Flush(Context&, Value, qe::Args, Value) {
  std::lock_guard lock(g_mutex);
  for (auto& [origin, o] : Origins()) {
    if (!Persistent(origin)) continue;
    for (const std::string& name : o.removed) CurrentStorage().Remove(kCache, origin, StorageId(kCache, name));
    o.removed.clear();
    uint64_t made = 0;
    for (const std::string& name : o.order) {
      ++made;
      Cache& cache = o.caches[name];
      if (!cache.dirty) continue;
      cache.dirty = false;
      std::string data = "SCH1";
      PutU64(data, made);
      PutU32(data, static_cast<uint32_t>(name.size()));
      data += name;
      PutU32(data, static_cast<uint32_t>(cache.entries.size()));
      std::vector<std::pair<uint64_t, uint64_t>> places;  // where each body is in the file that is made
      bool readable = true;
      for (const Entry& e : cache.entries) {
        PutU32(data, static_cast<uint32_t>(e.meta.size()));
        data += e.meta;
        std::optional<std::string> kept;
        if (e.onDisk) {
          kept = CurrentStorage().ReadAt(kCache, origin, cache.fileId, e.offset, static_cast<size_t>(e.length));
          if (!kept || kept->size() != e.length) readable = false;
        }
        const std::string& body = e.onDisk ? (kept ? *kept : std::string()) : e.body;
        PutU64(data, body.size());
        places.push_back({data.size(), body.size()});
        data += body;
      }
      // (A cache that could not be written, or whose bodies could not be read to write it, is tried again with the next flush.)
      const std::string id = StorageId(kCache, name);
      if (!readable || !CurrentStorage().Write(kCache, origin, id, data)) {
        cache.dirty = true;
        continue;
      }
      // What was written is where the bodies are now, and need not be in memory.
      cache.fileId = id;
      for (size_t i = 0; i < cache.entries.size(); ++i) {
        Entry& e = cache.entries[i];
        e.onDisk = true;
        e.offset = places[i].first;
        e.length = places[i].second;
        std::string().swap(e.body);
      }
    }
  }
  return qe::Undefined();
}

// ---- a few keyed texts (a service worker's registrations) ----

Value KvRead(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 2) return qe::Undefined();
  const std::string origin = qe::ToWtf8(ctx, args[0]);
  if (!Persistent(origin)) return qe::Undefined();
  const std::optional<std::string> text = CurrentStorage().Read(kKv, origin, StorageId(kKv, qe::ToWtf8(ctx, args[1])));
  if (!text) return qe::Undefined();
  return qe::FromWtf8(ctx, *text);
}

Value KvWrite(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 3) return qe::FromBool(false);
  const std::string origin = qe::ToWtf8(ctx, args[0]);
  if (!Persistent(origin)) return qe::FromBool(false);
  return qe::FromBool(CurrentStorage().Write(kKv, origin, StorageId(kKv, qe::ToWtf8(ctx, args[1])), qe::ToWtf8(ctx, args[2])));
}

}  // namespace

std::optional<std::string> ReadKv(const std::string& origin, const std::string& key) {
  if (!Persistent(origin)) return std::nullopt;
  return CurrentStorage().Read(kKv, origin, StorageId(kKv, key));
}

bool HasServiceWorkerFor(const std::string& origin, const std::string& url) {
  const std::optional<std::string> kept = ReadKv(origin, "sw-registrations");
  if (!kept) return false;
  const std::string& text = *kept;
  // (The scopes are the "scope" strings of the file; one that begins the address is a registration for it.)
  for (size_t at = text.find("\"scope\":\""); at != std::string::npos; at = text.find("\"scope\":\"", at + 1)) {
    const size_t from = at + 9, end = text.find('"', from);
    if (end == std::string::npos) break;
    if (url.compare(0, end - from, text, from, end - from) == 0) return true;
  }
  return false;
}

void InstallCacheStoreNatives(qe::Realm& realm) {
  Context& ctx = realm.GetContext();
  qe::DefineGlobalFunction(ctx, "__solarCacheNames", Names, 1);
  qe::DefineGlobalFunction(ctx, "__solarCacheOpen", Open, 2);
  qe::DefineGlobalFunction(ctx, "__solarCacheHas", Has, 2);
  qe::DefineGlobalFunction(ctx, "__solarCacheDelete", Delete, 2);
  qe::DefineGlobalFunction(ctx, "__solarCacheMetas", Metas, 2);
  qe::DefineGlobalFunction(ctx, "__solarCacheBody", Body, 3);
  qe::DefineGlobalFunction(ctx, "__solarCachePut", Put, 5);
  qe::DefineGlobalFunction(ctx, "__solarCacheRemove", Remove, 3);
  qe::DefineGlobalFunction(ctx, "__solarCacheFlush", Flush, 0);
  qe::DefineGlobalFunction(ctx, "__solarKvRead", KvRead, 2);
  qe::DefineGlobalFunction(ctx, "__solarKvWrite", KvWrite, 3);
}

}  // namespace solar::webapi
