#include "cpu/hle.h"

#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <unordered_map>

#include "core/log.h"
#include "cpu/cpu.h"

namespace nova::hle {
namespace {
constexpr std::uint32_t kMaxIndex = 0xFFFF;  // `svc` carries a 16-bit immediate
constexpr gsize kChunkBytes = 64 * 1024;

std::mutex g_mu;
std::deque<Entry> g_entries;  // stable addresses
std::unordered_map<std::string, Entry*> g_by_name;
std::atomic<Entry*> g_by_index[kMaxIndex + 1];
std::map<gaddr, Entry*> g_by_stub;
std::unordered_map<std::string, gaddr> g_data;

gaddr g_chunk = 0, g_chunk_next = 0, g_chunk_end = 0;
gaddr g_return_stub = 0;
std::atomic<int> g_policy{-1};

constexpr std::uint32_t Svc(std::uint32_t n) { return 0xD4000001u | (n << 5); }
constexpr std::uint32_t kRet = 0xD65F03C0u;

// Caller holds g_mu.
gaddr AllocStub(std::uint32_t index) {
  if (g_chunk_next + 8 > g_chunk_end) {
    GuestMemory::Init();
    g_chunk = GuestMemory::MapAnywhere(Zone::Thunks, kChunkBytes, kProtRX, "thunks");
    if (!g_chunk) {
      log::Error("hle: out of thunk space");
      std::abort();
    }
    g_chunk_next = g_chunk;
    g_chunk_end = g_chunk + kChunkBytes;
  }
  const gaddr at = g_chunk_next;
  g_chunk_next += 8;
  const std::uint32_t code[2] = {Svc(index), kRet};
  GuestMemory::WriteProtected(at, code, sizeof code);
  return at;
}

// Caller holds g_mu.
Entry& Create(std::string_view name, std::uint32_t index) {
  Entry& e = g_entries.emplace_back();
  e.name = std::string(name);
  e.index = index;
  e.stub = AllocStub(index);
  g_by_index[index].store(&e, std::memory_order_release);
  g_by_stub[e.stub] = &e;
  return e;
}

// Caller holds g_mu. Index 0 is the return stub.
void EnsureInit() {
  if (!g_return_stub) {
    Entry& r = Create("<return>", 0);
    g_by_name["<return>"] = &r;
    g_return_stub = r.stub;
  }
}
}  // namespace

Entry& EntryFor(std::string_view name) {
  std::lock_guard lk(g_mu);
  EnsureInit();
  auto it = g_by_name.find(std::string(name));
  if (it != g_by_name.end()) return *it->second;
  const auto index = static_cast<std::uint32_t>(g_entries.size());
  if (index > kMaxIndex) {
    log::Error("hle: more than {} imports", kMaxIndex);
    std::abort();
  }
  Entry& e = Create(name, index);
  g_by_name[e.name] = &e;
  return e;
}

gaddr StubFor(std::string_view name) { return EntryFor(name).stub; }

Entry* FindEntry(std::string_view name) {
  std::lock_guard lk(g_mu);
  auto it = g_by_name.find(std::string(name));
  return it == g_by_name.end() ? nullptr : it->second;
}

Entry* EntryByIndex(std::uint32_t index) {
  return index <= kMaxIndex ? g_by_index[index].load(std::memory_order_acquire) : nullptr;
}

void Register(std::string_view name, RawFn fn, std::uint32_t flags) {
  Entry& e = EntryFor(name);
  e.flags.store(flags);
  e.fn.store(fn);
}

bool IsRegistered(std::string_view name) {
  Entry* e = FindEntry(name);
  return e && e->fn.load() != nullptr;
}

std::string StubName(gaddr addr) {
  std::lock_guard lk(g_mu);
  auto it = g_by_stub.find(addr);
  return it == g_by_stub.end() ? std::string() : it->second->name;
}

std::vector<Entry*> AllEntries() {
  std::lock_guard lk(g_mu);
  std::vector<Entry*> out;
  out.reserve(g_entries.size());
  for (auto& e : g_entries) out.push_back(&e);
  return out;
}

gaddr ReturnStub() {
  std::lock_guard lk(g_mu);
  EnsureInit();
  return g_return_stub;
}

gaddr DefineData(std::string_view name, gsize size, gsize align) {
  std::lock_guard lk(g_mu);
  auto it = g_data.find(std::string(name));
  if (it != g_data.end()) return it->second;
  GuestMemory::Init();
  const gaddr a = GuestMemory::MapAnywhere(Zone::Thunks, size < 8 ? 8 : size, kProtRW, "hle-data", align < 16 ? 16 : align);
  if (!a) {
    log::Error("hle: cannot allocate data symbol {}", name);
    return 0;
  }
  g_data[std::string(name)] = a;
  return a;
}

void DefineDataAt(std::string_view name, gaddr addr) {
  std::lock_guard lk(g_mu);
  g_data[std::string(name)] = addr;
}

gaddr FindData(std::string_view name) {
  std::lock_guard lk(g_mu);
  auto it = g_data.find(std::string(name));
  return it == g_data.end() ? 0 : it->second;
}

void SetUnimplementedPolicy(OnUnimplemented p) { g_policy.store(static_cast<int>(p)); }

OnUnimplemented UnimplementedPolicy() {
  int p = g_policy.load();
  if (p < 0) {
    const char* e = std::getenv("NOVA_ABORT_ON_UNIMPLEMENTED");
    p = (e && *e && *e != '0') ? static_cast<int>(OnUnimplemented::Abort) : static_cast<int>(OnUnimplemented::ReturnZero);
    g_policy.store(p);
  }
  return static_cast<OnUnimplemented>(p);
}

std::vector<std::string> UnimplementedImports() {
  std::lock_guard lk(g_mu);
  std::vector<std::string> out;
  for (auto& e : g_entries)
    if (e.index != 0 && !e.fn.load()) out.push_back(e.name);
  return out;
}

bool HookGuestFunction(gaddr fn, std::string_view name) {
  const gaddr stub = StubFor(name);
  // ldr x16, #8 ; br x16 ; .quad stub
  const std::uint32_t code[4] = {0x58000050u, 0xD61F0200u, static_cast<std::uint32_t>(stub),
                                 static_cast<std::uint32_t>(stub >> 32)};
  if (!GuestMemory::WriteProtected(fn, code, sizeof code)) return false;
  cpu::InvalidateCode(fn, sizeof code);
  return true;
}
}  // namespace nova::hle
