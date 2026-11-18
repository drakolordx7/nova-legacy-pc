#include "hle/libc.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "core/log.h"
#include "cpu/hle_abi.h"
#include "hle/format.h"
#include "hle/heap.h"
#include "loader/elf64.h"

// Handler names are bionic's. Guest `long`/`size_t` are 64-bit: handlers use int64_t/uint64_t, never `long`.
// Guest addresses that the handler only passes around are `uint64_t`; pointers it dereferences are `T*` (g2h).
namespace nova::hle::libc {
namespace {
using std::int32_t;
using std::int64_t;
using std::uint32_t;
using std::uint64_t;

constexpr int kEPERM = 1, kENOENT = 2, kENOMEM = 12, kEBUSY = 16, kEINVAL = 22, kEDEADLK = 35, kETIMEDOUT = 110;

void SetErrno(Cpu& cpu, int e) { mem::W<int32_t>(cpu.errno_addr(), e); }

// ---------------------------------------------------------------------------------------------- heap
uint64_t h_malloc(Cpu& cpu, uint64_t n) {
  const gaddr p = heap::Alloc(n);
  if (!p) SetErrno(cpu, kENOMEM);
  return p;
}
void h_free(uint64_t p) { heap::Free(p); }
uint64_t h_calloc(Cpu& cpu, uint64_t n, uint64_t sz) {
  const gaddr p = heap::Calloc(n, sz);
  if (!p) SetErrno(cpu, kENOMEM);
  return p;
}
uint64_t h_realloc(Cpu& cpu, uint64_t p, uint64_t n) {
  const gaddr r = heap::Realloc(p, n);
  if (!r && n) SetErrno(cpu, kENOMEM);
  return r;
}
uint64_t h_memalign(uint64_t align, uint64_t n) { return heap::AllocAligned(align, n); }
int32_t h_posix_memalign(uint64_t* out, uint64_t align, uint64_t n) {
  if (align < 8 || (align & (align - 1))) return kEINVAL;
  const gaddr p = heap::AllocAligned(align, n);
  if (!p) return kENOMEM;
  *out = p;
  return 0;
}
uint64_t h_malloc_usable_size(uint64_t p) { return heap::UsableSize(p); }

// ---------------------------------------------------------------------------------------------- string.h
void* h_memcpy(void* d, const void* s, uint64_t n) {
  std::memcpy(d, s, n);
  return d;
}
void* h_memmove(void* d, const void* s, uint64_t n) {
  std::memmove(d, s, n);
  return d;
}
void* h_memset(void* d, int32_t c, uint64_t n) {
  std::memset(d, c, n);
  return d;
}
int32_t h_memcmp(const void* a, const void* b, uint64_t n) { return std::memcmp(a, b, n); }
void* h_memchr(const void* s, int32_t c, uint64_t n) { return const_cast<void*>(std::memchr(s, c, n)); }
void h_bzero(void* d, uint64_t n) { std::memset(d, 0, n); }
uint64_t h_strlen(const char* s) { return std::strlen(s); }
uint64_t h_strnlen(const char* s, uint64_t n) {
  uint64_t i = 0;
  while (i < n && s[i]) ++i;
  return i;
}
int32_t h_strcmp(const char* a, const char* b) { return std::strcmp(a, b); }
int32_t h_strncmp(const char* a, const char* b, uint64_t n) { return std::strncmp(a, b, n); }
int32_t h_strcasecmp(const char* a, const char* b) { return _stricmp(a, b); }
int32_t h_strncasecmp(const char* a, const char* b, uint64_t n) { return _strnicmp(a, b, n); }
char* h_strcpy(char* d, const char* s) {
  std::strcpy(d, s);
  return d;
}
char* h_strncpy(char* d, const char* s, uint64_t n) {
  uint64_t i = 0;
  for (; i < n && s[i]; ++i) d[i] = s[i];
  for (; i < n; ++i) d[i] = 0;
  return d;
}
char* h_strcat(char* d, const char* s) {
  std::strcat(d, s);
  return d;
}
char* h_strncat(char* d, const char* s, uint64_t n) {
  char* e = d + std::strlen(d);
  uint64_t i = 0;
  for (; i < n && s[i]; ++i) e[i] = s[i];
  e[i] = 0;
  return d;
}
char* h_strchr(const char* s, int32_t c) { return const_cast<char*>(std::strchr(s, c)); }
char* h_strrchr(const char* s, int32_t c) { return const_cast<char*>(std::strrchr(s, c)); }
char* h_strstr(const char* h, const char* n) { return const_cast<char*>(std::strstr(h, n)); }
uint64_t h_strdup(const char* s) {
  const uint64_t n = std::strlen(s) + 1;
  const gaddr p = heap::Alloc(n);
  if (p) std::memcpy(g2h(p), s, n);
  return p;
}
uint64_t h_strndup(const char* s, uint64_t max) {
  const uint64_t n = h_strnlen(s, max);
  const gaddr p = heap::Alloc(n + 1);
  if (p) {
    std::memcpy(g2h(p), s, n);
    g2h(p)[n] = 0;
  }
  return p;
}

// ---------------------------------------------------------------------------------------------- stdlib
int64_t h_strtol(const char* s, char** end, int32_t base) {
  char* e;
  const int64_t v = _strtoi64(s, &e, base);
  if (end) *reinterpret_cast<uint64_t*>(end) = e ? h2g(e) : 0;
  return v;
}
uint64_t h_strtoul(const char* s, char** end, int32_t base) {
  char* e;
  const uint64_t v = _strtoui64(s, &e, base);
  if (end) *reinterpret_cast<uint64_t*>(end) = e ? h2g(e) : 0;
  return v;
}
double h_strtod(const char* s, char** end) {
  char* e;
  const double v = std::strtod(s, &e);
  if (end) *reinterpret_cast<uint64_t*>(end) = e ? h2g(e) : 0;
  return v;
}
float h_strtof(const char* s, char** end) {
  char* e;
  const float v = std::strtof(s, &e);
  if (end) *reinterpret_cast<uint64_t*>(end) = e ? h2g(e) : 0;
  return v;
}
int32_t h_atoi(const char* s) { return std::atoi(s); }
int64_t h_atol(const char* s) { return _strtoi64(s, nullptr, 10); }
double h_atof(const char* s) { return std::atof(s); }
int32_t h_abs(int32_t x) { return x < 0 ? -x : x; }
int64_t h_labs(int64_t x) { return x < 0 ? -x : x; }
uint64_t h_getenv(const char*) { return 0; }

[[noreturn]] void h_abort(Cpu& cpu) { throw GuestFault(std::format("abort() called\n{}", debug::DumpState(cpu))); }
[[noreturn]] void h_exit(int32_t code) { throw GuestExit{code}; }
[[noreturn]] void h_stack_chk_fail(Cpu& cpu) {
  throw GuestFault(std::format("stack smashing detected (__stack_chk_fail)\n{}", debug::DumpState(cpu)));
}
[[noreturn]] void h_assert2(Cpu& cpu, const char* file, int32_t line, const char* func, const char* msg) {
  throw GuestFault(std::format("assertion failed: {} ({}:{} in {})\n{}", msg, file, line, func ? func : "?", debug::DumpState(cpu)));
}
int32_t* h_errno(Cpu& cpu) { return g2h<int32_t>(cpu.errno_addr()); }

// ---------------------------------------------------------------------------------------------- atexit
std::mutex g_atexit_mu;
std::vector<std::pair<uint64_t, uint64_t>> g_atexit;  // function, argument
int32_t h_cxa_atexit(uint64_t fn, uint64_t arg, uint64_t /*dso*/) {
  std::lock_guard lk(g_atexit_mu);
  g_atexit.emplace_back(fn, arg);
  return 0;
}
int32_t h_atexit(uint64_t fn) { return h_cxa_atexit(fn, 0, 0); }
void h_cxa_finalize(uint64_t) {}

// ---------------------------------------------------------------------------------------------- printf family
void StoreSized(char* buf, uint64_t size, const std::string& s) {
  if (!size) return;
  const uint64_t n = std::min<uint64_t>(s.size(), size - 1);
  std::memcpy(buf, s.data(), n);
  buf[n] = 0;
}
int32_t h_snprintf(char* buf, uint64_t size, const char* fmt, Varargs va) {
  const std::string s = FormatPrintf(fmt, va);
  StoreSized(buf, size, s);
  return static_cast<int32_t>(s.size());
}
int32_t h_sprintf(char* buf, const char* fmt, Varargs va) {
  const std::string s = FormatPrintf(fmt, va);
  std::memcpy(buf, s.c_str(), s.size() + 1);
  return static_cast<int32_t>(s.size());
}
int32_t h_vsnprintf(char* buf, uint64_t size, const char* fmt, VaList va) {
  const std::string s = FormatPrintf(fmt, va);
  StoreSized(buf, size, s);
  return static_cast<int32_t>(s.size());
}
int32_t h_vsprintf(char* buf, const char* fmt, VaList va) {
  const std::string s = FormatPrintf(fmt, va);
  std::memcpy(buf, s.c_str(), s.size() + 1);
  return static_cast<int32_t>(s.size());
}
int32_t h_printf(const char* fmt, Varargs va) {
  const std::string s = FormatPrintf(fmt, va);
  std::fwrite(s.data(), 1, s.size(), stdout);
  return static_cast<int32_t>(s.size());
}
int32_t h_vprintf(const char* fmt, VaList va) {
  const std::string s = FormatPrintf(fmt, va);
  std::fwrite(s.data(), 1, s.size(), stdout);
  return static_cast<int32_t>(s.size());
}
int32_t h_puts(const char* s) {
  std::fputs(s, stdout);
  std::fputc('\n', stdout);
  return 1;
}
int32_t h_putchar(int32_t c) {
  std::fputc(c, stdout);
  return c;
}

// ---------------------------------------------------------------------------------------------- liblog
const char* PrioName(int32_t p) {
  static const char* n[] = {"?", "?", "V", "D", "I", "W", "E", "F", "S"};
  return p >= 0 && p <= 8 ? n[p] : "?";
}
void LogLine(int32_t prio, const char* tag, const std::string& msg) {
  std::printf("%s/%s: %s\n", PrioName(prio), tag ? tag : "", msg.c_str());
  std::fflush(stdout);
}
int32_t h_android_log_print(int32_t prio, const char* tag, const char* fmt, Varargs va) {
  const std::string s = FormatPrintf(fmt, va);
  LogLine(prio, tag, s);
  return 1;
}
int32_t h_android_log_vprint(int32_t prio, const char* tag, const char* fmt, VaList va) {
  const std::string s = FormatPrintf(fmt, va);
  LogLine(prio, tag, s);
  return 1;
}
int32_t h_android_log_write(int32_t prio, const char* tag, const char* text) {
  LogLine(prio, tag, text ? text : "");
  return 1;
}
void h_android_log_assert(Cpu& cpu, const char* cond, const char* tag, const char* fmt, Varargs va) {
  std::string s = fmt ? FormatPrintf(fmt, va) : std::string("assertion failed: ") + (cond ? cond : "");
  LogLine(7, tag, s);
  throw GuestFault(std::format("__android_log_assert: {}\n{}", s, debug::DumpState(cpu)));
}

// ---------------------------------------------------------------------------------------------- pthread basics
// bionic LP64 pthread_mutex_t: static initialisers only set (type & 3) << 14 in word 0, everything else zero. We keep
// our state in the same 40 bytes: +4 lock flag, +8 owner thread id, +12 recursion count.
constexpr gsize kMtxFlag = 4, kMtxOwner = 8, kMtxCount = 12;
int MutexType(gaddr m) { return (mem::R32(m) >> 14) & 3; }
std::atomic_ref<uint32_t> Flag(gaddr m) { return std::atomic_ref<uint32_t>(*g2h<uint32_t>(m + kMtxFlag)); }

int32_t h_mutex_init(uint64_t m, const int64_t* attr) {
  std::memset(g2h(m), 0, 40);
  const int type = attr ? static_cast<int>(*attr & 3) : 0;
  mem::W32(m, static_cast<uint32_t>(type) << 14);
  return 0;
}
int32_t h_mutex_destroy(uint64_t) { return 0; }
int32_t h_mutex_lock(Cpu& cpu, uint64_t m) {
  const uint32_t tid = cpu.thread_id();
  const int type = MutexType(m);
  auto flag = Flag(m);
  if (type != 0 && flag.load() == 1 && mem::R32(m + kMtxOwner) == tid) {
    if (type == 2) return kEDEADLK;
    mem::W32(m + kMtxCount, mem::R32(m + kMtxCount) + 1);
    return 0;
  }
  for (;;) {
    uint32_t expected = 0;
    if (flag.compare_exchange_strong(expected, 1)) break;
    uint32_t cur = 1;
    WaitOnAddress(g2h(m + kMtxFlag), &cur, 4, INFINITE);
  }
  mem::W32(m + kMtxOwner, tid);
  mem::W32(m + kMtxCount, 1);
  return 0;
}
int32_t h_mutex_trylock(Cpu& cpu, uint64_t m) {
  const uint32_t tid = cpu.thread_id();
  const int type = MutexType(m);
  auto flag = Flag(m);
  if (type == 1 && flag.load() == 1 && mem::R32(m + kMtxOwner) == tid) {
    mem::W32(m + kMtxCount, mem::R32(m + kMtxCount) + 1);
    return 0;
  }
  uint32_t expected = 0;
  if (!flag.compare_exchange_strong(expected, 1)) return kEBUSY;
  mem::W32(m + kMtxOwner, tid);
  mem::W32(m + kMtxCount, 1);
  return 0;
}
int32_t h_mutex_unlock(Cpu& cpu, uint64_t m) {
  const int type = MutexType(m);
  auto flag = Flag(m);
  if (type != 0) {
    if (flag.load() != 1 || mem::R32(m + kMtxOwner) != cpu.thread_id()) return kEPERM;
    const uint32_t c = mem::R32(m + kMtxCount);
    if (c > 1) {
      mem::W32(m + kMtxCount, c - 1);
      return 0;
    }
  }
  mem::W32(m + kMtxOwner, 0);
  flag.store(0);
  WakeByAddressSingle(g2h(m + kMtxFlag));
  return 0;
}
int32_t h_mutexattr_init(int64_t* a) {
  *a = 0;
  return 0;
}
int32_t h_mutexattr_destroy(int64_t*) { return 0; }
int32_t h_mutexattr_settype(int64_t* a, int32_t t) {
  if (t < 0 || t > 2) return kEINVAL;
  *a = (*a & ~3ll) | t;
  return 0;
}
int32_t h_mutexattr_gettype(const int64_t* a, int32_t* t) {
  *t = static_cast<int32_t>(*a & 3);
  return 0;
}
int32_t h_mutexattr_setpshared(int64_t*, int32_t) { return 0; }

// Condition variable: a 32-bit sequence counter in word 0.
std::atomic_ref<uint32_t> Seq(gaddr c) { return std::atomic_ref<uint32_t>(*g2h<uint32_t>(c)); }
int32_t h_cond_init(uint64_t c, const void*) {
  std::memset(g2h(c), 0, 48);
  return 0;
}
int32_t h_cond_destroy(uint64_t) { return 0; }
int32_t h_cond_signal(uint64_t c) {
  Seq(c).fetch_add(1);
  WakeByAddressSingle(g2h(c));
  return 0;
}
int32_t h_cond_broadcast(uint64_t c) {
  Seq(c).fetch_add(1);
  WakeByAddressAll(g2h(c));
  return 0;
}
int32_t CondWait(Cpu& cpu, uint64_t c, uint64_t m, DWORD timeout_ms) {
  uint32_t seq = Seq(c).load();
  h_mutex_unlock(cpu, m);
  const BOOL ok = WaitOnAddress(g2h(c), &seq, 4, timeout_ms);
  const bool timed_out = !ok && GetLastError() == ERROR_TIMEOUT;
  h_mutex_lock(cpu, m);
  return timed_out ? kETIMEDOUT : 0;
}
int32_t h_cond_wait(Cpu& cpu, uint64_t c, uint64_t m) { return CondWait(cpu, c, m, INFINITE); }
int32_t h_cond_timedwait(Cpu& cpu, uint64_t c, uint64_t m, const int64_t* ts) {
  // ts = absolute CLOCK_REALTIME deadline {sec, nsec}
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
  const int64_t deadline_ms = ts[0] * 1000 + ts[1] / 1000000;
  const int64_t wait = std::max<int64_t>(0, deadline_ms - now_ms);
  return CondWait(cpu, c, m, static_cast<DWORD>(std::min<int64_t>(wait, 0x7fffffff)));
}

int32_t h_pthread_once(Cpu& cpu, int32_t* ctl, uint64_t init) {
  std::atomic_ref<int32_t> st(*ctl);
  int32_t expected = 0;
  if (st.compare_exchange_strong(expected, 1)) {
    cpu.Call<void>(init);
    st.store(2);
    WakeByAddressAll(ctl);
    return 0;
  }
  for (int32_t cur = st.load(); cur == 1; cur = st.load()) {
    int32_t one = 1;
    WaitOnAddress(ctl, &one, 4, INFINITE);
  }
  return 0;
}

// pthread keys: a process-wide table of destructors, per-thread values in the Cpu's Pthread slot.
constexpr int kMaxKeys = 256;
std::mutex g_key_mu;
bool g_key_used[kMaxKeys];
uint64_t g_key_dtor[kMaxKeys];
using KeyValues = std::unordered_map<int, uint64_t>;
KeyValues& Values(Cpu& cpu) {
  void*& slot = cpu.user(UserSlot::Pthread);
  if (!slot) slot = new KeyValues();
  return *static_cast<KeyValues*>(slot);
}
int32_t h_key_create(uint32_t* key, uint64_t dtor) {
  std::lock_guard lk(g_key_mu);
  for (int i = 0; i < kMaxKeys; ++i) {
    if (!g_key_used[i]) {
      g_key_used[i] = true;
      g_key_dtor[i] = dtor;
      *key = static_cast<uint32_t>(i);
      return 0;
    }
  }
  return 11;  // EAGAIN
}
int32_t h_key_delete(uint32_t key) {
  std::lock_guard lk(g_key_mu);
  if (key >= kMaxKeys || !g_key_used[key]) return kEINVAL;
  g_key_used[key] = false;
  return 0;
}
uint64_t h_getspecific(Cpu& cpu, uint32_t key) {
  KeyValues& v = Values(cpu);
  auto it = v.find(static_cast<int>(key));
  return it == v.end() ? 0 : it->second;
}
int32_t h_setspecific(Cpu& cpu, uint32_t key, uint64_t value) {
  if (key >= kMaxKeys) return kEINVAL;
  Values(cpu)[static_cast<int>(key)] = value;
  return 0;
}
uint64_t h_pthread_self(Cpu& cpu) { return cpu.tls(); }
int32_t h_pthread_equal(uint64_t a, uint64_t b) { return a == b; }

// ---------------------------------------------------------------------------------------------- misc system
struct Timespec {
  int64_t sec, nsec;
};
struct Timeval {
  int64_t sec, usec;
};
int32_t h_clock_gettime(Cpu& cpu, int32_t clk, Timespec* ts) {
  using namespace std::chrono;
  if (clk == 0) {
    const auto ns = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
    ts->sec = ns / 1000000000;
    ts->nsec = ns % 1000000000;
  } else if (clk == 2 || clk == 3) {  // process / thread CPU time
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    const uint64_t t100 = ((static_cast<uint64_t>(u.dwHighDateTime) << 32) | u.dwLowDateTime) +
                          ((static_cast<uint64_t>(k.dwHighDateTime) << 32) | k.dwLowDateTime);
    ts->sec = static_cast<int64_t>(t100 / 10000000);
    ts->nsec = static_cast<int64_t>(t100 % 10000000) * 100;
  } else {
    const auto ns = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    ts->sec = ns / 1000000000;
    ts->nsec = ns % 1000000000;
  }
  (void)cpu;
  return 0;
}
int32_t h_gettimeofday(Timeval* tv, void*) {
  using namespace std::chrono;
  if (tv) {
    const auto us = duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
    tv->sec = us / 1000000;
    tv->usec = us % 1000000;
  }
  return 0;
}
int64_t h_time(int64_t* t) {
  const int64_t s = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  if (t) *t = s;
  return s;
}
int32_t h_usleep(uint32_t us) {
  std::this_thread::sleep_for(std::chrono::microseconds(us));
  return 0;
}
int32_t h_nanosleep(const Timespec* req, Timespec*) {
  std::this_thread::sleep_for(std::chrono::nanoseconds(req->sec * 1000000000 + req->nsec));
  return 0;
}
int32_t h_sched_yield() {
  std::this_thread::yield();
  return 0;
}
int64_t h_sysconf(Cpu& cpu, int32_t name) {
  switch (name) {
    case 0x27: return 4096;                         // _SC_PAGESIZE
    case 0x60: case 0x61: return std::max(1u, std::thread::hardware_concurrency());  // _SC_NPROCESSORS_CONF/ONLN
    case 2: return 100;                             // _SC_CLK_TCK
    case 0x62: return 1 << 20;                      // _SC_PHYS_PAGES (4 GiB)
    case 0x63: return 1 << 19;                      // _SC_AVPHYS_PAGES
    default:
      SetErrno(cpu, kEINVAL);
      return -1;
  }
}
int32_t h_getpagesize() { return 4096; }
uint64_t h_getauxval(Cpu& cpu, uint64_t type) {
  switch (type) {
    case 6: return 4096;  // AT_PAGESZ
    case 16: return 3;    // AT_HWCAP: FP | ASIMD
    case 26: return 0;    // AT_HWCAP2
    default:
      SetErrno(cpu, kENOENT);
      return 0;
  }
}
int32_t h_system_property_get(const char*, char* value) {
  value[0] = 0;
  return 0;
}

// ---------------------------------------------------------------------------------------------- libdl
std::string g_dlerror;
int32_t h_dl_iterate_phdr(Cpu& cpu, uint64_t cb, uint64_t data) {
  // struct dl_phdr_info (LP64): addr, name, phdr, u16 phnum (+pad), adds, subs, tls_modid, tls_data = 64 bytes
  const gaddr info = heap::Alloc(64);
  int32_t ret = 0;
  for (const auto& m : loader::Modules()) {
    std::memset(g2h(info), 0, 64);
    mem::W64(info + 0, m->base);
    mem::W64(info + 8, m->name_addr);
    mem::W64(info + 16, m->phdr);
    mem::W<uint16_t>(info + 24, static_cast<uint16_t>(m->phnum));
    mem::W64(info + 32, 1);
    mem::W64(info + 40, 0);
    ret = cpu.Call<int32_t>(cb, info, uint64_t{64}, data);
    if (ret) break;
  }
  heap::Free(info);
  return ret;
}
uint64_t h_dlopen(const char* name, int32_t) {
  if (!name) return 1;
  const std::string n = name;
  for (const auto& m : loader::Modules())
    if (n.find(m->name) != std::string::npos) return 1 + (&m - loader::Modules().data()) + 1;
  static const char* kSys[] = {"libc.so", "libm.so", "libdl.so", "liblog.so", "libz.so", "libandroid.so", "libEGL.so",
                               "libGLESv2.so", "libGLESv3.so", "libOpenSLES.so"};
  for (const char* s : kSys)
    if (n.find(s) != std::string::npos) return 1;
  g_dlerror = "library \"" + n + "\" not found";
  return 0;
}
uint64_t h_dlsym(uint64_t handle, const char* name) {
  if (handle > 1) {
    const size_t i = handle - 2;
    if (i < loader::Modules().size())
      if (gaddr a = loader::Modules()[i]->Lookup(name)) return a;
  }
  for (const auto& m : loader::Modules())
    if (handle <= 1)
      if (gaddr a = m->Lookup(name)) return a;
  if (IsRegistered(name)) return StubFor(name);
  g_dlerror = std::string("undefined symbol: ") + name;
  return 0;
}
int32_t h_dlclose(uint64_t) { return 0; }
uint64_t h_dlerror() {
  if (g_dlerror.empty()) return 0;
  static gaddr buf = 0;
  if (!buf) buf = heap::Alloc(512);
  std::strncpy(g2h<char>(buf), g_dlerror.c_str(), 511);
  g2h<char>(buf)[511] = 0;
  g_dlerror.clear();
  return buf;
}
}  // namespace

void RegisterMinimal() {
  // data
  const gaddr guard = DefineData("__stack_chk_guard", 8, 8);
  mem::W64(guard, cpu::StackGuardValue());

  NOVA_HLE("malloc", h_malloc);
  NOVA_HLE("free", h_free);
  NOVA_HLE("calloc", h_calloc);
  NOVA_HLE("realloc", h_realloc);
  NOVA_HLE("memalign", h_memalign);
  NOVA_HLE("aligned_alloc", h_memalign);
  NOVA_HLE("posix_memalign", h_posix_memalign);
  NOVA_HLE("malloc_usable_size", h_malloc_usable_size);

  NOVA_HLE_FAST("memcpy", h_memcpy);
  NOVA_HLE_FAST("memmove", h_memmove);
  NOVA_HLE_FAST("memset", h_memset);
  NOVA_HLE_FAST("memcmp", h_memcmp);
  NOVA_HLE_FAST("bcmp", h_memcmp);
  NOVA_HLE_FAST("memchr", h_memchr);
  NOVA_HLE_FAST("bzero", h_bzero);
  NOVA_HLE_FAST("strlen", h_strlen);
  NOVA_HLE_FAST("strnlen", h_strnlen);
  NOVA_HLE_FAST("strcmp", h_strcmp);
  NOVA_HLE_FAST("strncmp", h_strncmp);
  NOVA_HLE_FAST("strcasecmp", h_strcasecmp);
  NOVA_HLE_FAST("strncasecmp", h_strncasecmp);
  NOVA_HLE_FAST("strcpy", h_strcpy);
  NOVA_HLE_FAST("strncpy", h_strncpy);
  NOVA_HLE_FAST("strcat", h_strcat);
  NOVA_HLE_FAST("strncat", h_strncat);
  NOVA_HLE_FAST("strchr", h_strchr);
  NOVA_HLE_FAST("strrchr", h_strrchr);
  NOVA_HLE_FAST("strstr", h_strstr);
  NOVA_HLE("strdup", h_strdup);
  NOVA_HLE("strndup", h_strndup);

  NOVA_HLE_FAST("strtol", h_strtol);
  NOVA_HLE_FAST("strtoll", h_strtol);
  NOVA_HLE_FAST("strtoul", h_strtoul);
  NOVA_HLE_FAST("strtoull", h_strtoul);
  NOVA_HLE_FAST("strtod", h_strtod);
  NOVA_HLE_FAST("strtof", h_strtof);
  NOVA_HLE_FAST("atoi", h_atoi);
  NOVA_HLE_FAST("atol", h_atol);
  NOVA_HLE_FAST("atoll", h_atol);
  NOVA_HLE_FAST("atof", h_atof);
  NOVA_HLE_FAST("abs", h_abs);
  NOVA_HLE_FAST("labs", h_labs);
  NOVA_HLE_FAST("llabs", h_labs);
  NOVA_HLE("getenv", h_getenv);

  NOVA_HLE("abort", h_abort);
  NOVA_HLE("exit", h_exit);
  NOVA_HLE("_exit", h_exit);
  NOVA_HLE("__stack_chk_fail", h_stack_chk_fail);
  NOVA_HLE("__assert2", h_assert2);
  NOVA_HLE_FAST("__errno", h_errno);

  NOVA_HLE("__cxa_atexit", h_cxa_atexit);
  NOVA_HLE("atexit", h_atexit);
  NOVA_HLE("__cxa_finalize", h_cxa_finalize);

  NOVA_HLE("snprintf", h_snprintf);
  NOVA_HLE("sprintf", h_sprintf);
  NOVA_HLE("vsnprintf", h_vsnprintf);
  NOVA_HLE("vsprintf", h_vsprintf);
  NOVA_HLE("printf", h_printf);
  NOVA_HLE("vprintf", h_vprintf);
  NOVA_HLE("puts", h_puts);
  NOVA_HLE("putchar", h_putchar);

  NOVA_HLE("__android_log_print", h_android_log_print);
  NOVA_HLE("__android_log_vprint", h_android_log_vprint);
  NOVA_HLE("__android_log_write", h_android_log_write);
  NOVA_HLE("__android_log_assert", h_android_log_assert);

  NOVA_HLE("pthread_mutex_init", h_mutex_init);
  NOVA_HLE("pthread_mutex_destroy", h_mutex_destroy);
  NOVA_HLE("pthread_mutex_lock", h_mutex_lock);
  NOVA_HLE("pthread_mutex_trylock", h_mutex_trylock);
  NOVA_HLE("pthread_mutex_unlock", h_mutex_unlock);
  NOVA_HLE("pthread_mutexattr_init", h_mutexattr_init);
  NOVA_HLE("pthread_mutexattr_destroy", h_mutexattr_destroy);
  NOVA_HLE("pthread_mutexattr_settype", h_mutexattr_settype);
  NOVA_HLE("pthread_mutexattr_gettype", h_mutexattr_gettype);
  NOVA_HLE("pthread_mutexattr_setpshared", h_mutexattr_setpshared);
  NOVA_HLE("pthread_cond_init", h_cond_init);
  NOVA_HLE("pthread_cond_destroy", h_cond_destroy);
  NOVA_HLE("pthread_cond_signal", h_cond_signal);
  NOVA_HLE("pthread_cond_broadcast", h_cond_broadcast);
  NOVA_HLE("pthread_cond_wait", h_cond_wait);
  NOVA_HLE("pthread_cond_timedwait", h_cond_timedwait);
  NOVA_HLE("pthread_once", h_pthread_once);
  NOVA_HLE("pthread_key_create", h_key_create);
  NOVA_HLE("pthread_key_delete", h_key_delete);
  NOVA_HLE("pthread_getspecific", h_getspecific);
  NOVA_HLE("pthread_setspecific", h_setspecific);
  NOVA_HLE("pthread_self", h_pthread_self);
  NOVA_HLE("pthread_equal", h_pthread_equal);

  NOVA_HLE("clock_gettime", h_clock_gettime);
  NOVA_HLE("gettimeofday", h_gettimeofday);
  NOVA_HLE("time", h_time);
  NOVA_HLE("usleep", h_usleep);
  NOVA_HLE("nanosleep", h_nanosleep);
  NOVA_HLE("sched_yield", h_sched_yield);
  NOVA_HLE("sysconf", h_sysconf);
  NOVA_HLE("getpagesize", h_getpagesize);
  NOVA_HLE("getauxval", h_getauxval);
  NOVA_HLE("__system_property_get", h_system_property_get);

  NOVA_HLE("dl_iterate_phdr", h_dl_iterate_phdr);
  NOVA_HLE("dlopen", h_dlopen);
  NOVA_HLE("dlsym", h_dlsym);
  NOVA_HLE("dlclose", h_dlclose);
  NOVA_HLE("dlerror", h_dlerror);

  RegisterExtra();
}
}  // namespace nova::hle::libc
