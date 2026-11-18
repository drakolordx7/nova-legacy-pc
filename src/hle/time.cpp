// Time: clocks (Linux clock ids), sleeping, and the broken-down-time functions on bionic's 56-byte LP64 struct tm
// {9 ints, pad, long tm_gmtoff, const char* tm_zone}. localtime/mktime follow the PC's time zone (UCRT); the rest
// (calendar arithmetic, strftime, strptime) is done here so formats and fields are bionic's, not the CRT's.
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "hle/libc_internal.h"

namespace nova::hle::libc {
namespace {
using std::int32_t;
using std::int64_t;
using std::uint32_t;
using std::uint64_t;

struct GTm {
  int32_t sec, min, hour, mday, mon, year, wday, yday, isdst;
  int32_t pad;
  int64_t gmtoff;
  uint64_t zone;
};
static_assert(sizeof(GTm) == 56);

// ---- calendar arithmetic (proleptic Gregorian, days since 1970-01-01) ----
int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}
void CivilFromDays(int64_t z, int64_t& y, unsigned& m, unsigned& d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y += m <= 2;
}
bool IsLeap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

// Fills the calendar fields (not isdst/gmtoff/zone) for `t` seconds since the epoch.
void Breakdown(int64_t t, GTm& tm) {
  int64_t days = t / 86400, rem = t % 86400;
  if (rem < 0) {
    rem += 86400;
    --days;
  }
  int64_t y;
  unsigned m, d;
  CivilFromDays(days, y, m, d);
  tm.sec = static_cast<int32_t>(rem % 60);
  tm.min = static_cast<int32_t>(rem / 60 % 60);
  tm.hour = static_cast<int32_t>(rem / 3600);
  tm.mday = static_cast<int32_t>(d);
  tm.mon = static_cast<int32_t>(m) - 1;
  tm.year = static_cast<int32_t>(y - 1900);
  tm.wday = static_cast<int32_t>(((days % 7) + 11) % 7);  // 1970-01-01 was a Thursday
  tm.yday = static_cast<int32_t>(days - DaysFromCivil(y, 1, 1));
}
// UTC seconds for the (possibly denormalised) fields, ignoring isdst/gmtoff.
int64_t TimegmFields(const GTm& tm) {
  int64_t y = tm.year + 1900ll, mon = tm.mon;
  y += mon / 12;
  mon %= 12;
  if (mon < 0) {
    mon += 12;
    --y;
  }
  const int64_t days = DaysFromCivil(y, static_cast<unsigned>(mon) + 1, 1) + (tm.mday - 1);
  return ((days * 24 + tm.hour) * 60 + tm.min) * 60 + tm.sec;
}

// ---- host time zone ----
struct LocalInfo {
  int64_t offset;  // seconds east of UTC
  bool dst;
};
LocalInfo LocalAt(int64_t t) {
  __time64_t tt = t;
  std::tm h{};
  if (t >= 0 && _localtime64_s(&h, &tt) == 0) {
    GTm g{};
    g.sec = h.tm_sec; g.min = h.tm_min; g.hour = h.tm_hour; g.mday = h.tm_mday; g.mon = h.tm_mon; g.year = h.tm_year;
    return {TimegmFields(g) - t, h.tm_isdst > 0};
  }
  long tz = 0;
  _get_timezone(&tz);
  return {-static_cast<int64_t>(tz), false};
}
std::string ZoneName(int64_t off, bool dst) {
  (void)dst;
  if (off == 0) return "UTC";
  char b[16];
  const int64_t a = off < 0 ? -off : off;
  if (a % 3600 == 0) std::snprintf(b, sizeof b, "GMT%c%lld", off < 0 ? '-' : '+', static_cast<long long>(a / 3600));
  else std::snprintf(b, sizeof b, "GMT%c%lld:%02lld", off < 0 ? '-' : '+', static_cast<long long>(a / 3600), static_cast<long long>(a / 60 % 60));
  return b;
}
void FillLocal(int64_t t, GTm& tm) {
  const LocalInfo li = LocalAt(t);
  Breakdown(t + li.offset, tm);
  tm.isdst = li.dst ? 1 : 0;
  tm.gmtoff = li.offset;
  tm.zone = InternString(ZoneName(li.offset, li.dst));
}
void FillUtc(int64_t t, GTm& tm) {
  Breakdown(t, tm);
  tm.isdst = 0;
  tm.gmtoff = 0;
  tm.zone = InternString("UTC");
}

GTm* H_gmtime_r(const int64_t* t, GTm* out) {
  FillUtc(*t, *out);
  return out;
}
GTm* H_localtime_r(const int64_t* t, GTm* out) {
  FillLocal(*t, *out);
  return out;
}
uint64_t h_gmtime_r(const int64_t* t, GTm* out) { return h2g(H_gmtime_r(t, out)); }
uint64_t h_localtime_r(const int64_t* t, GTm* out) { return h2g(H_localtime_r(t, out)); }
uint64_t h_gmtime(const int64_t* t) { return h2g(H_gmtime_r(t, g2h<GTm>(ThreadBuffer(0, 56)))); }
uint64_t h_localtime(const int64_t* t) { return h2g(H_localtime_r(t, g2h<GTm>(ThreadBuffer(0, 56)))); }
int64_t h_timegm(GTm* tm) {
  const int64_t t = TimegmFields(*tm);
  FillUtc(t, *tm);
  return t;
}
int64_t h_mktime(Cpu& cpu, GTm* tm) {
  // Normalise through the CRT so DST/zone rules are the PC's.
  std::tm h{};
  h.tm_sec = tm->sec; h.tm_min = tm->min; h.tm_hour = tm->hour; h.tm_mday = tm->mday; h.tm_mon = tm->mon;
  h.tm_year = tm->year; h.tm_isdst = tm->isdst;
  const __time64_t t = _mktime64(&h);
  if (t == -1 && !(h.tm_year == 69 && h.tm_mon == 11 && h.tm_mday == 31)) {
    SetErrno(cpu, err::OVERFLOW_);
    return -1;
  }
  FillLocal(t, *tm);
  return t;
}

// ---- strftime ----
const char* const kDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
const char* const kMonths[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};

void IsoWeek(const int32_t* f, int& iso_year, int& week) {
  const int year = f[5] + 1900, yday = f[7], wday = f[6];
  const int wd = (wday + 6) % 7;  // Monday = 0
  week = (yday - wd + 10) / 7;
  iso_year = year;
  auto weeks_in = [](int y) {
    const int64_t jan1 = (DaysFromCivil(y, 1, 1) % 7 + 11) % 7;  // weekday of Jan 1 (Sunday = 0)
    return (jan1 == 4 || (IsLeap(y) && jan1 == 3)) ? 53 : 52;
  };
  if (week < 1) {
    iso_year = year - 1;
    week = weeks_in(iso_year);
  } else if (week > weeks_in(year)) {
    week = 1;
    iso_year = year + 1;
  }
}

void Num(std::string& out, int64_t v, int width, char pad, char flag) {
  if (flag == '-') pad = 0;
  else if (flag == '_') pad = ' ';
  else if (flag == '0') pad = '0';
  char b[32];
  std::snprintf(b, sizeof b, "%lld", static_cast<long long>(v));
  const int n = static_cast<int>(std::strlen(b));
  if (pad) for (int i = n; i < width; ++i) out.push_back(pad);
  out += b;
}
}  // namespace

