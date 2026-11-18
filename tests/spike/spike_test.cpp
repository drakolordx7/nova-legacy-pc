// A64 spike: proves the dynarmic A64 + offset-window fastmem model on Windows before any loader work.
// Items: fastmem loads/stores, caught faults (unmapped, read-only, out of window, null, bad jump), LL/SC with a shared
// ExclusiveMonitor across two Jits on two host threads, svc dispatch (x0-x7, v0-v7 in; x0/v0 out; FAST and normal),
// re-entrant call_guest from inside a handler, TPIDR_EL0 per thread, call_guest calling convention.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "core/log.h"
#include "cpu/cpu.h"
#include "cpu/guest_memory.h"
#include "cpu/hle.h"
#include "guest_blob.h"

using namespace nova;

namespace {
int g_fail = 0;
int g_pass = 0;
void Report(const std::string& name, bool ok, const std::string& detail = {}) {
  if (ok) ++g_pass;
  else ++g_fail;
  std::printf("%s %-34s %s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.c_str());
  std::fflush(stdout);
}

gaddr Sym(const char* name) {
  for (const auto& s : guest_blob::kSyms)
    if (std::strcmp(s.name, name) == 0) return s.addr;
  std::printf("FAIL missing guest symbol %s\n", name);
  std::exit(2);
}

constexpr gaddr kData = 0x4800'0000ull;  // 1 MiB RW scratch
constexpr gaddr kRoPage = 0x4900'0000ull;  // one read-only page
constexpr gaddr kUnmapped = 0x30'0000'1000ull;

// Runs `fn`, expecting a GuestFault; returns its message ("" if none).
template <class F>
std::string Faults(F&& fn) {
  try {
    fn();
  } catch (const GuestFault& f) {
    return f.what();
  }
  return {};
}

// --- host handlers ---
struct Seen {
  std::uint64_t x[8] = {};
  double d[8] = {};
  std::atomic<int> calls{0};
} g_seen;

void SumHandler(Cpu& c) {
  std::uint64_t s = 0;
  double sd = 0;
  for (unsigned i = 0; i < 8; ++i) {
    g_seen.x[i] = c.x(i);
    g_seen.d[i] = c.d(i);
    s += c.x(i);
    sd += c.d(i);
  }
  g_seen.calls++;
  c.set_x(0, s);
  c.set_d(0, sd);
}
// Normal handler that re-enters the guest: (ctx, cb, val) -> cb(ctx, val)
void ReenterHandler(Cpu& c) {
  const gaddr ctx = c.x(0), cb = c.x(1), val = c.x(2);
  const std::uint64_t r = c.Call<std::uint64_t>(cb, ctx, val);
  c.set_x(0, r);
}

void TestLoadStore() {
  const gaddr fn = Sym("ls_test");
  const std::uint64_t n = 100;
  std::uint64_t expect = 0;
  for (std::uint64_t i = 0; i < n; ++i) expect += static_cast<std::uint8_t>(i * 7 + 1);
  expect += 0x11 + static_cast<std::uint8_t>(3 * 7 + 1) + 3;
  const auto slow0 = cpu::SlowPathAccesses();
  const auto r = call_guest<std::uint64_t>(fn, kData, n);
  const auto slow = cpu::SlowPathAccesses() - slow0;
  Report("fastmem loads/stores", r == expect && mem::R64(kData + n) == 0x1122334455667788ull && slow == 0,
         std::format("sum {} expect {}, slow-path accesses {} (want 0)", r, expect, slow));
}

void TestFaults() {
  const gaddr peek = Sym("peek"), poke = Sym("poke");
  {
    const std::string m = Faults([&] { call_guest<std::uint64_t>(peek, kUnmapped); });
    Report("fault: read unmapped page", m.find("unmapped guest address 0x30") != std::string::npos);
    std::printf("      first line: %s\n", m.substr(0, m.find('\n')).c_str());
    const std::string pcline = m.substr(m.find("pc = "), 60);
    std::printf("      %s (peek = 0x%llx)\n", pcline.substr(0, pcline.find('\n')).c_str(), (unsigned long long)peek);
  }
  {
    const std::string m = Faults([&] { call_guest<void>(poke, kRoPage, 1); });
    Report("fault: write read-only page", m.find("read-only guest address") != std::string::npos);
  }
  {
    const std::string m = Faults([&] { call_guest<std::uint64_t>(peek, 0); });
    Report("fault: null read", m.find("unmapped guest address 0x0") != std::string::npos);
  }
  {
    const std::string m = Faults([&] { call_guest<std::uint64_t>(peek, kWindowSize + 0x1000); });
    Report("fault: read outside window", m.find("unmapped guest address 0x10000001000") != std::string::npos);
  }
  {
    // A tagged pointer (top byte set) into mapped memory is accepted (TBI) through the slow path.
    mem::W64(kData + 0x500, 0xfeedface12345678ull);
    const std::uint64_t v = call_guest<std::uint64_t>(peek, (0xb4ull << 56) | (kData + 0x500));
    Report("tagged pointer (TBI) read", v == 0xfeedface12345678ull);
  }
  {
    const std::string m = Faults([&] { call_guest<void>(kUnmapped); });
    Report("fault: jump to unmapped code", m.find("non-executable") != std::string::npos);
  }
  {
    const std::string m = Faults([&] { call_guest<void>(kData); });  // mapped RW but not executable
    Report("fault: jump to non-exec page", m.find("non-executable") != std::string::npos);
  }
  // The host and the Cpu survived: a normal call still works afterwards.
  const auto r = call_guest<std::uint64_t>(Sym("many_args"), 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0.0);
  Report("cpu usable after faults", r == 55, std::format("{}", r));
}

void TestLlSc() {
  const gaddr counter = kData + 0x1000;
  mem::W64(counter, 0);
  const gaddr fn = Sym("inc_thread");
  threads::CreateParams p;
  p.entry = fn;
  p.arg = counter;
  p.name = "llsc-a";
  const auto a = threads::Create(p);
  p.name = "llsc-b";
  const auto b = threads::Create(p);
  std::string fa, fb;
  const auto slow0 = cpu::SlowPathAccesses();
  threads::Join(a, nullptr, &fa);
  threads::Join(b, nullptr, &fb);
  const std::uint64_t v = mem::R64(counter);
  const auto slow = cpu::SlowPathAccesses() - slow0;
  Report("LL/SC, 2 Jits on 2 threads", a && b && fa.empty() && fb.empty() && v == 800000 && slow == 0,
         std::format("counter {} (want 800000), slow-path accesses {} (want 0)", v, slow));
  mem::W64(kData + 0x1100, 0x55);
  const auto st = call_guest<std::uint64_t>(Sym("clrex_test"), kData + 0x1100);
  Report("ldxr/clrex/stxr fails", st == 1, std::format("status {}", st));
}

void TestSvc() {
  hle::Register("test_host_fast", &SumHandler, hle::kFast);
  hle::Register("test_host_slow", &SumHandler, hle::kDefault);
  for (const char* name : {"test_host_fast", "test_host_slow"}) {
    const gaddr stub = hle::StubFor(name);
    g_seen.calls = 0;
    const gaddr out = kData + 0x2000;
    call_guest<void>(Sym("svc_test"), stub, out);
    const std::uint64_t x0 = mem::R64(out);
    const double d0 = mem::R<double>(out + 8);
    bool seen_ok = g_seen.calls == 2;
    for (unsigned i = 0; i < 8; ++i) seen_ok &= g_seen.x[i] == i + 1 && g_seen.d[i] == i + 1.5;
    Report(std::string("svc dispatch (") + (name + 10) + ")", x0 == 36 && d0 == 40.0 && seen_ok,
           std::format("x0={} d0={} calls={}", x0, d0, g_seen.calls.load()));
  }
}

void TestReentrancy() {
  hle::Register("test_reenter", &ReenterHandler, hle::kDefault);
  const gaddr ctx = kData + 0x3000;
  mem::W64(ctx, hle::StubFor("test_reenter"));
  mem::W64(ctx + 8, Sym("reent_cb"));
  const auto r = call_guest<std::uint64_t>(Sym("reent_cb"), ctx, 4);
  // reent_cb(4) = 4 + (3 + (2 + (1 + 0)))
  Report("re-entrant call_guest from handler", r == 10, std::format("result {} (want 10; 0xdeadbeef = registers clobbered)", r));
}

void TestTpidr() {
  const gaddr o1 = kData + 0x4000, o2 = kData + 0x5000;
  threads::CreateParams p;
  p.entry = Sym("tp_thread");
  p.name = "tp-a";
  p.arg = o1;
  const auto a = threads::Create(p);
  p.name = "tp-b";
  p.arg = o2;
  const auto b = threads::Create(p);
  threads::Join(a);
  threads::Join(b);
  const std::uint64_t tp1 = mem::R64(o1), tp2 = mem::R64(o2);
  const bool ok = tp1 && tp2 && tp1 != tp2 && mem::R64(o1 + 8) == a && mem::R64(o2 + 8) == b && mem::R64(o1 + 16) == tp1 &&
                  mem::R64(o2 + 16) == tp2 && mem::R64(o1 + 24) == o1 + 0x100 && mem::R64(o2 + 24) == o2 + 0x100;
  Report("TPIDR_EL0 per thread", ok,
         std::format("tp {:x} {:x}, tid {} {}", tp1, tp2, mem::R64(o1 + 8), mem::R64(o2 + 8)));
  Cpu& c = Cpu::Current();
  Report("TPIDR_EL0 main thread TLS layout", c.tpidr() == c.tls() && mem::R64(c.tls() + 8) == c.thread_id());
}

void TestCallConv() {
  const double d = call_guest<double>(Sym("fp_test"), 3.0, 4.0);
  const std::uint64_t r = call_guest<std::uint64_t>(Sym("many_args"), 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 2.5);
  Report("call_guest float + stack args", d > 13.73 && d < 13.74 && r == 1 + 4 + 9 + 16 + 25 + 36 + 49 + 64 + 81 + 100 + 2,
         std::format("fp {} many {}", d, r));
}
}  // namespace

int main() {
  if (!GuestMemory::Init()) {
    std::printf("FAIL cannot reserve guest window\n");
    return 1;
  }
  // Guest code.
  const std::size_t sz = sizeof(guest_blob::kBytes);
  if (!GuestMemory::Map(guest_blob::kBase, sz, kProtRW, "spike-code")) return 1;
  std::memcpy(g2h(guest_blob::kBase), guest_blob::kBytes, sz);
  GuestMemory::Protect(guest_blob::kBase, sz, kProtRX);
  if (!GuestMemory::Map(kData, 1 << 20, kProtRW, "spike-data")) return 1;
  if (!GuestMemory::Map(kRoPage, kPageSize, kProtRW, "spike-ro")) return 1;
  GuestMemory::Protect(kRoPage, kPageSize, kProtRead);

  log::SetDebug(false);
  TestLoadStore();
  TestFaults();
  TestLlSc();
  TestSvc();
  TestReentrancy();
  TestTpidr();
  TestCallConv();
  std::printf("%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
