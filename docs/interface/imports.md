# Import map: libNOVA.so and libgenerator.so

Generated from the dynamic symbol tables (`.dynsym`, undefined entries) and the version-need records of both ELF files. Both are AArch64 ELF64 shared objects. "refs" counts come from a linear scan of `.text`: direct `bl`/`b` to PLT stubs plus GOT address loads of the import (so it includes address-taken uses, not just calls). "Users" are the namespaces of the containing function; `unexported-code` means the reference sits in a stripped, non-exported function that follows an exported symbol (statically linked OpenSSL/curl/FreeType/glitch internals), so its name is unknown.

## Totals

| Group | libNOVA.so undefined symbols | libgenerator.so undefined symbols |
|---|---|---|
| libc (bionic incl. pthread, stdio) | 311 | 125 |
| libm | 36 | 0 |
| libdl | 1 | 1 |
| libz | 0 | 0 |
| liblog | 1 | 0 |
| libGLESv2 | 96 | 0 |
| libEGL | 18 | 0 |
| libandroid | 25 | 0 |
| libOpenSLES | 4 | 0 |
| C++ runtime (libc++, libc++abi, libunwind) | 0 (statically linked) | 0 (statically linked) |
| Total | 492 (of which 6 data objects) | 126 (of which 1 data object) |

libc breakdown: Process, runtime support, environment 28, Memory 7, Raw memory and string 30, Number conversion, sort, random 19, ctype and locale 23, Wide characters (wchar_t is 32-bit on bionic, 16-bit on Windows) 49, stdio 43, Time 16, pthread, semaphore, scheduler 38, Files, directories, descriptors 26, Sockets, epoll, name resolution, terminal 32.

DT_NEEDED of libNOVA.so: libandroid.so, libGLESv2.so, liblog.so, libc.so, libm.so, libz.so, libdl.so, libOpenSLES.so, libEGL.so. DT_NEEDED of libgenerator.so: libc.so, libm.so, libdl.so. BIND_NOW is set on both (all PLT slots are resolved at load; an unresolved import is a hard load failure, so every name below needs at least a stub).

Notes on what is NOT imported (all verified against the symbol table):

- libz: libz.so is listed as NEEDED but no zlib symbol is imported. zlib (inflate, deflate, crc32, adler32, compressBound...), libzip (zip_open, zip_name_locate), OpenSSL, libcurl 7.59, FreeType, Lua 5.1, libpng-style code and the whole of libc++/libc++abi/libunwind are statically linked and exported from libNOVA.so itself. Nothing to provide for them.
- C++ runtime: no libc++_shared / libstdc++ dependency. `operator new/delete`, `__cxa_throw`, `__gxx_personality_v0`, `__dynamic_cast`, `_Unwind_*` are defined inside the library. The only external need is `dl_iterate_phdr` (see libdl).
- No dlopen/dlsym/dlerror/dladdr: the library never loads anything at run time. Java loads libNOVA.so and libgenerator.so (System.load); libNOVA does not reference libgenerator at all.
- No JNI function imports: JNI is reached through the JNIEnv/JavaVM function tables (see jni.md). The exported `JNI_OnLoad` is called by the loader.
- No `__stack_chk_guard`, no `__cxa_*` from libc: the canary lives in the thread control block (TPIDR_EL0+0x28).
- No mprotect, no getauxval, no clone/fork/exec (except `system`).
- Instruction-set facts the JIT must cover (from a full mnemonic count of libNOVA .text, 6,916,428 instructions): no `svc`, no crypto (aese/sha/pmull/crc32), no LSE atomics (cas/swp/ldadd). Atomics are `ldaxr/stlxr` pairs (7,856 pairs), plus `ldxr/stxr`, `ldar/stlr`, `dmb ish`, `clrex`. System registers read: `TPIDR_EL0` (19,156), `FPCR` (8), `FPSR` (5). `brk #0` 401 (traps), `udf` 35. NEON is used (ld1 411, st1 58, tbl 103, dup 3,039, ld4/st4 18) and hard-float scalar FP everywhere (fmul 37,315, fadd 23,451). `long double` operations are soft-float (compiler-rt) so no quad-precision opcodes are needed.
- Static linking consequences for the loader: 371 DT_INIT_ARRAY constructors (2968 bytes), 4 DT_FINI_ARRAY entries; relocations: 55,619 R_AARCH64_RELATIVE, 26,978 JUMP_SLOT, 7,388 GLOB_DAT, 19,196 ABS64 (no TLS, no IRELATIVE, no COPY). Most JUMP_SLOT/GLOB_DAT entries bind to the library's own exported symbols (it is linked without -Bsymbolic), so the resolver must look in the library itself before the host table, and the host table can override any exported game function by name (this is how engine hooks work, see engine.md).

## How libgenerator.so is used

libNOVA.so does not depend on libgenerator.so (not in DT_NEEDED, no undefined symbol resolves into it, libNOVA never mentions the library name; the word "generator" occurs only inside unrelated OpenSSL/Lua strings). It is a stand-alone JNI library loaded only from Java: `Device.getHDIDFV()` (and the unused IAP wrapper `Utils.getHDIDFV`) call `System.load(DecRawso.GetInstance().a("generator"))` and then the two natives of class `com.gameloft.android.hdidfv.HDIDFV`: `Java_com_gameloft_android_hdidfv_HDIDFV_getNHDIDFV` and `..._getNHDIDFVVersion` (the only two JNI exports, plus its own statically linked libc++/libunwind exports). It generates a random "HDIDFV" device UUID: it reads `/dev/urandom` (fopen "rb"), `time()`, `clock()` and `rand()`; the Java side caches the result in DataSharing and re-validates it against `[a-fA-F0-9]{8}-...-[a-fA-F0-9]{12}`. For the port it is not worth running under the JIT: return a persisted random UUID string and a version string from the host (`getHDIDFV` falls back to `00000000-0000-0000-0000-000000000000` if empty/invalid). If it is run anyway it needs only the libc symbols marked "also libgenerator" plus 11 that libNOVA lacks: `iswalpha iswblank iswcntrl iswdigit iswlower iswprint iswpunct iswspace iswupper iswxdigit` (non-_l variants) and `posix_memalign`.

## libc (bionic) - 311 symbols

Everything here is bionic ABI for LP64 AArch64 (LLVM/Android NDK clang 5.0.300080 + GCC 4.9 headers per `.comment`): `long`, `size_t`, `time_t`, `off_t` (for the *o and 64-suffix paths), `pthread_t` and all pointers are 64-bit; `char` is unsigned; `wchar_t` is a 32-bit `unsigned int`; `long double` is IEEE binary128. Windows x64 differs on `long` (32-bit), `wchar_t` (16-bit), `long double` (64-bit), `time_t`/`off_t` (offsets are 32-bit in `_stat`/`fseek` unless the 64 forms are used), `errno` values, `O_*`/`F_*`/`SOL_*` constants, `struct stat/dirent/tm/sockaddr/fd_set/pollfd/epoll_event` layouts and variadic calling convention (AAPCS64, not Win64). Because the code runs in a JIT, every pointer argument is a guest address; a host libc implementation must read and write guest memory explicitly (or use a guest-to-host window if guest memory is identity-mapped) rather than hand the pointer to the Windows CRT.

### Process, runtime support, environment (28)

- `__cxa_atexit` [also libgenerator] - 6538 refs; C/unexported-code 6117, unexported-code(glitch/gameswf/static libs) 122, asio 119 - (fn,arg,dso_handle) registers static destructors; __dso_handle is a local symbol of the library. Run at unload (optional).
- `__cxa_finalize` [also libgenerator] - 1 refs; C/unexported-code 1 - (dso_handle) run registered destructors for the library.
- `__errno` [also libgenerator] - 205 refs; C/unexported-code 88, asio 27, libc++ 22 - returns int* to the calling thread errno. errno values are Linux numbers (ENOENT 2, EAGAIN/EWOULDBLOCK 11, EINVAL 22, ETIMEDOUT 110, ECONNREFUSED 111, EINPROGRESS 115), not Windows CRT/WSA numbers; every host error must be translated.
- `__stack_chk_fail` [also libgenerator] - 19156 refs; unexported-code(glitch/gameswf/static libs) 3677, libc++ 3200, C/unexported-code 1470 - called on canary mismatch. The canary itself is NOT an import: the code reads it from the thread pointer, `mrs x,TPIDR_EL0 ; ldr x,[x,#0x28]` (bionic arm64 TLS slot 5), 19156 reads of TPIDR_EL0 in libNOVA, no other TPIDR offset sampled. Host must give every guest thread a TCB whose +0x28 is a non-zero constant. No PT_TLS segment, no TLS relocations.
- `abort` [also libgenerator] - 1023 refs; libc++ 931, unexported-code(glitch/gameswf/static libs) 42, C/unexported-code 32 - terminates; also used by statically linked libc++abi on uncaught exception. Host should log guest PC/stack and stop that process.
- `android_set_abort_message` [also libgenerator] - 1 refs; libc++ 1 - bionic-only, (const char*) stored for tombstones; no-op on host.
- `closelog` [also libgenerator] - 1 refs; libc++ 1 - only reached from libc++abi std::bad_typeid::what(); log or ignore.
- `exit` - 5 refs; unexported-code(OpenSSL/curl) 2, automation 2, unexported-code(glitch/gameswf/static libs) 1
- `getegid` - 1 refs; unexported-code(OpenSSL/curl) 1
- `getenv` - 14 refs; unexported-code(OpenSSL/curl) 11, unexported-code(glitch/gameswf/static libs) 2, C/unexported-code 1 - names read: HB_OPTIONS, HB_SHAPER_LIST (HarfBuzz), OPENSSL_CONF, OPENSSL_ENGINES, RANDFILE, HOME, OPENSSL_ALLOW_PROXY_CERTS; returning NULL is fine.
- `geteuid` - 2 refs; C/unexported-code 1, unexported-code(OpenSSL/curl) 1
- `getgid` - 1 refs; unexported-code(OpenSSL/curl) 1
- `gethostname` - 2 refs; Comms 1, glf 1
- `getpagesize` - 2 refs; glf 2 - guest-visible page size must stay 4096 even though Windows allocation granularity is 64 KiB.
- `getpid` - 11 refs; C/unexported-code 9, unexported-code(OpenSSL/curl) 2
- `getpwuid` - 1 refs; C/unexported-code 1 - curl .netrc lookup only (Curl_parsenetrc). Return NULL.
- `gettid` - 2 refs; glf 2
- `getuid` - 2 refs; unexported-code(OpenSSL/curl) 2
- `longjmp` - 101 refs; unexported-code(glitch/gameswf/static libs) 55, gaia 43, C/unexported-code 2 - non-signal variants, used by statically linked C libraries: FreeType (ft_validator_error, ps_hints_apply), an image-library region after gaia::HTTPRequestManager::IsSuccess, and the OpenSSL region. The jmp_buf is a bionic arm64 buffer in GUEST memory: setjmp/longjmp must save and restore guest registers x19-x30, sp, d8-d15, not host state.
- `openlog` [also libgenerator] - 1 refs; libc++ 1 - only reached from libc++abi std::bad_typeid::what(); log or ignore.
- `setjmp` - 5 refs; C/unexported-code 2, gaia 1, unexported-code(glitch/gameswf/static libs) 1 - non-signal variants, used by statically linked C libraries: FreeType (ft_validator_error, ps_hints_apply), an image-library region after gaia::HTTPRequestManager::IsSuccess, and the OpenSSL region. The jmp_buf is a bionic arm64 buffer in GUEST memory: setjmp/longjmp must save and restore guest registers x19-x30, sp, d8-d15, not host state.
- `sigaction` - 2 refs; unexported-code(OpenSSL/curl) 2 - one site (OpenSSL, same region as signal and setjmp/longjmp). Struct sigaction is the bionic arm64 layout (handler, flags, restorer, mask). No-op is acceptable; there is no crypto-instruction code in libNOVA (no aese/sha/pmull opcodes), so no SIGILL probing is needed.
- `signal` - 1 refs; unexported-code(OpenSSL/curl) 1 - one site, inside the statically linked OpenSSL code (tls1_alert_code is only the nearest exported symbol). With the JIT: make it a no-op returning SIG_DFL; no real signals are needed.
- `syscall` - 2 refs; glf 2 - variadic. Only two numbers seen, both aarch64 syscall numbers: 122 sched_setaffinity (glf::Thread::Impl::SetCoreAffinity) and 123 sched_getaffinity (GetCoreAffinity). Map to SetThreadAffinityMask or ignore.
- `sysconf` [also libgenerator] - 3 refs; C/unexported-code 1, automation 1, libc++ 1 - values seen: 39 and 40 (bionic _SC_PAGESIZE 0x27 / _SC_PAGE_SIZE 0x28; dlmalloc create_mspace and getCurrentResidentSetSize) and 97 (_SC_NPROCESSORS_ONLN 0x61; std::thread::hardware_concurrency). Constants are bionic values (glibc/musl differ for some). Return 4096 for page size.
- `syslog` [also libgenerator] - 1 refs; libc++ 1 - only reached from libc++abi std::bad_typeid::what(); log or ignore.
- `system` - 2 refs; CTexUpdater 1, unexported-code(OpenSSL/curl) 1 - two sites: OpenSSL console code and CTexUpdater::Run(), which runs the desktop dev command "..\..\make\BuildData.py -fast -dontexportlevels" (leftover from the Windows build, not reachable in a retail flow). Stub: return -1.
- `timezone` [DATA] - 3 refs; C/unexported-code 2, CShopManager 1 - DATA. bionic `long timezone` (8 bytes on arm64, 4 on Win32); seconds west of UTC. Only read, never written, as far as call sites show.

### Memory (7)

- `calloc` [also libgenerator] - 39 refs; unexported-code(glitch/gameswf/static libs) 19, unexported-code(OpenSSL/curl) 12, C/unexported-code 7 - bionic dlmalloc semantics: malloc(0) returns a unique pointer, memalign(alignment,size) alignment power of two. Guest memory must be 16-byte aligned. The game also carries its own dlmalloc (create_mspace) on top of mmap. libgenerator additionally imports posix_memalign.
- `free` [also libgenerator] - 1255 refs; C/unexported-code 923, libc++ 95, unexported-code(OpenSSL/curl) 94 - bionic dlmalloc semantics: malloc(0) returns a unique pointer, memalign(alignment,size) alignment power of two. Guest memory must be 16-byte aligned. The game also carries its own dlmalloc (create_mspace) on top of mmap. libgenerator additionally imports posix_memalign.
- `malloc` [also libgenerator] - 542 refs; C/unexported-code 313, glotv3 74, unexported-code(OpenSSL/curl) 50 - bionic dlmalloc semantics: malloc(0) returns a unique pointer, memalign(alignment,size) alignment power of two. Guest memory must be 16-byte aligned. The game also carries its own dlmalloc (create_mspace) on top of mmap. libgenerator additionally imports posix_memalign.
- `memalign` - 3 refs; unexported-code(OpenSSL/curl) 3 - bionic dlmalloc semantics: malloc(0) returns a unique pointer, memalign(alignment,size) alignment power of two. Guest memory must be 16-byte aligned. The game also carries its own dlmalloc (create_mspace) on top of mmap. libgenerator additionally imports posix_memalign.
- `mmap` - 2 refs; glf 1, C/unexported-code 1 - two kinds of use: anonymous (flags 0x22 = MAP_PRIVATE|MAP_ANONYMOUS, fd -1, dlmalloc create_mspace) and file mapping (glf::io2::FilePosix::MapV). Linux flag/prot numbers (PROT_READ 1, PROT_WRITE 2, MAP_PRIVATE 2, MAP_ANONYMOUS 0x20). No mprotect, madvise, mremap imported. Offsets are 64-bit off_t.
- `munmap` - 5 refs; glf 5 - (addr,len) of the above.
- `realloc` [also libgenerator] - 41 refs; libc++ 12, unexported-code(OpenSSL/curl) 10, C/unexported-code 8 - bionic dlmalloc semantics: malloc(0) returns a unique pointer, memalign(alignment,size) alignment power of two. Guest memory must be 16-byte aligned. The game also carries its own dlmalloc (create_mspace) on top of mmap. libgenerator additionally imports posix_memalign.

### Raw memory and string (30)

