#include "cpu/cpu.h"

#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <thread>

#include "core/log.h"
#include "cpu/hle.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/exclusive_monitor.h"

namespace nova {
namespace {
constexpr std::uint32_t kNoSvc = 0xFFFFFFFFu;

Dynarmic::ExclusiveMonitor& Monitor() {
  static Dynarmic::ExclusiveMonitor m(threads::kMaxThreads);
  return m;
}

std::mutex g_cpus_mu;
detail::CpuImpl* g_slots[threads::kMaxThreads] = {};
std::atomic<std::uint32_t> g_next_tid{1};
std::atomic<std::size_t> g_live{0};

std::mutex g_hooks_mu;
std::vector<std::function<void(Cpu&)>> g_start_hooks, g_exit_hooks;
std::function<void(const std::string&)> g_fatal;
std::function<void(int)> g_exit;
TlsTemplate g_tls_tmpl;

std::atomic<std::uint64_t> g_slow_accesses{0};

std::mutex g_sym_mu;
std::vector<debug::Symbolizer> g_symbolizers;
std::atomic<int> g_trace{-1};
std::string g_trace_filter;

bool EnvFlag(const char* name, bool def) {
  const char* e = std::getenv(name);
  if (!e || !*e) return def;
  return *e != '0';
}

bool TraceMatches(const std::string& name) {
  return g_trace_filter.empty() || name.find(g_trace_filter) != std::string::npos;
}

std::uint64_t QpcNow() {
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return static_cast<std::uint64_t>(c.QuadPart);
}

// AArch64 top-byte-ignore: Android userland may hand around tagged pointers. Only the slow path sees them (the
// fastmem window check rejects any address >= 1<<40), so strip the tag there.
constexpr gaddr Strip(gaddr a) { return (a >> 56) ? (a & ((gaddr{1} << 56) - 1)) : a; }
}  // namespace

namespace cpu {
std::uint64_t StackGuardValue() {
  static const std::uint64_t v = [] {
    std::random_device rd;
    std::uint64_t r = (static_cast<std::uint64_t>(rd()) << 32) | rd();
    return (r | 0x100) & ~0xFFull;  // low byte zero like bionic's canary, never 0
  }();
  return v;
}
std::uint64_t SlowPathAccesses() { return g_slow_accesses.load(); }
void SetTlsTemplate(const TlsTemplate& t) { g_tls_tmpl = t; }
const TlsTemplate& GetTlsTemplate() { return g_tls_tmpl; }
}  // namespace cpu

// ---------------------------------------------------------------------------
struct detail::CpuImpl final : Dynarmic::A64::UserCallbacks {
  using HR = Dynarmic::HaltReason;
  Cpu* cpu = nullptr;
  std::unique_ptr<Dynarmic::A64::Jit> jit;
  std::size_t slot = 0;
  std::uint32_t pending_svc = kNoSvc;
  bool fault_pending = false;
  bool in_fast = false;
  std::string fault_msg;
  std::optional<gaddr> fault_pc;
  gaddr stack_map = 0, tls_map = 0;
  gsize stack_map_size = 0, tls_map_size = 0;
  std::uint64_t tpidrro_el0 = 0;

  static Cpu* Create(gsize stack_size, std::string name);
  static void Destroy(Cpu* c);

  void Fault(std::string msg, bool mem) {
    if (!fault_pending) {
      fault_pending = true;
      fault_msg = std::move(msg);
    }
    jit->HaltExecution(mem ? HR::MemoryAbort : HR::UserDefined2);
  }

