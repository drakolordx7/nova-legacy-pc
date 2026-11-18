// Wide characters: bionic's wchar_t is a 32-bit unsigned int (the host's is 16-bit UTF-16), so everything here works
// on uint32_t arrays in guest memory. Multibyte text is UTF-8. Character classes come from Win32 GetStringTypeW
// (locale independent, BMP only; code points above U+FFFF are "not alpha/print...").
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "hle/format.h"
#include "hle/libc_internal.h"
#include "hle/numconv.h"

namespace nova::hle::libc {
namespace {
using std::int32_t;
using std::int64_t;
using std::uint16_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;
using wc_t = uint32_t;
constexpr uint32_t kWEOF = 0xFFFFFFFFu;
constexpr uint64_t kErr = ~0ull, kIncomplete = ~0ull - 1;  // (size_t)-1, (size_t)-2

// ---- classification ----
uint16_t WType(wc_t c) {
  static std::atomic<uint16_t> cache[65536];
  static std::once_flag init;
  std::call_once(init, [] {
    for (auto& e : cache) e.store(0xFFFF, std::memory_order_relaxed);
  });
  if (c >= 0x10000 || (c >= 0xD800 && c < 0xE000)) return 0;
  uint16_t v = cache[c].load(std::memory_order_relaxed);
  if (v == 0xFFFF) {
    WCHAR ch = static_cast<WCHAR>(c);
    WORD t = 0;
    GetStringTypeW(CT_CTYPE1, &ch, 1, &t);
    v = t;
    cache[c].store(v, std::memory_order_relaxed);
  }
  return v;
}
bool IsAlpha(wc_t c) { return (WType(c) & C1_ALPHA) != 0; }
bool IsUpper(wc_t c) { return (WType(c) & C1_UPPER) != 0; }
bool IsLower(wc_t c) { return (WType(c) & C1_LOWER) != 0; }
bool IsDigit(wc_t c) { return c >= '0' && c <= '9'; }
bool IsXdigit(wc_t c) { return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool NoBreakSpace(wc_t c) { return c == 0xA0 || c == 0x2007 || c == 0x202F; }
bool IsSpace(wc_t c) { return c < 0x80 ? (c == ' ' || (c >= 9 && c <= 13)) : ((WType(c) & C1_SPACE) && !NoBreakSpace(c)); }
bool IsBlank(wc_t c) { return c == ' ' || c == '\t' || (c >= 0x80 && (WType(c) & C1_BLANK) && !NoBreakSpace(c)); }
bool IsCntrl(wc_t c) { return (WType(c) & C1_CNTRL) != 0; }
bool IsPunct(wc_t c) { return (WType(c) & C1_PUNCT) != 0 && !IsAlpha(c) && !IsDigit(c); }
bool IsPrint(wc_t c) { return (WType(c) & C1_DEFINED) && !IsCntrl(c); }
bool IsGraph(wc_t c) { return IsPrint(c) && !IsSpace(c); }
bool IsAlnum(wc_t c) { return IsAlpha(c) || IsDigit(c); }

wc_t MapCase(wc_t c, DWORD flag) {
  if (c < 0x80) return flag == LCMAP_LOWERCASE ? (c >= 'A' && c <= 'Z' ? c + 32 : c) : (c >= 'a' && c <= 'z' ? c - 32 : c);
  if (c >= 0x10000 || (c >= 0xD800 && c < 0xE000)) return c;
  WCHAR in = static_cast<WCHAR>(c), out = in;
  LCMapStringEx(LOCALE_NAME_INVARIANT, flag, &in, 1, &out, 1, nullptr, nullptr, 0);
  return out;
}
wc_t h_towlower(wc_t c) { return MapCase(c, LCMAP_LOWERCASE); }
wc_t h_towupper(wc_t c) { return MapCase(c, LCMAP_UPPERCASE); }
wc_t h_towlower_l(wc_t c, uint64_t) { return MapCase(c, LCMAP_LOWERCASE); }
wc_t h_towupper_l(wc_t c, uint64_t) { return MapCase(c, LCMAP_UPPERCASE); }

#define ISW(name, fn)                                          \
  int32_t h_isw##name(wc_t c) { return fn(c) ? 1 : 0; }         \
  int32_t h_isw##name##_l(wc_t c, uint64_t) { return fn(c) ? 1 : 0; }
ISW(alnum, IsAlnum) ISW(alpha, IsAlpha) ISW(blank, IsBlank) ISW(cntrl, IsCntrl) ISW(digit, IsDigit) ISW(graph, IsGraph)
ISW(lower, IsLower) ISW(print, IsPrint) ISW(punct, IsPunct) ISW(space, IsSpace) ISW(upper, IsUpper) ISW(xdigit, IsXdigit)
#undef ISW

// wctype_t: 1-based index into this list
constexpr const char* kWctypeNames[] = {"alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower", "print", "punct", "space", "upper", "xdigit"};
int64_t h_wctype(const char* name) {
  for (int i = 0; i < 12; ++i)
    if (std::strcmp(name, kWctypeNames[i]) == 0) return i + 1;
  return 0;
}
int64_t h_wctype_l(const char* name, uint64_t) { return h_wctype(name); }
int32_t h_iswctype(wc_t c, int64_t t) {
  switch (t) {
    case 1: return IsAlnum(c); case 2: return IsAlpha(c); case 3: return IsBlank(c); case 4: return IsCntrl(c);
    case 5: return IsDigit(c); case 6: return IsGraph(c); case 7: return IsLower(c); case 8: return IsPrint(c);
    case 9: return IsPunct(c); case 10: return IsSpace(c); case 11: return IsUpper(c); case 12: return IsXdigit(c);
    default: return 0;
  }
}
int32_t h_iswctype_l(wc_t c, int64_t t, uint64_t) { return h_iswctype(c, t); }

// ---- UTF-8 <-> wchar ----
// Decodes one code point from [s, s+n). Returns the byte count (>0), 0 if more bytes are needed, -1 if invalid.
int Decode(const uint8_t* s, uint64_t n, wc_t& out) {
  if (!n) return 0;
  const uint8_t c = s[0];
  if (c < 0x80) {
    out = c;
    return 1;
  }
  int len;
  wc_t cp;
  if (c >= 0xC2 && c < 0xE0) len = 2, cp = c & 0x1F;
  else if (c >= 0xE0 && c < 0xF0) len = 3, cp = c & 0x0F;
  else if (c >= 0xF0 && c < 0xF5) len = 4, cp = c & 0x07;
  else return -1;
  for (int i = 1; i < len; ++i) {
    if (static_cast<uint64_t>(i) >= n) return 0;
    if ((s[i] & 0xC0) != 0x80) return -1;
    cp = (cp << 6) | (s[i] & 0x3F);
  }
  if ((len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) return -1;
  out = cp;
  return len;
}
int Encode(wc_t cp, char* out) {
  if (cp < 0x80) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    out[0] = static_cast<char>(0xC0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp >= 0xD800 && cp < 0xE000) return -1;
  if (cp < 0x10000) {
    out[0] = static_cast<char>(0xE0 | (cp >> 12));
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
  }
  if (cp > 0x10FFFF) return -1;
  out[0] = static_cast<char>(0xF0 | (cp >> 18));
  out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
  out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  out[3] = static_cast<char>(0x80 | (cp & 0x3F));
  return 4;
}
// mbstate_t (8 bytes in the guest): byte 0 = pending byte count, bytes 1..3 = the pending bytes.
struct MbState {
  uint8_t n;
  uint8_t b[3];
};
MbState g_mbstate{};  // used when the guest passes a NULL state

uint64_t h_mbrtowc(Cpu& cpu, wc_t* pwc, const char* s, uint64_t n, MbState* ps) {
  MbState& st = ps ? *ps : g_mbstate;
  if (!s) {  // reset
    st = {};
    return 0;
  }
  uint8_t buf[4];
  const int have0 = st.n;
  int have = have0;
  std::memcpy(buf, st.b, have0);
  uint64_t used = 0;
  for (;;) {
    wc_t cp = 0;
    const int r = Decode(buf, static_cast<uint64_t>(have), cp);
    if (r > 0) {
      st = {};
      if (pwc) *pwc = cp;
      return cp == 0 ? 0 : static_cast<uint64_t>(r - have0);
    }
    if (r < 0) {
      st = {};
      SetErrno(cpu, err::ILSEQ);
      return kErr;
    }
    if (used >= n) {  // the character is not complete yet: remember what we have
      st.n = static_cast<uint8_t>(have);
      std::memcpy(st.b, buf, static_cast<size_t>(have));
      return kIncomplete;
    }
    buf[have++] = static_cast<uint8_t>(s[used++]);
  }
}
uint64_t h_mbrlen(Cpu& cpu, const char* s, uint64_t n, MbState* ps) { return h_mbrtowc(cpu, nullptr, s, n, ps); }
int32_t h_mbtowc(Cpu& cpu, wc_t* pwc, const char* s, uint64_t n) {
  if (!s) return 0;  // no shift states
  if (!n) {
    SetErrno(cpu, err::ILSEQ);
    return -1;
  }
  wc_t cp = 0;
  const int r = Decode(reinterpret_cast<const uint8_t*>(s), n, cp);
  if (r <= 0) {
    SetErrno(cpu, err::ILSEQ);
    return -1;
  }
  if (pwc) *pwc = cp;
  return cp == 0 ? 0 : r;
}
uint64_t h_wcrtomb(Cpu& cpu, char* s, wc_t wc, MbState* ps) {
  if (ps) *ps = {};
  if (!s) return 1;
  const int n = Encode(wc, s);
  if (n < 0) {
    SetErrno(cpu, err::ILSEQ);
    return kErr;
  }
  return static_cast<uint64_t>(n);
}
uint32_t h_btowc(int32_t c) { return (c >= 0 && c < 0x80) ? static_cast<uint32_t>(c) : kWEOF; }
int32_t h_wctob(wc_t wc) { return wc < 0x80 ? static_cast<int32_t>(wc) : -1; }

// size_t mbsnrtowcs(wchar_t* dst, const char** src, size_t nms, size_t len, mbstate_t*)
uint64_t h_mbsnrtowcs(Cpu& cpu, wc_t* dst, uint64_t* src, uint64_t nms, uint64_t len, MbState*) {
  const uint8_t* s = g2h<uint8_t>(*src);
  uint64_t used = 0, count = 0;
  while (used < nms) {
    if (dst && count >= len) break;
    wc_t cp = 0;
    const int r = Decode(s + used, nms - used, cp);
    if (r < 0) {
      SetErrno(cpu, err::ILSEQ);
      if (dst) *src = h2g(s + used);
      return kErr;
    }
    if (r == 0) break;  // truncated sequence at the end of the window
    if (cp == 0) {
      if (dst) {
        dst[count] = 0;
        *src = 0;
      }
      return count;
    }
    if (dst) dst[count] = cp;
    ++count;
    used += static_cast<uint64_t>(r);
  }
  if (dst) *src = h2g(s + used);
  return count;
}
uint64_t h_mbsrtowcs(Cpu& cpu, wc_t* dst, uint64_t* src, uint64_t len, MbState* ps) {
  return h_mbsnrtowcs(cpu, dst, src, ~0ull >> 1, len, ps);
}
// size_t wcsnrtombs(char* dst, const wchar_t** src, size_t nwc, size_t len, mbstate_t*)
uint64_t h_wcsnrtombs(Cpu& cpu, char* dst, uint64_t* src, uint64_t nwc, uint64_t len, MbState*) {
  const wc_t* s = g2h<wc_t>(*src);
  uint64_t i = 0, count = 0;
  char tmp[4];
  for (; i < nwc; ++i) {
    const int n = Encode(s[i], tmp);
    if (n < 0) {
      SetErrno(cpu, err::ILSEQ);
      if (dst) *src = h2g(s + i);
      return kErr;
    }
    if (dst && count + static_cast<uint64_t>(n) > len) break;
    if (dst) std::memcpy(dst + count, tmp, static_cast<size_t>(n));
    if (s[i] == 0) {
      if (dst) *src = 0;
      return count;
    }
    count += static_cast<uint64_t>(n);
  }
  if (dst) *src = h2g(s + i);
  return count;
}

// ---- wide string / memory functions ----
uint64_t h_wcslen(const wc_t* s) {
  uint64_t n = 0;
  while (s[n]) ++n;
  return n;
}
int32_t h_wcscmp(const wc_t* a, const wc_t* b) {
  while (*a && *a == *b) ++a, ++b;
  return *a < *b ? -1 : (*a > *b ? 1 : 0);
}
int32_t h_wcscoll(const wc_t* a, const wc_t* b) { return h_wcscmp(a, b); }
int32_t h_wcscoll_l(const wc_t* a, const wc_t* b, uint64_t) { return h_wcscmp(a, b); }
wc_t* h_wcscpy(wc_t* d, const wc_t* s) {
  wc_t* r = d;
  while ((*d++ = *s++)) {}
  return r;
}
uint64_t h_wcsxfrm(wc_t* dst, const wc_t* src, uint64_t n) {
  const uint64_t len = h_wcslen(src);
  if (n) {
    const uint64_t k = std::min<uint64_t>(len, n - 1);
    std::memcpy(dst, src, k * 4);
    dst[k] = 0;
  }
  return len;
}
uint64_t h_wcsxfrm_l(wc_t* dst, const wc_t* src, uint64_t n, uint64_t) { return h_wcsxfrm(dst, src, n); }
wc_t* h_wmemchr(const wc_t* s, wc_t c, uint64_t n) {
  for (uint64_t i = 0; i < n; ++i)
    if (s[i] == c) return const_cast<wc_t*>(s + i);
  return nullptr;
}
int32_t h_wmemcmp(const wc_t* a, const wc_t* b, uint64_t n) {
  for (uint64_t i = 0; i < n; ++i)
    if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  return 0;
}
wc_t* h_wmemcpy(wc_t* d, const wc_t* s, uint64_t n) {
  std::memcpy(d, s, n * 4);
  return d;
}
wc_t* h_wmemmove(wc_t* d, const wc_t* s, uint64_t n) {
  std::memmove(d, s, n * 4);
  return d;
}
wc_t* h_wmemset(wc_t* d, wc_t c, uint64_t n) {
  for (uint64_t i = 0; i < n; ++i) d[i] = c;
  return d;
}

// ---- wide number parsing ----
int64_t h_wcstol(Cpu& cpu, const wc_t* s, uint64_t* end, int32_t base) {
  const auto r = ParseInteger(s, base);
  bool range;
  const int64_t v = ToSigned(r, range);
  if (range) SetErrno(cpu, err::RANGE);
  if (end) *end = h2g(r.end);
  return v;
}
uint64_t h_wcstoul(Cpu& cpu, const wc_t* s, uint64_t* end, int32_t base) {
  const auto r = ParseInteger(s, base);
  bool range;
  const uint64_t v = ToUnsigned(r, range);
  if (range) SetErrno(cpu, err::RANGE);
  if (end) *end = h2g(r.end);
  return v;
}
// Narrow a leading ASCII run of `s` for the host's strtod; reports how many wide chars were consumed.
double WideToDouble(Cpu& cpu, const wc_t* s, uint64_t* end) {
  std::string narrow;
  for (const wc_t* p = s; *p && *p < 0x80 && narrow.size() < 4096; ++p) narrow.push_back(static_cast<char>(*p));
  errno = 0;
  char* e = nullptr;
  const double v = std::strtod(narrow.c_str(), &e);
  if (errno == ERANGE) SetErrno(cpu, err::RANGE);
  if (end) *end = h2g(s + (e - narrow.c_str()));
  return v;
}
double h_wcstod(Cpu& cpu, const wc_t* s, uint64_t* end) { return WideToDouble(cpu, s, end); }
float h_wcstof(Cpu& cpu, const wc_t* s, uint64_t* end) { return static_cast<float>(WideToDouble(cpu, s, end)); }
void raw_wcstold(Cpu& cpu) {  // (const wchar_t*, wchar_t** end) -> long double in q0
  const wc_t* s = g2h<wc_t>(cpu.x(0));
  const gaddr endp = cpu.x(1);
  uint64_t e = 0;
  const double v = WideToDouble(cpu, s, &e);
  if (endp) mem::W64(endp, e);
  cpu.set_v(0, DoubleToQuad(v));
}

// ---- formatted output ----
std::string WideToUtf8(const wc_t* w, uint64_t n = ~0ull) {
  std::string s;
  char b[4];
  for (uint64_t i = 0; i < n && w[i]; ++i) {
    const int k = Encode(w[i], b);
    if (k > 0) s.append(b, static_cast<size_t>(k));
  }
  return s;
}
std::vector<wc_t> Utf8ToWide(const std::string& s) {
  std::vector<wc_t> out;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(s.data());
  uint64_t i = 0;
  while (i < s.size()) {
    wc_t cp;
    const int r = Decode(p + i, s.size() - i, cp);
    if (r <= 0) {
      out.push_back(p[i++]);  // invalid byte: pass it through
      continue;
    }
    out.push_back(cp);
    i += static_cast<uint64_t>(r);
  }
  return out;
}
int32_t StoreWide(wc_t* buf, uint64_t n, const std::string& utf8) {
  const std::vector<wc_t> w = Utf8ToWide(utf8);
  if (n == 0) return -1;
  if (w.size() + 1 > n) {  // does not fit: truncate, report failure like bionic
    std::memcpy(buf, w.data(), (n - 1) * 4);
    buf[n - 1] = 0;
    return -1;
  }
  std::memcpy(buf, w.data(), w.size() * 4);
  buf[w.size()] = 0;
  return static_cast<int32_t>(w.size());
}
int32_t h_swprintf(wc_t* buf, uint64_t n, const wc_t* fmt, Varargs va) {
  const std::string f = WideToUtf8(fmt);
  return StoreWide(buf, n, FormatPrintf(f.c_str(), va));
}
uint64_t h_wcsftime(wc_t* dst, uint64_t max, const wc_t* fmt, const int32_t* tm) {
  const std::string f = WideToUtf8(fmt);
  int64_t gmtoff;
  uint64_t zone;
  std::memcpy(&gmtoff, reinterpret_cast<const uint8_t*>(tm) + 40, 8);
  std::memcpy(&zone, reinterpret_cast<const uint8_t*>(tm) + 48, 8);
  const std::string s = StrftimeImpl(f.c_str(), tm, gmtoff, zone ? g2h<char>(zone) : nullptr);
  const std::vector<wc_t> w = Utf8ToWide(s);
  if (!max || w.size() + 1 > max) return 0;
  std::memcpy(dst, w.data(), w.size() * 4);
  dst[w.size()] = 0;
  return w.size();
}
}  // namespace

void RegisterWchar() {
  NOVA_HLE_FAST("towlower", h_towlower);
  NOVA_HLE_FAST("towupper", h_towupper);
  NOVA_HLE_FAST("towlower_l", h_towlower_l);
  NOVA_HLE_FAST("towupper_l", h_towupper_l);
#define REG_ISW(n)                               \
  NOVA_HLE_FAST("isw" #n, h_isw##n);             \
  NOVA_HLE_FAST("isw" #n "_l", h_isw##n##_l)
  REG_ISW(alnum); REG_ISW(alpha); REG_ISW(blank); REG_ISW(cntrl); REG_ISW(digit); REG_ISW(graph);
  REG_ISW(lower); REG_ISW(print); REG_ISW(punct); REG_ISW(space); REG_ISW(upper); REG_ISW(xdigit);
#undef REG_ISW
  NOVA_HLE_FAST("wctype", h_wctype);
  NOVA_HLE_FAST("wctype_l", h_wctype_l);
  NOVA_HLE_FAST("iswctype", h_iswctype);
  NOVA_HLE_FAST("iswctype_l", h_iswctype_l);

  NOVA_HLE_FAST("mbrtowc", h_mbrtowc);
  NOVA_HLE_FAST("mbrlen", h_mbrlen);
  NOVA_HLE_FAST("mbtowc", h_mbtowc);
  NOVA_HLE_FAST("wcrtomb", h_wcrtomb);
  NOVA_HLE_FAST("btowc", h_btowc);
  NOVA_HLE_FAST("wctob", h_wctob);
  NOVA_HLE_FAST("mbsnrtowcs", h_mbsnrtowcs);
  NOVA_HLE_FAST("mbsrtowcs", h_mbsrtowcs);
  NOVA_HLE_FAST("wcsnrtombs", h_wcsnrtombs);

  NOVA_HLE_FAST("wcslen", h_wcslen);
  NOVA_HLE_FAST("wcscmp", h_wcscmp);
  NOVA_HLE_FAST("wcscoll", h_wcscoll);
  NOVA_HLE_FAST("wcscoll_l", h_wcscoll_l);
  NOVA_HLE_FAST("wcscpy", h_wcscpy);
  NOVA_HLE_FAST("wcsxfrm", h_wcsxfrm);
  NOVA_HLE_FAST("wcsxfrm_l", h_wcsxfrm_l);
  NOVA_HLE_FAST("wmemchr", h_wmemchr);
  NOVA_HLE_FAST("wmemcmp", h_wmemcmp);
  NOVA_HLE_FAST("wmemcpy", h_wmemcpy);
  NOVA_HLE_FAST("wmemmove", h_wmemmove);
  NOVA_HLE_FAST("wmemset", h_wmemset);

  NOVA_HLE_FAST("wcstol", h_wcstol);
  NOVA_HLE_FAST("wcstoll", h_wcstol);
  NOVA_HLE_FAST("wcstoul", h_wcstoul);
  NOVA_HLE_FAST("wcstoull", h_wcstoul);
  NOVA_HLE_FAST("wcstod", h_wcstod);
  NOVA_HLE_FAST("wcstof", h_wcstof);
  hle::Register("wcstold", raw_wcstold, hle::kFast);

  NOVA_HLE("swprintf", h_swprintf);
  NOVA_HLE("wcsftime", h_wcsftime);
}
}  // namespace nova::hle::libc