- `memchr` [also libgenerator] - 53 refs; C/unexported-code 21, glf 6, glotv3 4
- `memcmp` [also libgenerator] - 940 refs; libc++ 375, C/unexported-code 82, glotv3 46
- `memcpy` [also libgenerator] - 9187 refs; gaia 2577, adslib 1085, C/unexported-code 937
- `memmove` [also libgenerator] - 622 refs; libc++ 329, unexported-code(glitch/gameswf/static libs) 81, C/unexported-code 56
- `memset` [also libgenerator] - 1710 refs; C/unexported-code 427, unexported-code(glitch/gameswf/static libs) 253, unexported-code(OpenSSL/curl) 203
- `strcasecmp` - 138 refs; vox 93, unexported-code(glitch/gameswf/static libs) 21, libc++ 12
- `strcat` - 26 refs; C/unexported-code 9, CLuaScriptManager 5, glf 3
- `strchr` - 172 refs; C/unexported-code 75, unexported-code(glitch/gameswf/static libs) 40, unexported-code(OpenSSL/curl) 39
- `strcmp` [also libgenerator] - 1637 refs; C/unexported-code 299, unexported-code(OpenSSL/curl) 229, unexported-code(glitch/gameswf/static libs) 208
- `strcoll` [also libgenerator] - 3 refs; unexported-code(OpenSSL/curl) 2, libc++ 1 - C-locale byte compare in practice; bionic locale_t is an opaque pointer, LC_GLOBAL_LOCALE is (locale_t)-1.
- `strcoll_l` - 1 refs; libc++ 1 - C-locale byte compare in practice; bionic locale_t is an opaque pointer, LC_GLOBAL_LOCALE is (locale_t)-1.
- `strcpy` - 104 refs; vox 21, glf 19, unexported-code(glitch/gameswf/static libs) 18
- `strcspn` - 4 refs; C/unexported-code 2, unexported-code(OpenSSL/curl) 1, automation 1
- `strdup` - 7 refs; C/unexported-code 7
- `strerror` - 21 refs; unexported-code(OpenSSL/curl) 6, C/unexported-code 5, automation 4 - bionic strings (e.g. "No such file or directory"); strerror_r is the XSI variant returning int.
- `strerror_r` [also libgenerator] - 3 refs; asio 1, C/unexported-code 1, automation 1 - bionic strings (e.g. "No such file or directory"); strerror_r is the XSI variant returning int.
- `strlen` [also libgenerator] - 7961 refs; C/unexported-code 1692, gaia 1666, adslib 1028
- `strncasecmp` - 10 refs; unexported-code(OpenSSL/curl) 6, vox 4
- `strncat` - 2 refs; OnlineManager 1, unexported-code(OpenSSL/curl) 1
- `strncmp` - 187 refs; C/unexported-code 60, unexported-code(OpenSSL/curl) 49, unexported-code(glitch/gameswf/static libs) 23
- `strncpy` - 78 refs; unexported-code(glitch/gameswf/static libs) 31, vox 12, unexported-code(OpenSSL/curl) 11
- `strnlen` - 2 refs; OnlineManager 2
- `strpbrk` - 6 refs; C/unexported-code 2, glwebtools 1, glf 1
- `strrchr` - 29 refs; C/unexported-code 10, vox 7, unexported-code(glitch/gameswf/static libs) 4
- `strspn` - 5 refs; CTTFFontSpr 5
- `strstr` - 135 refs; unexported-code(glitch/gameswf/static libs) 43, C/unexported-code 27, unexported-code(OpenSSL/curl) 9
- `strtok` - 15 refs; vox 6, CStatesSetComponent 4, crm 3
- `strtok_r` - 7 refs; C/unexported-code 7
- `strxfrm` [also libgenerator] - 1 refs; libc++ 1 - C-locale byte compare in practice; bionic locale_t is an opaque pointer, LC_GLOBAL_LOCALE is (locale_t)-1.
- `strxfrm_l` - 2 refs; libc++ 2 - C-locale byte compare in practice; bionic locale_t is an opaque pointer, LC_GLOBAL_LOCALE is (locale_t)-1.

### Number conversion, sort, random (19)

- `atof` - 45 refs; CCTweakable 14, CStatesSetComponent 7, gameoptions 6
- `atoi` - 146 refs; glwebtools 19, unexported-code(glitch/gameswf/static libs) 18, C/unexported-code 15
- `atol` - 2 refs; C/unexported-code 2
- `bsearch` - 13 refs; unexported-code(glitch/gameswf/static libs) 13 - comparator is a GUEST function pointer: host must call back into the JIT for every comparison (or re-implement the sort inside guest-callable code).
- `isnan` - 2 refs; automation 2 - function form (not the macro); used on float/double via libm headers.
- `ldexp` - 1 refs; unexported-code(OpenSSL/curl) 1 - out-pointer parameters (int* / double* / float*) are guest memory.
- `qsort` - 19 refs; C/unexported-code 9, unexported-code(OpenSSL/curl) 7, unexported-code(glitch/gameswf/static libs) 3 - comparator is a GUEST function pointer: host must call back into the JIT for every comparison (or re-implement the sort inside guest-callable code).
- `rand` [also libgenerator] - 100 refs; vox 18, 0> 8, unexported-code(glitch/gameswf/static libs) 8 - bionic rand() RAND_MAX is 0x7fffffff; Windows RAND_MAX is 0x7fff. Games may rely on sequence stability for deterministic effects.
- `srand` - 5 refs; Application 1, gaia 1, C/unexported-code 1 - bionic rand() RAND_MAX is 0x7fffffff; Windows RAND_MAX is 0x7fff. Games may rely on sequence stability for deterministic effects.
- `strtod` [also libgenerator] - 15 refs; unexported-code(glitch/gameswf/static libs) 9, glf 2, libc++ 2
- `strtof` [also libgenerator] - 2 refs; libc++ 2
- `strtol` [also libgenerator] - 29 refs; C/unexported-code 8, of 6, unexported-code(OpenSSL/curl) 5
- `strtold` [also libgenerator] - 2 refs; libc++ 2 - long double on arm64 is IEEE binary128 (16 bytes, returned in q0) and is soft-float (the library statically links compiler-rt __trunctfdf2 etc.). Host x86-64 has no quad type; parse into double and widen to binary128 by hand.
- `strtold_l` [also libgenerator] - 3 refs; std 3 - long double on arm64 is IEEE binary128 (16 bytes, returned in q0) and is soft-float (the library statically links compiler-rt __trunctfdf2 etc.). Host x86-64 has no quad type; parse into double and widen to binary128 by hand.
- `strtoll` [also libgenerator] - 2 refs; C/unexported-code 1, libc++ 1
- `strtoll_l` [also libgenerator] - 2 refs; std 2
- `strtoul` [also libgenerator] - 14 refs; C/unexported-code 6, unexported-code(OpenSSL/curl) 4, CShopManager 1
- `strtoull` [also libgenerator] - 2 refs; gaia 1, libc++ 1
- `strtoull_l` [also libgenerator] - 4 refs; std 3, libc++ 1

### ctype and locale (23)

- `__ctype_get_mb_cur_max` [also libgenerator] - 5 refs; libc++ 5
- `_ctype_` [DATA] - 1 refs; libc++ 1 - DATA. bionic ctype table, 1+256 bytes indexed by (c+1), flag bits _U=1 _L=2 _N=4 _S=8 _P=0x10 _C=0x20 _X=0x40 _B=0x80 (bionic ctype.h). Used by inlined isalpha-style macros, so a real table must exist in guest memory.
- `freelocale` [also libgenerator] - 41 refs; libc++ 41 - called by libc++ std::locale. Only "C" and ""/"C" are requested (see setlocale sites with "C"). Categories use bionic values LC_CTYPE 0 .. LC_ALL 6 and mask forms for newlocale. localeconv returns a struct lconv in guest memory.
- `isalnum` - 11 refs; unexported-code(OpenSSL/curl) 4, olutils 3, unexported-code(glitch/gameswf/static libs) 1
- `isalpha` - 7 refs; C/unexported-code 2, unexported-code(OpenSSL/curl) 2, Comms 1
- `iscntrl` - 7 refs; acp_utils 3, unexported-code(OpenSSL/curl) 2, glotv3 1
- `isdigit_l` - 2 refs; libc++ 2
- `islower` [also libgenerator] - 2 refs; unexported-code(OpenSSL/curl) 2
- `islower_l` - 3 refs; libc++ 3
- `ispunct` - 2 refs; boost 1, unexported-code(OpenSSL/curl) 1
- `isspace` - 31 refs; unexported-code(OpenSSL/curl) 18, C/unexported-code 6, acp_utils 2
- `isupper` [also libgenerator] - 5 refs; unexported-code(OpenSSL/curl) 3, C/unexported-code 2
- `isupper_l` - 4 refs; libc++ 4
- `isxdigit` [also libgenerator] - 8 refs; C/unexported-code 6, unexported-code(OpenSSL/curl) 2
- `isxdigit_l` - 2 refs; libc++ 2
- `localeconv` [also libgenerator] - 6 refs; libc++ 6 - called by libc++ std::locale. Only "C" and ""/"C" are requested (see setlocale sites with "C"). Categories use bionic values LC_CTYPE 0 .. LC_ALL 6 and mask forms for newlocale. localeconv returns a struct lconv in guest memory.
- `newlocale` [also libgenerator] - 73 refs; libc++ 65, std 8 - called by libc++ std::locale. Only "C" and ""/"C" are requested (see setlocale sites with "C"). Categories use bionic values LC_CTYPE 0 .. LC_ALL 6 and mask forms for newlocale. localeconv returns a struct lconv in guest memory.
- `setlocale` [also libgenerator] - 21 refs; libc++ 10, C/unexported-code 10, unexported-code(OpenSSL/curl) 1 - called by libc++ std::locale. Only "C" and ""/"C" are requested (see setlocale sites with "C"). Categories use bionic values LC_CTYPE 0 .. LC_ALL 6 and mask forms for newlocale. localeconv returns a struct lconv in guest memory.
- `tolower` [also libgenerator] - 78 refs; glf 34, C/unexported-code 9, unexported-code(glitch/gameswf/static libs) 8
- `tolower_l` - 2 refs; libc++ 2
- `toupper` [also libgenerator] - 9 refs; Materials_Tweakable 2, glf 2, InGameBrowser 2
- `toupper_l` - 2 refs; libc++ 2
- `uselocale` [also libgenerator] - 112 refs; libc++ 112 - called by libc++ std::locale. Only "C" and ""/"C" are requested (see setlocale sites with "C"). Categories use bionic values LC_CTYPE 0 .. LC_ALL 6 and mask forms for newlocale. localeconv returns a struct lconv in guest memory.

### Wide characters (wchar_t is 32-bit on bionic, 16-bit on Windows) (49)

- `btowc` [also libgenerator] - 3 refs; libc++ 3
- `getwc` - 3 refs; C/unexported-code 3
- `iswalpha_l` - 4 refs; libc++ 4
- `iswblank_l` - 4 refs; libc++ 4
- `iswcntrl_l` - 4 refs; libc++ 4
- `iswctype` - 2 refs; libc++ 2
- `iswdigit_l` - 4 refs; libc++ 4
- `iswlower_l` - 5 refs; libc++ 5
- `iswprint_l` - 4 refs; libc++ 4
- `iswpunct_l` - 4 refs; libc++ 4
- `iswspace_l` - 4 refs; libc++ 4
- `iswupper_l` - 4 refs; libc++ 4
- `iswxdigit_l` - 4 refs; libc++ 4
- `mbrlen` [also libgenerator] - 1 refs; libc++ 1
- `mbrtowc` [also libgenerator] - 4 refs; libc++ 4
- `mbsnrtowcs` [also libgenerator] - 1 refs; libc++ 1
- `mbsrtowcs` [also libgenerator] - 14 refs; libc++ 14
- `mbtowc` [also libgenerator] - 1 refs; libc++ 1
- `putwc` - 2 refs; C/unexported-code 2
- `swprintf` [also libgenerator] - 11 refs; libc++ 9, unexported-code(glitch/gameswf/static libs) 2 - variadic, AAPCS64: integer args in x1-x7 then stack, floating args in d0-d7 (promoted to double) then stack. Host formatter must read the guest register file/stack. Use a custom formatter, not MSVCRT: %ld/%lu are 64-bit, %zu/%zd 64-bit, %p prints lowercase 0x..., %lld/%llu, %ls/%lc take 32-bit wchar_t.
- `towlower` [also libgenerator] - 2 refs; libc++ 2
- `towlower_l` - 2 refs; libc++ 2
- `towupper` [also libgenerator] - 2 refs; libc++ 2
- `towupper_l` - 2 refs; libc++ 2
- `ungetwc` - 3 refs; C/unexported-code 3
- `wcrtomb` [also libgenerator] - 5 refs; libc++ 5
- `wcscmp` - 8 refs; unexported-code(glitch/gameswf/static libs) 8
- `wcscoll` [also libgenerator] - 1 refs; libc++ 1
- `wcscoll_l` - 1 refs; libc++ 1
- `wcscpy` - 1 refs; unexported-code(glitch/gameswf/static libs) 1
- `wcsftime` - 2 refs; C/unexported-code 2 - bionic format set (%z, %s, %e supported); strptime is POSIX, not available in the Windows CRT.
- `wcslen` [also libgenerator] - 208 refs; unexported-code(glitch/gameswf/static libs) 169, C/unexported-code 16, libc++ 12
- `wcsnrtombs` [also libgenerator] - 1 refs; libc++ 1
- `wcstod` [also libgenerator] - 1 refs; libc++ 1
- `wcstof` [also libgenerator] - 1 refs; libc++ 1
- `wcstol` [also libgenerator] - 2 refs; libc++ 2
- `wcstold` [also libgenerator] - 1 refs; libc++ 1 - long double on arm64 is IEEE binary128 (16 bytes, returned in q0) and is soft-float (the library statically links compiler-rt __trunctfdf2 etc.). Host x86-64 has no quad type; parse into double and widen to binary128 by hand.
- `wcstoll` [also libgenerator] - 1 refs; libc++ 1
- `wcstoul` [also libgenerator] - 1 refs; libc++ 1
- `wcstoull` [also libgenerator] - 1 refs; libc++ 1
- `wcsxfrm` [also libgenerator] - 1 refs; libc++ 1
- `wcsxfrm_l` - 2 refs; libc++ 2
- `wctob` [also libgenerator] - 6 refs; libc++ 6
- `wctype` - 11 refs; libc++ 11
- `wmemchr` [also libgenerator] - 16 refs; C/unexported-code 11, libc++ 4, unexported-code(glitch/gameswf/static libs) 1
- `wmemcmp` [also libgenerator] - 7 refs; unexported-code(glitch/gameswf/static libs) 5, glf 1, libc++ 1
- `wmemcpy` [also libgenerator] - 38 refs; C/unexported-code 18, libc++ 9, GS_Lockout 3
- `wmemmove` [also libgenerator] - 19 refs; libc++ 13, unexported-code(glitch/gameswf/static libs) 2, C/unexported-code 2
- `wmemset` [also libgenerator] - 22 refs; libc++ 14, C/unexported-code 6, unexported-code(glitch/gameswf/static libs) 2

### stdio (43)