  // --- memory (slow path: faults, tagged/out-of-window pointers, page crossings never reach here with fastmem) ---
  template <class T>
  T Rd(gaddr a) {
    g_slow_accesses.fetch_add(1, std::memory_order_relaxed);
    a = Strip(a);
    if (!GuestMemory::RangeHas(a, sizeof(T), kProtRead)) {
      Fault(std::format("read of {} bytes from {} guest address 0x{:x}", sizeof(T),
                        GuestMemory::IsMapped(a) ? "non-readable" : "unmapped", a),
            true);
      return T{};
    }
    T v;
    std::memcpy(&v, g2h(a), sizeof(T));
    return v;
  }
  template <class T>
  void Wr(gaddr a, T v) {
    g_slow_accesses.fetch_add(1, std::memory_order_relaxed);
    a = Strip(a);
    if (!GuestMemory::RangeHas(a, sizeof(T), kProtWrite)) {
      Fault(std::format("write of {} bytes to {} guest address 0x{:x}", sizeof(T),
                        GuestMemory::IsMapped(a) ? "read-only" : "unmapped", a),
            true);
      return;
    }
    std::memcpy(g2h(a), &v, sizeof(T));
  }
  std::optional<std::uint32_t> MemoryReadCode(gaddr a) override {
    if (a >= kWindowSize || !(GuestMemory::PageProt(a) & kProtExec)) return std::nullopt;
    std::uint32_t v;
    std::memcpy(&v, g2h(a), 4);
    return v;
  }
  std::uint8_t MemoryRead8(gaddr a) override { return Rd<std::uint8_t>(a); }
  std::uint16_t MemoryRead16(gaddr a) override { return Rd<std::uint16_t>(a); }
  std::uint32_t MemoryRead32(gaddr a) override { return Rd<std::uint32_t>(a); }
  std::uint64_t MemoryRead64(gaddr a) override { return Rd<std::uint64_t>(a); }
  Dynarmic::A64::Vector MemoryRead128(gaddr a) override { return Rd<Dynarmic::A64::Vector>(a); }
  void MemoryWrite8(gaddr a, std::uint8_t v) override { Wr(a, v); }
  void MemoryWrite16(gaddr a, std::uint16_t v) override { Wr(a, v); }
  void MemoryWrite32(gaddr a, std::uint32_t v) override { Wr(a, v); }
  void MemoryWrite64(gaddr a, std::uint64_t v) override { Wr(a, v); }
  void MemoryWrite128(gaddr a, Dynarmic::A64::Vector v) override { Wr(a, v); }

  bool ExclOk(gaddr& a, std::size_t n) {
    g_slow_accesses.fetch_add(1, std::memory_order_relaxed);
    a = Strip(a);
    if (GuestMemory::RangeHas(a, n, kProtWrite)) return true;
    Fault(std::format("exclusive store to non-writable guest address 0x{:x}", a), true);
    return false;
  }
  bool MemoryWriteExclusive8(gaddr a, std::uint8_t v, std::uint8_t e) override {
    return ExclOk(a, 1) && _InterlockedCompareExchange8(reinterpret_cast<char*>(g2h(a)), char(v), char(e)) == char(e);
  }
  bool MemoryWriteExclusive16(gaddr a, std::uint16_t v, std::uint16_t e) override {
    return ExclOk(a, 2) && _InterlockedCompareExchange16(reinterpret_cast<short*>(g2h(a)), short(v), short(e)) == short(e);
  }
  bool MemoryWriteExclusive32(gaddr a, std::uint32_t v, std::uint32_t e) override {
    return ExclOk(a, 4) &&
           _InterlockedCompareExchange(reinterpret_cast<volatile long*>(g2h(a)), long(v), long(e)) == long(e);
  }
  bool MemoryWriteExclusive64(gaddr a, std::uint64_t v, std::uint64_t e) override {
    return ExclOk(a, 8) && _InterlockedCompareExchange64(reinterpret_cast<volatile long long*>(g2h(a)), (long long)v,
                                                           (long long)e) == (long long)e;
  }
  bool MemoryWriteExclusive128(gaddr a, Dynarmic::A64::Vector v, Dynarmic::A64::Vector e) override {
    if (!ExclOk(a, 16)) return false;
    long long cmp[2] = {(long long)e[0], (long long)e[1]};
    return _InterlockedCompareExchange128(reinterpret_cast<volatile long long*>(g2h(a)), (long long)v[1], (long long)v[0],
                                          cmp) != 0;
  }

