// AAssetManager / AAsset / AAssetDir over the APK's assets/ folder. Handles are small guest heap blocks (the game
// treats them as opaque pointers); the host state sits in tables keyed by the handle's guest address.
// AAsset_getBuffer hands out a guest heap buffer that lives until AAsset_close.
#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "android/android.h"
#include "android/internal.h"
#include "android/zip.h"
#include "core/log.h"
#include "cpu/hle_abi.h"
#include "hle/heap.h"

namespace nova::android {
namespace {
std::mutex g_mu;
std::string g_apk_path;
std::unique_ptr<ZipArchive> g_zip;
bool g_zip_tried = false;
gaddr g_manager = 0;

struct Asset {
  const ZipArchive::Entry* entry = nullptr;
  std::int64_t data_off = 0;                              // stored entries: where the bytes start in the APK
  std::shared_ptr<const std::vector<std::uint8_t>> mem;   // deflated entries: the inflated bytes
  std::int64_t pos = 0;
  gaddr buffer = 0;  // AAsset_getBuffer result
};
struct Dir {
  std::vector<std::string> names;
  std::size_t cursor = 0;
  gaddr name_buf = 0;  // guest copy of the name last returned
};
std::unordered_map<gaddr, Asset> g_assets;
std::unordered_map<gaddr, Dir> g_dirs;

constexpr std::size_t kNameBuf = 512;

// Caller holds g_mu.
ZipArchive* Zip() {
  if (!g_zip && !g_zip_tried) {
    g_zip_tried = true;
    if (g_apk_path.empty()) log::Warn("apk: no APK path set (nova::android::SetApkPath); every asset lookup fails");
    else g_zip = ZipArchive::Open(g_apk_path);
  }
  return g_zip.get();
}

std::string AssetName(const char* name) {
  std::string n = name ? name : "";
  std::replace(n.begin(), n.end(), '\\', '/');
  while (!n.empty() && n.front() == '/') n.erase(0, 1);
  return "assets/" + n;
}

gaddr NewHandle() {
  const gaddr h = hle::heap::Alloc(16);
  if (h) {
    mem::W64(h, 0x41535345544e4f56ull);  // tag, never read back
    mem::W64(h + 8, 0);
  }
  return h;
}

std::uint64_t H_fromJava(std::uint64_t /*env*/, std::uint64_t /*asset_manager*/) { return AssetManagerHandle(); }

// AAsset* AAssetManager_open(AAssetManager*, const char* filename, int mode)
std::uint64_t H_open(std::uint64_t /*mgr*/, const char* name, std::int32_t /*mode*/) {
  if (!name) return 0;
  std::lock_guard lk(g_mu);
  ZipArchive* z = Zip();
  if (!z) return 0;
  const ZipArchive::Entry* e = z->Find(AssetName(name));
  if (!e) return 0;
  Asset a;
  a.entry = e;
  if (e->method == 0) {
    a.data_off = z->DataOffset(*e);
    if (a.data_off < 0) return 0;
  } else {
    a.mem = z->Load(*e);
    if (!a.mem) return 0;
  }
  const gaddr h = NewHandle();
  if (!h) return 0;
  g_assets.emplace(h, std::move(a));
  return h;
}

// int AAsset_read(AAsset*, void* buf, size_t count): bytes read, 0 at the end, < 0 on error
std::int32_t H_read(std::uint64_t asset, void* buf, std::uint64_t count) {
  std::lock_guard lk(g_mu);
  const auto it = g_assets.find(asset);
  if (it == g_assets.end() || (!buf && count)) return -1;
  Asset& a = it->second;
  const std::int64_t left = static_cast<std::int64_t>(a.entry->usize) - a.pos;
  if (left <= 0) return 0;
  const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(std::min<std::uint64_t>(count, 0x7FFFFFFF), left));
  if (a.mem) {
    std::memcpy(buf, a.mem->data() + a.pos, n);
  } else {
    const std::int64_t got = g_zip ? g_zip->ReadRaw(buf, n, a.data_off + a.pos) : -1;
    if (got < 0) return -1;
    a.pos += got;
    return static_cast<std::int32_t>(got);
  }
  a.pos += static_cast<std::int64_t>(n);
  return static_cast<std::int32_t>(n);
}

// off_t AAsset_seek(AAsset*, off_t offset, int whence): the new position or -1
std::int64_t H_seek(std::uint64_t asset, std::int64_t offset, std::int32_t whence) {
  std::lock_guard lk(g_mu);
  const auto it = g_assets.find(asset);
  if (it == g_assets.end()) return -1;
  Asset& a = it->second;
  std::int64_t base = 0;
  if (whence == 1) base = a.pos;
  else if (whence == 2) base = a.entry->usize;
  else if (whence != 0) return -1;
  const std::int64_t np = base + offset;
  if (np < 0 || np > static_cast<std::int64_t>(a.entry->usize)) return -1;
  a.pos = np;
  return np;
}

std::int64_t H_getLength(std::uint64_t asset) {
  std::lock_guard lk(g_mu);
  const auto it = g_assets.find(asset);
  return it == g_assets.end() ? 0 : static_cast<std::int64_t>(it->second.entry->usize);
}

std::int64_t H_getRemainingLength(std::uint64_t asset) {
  std::lock_guard lk(g_mu);
  const auto it = g_assets.find(asset);
  return it == g_assets.end() ? 0 : static_cast<std::int64_t>(it->second.entry->usize) - it->second.pos;
}

// const void* AAsset_getBuffer(AAsset*): the whole asset in guest memory, valid until AAsset_close
std::uint64_t H_getBuffer(std::uint64_t asset) {
  std::lock_guard lk(g_mu);
  const auto it = g_assets.find(asset);
  if (it == g_assets.end() || !g_zip) return 0;
  Asset& a = it->second;
  if (a.buffer) return a.buffer;
  const std::size_t size = a.entry->usize;
  const gaddr buf = hle::heap::Alloc(size ? size : 1);
  if (!buf) return 0;
  if (a.mem) {
    if (size) std::memcpy(g2h(buf), a.mem->data(), size);
  } else if (size && g_zip->ReadRaw(g2h(buf), size, a.data_off) != static_cast<std::int64_t>(size)) {
    hle::heap::Free(buf);
    return 0;
  }
  a.buffer = buf;
  return buf;
}

void H_close(std::uint64_t asset) {
  std::lock_guard lk(g_mu);
  const auto it = g_assets.find(asset);
  if (it == g_assets.end()) return;
  if (it->second.buffer) hle::heap::Free(it->second.buffer);
  g_assets.erase(it);
  hle::heap::Free(asset);
}

// AAssetDir* AAssetManager_openDir(AAssetManager*, const char* dirName): the files directly inside it
std::uint64_t H_openDir(std::uint64_t /*mgr*/, const char* name) {
  std::lock_guard lk(g_mu);
  Dir d;
  if (ZipArchive* z = Zip()) {
    std::string dir = AssetName(name ? name : "");
    while (!dir.empty() && dir.back() == '/') dir.pop_back();
    d.names = z->ListFiles(dir);
  }
  d.name_buf = hle::heap::Alloc(kNameBuf);
  const gaddr h = NewHandle();
  if (!h || !d.name_buf) return 0;
  g_dirs.emplace(h, std::move(d));
  return h;
}

// const char* AAssetDir_getNextFileName(AAssetDir*): NULL after the last one; the string lives until the next call
std::uint64_t H_dirNext(std::uint64_t dir) {
  std::lock_guard lk(g_mu);
  const auto it = g_dirs.find(dir);
  if (it == g_dirs.end()) return 0;
  Dir& d = it->second;
  if (d.cursor >= d.names.size()) return 0;
  const std::string& n = d.names[d.cursor++];
  const std::size_t len = std::min(n.size(), kNameBuf - 1);
  std::memcpy(g2h(d.name_buf), n.data(), len);
  g2h(d.name_buf)[len] = 0;
  return d.name_buf;
}

void H_dirRewind(std::uint64_t dir) {
  std::lock_guard lk(g_mu);
  const auto it = g_dirs.find(dir);
  if (it != g_dirs.end()) it->second.cursor = 0;
}

void H_dirClose(std::uint64_t dir) {
  std::lock_guard lk(g_mu);
  const auto it = g_dirs.find(dir);
  if (it == g_dirs.end()) return;
  hle::heap::Free(it->second.name_buf);
  g_dirs.erase(it);
  hle::heap::Free(dir);
}
}  // namespace