- `__sF` [DATA, also libgenerator] - 1012 refs; libc++ 933, unexported-code(glitch/gameswf/static libs) 46, C/unexported-code 11 - DATA. Array of 3 bionic FILE structs, sizeof(FILE)=152: stdin=__sF, stdout=__sF+152 (11 code sites), stderr=__sF+304 (993 sites, almost all fprintf/fputs for logging). Host must export the array in guest memory and recognise these 3 addresses in fprintf/fputs/fflush/fwrite.
- `clearerr` - 4 refs; C/unexported-code 3, unexported-code(OpenSSL/curl) 1
- `fclose` [also libgenerator] - 71 refs; C/unexported-code 29, unexported-code(OpenSSL/curl) 9, SocialLibManager 8
- `fdopen` - 2 refs; C/unexported-code 2 - FILE objects are opaque to the guest except __sF; descriptors are Linux ints (0,1,2 standard).
- `feof` - 3 refs; unexported-code(OpenSSL/curl) 3
- `ferror` - 14 refs; C/unexported-code 9, unexported-code(OpenSSL/curl) 5
- `fflush` - 20 refs; unexported-code(OpenSSL/curl) 8, C/unexported-code 7, glf 2
- `fgets` - 9 refs; unexported-code(OpenSSL/curl) 6, C/unexported-code 3 - plain functions (not macros touching FILE internals) in this binary; FILE layout is never read directly except via the 3 __sF entries.
- `fileno` - 5 refs; unexported-code(OpenSSL/curl) 3, C/unexported-code 1, glf 1 - FILE objects are opaque to the guest except __sF; descriptors are Linux ints (0,1,2 standard).
- `fopen` [also libgenerator] - 86 refs; C/unexported-code 24, unexported-code(OpenSSL/curl) 11, gameoptions 7 - path is a GUEST string using Android paths (/data/data/com.gameloft.android.ANMP.GloftNOHM/..., /sdcard/..., relative paths): host needs a virtual file system that maps those to Windows folders and is case-sensitive-compatible. Mode strings "rb", "wb", "r+b" etc.
- `fprintf` - 1008 refs; libc++ 930, unexported-code(glitch/gameswf/static libs) 45, C/unexported-code 15 - variadic, AAPCS64: integer args in x1-x7 then stack, floating args in d0-d7 (promoted to double) then stack. Host formatter must read the guest register file/stack. Use a custom formatter, not MSVCRT: %ld/%lu are 64-bit, %zu/%zd 64-bit, %p prints lowercase 0x..., %lld/%llu, %ls/%lc take 32-bit wchar_t.
- `fputc` [also libgenerator] - 16 refs; C/unexported-code 13, unexported-code(OpenSSL/curl) 1, libc++ 1 - plain functions (not macros touching FILE internals) in this binary; FILE layout is never read directly except via the 3 __sF entries.
- `fputs` - 18 refs; unexported-code(OpenSSL/curl) 8, C/unexported-code 3, __gnu_cxx 3 - plain functions (not macros touching FILE internals) in this binary; FILE layout is never read directly except via the 3 __sF entries.
- `fread` [also libgenerator] - 41 refs; C/unexported-code 27, unexported-code(OpenSSL/curl) 5, glf 2
- `freopen` - 1 refs; unexported-code(OpenSSL/curl) 1 - FILE objects are opaque to the guest except __sF; descriptors are Linux ints (0,1,2 standard).
- `fscanf` - 6 refs; SocialLibManager 4, unexported-code(OpenSSL/curl) 1, automation 1 - variadic, AAPCS64: integer args in x1-x7 then stack, floating args in d0-d7 (promoted to double) then stack. Host formatter must read the guest register file/stack. Use a custom formatter, not MSVCRT: %ld/%lu are 64-bit, %zu/%zd 64-bit, %p prints lowercase 0x..., %lld/%llu, %ls/%lc take 32-bit wchar_t.
- `fseek` - 41 refs; C/unexported-code 24, glf 5, automation 3 - off_t is 64-bit (fseeko/ftello); fseek/ftell take 64-bit long.
- `fseeko` - 15 refs; C/unexported-code 15 - off_t is 64-bit (fseeko/ftello); fseek/ftell take 64-bit long.
- `ftell` - 21 refs; C/unexported-code 10, glf 3, unexported-code(OpenSSL/curl) 2 - off_t is 64-bit (fseeko/ftello); fseek/ftell take 64-bit long.
- `ftello` - 9 refs; C/unexported-code 9 - off_t is 64-bit (fseeko/ftello); fseek/ftell take 64-bit long.
- `funopen` - 1 refs; C/unexported-code 1 - BSD/bionic-only: funopen(cookie,readfn,writefn,seekfn,closefn) returns a FILE*. Single site: android_fopen() wraps an AAsset in a FILE (used by glf::FileStreamImpl::Impl::Open as the fallback when a path is not a real file). The four callbacks are guest function pointers that the host libc must invoke from fread/fseek/fclose; seek uses fpos_t/off_t 64-bit.
- `fwrite` - 61 refs; C/unexported-code 33, libc++ 8, unexported-code(glitch/gameswf/static libs) 6
- `getc` - 11 refs; unexported-code(OpenSSL/curl) 5, libc++ 4, C/unexported-code 2 - plain functions (not macros touching FILE internals) in this binary; FILE layout is never read directly except via the 3 __sF entries.
- `perror` - 1 refs; C/unexported-code 1
- `printf` - 34 refs; unexported-code(glitch/gameswf/static libs) 10, C/unexported-code 6, glf 5 - variadic, AAPCS64: integer args in x1-x7 then stack, floating args in d0-d7 (promoted to double) then stack. Host formatter must read the guest register file/stack. Use a custom formatter, not MSVCRT: %ld/%lu are 64-bit, %zu/%zd 64-bit, %p prints lowercase 0x..., %lld/%llu, %ls/%lc take 32-bit wchar_t.
- `putc` - 33 refs; C/unexported-code 33 - plain functions (not macros touching FILE internals) in this binary; FILE layout is never read directly except via the 3 __sF entries.
- `putchar` - 3 refs; C/unexported-code 1, CSceneEffectScan 1, glwebtools 1 - plain functions (not macros touching FILE internals) in this binary; FILE layout is never read directly except via the 3 __sF entries.
- `puts` - 12 refs; glf 4, unexported-code(OpenSSL/curl) 3, VertexColorBaker 2 - plain functions (not macros touching FILE internals) in this binary; FILE layout is never read directly except via the 3 __sF entries.
- `remove` - 25 refs; Application 9, C/unexported-code 4, glf 2 - POSIX semantics, Linux flag numbers; mkdir mode argument ignored on Windows. lseek/pread use 64-bit off_t. access modes F_OK 0, R_OK 4, W_OK 2.
- `rename` - 9 refs; glf 3, C/unexported-code 1, olutils 1 - POSIX semantics, Linux flag numbers; mkdir mode argument ignored on Windows. lseek/pread use 64-bit off_t. access modes F_OK 0, R_OK 4, W_OK 2.
- `rewind` - 3 refs; C/unexported-code 1, ysM 1, automation 1
- `setvbuf` - 3 refs; unexported-code(OpenSSL/curl) 2, C/unexported-code 1 - FILE objects are opaque to the guest except __sF; descriptors are Linux ints (0,1,2 standard).
- `snprintf` [also libgenerator] - 78 refs; unexported-code(glitch/gameswf/static libs) 16, C/unexported-code 14, libc++ 11 - variadic, AAPCS64: integer args in x1-x7 then stack, floating args in d0-d7 (promoted to double) then stack. Host formatter must read the guest register file/stack. Use a custom formatter, not MSVCRT: %ld/%lu are 64-bit, %zu/%zd 64-bit, %p prints lowercase 0x..., %lld/%llu, %ls/%lc take 32-bit wchar_t.
- `sprintf` - 316 refs; C/unexported-code 60, unexported-code(glitch/gameswf/static libs) 36, GameMpManager 16 - variadic, AAPCS64: integer args in x1-x7 then stack, floating args in d0-d7 (promoted to double) then stack. Host formatter must read the guest register file/stack. Use a custom formatter, not MSVCRT: %ld/%lu are 64-bit, %zu/%zd 64-bit, %p prints lowercase 0x..., %lld/%llu, %ls/%lc take 32-bit wchar_t.
- `sscanf` [also libgenerator] - 63 refs; C/unexported-code 21, unexported-code(glitch/gameswf/static libs) 7, unexported-code(OpenSSL/curl) 6 - variadic, AAPCS64: integer args in x1-x7 then stack, floating args in d0-d7 (promoted to double) then stack. Host formatter must read the guest register file/stack. Use a custom formatter, not MSVCRT: %ld/%lu are 64-bit, %zu/%zd 64-bit, %p prints lowercase 0x..., %lld/%llu, %ls/%lc take 32-bit wchar_t.
- `tmpfile` - 1 refs; unexported-code(OpenSSL/curl) 1 - FILE objects are opaque to the guest except __sF; descriptors are Linux ints (0,1,2 standard).
- `tmpnam` - 1 refs; unexported-code(OpenSSL/curl) 1 - FILE objects are opaque to the guest except __sF; descriptors are Linux ints (0,1,2 standard).
- `ungetc` - 9 refs; libc++ 4, C/unexported-code 3, unexported-code(OpenSSL/curl) 2 - plain functions (not macros touching FILE internals) in this binary; FILE layout is never read directly except via the 3 __sF entries.
- `vasprintf` [also libgenerator] - 2 refs; libc++ 2 - take a va_list. On arm64 that is a 32-byte struct {void* stack; void* gr_top; void* vr_top; int gr_offs; int vr_offs;} in guest memory (not Win64 char*). vasprintf result must be freed by guest free().
- `vfprintf` [also libgenerator] - 2 refs; unexported-code(OpenSSL/curl) 1, libc++ 1 - take a va_list. On arm64 that is a 32-byte struct {void* stack; void* gr_top; void* vr_top; int gr_offs; int vr_offs;} in guest memory (not Win64 char*). vasprintf result must be freed by guest free().
- `vsnprintf` [also libgenerator] - 25 refs; glf 14, unexported-code(glitch/gameswf/static libs) 5, NetworkLog 2 - take a va_list. On arm64 that is a 32-byte struct {void* stack; void* gr_top; void* vr_top; int gr_offs; int vr_offs;} in guest memory (not Win64 char*). vasprintf result must be freed by guest free().
- `vsprintf` - 7 refs; CLuaScriptManager 4, C/unexported-code 3 - take a va_list. On arm64 that is a 32-byte struct {void* stack; void* gr_top; void* vr_top; int gr_offs; int vr_offs;} in guest memory (not Win64 char*). vasprintf result must be freed by guest free().
- `vsscanf` [also libgenerator] - 1 refs; libc++ 1 - take a va_list. On arm64 that is a 32-byte struct {void* stack; void* gr_top; void* vr_top; int gr_offs; int vr_offs;} in guest memory (not Win64 char*). vasprintf result must be freed by guest free().

### Time (16)

- `asctime` - 3 refs; acp_utils 2, livelogger 1
- `clock` [also libgenerator] - 2 refs; unexported-code(OpenSSL/curl) 2
- `clock_gettime` - 8 refs; C/unexported-code 3, libc++ 2, olutils 1 - clockid_t values are Linux: CLOCK_REALTIME 0, CLOCK_MONOTONIC 1. struct timespec {int64 tv_sec; int64 tv_nsec}. 8 sites: glf::GetTicks, GetMonotonicTimeMilliseconds, libc++ steady_clock/system_clock, curl, social lib.
- `difftime` - 7 refs; automation 4, of 1, olutils 1
- `gettimeofday` - 22 refs; C/unexported-code 6, vox 5, gameoptions 5 - struct timeval {int64 tv_sec; int64 tv_usec}; tz argument NULL.
- `gmtime` - 5 refs; C/unexported-code 2, of 2, unexported-code(OpenSSL/curl) 1 - bionic struct tm is 56 bytes: 9 ints, 4 bytes pad, long tm_gmtoff, const char* tm_zone. Windows struct tm lacks tm_gmtoff/tm_zone; fill them. localtime must honour the PC time zone (bionic reads the Android zone database).
- `gmtime_r` - 5 refs; olutils 4, unexported-code(OpenSSL/curl) 1 - bionic struct tm is 56 bytes: 9 ints, 4 bytes pad, long tm_gmtoff, const char* tm_zone. Windows struct tm lacks tm_gmtoff/tm_zone; fill them. localtime must honour the PC time zone (bionic reads the Android zone database).
- `localtime` - 23 refs; unexported-code(glitch/gameswf/static libs) 9, PushNotifications 4, C/unexported-code 2 - bionic struct tm is 56 bytes: 9 ints, 4 bytes pad, long tm_gmtoff, const char* tm_zone. Windows struct tm lacks tm_gmtoff/tm_zone; fill them. localtime must honour the PC time zone (bionic reads the Android zone database).
- `localtime_r` - 6 refs; acp_utils 2, olutils 2, automation 2 - bionic struct tm is 56 bytes: 9 ints, 4 bytes pad, long tm_gmtoff, const char* tm_zone. Windows struct tm lacks tm_gmtoff/tm_zone; fill them. localtime must honour the PC time zone (bionic reads the Android zone database).
- `mktime` - 14 refs; glwebtools 4, C/unexported-code 3, gaia 3 - bionic struct tm is 56 bytes: 9 ints, 4 bytes pad, long tm_gmtoff, const char* tm_zone. Windows struct tm lacks tm_gmtoff/tm_zone; fill them. localtime must honour the PC time zone (bionic reads the Android zone database).
- `nanosleep` [also libgenerator] - 1 refs; libc++ 1 - Application::LimitFps() frame limiter sleeps with usleep(ms*1000) and std::this_thread::sleep_for uses nanosleep. Both must really sleep; use high-resolution waits (Windows default granularity is 15 ms).
- `strftime` [also libgenerator] - 10 refs; olutils 3, gladsv2 2, automation 2 - bionic format set (%z, %s, %e supported); strptime is POSIX, not available in the Windows CRT.
- `strftime_l` - 17 refs; libc++ 17 - bionic format set (%z, %s, %e supported); strptime is POSIX, not available in the Windows CRT.
- `strptime` - 1 refs; gaia 1 - bionic format set (%z, %s, %e supported); strptime is POSIX, not available in the Windows CRT.
- `time` [also libgenerator] - 115 refs; C/unexported-code 32, automation 15, unexported-code(OpenSSL/curl) 7 - time_t is 64-bit.
- `usleep` - 20 refs; glwebtools 12, glf 3, vox 2 - Application::LimitFps() frame limiter sleeps with usleep(ms*1000) and std::this_thread::sleep_for uses nanosleep. Both must really sleep; use high-resolution waits (Windows default granularity is 15 ms).

### pthread, semaphore, scheduler (38)

