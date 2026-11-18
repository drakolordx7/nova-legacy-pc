// Guest CPU (dynarmic A64), host-to-guest calls and guest threads. One host thread + one A64::Jit per guest
// thread, all sharing one ExclusiveMonitor and the guest window.
// Adapted from Zombiewood's cpu/cpu (MIT, (c) 2026 ChrisDutary): AAPCS64, TPIDR_EL0, 64-bit addresses.
#pragma once
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "cpu/guest_memory.h"

namespace nova {

// The guest did something unrecoverable (access to unmapped memory, undefined instruction, jump to a
// non-executable page, import without a handler under the Abort policy...). what() carries a register dump.
struct GuestFault : std::runtime_error {
  using std::runtime_error::runtime_error;
};
// Thrown from a (non-FAST) handler to end the guest process (exit/abort).
struct GuestExit {
  int code = 0;
};

using Vec128 = std::array<std::uint64_t, 2>;

// bionic arm64 TLS block layout (TPIDR_EL0 points at slot 0). Slots are 8 bytes.
inline constexpr unsigned kTlsSlotSelf = 0;
inline constexpr unsigned kTlsSlotThreadId = 1;
inline constexpr unsigned kTlsSlotApp = 2;
inline constexpr unsigned kTlsSlotOpenGL = 3;
inline constexpr unsigned kTlsSlotOpenGLApi = 4;
inline constexpr unsigned kTlsSlotStackGuard = 5;  // the compiler's -mstack-protector-guard=sysreg reads [tp + 40]
inline constexpr unsigned kTlsSlotSanitizer = 6;
inline constexpr unsigned kTlsSlotArtThreadSelf = 7;
inline constexpr unsigned kTlsSlotDtv = 8;
inline constexpr unsigned kTlsSlotBionicTls = 9;
inline constexpr gsize kTlsErrnoOffset = 0x80;     // int errno of this thread (what __errno() returns)
inline constexpr gsize kTlsStaticOffset = 0x100;   // start of the static TLS block (PT_TLS image) from TPIDR_EL0

// Global values every thread's TLS block gets.
struct TlsTemplate {
  gaddr image = 0;    // guest address of the initialised part (.tdata)
  gsize filesz = 0;
  gsize memsz = 0;    // total (tdata + tbss)
  gsize align = 16;
};

namespace detail {
struct CpuImpl;

template <class T>
inline constexpr bool is_int_like = (std::is_integral_v<T> || std::is_enum_v<T>) && sizeof(T) <= 8;

// Host-to-guest argument packing (AAPCS64): ints/pointers in x0-x7, float/double in v0-v7, rest on the stack in
// 8-byte slots.
struct ArgPack {
  std::uint64_t x[8] = {};
  std::uint64_t v[8] = {};
  std::uint64_t st[24] = {};
  unsigned nx = 0, nv = 0, ns = 0;

  void PushInt(std::uint64_t u) {
    if (nx < 8) x[nx++] = u;
    else st[ns++] = u;
  }
  void PushFp(std::uint64_t bits) {
    if (nv < 8) v[nv++] = bits;
    else st[ns++] = bits;
  }
  template <class T>
  void Push(const T& a) {
    using D = std::decay_t<T>;
    if constexpr (std::is_same_v<D, bool>) {
      PushInt(a ? 1 : 0);
    } else if constexpr (std::is_same_v<D, float>) {
      PushFp(std::bit_cast<std::uint32_t>(a));
    } else if constexpr (std::is_same_v<D, double>) {
      PushFp(std::bit_cast<std::uint64_t>(a));
    } else if constexpr (std::is_enum_v<D>) {
      PushInt(static_cast<std::uint64_t>(static_cast<std::underlying_type_t<D>>(a)));
    } else if constexpr (std::is_integral_v<D>) {
      static_assert(sizeof(D) <= 8);
      if constexpr (std::is_signed_v<D>) PushInt(static_cast<std::uint64_t>(static_cast<std::int64_t>(a)));
      else PushInt(static_cast<std::uint64_t>(a));
    } else if constexpr (std::is_pointer_v<D>) {
      PushInt(a ? h2g(a) : 0);
    } else if constexpr (std::is_null_pointer_v<D>) {
      PushInt(0);
    } else {
      static_assert(sizeof(D) == 0, "call_guest: unsupported argument type");
    }
  }
};

template <class R>
R UnpackResult(std::uint64_t x0, std::uint64_t v0) {
  if constexpr (std::is_void_v<R>) {
    (void)x0;
    (void)v0;
  } else if constexpr (std::is_same_v<R, bool>) {
    return (x0 & 0xFF) != 0;
  } else if constexpr (std::is_same_v<R, float>) {
    return std::bit_cast<float>(static_cast<std::uint32_t>(v0));
  } else if constexpr (std::is_same_v<R, double>) {
    return std::bit_cast<double>(v0);
  } else if constexpr (std::is_pointer_v<R>) {
    return x0 ? g2h<std::remove_pointer_t<R>>(x0) : nullptr;
  } else if constexpr (is_int_like<R>) {
    return static_cast<R>(x0);
  } else {
    static_assert(sizeof(R) == 0, "call_guest: unsupported return type");
  }
}
}  // namespace detail

// Per-thread slots for HLE modules (opaque host pointers, initially null).
enum class UserSlot { Libc, Pthread, Gles, Audio, Jni, Frontend, Spare0, Spare1, Count };

// State of one guest thread. Exactly one per host thread that runs guest code; use it only from that thread.
class Cpu {
 public:
  // The Cpu of the calling host thread, attached on first use with a main-thread sized stack.
  static Cpu& Current();
  static Cpu* CurrentOrNull();