  void InterpreterFallback(gaddr pc, std::size_t n) override {
    fault_pc = pc;
    Fault(std::format("instruction at 0x{:x} ({} instr.) needs the interpreter, which is not available", pc, n), false);
  }
  void ExceptionRaised(gaddr pc, Dynarmic::A64::Exception ex) override {
    using E = Dynarmic::A64::Exception;
    const char* what = nullptr;
    switch (ex) {
      case E::UnallocatedEncoding: what = "unallocated instruction encoding"; break;
      case E::ReservedValue: what = "reserved value in instruction"; break;
      case E::UnpredictableInstruction: what = "unpredictable instruction"; break;
      case E::Breakpoint: what = "brk"; break;
      case E::NoExecuteFault: what = "execution of unmapped or non-executable memory"; break;
      default: return;  // WFI/WFE/SEV/SEVL/YIELD hints: ignore
    }
    fault_pc = pc;
    Fault(std::format("{} at pc = 0x{:x} ({})", what, pc, debug::addr2sym(pc)), false);
  }
  void AddTicks(std::uint64_t) override {}
  std::uint64_t GetTicksRemaining() override { return 1ull << 30; }
  std::uint64_t GetCNTPCT() override { return QpcNow(); }

  void CallSVC(std::uint32_t swi) override {
    if (swi != 0) {
      hle::Entry* e = hle::EntryByIndex(swi);
      if (e && (e->flags.load(std::memory_order_relaxed) & hle::kFast) && g_trace.load(std::memory_order_relaxed) <= 0) {
        if (hle::RawFn fn = e->fn.load(std::memory_order_relaxed)) {
          e->calls.fetch_add(1, std::memory_order_relaxed);
          const char* prev = cpu->cur_import_;
          cpu->cur_import_ = e->name.c_str();
          in_fast = true;
          fn(*cpu);
          in_fast = false;
          cpu->cur_import_ = prev;
          return;
        }
      }
    }
    pending_svc = swi;
    jit->HaltExecution(HR::UserDefined1);
  }

  void Dispatch(std::uint32_t swi) {
    hle::Entry* e = hle::EntryByIndex(swi);
    if (!e) throw GuestFault(std::format("svc #{} is not a known import\n{}", swi, debug::DumpState(*cpu)));
    e->calls.fetch_add(1, std::memory_order_relaxed);
    hle::RawFn fn = e->fn.load();
    if (!fn) {
      if (hle::UnimplementedPolicy() == hle::OnUnimplemented::Abort)
        throw GuestFault(std::format("import without a handler: {} (lr = {})\n{}", e->name,
                                     debug::addr2sym(cpu->lr()), debug::DumpState(*cpu)));
      if (!e->warned.exchange(true))
        log::Warn("unimplemented import: {} (lr = {}) -> returns 0", e->name, debug::addr2sym(cpu->lr()));
      jit->SetRegister(0, 0);
      return;
    }
    const bool trace = g_trace.load(std::memory_order_relaxed) > 0 && TraceMatches(e->name);
    if (trace)
      log::Info("[t{}] {}(0x{:x}, 0x{:x}, 0x{:x}, 0x{:x}) lr={}", cpu->thread_id_, e->name, cpu->x(0), cpu->x(1), cpu->x(2),
                cpu->x(3), debug::addr2sym(cpu->lr()));
    const char* prev = cpu->cur_import_;
    cpu->cur_import_ = e->name.c_str();
    struct Restore {
      Cpu* c;
      const char* p;
      ~Restore() { c->cur_import_ = p; }
    } restore{cpu, prev};
    fn(*cpu);
    if (trace) log::Info("[t{}] {} -> 0x{:x}", cpu->thread_id_, e->name, cpu->x(0));
  }