- `pthread_attr_destroy` - 2 refs; glwebtools 1, glf 1 - detach state PTHREAD_CREATE_DETACHED=1; stack sizes requested are honoured for guest stacks (allocate guest stack of that size).
- `pthread_attr_init` - 2 refs; glwebtools 1, glf 1 - detach state PTHREAD_CREATE_DETACHED=1; stack sizes requested are honoured for guest stacks (allocate guest stack of that size).
- `pthread_attr_setdetachstate` - 1 refs; glf 1 - detach state PTHREAD_CREATE_DETACHED=1; stack sizes requested are honoured for guest stacks (allocate guest stack of that size).
- `pthread_attr_setstacksize` - 2 refs; glwebtools 1, glf 1 - detach state PTHREAD_CREATE_DETACHED=1; stack sizes requested are honoured for guest stacks (allocate guest stack of that size).
- `pthread_cond_broadcast` [also libgenerator] - 22 refs; asio 6, libc++ 6, glf 3 - bionic LP64 sizes: pthread_mutex_t 40 bytes, pthread_cond_t 48, pthread_mutexattr_t 4, pthread_attr_t 64, pthread_rwlock_t 56 (header values, not re-measured here). PTHREAD_MUTEX_INITIALIZER is all-zero and libc++ std::mutex/condition_variable embed these structs directly and use them without calling _init, so the host must lazily bind a host object to the guest ADDRESS (side table keyed by address, or store a handle in the first 8 bytes). pthread_cond_timedwait takes an ABSOLUTE CLOCK_REALTIME timespec.
- `pthread_cond_destroy` [also libgenerator] - 12 refs; glf 5, AnubisLib 2, asio 2 - bionic LP64 sizes: pthread_mutex_t 40 bytes, pthread_cond_t 48, pthread_mutexattr_t 4, pthread_attr_t 64, pthread_rwlock_t 56 (header values, not re-measured here). PTHREAD_MUTEX_INITIALIZER is all-zero and libc++ std::mutex/condition_variable embed these structs directly and use them without calling _init, so the host must lazily bind a host object to the guest ADDRESS (side table keyed by address, or store a handle in the first 8 bytes). pthread_cond_timedwait takes an ABSOLUTE CLOCK_REALTIME timespec.
- `pthread_cond_init` - 10 refs; glf 5, AnubisLib 2, unexported-code(OpenSSL/curl) 2 - bionic LP64 sizes: pthread_mutex_t 40 bytes, pthread_cond_t 48, pthread_mutexattr_t 4, pthread_attr_t 64, pthread_rwlock_t 56 (header values, not re-measured here). PTHREAD_MUTEX_INITIALIZER is all-zero and libc++ std::mutex/condition_variable embed these structs directly and use them without calling _init, so the host must lazily bind a host object to the guest ADDRESS (side table keyed by address, or store a handle in the first 8 bytes). pthread_cond_timedwait takes an ABSOLUTE CLOCK_REALTIME timespec.
- `pthread_cond_signal` [also libgenerator] - 13 refs; glf 4, unexported-code(OpenSSL/curl) 4, AnubisLib 2 - bionic LP64 sizes: pthread_mutex_t 40 bytes, pthread_cond_t 48, pthread_mutexattr_t 4, pthread_attr_t 64, pthread_rwlock_t 56 (header values, not re-measured here). PTHREAD_MUTEX_INITIALIZER is all-zero and libc++ std::mutex/condition_variable embed these structs directly and use them without calling _init, so the host must lazily bind a host object to the guest ADDRESS (side table keyed by address, or store a handle in the first 8 bytes). pthread_cond_timedwait takes an ABSOLUTE CLOCK_REALTIME timespec.
- `pthread_cond_timedwait` [also libgenerator] - 3 refs; glf 2, libc++ 1 - bionic LP64 sizes: pthread_mutex_t 40 bytes, pthread_cond_t 48, pthread_mutexattr_t 4, pthread_attr_t 64, pthread_rwlock_t 56 (header values, not re-measured here). PTHREAD_MUTEX_INITIALIZER is all-zero and libc++ std::mutex/condition_variable embed these structs directly and use them without calling _init, so the host must lazily bind a host object to the guest ADDRESS (side table keyed by address, or store a handle in the first 8 bytes). pthread_cond_timedwait takes an ABSOLUTE CLOCK_REALTIME timespec.
- `pthread_cond_wait` [also libgenerator] - 11 refs; glf 3, AnubisLib 2, unexported-code(OpenSSL/curl) 2 - bionic LP64 sizes: pthread_mutex_t 40 bytes, pthread_cond_t 48, pthread_mutexattr_t 4, pthread_attr_t 64, pthread_rwlock_t 56 (header values, not re-measured here). PTHREAD_MUTEX_INITIALIZER is all-zero and libc++ std::mutex/condition_variable embed these structs directly and use them without calling _init, so the host must lazily bind a host object to the guest ADDRESS (side table keyed by address, or store a handle in the first 8 bytes). pthread_cond_timedwait takes an ABSOLUTE CLOCK_REALTIME timespec.
- `pthread_create` [also libgenerator] - 114 refs; C/unexported-code 82, libc++ 14, unexported-code(OpenSSL/curl) 11 - (pthread_t*,attr,start_routine,arg). pthread_t is a 64-bit pointer-sized value. Host must start a host thread that enters the JIT at start_routine with a fresh guest stack and fresh TCB (TPIDR_EL0 + canary). 21 call sites: std::thread, glf::Thread::Impl::Start, vox::VoxThread, glwebtools::Thread, asio, gaia, OnGameInit() (the game main thread, AndroidMain).
- `pthread_detach` [also libgenerator] - 3 refs; asio 2, libc++ 1 - pthread_self returns the same value as the guest thread pointer stored in the TCB.
- `pthread_equal` [also libgenerator] - 3 refs; libc++ 2, glf 1 - pthread_self returns the same value as the guest thread pointer stored in the TCB.
- `pthread_exit` - 1 refs; C/unexported-code 1 - used by AndroidMain() after glf::App::Impl::Run() returns; must unwind the guest thread.
- `pthread_getschedparam` - 1 refs; glotv3 1 - struct sched_param is one int; SCHED_OTHER 0, SCHED_FIFO 1, SCHED_RR 2. glf::Thread::Impl::SetPriority maps to these (and to Java Process.setThreadPriority via JNI "setThreadPriority(I)V").
- `pthread_getspecific` [also libgenerator] - 41 refs; asio 34, libc++ 3, C/unexported-code 2 - pthread_once_t and pthread_key_t are 32-bit ints; PTHREAD_ONCE_INIT is 0; init_routine and key destructor are guest function pointers. Keys are used by libc++/exception globals (__cxa_get_globals) so per-guest-thread storage is required.
- `pthread_join` [also libgenerator] - 9 refs; vox 2, asio 2, unexported-code(OpenSSL/curl) 2 - pthread_self returns the same value as the guest thread pointer stored in the TCB.
- `pthread_key_create` [also libgenerator] - 6 refs; C/unexported-code 3, glf 1, asio 1 - pthread_once_t and pthread_key_t are 32-bit ints; PTHREAD_ONCE_INIT is 0; init_routine and key destructor are guest function pointers. Keys are used by libc++/exception globals (__cxa_get_globals) so per-guest-thread storage is required.
- `pthread_key_delete` - 5 refs; asio 3, glf 2 - pthread_once_t and pthread_key_t are 32-bit ints; PTHREAD_ONCE_INIT is 0; init_routine and key destructor are guest function pointers. Keys are used by libc++/exception globals (__cxa_get_globals) so per-guest-thread storage is required.
- `pthread_mutex_destroy` [also libgenerator] - 52 refs; vox 18, asio 13, libc++ 9 - bionic LP64 sizes: pthread_mutex_t 40 bytes, pthread_cond_t 48, pthread_mutexattr_t 4, pthread_attr_t 64, pthread_rwlock_t 56 (header values, not re-measured here). PTHREAD_MUTEX_INITIALIZER is all-zero and libc++ std::mutex/condition_variable embed these structs directly and use them without calling _init, so the host must lazily bind a host object to the guest ADDRESS (side table keyed by address, or store a handle in the first 8 bytes). pthread_cond_timedwait takes an ABSOLUTE CLOCK_REALTIME timespec.
- `pthread_mutex_init` [also libgenerator] - 47 refs; vox 22, asio 9, glf 6 - bionic LP64 sizes: pthread_mutex_t 40 bytes, pthread_cond_t 48, pthread_mutexattr_t 4, pthread_attr_t 64, pthread_rwlock_t 56 (header values, not re-measured here). PTHREAD_MUTEX_INITIALIZER is all-zero and libc++ std::mutex/condition_variable embed these structs directly and use them without calling _init, so the host must lazily bind a host object to the guest ADDRESS (side table keyed by address, or store a handle in the first 8 bytes). pthread_cond_timedwait takes an ABSOLUTE CLOCK_REALTIME timespec.
- `pthread_mutex_lock` [also libgenerator] - 479 refs; vox 324, asio 72, glf 26 - bionic LP64 sizes: pthread_mutex_t 40 bytes, pthread_cond_t 48, pthread_mutexattr_t 4, pthread_attr_t 64, pthread_rwlock_t 56 (header values, not re-measured here). PTHREAD_MUTEX_INITIALIZER is all-zero and libc++ std::mutex/condition_variable embed these structs directly and use them without calling _init, so the host must lazily bind a host object to the guest ADDRESS (side table keyed by address, or store a handle in the first 8 bytes). pthread_cond_timedwait takes an ABSOLUTE CLOCK_REALTIME timespec.
- `pthread_mutex_trylock` [also libgenerator] - 6 refs; libc++ 4, vox 1, glf 1 - bionic LP64 sizes: pthread_mutex_t 40 bytes, pthread_cond_t 48, pthread_mutexattr_t 4, pthread_attr_t 64, pthread_rwlock_t 56 (header values, not re-measured here). PTHREAD_MUTEX_INITIALIZER is all-zero and libc++ std::mutex/condition_variable embed these structs directly and use them without calling _init, so the host must lazily bind a host object to the guest ADDRESS (side table keyed by address, or store a handle in the first 8 bytes). pthread_cond_timedwait takes an ABSOLUTE CLOCK_REALTIME timespec.
- `pthread_mutex_unlock` [also libgenerator] - 550 refs; vox 326, asio 133, glf 25 - bionic LP64 sizes: pthread_mutex_t 40 bytes, pthread_cond_t 48, pthread_mutexattr_t 4, pthread_attr_t 64, pthread_rwlock_t 56 (header values, not re-measured here). PTHREAD_MUTEX_INITIALIZER is all-zero and libc++ std::mutex/condition_variable embed these structs directly and use them without calling _init, so the host must lazily bind a host object to the guest ADDRESS (side table keyed by address, or store a handle in the first 8 bytes). pthread_cond_timedwait takes an ABSOLUTE CLOCK_REALTIME timespec.
- `pthread_mutexattr_destroy` [also libgenerator] - 3 refs; libc++ 2, glf 1
- `pthread_mutexattr_init` [also libgenerator] - 2 refs; glf 1, libc++ 1
- `pthread_mutexattr_settype` [also libgenerator] - 2 refs; glf 1, libc++ 1 - type values 0 NORMAL, 1 RECURSIVE, 2 ERRORCHECK (Linux numbering).
- `pthread_once` [also libgenerator] - 5 refs; C/unexported-code 5 - pthread_once_t and pthread_key_t are 32-bit ints; PTHREAD_ONCE_INIT is 0; init_routine and key destructor are guest function pointers. Keys are used by libc++/exception globals (__cxa_get_globals) so per-guest-thread storage is required.
- `pthread_self` [also libgenerator] - 30 refs; glf 21, unexported-code(glitch/gameswf/static libs) 5, libc++ 3 - pthread_self returns the same value as the guest thread pointer stored in the TCB.
- `pthread_setschedparam` - 1 refs; glotv3 1 - struct sched_param is one int; SCHED_OTHER 0, SCHED_FIFO 1, SCHED_RR 2. glf::Thread::Impl::SetPriority maps to these (and to Java Process.setThreadPriority via JNI "setThreadPriority(I)V").
- `pthread_setspecific` [also libgenerator] - 65 refs; asio 58, glf 4, C/unexported-code 2 - pthread_once_t and pthread_key_t are 32-bit ints; PTHREAD_ONCE_INIT is 0; init_routine and key destructor are guest function pointers. Keys are used by libc++/exception globals (__cxa_get_globals) so per-guest-thread storage is required.
- `sched_get_priority_min` - 1 refs; glotv3 1 - struct sched_param is one int; SCHED_OTHER 0, SCHED_FIFO 1, SCHED_RR 2. glf::Thread::Impl::SetPriority maps to these (and to Java Process.setThreadPriority via JNI "setThreadPriority(I)V").
- `sched_yield` [also libgenerator] - 15 refs; glf 14, libc++ 1 - struct sched_param is one int; SCHED_OTHER 0, SCHED_FIFO 1, SCHED_RR 2. glf::Thread::Impl::SetPriority maps to these (and to Java Process.setThreadPriority via JNI "setThreadPriority(I)V").
- `sem_destroy` - 3 refs; unexported-code(OpenSSL/curl) 2, glf 1 - sem_t is 32 bytes on bionic LP64; same address-keyed lazy binding as mutexes.
- `sem_init` - 3 refs; unexported-code(OpenSSL/curl) 2, glf 1 - sem_t is 32 bytes on bionic LP64; same address-keyed lazy binding as mutexes.
- `sem_post` - 6 refs; unexported-code(OpenSSL/curl) 5, glf 1 - sem_t is 32 bytes on bionic LP64; same address-keyed lazy binding as mutexes.
- `sem_trywait` - 1 refs; glf 1 - sem_t is 32 bytes on bionic LP64; same address-keyed lazy binding as mutexes.
- `sem_wait` - 5 refs; unexported-code(OpenSSL/curl) 4, glf 1 - sem_t is 32 bytes on bionic LP64; same address-keyed lazy binding as mutexes.

### Files, directories, descriptors (26)

- `access` - 2 refs; C/unexported-code 1, PopUpsLib 1 - POSIX semantics, Linux flag numbers; mkdir mode argument ignored on Windows. lseek/pread use 64-bit off_t. access modes F_OK 0, R_OK 4, W_OK 2.
- `chdir` - 2 refs; glf 2 - POSIX semantics, Linux flag numbers; mkdir mode argument ignored on Windows. lseek/pread use 64-bit off_t. access modes F_OK 0, R_OK 4, W_OK 2.
- `chmod` - 1 refs; C/unexported-code 1 - POSIX semantics, Linux flag numbers; mkdir mode argument ignored on Windows. lseek/pread use 64-bit off_t. access modes F_OK 0, R_OK 4, W_OK 2.
- `close` [also libgenerator] - 53 refs; asio 10, olutils 9, glf 9 - POSIX semantics, Linux flag numbers; mkdir mode argument ignored on Windows. lseek/pread use 64-bit off_t. access modes F_OK 0, R_OK 4, W_OK 2.
- `closedir` - 12 refs; glf 5, acp_utils 2, olutils 1 - DIR* is opaque; struct dirent is the 64-bit kernel layout: d_ino u64@0, d_off i64@8, d_reclen u16@16, d_type u8@18, d_name[256]@19 (280 bytes). d_type DT_DIR 4, DT_REG 8. Returns "." and ".." entries; Windows FindFirstFile does not.
- `fcntl` - 12 refs; asio 7, glf 2, automation 2 - variadic. Seen: F_GETFL 3, F_SETFL 4, F_GETFD 1, F_SETFD 2 with O_NONBLOCK 0x800 / FD_CLOEXEC on sockets and epoll fds; Windows needs ioctlsocket(FIONBIO).
- `fstat` - 5 refs; C/unexported-code 3, glf 1, unexported-code(OpenSSL/curl) 1 - struct stat is the asm-generic arm64 layout, 128 bytes: st_dev u64@0, st_ino u64@8, st_mode u32@16, st_nlink u32@20, st_uid@24, st_gid@28, st_rdev u64@32, pad@40, st_size i64@48, st_blksize i32@56, pad, st_blocks i64@64, st_atim@72, st_mtim@88, st_ctim@104 (timespec 16 bytes each). This is the standard asm-generic arm64 layout (offsets recorded from the kernel/bionic header, not re-measured in the binary). Windows struct stat differs completely; host must marshal. Mode bits are POSIX (S_IFDIR 0040000, S_IFREG 0100000).
- `getcwd` - 2 refs; glf 2 - POSIX semantics, Linux flag numbers; mkdir mode argument ignored on Windows. lseek/pread use 64-bit off_t. access modes F_OK 0, R_OK 4, W_OK 2.
- `ioctl` - 6 refs; glwebtools 3, asio 2, gameoptions 1 - variadic. Requests seen (Linux values): 0x5421 FIONBIO (asio), 0x8912 SIOCGIFCONF and 0x8915 SIOCGIFADDR (glwebtools::Socket::GetNetInterfaceInfoArray, gameoptions::TCPSocketConnection::GetIpData); struct ifconf/ifreq are Linux layouts (ifreq 40 bytes). Host: map to GetAdaptersAddresses.
- `lseek` - 4 refs; glf 2, olutils 1, C/unexported-code 1 - POSIX semantics, Linux flag numbers; mkdir mode argument ignored on Windows. lseek/pread use 64-bit off_t. access modes F_OK 0, R_OK 4, W_OK 2.
- `mkdir` - 11 refs; glf 2, DlcManager 1, SubstanceProcessor 1 - POSIX semantics, Linux flag numbers; mkdir mode argument ignored on Windows. lseek/pread use 64-bit off_t. access modes F_OK 0, R_OK 4, W_OK 2.
- `mkstemp` - 1 refs; C/unexported-code 1 - POSIX semantics, Linux flag numbers; mkdir mode argument ignored on Windows. lseek/pread use 64-bit off_t. access modes F_OK 0, R_OK 4, W_OK 2.
- `open` [also libgenerator] - 8 refs; C/unexported-code 4, olutils 1, glf 1 - variadic third arg (mode). Flag numbers are Linux arm64: O_RDONLY 0, O_WRONLY 1, O_RDWR 2, O_CREAT 0x40, O_EXCL 0x80, O_TRUNC 0x200, O_APPEND 0x400, O_NONBLOCK 0x800, O_DIRECTORY 0x4000, O_CLOEXEC 0x80000. Used for /dev/urandom (libgenerator, libc++ random_device) and regular files.
- `opendir` - 9 refs; acp_utils 2, glf 2, olutils 1 - DIR* is opaque; struct dirent is the 64-bit kernel layout: d_ino u64@0, d_off i64@8, d_reclen u16@16, d_type u8@18, d_name[256]@19 (280 bytes). d_type DT_DIR 4, DT_REG 8. Returns "." and ".." entries; Windows FindFirstFile does not.
- `pipe` - 1 refs; asio 1 - asio interrupter (eventfd with the usual EFD_NONBLOCK 0x800 / EFD_CLOEXEC 0x80000 flags; flag values not decoded at the call). Host: socketpair emulation.
- `pread` - 1 refs; glf 1 - POSIX semantics, Linux flag numbers; mkdir mode argument ignored on Windows. lseek/pread use 64-bit off_t. access modes F_OK 0, R_OK 4, W_OK 2.
- `read` [also libgenerator] - 8 refs; unexported-code(OpenSSL/curl) 4, olutils 1, glf 1 - POSIX semantics, Linux flag numbers; mkdir mode argument ignored on Windows. lseek/pread use 64-bit off_t. access modes F_OK 0, R_OK 4, W_OK 2.
- `readdir` - 15 refs; acp_utils 3, glf 3, olutils 2 - DIR* is opaque; struct dirent is the 64-bit kernel layout: d_ino u64@0, d_off i64@8, d_reclen u16@16, d_type u8@18, d_name[256]@19 (280 bytes). d_type DT_DIR 4, DT_REG 8. Returns "." and ".." entries; Windows FindFirstFile does not.
- `realpath` - 1 refs; automation 1 - POSIX semantics, Linux flag numbers; mkdir mode argument ignored on Windows. lseek/pread use 64-bit off_t. access modes F_OK 0, R_OK 4, W_OK 2.
- `rmdir` - 3 refs; glf 2, olutils 1 - POSIX semantics, Linux flag numbers; mkdir mode argument ignored on Windows. lseek/pread use 64-bit off_t. access modes F_OK 0, R_OK 4, W_OK 2.
- `sendfile` - 1 refs; glf 1 - POSIX semantics, Linux flag numbers; mkdir mode argument ignored on Windows. lseek/pread use 64-bit off_t. access modes F_OK 0, R_OK 4, W_OK 2.
- `stat` - 21 refs; C/unexported-code 4, glf 4, glotv3 3 - struct stat is the asm-generic arm64 layout, 128 bytes: st_dev u64@0, st_ino u64@8, st_mode u32@16, st_nlink u32@20, st_uid@24, st_gid@28, st_rdev u64@32, pad@40, st_size i64@48, st_blksize i32@56, pad, st_blocks i64@64, st_atim@72, st_mtim@88, st_ctim@104 (timespec 16 bytes each). This is the standard asm-generic arm64 layout (offsets recorded from the kernel/bionic header, not re-measured in the binary). Windows struct stat differs completely; host must marshal. Mode bits are POSIX (S_IFDIR 0040000, S_IFREG 0100000).
- `statfs` - 3 refs; olutils 1, glotv3 1, automation 1 - bionic arm64 struct statfs (f_type,f_bsize,f_blocks,f_bfree,f_bavail,f_files,f_ffree,f_fsid,f_namelen,f_frsize,f_flags,spare = 120 bytes), 64-bit fields. Used for free-space checks.
- `umask` - 2 refs; C/unexported-code 2 - POSIX semantics, Linux flag numbers; mkdir mode argument ignored on Windows. lseek/pread use 64-bit off_t. access modes F_OK 0, R_OK 4, W_OK 2.
- `unlink` - 4 refs; glf 4 - POSIX semantics, Linux flag numbers; mkdir mode argument ignored on Windows. lseek/pread use 64-bit off_t. access modes F_OK 0, R_OK 4, W_OK 2.
- `write` - 8 refs; unexported-code(OpenSSL/curl) 3, asio 2, olutils 1 - POSIX semantics, Linux flag numbers; mkdir mode argument ignored on Windows. lseek/pread use 64-bit off_t. access modes F_OK 0, R_OK 4, W_OK 2.

### Sockets, epoll, name resolution, terminal (32)

