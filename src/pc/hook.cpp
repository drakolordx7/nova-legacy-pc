#include "pc/hook.h"

#include <mutex>

#include "core/log.h"

namespace nova::pc::hook {
namespace {
constexpr gsize kSlot = 32;       // 4 copied instructions + ldr x16 / br x16 / .quad
constexpr gsize kChunk = 0x1000;

std::mutex g_mu;
gaddr g_chunk = 0, g_next = 0, g_end = 0;

// Instructions whose result depends on their own address.
bool IsPcRelative(std::uint32_t i) {
  if ((i & 0x1F000000u) == 0x10000000u) return true;  // adr / adrp
  if ((i & 0x7C000000u) == 0x14000000u) return true;  // b / bl
  if ((i & 0xFF000010u) == 0x54000000u) return true;  // b.cond
  if ((i & 0x7E000000u) == 0x34000000u) return true;  // cbz / cbnz
  if ((i & 0x7E000000u) == 0x36000000u) return true;  // tbz / tbnz
  if ((i & 0x3B000000u) == 0x18000000u) return true;  // ldr / ldrsw / prfm literal
  if ((i & 0xFFFFFC1Fu) == 0xD65F0000u) return true;  // ret (the function ends inside the patch)
  if ((i & 0xFFFFFC1Fu) == 0xD61F0000u) return true;  // br
  return false;
}

gaddr AllocSlot() {
  if (g_next + kSlot > g_end) {
    g_chunk = GuestMemory::MapAnywhere(Zone::Thunks, kChunk, kProtRX, "pc-trampolines");
    if (!g_chunk) return 0;
    g_next = g_chunk;
    g_end = g_chunk + kChunk;
  }
  const gaddr a = g_next;
  g_next += kSlot;
  return a;
}
}  // namespace

bool Replace(gaddr fn, const char* name, hle::RawFn handler) {
  hle::Register(name, handler, hle::kDefault);
  return hle::HookGuestFunction(fn, name);
}

gaddr Wrap(gaddr fn, const char* name, hle::RawFn handler) {
  std::uint32_t orig[4];
  std::memcpy(orig, g2h(fn), sizeof orig);
  for (int k = 0; k < 4; ++k) {
    if (IsPcRelative(orig[k])) {
      log::Warn("[pc] cannot wrap {} (0x{:x}): instruction {} of the prologue (0x{:08x}) is position dependent", name, fn, k, orig[k]);
      return 0;
    }
  }
  std::lock_guard lk(g_mu);
  const gaddr tramp = AllocSlot();
  if (!tramp) {
    log::Warn("[pc] no room for a trampoline for {}", name);
    return 0;
  }
  const gaddr back = fn + 16;
  std::uint32_t code[8];
  std::memcpy(code, orig, 16);
  code[4] = 0x58000050u;  // ldr x16, #8
  code[5] = 0xD61F0200u;  // br x16
  code[6] = static_cast<std::uint32_t>(back);
  code[7] = static_cast<std::uint32_t>(back >> 32);
  if (!GuestMemory::WriteProtected(tramp, code, sizeof code)) return 0;
  // The trampoline is new code: nothing to invalidate. Patch the entry last.
  if (!Replace(fn, name, handler)) return 0;
  return tramp;
}

Cpu::Result CallOriginal(Cpu& cpu, gaddr trampoline) {
  detail::ArgPack p;
  for (unsigned i = 0; i < 8; ++i) p.x[i] = cpu.x(i);
  p.nx = 8;
  for (unsigned i = 0; i < 8; ++i) p.v[i] = cpu.v(i)[0];
  p.nv = 8;
  return cpu.CallPacked(trampoline, p);
}

void Forward(Cpu& cpu, const Cpu::Result& r) {
  cpu.set_x(0, r.x0);
  cpu.set_x(1, r.x1);
  cpu.set_v(0, r.v0);
}

}  // namespace nova::pc::hook
