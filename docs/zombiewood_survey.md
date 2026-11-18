# Zombiewood survey (template for the N.O.V.A. Legacy A64 port)

Source: `third_party/ref/Zombiewood` v0.2.2, MIT (c) 2026 ChrisDutary. About 19.2k lines in `src/`, 2.6k in `tests/`. Comments and logs are Spanish; docs are English. Guest: ELF32 ARM v7, softfp, Thumb-2, `libZombieWood.so` (Gameloft Glitch, GLES2).

## 1. Layout

| Dir (lines) | Role |
|---|---|
| `core/` (310) | log, json config, paths |
| `cpu/` (2200) | `guest_memory` (4 GiB window, page-protection table, zone allocator, guest heap); `cpu.cpp` (dynarmic wrapper, `call_guest`, threads, CP15, kuser page, setjmp/longjmp); `hle.h/.cpp` (import registry, template marshalling, `VaList`, `HookGuestFunction`) |
| `loader/` (1100) | `loader.cpp` ELF32 loader |
| `hle/libc/` (6400) | bionic libc/libm/pthread/stdio (real bionic `FILE`), printf/scanf engine, wchar, time, failing net stubs, `vfs` (Android path to host path, OBB/ZIP mounts, fd table), `zip.cpp` (ZIP reader + own inflate) |
| `hle/gles/` (1170) | 99 `gl*` thunks to ANGLE; `texdecode` (ETC1/PVRTC/ATC to RGBA8) |
| `hle/audio/` (380) | AudioTrack emulation on an SDL queue; OpenSL stub |
| `jni/` (2900) | fake JavaVM/JNIEnv, object model, `classes_game.cpp`, `classes_android.cpp` |
| `frontend/` (4000) | window, `game.cpp` (threads, step loop, pacing, pause), input mapper, Media Foundation video, ImGui menu, setup wizard, saves, settings |
| `installer/` (745) | extracts files from the user's APK |

## 2. Loader (415 lines)

- Maps PT_LOAD segments into a Libs zone (0x10000000+) with per-segment protection.
- Supports only `DT_HASH` and `DT_REL`; it errors on RELA or a missing DT_HASH.
- Relocations: RELATIVE, ABS32, GLOB_DAT, JUMP_SLOT, REL32, all eager.
- No PT_TLS. TPIDRURO returns a per-thread 64-word block (errno at slot 2); the game needed no more.
- Runs DT_INIT and `.init_array` (552 constructors) via `RunInit` after all HLE is registered, then `JNI_OnLoad`.
- Import resolution: the module itself, then other modules, then HLE data (`DefineData`), then an HLE stub. A function import always resolves, handler or not.
- C++ exceptions work because the loader serves `__gnu_Unwind_Find_exidx` from `.ARM.exidx`.

## 3. Guest to host calls

- Every import gets an 8-byte stub in a Thunks zone: `svc #n; bx lr`. The same stub serves PLT, GOT pointers, JNI vtable slots and any function pointer given to the guest.
- `CallSVC` has two paths. FAST handlers (mem*, str*, libm, gl*) run inside the JIT callback and may not call the guest or throw. Normal handlers `HaltExecution`, return from `Jit::Run`, then dispatch outside the JIT, so they may block or re-enter the guest.
- Marshalling is deduced from the C++ signature (`ZW_HLE(name, fn)`): r0-r3 plus stack words, even-aligned pairs for 64-bit values, `T*` translated by `g2h`. Bare `size_t`/`int64_t` is a static_assert error.
- Varargs: `Varargs`/`VaList` with `Next<T>()`, either reading call registers (`FromCall`) or a guest `va_list` (a plain pointer on ARM32). JNI `Call*Method`, `*V`, `*A` use the same readers.
- Host to guest: `call_guest<R>(fn, args...)` saves guest registers, sets LR to a return stub (SVC 0) and honours the Thumb bit. It is fully re-entrant (qsort comparators, JNI to guest to JNI). A guest `longjmp` across frames is an internal C++ exception.
- Unimplemented imports log once and return 0. `ZW_TRACE_IMPORTS` and a filter trace calls. `HookGuestFunction` patches a guest function with `ldr pc,[pc,#-4]` to divert it to an HLE stub.