- `accept` - 6 refs; glf 2, asio 2, gameoptions 1 - Linux numbering throughout: AF_INET 2, AF_INET6 10 (Windows 23), SOCK_STREAM 1, SOCK_DGRAM 2, SOL_SOCKET 1 (Windows 0xffff), SO_REUSEADDR 2, SO_RCVBUF 8, SO_ERROR 4, MSG_NOSIGNAL 0x4000, MSG_DONTWAIT 0x40. struct sockaddr has NO sa_len; sockaddr_in6 family value differs. fd_set is a 1024-bit bitmap indexed by fd (Windows fd_set is an array of SOCKET handles); struct pollfd {int fd; short events; short revents}, POLLIN 1 POLLOUT 4 POLLERR 8 POLLHUP 0x10 (Winsock WSAPoll values differ). Socket fds must be small ints, not SOCKET handles. Users: asio, curl, glf::Socket, glwebtools::Socket, Comms (LAN multiplayer), gameoptions::TCPSocketConnection, automation::TcpServer.
- `bind` - 11 refs; Comms 3, glf 3, C/unexported-code 2 - Linux numbering throughout: AF_INET 2, AF_INET6 10 (Windows 23), SOCK_STREAM 1, SOCK_DGRAM 2, SOL_SOCKET 1 (Windows 0xffff), SO_REUSEADDR 2, SO_RCVBUF 8, SO_ERROR 4, MSG_NOSIGNAL 0x4000, MSG_DONTWAIT 0x40. struct sockaddr has NO sa_len; sockaddr_in6 family value differs. fd_set is a 1024-bit bitmap indexed by fd (Windows fd_set is an array of SOCKET handles); struct pollfd {int fd; short events; short revents}, POLLIN 1 POLLOUT 4 POLLERR 8 POLLHUP 0x10 (Winsock WSAPoll values differ). Socket fds must be small ints, not SOCKET handles. Users: asio, curl, glf::Socket, glwebtools::Socket, Comms (LAN multiplayer), gameoptions::TCPSocketConnection, automation::TcpServer.
- `connect` - 7 refs; unexported-code(OpenSSL/curl) 2, AnubisLib 1, glf 1 - Linux numbering throughout: AF_INET 2, AF_INET6 10 (Windows 23), SOCK_STREAM 1, SOCK_DGRAM 2, SOL_SOCKET 1 (Windows 0xffff), SO_REUSEADDR 2, SO_RCVBUF 8, SO_ERROR 4, MSG_NOSIGNAL 0x4000, MSG_DONTWAIT 0x40. struct sockaddr has NO sa_len; sockaddr_in6 family value differs. fd_set is a 1024-bit bitmap indexed by fd (Windows fd_set is an array of SOCKET handles); struct pollfd {int fd; short events; short revents}, POLLIN 1 POLLOUT 4 POLLERR 8 POLLHUP 0x10 (Winsock WSAPoll values differ). Socket fds must be small ints, not SOCKET handles. Users: asio, curl, glf::Socket, glwebtools::Socket, Comms (LAN multiplayer), gameoptions::TCPSocketConnection, automation::TcpServer.
- `epoll_create` - 1 refs; asio 1 - asio epoll_reactor. struct epoll_event on aarch64 is 16 bytes {u32 events; 4 pad; u64 data} (NOT packed like x86-64 where it is 12 bytes). EPOLLIN 1, EPOLLOUT 4, EPOLLERR 8, EPOLLHUP 0x10, EPOLLET 0x80000000. Host: emulate with WSAPoll/IOCP over Winsock.
- `epoll_create1` - 1 refs; asio 1 - asio epoll_reactor. struct epoll_event on aarch64 is 16 bytes {u32 events; 4 pad; u64 data} (NOT packed like x86-64 where it is 12 bytes). EPOLLIN 1, EPOLLOUT 4, EPOLLERR 8, EPOLLHUP 0x10, EPOLLET 0x80000000. Host: emulate with WSAPoll/IOCP over Winsock.
- `epoll_ctl` - 31 refs; asio 23, glotv3 3, libc++ 3 - asio epoll_reactor. struct epoll_event on aarch64 is 16 bytes {u32 events; 4 pad; u64 data} (NOT packed like x86-64 where it is 12 bytes). EPOLLIN 1, EPOLLOUT 4, EPOLLERR 8, EPOLLHUP 0x10, EPOLLET 0x80000000. Host: emulate with WSAPoll/IOCP over Winsock.
- `epoll_wait` - 1 refs; asio 1 - asio epoll_reactor. struct epoll_event on aarch64 is 16 bytes {u32 events; 4 pad; u64 data} (NOT packed like x86-64 where it is 12 bytes). EPOLLIN 1, EPOLLOUT 4, EPOLLERR 8, EPOLLHUP 0x10, EPOLLET 0x80000000. Host: emulate with WSAPoll/IOCP over Winsock.
- `eventfd` - 2 refs; asio 2 - asio interrupter (eventfd with the usual EFD_NONBLOCK 0x800 / EFD_CLOEXEC 0x80000 flags; flag values not decoded at the call). Host: socketpair emulation.
- `freeaddrinfo` - 14 refs; Comms 4, asio 4, automation 4 - struct addrinfo bionic order: ai_flags, ai_family, ai_socktype, ai_protocol (ints), ai_addrlen (socklen_t, 4 bytes + pad), ai_canonname (char*), ai_addr (sockaddr*), ai_next (Windows has addrlen as size_t). Results live in guest memory and are freed by guest calling freeaddrinfo.
- `getaddrinfo` - 8 refs; asio 4, Comms 2, AnubisLib 1 - struct addrinfo bionic order: ai_flags, ai_family, ai_socktype, ai_protocol (ints), ai_addrlen (socklen_t, 4 bytes + pad), ai_canonname (char*), ai_addr (sockaddr*), ai_next (Windows has addrlen as size_t). Results live in guest memory and are freed by guest calling freeaddrinfo.
- `gethostbyname` - 3 refs; Comms 1, gameoptions 1, C/unexported-code 1 - returns pointer to a static struct hostent in guest memory (bionic LP64 layout h_name,h_aliases,h_addrtype,h_length,h_addr_list).
- `getpeername` - 1 refs; C/unexported-code 1 - Linux numbering throughout: AF_INET 2, AF_INET6 10 (Windows 23), SOCK_STREAM 1, SOCK_DGRAM 2, SOL_SOCKET 1 (Windows 0xffff), SO_REUSEADDR 2, SO_RCVBUF 8, SO_ERROR 4, MSG_NOSIGNAL 0x4000, MSG_DONTWAIT 0x40. struct sockaddr has NO sa_len; sockaddr_in6 family value differs. fd_set is a 1024-bit bitmap indexed by fd (Windows fd_set is an array of SOCKET handles); struct pollfd {int fd; short events; short revents}, POLLIN 1 POLLOUT 4 POLLERR 8 POLLHUP 0x10 (Winsock WSAPoll values differ). Socket fds must be small ints, not SOCKET handles. Users: asio, curl, glf::Socket, glwebtools::Socket, Comms (LAN multiplayer), gameoptions::TCPSocketConnection, automation::TcpServer.
- `getsockname` - 4 refs; C/unexported-code 2, Comms 1, glf 1 - Linux numbering throughout: AF_INET 2, AF_INET6 10 (Windows 23), SOCK_STREAM 1, SOCK_DGRAM 2, SOL_SOCKET 1 (Windows 0xffff), SO_REUSEADDR 2, SO_RCVBUF 8, SO_ERROR 4, MSG_NOSIGNAL 0x4000, MSG_DONTWAIT 0x40. struct sockaddr has NO sa_len; sockaddr_in6 family value differs. fd_set is a 1024-bit bitmap indexed by fd (Windows fd_set is an array of SOCKET handles); struct pollfd {int fd; short events; short revents}, POLLIN 1 POLLOUT 4 POLLERR 8 POLLHUP 0x10 (Winsock WSAPoll values differ). Socket fds must be small ints, not SOCKET handles. Users: asio, curl, glf::Socket, glwebtools::Socket, Comms (LAN multiplayer), gameoptions::TCPSocketConnection, automation::TcpServer.
- `getsockopt` - 2 refs; asio 1, C/unexported-code 1 - Linux numbering throughout: AF_INET 2, AF_INET6 10 (Windows 23), SOCK_STREAM 1, SOCK_DGRAM 2, SOL_SOCKET 1 (Windows 0xffff), SO_REUSEADDR 2, SO_RCVBUF 8, SO_ERROR 4, MSG_NOSIGNAL 0x4000, MSG_DONTWAIT 0x40. struct sockaddr has NO sa_len; sockaddr_in6 family value differs. fd_set is a 1024-bit bitmap indexed by fd (Windows fd_set is an array of SOCKET handles); struct pollfd {int fd; short events; short revents}, POLLIN 1 POLLOUT 4 POLLERR 8 POLLHUP 0x10 (Winsock WSAPoll values differ). Socket fds must be small ints, not SOCKET handles. Users: asio, curl, glf::Socket, glwebtools::Socket, Comms (LAN multiplayer), gameoptions::TCPSocketConnection, automation::TcpServer.
- `if_indextoname` - 1 refs; asio 1 - inet_ntop address-family argument is the Linux AF value (AF_INET6=10).
- `inet_addr` - 3 refs; glf 2, Comms 1 - inet_ntop address-family argument is the Linux AF value (AF_INET6=10).
- `inet_ntoa` - 2 refs; glf 1, gameoptions 1 - inet_ntop address-family argument is the Linux AF value (AF_INET6=10).
- `inet_ntop` - 1 refs; asio 1 - inet_ntop address-family argument is the Linux AF value (AF_INET6=10).
- `listen` - 6 refs; glf 3, asio 1, gameoptions 1 - Linux numbering throughout: AF_INET 2, AF_INET6 10 (Windows 23), SOCK_STREAM 1, SOCK_DGRAM 2, SOL_SOCKET 1 (Windows 0xffff), SO_REUSEADDR 2, SO_RCVBUF 8, SO_ERROR 4, MSG_NOSIGNAL 0x4000, MSG_DONTWAIT 0x40. struct sockaddr has NO sa_len; sockaddr_in6 family value differs. fd_set is a 1024-bit bitmap indexed by fd (Windows fd_set is an array of SOCKET handles); struct pollfd {int fd; short events; short revents}, POLLIN 1 POLLOUT 4 POLLERR 8 POLLHUP 0x10 (Winsock WSAPoll values differ). Socket fds must be small ints, not SOCKET handles. Users: asio, curl, glf::Socket, glwebtools::Socket, Comms (LAN multiplayer), gameoptions::TCPSocketConnection, automation::TcpServer.
- `poll` - 4 refs; asio 3, unexported-code(OpenSSL/curl) 1 - Linux numbering throughout: AF_INET 2, AF_INET6 10 (Windows 23), SOCK_STREAM 1, SOCK_DGRAM 2, SOL_SOCKET 1 (Windows 0xffff), SO_REUSEADDR 2, SO_RCVBUF 8, SO_ERROR 4, MSG_NOSIGNAL 0x4000, MSG_DONTWAIT 0x40. struct sockaddr has NO sa_len; sockaddr_in6 family value differs. fd_set is a 1024-bit bitmap indexed by fd (Windows fd_set is an array of SOCKET handles); struct pollfd {int fd; short events; short revents}, POLLIN 1 POLLOUT 4 POLLERR 8 POLLHUP 0x10 (Winsock WSAPoll values differ). Socket fds must be small ints, not SOCKET handles. Users: asio, curl, glf::Socket, glwebtools::Socket, Comms (LAN multiplayer), gameoptions::TCPSocketConnection, automation::TcpServer.
- `recv` - 10 refs; C/unexported-code 4, gameoptions 3, AnubisLib 1 - Linux numbering throughout: AF_INET 2, AF_INET6 10 (Windows 23), SOCK_STREAM 1, SOCK_DGRAM 2, SOL_SOCKET 1 (Windows 0xffff), SO_REUSEADDR 2, SO_RCVBUF 8, SO_ERROR 4, MSG_NOSIGNAL 0x4000, MSG_DONTWAIT 0x40. struct sockaddr has NO sa_len; sockaddr_in6 family value differs. fd_set is a 1024-bit bitmap indexed by fd (Windows fd_set is an array of SOCKET handles); struct pollfd {int fd; short events; short revents}, POLLIN 1 POLLOUT 4 POLLERR 8 POLLHUP 0x10 (Winsock WSAPoll values differ). Socket fds must be small ints, not SOCKET handles. Users: asio, curl, glf::Socket, glwebtools::Socket, Comms (LAN multiplayer), gameoptions::TCPSocketConnection, automation::TcpServer.
- `recvfrom` - 2 refs; Comms 1, glf 1 - Linux numbering throughout: AF_INET 2, AF_INET6 10 (Windows 23), SOCK_STREAM 1, SOCK_DGRAM 2, SOL_SOCKET 1 (Windows 0xffff), SO_REUSEADDR 2, SO_RCVBUF 8, SO_ERROR 4, MSG_NOSIGNAL 0x4000, MSG_DONTWAIT 0x40. struct sockaddr has NO sa_len; sockaddr_in6 family value differs. fd_set is a 1024-bit bitmap indexed by fd (Windows fd_set is an array of SOCKET handles); struct pollfd {int fd; short events; short revents}, POLLIN 1 POLLOUT 4 POLLERR 8 POLLHUP 0x10 (Winsock WSAPoll values differ). Socket fds must be small ints, not SOCKET handles. Users: asio, curl, glf::Socket, glwebtools::Socket, Comms (LAN multiplayer), gameoptions::TCPSocketConnection, automation::TcpServer.
- `recvmsg` - 2 refs; asio 2 - Linux numbering throughout: AF_INET 2, AF_INET6 10 (Windows 23), SOCK_STREAM 1, SOCK_DGRAM 2, SOL_SOCKET 1 (Windows 0xffff), SO_REUSEADDR 2, SO_RCVBUF 8, SO_ERROR 4, MSG_NOSIGNAL 0x4000, MSG_DONTWAIT 0x40. struct sockaddr has NO sa_len; sockaddr_in6 family value differs. fd_set is a 1024-bit bitmap indexed by fd (Windows fd_set is an array of SOCKET handles); struct pollfd {int fd; short events; short revents}, POLLIN 1 POLLOUT 4 POLLERR 8 POLLHUP 0x10 (Winsock WSAPoll values differ). Socket fds must be small ints, not SOCKET handles. Users: asio, curl, glf::Socket, glwebtools::Socket, Comms (LAN multiplayer), gameoptions::TCPSocketConnection, automation::TcpServer.
- `select` - 15 refs; Comms 4, AnubisLib 3, C/unexported-code 3 - Linux numbering throughout: AF_INET 2, AF_INET6 10 (Windows 23), SOCK_STREAM 1, SOCK_DGRAM 2, SOL_SOCKET 1 (Windows 0xffff), SO_REUSEADDR 2, SO_RCVBUF 8, SO_ERROR 4, MSG_NOSIGNAL 0x4000, MSG_DONTWAIT 0x40. struct sockaddr has NO sa_len; sockaddr_in6 family value differs. fd_set is a 1024-bit bitmap indexed by fd (Windows fd_set is an array of SOCKET handles); struct pollfd {int fd; short events; short revents}, POLLIN 1 POLLOUT 4 POLLERR 8 POLLHUP 0x10 (Winsock WSAPoll values differ). Socket fds must be small ints, not SOCKET handles. Users: asio, curl, glf::Socket, glwebtools::Socket, Comms (LAN multiplayer), gameoptions::TCPSocketConnection, automation::TcpServer.
- `send` - 23 refs; gameoptions 15, glf 5, AnubisLib 1 - Linux numbering throughout: AF_INET 2, AF_INET6 10 (Windows 23), SOCK_STREAM 1, SOCK_DGRAM 2, SOL_SOCKET 1 (Windows 0xffff), SO_REUSEADDR 2, SO_RCVBUF 8, SO_ERROR 4, MSG_NOSIGNAL 0x4000, MSG_DONTWAIT 0x40. struct sockaddr has NO sa_len; sockaddr_in6 family value differs. fd_set is a 1024-bit bitmap indexed by fd (Windows fd_set is an array of SOCKET handles); struct pollfd {int fd; short events; short revents}, POLLIN 1 POLLOUT 4 POLLERR 8 POLLHUP 0x10 (Winsock WSAPoll values differ). Socket fds must be small ints, not SOCKET handles. Users: asio, curl, glf::Socket, glwebtools::Socket, Comms (LAN multiplayer), gameoptions::TCPSocketConnection, automation::TcpServer.
- `sendmsg` - 2 refs; asio 2 - Linux numbering throughout: AF_INET 2, AF_INET6 10 (Windows 23), SOCK_STREAM 1, SOCK_DGRAM 2, SOL_SOCKET 1 (Windows 0xffff), SO_REUSEADDR 2, SO_RCVBUF 8, SO_ERROR 4, MSG_NOSIGNAL 0x4000, MSG_DONTWAIT 0x40. struct sockaddr has NO sa_len; sockaddr_in6 family value differs. fd_set is a 1024-bit bitmap indexed by fd (Windows fd_set is an array of SOCKET handles); struct pollfd {int fd; short events; short revents}, POLLIN 1 POLLOUT 4 POLLERR 8 POLLHUP 0x10 (Winsock WSAPoll values differ). Socket fds must be small ints, not SOCKET handles. Users: asio, curl, glf::Socket, glwebtools::Socket, Comms (LAN multiplayer), gameoptions::TCPSocketConnection, automation::TcpServer.
- `sendto` - 5 refs; Comms 3, glf 1, C/unexported-code 1 - Linux numbering throughout: AF_INET 2, AF_INET6 10 (Windows 23), SOCK_STREAM 1, SOCK_DGRAM 2, SOL_SOCKET 1 (Windows 0xffff), SO_REUSEADDR 2, SO_RCVBUF 8, SO_ERROR 4, MSG_NOSIGNAL 0x4000, MSG_DONTWAIT 0x40. struct sockaddr has NO sa_len; sockaddr_in6 family value differs. fd_set is a 1024-bit bitmap indexed by fd (Windows fd_set is an array of SOCKET handles); struct pollfd {int fd; short events; short revents}, POLLIN 1 POLLOUT 4 POLLERR 8 POLLHUP 0x10 (Winsock WSAPoll values differ). Socket fds must be small ints, not SOCKET handles. Users: asio, curl, glf::Socket, glwebtools::Socket, Comms (LAN multiplayer), gameoptions::TCPSocketConnection, automation::TcpServer.
- `setsockopt` - 15 refs; C/unexported-code 5, glf 3, Comms 2 - Linux numbering throughout: AF_INET 2, AF_INET6 10 (Windows 23), SOCK_STREAM 1, SOCK_DGRAM 2, SOL_SOCKET 1 (Windows 0xffff), SO_REUSEADDR 2, SO_RCVBUF 8, SO_ERROR 4, MSG_NOSIGNAL 0x4000, MSG_DONTWAIT 0x40. struct sockaddr has NO sa_len; sockaddr_in6 family value differs. fd_set is a 1024-bit bitmap indexed by fd (Windows fd_set is an array of SOCKET handles); struct pollfd {int fd; short events; short revents}, POLLIN 1 POLLOUT 4 POLLERR 8 POLLHUP 0x10 (Winsock WSAPoll values differ). Socket fds must be small ints, not SOCKET handles. Users: asio, curl, glf::Socket, glwebtools::Socket, Comms (LAN multiplayer), gameoptions::TCPSocketConnection, automation::TcpServer.
- `shutdown` - 2 refs; asio 1, unexported-code(OpenSSL/curl) 1 - Linux numbering throughout: AF_INET 2, AF_INET6 10 (Windows 23), SOCK_STREAM 1, SOCK_DGRAM 2, SOL_SOCKET 1 (Windows 0xffff), SO_REUSEADDR 2, SO_RCVBUF 8, SO_ERROR 4, MSG_NOSIGNAL 0x4000, MSG_DONTWAIT 0x40. struct sockaddr has NO sa_len; sockaddr_in6 family value differs. fd_set is a 1024-bit bitmap indexed by fd (Windows fd_set is an array of SOCKET handles); struct pollfd {int fd; short events; short revents}, POLLIN 1 POLLOUT 4 POLLERR 8 POLLHUP 0x10 (Winsock WSAPoll values differ). Socket fds must be small ints, not SOCKET handles. Users: asio, curl, glf::Socket, glwebtools::Socket, Comms (LAN multiplayer), gameoptions::TCPSocketConnection, automation::TcpServer.
- `socket` - 13 refs; Comms 3, gameoptions 3, AnubisLib 1 - Linux numbering throughout: AF_INET 2, AF_INET6 10 (Windows 23), SOCK_STREAM 1, SOCK_DGRAM 2, SOL_SOCKET 1 (Windows 0xffff), SO_REUSEADDR 2, SO_RCVBUF 8, SO_ERROR 4, MSG_NOSIGNAL 0x4000, MSG_DONTWAIT 0x40. struct sockaddr has NO sa_len; sockaddr_in6 family value differs. fd_set is a 1024-bit bitmap indexed by fd (Windows fd_set is an array of SOCKET handles); struct pollfd {int fd; short events; short revents}, POLLIN 1 POLLOUT 4 POLLERR 8 POLLHUP 0x10 (Winsock WSAPoll values differ). Socket fds must be small ints, not SOCKET handles. Users: asio, curl, glf::Socket, glwebtools::Socket, Comms (LAN multiplayer), gameoptions::TCPSocketConnection, automation::TcpServer.
- `tcgetattr` - 1 refs; unexported-code(OpenSSL/curl) 1 - OpenSSL console prompt only (UI_OpenSSL); fail with ENOTTY.
- `tcsetattr` - 2 refs; unexported-code(OpenSSL/curl) 2 - OpenSSL console prompt only (UI_OpenSSL); fail with ENOTTY.

