// Process / runtime side of the bionic LP64 HLE: exit and atexit, environment, signals (recorded only),
// setjmp/longjmp on guest registers, system properties of the spoofed phone, sysconf, syscall(), qsort/bsearch (the
// BSD algorithm bionic uses, so ties sort the same way), rand (bionic's rand() is lrand48()), strerror, syslog.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

#include "core/log.h"
#include "hle/format.h"
#include "hle/heap.h"
#include "hle/libc_internal.h"
#include "hle/vfs.h"

namespace nova::hle::libc {

// ------------------------------------------------------------------------------------------ shared helpers
gaddr InternString(std::string_view s) {
  static std::mutex mu;
  static std::unordered_map<std::string, gaddr> table;
  std::lock_guard lk(mu);
  auto it = table.find(std::string(s));
  if (it != table.end()) return it->second;
  const gaddr p = heap::Alloc(s.size() + 1);
  if (!p) return 0;
  std::memcpy(g2h(p), s.data(), s.size());
  g2h(p)[s.size()] = 0;
  table.emplace(std::string(s), p);
  return p;
}

gaddr ThreadBuffer(unsigned slot, gsize size) {
  struct Bufs {
    gaddr addr[8] = {};
    gsize cap[8] = {};
  };
  thread_local Bufs b;
  slot &= 7;
  if (b.cap[slot] < size) {
    b.addr[slot] = heap::Alloc(std::max<gsize>(size, 256));
    b.cap[slot] = std::max<gsize>(size, 256);
  }
  return b.addr[slot];
}

std::int64_t RealtimeNs() {
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  const std::uint64_t t = (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
  return (static_cast<std::int64_t>(t) - 116444736000000000ll) * 100;
}
std::int64_t MonotonicNs() {
  static const std::int64_t freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  const std::int64_t whole = c.QuadPart / freq, rem = c.QuadPart % freq;
  return whole * 1000000000ll + rem * 1000000000ll / freq;
}
void SleepNs(std::int64_t ns) {
  if (ns <= 0) {
    SwitchToThread();
    return;
  }
  thread_local HANDLE timer = [] {
    HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    return h ? h : CreateWaitableTimerW(nullptr, TRUE, nullptr);
  }();
  if (timer) {
    LARGE_INTEGER due;
    due.QuadPart = -(ns / 100 ? ns / 100 : 1);
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(timer, INFINITE);
      return;
    }
  }
  Sleep(static_cast<DWORD>((ns + 999999) / 1000000));
}

std::string ErrorText(int e) {
  static const std::map<int, const char*> t = {
      {0, "Success"}, {1, "Operation not permitted"}, {2, "No such file or directory"}, {3, "No such process"},
      {4, "Interrupted system call"}, {5, "I/O error"}, {6, "No such device or address"}, {7, "Argument list too long"},
      {8, "Exec format error"}, {9, "Bad file descriptor"}, {10, "No child processes"}, {11, "Try again"},
      {12, "Out of memory"}, {13, "Permission denied"}, {14, "Bad address"}, {15, "Block device required"},
      {16, "Device or resource busy"}, {17, "File exists"}, {18, "Cross-device link"}, {19, "No such device"},
      {20, "Not a directory"}, {21, "Is a directory"}, {22, "Invalid argument"}, {23, "File table overflow"},
      {24, "Too many open files"}, {25, "Inappropriate ioctl for device"}, {26, "Text file busy"}, {27, "File too large"},
      {28, "No space left on device"}, {29, "Illegal seek"}, {30, "Read-only file system"}, {31, "Too many links"},
      {32, "Broken pipe"}, {33, "Math argument out of domain of func"}, {34, "Math result not representable"},
      {35, "Deadlock condition"}, {36, "File name too long"}, {37, "No record locks available"},
      {38, "Function not implemented"}, {39, "Directory not empty"}, {40, "Too many symbolic links encountered"},
      {75, "Value too large for defined data type"}, {84, "Illegal byte sequence"}, {88, "Socket operation on non-socket"},
      {89, "Destination address required"}, {90, "Message too long"}, {91, "Protocol wrong type for socket"},
      {92, "Protocol not available"}, {93, "Protocol not supported"}, {95, "Operation not supported on transport endpoint"},
      {97, "Address family not supported by protocol"}, {98, "Address already in use"},
      {99, "Cannot assign requested address"}, {100, "Network is down"}, {101, "Network is unreachable"},
      {103, "Software caused connection abort"}, {104, "Connection reset by peer"}, {105, "No buffer space available"},
      {106, "Transport endpoint is already connected"}, {107, "Transport endpoint is not connected"},
      {110, "Connection timed out"}, {111, "Connection refused"}, {112, "Host is down"}, {113, "No route to host"},
      {114, "Operation already in progress"}, {115, "Operation now in progress"},
  };
  auto it = t.find(e);
  if (it != t.end()) return it->second;
  return "Unknown error " + std::to_string(e);
}

namespace {
using std::int32_t;
using std::int64_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;

constexpr int kPid = 4242;

// ------------------------------------------------------------------------------------------ exit / atexit
struct AtExitEntry {
  uint64_t fn, arg, dso;
  bool plain;  // registered through atexit(), as opposed to __cxa_atexit (static destructors)
};
std::mutex g_atexit_mu;
std::vector<AtExitEntry> g_atexit;

int32_t h_cxa_atexit(uint64_t fn, uint64_t arg, uint64_t dso) {
  std::lock_guard lk(g_atexit_mu);
  g_atexit.push_back({fn, arg, dso, false});
  return 0;
}
int32_t h_atexit(uint64_t fn) {
  std::lock_guard lk(g_atexit_mu);
  g_atexit.push_back({fn, 0, 0, true});
  return 0;
}
// Runs (and removes) the handlers of `dso` in reverse registration order; dso 0 = every plain atexit() handler.
void RunHandlers(Cpu& cpu, uint64_t dso, bool only_plain) {
  for (;;) {
    AtExitEntry e{};
    {
      std::lock_guard lk(g_atexit_mu);
      auto it = std::find_if(g_atexit.rbegin(), g_atexit.rend(), [&](const AtExitEntry& x) {
        return only_plain ? x.plain : (dso == 0 || x.dso == dso);
      });
      if (it == g_atexit.rend()) return;
      e = *it;
      g_atexit.erase(std::next(it).base());
    }
    cpu.Call<void>(e.fn, e.arg);
  }
}
void h_cxa_finalize(Cpu& cpu, uint64_t dso) { RunHandlers(cpu, dso, false); }

// exit(): atexit() handlers and a stdio flush. The __cxa_atexit static destructors (thousands in libNOVA, torn down
// against live engine threads) are NOT run: the process is ending anyway and they only risk crashing on the way out.
[[noreturn]] void h_exit(Cpu& cpu, int32_t code) {
  RunHandlers(cpu, 0, true);
  FlushAllStreams();
  throw GuestExit{code};
}
[[noreturn]] void h__exit(int32_t code) {
  FlushAllStreams();
  throw GuestExit{code};
}

// ------------------------------------------------------------------------------------------ environment
struct EnvVar {
  std::string value;
  gaddr guest = 0;
};
std::mutex g_env_mu;
std::map<std::string, EnvVar>& Env() {
  static std::map<std::string, EnvVar> env = {
      {"HOME", {std::string("/data/data/") + vfs::kPackage + "/files", 0}},
      {"TMPDIR", {"/data/local/tmp", 0}},
      {"PATH", {"/sbin:/system/sbin:/system/bin:/system/xbin", 0}},
      {"ANDROID_DATA", {"/data", 0}},
      {"ANDROID_ROOT", {"/system", 0}},
      {"ANDROID_STORAGE", {"/storage", 0}},
      {"EXTERNAL_STORAGE", {"/sdcard", 0}},
  };
  return env;
}
uint64_t h_getenv(const char* name) {
  if (!name) return 0;
  std::lock_guard lk(g_env_mu);
  auto it = Env().find(name);
  if (it == Env().end()) return 0;
  if (!it->second.guest) it->second.guest = InternString(it->second.value);
  return it->second.guest;
}
int32_t h_setenv(Cpu& cpu, const char* name, const char* value, int32_t overwrite) {
  if (!name || !*name || std::strchr(name, '=')) {
    SetErrno(cpu, err::INVAL);
    return -1;
  }
  std::lock_guard lk(g_env_mu);
  auto it = Env().find(name);
  if (it != Env().end() && !overwrite) return 0;
  Env()[name] = EnvVar{value ? value : "", 0};
  return 0;
}
int32_t h_putenv(Cpu& cpu, char* str) {
  if (!str) {
    SetErrno(cpu, err::INVAL);
    return -1;
  }
  const char* eq = std::strchr(str, '=');
  if (!eq) {
    std::lock_guard lk(g_env_mu);
    Env().erase(str);
    return 0;
  }
  std::lock_guard lk(g_env_mu);
  Env()[std::string(str, static_cast<size_t>(eq - str))] = EnvVar{eq + 1, 0};
  return 0;
}
int32_t h_unsetenv(Cpu& cpu, const char* name) {
  if (!name || !*name || std::strchr(name, '=')) {
    SetErrno(cpu, err::INVAL);
    return -1;
  }
  std::lock_guard lk(g_env_mu);
  Env().erase(name);
  return 0;
}

// ------------------------------------------------------------------------------------------ signals (recorded only)
struct SigAct {
  uint64_t handler = 0, flags = 0, restorer = 0, mask = 0;
};
std::mutex g_sig_mu;
SigAct g_sigs[65];
int32_t h_sigaction(Cpu& cpu, int32_t sig, const uint8_t* act, uint8_t* old) {
  if (sig < 1 || sig > 64 || (act && (sig == 9 || sig == 19))) {
    SetErrno(cpu, err::INVAL);
    return -1;
  }
  std::lock_guard lk(g_sig_mu);
  SigAct& s = g_sigs[sig];
  if (old) {  // bionic arm64 struct sigaction: handler@0, flags@8, restorer@16, mask@24
    std::memcpy(old, &s.handler, 8);
    std::memcpy(old + 8, &s.flags, 8);
    std::memcpy(old + 16, &s.restorer, 8);
    std::memcpy(old + 24, &s.mask, 8);
  }
  if (act) {
    std::memcpy(&s.handler, act, 8);
    uint32_t fl;
    std::memcpy(&fl, act + 8, 4);
    s.flags = fl;
    std::memcpy(&s.restorer, act + 16, 8);
    std::memcpy(&s.mask, act + 24, 8);
  }
  return 0;
}
uint64_t h_signal(Cpu& cpu, int32_t sig, uint64_t handler) {
  if (sig < 1 || sig > 64 || sig == 9 || sig == 19) {
    SetErrno(cpu, err::INVAL);
    return ~0ull;  // SIG_ERR
  }
  std::lock_guard lk(g_sig_mu);
  const uint64_t prev = g_sigs[sig].handler;
  g_sigs[sig].handler = handler;
  return prev;
}
int32_t h_sigprocmask(int32_t, const uint64_t*, uint64_t* old) {
  if (old) *old = 0;
  return 0;
}
int32_t h_ret0() { return 0; }

// ------------------------------------------------------------------------------------------ setjmp / longjmp
// jmp_buf in guest memory (bionic reserves 256 bytes on arm64): +0 magic, +8 x19..x30, +104 sp, +112 d8..d15,
// +176 call depth of the saving thread. longjmp restores x19-x30, sp and d8-d15 and returns to the setjmp caller.
constexpr uint64_t kJmpMagic = 0x4E4F56414A4D5042ull;
void SaveContext(Cpu& cpu, gaddr buf) {
  mem::W64(buf, kJmpMagic);
  for (unsigned i = 0; i < 12; ++i) mem::W64(buf + 8 + 8 * i, cpu.x(19 + i));
  mem::W64(buf + 104, cpu.sp());
  for (unsigned i = 0; i < 8; ++i) mem::W64(buf + 112 + 8 * i, cpu.v(8 + i)[0]);
  mem::W64(buf + 176, static_cast<uint64_t>(cpu.call_depth()));
}
int32_t h_setjmp(Cpu& cpu, uint64_t buf) {
  SaveContext(cpu, buf);
  return 0;
}
int32_t h_sigsetjmp(Cpu& cpu, uint64_t buf, int32_t) {
  SaveContext(cpu, buf);
  return 0;
}
void h_longjmp(Cpu& cpu, uint64_t buf, int32_t val) {
  if (mem::R64(buf) != kJmpMagic)
    throw GuestFault("longjmp: jmp_buf was not filled by setjmp\n" + debug::DumpState(cpu));
  if (mem::R64(buf + 176) != static_cast<uint64_t>(cpu.call_depth()))
    throw GuestFault("longjmp across a host->guest call boundary (qsort/pthread_once callback...) is not supported\n" +
                     debug::DumpState(cpu));
  for (unsigned i = 0; i < 12; ++i) cpu.set_x(19 + i, mem::R64(buf + 8 + 8 * i));
  cpu.set_sp(mem::R64(buf + 104));
  for (unsigned i = 0; i < 8; ++i) cpu.set_v(8 + i, {mem::R64(buf + 112 + 8 * i), 0});
  cpu.set_x(0, val ? static_cast<uint64_t>(static_cast<int64_t>(val)) : 1);
  // execution continues at the stub's `ret`, which now returns through the restored x30 (the setjmp call site)
}

// ------------------------------------------------------------------------------------------ properties, sysconf, ids
int32_t h_system_property_get(const char* name, char* value) {
  bool found = false;
  std::string v = vfs::SystemProperty(name ? name : "", &found);
  if (v.size() > 91) v.resize(91);
  std::memcpy(value, v.c_str(), v.size() + 1);
  return static_cast<int32_t>(v.size());
}
int64_t h_sysconf(Cpu& cpu, int32_t name) {
  MEMORYSTATUSEX m{};
  m.dwLength = sizeof m;
  GlobalMemoryStatusEx(&m);
  const uint64_t total = std::min<uint64_t>(m.ullTotalPhys, 6ull << 30);
  switch (name) {
    case 0: return 131072;   // _SC_ARG_MAX
    case 1: return 1024;     // _SC_CHILD_MAX
    case 2: return 100;      // _SC_CLK_TCK
    case 3: return 65536;    // _SC_NGROUPS_MAX
    case 4: return 1024;     // _SC_OPEN_MAX
    case 0x27: case 0x28: return 4096;  // _SC_PAGESIZE / _SC_PAGE_SIZE
    case 0x60: case 0x61: return 8;     // _SC_NPROCESSORS_CONF / _ONLN
    case 0x62: return static_cast<int64_t>(total / 4096);  // _SC_PHYS_PAGES
    case 0x63: return static_cast<int64_t>(std::min<uint64_t>(m.ullAvailPhys, total) / 4096);  // _SC_AVPHYS_PAGES
    case 0x74: return 255;   // _SC_HOST_NAME_MAX
    default:
      SetErrno(cpu, err::INVAL);
      return -1;
  }
}
int32_t h_getpid() { return kPid; }
int32_t h_gettid(Cpu& cpu) { return kPid - 1 + static_cast<int32_t>(cpu.thread_id()); }
int32_t h_zero() { return 0; }

int64_t h_syscall(Cpu& cpu, int64_t nr, Varargs va) {
  switch (nr) {
    case 122: return 0;  // sched_setaffinity: accepted, ignored
    case 123: {          // sched_getaffinity(pid, len, mask): all 8 CPUs allowed
      (void)va.NextInt();
      const uint64_t len = va.NextInt();
      const gaddr mask = va.NextInt();
      const uint64_t n = std::min<uint64_t>(len, 8);
      if (mask && n) {
        std::memset(g2h(mask), 0, len);
        std::memset(g2h(mask), 0xFF, 1);  // CPUs 0..7
      }
      return static_cast<int64_t>(n);
    }
    case 172: return kPid;                                           // getpid
    case 178: return kPid - 1 + static_cast<int64_t>(cpu.thread_id());  // gettid
    default:
      SetErrno(cpu, err::NOSYS);
      return -1;
  }
}

void h_syslog(int32_t prio, const char* fmt, Varargs va) {
  static const char* kName[] = {"emerg", "alert", "crit", "err", "warning", "notice", "info", "debug"};
  log::Debug("syslog[{}]: {}", kName[prio & 7], FormatPrintf(fmt, va));
}

// ------------------------------------------------------------------------------------------ rand (lrand48)
std::mutex g_rand_mu;
uint64_t g_rand48 = 0x1234ABCD330Eull;
int32_t Lrand48() {
  g_rand48 = (g_rand48 * 0x5DEECE66Dull + 0xB) & ((1ull << 48) - 1);
  return static_cast<int32_t>(g_rand48 >> 17);
}
int32_t h_rand() {
  std::lock_guard lk(g_rand_mu);
  return Lrand48();
}
void h_srand(uint32_t seed) {
  std::lock_guard lk(g_rand_mu);
  g_rand48 = (static_cast<uint64_t>(seed) << 16) | 0x330E;
}
int32_t h_rand_r(uint32_t* seed) {
  // OpenBSD rand_r
  uint32_t next = *seed;
  next = next * 1103515245 + 12345;
  *seed = next;
  return static_cast<int32_t>((next / 65536) % 32768);
}

// ------------------------------------------------------------------------------------------ qsort / bsearch (BSD algorithm)
struct Sorter {
  Cpu& cpu;
  uint64_t cmp;
  uint64_t es;
  int Cmp(gaddr a, gaddr b) { return cpu.Call<int32_t>(cmp, a, b); }
  void Swap(gaddr a, gaddr b) {
    if (a == b) return;
    std::swap_ranges(g2h(a), g2h(a) + es, g2h(b));
  }
  void VecSwap(gaddr a, gaddr b, uint64_t n) {
    for (uint64_t i = 0; i < n; i += es) Swap(a + i, b + i);
  }
  gaddr Med3(gaddr a, gaddr b, gaddr c) {
    return Cmp(a, b) < 0 ? (Cmp(b, c) < 0 ? b : (Cmp(a, c) < 0 ? c : a)) : (Cmp(b, c) > 0 ? b : (Cmp(a, c) < 0 ? a : c));
  }
  void Insertion(gaddr a, uint64_t n) {
    for (gaddr pm = a + es; pm < a + n * es; pm += es)
      for (gaddr pl = pm; pl > a && Cmp(pl - es, pl) > 0; pl -= es) Swap(pl, pl - es);
  }
  void Sort(gaddr a, uint64_t n) {
    for (;;) {
      int swap_cnt = 0;
      if (n < 7) {
        Insertion(a, n);
        return;
      }
      gaddr pm = a + (n / 2) * es;
      if (n > 7) {
        gaddr pl = a, pn = a + (n - 1) * es;
        if (n > 40) {
          const uint64_t d = (n / 8) * es;
          pl = Med3(pl, pl + d, pl + 2 * d);
          pm = Med3(pm - d, pm, pm + d);
          pn = Med3(pn - 2 * d, pn - d, pn);
        }
        pm = Med3(pl, pm, pn);
      }
      Swap(a, pm);
      gaddr pa = a + es, pb = pa;
      gaddr pc = a + (n - 1) * es, pd = pc;
      for (;;) {
        int r;
        while (pb <= pc && (r = Cmp(pb, a)) <= 0) {
          if (r == 0) {
            swap_cnt = 1;
            Swap(pa, pb);
            pa += es;
          }
          pb += es;
        }
        while (pb <= pc && (r = Cmp(pc, a)) >= 0) {
          if (r == 0) {
            swap_cnt = 1;
            Swap(pc, pd);
            pd -= es;
          }
          pc -= es;
        }
        if (pb > pc) break;
        Swap(pb, pc);
        swap_cnt = 1;
        pb += es;
        pc -= es;
      }
      if (swap_cnt == 0) {  // no swaps: nearly sorted, finish with insertion sort
        Insertion(a, n);
        return;
      }
      const gaddr pn = a + n * es;
      uint64_t r = std::min<uint64_t>(pa - a, pb - pa);
      VecSwap(a, pb - r, r);
      r = std::min<uint64_t>(pd - pc, pn - pd - es);
      VecSwap(pb, pn - r, r);
      if ((r = pb - pa) > es) Sort(a, r / es);
      if ((r = pd - pc) > es) {  // iterate instead of recursing on the second part
        a = pn - r;
        n = r / es;
        continue;
      }
      return;
    }
  }
};
void h_qsort(Cpu& cpu, uint64_t base, uint64_t n, uint64_t size, uint64_t cmp) {
  if (n < 2 || !size) return;
  Sorter s{cpu, cmp, size};
  s.Sort(base, n);
}
uint64_t h_bsearch(Cpu& cpu, uint64_t key, uint64_t base, uint64_t n, uint64_t size, uint64_t cmp) {
  uint64_t lim = n;
  gaddr p = base;
  for (; lim != 0; lim >>= 1) {
    const gaddr mid = p + (lim >> 1) * size;
    const int32_t c = cpu.Call<int32_t>(cmp, key, mid);
    if (c == 0) return mid;
    if (c > 0) {
      p = mid + size;
      --lim;
    }
  }
  return 0;
}

// ------------------------------------------------------------------------------------------ strerror
uint64_t h_strerror(int32_t e) {
  const std::string s = ErrorText(e);
  return InternString(s);
}
int32_t h_strerror_r(Cpu& cpu, int32_t e, char* buf, uint64_t len) {
  const std::string s = ErrorText(e);
  if (!len) return err::RANGE;
  const bool known = s.rfind("Unknown error", 0) != 0 || e == 0;
  const uint64_t n = std::min<uint64_t>(s.size(), len - 1);
  std::memcpy(buf, s.data(), n);
  buf[n] = 0;
  (void)cpu;
  if (s.size() > n) return err::RANGE;
  return known ? 0 : err::INVAL;
}
}  // namespace

void RunAtExit(std::uint64_t dso) { RunHandlers(Cpu::Current(), dso, false); }

void RegisterRuntime() {
  NOVA_HLE("__cxa_atexit", h_cxa_atexit);
  NOVA_HLE("atexit", h_atexit);
  NOVA_HLE("__cxa_finalize", h_cxa_finalize);
  NOVA_HLE("exit", h_exit);
  NOVA_HLE("_exit", h__exit);
  NOVA_HLE("quick_exit", h__exit);

  NOVA_HLE("getenv", h_getenv);
  NOVA_HLE("setenv", h_setenv);
  NOVA_HLE("putenv", h_putenv);
  NOVA_HLE("unsetenv", h_unsetenv);

  NOVA_HLE("sigaction", h_sigaction);
  NOVA_HLE("signal", h_signal);
  NOVA_HLE("sigprocmask", h_sigprocmask);
  NOVA_HLE("raise", h_ret0);
  NOVA_HLE("kill", h_ret0);

  NOVA_HLE_FAST("setjmp", h_setjmp);
  NOVA_HLE_FAST("_setjmp", h_setjmp);
  NOVA_HLE_FAST("sigsetjmp", h_sigsetjmp);
  NOVA_HLE_FAST("__sigsetjmp", h_sigsetjmp);
  NOVA_HLE("longjmp", h_longjmp);  // changes sp / x19-x30: must run outside the JIT callback
  NOVA_HLE("_longjmp", h_longjmp);
  NOVA_HLE("siglongjmp", h_longjmp);

  NOVA_HLE_FAST("__system_property_get", h_system_property_get);
  NOVA_HLE("sysconf", h_sysconf);
  NOVA_HLE_FAST("getpid", h_getpid);
  NOVA_HLE_FAST("gettid", h_gettid);
  NOVA_HLE("syscall", h_syscall);
  NOVA_HLE("syslog", h_syslog);
  NOVA_HLE("openlog", h_zero);
  NOVA_HLE("closelog", h_zero);

  NOVA_HLE_FAST("rand", h_rand);
  NOVA_HLE_FAST("srand", h_srand);
  NOVA_HLE_FAST("rand_r", h_rand_r);
  NOVA_HLE("qsort", h_qsort);
  NOVA_HLE("bsearch", h_bsearch);

  NOVA_HLE("strerror", h_strerror);
  NOVA_HLE("strerror_r", h_strerror_r);
}
}  // namespace nova::hle::libc