  // --- registers (valid between Run calls, i.e. in handlers and between call_guest calls) ---
  std::uint64_t x(unsigned i) const;
  void set_x(unsigned i, std::uint64_t v);
  std::uint64_t sp() const;
  void set_sp(std::uint64_t v);
  std::uint64_t pc() const;
  void set_pc(std::uint64_t v);
  std::uint64_t lr() const { return x(30); }
  Vec128 v(unsigned i) const;
  void set_v(unsigned i, Vec128 val);
  double d(unsigned i) const { return std::bit_cast<double>(v(i)[0]); }
  float s(unsigned i) const { return std::bit_cast<float>(static_cast<std::uint32_t>(v(i)[0])); }
  void set_d(unsigned i, double val) { set_v(i, {std::bit_cast<std::uint64_t>(val), 0}); }
  void set_s(unsigned i, float val) { set_v(i, {std::bit_cast<std::uint32_t>(val), 0}); }
  std::uint32_t nzcv() const;
  std::uint32_t fpcr() const;
  void set_fpcr(std::uint32_t v);

  // AAPCS64: stack-passed argument n (0-based, after the registers) of the import being handled.
  std::uint64_t stack_arg(unsigned n) const { return mem::R64(sp() + 8ull * n); }

  std::uint64_t tpidr() const { return tpidr_el0_; }
  void set_tpidr(std::uint64_t v) { tpidr_el0_ = v; }

  // --- identity / per-thread data ---
  std::uint32_t thread_id() const { return thread_id_; }  // != 0, unique among live threads
  const std::string& name() const { return name_; }
  gaddr tls() const { return tls_; }  // TLS block (initial TPIDR_EL0)
  gaddr errno_addr() const { return tls_ + kTlsErrnoOffset; }
  gaddr stack_base() const { return stack_base_; }  // lowest usable stack address
  gsize stack_size() const { return stack_size_; }
  void*& user(UserSlot s) { return user_[static_cast<int>(s)]; }
  // Name of the import whose handler is running ("" if none).
  const char* current_import() const { return cur_import_; }
  // Nesting of host-to-guest calls (0 = no guest code running on this thread).
  int call_depth() const { return depth_; }

  // --- host -> guest ---
  struct Result {
    std::uint64_t x0 = 0, x1 = 0;
    Vec128 v0{};
  };
  // Calls guest function `fn` with packed arguments. Re-entrant: usable from a non-FAST handler, nesting on
  // the current guest stack. Preserves every guest register of the interrupted code. Throws GuestFault/GuestExit.
  Result CallPacked(gaddr fn, const detail::ArgPack& args);

  template <class R = std::uint64_t, class... A>
  R Call(gaddr fn, const A&... args) {
    detail::ArgPack p;
    (p.Push(args), ...);
    const Result r = CallPacked(fn, p);
    return detail::UnpackResult<R>(r.x0, r.v0[0]);
  }

  Cpu(const Cpu&) = delete;
  Cpu& operator=(const Cpu&) = delete;
  ~Cpu();

