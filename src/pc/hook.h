// Host code that runs before / after (or instead of) a guest function.
//
//   Replace(fn, name, handler)   the function is gone: `handler` is an HLE handler that produces the result
//                                (hle::HookGuestFunction, a 16 byte `br` to the stub).
//   Wrap(fn, name, handler)      the same patch, but the original stays callable: the first 4 instructions of `fn`
//                                are copied to a trampoline (followed by a jump to fn+16) and its address is returned.
//                                The handler calls CallOriginal() where it wants the original to run and does its own
//                                work before / after it. Because the patch is at the entry, every caller is covered:
//                                direct calls, PLT/GOT calls, virtual calls and function pointers.
//
// Why not patch each `bl` call site: the Thunks zone is further than the +-128 MiB reach of `bl` from the library, and
// an entry patch also catches the indirect callers. The price is that the first 4 instructions must be position
// independent (no adr/adrp/b/bl/b.cond/cbz/tbz/ldr literal); Wrap() refuses (returns 0, logs) otherwise.
//
// Apply every patch before the guest threads start. Handlers run on whichever guest thread calls the function.
#pragma once
#include "cpu/cpu.h"
#include "cpu/hle.h"

namespace nova::pc::hook {

// Replaces `fn` entirely. `handler` gets the guest's entry registers and must leave the result in x0 / v0.
bool Replace(gaddr fn, const char* name, hle::RawFn handler);

// Wraps `fn`; returns the trampoline (the "original", callable with call_guest / CallOriginal) or 0 on failure.
gaddr Wrap(gaddr fn, const char* name, hle::RawFn handler);

// Runs the original through `trampoline` with the arguments the handler was entered with (x0-x7, v0-v7; stack
// arguments are not forwarded) and returns its raw result. Does not touch the handler's own registers: forward the
// result with Forward() (or set x0 / v0 yourself).
Cpu::Result CallOriginal(Cpu& cpu, gaddr trampoline);
void Forward(Cpu& cpu, const Cpu::Result& r);

}  // namespace nova::pc::hook
