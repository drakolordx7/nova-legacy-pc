// Native anti-tamper patches. Each function is replaced (hle::HookGuestFunction: a `br` to an HLE stub) by a
// handler that returns the value of an untampered install. The clean value of each was confirmed from the AArch64
// disassembly of libNOVA.so 5.8.4a (llvm-objdump), see the comment on each entry.
#include <cstring>

#include "core/log.h"
#include "cpu/cpu.h"
#include "cpu/hle.h"
#include "game/game.h"

namespace nova::game {
namespace {

// Returns an empty std::string (libc++ NDK, 24 bytes, all zero = empty short string) through the sret pointer in x8.
// Used by the two reporting functions whose result only goes into the anti-hack telemetry JSON.
void ReturnEmptyString(Cpu& cpu) {
  const gaddr sret = cpu.x(8);
  if (sret) std::memset(g2h(sret), 0, 24);
  cpu.set_x(0, sret);
}
void ReturnOne(Cpu& cpu) { cpu.set_x(0, 1); }
void ReturnZero(Cpu& cpu) { cpu.set_x(0, 0); }

struct Patch {
  const char* symbol;  // mangled export
  const char* handler;
  hle::RawFn fn;
};

// - _ZN3ysM3ftCEv  (ysM::ftC, the APK/library integrity scan: fopen(GetLibSoPath), SHA-256 of ELF regions, zip_open
//   of GetApkPath). Returns std::string by sret (x8): the only caller is platform::AntiHacking::GetBatman, a one-
//   instruction thunk (ldr x0,[instance]; b ftC) used by olutils::antihack::GetEvents, which strlen()s the result and
//   adds it as a JSON field for the server. An empty string is accepted (disassembly of GetEvents at 0x1949e18: the
//   SSO-flag test then strlen loop work for 24 zero bytes).
// - _ZN3ysM13GET_ALL_SIZESEv: same shape (sret std::string), reached through platform::AntiHacking::GetZoro
//   (thunk at 0x19558e0, caller GetEvents at 0x194a0c8).
// - _ZN3ysM16CheckOfflineZoroEv: formats "%d|%d|%d" from ysM::yfS(0)/yfS(3), decrypts assets/content.txt with
//   GetPlainText(.., "19GmLfT99") and strcmps; the match path (0x1222c08) sets w19 = 1 and returns it, the mismatch
//   path calls PackageUtils::ShowOfflineZoroDialog() (ban dialog + exit) and returns 0. Clean = 1.
// - _ZN9acp_utils3api12PackageUtils8IsAppEncEv: calls Java SUtils.retrieveBarrels() -> int[4] and compares each entry
//   with 404250541 (index 0) / 202150541; the result w20 is 1 when the array is null, all entries are <= 0 ("signature
//   unavailable") or one matches, and 0 only for a non-matching positive hash (0x1233d3c..0x1233d98). Clean = 1.
// - _ZN9acp_utils3api12PackageUtils15IsRunOnEmulatorEv: returns the jboolean of Java EmulatorDetector.
//   IsGameRunOnEmulator() (CallStaticBooleanMethod at 0x1228d..), true = emulator. Clean = 0.
// - _ZN9acp_utils3api12PackageUtils14IsDeviceRoutedEv ("routed" = rooted): static cache, fopen(su path) / stat(su)
//   success sets it to 1, w0 = (cache != 0) (0x1233b34..0x1233b48). Clean = 0.
const Patch kPatches[] = {
    {"_ZN3ysM3ftCEv", "patch::ysM::ftC", ReturnEmptyString},
    {"_ZN3ysM13GET_ALL_SIZESEv", "patch::ysM::GET_ALL_SIZES", ReturnEmptyString},
    {"_ZN3ysM16CheckOfflineZoroEv", "patch::ysM::CheckOfflineZoro", ReturnOne},
    {"_ZN9acp_utils3api12PackageUtils8IsAppEncEv", "patch::PackageUtils::IsAppEnc", ReturnOne},
    {"_ZN9acp_utils3api12PackageUtils15IsRunOnEmulatorEv", "patch::PackageUtils::IsRunOnEmulator", ReturnZero},
    {"_ZN9acp_utils3api12PackageUtils14IsDeviceRoutedEv", "patch::PackageUtils::IsDeviceRouted", ReturnZero},
};

}  // namespace

int ApplyGamePatches(const loader::Module& module) {
  int applied = 0;
  for (const Patch& p : kPatches) {
    const gaddr fn = module.Lookup(p.symbol);
    if (!fn) {
      log::Warn("[game] patch target {} not found in {}", p.symbol, module.name);
      continue;
    }
    hle::Register(p.handler, p.fn, hle::kDefault);
    if (!hle::HookGuestFunction(fn, p.handler)) {
      log::Warn("[game] could not patch {} (0x{:x})", p.symbol, fn);
      continue;
    }
    log::Info("[game] patched {} at 0x{:x}", p.symbol, fn);
    ++applied;
  }
  return applied;
}

}  // namespace nova::game