## 4. CPU and threading

- dynarmic azahar fork, commit a466015, A32 frontend only, x64 backend.
- Fastmem with `fastmem_pointer = g_membase` over a 4 GiB `VirtualAlloc(MEM_RESERVE)` window: guest address 0 sits at the base, so it is not identity-mapped. `fastmem_exclusive_access` is on. A page-table fallback exists (`ZW_FASTMEM=0`). Slow-path callbacks fault with a register dump on unmapped or read-only access.
- Exclusives use host `_InterlockedCompareExchange*` plus one global `ExclusiveMonitor(128)`.
- One host thread plus one `A32::Jit` per guest thread, real concurrency, no big lock. Each Jit has its own 64 MiB code cache (translations are not shared). Maximum 128 threads.
- dynarmic needs a local patch on Windows. Its exception handler reserved 8 bytes of stack where x64 needs 0x28 shadow space, so the first fastmem fault crashed. `cmake/ZwDependencies.cmake` patches it by string replacement after FetchContent. It will matter for A64 too.

## 5. JNI

- `JNIEnv` is a guest pointer to 232 slots, each an HLE stub with the real slot signature. JavaVM `AttachCurrentThread`/`GetEnv` are per thread.
- References are 32-bit handles `(index<<2)|kind`. The object model is C++ (`Object`, `Class`, `Method`, `Field`, `Value`, `String`, `Array`, `Bundle`). Primitive arrays live in guest memory, so `Get*ArrayElements` returns direct pointers.
- A DSL defines emulated classes: `DefineClass("a/b").Static("n","(I)V",lambda).Virtual(...).Ctor(...).StaticField(...)`. Unknown classes and methods return a generic non-null object and log class, name and signature once. That log is the discovery tool.
- `classes_game.cpp` (545 lines) is the game's Java side: `Game`, `GL2JNILib`, `SUtils`, `Device`, `GameInstaller`, social/IAB/ads no-ops, spoofed device profile. `classes_android.cpp` covers java/lang, Bundle, Context, AudioTrack.

## 6. Graphics, audio, pacing, input, installer

- Graphics: SDL2 with `SDL_HINT_OPENGL_ES_DRIVER` loads ANGLE's EGL/GLES DLLs (requests ES 3.0, falls back to 2.0). One GL thread calls `step`/`resize`/`stateChanged`. The guest's framebuffer 0 is redirected to a frontend FBO, then letterbox-blitted. GL thunks call `::glXxx` directly with `g2h`-translated pointers. `glBufferData` keeps shadow copies for `glMapBufferOES`. `glGetString` and extensions are synthesised with a spoofed GPU name. ETC1 is uploaded as ETC2 or software-decoded.
- Audio: the game uses AudioTrack via JNI (Vox driver). OpenSL is a dead path: `slCreateEngine` returns FEATURE_UNSUPPORTED and the IID data symbols exist. AudioTrack becomes an SDL queue at 44.1 kHz S16 with about 150 ms of buffering; `write` blocks when full.
- Pacing: the game measures real time. The frontend runs `step()`, swaps, applies vsync, and `sleep_until` for a 60 fps limit. The UI thread only queues SDL events; input is injected between frames on the GL thread.
- Input: keyboard, mouse and gamepad are mapped to virtual touches (`nativeOnTouch(action,x,y,id)`, sticks and buttons placed over the HUD) and Android BACK/MENU keycodes.
- Installer: its own ZIP central-directory parser, streaming inflate with CRC check, BCrypt SHA-256, `.part` files renamed on completion. It handles a repack APK (split `archiveObbCache.zip.00N`), an original APK plus loose OBBs, or an APK holding `assets/game.apk`. It copies the unmodified `.so`, OBBs and videos to `data/`, compares known hashes and only warns on mismatch. No game data is shipped. This fits our bring-your-own-APK model.

## 7. Reuse for an A64 guest on x64