std::string StrftimeImpl(const char* fmt, const int32_t* f, int64_t gmtoff, const char* zone) {
  // f = sec min hour mday mon year wday yday isdst
  std::string out;
  for (const char* p = fmt; *p; ++p) {
    if (*p != '%') {
      out.push_back(*p);
      continue;
    }
    ++p;
    char flag = 0;
    while (*p == '-' || *p == '_' || *p == '0' || *p == '^' || *p == '#') flag = *p++;
    while (*p >= '1' && *p <= '9') ++p;  // width: accepted, not applied
    if (*p == 'E' || *p == 'O') ++p;
    const int h12 = f[2] % 12 == 0 ? 12 : f[2] % 12;
    switch (*p) {
      case 0: return out;
      case 'a': out.append(kDays[(f[6] % 7 + 7) % 7], 3); break;
      case 'A': out += kDays[(f[6] % 7 + 7) % 7]; break;
      case 'b': case 'h': out.append(kMonths[(f[4] % 12 + 12) % 12], 3); break;
      case 'B': out += kMonths[(f[4] % 12 + 12) % 12]; break;
      case 'c': out += StrftimeImpl("%a %b %e %H:%M:%S %Y", f, gmtoff, zone); break;
      case 'C': Num(out, (f[5] + 1900) / 100, 2, '0', flag); break;
      case 'd': Num(out, f[3], 2, '0', flag); break;
      case 'D': out += StrftimeImpl("%m/%d/%y", f, gmtoff, zone); break;
      case 'e': Num(out, f[3], 2, ' ', flag); break;
      case 'F': out += StrftimeImpl("%Y-%m-%d", f, gmtoff, zone); break;
      case 'g': { int y, w; IsoWeek(f, y, w); Num(out, y % 100, 2, '0', flag); break; }
      case 'G': { int y, w; IsoWeek(f, y, w); Num(out, y, 4, '0', flag); break; }
      case 'H': Num(out, f[2], 2, '0', flag); break;
      case 'I': Num(out, h12, 2, '0', flag); break;
      case 'j': Num(out, f[7] + 1, 3, '0', flag); break;
      case 'k': Num(out, f[2], 2, ' ', flag); break;
      case 'l': Num(out, h12, 2, ' ', flag); break;
      case 'm': Num(out, f[4] + 1, 2, '0', flag); break;
      case 'M': Num(out, f[1], 2, '0', flag); break;
      case 'n': out.push_back('\n'); break;
      case 'p': out += f[2] >= 12 ? "PM" : "AM"; break;
      case 'P': out += f[2] >= 12 ? "pm" : "am"; break;
      case 'r': out += StrftimeImpl("%I:%M:%S %p", f, gmtoff, zone); break;
      case 'R': out += StrftimeImpl("%H:%M", f, gmtoff, zone); break;
      case 's': {
        GTm g{};
        g.sec = f[0]; g.min = f[1]; g.hour = f[2]; g.mday = f[3]; g.mon = f[4]; g.year = f[5];
        Num(out, TimegmFields(g) - gmtoff, 1, 0, flag);
        break;
      }
      case 'S': Num(out, f[0], 2, '0', flag); break;
      case 't': out.push_back('\t'); break;
      case 'T': out += StrftimeImpl("%H:%M:%S", f, gmtoff, zone); break;
      case 'u': Num(out, f[6] == 0 ? 7 : f[6], 1, 0, flag); break;
      case 'U': Num(out, (f[7] + 7 - f[6]) / 7, 2, '0', flag); break;
      case 'V': { int y, w; IsoWeek(f, y, w); Num(out, w, 2, '0', flag); break; }
      case 'w': Num(out, f[6], 1, 0, flag); break;
      case 'W': Num(out, (f[7] + 7 - ((f[6] + 6) % 7)) / 7, 2, '0', flag); break;
      case 'x': out += StrftimeImpl("%m/%d/%y", f, gmtoff, zone); break;
      case 'X': out += StrftimeImpl("%H:%M:%S", f, gmtoff, zone); break;
      case 'y': Num(out, ((f[5] + 1900) % 100 + 100) % 100, 2, '0', flag); break;
      case 'Y': Num(out, f[5] + 1900, 1, 0, flag); break;
      case 'z': {
        const int64_t a = gmtoff < 0 ? -gmtoff : gmtoff;
        char b[8];
        std::snprintf(b, sizeof b, "%c%02d%02d", gmtoff < 0 ? '-' : '+', static_cast<int>(a / 3600), static_cast<int>(a / 60 % 60));
        out += b;
        break;
      }
      case 'Z': out += zone ? zone : "UTC"; break;
      case '%': out.push_back('%'); break;
      default: out.push_back('%'); out.push_back(*p); break;
    }
  }
  return out;
}

