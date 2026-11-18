// pthread / semaphore / scheduler layer on bionic LP64 types. Synchronisation state lives INSIDE the guest object
// (static initialisers are all-zero and libc++ std::mutex / condition_variable embed these structs without calling
// _init), waits use WaitOnAddress on the guest word, so there is no side table and no lifetime to manage:
//   pthread_mutex_t (40 bytes)  +0 type bits (14..15, as bionic), +4 futex word (0 free, 1 locked, 2 contended),
//                               +8 owner thread id, +12 recursion count
//   pthread_cond_t  (48 bytes)  +0 sequence counter (futex word), +4 clock (0 REALTIME, 1 MONOTONIC)
//   pthread_rwlock_t            +0 state (0 free, n readers, 0xFFFFFFFF writer)
//   sem_t                       +0 count
//   pthread_attr_t  (56 bytes)  flags@0 (bit0 = detached), stack_base@8, stack_size@16, guard@24, policy@32, prio@36
// pthread_t is the guest thread id (cpu thread_id(), unique for the process lifetime).
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>

#include "core/log.h"
#include "hle/libc_internal.h"

namespace nova::hle::libc {
namespace {
using std::int32_t;
using std::int64_t;
using std::uint32_t;
using std::uint64_t;

std::atomic_ref<uint32_t> A32(gaddr a) { return std::atomic_ref<uint32_t>(*g2h<uint32_t>(a)); }
void* H(gaddr a) { return g2h(a); }

constexpr int kEBUSY = err::BUSY, kEDEADLK = err::DEADLK, kEPERM = err::PERM, kEINVAL = err::INVAL,
              kEAGAIN = err::AGAIN, kESRCH = err::SRCH, kETIMEDOUT = err::TIMEDOUT;

// Waits while *addr == expected. timeout_ms < 0 waits forever. Returns false on timeout.
bool WaitWord(gaddr addr, uint32_t expected, int64_t timeout_ms) {
  const DWORD t = timeout_ms < 0 ? INFINITE : static_cast<DWORD>(std::min<int64_t>(timeout_ms, 0xFFFFFFFEll));
  if (WaitOnAddress(H(addr), &expected, 4, t)) return true;
  return GetLastError() != ERROR_TIMEOUT;
}
void WakeOne(gaddr a) { WakeByAddressSingle(H(a)); }
void WakeAll(gaddr a) { WakeByAddressAll(H(a)); }

// ---------------------------------------------------------------------------------------------- mutex
constexpr gsize kMtxFutex = 4, kMtxOwner = 8, kMtxCount = 12;
int MutexType(gaddr m) { return (mem::R32(m) >> 14) & 3; }  // 0 normal, 1 recursive, 2 errorcheck

// Drepper futex mutex: 0 free, 1 locked, 2 locked with (possible) waiters.
bool TryAcquire(gaddr m) {
  uint32_t expected = 0;
  return A32(m + kMtxFutex).compare_exchange_strong(expected, 1, std::memory_order_acquire);
}
void AcquireSlow(gaddr m) {
  auto f = A32(m + kMtxFutex);
  // Short spin before parking: guest critical sections are tiny, so most contention clears within microseconds.
  for (int i = 0; i < 64; ++i) {
    uint32_t expected = 0;
    if (f.compare_exchange_weak(expected, 1, std::memory_order_acquire)) return;
    YieldProcessor();
  }
  uint32_t c = f.exchange(2, std::memory_order_acquire);
  while (c != 0) {
    WaitWord(m + kMtxFutex, 2, -1);
    c = f.exchange(2, std::memory_order_acquire);
  }
}
void Release(gaddr m) {
  mem::W32(m + kMtxOwner, 0);
  mem::W32(m + kMtxCount, 0);
  if (A32(m + kMtxFutex).exchange(0, std::memory_order_release) == 2) WakeOne(m + kMtxFutex);
}

int MutexLock(Cpu& cpu, gaddr m) {
  const uint32_t tid = cpu.thread_id();
  const int type = MutexType(m);
  if (type != 0 && mem::R32(m + kMtxOwner) == tid && A32(m + kMtxFutex).load() != 0) {
    if (type == 2) return kEDEADLK;
    mem::W32(m + kMtxCount, mem::R32(m + kMtxCount) + 1);
    return 0;
  }
  if (!TryAcquire(m)) AcquireSlow(m);
  mem::W32(m + kMtxOwner, tid);
  mem::W32(m + kMtxCount, 1);
  return 0;
}
int MutexTryLock(Cpu& cpu, gaddr m) {
  const uint32_t tid = cpu.thread_id();
  const int type = MutexType(m);
  if (type == 1 && mem::R32(m + kMtxOwner) == tid && A32(m + kMtxFutex).load() != 0) {
    mem::W32(m + kMtxCount, mem::R32(m + kMtxCount) + 1);
    return 0;
  }
  if (!TryAcquire(m)) return kEBUSY;
  mem::W32(m + kMtxOwner, tid);
  mem::W32(m + kMtxCount, 1);
  return 0;
}
int MutexUnlock(Cpu& cpu, gaddr m) {
  const int type = MutexType(m);
  if (type != 0) {
    if (A32(m + kMtxFutex).load() == 0 || mem::R32(m + kMtxOwner) != cpu.thread_id()) return kEPERM;
    const uint32_t c = mem::R32(m + kMtxCount);
    if (c > 1) {
      mem::W32(m + kMtxCount, c - 1);
      return 0;
    }
  }
  Release(m);
  return 0;
}

int h_mutex_init(uint64_t m, const uint32_t* attr) {
  if (!m) return kEINVAL;
  std::memset(H(m), 0, 40);
  const uint32_t type = attr ? (*attr & 3) : 0;
  mem::W32(m, type << 14);
  return 0;
}
int h_mutex_destroy(uint64_t m) { return m ? 0 : kEINVAL; }
int h_mutex_lock(Cpu& cpu, uint64_t m) { return m ? MutexLock(cpu, m) : kEINVAL; }
int h_mutex_trylock(Cpu& cpu, uint64_t m) { return m ? MutexTryLock(cpu, m) : kEINVAL; }
int h_mutex_unlock(Cpu& cpu, uint64_t m) { return m ? MutexUnlock(cpu, m) : kEINVAL; }
// absolute CLOCK_REALTIME deadline
int h_mutex_timedlock(Cpu& cpu, uint64_t m, const Timespec* ts) {
  if (!m || !ts) return kEINVAL;
  const int r = MutexTryLock(cpu, m);
  if (r != kEBUSY) return r;
  const int64_t deadline = ts->sec * 1000000000ll + ts->nsec;
  for (;;) {
    const int64_t left = deadline - RealtimeNs();
    if (left <= 0) return kETIMEDOUT;
    if (MutexTryLock(cpu, m) != kEBUSY) return 0;
    SleepNs(std::min<int64_t>(left, 500000));
  }
}
int h_mutex_lock_timeout_np(Cpu& cpu, uint64_t m, uint32_t ms) {
  const int64_t deadline = MonotonicNs() + int64_t{ms} * 1000000;
  for (;;) {
    const int r = MutexTryLock(cpu, m);
    if (r != kEBUSY) return r;
    if (MonotonicNs() >= deadline) return kEBUSY;
    SleepNs(200000);
  }
}

int h_mutexattr_init(uint32_t* a) {
  *a = 0;
  return 0;
}
int h_mutexattr_destroy(uint32_t* a) {
  *a = 0xFFFFFFFFu;
  return 0;
}
int h_mutexattr_settype(uint32_t* a, int32_t t) {
  if (t < 0 || t > 2) return kEINVAL;
  *a = (*a & ~3u) | static_cast<uint32_t>(t);
  return 0;
}
int h_mutexattr_gettype(const uint32_t* a, int32_t* t) {
  *t = static_cast<int32_t>(*a & 3);
  return 0;
}
int h_mutexattr_setpshared(uint32_t* a, int32_t p) {
  *a = (*a & ~4u) | (p ? 4u : 0u);
  return 0;
}
int h_mutexattr_getpshared(const uint32_t* a, int32_t* p) {
  *p = (*a & 4) ? 1 : 0;
  return 0;
}

// ---------------------------------------------------------------------------------------------- condition variable
constexpr gsize kCondSeq = 0, kCondClock = 4;

int h_cond_init(uint64_t c, const uint32_t* attr) {
  if (!c) return kEINVAL;
  std::memset(H(c), 0, 48);
  if (attr) mem::W32(c + kCondClock, (*attr >> 1) & 1);
  return 0;
}
int h_cond_destroy(uint64_t c) { return c ? 0 : kEINVAL; }
int h_cond_signal(uint64_t c) {
  A32(c + kCondSeq).fetch_add(1);
  WakeOne(c + kCondSeq);
  return 0;
}
int h_cond_broadcast(uint64_t c) {
  A32(c + kCondSeq).fetch_add(1);
  WakeAll(c + kCondSeq);
  return 0;
}
int CondWait(Cpu& cpu, gaddr c, gaddr m, int64_t timeout_ms) {
  const uint32_t seq = A32(c + kCondSeq).load();
  const int ur = MutexUnlock(cpu, m);
  if (ur != 0) return ur;
  const bool woke = WaitWord(c + kCondSeq, seq, timeout_ms);
  MutexLock(cpu, m);
  return woke ? 0 : kETIMEDOUT;
}
int64_t MsUntil(int64_t deadline_ns, int clock) {
  const int64_t left = deadline_ns - (clock == 1 ? MonotonicNs() : RealtimeNs());
  return left <= 0 ? 0 : (left + 999999) / 1000000;
}
int h_cond_wait(Cpu& cpu, uint64_t c, uint64_t m) { return CondWait(cpu, c, m, -1); }
// absolute deadline on the condition variable's clock (REALTIME unless pthread_condattr_setclock chose MONOTONIC)
int h_cond_timedwait(Cpu& cpu, uint64_t c, uint64_t m, const Timespec* ts) {
  if (!ts || ts->nsec < 0 || ts->nsec >= 1000000000) return kEINVAL;
  const int clock = static_cast<int>(mem::R32(c + kCondClock) & 1);
  return CondWait(cpu, c, m, MsUntil(ts->sec * 1000000000ll + ts->nsec, clock));
}
int h_cond_timedwait_monotonic_np(Cpu& cpu, uint64_t c, uint64_t m, const Timespec* ts) {
  if (!ts || ts->nsec < 0 || ts->nsec >= 1000000000) return kEINVAL;
  return CondWait(cpu, c, m, MsUntil(ts->sec * 1000000000ll + ts->nsec, 1));
}
int h_cond_timeout_np(Cpu& cpu, uint64_t c, uint64_t m, uint32_t ms) { return CondWait(cpu, c, m, ms); }

int h_condattr_init(uint32_t* a) {
  *a = 0;
  return 0;
}
int h_condattr_destroy(uint32_t* a) {
  *a = 0xFFFFFFFFu;
  return 0;
}
int h_condattr_setclock(uint32_t* a, int32_t clk) {
  if (clk != 0 && clk != 1) return kEINVAL;
  *a = (*a & ~2u) | (clk == 1 ? 2u : 0u);
  return 0;
}
int h_condattr_getclock(const uint32_t* a, int32_t* clk) {
  *clk = (*a & 2) ? 1 : 0;
  return 0;
}
int h_condattr_setpshared(uint32_t* a, int32_t p) {
  *a = (*a & ~1u) | (p ? 1u : 0u);
  return 0;
}
int h_condattr_getpshared(const uint32_t* a, int32_t* p) {
  *p = *a & 1;
  return 0;
}

// ---------------------------------------------------------------------------------------------- rwlock
constexpr uint32_t kWriter = 0xFFFFFFFFu;
int h_rwlock_init(uint64_t l, const void*) {
  if (!l) return kEINVAL;
  std::memset(H(l), 0, 40);
  return 0;
}
int h_rwlock_destroy(uint64_t l) { return l ? 0 : kEINVAL; }
int RdTry(gaddr l) {
  auto s = A32(l);
  uint32_t v = s.load();
  while (v != kWriter) {
    if (s.compare_exchange_weak(v, v + 1)) return 0;
  }
  return kEBUSY;
}
int WrTry(gaddr l) {
  uint32_t expected = 0;
  return A32(l).compare_exchange_strong(expected, kWriter) ? 0 : kEBUSY;
}
int h_rwlock_rdlock(uint64_t l) {
  for (;;) {
    if (RdTry(l) == 0) return 0;
    WaitWord(l, kWriter, -1);
  }
}
int h_rwlock_tryrdlock(uint64_t l) { return RdTry(l); }
int h_rwlock_wrlock(uint64_t l) {
  for (;;) {
    if (WrTry(l) == 0) return 0;
    const uint32_t v = A32(l).load();
    if (v != 0) WaitWord(l, v, -1);
  }
}
int h_rwlock_trywrlock(uint64_t l) { return WrTry(l); }
int h_rwlock_unlock(uint64_t l) {
  auto s = A32(l);
  const uint32_t v = s.load();
  if (v == 0) return kEPERM;
  if (v == kWriter) s.store(0);
  else s.fetch_sub(1);
  WakeAll(l);
  return 0;
}

// ---------------------------------------------------------------------------------------------- once
int h_pthread_once(Cpu& cpu, uint64_t ctl, uint64_t init) {
  auto st = A32(ctl);
  uint32_t expected = 0;
  if (st.compare_exchange_strong(expected, 1)) {
    cpu.Call<void>(init);
    st.store(2);
    WakeAll(ctl);
    return 0;
  }
  for (uint32_t cur = st.load(); cur == 1; cur = st.load()) WaitWord(ctl, 1, -1);
  return 0;
}

// ---------------------------------------------------------------------------------------------- keys
constexpr unsigned kMaxKeys = 128;  // PTHREAD_KEYS_MAX
std::mutex g_key_mu;
struct KeyInfo {
  bool used = false;
  uint32_t gen = 0;
  uint64_t dtor = 0;
} g_keys[kMaxKeys];
struct KeyValues {
  uint32_t gen[kMaxKeys] = {};
  uint64_t val[kMaxKeys] = {};
};
KeyValues& Values(Cpu& cpu) {
  void*& slot = cpu.user(UserSlot::Pthread);
  if (!slot) slot = new KeyValues();
  return *static_cast<KeyValues*>(slot);
}
int h_key_create(uint32_t* key, uint64_t dtor) {
  std::lock_guard lk(g_key_mu);
  for (unsigned i = 0; i < kMaxKeys; ++i) {
    if (!g_keys[i].used) {
      g_keys[i].used = true;
      ++g_keys[i].gen;
      g_keys[i].dtor = dtor;
      *key = i;
      return 0;
    }
  }
  return kEAGAIN;
}
int h_key_delete(uint32_t key) {
  std::lock_guard lk(g_key_mu);
  if (key >= kMaxKeys || !g_keys[key].used) return kEINVAL;
  g_keys[key].used = false;  // stale per-thread values are ignored through the generation counter
  return 0;
}
uint64_t h_getspecific(Cpu& cpu, uint32_t key) {
  if (key >= kMaxKeys) return 0;
  KeyValues& v = Values(cpu);
  return v.gen[key] == g_keys[key].gen && g_keys[key].used ? v.val[key] : 0;
}
int h_setspecific(Cpu& cpu, uint32_t key, uint64_t value) {
  if (key >= kMaxKeys || !g_keys[key].used) return kEINVAL;
  KeyValues& v = Values(cpu);
  v.gen[key] = g_keys[key].gen;
  v.val[key] = value;
  return 0;
}
// Runs at the end of every guest thread (threads::AddExitHook): destructors for non-null values, up to 4 rounds.
void RunKeyDestructors(Cpu& cpu) {
  auto* kv = static_cast<KeyValues*>(cpu.user(UserSlot::Pthread));
  if (!kv) return;
  for (int round = 0; round < 4; ++round) {
    bool any = false;
    for (unsigned k = 0; k < kMaxKeys; ++k) {
      uint64_t dtor = 0, v = 0;
      {
        std::lock_guard lk(g_key_mu);
        if (!g_keys[k].used || kv->gen[k] != g_keys[k].gen || !kv->val[k]) continue;
        v = kv->val[k];
        dtor = g_keys[k].dtor;
        kv->val[k] = 0;
      }
      if (dtor) {
        any = true;
        cpu.Call<void>(dtor, v);
      }
    }
    if (!any) break;
  }
  delete kv;
  cpu.user(UserSlot::Pthread) = nullptr;
}

// ---------------------------------------------------------------------------------------------- attributes
constexpr gsize kAttrFlags = 0, kAttrStackBase = 8, kAttrStackSize = 16, kAttrGuard = 24, kAttrPolicy = 32, kAttrPrio = 36;
constexpr uint64_t kDefaultStack = 1u << 20;

int h_attr_init(uint64_t a) {
  if (!a) return kEINVAL;
  std::memset(H(a), 0, 56);
  mem::W64(a + kAttrStackSize, kDefaultStack);
  mem::W64(a + kAttrGuard, 4096);
  return 0;
}
int h_attr_destroy(uint64_t) { return 0; }
int h_attr_setdetachstate(uint64_t a, int32_t s) {
  if (s != 0 && s != 1) return kEINVAL;
  mem::W32(a + kAttrFlags, (mem::R32(a + kAttrFlags) & ~1u) | static_cast<uint32_t>(s));
  return 0;
}
int h_attr_getdetachstate(uint64_t a, int32_t* s) {
  *s = static_cast<int32_t>(mem::R32(a + kAttrFlags) & 1);
  return 0;
}
int h_attr_setstacksize(uint64_t a, uint64_t sz) {
  if (sz < 8192) return kEINVAL;
  mem::W64(a + kAttrStackSize, sz);
  return 0;
}
int h_attr_getstacksize(uint64_t a, uint64_t* sz) {
  *sz = mem::R64(a + kAttrStackSize);
  return 0;
}
int h_attr_setstack(uint64_t a, uint64_t base, uint64_t sz) {
  mem::W64(a + kAttrStackBase, base);
  mem::W64(a + kAttrStackSize, sz);
  return 0;
}
int h_attr_getstack(uint64_t a, uint64_t* base, uint64_t* sz) {
  *base = mem::R64(a + kAttrStackBase);
  *sz = mem::R64(a + kAttrStackSize);
  return 0;
}
int h_attr_setguardsize(uint64_t a, uint64_t g) {
  mem::W64(a + kAttrGuard, g);
  return 0;
}
int h_attr_getguardsize(uint64_t a, uint64_t* g) {
  *g = mem::R64(a + kAttrGuard);
  return 0;
}
int h_attr_setschedparam(uint64_t a, const int32_t* p) {
  mem::W32(a + kAttrPrio, static_cast<uint32_t>(p[0]));
  return 0;
}
int h_attr_getschedparam(uint64_t a, int32_t* p) {
  p[0] = static_cast<int32_t>(mem::R32(a + kAttrPrio));
  return 0;
}
int h_attr_setschedpolicy(uint64_t a, int32_t pol) {
  mem::W32(a + kAttrPolicy, static_cast<uint32_t>(pol));
  return 0;
}
int h_attr_getschedpolicy(uint64_t a, int32_t* pol) {
  *pol = static_cast<int32_t>(mem::R32(a + kAttrPolicy));
  return 0;
}
int h_attr_setscope(uint64_t, int32_t scope) { return scope == 0 ? 0 : (scope == 1 ? 95 /*ENOTSUP*/ : kEINVAL); }
int h_attr_getscope(uint64_t, int32_t* scope) {
  *scope = 0;
  return 0;
}
int h_attr_getattr_np(uint64_t thread, uint64_t a) {
  (void)thread;
  h_attr_init(a);
  return 0;
}

// ---------------------------------------------------------------------------------------------- threads
struct StartGate {
  std::atomic<uint32_t> state{0};  // 1 = the creator stored the thread id, the new thread may run
};

int h_pthread_create(Cpu& cpu, uint64_t* thread, uint64_t attr, uint64_t start, uint64_t arg) {
  (void)cpu;
  if (!thread || !start) return kEINVAL;
  threads::CreateParams p;
  p.entry = start;
  p.arg = arg;
  if (attr) {
    p.detached = (mem::R32(attr + kAttrFlags) & 1) != 0;
    p.stack_size = mem::R64(attr + kAttrStackSize);
    if (p.stack_size > (64ull << 20)) p.stack_size = 64ull << 20;
  }
  auto gate = std::make_shared<StartGate>();
  // bionic stores the pthread_t before the new thread runs; the start routine may read it right away.
  p.on_start = [gate](Cpu&) {
    while (gate->state.load(std::memory_order_acquire) == 0) {
      uint32_t zero = 0;
      WaitOnAddress(&gate->state, &zero, 4, 50);
    }
  };
  const threads::ThreadId id = threads::Create(p);
  if (!id) return kEAGAIN;
  *thread = id;  // `thread` points into guest memory
  gate->state.store(1, std::memory_order_release);
  WakeByAddressAll(&gate->state);
  return 0;
}
int h_pthread_join(Cpu& cpu, uint64_t thread, uint64_t* retval) {
  if (thread == cpu.thread_id()) return kEDEADLK;
  uint64_t r = 0;
  if (!threads::Join(static_cast<threads::ThreadId>(thread), &r)) return kESRCH;
  if (retval) *retval = r;
  return 0;
}
int h_pthread_detach(uint64_t thread) { return threads::Detach(static_cast<threads::ThreadId>(thread)) ? 0 : kESRCH; }
// Ends the calling guest thread: make the thread entry return `retval` (the exit hooks then run key destructors).
void h_pthread_exit(Cpu& cpu, uint64_t retval) {
  if (cpu.call_depth() > 1)
    log::Warn("pthread_exit called from a nested host->guest call (depth {}): returning from the innermost call only",
              cpu.call_depth());
  cpu.set_x(0, retval);
  cpu.set_pc(hle::ReturnStub());
}
uint64_t h_pthread_self(Cpu& cpu) { return cpu.thread_id(); }
int32_t h_pthread_equal(uint64_t a, uint64_t b) { return a == b; }
int h_pthread_kill(uint64_t thread, int32_t sig) {
  if (sig < 0 || sig > 64) return kEINVAL;
  return threads::IsAlive(static_cast<threads::ThreadId>(thread)) || thread == Cpu::Current().thread_id() ? 0 : kESRCH;
}
int h_zero_n() { return 0; }

// scheduling: recorded nowhere, everything runs as SCHED_OTHER
int h_getschedparam(uint64_t, int32_t* policy, int32_t* param) {
  *policy = 0;
  param[0] = 0;
  return 0;
}
int h_setschedparam(uint64_t, int32_t, const int32_t*) { return 0; }
int32_t h_sched_get_priority_min(int32_t pol) { return pol == 1 || pol == 2 ? 1 : 0; }
int32_t h_sched_get_priority_max(int32_t pol) { return pol == 1 || pol == 2 ? 99 : 0; }
int32_t h_sched_yield() {
  SwitchToThread();
  return 0;
}

// ---------------------------------------------------------------------------------------------- semaphores (return -1 + errno)
int32_t h_sem_init(Cpu& cpu, uint64_t s, int32_t, uint32_t value) {
  if (!s) {
    SetErrno(cpu, err::INVAL);
    return -1;
  }
  std::memset(H(s), 0, 16);
  mem::W32(s, value);
  return 0;
}
int32_t h_sem_destroy(uint64_t) { return 0; }
int32_t h_sem_post(Cpu& cpu, uint64_t s) {
  auto c = A32(s);
  uint32_t v = c.load();
  do {
    if (v == 0x7FFFFFFFu) {
      SetErrno(cpu, err::OVERFLOW_);
      return -1;
    }
  } while (!c.compare_exchange_weak(v, v + 1));
  WakeOne(s);
  return 0;
}
bool SemTake(gaddr s) {
  auto c = A32(s);
  uint32_t v = c.load();
  while (v > 0) {
    if (c.compare_exchange_weak(v, v - 1)) return true;
  }
  return false;
}
int32_t h_sem_wait(uint64_t s) {
  for (;;) {
    if (SemTake(s)) return 0;
    WaitWord(s, 0, -1);
  }
}
int32_t h_sem_trywait(Cpu& cpu, uint64_t s) {
  if (SemTake(s)) return 0;
  SetErrno(cpu, err::AGAIN);
  return -1;
}
int32_t h_sem_timedwait(Cpu& cpu, uint64_t s, const Timespec* ts) {
  const int64_t deadline = ts->sec * 1000000000ll + ts->nsec;
  for (;;) {
    if (SemTake(s)) return 0;
    const int64_t ms = MsUntil(deadline, 0);
    if (ms == 0) {
      SetErrno(cpu, err::TIMEDOUT);
      return -1;
    }
    WaitWord(s, 0, ms);
  }
}
int32_t h_sem_getvalue(uint64_t s, int32_t* out) {
  *out = static_cast<int32_t>(mem::R32(s));
  return 0;
}
}  // namespace

void RegisterPthread() {
  // Key destructors run on the guest thread right before its Cpu is torn down.
  static std::once_flag once;
  std::call_once(once, [] { threads::AddExitHook(RunKeyDestructors); });

  NOVA_HLE("pthread_create", h_pthread_create);
  NOVA_HLE("pthread_join", h_pthread_join);
  NOVA_HLE("pthread_detach", h_pthread_detach);
  NOVA_HLE("pthread_exit", h_pthread_exit);
  NOVA_HLE_FAST("pthread_self", h_pthread_self);
  NOVA_HLE_FAST("pthread_equal", h_pthread_equal);
  NOVA_HLE("pthread_kill", h_pthread_kill);
  for (const char* n : {"pthread_sigmask", "pthread_setname_np", "pthread_atfork", "pthread_setcancelstate",
                        "pthread_setcanceltype", "pthread_setaffinity_np"})
    NOVA_HLE(n, h_zero_n);
  NOVA_HLE("pthread_getattr_np", h_attr_getattr_np);
  NOVA_HLE("pthread_getschedparam", h_getschedparam);
  NOVA_HLE("pthread_setschedparam", h_setschedparam);
  NOVA_HLE("sched_get_priority_min", h_sched_get_priority_min);
  NOVA_HLE("sched_get_priority_max", h_sched_get_priority_max);
  NOVA_HLE("sched_yield", h_sched_yield);

  NOVA_HLE("pthread_attr_init", h_attr_init);
  NOVA_HLE("pthread_attr_destroy", h_attr_destroy);
  NOVA_HLE("pthread_attr_setdetachstate", h_attr_setdetachstate);
  NOVA_HLE("pthread_attr_getdetachstate", h_attr_getdetachstate);
  NOVA_HLE("pthread_attr_setstacksize", h_attr_setstacksize);
  NOVA_HLE("pthread_attr_getstacksize", h_attr_getstacksize);
  NOVA_HLE("pthread_attr_setstack", h_attr_setstack);
  NOVA_HLE("pthread_attr_getstack", h_attr_getstack);
  NOVA_HLE("pthread_attr_setguardsize", h_attr_setguardsize);
  NOVA_HLE("pthread_attr_getguardsize", h_attr_getguardsize);
  NOVA_HLE("pthread_attr_setschedparam", h_attr_setschedparam);
  NOVA_HLE("pthread_attr_getschedparam", h_attr_getschedparam);
  NOVA_HLE("pthread_attr_setschedpolicy", h_attr_setschedpolicy);
  NOVA_HLE("pthread_attr_getschedpolicy", h_attr_getschedpolicy);
  NOVA_HLE("pthread_attr_setscope", h_attr_setscope);
  NOVA_HLE("pthread_attr_getscope", h_attr_getscope);

  NOVA_HLE_FAST("pthread_mutex_init", h_mutex_init);
  NOVA_HLE_FAST("pthread_mutex_destroy", h_mutex_destroy);
  // kFast: the uncontended path never leaves the JIT callback. A contended lock blocks inside the callback (futex wait),
// which is no different from blocking in the halting dispatcher: no guest code runs on this thread meanwhile.
  NOVA_HLE_FAST("pthread_mutex_lock", h_mutex_lock);
  NOVA_HLE_FAST("pthread_mutex_trylock", h_mutex_trylock);
  NOVA_HLE_FAST("pthread_mutex_unlock", h_mutex_unlock);
  NOVA_HLE("pthread_mutex_timedlock", h_mutex_timedlock);
  NOVA_HLE("pthread_mutex_lock_timeout_np", h_mutex_lock_timeout_np);
  NOVA_HLE("pthread_mutexattr_init", h_mutexattr_init);
  NOVA_HLE("pthread_mutexattr_destroy", h_mutexattr_destroy);
  NOVA_HLE("pthread_mutexattr_settype", h_mutexattr_settype);
  NOVA_HLE("pthread_mutexattr_gettype", h_mutexattr_gettype);
  NOVA_HLE("pthread_mutexattr_setpshared", h_mutexattr_setpshared);
  NOVA_HLE("pthread_mutexattr_getpshared", h_mutexattr_getpshared);

  NOVA_HLE("pthread_cond_init", h_cond_init);
  NOVA_HLE("pthread_cond_destroy", h_cond_destroy);
  NOVA_HLE_FAST("pthread_cond_signal", h_cond_signal);
  NOVA_HLE_FAST("pthread_cond_broadcast", h_cond_broadcast);
  NOVA_HLE("pthread_cond_wait", h_cond_wait);
  NOVA_HLE("pthread_cond_timedwait", h_cond_timedwait);
  NOVA_HLE("pthread_cond_timedwait_monotonic_np", h_cond_timedwait_monotonic_np);
  NOVA_HLE("pthread_cond_timeout_np", h_cond_timeout_np);
  NOVA_HLE("pthread_condattr_init", h_condattr_init);
  NOVA_HLE("pthread_condattr_destroy", h_condattr_destroy);
  NOVA_HLE("pthread_condattr_setclock", h_condattr_setclock);
  NOVA_HLE("pthread_condattr_getclock", h_condattr_getclock);
  NOVA_HLE("pthread_condattr_setpshared", h_condattr_setpshared);
  NOVA_HLE("pthread_condattr_getpshared", h_condattr_getpshared);

  NOVA_HLE("pthread_rwlock_init", h_rwlock_init);
  NOVA_HLE("pthread_rwlock_destroy", h_rwlock_destroy);
  NOVA_HLE("pthread_rwlock_rdlock", h_rwlock_rdlock);
  NOVA_HLE_FAST("pthread_rwlock_tryrdlock", h_rwlock_tryrdlock);
  NOVA_HLE("pthread_rwlock_wrlock", h_rwlock_wrlock);
  NOVA_HLE_FAST("pthread_rwlock_trywrlock", h_rwlock_trywrlock);
  NOVA_HLE_FAST("pthread_rwlock_unlock", h_rwlock_unlock);

  NOVA_HLE("pthread_once", h_pthread_once);
  NOVA_HLE("pthread_key_create", h_key_create);
  NOVA_HLE("pthread_key_delete", h_key_delete);
  NOVA_HLE_FAST("pthread_getspecific", h_getspecific);
  NOVA_HLE_FAST("pthread_setspecific", h_setspecific);

  NOVA_HLE("sem_init", h_sem_init);
  NOVA_HLE("sem_destroy", h_sem_destroy);
  NOVA_HLE_FAST("sem_post", h_sem_post);
  NOVA_HLE("sem_wait", h_sem_wait);
  NOVA_HLE_FAST("sem_trywait", h_sem_trywait);
  NOVA_HLE("sem_timedwait", h_sem_timedwait);
  NOVA_HLE_FAST("sem_getvalue", h_sem_getvalue);
}
}  // namespace nova::hle::libc