## libm - 36 symbols

All are plain C99 functions; float variants take/return `float` in s0, doubles in d0 (AAPCS64) - the JIT must marshal FP registers. Results come from bionic/NEON-friendly implementations; MSVC/UCRT results may differ in the last ulp (matters only for deterministic multiplayer simulation). No `fenv`, no `lgamma`.

- `acos` - 4 refs; unexported-code(glitch/gameswf/static libs) 3, unexported-code(OpenSSL/curl) 1
- `acosf` - 73 refs; unexported-code(glitch/gameswf/static libs) 8, unexported-code(OpenSSL/curl) 7, Hud 6
- `asin` - 2 refs; unexported-code(glitch/gameswf/static libs) 1, unexported-code(OpenSSL/curl) 1
- `asinf` - 4 refs; C/unexported-code 1, btMatrix3x3 1, CProjectileComponent 1
- `atan` - 6 refs; glitch 4, unexported-code(glitch/gameswf/static libs) 1, unexported-code(OpenSSL/curl) 1
- `atan2` - 7 refs; C/unexported-code 3, unexported-code(glitch/gameswf/static libs) 3, unexported-code(OpenSSL/curl) 1
- `atan2f` - 53 refs; unexported-code(OpenSSL/curl) 23, unexported-code(glitch/gameswf/static libs) 16, CActorComponent 7
- `atanf` - 5 refs; CNPCWeaponComponent 2, GameFOV 2, unexported-code(glitch/gameswf/static libs) 1
- `cos` - 142 refs; PlayerComponent 22, CMotionOnPathComponent 18, unexported-code(glitch/gameswf/static libs) 18
- `cosf` - 217 refs; unexported-code(glitch/gameswf/static libs) 70, unexported-code(OpenSSL/curl) 39, vox 14
- `cosh` - 1 refs; unexported-code(OpenSSL/curl) 1 - double versions; bionic arm64 results may differ from MSVC in the last ulp.
- `exp` - 2 refs; unexported-code(glitch/gameswf/static libs) 1, unexported-code(OpenSSL/curl) 1
- `exp2` - 1 refs; vox 1
- `exp2f` - 18 refs; vox 17, CGameObject 1
- `expf` - 37 refs; unexported-code(OpenSSL/curl) 24, unexported-code(glitch/gameswf/static libs) 11, C/unexported-code 2
- `fmod` - 6 refs; olutils 2, unexported-code(glitch/gameswf/static libs) 2, CMenuModelManager 1
- `fmodf` - 22 refs; unexported-code(glitch/gameswf/static libs) 9, unexported-code(OpenSSL/curl) 4, CMonolithComponent 2
- `frexp` - 1 refs; unexported-code(OpenSSL/curl) 1 - out-pointer parameters (int* / double* / float*) are guest memory.
- `ldexpf` - 3 refs; C/unexported-code 2, unexported-code(OpenSSL/curl) 1
- `log` - 2 refs; unexported-code(glitch/gameswf/static libs) 1, unexported-code(OpenSSL/curl) 1
- `log10` - 3 refs; C/unexported-code 2, unexported-code(OpenSSL/curl) 1
- `log10f` - 1 refs; vox 1
- `logf` - 31 refs; unexported-code(OpenSSL/curl) 25, unexported-code(glitch/gameswf/static libs) 4, VoxSoundManager 1
- `modf` - 1 refs; unexported-code(OpenSSL/curl) 1 - out-pointer parameters (int* / double* / float*) are guest memory.
- `modff` - 19 refs; unexported-code(glitch/gameswf/static libs) 19 - out-pointer parameters (int* / double* / float*) are guest memory.
- `pow` - 12 refs; C/unexported-code 4, unexported-code(OpenSSL/curl) 4, CTuningManager 2
- `powf` - 44 refs; vox 22, unexported-code(OpenSSL/curl) 17, unexported-code(glitch/gameswf/static libs) 3
- `sin` - 138 refs; PlayerComponent 22, CMotionOnPathComponent 18, unexported-code(glitch/gameswf/static libs) 16
- `sinf` - 195 refs; unexported-code(glitch/gameswf/static libs) 60, unexported-code(OpenSSL/curl) 39, CActorComponent 10
- `sinh` - 1 refs; unexported-code(OpenSSL/curl) 1 - double versions; bionic arm64 results may differ from MSVC in the last ulp.
- `sinhf` - 4 refs; vox 4
- `sqrt` - 20 refs; unexported-code(glitch/gameswf/static libs) 13, unexported-code(OpenSSL/curl) 3, MultiTouchControl 2
- `sqrtf` - 1193 refs; unexported-code(glitch/gameswf/static libs) 221, unexported-code(OpenSSL/curl) 202, CEnemyDroneAIComponent 48
- `tan` - 10 refs; unexported-code(glitch/gameswf/static libs) 7, CMenuModelManager 2, unexported-code(OpenSSL/curl) 1
- `tanf` - 18 refs; unexported-code(glitch/gameswf/static libs) 5, C/unexported-code 3, CNovaSceneManager 2
- `tanh` - 1 refs; unexported-code(OpenSSL/curl) 1 - double versions; bionic arm64 results may differ from MSVC in the last ulp.

## libdl - 1 symbol

- `dl_iterate_phdr` [also libgenerator] - 1 refs; C/unexported-code 1 - (callback(struct dl_phdr_info*,size_t size,void*),data). Called by the statically linked unwinder (_Unwind_Find_FDE in libgenerator, same code in libNOVA) to locate .eh_frame_hdr. Host must invoke the guest callback once per loaded object (libNOVA, libgenerator) with dlpi_addr (load bias), dlpi_name, dlpi_phdr pointing at the program headers IN GUEST MEMORY (so the ELF loader must map them), dlpi_phnum, and PT_GNU_EH_FRAME present. Without it every C++ throw terminates. libNOVA exports __cxa_throw, _Unwind_RaiseException, __gxx_personality_v0, so libc++abi and the unwinder are inside the library.

## libz - 0 symbols

None imported (zlib is statically linked, see above).

## liblog - 1 symbol

- `__android_log_print` - 28 refs; acp_utils 19, glf 6, sProfiler 2 - (int prio, const char* tag, const char* fmt, ...) variadic; prio 2 VERBOSE .. 6 ERROR. 18 call sites (glf::Print, FileLogger, FileStreamImpl::Open, App::Impl::Run on eglSwapBuffers failure, acp_utils). Redirect to the host log.

## libEGL - 18 symbols

- `eglChooseConfig` - 3 refs; ANativeSurface 2, gameoptions 1 - attrib list is a guest int array terminated by EGL_NONE 0x3038. eglGetConfigAttrib attribute names seen at call sites: 0x3028 EGL_CONFIG_ID and 0x3024 EGL_RED_SIZE (FilterRenderingConfig / LogConfig / IsValidConfig, gameoptions::CreateAndMakeCurrent). Host must return at least one window+pbuffer-capable ES2 config.
- `eglCreateContext` - 4 refs; ANativeSurface 3, gameoptions 1 - attrib list is a guest int array (an ES2 context is expected since only libGLESv2 is linked; the version attribute value was not decoded). Three call sites (NativeSurface::CreateGLEnvironment/CreateContext, gameoptions::CreateAndMakeCurrent).
- `eglCreatePbufferSurface` - 2 refs; ANativeSurface 1, gameoptions 1 - window argument is an ANativeWindow* the host handed out via ANativeWindow_fromSurface; pbuffer is used by gameoptions::CreateAndMakeCurrent (GPU probing).
- `eglCreateWindowSurface` - 1 refs; ANativeSurface 1 - window argument is an ANativeWindow* the host handed out via ANativeWindow_fromSurface; pbuffer is used by gameoptions::CreateAndMakeCurrent (GPU probing).
- `eglDestroyContext` - 5 refs; ANativeSurface 3, gameoptions 2 - plain EGL 1.x queries (eglQuerySurface supplies surface size; eglQueryString is called with an EGL_VENDOR/VERSION/EXTENSIONS-style name by CreateGLEnvironment and NativeSurface::QueryString).
- `eglDestroySurface` - 7 refs; ANativeSurface 5, gameoptions 2 - plain EGL 1.x queries (eglQuerySurface supplies surface size; eglQueryString is called with an EGL_VENDOR/VERSION/EXTENSIONS-style name by CreateGLEnvironment and NativeSurface::QueryString).
- `eglGetConfigAttrib` - 26 refs; ANativeSurface 24, gameoptions 2 - attrib list is a guest int array terminated by EGL_NONE 0x3038. eglGetConfigAttrib attribute names seen at call sites: 0x3028 EGL_CONFIG_ID and 0x3024 EGL_RED_SIZE (FilterRenderingConfig / LogConfig / IsValidConfig, gameoptions::CreateAndMakeCurrent). Host must return at least one window+pbuffer-capable ES2 config.
- `eglGetCurrentContext` - 4 refs; glf 3, gameoptions 1 - plain EGL 1.x queries (eglQuerySurface supplies surface size; eglQueryString is called with an EGL_VENDOR/VERSION/EXTENSIONS-style name by CreateGLEnvironment and NativeSurface::QueryString).
- `eglGetCurrentDisplay` - 3 refs; unexported-code(glitch/gameswf/static libs) 3 - plain EGL 1.x queries (eglQuerySurface supplies surface size; eglQueryString is called with an EGL_VENDOR/VERSION/EXTENSIONS-style name by CreateGLEnvironment and NativeSurface::QueryString).
- `eglGetDisplay` - 3 refs; ANativeSurface 2, gameoptions 1 - EGLDisplay is a pointer-sized opaque; EGL_DEFAULT_DISPLAY = 0.
- `eglGetError` - 1 refs; ANativeSurface 1 - plain EGL 1.x queries (eglQuerySurface supplies surface size; eglQueryString is called with an EGL_VENDOR/VERSION/EXTENSIONS-style name by CreateGLEnvironment and NativeSurface::QueryString).
- `eglGetProcAddress` - 115 refs; unexported-code(glitch/gameswf/static libs) 115 - (const char*) returns a guest-callable function pointer: host must hand back trampolines (thunks into the host GL layer) for the names listed in the runtime-lookup table below, or NULL for unsupported ones. The only caller is one stripped function in the glitch GL ES driver (about 130 call sites between 0x1e08e38 and 0x1e0cd14).
- `eglInitialize` - 3 refs; ANativeSurface 2, gameoptions 1 - EGLDisplay is a pointer-sized opaque; EGL_DEFAULT_DISPLAY = 0.
- `eglMakeCurrent` - 10 refs; ANativeSurface 5, glf 3, gameoptions 2 - nine sites: glf::App::SetCurrentContext, UnsetAsMainThread, ReleaseContext, NativeSurface::MakeCurrent/Create/Refresh/DestroyGLEnvironment, gameoptions::CreateAndMakeCurrent. Context hand-off between threads happens (ReserveContext/ReleaseContext), so the GL layer must be bindable from more than one host thread.
- `eglQueryString` - 4 refs; ANativeSurface 4 - plain EGL 1.x queries (eglQuerySurface supplies surface size; eglQueryString is called with an EGL_VENDOR/VERSION/EXTENSIONS-style name by CreateGLEnvironment and NativeSurface::QueryString).
- `eglQuerySurface` - 4 refs; ANativeSurface 4 - plain EGL 1.x queries (eglQuerySurface supplies surface size; eglQueryString is called with an EGL_VENDOR/VERSION/EXTENSIONS-style name by CreateGLEnvironment and NativeSurface::QueryString).
- `eglSwapBuffers` - 2 refs; glf 2 - called once per frame from glf::App::Impl::Run()/Present(); the host present/vsync point. No eglSwapInterval is imported, so vsync is entirely host controlled; the in-game limiter is Application::LimitFps().
- `eglTerminate` - 5 refs; gameoptions 3, ANativeSurface 2 - EGLDisplay is a pointer-sized opaque; EGL_DEFAULT_DISPLAY = 0.

## libGLESv2 - 96 symbols

Pure OpenGL ES 2.0 core (no VAOs, no instancing, no mapbuffer, no ES3 entry points are imported). `glGenVertexArrays`/instancing/MapBuffer/FBO-EXT variants are only reached through `eglGetProcAddress` (next section). GL enums and object names are 32-bit; there is no `glGetStringi`.