namespace {
uint64_t h_strftime(char* buf, uint64_t max, const char* fmt, const GTm* tm) {
  const std::string s = StrftimeImpl(fmt, &tm->sec, tm->gmtoff, tm->zone ? g2h<char>(tm->zone) : nullptr);
  if (!max || s.size() + 1 > max) return 0;
  std::memcpy(buf, s.c_str(), s.size() + 1);
  return s.size();
}
uint64_t h_strftime_l(char* buf, uint64_t max, const char* fmt, const GTm* tm, uint64_t) { return h_strftime(buf, max, fmt, tm); }
uint64_t h_asctime(const GTm* tm) {
  const gaddr b = ThreadBuffer(2, 32);
  char* o = g2h<char>(b);
  std::snprintf(o, 32, "%.3s %.3s%3d %.2d:%.2d:%.2d %d\n", kDays[(tm->wday % 7 + 7) % 7], kMonths[(tm->mon % 12 + 12) % 12],
                tm->mday, tm->hour, tm->min, tm->sec, 1900 + tm->year);
  return b;
}

// ---- strptime ----
bool MatchName(const char*& s, const char* const* names, int n, int& idx) {
  for (int i = 0; i < n; ++i) {
    const char* nm = names[i];
    const size_t full = std::strlen(nm);
    if (_strnicmp(s, nm, full) == 0) {
      idx = i;
      s += full;
      return true;
    }
    if (_strnicmp(s, nm, 3) == 0) {
      idx = i;
      s += 3;
      return true;
    }
  }
  return false;
}
bool ReadNum(const char*& s, int maxdigits, int lo, int hi, int& out) {
  if (!std::isdigit(static_cast<unsigned char>(*s))) return false;
  int v = 0, n = 0;
  while (n < maxdigits && std::isdigit(static_cast<unsigned char>(*s))) {
    v = v * 10 + (*s++ - '0');
    ++n;
  }
  if (v < lo || v > hi) return false;
  out = v;
  return true;
}
const char* Strptime(const char* s, const char* fmt, GTm* tm) {
  bool pm = false, have_pm = false, have_h12 = false;
  int h12 = 0;
  bool have_date = false;
  for (const char* p = fmt; *p; ++p) {
    if (std::isspace(static_cast<unsigned char>(*p))) {
      while (std::isspace(static_cast<unsigned char>(*s))) ++s;
      continue;
    }
    if (*p != '%') {
      if (*s++ != *p) return nullptr;
      continue;
    }
    ++p;
    if (*p == 'E' || *p == 'O') ++p;
    int v = 0;
    switch (*p) {
      case '%': if (*s++ != '%') return nullptr; break;
      case 'a': case 'A': if (!MatchName(s, kDays, 7, v)) return nullptr; tm->wday = v; break;
      case 'b': case 'B': case 'h': if (!MatchName(s, kMonths, 12, v)) return nullptr; tm->mon = v; have_date = true; break;
      case 'C': if (!ReadNum(s, 2, 0, 99, v)) return nullptr; tm->year = v * 100 - 1900 + (tm->year + 1900) % 100; have_date = true; break;
      case 'd': case 'e': while (*s == ' ') ++s; if (!ReadNum(s, 2, 1, 31, v)) return nullptr; tm->mday = v; have_date = true; break;
      case 'D': if (!(s = Strptime(s, "%m/%d/%y", tm))) return nullptr; have_date = true; break;
      case 'F': if (!(s = Strptime(s, "%Y-%m-%d", tm))) return nullptr; have_date = true; break;
      case 'H': case 'k': while (*s == ' ') ++s; if (!ReadNum(s, 2, 0, 23, v)) return nullptr; tm->hour = v; break;
      case 'I': case 'l': while (*s == ' ') ++s; if (!ReadNum(s, 2, 1, 12, v)) return nullptr; h12 = v; have_h12 = true; break;
      case 'j': if (!ReadNum(s, 3, 1, 366, v)) return nullptr; tm->yday = v - 1; break;
      case 'm': if (!ReadNum(s, 2, 1, 12, v)) return nullptr; tm->mon = v - 1; have_date = true; break;
      case 'M': if (!ReadNum(s, 2, 0, 59, v)) return nullptr; tm->min = v; break;
      case 'n': case 't': while (std::isspace(static_cast<unsigned char>(*s))) ++s; break;
      case 'p': case 'P':
        if (_strnicmp(s, "AM", 2) == 0) { pm = false; have_pm = true; s += 2; }
        else if (_strnicmp(s, "PM", 2) == 0) { pm = true; have_pm = true; s += 2; }
        else return nullptr;
        break;
      case 'r': if (!(s = Strptime(s, "%I:%M:%S %p", tm))) return nullptr; break;
      case 'R': if (!(s = Strptime(s, "%H:%M", tm))) return nullptr; break;
      case 'S': if (!ReadNum(s, 2, 0, 61, v)) return nullptr; tm->sec = v; break;
      case 'T': case 'X': if (!(s = Strptime(s, "%H:%M:%S", tm))) return nullptr; break;
      case 'x': if (!(s = Strptime(s, "%m/%d/%y", tm))) return nullptr; have_date = true; break;
      case 'u': if (!ReadNum(s, 1, 1, 7, v)) return nullptr; tm->wday = v % 7; break;
      case 'w': if (!ReadNum(s, 1, 0, 6, v)) return nullptr; tm->wday = v; break;
      case 'y': if (!ReadNum(s, 2, 0, 99, v)) return nullptr; tm->year = v < 69 ? v + 100 : v; have_date = true; break;
      case 'Y': if (!ReadNum(s, 4, 0, 9999, v)) return nullptr; tm->year = v - 1900; have_date = true; break;
      case 'z': {
        while (*s == ' ') ++s;
        if (*s == 'Z') { ++s; tm->gmtoff = 0; break; }
        if (*s != '+' && *s != '-') return nullptr;
        const int sign = *s++ == '-' ? -1 : 1;
        int hh, mm = 0;
        if (!ReadNum(s, 2, 0, 14, hh)) return nullptr;
        if (*s == ':') ++s;
        if (std::isdigit(static_cast<unsigned char>(*s)) && !ReadNum(s, 2, 0, 59, mm)) return nullptr;
        tm->gmtoff = sign * (hh * 3600 + mm * 60);
        break;
      }
      case 'Z': while (std::isalpha(static_cast<unsigned char>(*s))) ++s; break;
      default: return nullptr;
    }
  }
  if (have_h12) tm->hour = (h12 % 12) + ((have_pm && pm) ? 12 : 0);
  else if (have_pm && pm && tm->hour < 12) tm->hour += 12;
  if (have_date) {  // wday / yday from the date, as glibc does
    GTm tmp = *tm;
    const int64_t t = TimegmFields(tmp);
    Breakdown(t, tmp);
    tm->wday = tmp.wday;
    tm->yday = tmp.yday;
  }
  return s;
}
uint64_t h_strptime(const char* s, const char* fmt, GTm* tm) {
  const char* r = Strptime(s, fmt, tm);
  return r ? h2g(r) : 0;
}

// ---- clocks ----
int64_t CpuTimeNs(int clk) {
  FILETIME c, e, k, u;
  if (clk == 3) GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u);
  else GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
  const uint64_t t100 = ((static_cast<uint64_t>(u.dwHighDateTime) << 32) | u.dwLowDateTime) +
                        ((static_cast<uint64_t>(k.dwHighDateTime) << 32) | k.dwLowDateTime);
  return static_cast<int64_t>(t100) * 100;
}
int32_t h_clock_gettime(Cpu& cpu, int32_t clk, Timespec* ts) {
  int64_t ns;
  switch (clk) {
    case 0: case 5: case 8: ns = RealtimeNs(); break;                    // REALTIME(_COARSE/_ALARM)
    case 1: case 4: case 6: case 7: case 9: ns = MonotonicNs(); break;   // MONOTONIC(_RAW/_COARSE), BOOTTIME
    case 2: case 3: ns = CpuTimeNs(clk); break;                           // PROCESS / THREAD CPUTIME
    default:
      SetErrno(cpu, err::INVAL);
      return -1;
  }
  ts->sec = ns / 1000000000;
  ts->nsec = ns % 1000000000;
  return 0;
}
int32_t h_clock_getres(Cpu& cpu, int32_t clk, Timespec* ts) {
  if (clk < 0 || clk > 9) {
    SetErrno(cpu, err::INVAL);
    return -1;
  }
  if (ts) {
    ts->sec = 0;
    ts->nsec = clk == 5 || clk == 6 ? 1000000 : 1;
  }
  return 0;
}
int32_t h_gettimeofday(Timeval* tv, int32_t* tz) {
  if (tv) {
    const int64_t us = RealtimeNs() / 1000;
    tv->sec = us / 1000000;
    tv->usec = us % 1000000;
  }
  if (tz) {
    const LocalInfo li = LocalAt(RealtimeNs() / 1000000000);
    tz[0] = static_cast<int32_t>(-li.offset / 60);
    tz[1] = 0;
  }
  return 0;
}
int64_t h_time(int64_t* t) {
  const int64_t s = RealtimeNs() / 1000000000;
  if (t) *t = s;
  return s;
}
int64_t h_clock() { return CpuTimeNs(2) / 1000; }  // CLOCKS_PER_SEC = 1000000 on bionic
int32_t h_usleep(uint32_t us) {
  SleepNs(int64_t{us} * 1000);
  return 0;
}
uint32_t h_sleep(uint32_t s) {
  SleepNs(int64_t{s} * 1000000000);
  return 0;
}
int32_t h_nanosleep(Cpu& cpu, const Timespec* req, Timespec* rem) {
  if (!req || req->sec < 0 || req->nsec < 0 || req->nsec >= 1000000000) {
    SetErrno(cpu, err::INVAL);
    return -1;
  }
  SleepNs(req->sec * 1000000000 + req->nsec);
  if (rem) rem->sec = rem->nsec = 0;
  return 0;
}
double h_difftime(int64_t a, int64_t b) { return static_cast<double>(a - b); }
int32_t h_zero() { return 0; }
}  // namespace

