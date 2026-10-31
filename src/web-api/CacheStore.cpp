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
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "solar/web-api/IndexedDb.h"

namespace solar::webapi {

namespace qe = Quanta::Embed;
using Quanta::Context;
using Quanta::Value;
namespace fs = std::filesystem;

namespace {

struct Entry {
  std::string meta;  // JSON, made and read by the script
  std::string body;
};

struct Cache {
  std::vector<Entry> entries;
  bool dirty = false;
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

fs::path RootOf(const char* kind) { return fs::path(IndexedDbDirectory() + "-" + kind); }

fs::path OriginDir(const char* kind, const std::string& origin) {
  std::string readable;
  for (char c : origin) {
    if (readable.size() >= 40) break;
    readable += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-') ? c : '_';
  }
  return RootOf(kind) / (HexOf(origin) + "-" + readable);
}

bool Persistent(const std::string& origin) { return origin != "null" && !origin.empty() && !IndexedDbDirectory().empty(); }

void PutU32(std::string& out, uint32_t v) { out.append(reinterpret_cast<const char*>(&v), 4); }
void PutU64(std::string& out, uint64_t v) { out.append(reinterpret_cast<const char*>(&v), 8); }

// One file of a cache: "SCH1", the name, the number of entries, and each entry's meta and body.
void Load(const std::string& origin, Origin& o) {
  if (o.loaded) return;
  o.loaded = true;
  if (!Persistent(origin)) return;
  std::error_code error;
  std::vector<std::pair<uint64_t, std::pair<std::string, Cache>>> found;
  for (fs::directory_iterator it(OriginDir("cache", origin), error), end; !error && it != end; it.increment(error)) {
    if (it->path().extension() != ".cache") continue;
    std::ifstream in(it->path(), std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string data = buffer.str();
    size_t at = 0;
    auto u32 = [&](uint32_t& v) { if (at + 4 > data.size()) return false; std::memcpy(&v, data.data() + at, 4); at += 4; return true; };
    auto u64 = [&](uint64_t& v) { if (at + 8 > data.size()) return false; std::memcpy(&v, data.data() + at, 8); at += 8; return true; };
    if (data.compare(0, 4, "SCH1") != 0) continue;
    at = 4;
    uint64_t made = 0;
    uint32_t nameLen = 0, count = 0;
    if (!u64(made) || !u32(nameLen) || at + nameLen > data.size()) continue;
    std::string name = data.substr(at, nameLen);
    at += nameLen;
    if (!u32(count)) continue;
    Cache cache;
    bool good = true;
    for (uint32_t i = 0; i < count && good; ++i) {
      uint32_t metaLen = 0;
      uint64_t bodyLen = 0;
      good = u32(metaLen) && at + metaLen <= data.size();
      if (!good) break;
      Entry e;
      e.meta = data.substr(at, metaLen);
      at += metaLen;
      good = u64(bodyLen) && at + bodyLen <= data.size();
      if (!good) break;
      e.body = data.substr(at, bodyLen);
      at += bodyLen;
      cache.entries.push_back(std::move(e));
    }
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

fs::path FileOf(const std::string& origin, const std::string& name) { return OriginDir("cache", origin) / (HexOf(name) + ".cache"); }

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
  const std::string& body = found->second.entries[index].body;
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
    std::error_code error;
    for (const std::string& name : o.removed) fs::remove(FileOf(origin, name), error);
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
      for (const Entry& e : cache.entries) {
        PutU32(data, static_cast<uint32_t>(e.meta.size()));
        data += e.meta;
        PutU64(data, e.body.size());
        data += e.body;
      }
      fs::create_directories(OriginDir("cache", origin), error);
      const fs::path path = FileOf(origin, name);
      const fs::path temporary = path.string() + ".tmp";
      {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << data;
      }
      fs::rename(temporary, path, error);
    }
  }
  return qe::Undefined();
}

// ---- a few keyed texts (a service worker's registrations) ----

fs::path KvFile(const std::string& origin, const std::string& key) { return OriginDir("kv", origin) / (HexOf(key) + ".kv"); }

Value KvRead(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 2) return qe::Undefined();
  const std::string origin = qe::ToWtf8(ctx, args[0]);
  if (!Persistent(origin)) return qe::Undefined();
  std::ifstream in(KvFile(origin, qe::ToWtf8(ctx, args[1])), std::ios::binary);
  if (!in) return qe::Undefined();
  std::stringstream buffer;
  buffer << in.rdbuf();
  return qe::FromWtf8(ctx, buffer.str());
}

Value KvWrite(Context& ctx, Value, qe::Args args, Value) {
  if (args.size() < 3) return qe::FromBool(false);
  const std::string origin = qe::ToWtf8(ctx, args[0]);
  if (!Persistent(origin)) return qe::FromBool(false);
  const fs::path path = KvFile(origin, qe::ToWtf8(ctx, args[1]));
  std::error_code error;
  fs::create_directories(path.parent_path(), error);
  const fs::path temporary = path.string() + ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out << qe::ToWtf8(ctx, args[2]);
    if (!out) return qe::FromBool(false);
  }
  fs::rename(temporary, path, error);
  return qe::FromBool(!error);
}

}  // namespace

bool HasServiceWorkerFor(const std::string& origin, const std::string& url) {
  if (!Persistent(origin)) return false;
  std::ifstream in(KvFile(origin, "sw-registrations"), std::ios::binary);
  if (!in) return false;
  std::stringstream buffer;
  buffer << in.rdbuf();
  const std::string text = buffer.str();
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