- `glActiveTexture` - 6 refs; unexported-code(glitch/gameswf/static libs) 6
- `glAttachShader` - 1 refs; unexported-code(glitch/gameswf/static libs) 1
- `glBindBuffer` - 20 refs; unexported-code(glitch/gameswf/static libs) 20
- `glBindFramebuffer` - 4 refs; unexported-code(glitch/gameswf/static libs) 4 - core in ES2; the engine also tries the *EXT/*OES names through eglGetProcAddress (see table).
- `glBindRenderbuffer` - 1 refs; unexported-code(glitch/gameswf/static libs) 1 - core in ES2; the engine also tries the *EXT/*OES names through eglGetProcAddress (see table).
- `glBindTexture` - 9 refs; unexported-code(glitch/gameswf/static libs) 8, glf 1
- `glBlendColor` - 2 refs; unexported-code(glitch/gameswf/static libs) 2 - plain state calls; GL_UNPACK_ALIGNMENT is set by the engine.
- `glBlendEquation` - 3 refs; unexported-code(glitch/gameswf/static libs) 3 - plain state calls; GL_UNPACK_ALIGNMENT is set by the engine.
- `glBlendFunc` - 3 refs; unexported-code(glitch/gameswf/static libs) 3 - plain state calls; GL_UNPACK_ALIGNMENT is set by the engine.
- `glBufferData` - 1 refs; unexported-code(glitch/gameswf/static libs) 1 - pointer arguments reference GUEST memory (client-side vertex arrays are possible: glVertexAttribPointer/glDrawElements with no buffer bound pass raw guest addresses; with a buffer bound the pointer is an offset). GLsizeiptr/GLintptr are 64-bit.
- `glBufferSubData` - 5 refs; unexported-code(glitch/gameswf/static libs) 5 - pointer arguments reference GUEST memory (client-side vertex arrays are possible: glVertexAttribPointer/glDrawElements with no buffer bound pass raw guest addresses; with a buffer bound the pointer is an offset). GLsizeiptr/GLintptr are 64-bit.
- `glCheckFramebufferStatus` - 1 refs; unexported-code(glitch/gameswf/static libs) 1 - core in ES2; the engine also tries the *EXT/*OES names through eglGetProcAddress (see table).
- `glClear` - 4 refs; unexported-code(glitch/gameswf/static libs) 4
- `glClearColor` - 4 refs; unexported-code(glitch/gameswf/static libs) 4
- `glClearDepthf` - 2 refs; unexported-code(glitch/gameswf/static libs) 2 - float variants (ES2); map to glClearDepth/glDepthRange.
- `glClearStencil` - 1 refs; unexported-code(glitch/gameswf/static libs) 1
- `glColorMask` - 2 refs; unexported-code(glitch/gameswf/static libs) 2 - plain state calls; GL_UNPACK_ALIGNMENT is set by the engine.
- `glCompileShader` - 1 refs; unexported-code(glitch/gameswf/static libs) 1
- `glCompressedTexImage2D` - 1 refs; unexported-code(glitch/gameswf/static libs) 1 - texture formats the engine may upload compressed: ETC1, PVRTC (.pvr), ATC, ASTC, DXT/S3TC per the extension strings (.ktx/.pvr/.dds loaders in glitch). Desktop GL has S3TC but not ETC1/PVRTC/ATC; the host must decode or transcode if the game picks those.
- `glCompressedTexSubImage2D` - 1 refs; unexported-code(glitch/gameswf/static libs) 1 - texture formats the engine may upload compressed: ETC1, PVRTC (.pvr), ATC, ASTC, DXT/S3TC per the extension strings (.ktx/.pvr/.dds loaders in glitch). Desktop GL has S3TC but not ETC1/PVRTC/ATC; the host must decode or transcode if the game picks those.
- `glCopyTexSubImage2D` - 1 refs; unexported-code(glitch/gameswf/static libs) 1
- `glCreateProgram` - 1 refs; unexported-code(glitch/gameswf/static libs) 1
- `glCreateShader` - 1 refs; unexported-code(glitch/gameswf/static libs) 1
- `glCullFace` - 2 refs; unexported-code(glitch/gameswf/static libs) 2 - plain state calls; GL_UNPACK_ALIGNMENT is set by the engine.
- `glDeleteBuffers` - 3 refs; unexported-code(glitch/gameswf/static libs) 3
- `glDeleteFramebuffers` - 3 refs; unexported-code(glitch/gameswf/static libs) 3 - core in ES2; the engine also tries the *EXT/*OES names through eglGetProcAddress (see table).
- `glDeleteProgram` - 3 refs; unexported-code(glitch/gameswf/static libs) 3
- `glDeleteRenderbuffers` - 3 refs; unexported-code(glitch/gameswf/static libs) 3 - core in ES2; the engine also tries the *EXT/*OES names through eglGetProcAddress (see table).
- `glDeleteShader` - 2 refs; unexported-code(glitch/gameswf/static libs) 2
- `glDeleteTextures` - 4 refs; unexported-code(glitch/gameswf/static libs) 2, glf 2
- `glDepthFunc` - 3 refs; unexported-code(glitch/gameswf/static libs) 3
- `glDepthMask` - 5 refs; unexported-code(glitch/gameswf/static libs) 5 - plain state calls; GL_UNPACK_ALIGNMENT is set by the engine.
- `glDepthRangef` - 2 refs; unexported-code(glitch/gameswf/static libs) 2 - float variants (ES2); map to glClearDepth/glDepthRange.
- `glDisable` - 21 refs; unexported-code(glitch/gameswf/static libs) 21
- `glDisableVertexAttribArray` - 4 refs; unexported-code(glitch/gameswf/static libs) 4
- `glDrawArrays` - 2 refs; unexported-code(glitch/gameswf/static libs) 2
- `glDrawElements` - 4 refs; unexported-code(glitch/gameswf/static libs) 4 - pointer arguments reference GUEST memory (client-side vertex arrays are possible: glVertexAttribPointer/glDrawElements with no buffer bound pass raw guest addresses; with a buffer bound the pointer is an offset). GLsizeiptr/GLintptr are 64-bit.
- `glEnable` - 21 refs; unexported-code(glitch/gameswf/static libs) 20, glf 1
- `glEnableVertexAttribArray` - 3 refs; unexported-code(glitch/gameswf/static libs) 3
- `glFinish` - 1 refs; unexported-code(glitch/gameswf/static libs) 1
- `glFlush` - 8 refs; unexported-code(glitch/gameswf/static libs) 8
- `glFramebufferRenderbuffer` - 2 refs; unexported-code(glitch/gameswf/static libs) 2 - core in ES2; the engine also tries the *EXT/*OES names through eglGetProcAddress (see table).
- `glFramebufferTexture2D` - 1 refs; unexported-code(glitch/gameswf/static libs) 1 - core in ES2; the engine also tries the *EXT/*OES names through eglGetProcAddress (see table).
- `glFrontFace` - 6 refs; unexported-code(glitch/gameswf/static libs) 6 - plain state calls; GL_UNPACK_ALIGNMENT is set by the engine.
- `glGenBuffers` - 1 refs; unexported-code(glitch/gameswf/static libs) 1
- `glGenFramebuffers` - 1 refs; unexported-code(glitch/gameswf/static libs) 1 - core in ES2; the engine also tries the *EXT/*OES names through eglGetProcAddress (see table).
- `glGenRenderbuffers` - 1 refs; unexported-code(glitch/gameswf/static libs) 1 - core in ES2; the engine also tries the *EXT/*OES names through eglGetProcAddress (see table).
- `glGenTextures` - 2 refs; unexported-code(glitch/gameswf/static libs) 1, glf 1
- `glGenerateMipmap` - 1 refs; unexported-code(glitch/gameswf/static libs) 1 - core in ES2; the engine also tries the *EXT/*OES names through eglGetProcAddress (see table).
- `glGetActiveAttrib` - 1 refs; unexported-code(glitch/gameswf/static libs) 1 - out-pointers into guest memory; result must be written with guest-visible byte order/size (GLint 4 bytes, GLfloat 4 bytes, GLsizei 4 bytes).
- `glGetActiveUniform` - 1 refs; unexported-code(glitch/gameswf/static libs) 1 - out-pointers into guest memory; result must be written with guest-visible byte order/size (GLint 4 bytes, GLfloat 4 bytes, GLsizei 4 bytes).
- `glGetAttribLocation` - 1 refs; unexported-code(glitch/gameswf/static libs) 1 - out-pointers into guest memory; result must be written with guest-visible byte order/size (GLint 4 bytes, GLfloat 4 bytes, GLsizei 4 bytes).
- `glGetError` - 2 refs; unexported-code(glitch/gameswf/static libs) 2 - out-pointers into guest memory; result must be written with guest-visible byte order/size (GLint 4 bytes, GLfloat 4 bytes, GLsizei 4 bytes).
- `glGetFloatv` - 1 refs; unexported-code(glitch/gameswf/static libs) 1 - out-pointers into guest memory; result must be written with guest-visible byte order/size (GLint 4 bytes, GLfloat 4 bytes, GLsizei 4 bytes).
- `glGetIntegerv` - 10 refs; unexported-code(glitch/gameswf/static libs) 10 - out-pointers into guest memory; result must be written with guest-visible byte order/size (GLint 4 bytes, GLfloat 4 bytes, GLsizei 4 bytes).
- `glGetProgramInfoLog` - 1 refs; unexported-code(glitch/gameswf/static libs) 1 - out-pointers into guest memory; result must be written with guest-visible byte order/size (GLint 4 bytes, GLfloat 4 bytes, GLsizei 4 bytes).
- `glGetProgramiv` - 7 refs; unexported-code(glitch/gameswf/static libs) 7 - out-pointers into guest memory; result must be written with guest-visible byte order/size (GLint 4 bytes, GLfloat 4 bytes, GLsizei 4 bytes).
- `glGetShaderInfoLog` - 2 refs; unexported-code(glitch/gameswf/static libs) 2 - out-pointers into guest memory; result must be written with guest-visible byte order/size (GLint 4 bytes, GLfloat 4 bytes, GLsizei 4 bytes).
- `glGetShaderSource` - 2 refs; unexported-code(glitch/gameswf/static libs) 2 - out-pointers into guest memory; result must be written with guest-visible byte order/size (GLint 4 bytes, GLfloat 4 bytes, GLsizei 4 bytes).
- `glGetShaderiv` - 7 refs; unexported-code(glitch/gameswf/static libs) 7 - out-pointers into guest memory; result must be written with guest-visible byte order/size (GLint 4 bytes, GLfloat 4 bytes, GLsizei 4 bytes).
- `glGetString` - 16 refs; unexported-code(glitch/gameswf/static libs) 13, gameoptions 2, ANativeSurface 1 - (names seen at call sites: 7937 = GL_RENDERER in CDeviceSpecs_android::GetDeviceGpu, 7938 = GL_VERSION in GetGraphicsFeatureLevel, 7939 = GL_EXTENSIONS in CreateGLEnvironment and the extension loader). Return strings in GUEST memory (allocate once). Engine parses GL_RENDERER for GPU profile ("Adreno (TM) 330", "Mali-T628", "PowerVR SGX 544" are string-matched in the file system and device options) and GL_VERSION for "OpenGL ES 2.0/3.0/3.1/3.2"; GL_EXTENSIONS drives the extension loader below.
- `glGetUniformLocation` - 4 refs; unexported-code(glitch/gameswf/static libs) 4 - out-pointers into guest memory; result must be written with guest-visible byte order/size (GLint 4 bytes, GLfloat 4 bytes, GLsizei 4 bytes).
- `glLineWidth` - 2 refs; unexported-code(glitch/gameswf/static libs) 2 - plain state calls; GL_UNPACK_ALIGNMENT is set by the engine.
- `glLinkProgram` - 1 refs; unexported-code(glitch/gameswf/static libs) 1
- `glPixelStorei` - 2 refs; unexported-code(glitch/gameswf/static libs) 2 - plain state calls; GL_UNPACK_ALIGNMENT is set by the engine.
- `glPolygonOffset` - 2 refs; unexported-code(glitch/gameswf/static libs) 2 - plain state calls; GL_UNPACK_ALIGNMENT is set by the engine.
- `glReadPixels` - 1 refs; unexported-code(glitch/gameswf/static libs) 1 - out-pointers into guest memory; result must be written with guest-visible byte order/size (GLint 4 bytes, GLfloat 4 bytes, GLsizei 4 bytes).
- `glRenderbufferStorage` - 1 refs; unexported-code(glitch/gameswf/static libs) 1 - core in ES2; the engine also tries the *EXT/*OES names through eglGetProcAddress (see table).
- `glSampleCoverage` - 3 refs; unexported-code(glitch/gameswf/static libs) 3 - plain state calls; GL_UNPACK_ALIGNMENT is set by the engine.
- `glScissor` - 2 refs; unexported-code(glitch/gameswf/static libs) 2
- `glShaderSource` - 1 refs; unexported-code(glitch/gameswf/static libs) 1 - (shader,count,const GLchar* const* string,const GLint* length): array of GUEST pointers; shaders are GLSL ES 1.00 (precision qualifiers, no #version or "#version 100"), must be accepted by the host GL (ANGLE or desktop GL with conversion).
- `glStencilFunc` - 3 refs; unexported-code(glitch/gameswf/static libs) 3
- `glStencilMask` - 3 refs; unexported-code(glitch/gameswf/static libs) 3 - plain state calls; GL_UNPACK_ALIGNMENT is set by the engine.
- `glStencilOp` - 3 refs; unexported-code(glitch/gameswf/static libs) 3
- `glTexImage2D` - 2 refs; unexported-code(glitch/gameswf/static libs) 1, glf 1 - pointer arguments reference GUEST memory (client-side vertex arrays are possible: glVertexAttribPointer/glDrawElements with no buffer bound pass raw guest addresses; with a buffer bound the pointer is an offset). GLsizeiptr/GLintptr are 64-bit.
- `glTexParameterf` - 7 refs; unexported-code(glitch/gameswf/static libs) 5, glf 2
- `glTexParameteri` - 14 refs; unexported-code(glitch/gameswf/static libs) 14
- `glTexSubImage2D` - 1 refs; unexported-code(glitch/gameswf/static libs) 1 - pointer arguments reference GUEST memory (client-side vertex arrays are possible: glVertexAttribPointer/glDrawElements with no buffer bound pass raw guest addresses; with a buffer bound the pointer is an offset). GLsizeiptr/GLintptr are 64-bit.
- `glUniform1f` - 6 refs; unexported-code(glitch/gameswf/static libs) 6
- `glUniform1fv` - 2 refs; unexported-code(glitch/gameswf/static libs) 2 - pointer arguments reference GUEST memory (client-side vertex arrays are possible: glVertexAttribPointer/glDrawElements with no buffer bound pass raw guest addresses; with a buffer bound the pointer is an offset). GLsizeiptr/GLintptr are 64-bit.
- `glUniform1i` - 8 refs; unexported-code(glitch/gameswf/static libs) 8
- `glUniform1iv` - 3 refs; unexported-code(glitch/gameswf/static libs) 3 - pointer arguments reference GUEST memory (client-side vertex arrays are possible: glVertexAttribPointer/glDrawElements with no buffer bound pass raw guest addresses; with a buffer bound the pointer is an offset). GLsizeiptr/GLintptr are 64-bit.
- `glUniform2fv` - 8 refs; unexported-code(glitch/gameswf/static libs) 8 - pointer arguments reference GUEST memory (client-side vertex arrays are possible: glVertexAttribPointer/glDrawElements with no buffer bound pass raw guest addresses; with a buffer bound the pointer is an offset). GLsizeiptr/GLintptr are 64-bit.
- `glUniform2iv` - 5 refs; unexported-code(glitch/gameswf/static libs) 5 - pointer arguments reference GUEST memory (client-side vertex arrays are possible: glVertexAttribPointer/glDrawElements with no buffer bound pass raw guest addresses; with a buffer bound the pointer is an offset). GLsizeiptr/GLintptr are 64-bit.
- `glUniform3fv` - 9 refs; unexported-code(glitch/gameswf/static libs) 9 - pointer arguments reference GUEST memory (client-side vertex arrays are possible: glVertexAttribPointer/glDrawElements with no buffer bound pass raw guest addresses; with a buffer bound the pointer is an offset). GLsizeiptr/GLintptr are 64-bit.
- `glUniform3iv` - 4 refs; unexported-code(glitch/gameswf/static libs) 4 - pointer arguments reference GUEST memory (client-side vertex arrays are possible: glVertexAttribPointer/glDrawElements with no buffer bound pass raw guest addresses; with a buffer bound the pointer is an offset). GLsizeiptr/GLintptr are 64-bit.
- `glUniform4f` - 1 refs; unexported-code(glitch/gameswf/static libs) 1
- `glUniform4fv` - 10 refs; unexported-code(glitch/gameswf/static libs) 10 - pointer arguments reference GUEST memory (client-side vertex arrays are possible: glVertexAttribPointer/glDrawElements with no buffer bound pass raw guest addresses; with a buffer bound the pointer is an offset). GLsizeiptr/GLintptr are 64-bit.
- `glUniform4iv` - 3 refs; unexported-code(glitch/gameswf/static libs) 3 - pointer arguments reference GUEST memory (client-side vertex arrays are possible: glVertexAttribPointer/glDrawElements with no buffer bound pass raw guest addresses; with a buffer bound the pointer is an offset). GLsizeiptr/GLintptr are 64-bit.
- `glUniformMatrix2fv` - 3 refs; unexported-code(glitch/gameswf/static libs) 3 - pointer arguments reference GUEST memory (client-side vertex arrays are possible: glVertexAttribPointer/glDrawElements with no buffer bound pass raw guest addresses; with a buffer bound the pointer is an offset). GLsizeiptr/GLintptr are 64-bit.
- `glUniformMatrix3fv` - 3 refs; unexported-code(glitch/gameswf/static libs) 3 - pointer arguments reference GUEST memory (client-side vertex arrays are possible: glVertexAttribPointer/glDrawElements with no buffer bound pass raw guest addresses; with a buffer bound the pointer is an offset). GLsizeiptr/GLintptr are 64-bit.
- `glUniformMatrix4fv` - 4 refs; unexported-code(glitch/gameswf/static libs) 4 - pointer arguments reference GUEST memory (client-side vertex arrays are possible: glVertexAttribPointer/glDrawElements with no buffer bound pass raw guest addresses; with a buffer bound the pointer is an offset). GLsizeiptr/GLintptr are 64-bit.
- `glUseProgram` - 3 refs; unexported-code(glitch/gameswf/static libs) 3
- `glVertexAttrib4f` - 1 refs; unexported-code(glitch/gameswf/static libs) 1
- `glVertexAttribPointer` - 1 refs; unexported-code(glitch/gameswf/static libs) 1 - pointer arguments reference GUEST memory (client-side vertex arrays are possible: glVertexAttribPointer/glDrawElements with no buffer bound pass raw guest addresses; with a buffer bound the pointer is an offset). GLsizeiptr/GLintptr are 64-bit.
- `glViewport` - 1 refs; unexported-code(glitch/gameswf/static libs) 1

## libandroid - 25 symbols

- `AAssetDir_close` - 1 refs; acp_utils 1 - directory listing of APK assets (names without path).
- `AAssetDir_getNextFileName` - 1 refs; acp_utils 1 - directory listing of APK assets (names without path).
- `AAssetDir_rewind` - 1 refs; acp_utils 1 - directory listing of APK assets (names without path).
- `AAssetManager_fromJava` - 1 refs; acp_utils 1 - (JNIEnv*, jobject assetManager). The jobject is the result of the Java call AndroidUtils.GetAssetManager() (see JNI lookup table). Host returns an AAssetManager that reads from the APK zip (assets/ prefix stripped).
- `AAssetManager_open` - 1 refs; acp_utils 1 - (mgr, filename, mode) mode AASSET_MODE_UNKNOWN 0/RANDOM 1/STREAMING 2/BUFFER 3; returns NULL when missing (callers rely on that: FileStreamImpl falls back to it after a failed fopen).
- `AAssetManager_openDir` - 1 refs; acp_utils 1 - directory listing of APK assets (names without path).
- `AAsset_close` - 2 refs; acp_utils 1, C/unexported-code 1 - off_t is 64-bit (AAsset_seek returns off_t). AAsset_getBuffer returns a pointer that must stay valid until AAsset_close.
- `AAsset_getBuffer` - 1 refs; acp_utils 1 - off_t is 64-bit (AAsset_seek returns off_t). AAsset_getBuffer returns a pointer that must stay valid until AAsset_close.
- `AAsset_getLength` - 1 refs; acp_utils 1 - off_t is 64-bit (AAsset_seek returns off_t). AAsset_getBuffer returns a pointer that must stay valid until AAsset_close.
- `AAsset_read` - 2 refs; acp_utils 1, C/unexported-code 1 - off_t is 64-bit (AAsset_seek returns off_t). AAsset_getBuffer returns a pointer that must stay valid until AAsset_close.
- `AAsset_seek` - 2 refs; acp_utils 1, C/unexported-code 1 - off_t is 64-bit (AAsset_seek returns off_t). AAsset_getBuffer returns a pointer that must stay valid until AAsset_close.
- `ALooper_forThread` - 1 refs; acp_utils 1 - only used by AndroidSensorManager::AndroidInitSensors (sensor event queue needs a looper). Return a dummy looper.
- `ALooper_prepare` - 1 refs; acp_utils 1 - only used by AndroidSensorManager::AndroidInitSensors (sensor event queue needs a looper). Return a dummy looper.
- `ANativeWindow_fromSurface` - 1 refs; C/unexported-code 1 - (JNIEnv*, jobject surface) -> ANativeWindow*; the host passes a fake jobject (the Surface passed to NativeSurfaceChanged) and returns its own window object. Called from JNIBridge.NativeSurfaceChanged.
- `ANativeWindow_getHeight` - 1 refs; ANativeSurface 1 - setBuffersGeometry(window,w,h,format) is called by NativeSurface::SetWindowSpecific with the EGL native visual id; width/height drive the screen size the engine sees.
- `ANativeWindow_getWidth` - 1 refs; ANativeSurface 1 - setBuffersGeometry(window,w,h,format) is called by NativeSurface::SetWindowSpecific with the EGL native visual id; width/height drive the screen size the engine sees.
- `ANativeWindow_release` - 1 refs; C/unexported-code 1 - setBuffersGeometry(window,w,h,format) is called by NativeSurface::SetWindowSpecific with the EGL native visual id; width/height drive the screen size the engine sees.
- `ANativeWindow_setBuffersGeometry` - 1 refs; ANativeSurface 1 - setBuffersGeometry(window,w,h,format) is called by NativeSurface::SetWindowSpecific with the EGL native visual id; width/height drive the screen size the engine sees.
- `ASensorEventQueue_disableSensor` - 6 refs; acp_utils 6 - accelerometer (type 1) and gyroscope (type 4) via AndroidSensorManager; ASensorEvent is 104 bytes {int32 version; int32 sensor; int32 type; int32 reserved0; int64 timestamp; float data[16]; int32 flags; int32 reserved1[3]}. On PC return NULL sensors (getDefaultSensor returns NULL) or feed mouse look.
- `ASensorEventQueue_enableSensor` - 4 refs; acp_utils 4 - accelerometer (type 1) and gyroscope (type 4) via AndroidSensorManager; ASensorEvent is 104 bytes {int32 version; int32 sensor; int32 type; int32 reserved0; int64 timestamp; float data[16]; int32 flags; int32 reserved1[3]}. On PC return NULL sensors (getDefaultSensor returns NULL) or feed mouse look.
- `ASensorEventQueue_getEvents` - 2 refs; acp_utils 2 - accelerometer (type 1) and gyroscope (type 4) via AndroidSensorManager; ASensorEvent is 104 bytes {int32 version; int32 sensor; int32 type; int32 reserved0; int64 timestamp; float data[16]; int32 flags; int32 reserved1[3]}. On PC return NULL sensors (getDefaultSensor returns NULL) or feed mouse look.
- `ASensorEventQueue_setEventRate` - 4 refs; acp_utils 4 - accelerometer (type 1) and gyroscope (type 4) via AndroidSensorManager; ASensorEvent is 104 bytes {int32 version; int32 sensor; int32 type; int32 reserved0; int64 timestamp; float data[16]; int32 flags; int32 reserved1[3]}. On PC return NULL sensors (getDefaultSensor returns NULL) or feed mouse look.
- `ASensorManager_createEventQueue` - 1 refs; acp_utils 1 - accelerometer (type 1) and gyroscope (type 4) via AndroidSensorManager; ASensorEvent is 104 bytes {int32 version; int32 sensor; int32 type; int32 reserved0; int64 timestamp; float data[16]; int32 flags; int32 reserved1[3]}. On PC return NULL sensors (getDefaultSensor returns NULL) or feed mouse look.
- `ASensorManager_getDefaultSensor` - 2 refs; acp_utils 2 - accelerometer (type 1) and gyroscope (type 4) via AndroidSensorManager; ASensorEvent is 104 bytes {int32 version; int32 sensor; int32 type; int32 reserved0; int64 timestamp; float data[16]; int32 flags; int32 reserved1[3]}. On PC return NULL sensors (getDefaultSensor returns NULL) or feed mouse look.
- `ASensorManager_getInstance` - 1 refs; acp_utils 1 - accelerometer (type 1) and gyroscope (type 4) via AndroidSensorManager; ASensorEvent is 104 bytes {int32 version; int32 sensor; int32 type; int32 reserved0; int64 timestamp; float data[16]; int32 flags; int32 reserved1[3]}. On PC return NULL sensors (getDefaultSensor returns NULL) or feed mouse look.

## libOpenSLES - 4 symbols (1 function, 3 data)

- `SL_IID_BUFFERQUEUE` [DATA] - 1 refs; vox 1 - DATA symbols of type `const SLInterfaceID` = pointer to a 16-byte GUID struct {u32 time_low; u16 time_mid; u16 time_hi_and_version; u16 clock_seq; u8 node[6]}. Must be exported as guest pointers to guest-memory GUIDs with the standard OpenSL values; the game passes them to GetInterface and the host compares by value or pointer.
- `SL_IID_ENGINE` [DATA] - 1 refs; vox 1 - DATA symbols of type `const SLInterfaceID` = pointer to a 16-byte GUID struct {u32 time_low; u16 time_mid; u16 time_hi_and_version; u16 clock_seq; u8 node[6]}. Must be exported as guest pointers to guest-memory GUIDs with the standard OpenSL values; the game passes them to GetInterface and the host compares by value or pointer.
- `SL_IID_PLAY` [DATA] - 1 refs; vox 1 - DATA symbols of type `const SLInterfaceID` = pointer to a 16-byte GUID struct {u32 time_low; u16 time_mid; u16 time_hi_and_version; u16 clock_seq; u8 node[6]}. Must be exported as guest pointers to guest-memory GUIDs with the standard OpenSL values; the game passes them to GetInterface and the host compares by value or pointer.
- `slCreateEngine` - 1 refs; vox 1 - (SLObjectItf* out, nOptions, pOptions, nInterfaces, pIds, pRequired). Single caller vox::DriverAndroid::_InitOSL. The result is a COM-style object: SLObjectItf = pointer to pointer to a struct of function pointers; the game calls straight through those tables (indirect blr), so the host must build guest-visible vtables whose slots are trampolines. Interfaces requested: SL_IID_ENGINE, SL_IID_PLAY, SL_IID_BUFFERQUEUE (SLBufferQueue, not SLAndroidSimpleBufferQueue for playback). Vtable slots called in _InitOSL (by offset): SLObject slots 0 Realize, 3 GetInterface, 6 Destroy; SLEngine slots 2 CreateAudioPlayer and 7 CreateOutputMix; then SLPlay SetPlayState, BufferQueue Enqueue/RegisterCallback (inferred from vtable offsets, not checked against OpenSLES.h line by line). A Java AudioTrack backend also exists (vox::DriverAndroid::_InitAT; the class android/media/AudioTrack is in the preloaded JNI class table) and could be selected instead.

## Imported data symbols (variables)

| Symbol | Library | Type and layout | Notes |
|---|---|---|---|
| `SL_IID_BUFFERQUEUE` | libOpenSLES | const SLInterfaceID | pass to GetInterface |
| `SL_IID_ENGINE` | libOpenSLES | const SLInterfaceID (pointer to 16-byte GUID) | pass to GetInterface |
| `SL_IID_PLAY` | libOpenSLES | const SLInterfaceID | pass to GetInterface |
| `__sF` | libc | FILE[3], 152 bytes each (456 total) | stdin/stdout/stderr; code takes &__sF+152 and &__sF+304 |
| `_ctype_` | libc | char[257] bionic ctype flag table | indexed [c+1] |
| `timezone` | libc | long (8 bytes) | read only |

libgenerator.so imports only `__sF`.

## Names looked up at run time (eglGetProcAddress)

One function in the glitch GL ES driver initialisation (unnamed, between `sociallib::OluPrint` and the next exported symbol in the symbol table; about 130 `eglGetProcAddress` call sites in 0x1e08e38-0x1e0cd14) resolves the following names by string. All names also exist in the `.rodata` as plain C strings. Names that are ALSO linked statically (the FBO/renderbuffer set) are retried with the EXT suffix. Everything not found stays NULL and the engine takes the core-ES2 path; the host should return real thunks only for functions it implements AND advertises in `GL_EXTENSIONS`.

- Vertex array / instancing / divisor (12): `glDrawArraysInstanced` `glDrawArraysInstancedARB` `glDrawArraysInstancedEXT` `glDrawArraysInstancedNV` `glDrawElementsInstanced` `glDrawElementsInstancedARB` `glDrawElementsInstancedEXT` `glDrawElementsInstancedNV` `glVertexAttribDivisor` `glVertexAttribDivisorARB` `glVertexAttribDivisorEXT` `glVertexAttribDivisorNV`
- Buffer mapping and ranges (19): `glBindBufferBase` `glBindBufferBaseEXT` `glBindBufferBaseNV` `glBindBufferRange` `glBindBufferRangeEXT` `glBindBufferRangeNV` `glFlushMappedBufferRange` `glFlushMappedBufferRangeEXT` `glGetBufferPointerv` `glGetBufferPointervARB` `glGetBufferPointervOES` `glMapBuffer` `glMapBufferARB` `glMapBufferOES` `glMapBufferRange` `glMapBufferRangeEXT` `glUnmapBuffer` `glUnmapBufferARB` `glUnmapBufferOES`
- Framebuffer / renderbuffer (EXT/OES/ANGLE/NV/APPLE/IMG variants and multisample) (31): `glBindFramebufferEXT` `glBindRenderbufferEXT` `glBlitFramebuffer` `glBlitFramebufferANGLE` `glBlitFramebufferEXT` `glBlitFramebufferNV` `glCheckFramebufferStatusEXT` `glDeleteFramebuffersEXT` `glDeleteRenderbuffersEXT` `glDiscardFramebufferEXT` `glDrawBuffers` `glDrawBuffersARB` `glDrawBuffersNV` `glFramebufferRenderbufferEXT` `glFramebufferTexture2DEXT` `glFramebufferTexture2DMultisampleEXT` `glFramebufferTexture2DMultisampleIMG` `glGenFramebuffersEXT` `glGenRenderbuffersEXT` `glGenerateMipmapEXT` `glInvalidateFramebuffer` `glReadBuffer` `glReadBufferNV` `glRenderbufferStorageEXT` `glRenderbufferStorageMultisample` `glRenderbufferStorageMultisampleANGLE` `glRenderbufferStorageMultisampleAPPLE` `glRenderbufferStorageMultisampleEXT` `glRenderbufferStorageMultisampleIMG` `glRenderbufferStorageMultisampleNV` `glResolveMultisampleFramebufferAPPLE`
- 3D textures and storage (14): `glCompressedTexImage3D` `glCompressedTexImage3DOES` `glCompressedTexSubImage3D` `glCompressedTexSubImage3DOES` `glTexImage3D` `glTexImage3DOES` `glTexStorage1D` `glTexStorage1DEXT` `glTexStorage2D` `glTexStorage2DEXT` `glTexStorage3D` `glTexStorage3DEXT` `glTexSubImage3D` `glTexSubImage3DOES`
- Queries, sync objects, fences (37): `eglClientWaitSyncKHR` `eglCreateSyncKHR` `eglDestroySyncKHR` `eglGetSyncAttribKHR` `glBeginQuery` `glBeginQueryARB` `glBeginQueryEXT` `glClientWaitSync` `glClientWaitSyncAPPLE` `glDeleteFencesAPLLE` `glDeleteFencesNV` `glDeleteQueries` `glDeleteQueriesARB` `glDeleteQueriesEXT` `glDeleteSync` `glDeleteSyncAPPLE` `glEndQuery` `glEndQueryARB` `glEndQueryEXT` `glFenceSync` `glFenceSyncAPPLE` `glFinishFenceAPPLE` `glFinishFenceNV` `glGenFencesAPPLE` `glGenFencesNV` `glGenQueries` `glGenQueriesARB` `glGenQueriesEXT` `glGetQueryObjectuiv` `glGetQueryObjectuivARB` `glGetQueryObjectuivEXT` `glGetSynciv` `glGetSyncivAPPLE` `glSetFenceAPPLE` `glSetFenceNV` `glTestFenceAPPLE` `glTestFenceNV`
- Transform feedback, program binary (20): `glBeginTransformFeedback` `glBeginTransformFeedbackEXT` `glBeginTransformFeedbackNV` `glBindTransformFeedback` `glBindTransformFeedbackNV` `glDeleteTransformFeedbacks` `glDeleteTransformFeedbacksNV` `glEndTransformFeedback` `glEndTransformFeedbackEXT` `glEndTransformFeedbackNV` `glGenTransformFeedbacks` `glGenTransformFeedbacksNV` `glGetProgramBinaryOES` `glGetTransformFeedbackVarying` `glGetTransformFeedbackVaryingEXT` `glGetTransformFeedbackVaryingNV` `glProgramBinaryOES` `glTransformFeedbackVaryings` `glTransformFeedbackVaryingsEXT` `glTransformFeedbackVaryingsNV`
- Other (1): `glAlphaFuncQCOM`

Extension strings tested in `GL_EXTENSIONS` (128 ES-relevant ones found in .rodata; the engine also carries the whole desktop GLEW-style ARB/SGI/NV list, which is dead on ES): GL_OES_vertex_array_object, GL_OES_depth_texture, GL_OES_packed_depth_stencil, GL_OES_texture_float(+_linear), GL_OES_texture_half_float(+_linear), GL_OES_standard_derivatives, GL_OES_element_index_uint, GL_OES_rgb8_rgba8, GL_OES_compressed_ETC1_RGB8_texture, GL_OES_texture_compression_astc, GL_KHR_texture_compression_astc_ldr, GL_IMG_texture_compression_pvrtc(+2), GL_AMD_compressed_ATC_texture, GL_AMD_compressed_3DC_texture, GL_EXT_texture_compression_s3tc/dxt1/latc/rgtc, GL_EXT_texture_filter_anisotropic, GL_EXT_discard_framebuffer, GL_EXT_shader_framebuffer_fetch, GL_EXT_draw_buffers, GL_EXT_color_buffer_float/half_float, GL_EXT_texture_storage, GL_EXT_map_buffer_range, GL_EXT_multisampled_render_to_texture, GL_IMG_multisampled_render_to_texture, GL_NV_draw_buffers, GL_NV_fbo_color_attachments, GL_ANGLE_instanced_arrays, GL_ANGLE_framebuffer_blit/multisample, GL_ARM_mali_program_binary/shader_binary, GL_IMG_program_binary, GL_OES_get_program_binary, GL_AMD_program_binary_Z400, GL_ANGLE_program_binary, GL_NVIDIA_PLATFORM_BINARY_NV, GL_SGX_BINARY_IMG, GL_MALI_SHADER_BINARY_ARM. The shader-info cache logic ("shader info cache", `GL_*_BINARY*` strings) may call glProgramBinaryOES/glGetProgramBinaryOES; report no binary formats so it falls back to source.

## Other bionic-specific points for the loader and the libc layer

- TCB / TLS: see `__stack_chk_fail`. The guest has no `__thread` variables (no PT_TLS); per-thread data is pthread keys only.
- Callbacks from host into guest are required by: qsort, bsearch, pthread_create (start routine), pthread_once, pthread_key_create (destructor), __cxa_atexit, funopen (4 callbacks), dl_iterate_phdr, signal/sigaction, OpenSL buffer-queue callbacks (`vox::DriverAndroid::CallbackOSL`), GL/EGL extension thunks (the reverse direction) and every `JNIEnv`/`JavaVM` table slot (guest calls host).
- Guest-visible object lifetimes: `gethostbyname`, `getaddrinfo`, `glGetString`, `strerror`, `localeconv`, `AAsset_getBuffer`, `ctime`-style statics return pointers to host-owned buffers that must live in guest memory and stay valid (bionic returns static storage).
- Environment/identity returned by libc and sysfs reads that games probe (all can be answered with fixed values): `/proc/cpuinfo`, `/proc/meminfo`, `/proc/self/statm`, `/sys/devices/system/cpu/*`, GPU clock nodes, `/system/build.prop`, `/sys/class/thermal`, `/sys/bus/soc/devices/soc0/family`, `/dev/urandom`, `/system/bin/su` (root check; make `access`/`fopen`/`stat` return ENOENT). These go through the libc file functions above, so the virtual FS can serve them.
- `__cxa_demangle`, `__cxa_guard_*`, `std::__ndk1` are all exported by libNOVA itself; hooking `std::` symbols by name would hit libNOVA code, not host code.
