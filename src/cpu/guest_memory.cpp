#include "cpu/guest_memory.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>

#include "core/log.h"

namespace nova {
std::uint8_t* g_membase = nullptr;

namespace {
constexpr unsigned kLeafBits = 14;  // pages per leaf of the protection table
constexpr gsize kLeafPages = gsize{1} << kLeafBits;
constexpr gsize kTopCount = (kWindowSize / kPageSize) >> kLeafBits;
constexpr std::uint8_t kMapped = 0x80;

std::atomic<std::uint8_t*> g_leaf[kTopCount];  // lazily allocated, one byte per page: kMapped | Prot
std::mutex g_mu;

struct Region {
  gsize size;
  std::uint8_t prot;
  std::string name;
};
std::map<gaddr, Region> g_regions;  // start -> region (informational)

struct ZoneInfo {
  gaddr base, limit, next;
  std::vector<std::pair<gaddr, gsize>> free_list;
};
ZoneInfo g_zones[] = {
    {0x0000'1000'0000ull, 0x0000'2000'0000ull, 0x0000'1000'0000ull, {}},  // Thunks: 256 MiB
    {0x0000'4000'0000ull, 0x0000'8000'0000ull, 0x0000'4000'0000ull, {}},  // Libs: 1 GiB
    {0x0020'0000'0000ull, 0x0030'0000'0000ull, 0x0020'0000'0000ull, {}},  // Heap: 64 GiB
    {0x0040'0000'0000ull, 0x0044'0000'0000ull, 0x0040'0000'0000ull, {}},  // Stacks: 16 GiB
    {0x0050'0000'0000ull, 0x0090'0000'0000ull, 0x0050'0000'0000ull, {}},  // Anon (mmap): 256 GiB
};

constexpr gsize PageUp(gsize v) { return (v + kPageSize - 1) & ~(kPageSize - 1); }

DWORD HostProt(std::uint8_t prot) {
  if (prot & kProtWrite) return PAGE_READWRITE;
  if (prot & (kProtRead | kProtExec)) return PAGE_READONLY;
  return PAGE_NOACCESS;
}

std::uint8_t Entry(gaddr addr) {
  if (addr >= kWindowSize) return 0;
  const gsize page = addr >> 12;
  const std::uint8_t* leaf = g_leaf[page >> kLeafBits].load(std::memory_order_acquire);
  return leaf ? leaf[page & (kLeafPages - 1)] : 0;
}

// Caller holds g_mu.
void SetEntries(gaddr addr, gsize size, std::uint8_t value) {
  for (gsize p = addr >> 12, e = (addr + size) >> 12; p < e; ++p) {
    std::uint8_t* leaf = g_leaf[p >> kLeafBits].load(std::memory_order_relaxed);
    if (!leaf) {
      leaf = new std::uint8_t[kLeafPages]();
      g_leaf[p >> kLeafBits].store(leaf, std::memory_order_release);
    }
    leaf[p & (kLeafPages - 1)] = value;
  }
}

// Caller holds g_mu. Removes [addr, addr+size) from the informational region map.
void CutRegions(gaddr addr, gsize size) {
  const gaddr end = addr + size;
  auto it = g_regions.upper_bound(addr);
  if (it != g_regions.begin()) --it;
  while (it != g_regions.end() && it->first < end) {
    const gaddr s = it->first, e = it->first + it->second.size;
    if (e <= addr) {
      ++it;
      continue;
    }
    Region r = it->second;
    it = g_regions.erase(it);
    if (s < addr) g_regions[s] = Region{addr - s, r.prot, r.name};
    if (e > end) g_regions[end] = Region{e - end, r.prot, r.name};
  }
}

ZoneInfo* ZoneOf(gaddr a) {
  for (auto& z : g_zones)
    if (a >= z.base && a < z.limit) return &z;
  return nullptr;
}
}  // namespace

bool GuestMemory::Init() {
  static std::once_flag once;
  static bool ok = false;
  std::call_once(once, [] {
    void* p = VirtualAlloc(nullptr, kWindowSize, MEM_RESERVE, PAGE_NOACCESS);
    if (!p) {
      log::Error("GuestMemory: cannot reserve the {} GiB guest window (error {})", kWindowSize >> 30, GetLastError());
      return;
    }
    g_membase = static_cast<std::uint8_t*>(p);
    ok = true;
    log::Debug("GuestMemory: window reserved at {}", p);
  });
  return ok;
}

bool GuestMemory::Map(gaddr addr, gsize size, std::uint8_t prot, const char* name) {
  size = PageUp(size);
  if (!g_membase || (addr & (kPageSize - 1)) || size == 0 || addr + size < addr || addr + size > kWindowSize) {
    log::Error("GuestMemory::Map: bad range 0x{:x}+0x{:x}", addr, size);
    return false;
  }
  std::lock_guard lk(g_mu);
  for (gaddr a = addr; a < addr + size; a += kPageSize) {
    if (Entry(a) & kMapped) {
      log::Error("GuestMemory::Map: 0x{:x}+0x{:x} ('{}') overlaps an existing mapping at 0x{:x}", addr, size, name, a);
      return false;
    }
  }
  if (!VirtualAlloc(g_membase + addr, size, MEM_COMMIT, HostProt(prot))) {
    log::Error("GuestMemory::Map: commit of 0x{:x}+0x{:x} ('{}') failed (error {})", addr, size, name, GetLastError());
    return false;
  }
  SetEntries(addr, size, static_cast<std::uint8_t>(kMapped | prot));
  g_regions[addr] = Region{size, prot, name};
  return true;
}

gaddr GuestMemory::MapAnywhere(Zone zone, gsize size, std::uint8_t prot, const char* name, gsize align) {
  size = PageUp(size);
  if (align < kPageSize) align = kPageSize;
  gaddr addr = 0;
  {
    std::lock_guard lk(g_mu);
    ZoneInfo& z = g_zones[static_cast<int>(zone)];
    for (auto it = z.free_list.begin(); it != z.free_list.end() && !addr; ++it) {
      const gaddr s = (it->first + align - 1) & ~(align - 1);
      if (s + size <= it->first + it->second) {
        const gaddr fs = it->first, fe = it->first + it->second;
        z.free_list.erase(it);
        if (s > fs) z.free_list.emplace_back(fs, s - fs);
        if (s + size < fe) z.free_list.emplace_back(s + size, fe - (s + size));
        addr = s;
        break;
      }
    }
    if (!addr) {
      const gaddr s = (z.next + align - 1) & ~(align - 1);
      if (s + size > z.limit) return 0;
      z.next = s + size;
      addr = s;
    }
  }
  if (!Map(addr, size, prot, name)) return 0;
  return addr;
}

bool GuestMemory::Unmap(gaddr addr, gsize size) {
  size = PageUp(size);
  if (!g_membase || (addr & (kPageSize - 1)) || addr + size > kWindowSize) return false;
  std::lock_guard lk(g_mu);
  for (gaddr a = addr; a < addr + size; a += kPageSize)
    if (!(Entry(a) & kMapped)) return false;
  VirtualFree(g_membase + addr, size, MEM_DECOMMIT);
  SetEntries(addr, size, 0);
  CutRegions(addr, size);
  if (ZoneInfo* z = ZoneOf(addr)) z->free_list.emplace_back(addr, size);
  return true;
}

bool GuestMemory::Protect(gaddr addr, gsize size, std::uint8_t prot) {
  size = PageUp(size);
  if (!g_membase || (addr & (kPageSize - 1)) || addr + size > kWindowSize) return false;
  std::lock_guard lk(g_mu);
  for (gaddr a = addr; a < addr + size; a += kPageSize)
    if (!(Entry(a) & kMapped)) return false;
  DWORD old;
  if (!VirtualProtect(g_membase + addr, size, HostProt(prot), &old)) {
    log::Error("GuestMemory::Protect: VirtualProtect failed (error {})", GetLastError());
    return false;
  }
  SetEntries(addr, size, static_cast<std::uint8_t>(kMapped | prot));
  auto it = g_regions.upper_bound(addr);
  if (it != g_regions.begin()) {
    --it;
    if (it->first <= addr && it->first + it->second.size >= addr + size) {
      // Split so the region list shows the new protection (diagnostics only).
      Region r = it->second;
      const gaddr s = it->first, e = s + r.size;
      g_regions.erase(it);
      if (s < addr) g_regions[s] = Region{addr - s, r.prot, r.name};
      g_regions[addr] = Region{size, prot, r.name};
      if (e > addr + size) g_regions[addr + size] = Region{e - (addr + size), r.prot, r.name};
    }
  }
  return true;
}

std::uint8_t GuestMemory::PageProt(gaddr addr) {
  const std::uint8_t e = Entry(addr);
  return (e & kMapped) ? static_cast<std::uint8_t>(e & 7) : 0;
}

bool GuestMemory::IsMapped(gaddr addr) { return (Entry(addr) & kMapped) != 0; }

bool GuestMemory::RangeHas(gaddr addr, gsize len, std::uint8_t need) {
  if (len == 0) return true;
  if (addr >= kWindowSize || addr + len < addr || addr + len > kWindowSize) return false;
  for (gaddr p = addr >> 12, e = (addr + len - 1) >> 12; p <= e; ++p) {
    const std::uint8_t v = Entry(p << 12);
    if (!(v & kMapped) || (v & need) != need) return false;
  }
  return true;
}

bool GuestMemory::Contains(const void* p) {
  const auto* b = static_cast<const std::uint8_t*>(p);
  return g_membase && b >= g_membase && b < g_membase + kWindowSize;
}

std::string GuestMemory::Describe(gaddr addr) {
  std::lock_guard lk(g_mu);
  auto it = g_regions.upper_bound(addr);
  if (it == g_regions.begin()) return {};
  --it;
  if (addr >= it->first + it->second.size) return {};
  return std::format("{}+0x{:x}", it->second.name, addr - it->first);
}

std::vector<GuestMemory::RegionInfo> GuestMemory::Regions() {
  std::lock_guard lk(g_mu);
  std::vector<RegionInfo> out;
  for (auto& [s, r] : g_regions) out.push_back({s, r.size, r.prot, r.name});
  return out;
}

bool GuestMemory::WriteProtected(gaddr addr, const void* data, gsize len) {
  if (!g_membase || len == 0 || addr + len > kWindowSize) return false;
  std::lock_guard lk(g_mu);
  for (gaddr a = addr & ~(kPageSize - 1); a < addr + len; a += kPageSize)
    if (!(Entry(a) & kMapped)) return false;
  const gaddr first = addr & ~(kPageSize - 1);
  const gsize span = PageUp(addr + len - first);
  DWORD old;
  if (!VirtualProtect(g_membase + first, span, PAGE_READWRITE, &old)) return false;
  std::memcpy(g_membase + addr, data, len);
  // Restore per-page host protection from the table.
  for (gaddr a = first; a < first + span; a += kPageSize) {
    DWORD o2;
    VirtualProtect(g_membase + a, kPageSize, HostProt(static_cast<std::uint8_t>(Entry(a) & 7)), &o2);
  }
  return true;
}

std::string mem::CString(gaddr a, std::size_t max) {
  std::string s;
  const char* p = reinterpret_cast<const char*>(g2h(a));
  while (s.size() < max && *p) s.push_back(*p++);
  return s;
}
}  // namespace nova