void SetApkPath(const std::string& path) {
  std::lock_guard lk(g_mu);
  g_apk_path = path;
  // Open assets point into the old archive's entry table, so it is parked instead of destroyed.
  static std::vector<std::unique_ptr<ZipArchive>> retired;
  if (g_zip) retired.push_back(std::move(g_zip));
  g_zip_tried = false;
}

gaddr AssetManagerHandle() {
  std::lock_guard lk(g_mu);
  if (!g_manager) g_manager = NewHandle();
  return g_manager;
}

void RegisterAssets() {
  NOVA_HLE("AAssetManager_fromJava", H_fromJava);
  NOVA_HLE("AAssetManager_open", H_open);
  NOVA_HLE("AAssetManager_openDir", H_openDir);
  NOVA_HLE("AAsset_read", H_read);
  NOVA_HLE("AAsset_seek", H_seek);
  NOVA_HLE("AAsset_seek64", H_seek);
  NOVA_HLE("AAsset_getLength", H_getLength);
  NOVA_HLE("AAsset_getLength64", H_getLength);
  NOVA_HLE("AAsset_getRemainingLength", H_getRemainingLength);
  NOVA_HLE("AAsset_getRemainingLength64", H_getRemainingLength);
  NOVA_HLE("AAsset_getBuffer", H_getBuffer);
  NOVA_HLE("AAsset_close", H_close);
  NOVA_HLE("AAssetDir_getNextFileName", H_dirNext);
  NOVA_HLE("AAssetDir_rewind", H_dirRewind);
  NOVA_HLE("AAssetDir_close", H_dirClose);
}
}  // namespace nova::android
