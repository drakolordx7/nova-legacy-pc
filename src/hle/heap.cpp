#include "hle/heap.h"

#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "core/log.h"

namespace nova::hle::heap {
namespace {
constexpr gsize kHeader = 16;
constexpr gsize kChunk = 8ull << 20;          // chunk of the heap zone
constexpr gsize kMaxSmall = 1ull << 20;       // above this, map pages directly
constexpr std::uint64_t kBigBit = 1ull << 63;  // set in the header capacity of directly mapped blocks

std::mutex g_mu;
gaddr g_cur = 0, g_end = 0;
std::unordered_map<gsize, std::vector<gaddr>> g_free;  // capacity -> payload addresses

gsize ClassOf(gsize n) {
  if (n < 16) n = 16;
  if (n <= 1024) return (n + 15) & ~15ull;
  gsize top = 1;
  while ((top << 1) <= n) top <<= 1;
  const gsize step = top / 4;
  return (n + step - 1) / step * step;
}

gaddr AllocLocked(gsize size) {
  if (size > kMaxSmall) {
    const gsize total = (size + kHeader + kPageSize - 1) & ~(kPageSize - 1);
    const gaddr base = GuestMemory::MapAnywhere(Zone::Heap, total, kProtRW, "malloc-big");
    if (!base) return 0;
    const gaddr p = base + kHeader;
    mem::W64(p - 16, (total - kHeader) | kBigBit);
    mem::W64(p - 8, p);
    return p;
  }
  const gsize cap = ClassOf(size);
  auto& fl = g_free[cap];
  if (!fl.empty()) {
    const gaddr p = fl.back();
    fl.pop_back();
    return p;
  }
  const gsize need = cap + kHeader;
  if (g_cur + need > g_end) {
    const gaddr c = GuestMemory::MapAnywhere(Zone::Heap, kChunk, kProtRW, "malloc");
    if (!c) return 0;
    g_cur = c;
    g_end = c + kChunk;
  }
  const gaddr p = g_cur + kHeader;
  g_cur += need;
  mem::W64(p - 16, cap);
  mem::W64(p - 8, p);
  return p;
}

void FreeLocked(gaddr p) {
  const gaddr orig = mem::R64(p - 8);
  const std::uint64_t cap = mem::R64(orig - 16);
  if (cap & kBigBit) {
    const gsize total = (cap & ~kBigBit) + kHeader;
    GuestMemory::Unmap(orig - kHeader, total);
    return;
  }
  g_free[cap].push_back(orig);
}
}  // namespace

gaddr Alloc(gsize size) {
  std::lock_guard lk(g_mu);
  return AllocLocked(size);
}

gaddr AllocAligned(gsize align, gsize size) {
  if (align <= 16) return Alloc(size);
  std::lock_guard lk(g_mu);
  const gaddr raw = AllocLocked(size + align + kHeader);
  if (!raw) return 0;
  const gaddr p = (raw + kHeader + align - 1) & ~(align - 1);
  const std::uint64_t rawcap = mem::R64(raw - 16);
  mem::W64(p - 16, (rawcap & ~kBigBit) - (p - raw));  // usable bytes from p (not flagged big: freeing goes via orig)
  mem::W64(p - 8, raw);
  return p;
}

gaddr Calloc(gsize n, gsize size) {
  if (size && n > (~0ull) / size) return 0;
  const gsize total = n * size;
  const gaddr p = Alloc(total);
  if (p) std::memset(g2h(p), 0, total);
  return p;
}

void Free(gaddr p) {
  if (!p) return;
  std::lock_guard lk(g_mu);
  FreeLocked(p);
}

gsize UsableSize(gaddr p) {
  if (!p) return 0;
  return mem::R64(p - 16) & ~kBigBit;
}

gaddr Realloc(gaddr p, gsize size) {
  if (!p) return Alloc(size);
  if (size == 0) {
    Free(p);
    return 0;
  }
  const gsize usable = UsableSize(p);
  if (size <= usable) return p;
  const gaddr n = Alloc(size);
  if (!n) return 0;
  std::memcpy(g2h(n), g2h(p), usable);
  Free(p);
  return n;
}
}  // namespace nova::hle::heap