 private:
  friend struct detail::CpuImpl;
  Cpu();
  std::uint64_t tpidr_el0_ = 0;
  std::uint32_t thread_id_ = 0;
  std::string name_;
  gaddr tls_ = 0, stack_base_ = 0;
  gsize stack_size_ = 0;
  int depth_ = 0;
  const char* cur_import_ = "";
  void* user_[static_cast<int>(UserSlot::Count)] = {};
  std::unique_ptr<detail::CpuImpl> impl_;
};

// Calls a guest function from the host on the current thread (see Cpu::Call).
//   uint64_t r = nova::call_guest<uint64_t>(fn, a, b);  // ints/pointers -> x regs, float/double -> v regs
template <class R = std::uint64_t, class... A>
inline R call_guest(gaddr fn, const A&... args) {
  return Cpu::Current().template Call<R>(fn, args...);
}

// ---------------------------------------------------------------------------
namespace threads {
inline constexpr gsize kDefaultStackSize = 1u << 20;       // 1 MiB
inline constexpr gsize kMainStackSize = 8u << 20;          // implicitly attached host threads
inline constexpr gsize kMinStackSize = 128u << 10;
inline constexpr std::size_t kMaxThreads = 128;            // live guest threads at once

using ThreadId = std::uint32_t;  // == Cpu::thread_id(); 0 = invalid

struct CreateParams {
  gaddr entry = 0;  // void* (*)(void*) in the guest
  gaddr arg = 0;
  gsize stack_size = 0;  // 0 = kDefaultStackSize
  bool detached = false;
  std::string name;
  // Called on the new thread after its Cpu exists and before `entry` runs (set TPIDR_EL0 here, etc.).
  std::function<void(Cpu&)> on_start;
};

// Starts a host thread with its own Cpu/Jit running entry(arg). Returns 0 on failure.
ThreadId Create(const CreateParams& p);
// Waits for the thread and collects its return value. Returns false if the id is unknown/detached/already joined.
// If the guest thread died of a fault, *fault gets the message (empty otherwise).
bool Join(ThreadId id, std::uint64_t* retval = nullptr, std::string* fault = nullptr);
bool Detach(ThreadId id);
bool IsAlive(ThreadId id);
ThreadId CurrentId();
std::size_t Count();

// Hooks that run ON the guest thread: at start (before entry) and at the end (before the Cpu is destroyed; pthread
// key destructors go here). Register before creating threads.
void AddStartHook(std::function<void(Cpu&)> fn);
void AddExitHook(std::function<void(Cpu&)> fn);
// Runs the exit hooks and destroys the Cpu of an implicitly attached host thread.
void DetachCurrent();
}  // namespace threads

// ---------------------------------------------------------------------------
namespace cpu {
// Drops translated code for [addr, addr+size) in every thread (after patching guest code).
void InvalidateCode(gaddr addr, gsize size);
// What to do when a thread made by threads::Create dies of a fault / calls exit. Default: log and _Exit.
void SetFatalHandler(std::function<void(const std::string& what)> fn);
void SetExitHandler(std::function<void(int code)> fn);
// Static TLS image (PT_TLS) copied into every new thread's TLS block. Set before creating threads.
void SetTlsTemplate(const TlsTemplate& t);
const TlsTemplate& GetTlsTemplate();
// Value of the stack-guard canary (TLS slot 5 and __stack_chk_guard).
std::uint64_t StackGuardValue();
// Number of guest memory accesses that went through the slow-path callbacks instead of fastmem (diagnostics).
std::uint64_t SlowPathAccesses();
}  // namespace cpu

// ---------------------------------------------------------------------------
namespace debug {
// Resolves a guest address to "lib!symbol+0xNN". The loader installs a symbolizer.
using Symbolizer = std::function<bool(gaddr addr, std::string& out)>;
void AddSymbolizer(Symbolizer fn);
std::string addr2sym(gaddr addr);
// Return addresses found by walking the frame-pointer chain (x29), [0] = pc, [1] = lr.
std::vector<gaddr> Backtrace(const Cpu& cpu, std::size_t max_frames = 32);
// Registers + symbolised backtrace, ready for a log.
std::string DumpState(const Cpu& cpu);
// Import call tracing (also NOVA_TRACE_IMPORTS=1; NOVA_TRACE_FILTER=substring).
void SetTraceImports(bool on);
bool TraceImports();
}  // namespace debug

}  // namespace nova