void RegisterTime() {
  // `long timezone`: seconds west of UTC, read-only for the game (data symbol, defined before the module loads).
  long tz = 0;
  _get_timezone(&tz);
  mem::W64(DefineData("timezone", 8, 8), static_cast<uint64_t>(static_cast<int64_t>(tz)));

  NOVA_HLE_FAST("clock_gettime", h_clock_gettime);
  NOVA_HLE("clock_getres", h_clock_getres);
  NOVA_HLE_FAST("gettimeofday", h_gettimeofday);
  NOVA_HLE_FAST("time", h_time);
  NOVA_HLE_FAST("clock", h_clock);
  NOVA_HLE("usleep", h_usleep);
  NOVA_HLE("sleep", h_sleep);
  NOVA_HLE("nanosleep", h_nanosleep);
  NOVA_HLE_FAST("difftime", h_difftime);
  NOVA_HLE("tzset", h_zero);

  NOVA_HLE("gmtime", h_gmtime);
  NOVA_HLE("gmtime_r", h_gmtime_r);
  NOVA_HLE("localtime", h_localtime);
  NOVA_HLE("localtime_r", h_localtime_r);
  NOVA_HLE("mktime", h_mktime);
  NOVA_HLE("timegm", h_timegm);
  NOVA_HLE("asctime", h_asctime);
  NOVA_HLE("strftime", h_strftime);
  NOVA_HLE("strftime_l", h_strftime_l);
  NOVA_HLE("strptime", h_strptime);
}
}  // namespace nova::hle::libc