| Part | Lines | Reuse | What |
|---|---|---|---|
| core | 310 | 90% | copy |
| cpu | 2200 | 35% | Keep: SVC-halt dispatch, FAST/normal split, re-entrant `call_guest`, per-thread Jit, exclusives, fault dumps, hooks, tracing. Rewrite: AAPCS64 marshalling and `VaList`, A64 `UserConfig`/callbacks (TPIDR_EL0, no CP15), setjmp/longjmp, guest memory (identity map), stubs (`svc #n; ret`); drop kuser |
| loader | 1100 | 40% | Keep: segment mapping, symbol and import bookkeeping, `load_only`, init order. Rewrite: ELF64, RELA (RELATIVE/ABS64/GLOB_DAT/JUMP_SLOT, maybe IRELATIVE, RELR), GNU hash, PT_TLS and TLS relocs. Replace exidx with `dl_iterate_phdr` plus PT_GNU_EH_FRAME |
| hle/libc | 6400 | 50% | Algorithms (format, string, math, wchar, time, vfs/zip/inflate, stdio) reuse. Signatures and all bionic struct layouts change to LP64 (`stat`, `FILE`, `dirent`, pthread types, `long`/`size_t` 64-bit, `jmp_buf`) |
| hle/gles | 1170 | 65% | Bodies and shadow buffers reuse; `g2h` goes away; GLsizeiptr is 64-bit; import list is NOVA's. `texdecode` 95% |
| hle/audio | 380 | 50% | AudioTrack queue reusable if NOVA uses AudioTrack; if it uses real OpenSL, write an engine and bufferqueue |
| jni | 2900 | 55% | `env/runtime/java.h/classes_android` keep their design (widen `jref` to 64 bits, 8-byte slots). `classes_game.cpp` is rewritten from NOVA's dex |
| frontend | 4000 | 60% | Window, ANGLE, FBO, pacing, input, video, menu, saves reuse. SDL2 to SDL3 renames. `game.cpp` natives and startup order are game-specific |
| installer | 745 | 70% | ZIP, inflate, SHA, atomic extraction. New paths (`lib/arm64-v8a/`), hashes and layouts |
| tests | 2650 | 30% | Harness pattern only; guest blobs are ARM32 machine code built from C by `gen_blob*.py` and must be rebuilt for aarch64 |

Overall: about 54% of lines (about 10.5k of 19.2k) are reusable with edits. The A64 core (cpu, loader, ABI-facing HLE) is nearer 35%. About 25% of the code is a near-verbatim copy.

- Copy: `core/*`, `hle/gles/texdecode.*`, `hle/libc/zip.cpp`, installer ZIP/SHA code, `frontend/{input,saves,settings,menu,video,cursor,png}`.
- Adapt: `cpu/cpu.cpp`, `cpu/hle.*`, `loader/loader.cpp`, `hle/libc/{format,string,math,time,vfs,stdio,pthread}`, `hle/gles/gles.cpp`, `jni/{env,runtime,java.h,classes_android}`, `frontend/{game,window,main}`, `cmake/ZwDependencies.cmake`.
- Rewrite: AAPCS64 marshalling and varargs, ELF64 loader parts, guest memory, `classes_game.cpp`, setjmp/longjmp, TLS, test blobs.

## 8. Build, dependencies, licences

- CMake 3.25+, C++20, MSVC via `CMakePresets.json`, vcpkg baseline `177e8175`.
- vcpkg: SDL2, `unofficial-angle`, zlib, nlohmann-json, boost-icl and boost-variant (headers, for dynarmic).
- FetchContent: dear ImGui v1.91.9 (SDL2 and OpenGL3 backends); dynarmic azahar-emu a46601580d55 with bundled externals (`DYNARMIC_FRONTENDS "A32"`; we must enable A64).
- Tests: CTest, one custom-`main` exe per module (core, cpu, loader, libc, gles, audio, jni, installer), no framework. The CPU, loader and JNI tests run embedded machine-code blobs.
- Licences: project code MIT. dynarmic 0BSD; its externals (mcl, xbyak, Zydis/Zycore, fmt, robin-map, biscuit, oaknut) MIT/BSD. ANGLE BSD-3. SDL2 zlib. ImGui MIT. nlohmann MIT. Boost BSL. zlib zlib.
- No vendored third-party code in-tree; the only modification of a dependency is the dynarmic shadow-space patch applied by cmake. The icon and cursor art are the author's and excluded from the MIT grant. Game files are never committed; known SHA-256s of the supported 1.5.3 files are hard-coded.