  // Runs until the guest returns to the return stub of the current call level.
  void RunUntilReturn() {
    for (;;) {
      pending_svc = kNoSvc;
      jit->Run();
      if (fault_pending) {
        fault_pending = false;
        jit->ClearHalt(HR::MemoryAbort);
        jit->ClearHalt(HR::UserDefined2);
        std::string msg = std::move(fault_msg);
        if (fault_pc) {
          jit->SetPC(*fault_pc);
          fault_pc.reset();
        }
        msg += "\n" + debug::DumpState(*cpu);
        log::Debug("guest fault: {}", msg);
        throw GuestFault(msg);
      }
      const std::uint32_t svc = pending_svc;
      if (svc == kNoSvc) continue;  // cache invalidation or another spurious stop
      if (svc == 0) return;
      Dispatch(svc);
    }
  }
};

// ---------------------------------------------------------------------------
namespace {
thread_local Cpu* t_cpu = nullptr;
struct TlsHolder {
  ~TlsHolder() {
    if (t_cpu) {
      detail::CpuImpl::Destroy(t_cpu);
      t_cpu = nullptr;
    }
  }
};
thread_local TlsHolder t_holder;

LPTOP_LEVEL_EXCEPTION_FILTER g_prev_crash_filter = nullptr;

// A host-side access violation inside window memory is almost always an HLE handler dereferencing a bad guest
// pointer: say which guest address and which import.
LONG WINAPI HostCrashFilter(EXCEPTION_POINTERS* ep) {
  struct Chain {
    EXCEPTION_POINTERS* ep;
    ~Chain() {
      if (g_prev_crash_filter) g_prev_crash_filter(ep);
    }
  } chain{ep};
  static std::atomic<bool> busy{false};
  if (ep && ep->ExceptionRecord && ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && !busy.exchange(true)) {
    const auto addr = static_cast<std::uintptr_t>(ep->ExceptionRecord->ExceptionInformation[1]);
    const auto* p = reinterpret_cast<const std::uint8_t*>(addr);
    std::string msg = std::format("host access violation at {} (rip = {})", static_cast<const void*>(p),
                                  ep->ExceptionRecord->ExceptionAddress);
    if (GuestMemory::Contains(p)) msg += std::format(" = guest address 0x{:x}", static_cast<gaddr>(p - g_membase));
    if (t_cpu) {
      msg += std::format("\nimport in progress: '{}'\n", t_cpu->current_import());
      msg += debug::DumpState(*t_cpu);
    }
    log::Error("{}", msg);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

void RunHooks(std::vector<std::function<void(Cpu&)>>& list, Cpu& c) {
  std::vector<std::function<void(Cpu&)>> copy;
  {
    std::lock_guard lk(g_hooks_mu);
    copy = list;
  }
  for (auto& f : copy) f(c);
}
}  // namespace

Cpu::Cpu() = default;
Cpu::~Cpu() = default;

Cpu* detail::CpuImpl::Create(gsize stack_size, std::string name) {
  static std::once_flag once;
  std::call_once(once, [] {
    GuestMemory::Init();
    hle::ReturnStub();
    g_prev_crash_filter = SetUnhandledExceptionFilter(HostCrashFilter);
    if (g_trace.load() < 0) {
      if (const char* f = std::getenv("NOVA_TRACE_FILTER")) g_trace_filter = f;
      g_trace.store(EnvFlag("NOVA_TRACE_IMPORTS", false) ? 1 : 0);
    }
  });
  if (!g_membase) return nullptr;

  auto* c = new Cpu();
  c->impl_ = std::make_unique<CpuImpl>();
  CpuImpl& I = *c->impl_;
  I.cpu = c;
  {
    std::lock_guard lk(g_cpus_mu);
    std::size_t s = 0;
    while (s < threads::kMaxThreads && g_slots[s]) ++s;
    if (s == threads::kMaxThreads) {
      log::Error("Cpu: too many guest threads (max {})", threads::kMaxThreads);
      delete c;
      return nullptr;
    }
    g_slots[s] = &I;
    I.slot = s;
  }
  c->thread_id_ = g_next_tid.fetch_add(1);
  c->name_ = name.empty() ? std::format("thread{}", c->thread_id_) : std::move(name);

  if (stack_size == 0) stack_size = threads::kDefaultStackSize;
  if (stack_size < threads::kMinStackSize) stack_size = threads::kMinStackSize;
  stack_size = (stack_size + 0xFFF) & ~0xFFFull;
  I.stack_map_size = stack_size + kPageSize;  // + guard page
  I.stack_map = GuestMemory::MapAnywhere(Zone::Stacks, I.stack_map_size, kProtRW, "stack", kPageSize);
  const TlsTemplate& tt = g_tls_tmpl;
  I.tls_map_size = (kTlsStaticOffset + tt.memsz + tt.align + 0xFFF) & ~0xFFFull;
  I.tls_map = GuestMemory::MapAnywhere(Zone::Stacks, I.tls_map_size, kProtRW, "tls", kPageSize);
  if (!I.stack_map || !I.tls_map) {
    log::Error("Cpu: out of guest memory for the thread stack/TLS");
    Destroy(c);
    return nullptr;
  }
  GuestMemory::Protect(I.stack_map, kPageSize, kProtNone);
  c->stack_base_ = I.stack_map + kPageSize;
  c->stack_size_ = stack_size;
  c->tls_ = I.tls_map;
  mem::W64(c->tls_ + 8 * kTlsSlotSelf, c->tls_);
  mem::W64(c->tls_ + 8 * kTlsSlotThreadId, c->thread_id_);
  mem::W64(c->tls_ + 8 * kTlsSlotStackGuard, cpu::StackGuardValue());
  if (tt.memsz) {
    if (tt.filesz) std::memcpy(g2h(c->tls_ + kTlsStaticOffset), g2h(tt.image), tt.filesz);
  }
  c->tpidr_el0_ = c->tls_;

  Dynarmic::A64::UserConfig cfg;
  cfg.callbacks = &I;
  cfg.processor_id = I.slot;
  cfg.global_monitor = &Monitor();
  cfg.tpidrro_el0 = &I.tpidrro_el0;
  cfg.tpidr_el0 = &c->tpidr_el0_;
  // Offset window: fastmem over the reserved window; anything outside 1<<40 or on an uncommitted page traps into
  // the memory callbacks above and is reported (needs the shadow-space patch from cmake/NovaDependencies.cmake).
  cfg.fastmem_pointer = reinterpret_cast<std::uintptr_t>(g_membase);
  cfg.fastmem_address_space_bits = kWindowBits;
  cfg.silently_mirror_fastmem = false;
  cfg.recompile_on_fastmem_failure = true;
  cfg.fastmem_exclusive_access = EnvFlag("NOVA_FASTMEM_EXCLUSIVE", true);
  cfg.recompile_on_exclusive_fastmem_failure = true;
  // Precise faults (off by default): halt right after a faulting access so the reported pc is the faulting
  // instruction. It costs a halt test after every guest memory access and disables get/set elimination, i.e. bigger
  // IR, slower translation and slower guest code. NOVA_PRECISE_FAULTS=1 turns it on when debugging a fault.
  cfg.check_halt_on_memory_access = EnvFlag("NOVA_PRECISE_FAULTS", false);
  // Every safe optimisation (block linking, return stack buffer, fast dispatch, constant propagation, misc IR opts).
  // A64 get/set elimination is left off: with it on (it only runs when precise faults are off) this dynarmic pin
  // asserts "value doesn't support the GetNZCVFromOp pseudo-op" while translating the game's loading code.
  // NOVA_GETSET_ELIM=1 re-enables it. No unsafe optimisations: FMA/NaN/FPCR accuracy matters for the game's float
  // code and the global monitor is needed for exclusives.
  cfg.optimizations = Dynarmic::all_safe_optimizations;
  if (!EnvFlag("NOVA_GETSET_ELIM", false)) cfg.optimizations &= ~Dynarmic::OptimizationFlag::GetSetElimination;
  cfg.unsafe_optimizations = false;
  cfg.define_unpredictable_behaviour = true;
  cfg.wall_clock_cntpct = true;
  cfg.enable_cycle_counting = false;
  LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
  cfg.cntfrq_el0 = static_cast<std::uint32_t>(freq.QuadPart);
  std::size_t mb = 64;
  if (const char* e = std::getenv("NOVA_JIT_CACHE_MB")) mb = std::max(16, std::atoi(e));
  cfg.code_cache_size = mb << 20;
  I.jit = std::make_unique<Dynarmic::A64::Jit>(cfg);
  I.jit->SetSP(c->stack_base_ + c->stack_size_);
  I.jit->SetFpcr(0);
  g_live.fetch_add(1);
  return c;
}

void detail::CpuImpl::Destroy(Cpu* c) {
  if (!c) return;
  CpuImpl& I = *c->impl_;
  {
    std::lock_guard lk(g_cpus_mu);
    if (g_slots[I.slot] == &I) g_slots[I.slot] = nullptr;
  }
  if (I.jit) {
    I.jit.reset();
    g_live.fetch_sub(1);
  }
  Monitor().ClearProcessor(I.slot);
  if (I.stack_map) GuestMemory::Unmap(I.stack_map, I.stack_map_size);
  if (I.tls_map) GuestMemory::Unmap(I.tls_map, I.tls_map_size);
  delete c;
}

Cpu* Cpu::CurrentOrNull() { return t_cpu; }

Cpu& Cpu::Current() {
  if (!t_cpu) {
    (void)t_holder;  // forces the holder's construction on this thread
    t_cpu = detail::CpuImpl::Create(threads::kMainStackSize, {});
    if (!t_cpu) {
      log::Error("Cpu::Current: could not create the thread's Cpu");
      std::abort();
    }
    RunHooks(g_start_hooks, *t_cpu);
  }
  return *t_cpu;
}

std::uint64_t Cpu::x(unsigned i) const { return i >= 31 ? impl_->jit->GetSP() : impl_->jit->GetRegister(i); }
void Cpu::set_x(unsigned i, std::uint64_t v) {
  if (i >= 31) impl_->jit->SetSP(v);
  else impl_->jit->SetRegister(i, v);
}
std::uint64_t Cpu::sp() const { return impl_->jit->GetSP(); }
void Cpu::set_sp(std::uint64_t v) { impl_->jit->SetSP(v); }
std::uint64_t Cpu::pc() const { return impl_->jit->GetPC(); }
void Cpu::set_pc(std::uint64_t v) { impl_->jit->SetPC(v); }
Vec128 Cpu::v(unsigned i) const { return impl_->jit->GetVector(i); }
void Cpu::set_v(unsigned i, Vec128 val) { impl_->jit->SetVector(i, val); }
std::uint32_t Cpu::nzcv() const { return impl_->jit->GetPstate() & 0xF0000000u; }
std::uint32_t Cpu::fpcr() const { return impl_->jit->GetFpcr(); }
void Cpu::set_fpcr(std::uint32_t v) { impl_->jit->SetFpcr(v); }

Cpu::Result Cpu::CallPacked(gaddr fn, const detail::ArgPack& args) {
  detail::CpuImpl& I = *impl_;
  if (I.in_fast) {
    log::Error("call_guest invoked from the FAST handler '{}': register it as a normal handler", cur_import_);
    std::abort();
  }
  if (!fn) throw GuestFault(std::format("call_guest(NULL)\n{}", debug::DumpState(*this)));

  Dynarmic::A64::Jit& J = *I.jit;
  struct Saved {
    std::array<std::uint64_t, 31> regs;
    std::array<Dynarmic::A64::Vector, 32> vecs;
    std::uint64_t sp, pc;
    std::uint32_t pstate, fpcr, fpsr;
  } saved{J.GetRegisters(), J.GetVectors(), J.GetSP(), J.GetPC(), J.GetPstate(), J.GetFpcr(), J.GetFpsr()};
  const char* saved_import = cur_import_;

  std::uint64_t sp = depth_ == 0 ? stack_base_ + stack_size_ : saved.sp;
  sp &= ~0xFull;
  const unsigned nstack = args.ns;
  sp -= (8ull * nstack + 15) & ~0xFull;
  for (unsigned i = 0; i < nstack; ++i) mem::W64(sp + 8ull * i, args.st[i]);
  for (unsigned i = 0; i < 8; ++i) J.SetRegister(i, args.x[i]);
  for (unsigned i = 0; i < 8; ++i) J.SetVector(i, {args.v[i], 0});
  J.SetRegister(30, hle::ReturnStub());
  J.SetSP(sp);
  J.SetPC(fn);

  ++depth_;
  cur_import_ = "";
  Result res;
  auto restore = [&] {
    J.SetRegisters(saved.regs);
    J.SetVectors(saved.vecs);
    J.SetSP(saved.sp);
    J.SetPC(saved.pc);
    J.SetPstate(saved.pstate);
    J.SetFpcr(saved.fpcr);
    J.SetFpsr(saved.fpsr);
    --depth_;
    cur_import_ = saved_import;
  };
  try {
    I.RunUntilReturn();
  } catch (...) {
    restore();
    throw;
  }
  res.x0 = J.GetRegister(0);
  res.x1 = J.GetRegister(1);
  res.v0 = J.GetVector(0);
  restore();
  return res;
}

// ---------------------------------------------------------------------------
namespace threads {
namespace {
struct Rec {
  std::thread th;
  bool detached = false;
  std::atomic<bool> finished{false};
  std::uint64_t ret = 0;
  std::string fault;
};
std::mutex g_threads_mu;
std::map<ThreadId, std::shared_ptr<Rec>> g_threads;

void DefaultFatal(const std::string& what) {
  log::Error("guest thread died of a fault; terminating the process.\n{}", what);
  std::_Exit(70);
}
}  // namespace

ThreadId Create(const CreateParams& p) {
  if (!p.entry) return 0;
  auto rec = std::make_shared<Rec>();
  rec->detached = p.detached;
  std::promise<ThreadId> started;
  auto fut = started.get_future();
  rec->th = std::thread([rec, p, pr = std::move(started)]() mutable {
    (void)t_holder;
    Cpu* c = detail::CpuImpl::Create(p.stack_size, p.name);
    if (!c) {
      pr.set_value(0);
      return;
    }
    t_cpu = c;
    const ThreadId id = c->thread_id();
    {
      std::lock_guard lk(g_threads_mu);  // register before telling the creator, so Join(id) finds it
      g_threads[id] = rec;
    }
    pr.set_value(id);
    try {
      RunHooks(g_start_hooks, *c);
      if (p.on_start) p.on_start(*c);
      rec->ret = c->Call<std::uint64_t>(p.entry, p.arg);
      RunHooks(g_exit_hooks, *c);
    } catch (const GuestFault& f) {
      rec->fault = f.what();
      if (g_fatal) g_fatal(f.what());
      else if (p.detached) DefaultFatal(f.what());
    } catch (const GuestExit& e) {
      if (g_exit) g_exit(e.code);
      else std::_Exit(e.code);
    }
    t_cpu = nullptr;
    detail::CpuImpl::Destroy(c);
    rec->finished.store(true);
    if (rec->detached) {
      std::lock_guard lk(g_threads_mu);
      auto it = g_threads.find(id);
      if (it != g_threads.end() && it->second->detached) g_threads.erase(it);
    }
  });
  const ThreadId id = fut.get();
  if (!id) {
    rec->th.join();
    return 0;
  }
  if (p.detached) rec->th.detach();
  return id;
}

bool Join(ThreadId id, std::uint64_t* retval, std::string* fault) {
  std::shared_ptr<Rec> rec;
  {
    std::lock_guard lk(g_threads_mu);
    auto it = g_threads.find(id);
    if (it == g_threads.end() || it->second->detached) return false;
    rec = it->second;
    g_threads.erase(it);
  }
  if (rec->th.joinable()) rec->th.join();
  if (retval) *retval = rec->ret;
  if (fault) *fault = rec->fault;
  return true;
}

bool Detach(ThreadId id) {
  std::lock_guard lk(g_threads_mu);
  auto it = g_threads.find(id);
  if (it == g_threads.end() || it->second->detached) return false;
  it->second->detached = true;
  if (it->second->th.joinable()) it->second->th.detach();
  if (it->second->finished.load()) g_threads.erase(it);
  return true;
}

bool IsAlive(ThreadId id) {
  std::lock_guard lk(g_threads_mu);
  auto it = g_threads.find(id);
  return it != g_threads.end() && !it->second->finished.load();
}

ThreadId CurrentId() { return Cpu::Current().thread_id(); }
std::size_t Count() { return g_live.load(); }

void AddStartHook(std::function<void(Cpu&)> fn) {
  std::lock_guard lk(g_hooks_mu);
  g_start_hooks.push_back(std::move(fn));
}
void AddExitHook(std::function<void(Cpu&)> fn) {
  std::lock_guard lk(g_hooks_mu);
  g_exit_hooks.push_back(std::move(fn));
}
void DetachCurrent() {
  if (!t_cpu || t_cpu->call_depth() != 0) return;
  RunHooks(g_exit_hooks, *t_cpu);
  detail::CpuImpl::Destroy(t_cpu);
  t_cpu = nullptr;
}
}  // namespace threads

// ---------------------------------------------------------------------------
namespace cpu {
void InvalidateCode(gaddr addr, gsize size) {
  std::lock_guard lk(g_cpus_mu);
  for (auto* I : g_slots)
    if (I && I->jit) I->jit->InvalidateCacheRange(addr, size);
}
void SetFatalHandler(std::function<void(const std::string&)> fn) { g_fatal = std::move(fn); }
void SetExitHandler(std::function<void(int)> fn) { g_exit = std::move(fn); }
}  // namespace cpu

// ---------------------------------------------------------------------------
namespace debug {
void AddSymbolizer(Symbolizer fn) {
  std::lock_guard lk(g_sym_mu);
  g_symbolizers.push_back(std::move(fn));
}

std::string addr2sym(gaddr addr) {
  std::vector<Symbolizer> syms;
  {
    std::lock_guard lk(g_sym_mu);
    syms = g_symbolizers;
  }
  std::string out;
  for (auto& s : syms)
    if (s(addr, out)) return out;
  const std::string stub = hle::StubName(addr);
  if (!stub.empty()) return "hle!" + stub;
  const std::string r = GuestMemory::Describe(addr);
  return r.empty() ? std::format("0x{:x}", addr) : std::format("0x{:x} ({})", addr, r);
}

std::vector<gaddr> Backtrace(const Cpu& cpu, std::size_t max_frames) {
  std::vector<gaddr> out{cpu.pc(), cpu.lr()};
  gaddr fp = cpu.x(29);
  while (out.size() < max_frames && fp && !(fp & 7) && GuestMemory::RangeHas(fp, 16, kProtRead)) {
    const gaddr next = mem::R64(fp), ret = mem::R64(fp + 8);
    if (!ret) break;
    out.push_back(ret);
    if (next <= fp) break;
    fp = next;
  }
  return out;
}

std::string DumpState(const Cpu& cpu) {
  std::string s = std::format("  pc = 0x{:016x} ({})\n  sp = 0x{:016x}  tpidr = 0x{:x}\n", cpu.pc(), addr2sym(cpu.pc()), cpu.sp(),
                              cpu.tpidr());
  for (unsigned i = 0; i < 31; ++i) {
    s += std::format("  x{:<2}= 0x{:016x}", i, cpu.x(i));
    s += (i % 3 == 2 || i == 30) ? "\n" : " ";
  }
  s += "  backtrace:";
  for (gaddr a : Backtrace(cpu, 16)) s += "\n    " + addr2sym(a);
  return s;
}

void SetTraceImports(bool on) { g_trace.store(on ? 1 : 0); }
bool TraceImports() { return g_trace.load() > 0; }
}  // namespace debug
}  // namespace nova
