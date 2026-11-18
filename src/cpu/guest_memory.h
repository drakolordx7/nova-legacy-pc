// Guest memory: one VirtualAlloc(MEM_RESERVE) window of 1<<40 bytes. Guest address 0 is the window base, so
// guest addresses are offsets, not host pointers ("offset window", see docs/ARCHITECTURE.md). dynarmic's fastmem
// uses the window directly; pages are committed on demand by Map(). Anything not mapped traps and is reported
// by the CPU layer; the guest cannot reach the host heap because g2h() masks to the window.
// Adapted from Zombiewood's cpu/guest_memory (MIT, (c) 2026 ChrisDutary), widened to 64-bit addresses.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace nova {
using gaddr = std::uint64_t;
using gsize = std::uint64_t;

inline constexpr unsigned kWindowBits = 40;
inline constexpr gsize kWindowSize = gsize{1} << kWindowBits;
inline constexpr gsize kPageSize = 0x1000;

enum Prot : std::uint8_t {
  kProtNone = 0,
  kProtRead = 1,
  kProtWrite = 2,
  kProtExec = 4,
  kProtRW = kProtRead | kProtWrite,
  kProtRX = kProtRead | kProtExec,
  kProtRWX = kProtRead | kProtWrite | kProtExec,
};

extern std::uint8_t* g_membase;  // host address of guest address 0 (null until GuestMemory::Init)

// Unchecked translation. The address is masked to the window so a wild guest value can never produce a host
// pointer outside it (it will fault on an unmapped page instead).
inline std::uint8_t* g2h(gaddr a) { return g_membase + (a & (kWindowSize - 1)); }
template <class T>
inline T* g2h(gaddr a) { return reinterpret_cast<T*>(g2h(a)); }
inline gaddr h2g(const void* p) { return static_cast<gaddr>(reinterpret_cast<const std::uint8_t*>(p) - g_membase); }

// Fixed zones of the window (addresses are plain bump allocators with a free list).
enum class Zone { Thunks, Libs, Heap, Stacks, Anon };

class GuestMemory {
 public:
  // Reserves the window. Idempotent; returns false (and logs) if the reservation fails.
  static bool Init();

  // Commits [addr, addr+size) (rounded up to pages) with the given guest protection. Fails if any page is
  // already mapped, or the range leaves the window. New pages are zero.
  static bool Map(gaddr addr, gsize size, std::uint8_t prot, const char* name = "");
  // Picks a free range inside `zone`. Returns 0 on failure.
  static gaddr MapAnywhere(Zone zone, gsize size, std::uint8_t prot, const char* name = "", gsize align = kPageSize);
  // Decommits the range (all pages must be mapped) and returns it to its zone's free list.
  static bool Unmap(gaddr addr, gsize size);
  // Changes the guest protection of mapped pages. Host protection: None -> NOACCESS, R/RX -> READONLY,
  // anything with W -> READWRITE. (Guest exec is only enforced by the CPU layer.)
  static bool Protect(gaddr addr, gsize size, std::uint8_t prot);

  // Guest protection of the page containing addr (0 if unmapped).
  static std::uint8_t PageProt(gaddr addr);
  static bool IsMapped(gaddr addr);
  // True if every page in [addr, addr+len) is mapped and has all bits of `need`.
  static bool RangeHas(gaddr addr, gsize len, std::uint8_t need);
  // True if the host pointer lies inside the window.
  static bool Contains(const void* p);
  // "name+0xoff" of the mapping containing addr, or "" when unmapped.
  static std::string Describe(gaddr addr);
  // All mappings, for diagnostics.
  struct RegionInfo {
    gaddr start;
    gsize size;
    std::uint8_t prot;
    std::string name;
  };
  static std::vector<RegionInfo> Regions();

  // Writes into a page whatever its protection (temporarily making it host-writable). For stubs and patches;
  // invalidate translated code yourself if the range may have run.
  static bool WriteProtected(gaddr addr, const void* data, gsize len);
};

// Unchecked accessors for host code (the HLE trusts the guest pointer; a bad one is reported by the crash filter).
namespace mem {
template <class T>
inline T R(gaddr a) {
  T v;
  std::memcpy(&v, g2h(a), sizeof(T));
  return v;
}
template <class T>
inline void W(gaddr a, T v) {
  std::memcpy(g2h(a), &v, sizeof(T));
}
inline std::uint64_t R64(gaddr a) { return R<std::uint64_t>(a); }
inline std::uint32_t R32(gaddr a) { return R<std::uint32_t>(a); }
inline void W64(gaddr a, std::uint64_t v) { W<std::uint64_t>(a, v); }
inline void W32(gaddr a, std::uint32_t v) { W<std::uint32_t>(a, v); }
// Reads a NUL-terminated string (at most max bytes).
std::string CString(gaddr a, std::size_t max = 1 << 20);
}  // namespace mem
}  // namespace nova
