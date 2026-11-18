# Architecture (draft, 2026-11-18)

Native Windows x64 executable that loads the unmodified arm64 `libNOVA.so` from the user's own N.O.V.A. Legacy 5.8.4a APK and runs it. Campaign only, offline. Same engine family (Gameloft Glitch + glf) as Zombiewood, whose structure we fork (MIT, credited).

## Pieces

| Piece | Design |
|---|---|
| Installer | User points at their APK. Extract `assets/datv8` -> XOR 0x61, restore 7z magic -> LZMA unpack `libNOVA.so`/`libgenerator.so`; extract assets (`compressed_res.gla2`, etc.) to `data/`. Hash-check, warn on mismatch. No game files ship. |
| CPU | dynarmic A64 (azahar-emu fork, pinned commit, Windows shadow-space patch as in Zombiewood; Eden's in-tree copy for A64 fixes). Offset window: `fastmem_pointer = VirtualAlloc(MEM_RESERVE, 1<<40)`, `fastmem_address_space_bits = 40`, `silently_mirror_fastmem = false`: out-of-range guest accesses trap and get logged, tagged pointers are masked, guest cannot scribble on the host heap. Shims translate with g2h/h2g (as Zombiewood). Identity mapping (base 0) is a possible late optimisation only. One host thread + one `A64::Jit` per guest thread, shared `ExclusiveMonitor`. TPIDR_EL0 per thread for bionic TLS. |
| Calls | Every import -> 8-byte stub `svc #n; ret`. FAST handlers run inside the SVC callback (no re-entry); normal handlers halt the JIT and dispatch outside it (may block/re-enter). AAPCS64 marshalling: x0-x7 ints/pointers, v0-v7 floats/doubles, stack beyond, structs <=16 bytes in x0:x1, HFAs in v0-v3, large struct returns via x8 (sret); varargs via register save + AAPCS64 `va_list` struct reader. `call_guest` re-entrant host->guest with LR = return stub. |
| Loader | ELF64 RELA (RELATIVE, ABS64, GLOB_DAT, JUMP_SLOT, TLS relocs), GNU hash + DT_HASH, PT_TLS static TLS block per thread, `.init_array`, `JNI_OnLoad`. C++ exceptions via `dl_iterate_phdr` + PT_GNU_EH_FRAME (guest libc++/libunwind does the unwinding itself). |
| libc | bionic LP64 HLE (string/math/stdio/pthread/time/vfs/format), adapted from Zombiewood with LP64 struct layouts. `long` is 64-bit in the guest; host shims never use Windows `long`. |
| JNI | Fake JavaVM/JNIEnv (Zombiewood DSL), 64-bit refs. `classes_game.cpp` rewritten from NOVA's dex (docs/interface/jni.md). Online, ads, IAP, social -> offline no-ops. |
| Graphics | SDL3 window, ANGLE (D3D11, GLES 3.0) via EGL. GL thunks pass guest pointers straight through. Guest framebuffer 0 -> our FBO -> blit at native resolution. Uncapped frame rate, vsync toggle. |
| Audio | Whatever NOVA actually uses (OpenSL ES or AudioTrack via JNI) -> SDL3 audio stream. |
| PC layer | Resolution/aspect via the engine's own screen-size path (NativeSurfaceChanged + AndroidDevice size); FOV correction via `CCustomCameraSceneNode::setFOV` hook; raw mouse via SDL3 relative mode into the engine's camera/look input (glf::Mouse or the touch-look path); keyboard binds; HUD touch controls hidden. Frame pacing unlocked (engine fps cap hooked). |

## Design rules
- Phase 1 starts with an A64 fastmem spike on Windows (loads/stores, faults through the SEH handler, LL/SC, SVC, re-entrant call) before any loader or HLE work.
- Guest code patches (HookGuestFunction) are all applied before guest threads start; any later patch invalidates every thread's Jit.
- `long double` is 128-bit in the guest: never forward to the host CRT. `__stack_chk_guard` must exist. C++ exceptions must never cross host frames.
- Before uncapping the frame rate, confirm the engine uses real delta time.
- Resolve audio (OpenSL ES vs AudioTrack) from the interface map before Phase 3.

## Phases
1. Foundation: fork skeleton, dynarmic A64 build, ELF64 loader, SVC dispatch, `--load-only` resolves all imports and runs init_array.
2. Boot: libc/pthread/JNI/EGL/GLES shims until the first frame renders.
3. Playable: audio, touch emulation, menus, save path, campaign mission loads.
4. PC layer: resolution, refresh, raw mouse, keyboard, FOV, settings menu.
5. Installer + public release.
