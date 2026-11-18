// Import registry and SVC stubs. Every import (a function the guest calls into the host) gets one 8-byte stub in
// the Thunks zone: `svc #n ; ret`. The same stub serves PLT/GOT slots and any function pointer handed to the
// guest. The typed, signature-deduced marshalling (NOVA_HLE) lives in cpu/hle_abi.h.
// Adapted from Zombiewood's cpu/hle (MIT, (c) 2026 ChrisDutary): A64 stubs, 64-bit addresses.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cpu/guest_memory.h"

namespace nova {
class Cpu;

namespace hle {
// Raw handler: reads x0-x7 / v0-v7 / stack from the Cpu and writes x0 (x1) / v0.
using RawFn = void (*)(Cpu&);

enum Flags : std::uint32_t {
  kDefault = 0,
  // Runs INSIDE the JIT callback (no exit from Jit::Run). It must not call into the guest, throw, block for long,
  // or change pc/sp. For memcpy/strlen/libm/gl* style leaf handlers.
  kFast = 1,
};

struct Entry {
  std::string name;
  std::string lib;  // library the import is classified under ("libc", "libm", ...); filled by the loader
  std::atomic<RawFn> fn{nullptr};
  std::atomic<std::uint32_t> flags{0};
  std::atomic<std::uint64_t> calls{0};
  std::atomic<bool> warned{false};
  std::uint32_t index = 0;  // the `svc #index`
  gaddr stub = 0;
};

// Binds `fn` to import `name` (the last registration wins). Creates the entry and its stub if needed.
void Register(std::string_view name, RawFn fn, std::uint32_t flags = kDefault);
bool IsRegistered(std::string_view name);

// The entry / stub address for `name`, created on first use. Always the same address for a name.
Entry& EntryFor(std::string_view name);
gaddr StubFor(std::string_view name);
Entry* FindEntry(std::string_view name);
// Lock-free lookup used on the SVC path. nullptr for unknown indices.
Entry* EntryByIndex(std::uint32_t index);
// Import name owning a stub address, or "".
std::string StubName(gaddr addr);
// Snapshot of every entry, in index order.
std::vector<Entry*> AllEntries();

// The `svc #0 ; ret` stub: call_guest points LR here, so returning from the guest function ends the call.
gaddr ReturnStub();

// Imported data objects (`__stack_chk_guard`, `__sF`, ...). DefineData creates (or returns) a zeroed guest block;
// call it before loading the module that imports the symbol.
gaddr DefineData(std::string_view name, gsize size, gsize align = 8);
void DefineDataAt(std::string_view name, gaddr addr);
gaddr FindData(std::string_view name);  // 0 if none

enum class OnUnimplemented { ReturnZero, Abort };
// Default ReturnZero (log once per import); NOVA_ABORT_ON_UNIMPLEMENTED=1 selects Abort.
void SetUnimplementedPolicy(OnUnimplemented p);
OnUnimplemented UnimplementedPolicy();
// Imports whose stub exists and that have no handler.
std::vector<std::string> UnimplementedImports();

// Replaces guest code at `fn` with a `br` to the stub of import `name` (HookGuestFunction). Needs 16 bytes.
// Apply before any guest thread starts (the patch invalidates translated code only for the calling thread's
// knowledge; see docs/ARCHITECTURE.md design rules).
bool HookGuestFunction(gaddr fn, std::string_view name);
}  // namespace hle
}  // namespace nova
