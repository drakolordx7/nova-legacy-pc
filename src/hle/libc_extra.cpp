// More of the small libc surface the constructors of libNOVA.so reach: random, qsort, ctype, locale stubs, a minimal
// fd layer (/dev/urandom, stdio fds), FILE* output to stdout/stderr, ids, signals as no-ops.
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <string>

#include "core/log.h"
#include "cpu/hle_abi.h"
#include "hle/format.h"
#include "hle/heap.h"
#include "hle/libc.h"

namespace nova::hle::libc {
namespace {
using std::int32_t;
using std::int64_t;
using std::uint32_t;
using std::uint64_t;

constexpr int kENOENT = 2, kEBADF = 9, kEINVAL = 22;
void SetErrno(Cpu& cpu, int e) { mem::W<int32_t>(cpu.errno_addr(), e); }

// ------------------------------------------------------------------------------------------ random / sort
std::mutex g_rand_mu;
uint64_t g_rand = 0x2545F4914F6CDD1Dull;
int32_t h_rand() {
  std::lock_guard lk(g_rand_mu);
  g_rand ^= g_rand << 13;
  g_rand ^= g_rand >> 7;
  g_rand ^= g_rand << 17;
  return static_cast<int32_t>((g_rand >> 33) & 0x7fffffff);
}
void h_srand(uint32_t seed) {
  std::lock_guard lk(g_rand_mu);
  g_rand = (static_cast<uint64_t>(seed) << 32 | seed) ^ 0x2545F4914F6CDD1Dull;
  if (!g_rand) g_rand = 1;
}

void SwapBytes(gaddr a, gaddr b, uint64_t n) {
  std::swap_ranges(g2h(a), g2h(a) + n, g2h(b));
}
// Heapsort in place on guest memory (comparator pointers must address the real elements).
void h_qsort(Cpu& cpu, uint64_t base, uint64_t n, uint64_t size, uint64_t cmp) {
  if (n < 2 || !size) return;
  auto el = [&](uint64_t i) { return base + i * size; };
  auto less = [&](uint64_t i, uint64_t j) { return cpu.Call<int32_t>(cmp, el(i), el(j)) < 0; };
  auto sift = [&](uint64_t root, uint64_t end) {
    for (;;) {
      uint64_t child = 2 * root + 1;
      if (child >= end) return;
      if (child + 1 < end && less(child, child + 1)) ++child;
      if (!less(root, child)) return;
      SwapBytes(el(root), el(child), size);
      root = child;
    }
  };
  for (uint64_t i = n / 2; i-- > 0;) sift(i, n);
  for (uint64_t end = n; end-- > 1;) {
    SwapBytes(el(0), el(end), size);
    sift(0, end);
  }
}
uint64_t h_bsearch(Cpu& cpu, uint64_t key, uint64_t base, uint64_t n, uint64_t size, uint64_t cmp) {
  uint64_t lo = 0, hi = n;
  while (lo < hi) {
    const uint64_t mid = lo + (hi - lo) / 2;
    const int32_t c = cpu.Call<int32_t>(cmp, key, base + mid * size);
    if (c == 0) return base + mid * size;
    if (c < 0) hi = mid;
    else lo = mid + 1;
  }
  return 0;
}

// ------------------------------------------------------------------------------------------ errors, ids
uint64_t StaticString(const std::string& key, const std::string& value) {
  const gaddr a = DefineData("__nova_str_" + key, value.size() + 1, 1);
  std::memcpy(g2h(a), value.c_str(), value.size() + 1);
  return a;
}
uint64_t h_strerror(int32_t e) {
  const char* t = "Unknown error";
  switch (e) {
    case 0: t = "Success"; break;
    case 1: t = "Operation not permitted"; break;
    case 2: t = "No such file or directory"; break;
    case 4: t = "Interrupted system call"; break;
    case 5: t = "I/O error"; break;
    case 9: t = "Bad file descriptor"; break;
    case 11: t = "Try again"; break;
    case 12: t = "Out of memory"; break;
    case 13: t = "Permission denied"; break;
    case 17: t = "File exists"; break;
    case 22: t = "Invalid argument"; break;
    default: break;
  }
  return StaticString("strerror" + std::to_string(e), e > 0 && std::strcmp(t, "Unknown error") == 0 ? "Unknown error " + std::to_string(e) : t);
}
int32_t h_strerror_r(int32_t e, char* buf, uint64_t len) {
  const std::string s = mem::CString(h_strerror(e));
  if (len) {
    std::strncpy(buf, s.c_str(), len - 1);
    buf[len - 1] = 0;
  }
  return 0;
}
int32_t h_getpid() { return 4242; }
int32_t h_gettid(Cpu& cpu) { return static_cast<int32_t>(cpu.thread_id()) + 4242; }
int32_t h_zero() { return 0; }
int32_t h_set_abort_message(const char* m) {
  log::Error("android_set_abort_message: {}", m ? m : "");
  return 0;
}

// ------------------------------------------------------------------------------------------ fds (minimal)
// 0/1/2 are the host stdio; /dev/urandom and /dev/random open as random fds; everything else is ENOENT until the
// VFS arrives in Phase 2.
std::mutex g_fd_mu;
bool g_fd_random[64];
int32_t h_open(Cpu& cpu, const char* path, int32_t, Varargs) {
  const std::string p = path ? path : "";
  if (p == "/dev/urandom" || p == "/dev/random") {
    std::lock_guard lk(g_fd_mu);
    for (int i = 3; i < 64; ++i)
      if (!g_fd_random[i]) {
        g_fd_random[i] = true;
        return i;
      }
  }
  SetErrno(cpu, kENOENT);
  return -1;
}
int32_t h_close(Cpu& cpu, int32_t fd) {
  std::lock_guard lk(g_fd_mu);
  if (fd >= 3 && fd < 64 && g_fd_random[fd]) {
    g_fd_random[fd] = false;
    return 0;
  }
  if (fd >= 0 && fd <= 2) return 0;
  SetErrno(cpu, kEBADF);
  return -1;
}
int64_t h_read(Cpu& cpu, int32_t fd, void* buf, uint64_t n) {
  bool random;
  {
    std::lock_guard lk(g_fd_mu);
    random = fd >= 3 && fd < 64 && g_fd_random[fd];
  }
  if (random) {
    BCryptGenRandom(nullptr, static_cast<PUCHAR>(buf), static_cast<ULONG>(std::min<uint64_t>(n, 1 << 20)), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return static_cast<int64_t>(n);
  }
  if (fd == 0) return 0;  // stdin: EOF
  SetErrno(cpu, kEBADF);
  return -1;
}
int64_t h_write(Cpu& cpu, int32_t fd, const void* buf, uint64_t n) {
  if (fd == 1 || fd == 2) {
    std::fwrite(buf, 1, n, fd == 1 ? stdout : stderr);
    return static_cast<int64_t>(n);
  }
  SetErrno(cpu, kEBADF);
  return -1;
}
int64_t h_lseek(Cpu& cpu, int32_t, int64_t, int32_t) {
  SetErrno(cpu, kEBADF);
  return -1;
}

// ------------------------------------------------------------------------------------------ FILE* (stdout/stderr only)
constexpr uint64_t kFileSize = 152;  // sizeof(FILE) in bionic LP64
FILE* HostFile(uint64_t f) {
  const gaddr sf = FindData("__sF");
  if (sf && f >= sf && f < sf + 3 * kFileSize) {
    const uint64_t i = (f - sf) / kFileSize;
    return i == 0 ? stdin : (i == 1 ? stdout : stderr);
  }
  return nullptr;
}
int32_t h_fprintf(uint64_t f, const char* fmt, Varargs va) {
  const std::string s = FormatPrintf(fmt, va);
  if (FILE* h = HostFile(f)) std::fwrite(s.data(), 1, s.size(), h);
  return static_cast<int32_t>(s.size());
}
int32_t h_vfprintf(uint64_t f, const char* fmt, VaList va) {
  const std::string s = FormatPrintf(fmt, va);
  if (FILE* h = HostFile(f)) std::fwrite(s.data(), 1, s.size(), h);
  return static_cast<int32_t>(s.size());
}
int32_t h_fputs(const char* s, uint64_t f) {
  if (FILE* h = HostFile(f)) std::fputs(s, h);
  return 1;
}
int32_t h_fputc(int32_t c, uint64_t f) {
  if (FILE* h = HostFile(f)) std::fputc(c, h);
  return c;
}
uint64_t h_fwrite(const void* p, uint64_t size, uint64_t n, uint64_t f) {
  if (FILE* h = HostFile(f)) return std::fwrite(p, size, n, h);
  return 0;
}
int32_t h_fflush(uint64_t f) {
  if (FILE* h = HostFile(f)) std::fflush(h);
  return 0;
}
int32_t h_setvbuf(uint64_t, void*, int32_t, uint64_t) { return 0; }
uint64_t h_fopen(Cpu& cpu, const char*, const char*) {
  SetErrno(cpu, kENOENT);
  return 0;
}

// ------------------------------------------------------------------------------------------ ctype
// bionic's `_ctype_` flag table (index c + 1): _U 1, _L 2, _N 4, _S 8, _P 16, _C 32, _X 64, _B 128.
void FillCtype(gaddr t) {
  auto* p = g2h(t);
  p[0] = 0;
  for (int c = 0; c < 256; ++c) {
    std::uint8_t f = 0;
    if (c < 128) {
      if (c >= 'A' && c <= 'Z') f |= 1;
      if (c >= 'a' && c <= 'z') f |= 2;
      if (c >= '0' && c <= '9') f |= 4;
      if (c == ' ') f |= 8 | 128;
      if (c >= 9 && c <= 13) f |= 8 | 32;
      else if (c < 32 || c == 127) f |= 32;
      if (std::ispunct(c)) f |= 16;
      if (std::isxdigit(c)) f |= 64;
    }
    p[1 + c] = f;
  }
}
int32_t h_isalnum(int32_t c) { return (c >= 0 && c < 128) ? std::isalnum(c) != 0 : 0; }
int32_t h_isalpha(int32_t c) { return (c >= 0 && c < 128) ? std::isalpha(c) != 0 : 0; }
int32_t h_iscntrl(int32_t c) { return (c >= 0 && c < 128) ? std::iscntrl(c) != 0 : 0; }
int32_t h_isdigit(int32_t c) { return c >= '0' && c <= '9'; }
int32_t h_islower(int32_t c) { return c >= 'a' && c <= 'z'; }
int32_t h_isupper(int32_t c) { return c >= 'A' && c <= 'Z'; }
int32_t h_ispunct(int32_t c) { return (c >= 0 && c < 128) ? std::ispunct(c) != 0 : 0; }
int32_t h_isspace(int32_t c) { return c == ' ' || (c >= 9 && c <= 13); }
int32_t h_isxdigit(int32_t c) { return (c >= 0 && c < 128) ? std::isxdigit(c) != 0 : 0; }
int32_t h_tolower(int32_t c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int32_t h_toupper(int32_t c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
// *_l variants take a locale_t last and ignore it (C locale only).
int32_t h_isdigit_l(int32_t c, uint64_t) { return h_isdigit(c); }
int32_t h_islower_l(int32_t c, uint64_t) { return h_islower(c); }
int32_t h_isupper_l(int32_t c, uint64_t) { return h_isupper(c); }
int32_t h_isxdigit_l(int32_t c, uint64_t) { return h_isxdigit(c); }
int32_t h_tolower_l(int32_t c, uint64_t) { return h_tolower(c); }
int32_t h_toupper_l(int32_t c, uint64_t) { return h_toupper(c); }
int32_t h_towlower(int32_t c) { return static_cast<int32_t>(std::towlower(static_cast<wint_t>(c))); }
int32_t h_towupper(int32_t c) { return static_cast<int32_t>(std::towupper(static_cast<wint_t>(c))); }
int32_t h_towlower_l(int32_t c, uint64_t) { return h_towlower(c); }
int32_t h_towupper_l(int32_t c, uint64_t) { return h_towupper(c); }
int32_t h_mb_cur_max() { return 4; }
// Wide-character helpers, C locale (ASCII only below 128; wchar_t is 32-bit on Android).
int32_t h_btowc(int32_t c) { return (c >= 0 && c < 128) ? c : -1; }
int32_t h_wctob(int32_t wc) { return (wc >= 0 && wc < 128) ? wc : -1; }
int32_t h_mbtowc(int32_t* pwc, const char* s, uint64_t n) {
  if (!s) return 0;
  if (n == 0) return -1;
  const auto c = static_cast<unsigned char>(*s);
  if (pwc) *pwc = c;
  return c ? 1 : 0;
}
// wctype_t is an opaque long: 1-based index into this list.
constexpr const char* kWctypeNames[] = {"alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower", "print", "punct", "space", "upper", "xdigit"};
int64_t h_wctype(const char* name) {
  for (int i = 0; i < 12; ++i)
    if (std::strcmp(name, kWctypeNames[i]) == 0) return i + 1;
  return 0;
}
int32_t h_iswctype(int32_t wc, int64_t t) {
  const auto w = static_cast<wint_t>(wc);
  switch (t) {
    case 1: return std::iswalnum(w) != 0;
    case 2: return std::iswalpha(w) != 0;
    case 3: return w == L' ' || w == 9;  // blank: space or tab
    case 4: return std::iswcntrl(w) != 0;
    case 5: return std::iswdigit(w) != 0;
    case 6: return std::iswgraph(w) != 0;
    case 7: return std::iswlower(w) != 0;
    case 8: return std::iswprint(w) != 0;
    case 9: return std::iswpunct(w) != 0;
    case 10: return std::iswspace(w) != 0;
    case 11: return std::iswupper(w) != 0;
    case 12: return std::iswxdigit(w) != 0;
    default: return 0;
  }
}

// ------------------------------------------------------------------------------------------ locale (C only)
uint64_t h_newlocale(int32_t, const char*, uint64_t) { return DefineData("__nova_locale", 16, 16); }
uint64_t h_uselocale(uint64_t) { return DefineData("__nova_locale", 16, 16); }
void h_freelocale(uint64_t) {}
uint64_t h_setlocale(int32_t, const char*) { return StaticString("locale_C", "C"); }
uint64_t h_localeconv() {
  const gaddr a = DefineData("__nova_lconv", 128, 16);
  mem::W64(a, StaticString("lconv_dp", "."));
  for (int i = 1; i < 10; ++i) mem::W64(a + 8 * i, StaticString("lconv_empty", ""));
  std::memset(g2h(a + 80), 0xFF, 14);  // CHAR_MAX for the numeric fields
  return a;
}

// ------------------------------------------------------------------------------------------ strings, misc
uint64_t h_strspn_like(const char* s, const char* set, bool accept) {
  uint64_t n = 0;
  for (; s[n]; ++n) {
    const bool in = std::strchr(set, s[n]) != nullptr;
    if (in != accept) break;
  }
  return n;
}
uint64_t h_strspn(const char* s, const char* a) { return h_strspn_like(s, a, true); }
uint64_t h_strcspn(const char* s, const char* r) { return h_strspn_like(s, r, false); }
char* h_strpbrk(const char* s, const char* a) {
  const char* p = std::strpbrk(s, a);
  return const_cast<char*>(p);
}
uint64_t h_strtok_r(char* s, const char* delim, uint64_t* save) {
  char* cur = s ? s : g2h<char>(*save);
  if (!cur) return 0;
  cur += std::strspn(cur, delim);
  if (!*cur) {
    *save = 0;
    return 0;
  }
  char* tok = cur;
  cur += std::strcspn(cur, delim);
  if (*cur) {
    *cur++ = 0;
    *save = h2g(cur);
  } else {
    *save = h2g(cur);
  }
  return h2g(tok);
}
double h_difftime(int64_t a, int64_t b) { return static_cast<double>(a - b); }
int64_t h_clock() { return static_cast<int64_t>(std::clock()) * 1000; }
int32_t h_sem_zero(uint64_t) { return 0; }
int32_t h_sigaction(int32_t, uint64_t, uint64_t) { return 0; }
uint64_t h_signal(int32_t, uint64_t) { return 0; }
int32_t h_pthread_attr_init(uint64_t a) {
  std::memset(g2h(a), 0, 56);
  return 0;
}
int32_t h_ret0_1(uint64_t) { return 0; }
int32_t h_ret0_2(uint64_t, uint64_t) { return 0; }
}  // namespace

void RegisterExtra() {
  FillCtype(DefineData("_ctype_", 257, 16));
  DefineData("__sF", 3 * kFileSize, 16);

  NOVA_HLE_FAST("rand", h_rand);
  NOVA_HLE_FAST("srand", h_srand);
  NOVA_HLE("qsort", h_qsort);
  NOVA_HLE("bsearch", h_bsearch);
  NOVA_HLE("strerror", h_strerror);
  NOVA_HLE("strerror_r", h_strerror_r);
  NOVA_HLE_FAST("getpid", h_getpid);
  NOVA_HLE_FAST("gettid", h_gettid);
  for (const char* n : {"getuid", "geteuid", "getgid", "getegid"}) NOVA_HLE_FAST(n, h_zero);
  NOVA_HLE("android_set_abort_message", h_set_abort_message);

  NOVA_HLE("open", h_open);
  NOVA_HLE("close", h_close);
  NOVA_HLE("read", h_read);
  NOVA_HLE("write", h_write);
  NOVA_HLE("lseek", h_lseek);

  NOVA_HLE("fprintf", h_fprintf);
  NOVA_HLE("vfprintf", h_vfprintf);
  NOVA_HLE("fputs", h_fputs);
  NOVA_HLE("fputc", h_fputc);
  NOVA_HLE("putc", h_fputc);
  NOVA_HLE("fwrite", h_fwrite);
  NOVA_HLE("fflush", h_fflush);
  NOVA_HLE("setvbuf", h_setvbuf);
  NOVA_HLE("fopen", h_fopen);

  NOVA_HLE_FAST("isalnum", h_isalnum);
  NOVA_HLE_FAST("isalpha", h_isalpha);
  NOVA_HLE_FAST("iscntrl", h_iscntrl);
  NOVA_HLE_FAST("isdigit", h_isdigit);
  NOVA_HLE_FAST("islower", h_islower);
  NOVA_HLE_FAST("isupper", h_isupper);
  NOVA_HLE_FAST("ispunct", h_ispunct);
  NOVA_HLE_FAST("isspace", h_isspace);
  NOVA_HLE_FAST("isxdigit", h_isxdigit);
  NOVA_HLE_FAST("tolower", h_tolower);
  NOVA_HLE_FAST("toupper", h_toupper);
  NOVA_HLE_FAST("isdigit_l", h_isdigit_l);
  NOVA_HLE_FAST("islower_l", h_islower_l);
  NOVA_HLE_FAST("isupper_l", h_isupper_l);
  NOVA_HLE_FAST("isxdigit_l", h_isxdigit_l);
  NOVA_HLE_FAST("tolower_l", h_tolower_l);
  NOVA_HLE_FAST("toupper_l", h_toupper_l);
  NOVA_HLE_FAST("towlower", h_towlower);
  NOVA_HLE_FAST("towupper", h_towupper);
  NOVA_HLE_FAST("towlower_l", h_towlower_l);
  NOVA_HLE_FAST("towupper_l", h_towupper_l);
  NOVA_HLE_FAST("__ctype_get_mb_cur_max", h_mb_cur_max);
  NOVA_HLE_FAST("btowc", h_btowc);
  NOVA_HLE_FAST("wctob", h_wctob);
  NOVA_HLE_FAST("mbtowc", h_mbtowc);
  NOVA_HLE_FAST("wctype", h_wctype);
  NOVA_HLE_FAST("iswctype", h_iswctype);

  NOVA_HLE("newlocale", h_newlocale);
  NOVA_HLE("uselocale", h_uselocale);
  NOVA_HLE("freelocale", h_freelocale);
  NOVA_HLE("setlocale", h_setlocale);
  NOVA_HLE("localeconv", h_localeconv);

  NOVA_HLE_FAST("strspn", h_strspn);
  NOVA_HLE_FAST("strcspn", h_strcspn);
  NOVA_HLE_FAST("strpbrk", h_strpbrk);
  NOVA_HLE("strtok_r", h_strtok_r);
  NOVA_HLE_FAST("difftime", h_difftime);
  NOVA_HLE("clock", h_clock);
  NOVA_HLE("sigaction", h_sigaction);
  NOVA_HLE("signal", h_signal);
  NOVA_HLE("pthread_attr_init", h_pthread_attr_init);
  NOVA_HLE("pthread_attr_destroy", h_ret0_1);
  NOVA_HLE("pthread_attr_setdetachstate", h_ret0_2);
  NOVA_HLE("pthread_attr_setstacksize", h_ret0_2);
  for (const char* n : {"sem_init", "sem_destroy", "sem_post", "sem_wait", "sem_trywait"}) NOVA_HLE(n, h_sem_zero);
}
}  // namespace nova::hle::libc
