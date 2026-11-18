// Number conversion and the string functions that the Phase 1 minimal layer lacked. strtol/strtoul/strtoll/strtoull
// follow bionic (64-bit long, ERANGE saturation); strtod/strtof parse on the host and report ERANGE; strtold parses
// on the host and widens to IEEE binary128 by hand (long double never goes through the host CRT).
#include <algorithm>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "hle/libc_internal.h"
#include "hle/numconv.h"

namespace nova::hle::libc {

// double -> binary128 (the guest's long double), exact.
Vec128 DoubleToQuad(double d) {
  const std::uint64_t bits = std::bit_cast<std::uint64_t>(d);
  const std::uint64_t sign = bits >> 63;
  const int exp = static_cast<int>((bits >> 52) & 0x7FF);
  std::uint64_t frac = bits & 0xFFFFFFFFFFFFFull;
  std::uint64_t qexp;
  if (exp == 0x7FF) {
    qexp = 0x7FFF;
  } else if (exp == 0) {
    if (frac == 0) return {0, sign << 63};
    const int shift = std::countl_zero(frac) - 11;  // renormalise the subnormal
    frac = (frac << shift) & 0xFFFFFFFFFFFFFull;
    qexp = static_cast<std::uint64_t>(-1022 - shift + 16383);
  } else {
    qexp = static_cast<std::uint64_t>(exp - 1023 + 16383);
  }
  return {frac << 60, (sign << 63) | (qexp << 48) | (frac >> 4)};
}

namespace {
using std::int32_t;
using std::int64_t;
using std::uint32_t;
using std::uint64_t;

void StoreEnd(uint64_t* end, const void* p) {
  if (end) *end = h2g(p);
}

int64_t h_strtol(Cpu& cpu, const char* s, uint64_t* end, int32_t base) {
  const auto r = ParseInteger(reinterpret_cast<const unsigned char*>(s), base);
  bool range;
  const int64_t v = ToSigned(r, range);
  if (range) SetErrno(cpu, err::RANGE);
  StoreEnd(end, r.end);
  return v;
}
uint64_t h_strtoul(Cpu& cpu, const char* s, uint64_t* end, int32_t base) {
  const auto r = ParseInteger(reinterpret_cast<const unsigned char*>(s), base);
  bool range;
  const uint64_t v = ToUnsigned(r, range);
  if (range) SetErrno(cpu, err::RANGE);
  StoreEnd(end, r.end);
  return v;
}
int64_t h_strtol_l(Cpu& cpu, const char* s, uint64_t* end, int32_t base, uint64_t) { return h_strtol(cpu, s, end, base); }
uint64_t h_strtoul_l(Cpu& cpu, const char* s, uint64_t* end, int32_t base, uint64_t) { return h_strtoul(cpu, s, end, base); }
int32_t h_atoi(const char* s) {
  const auto r = ParseInteger(reinterpret_cast<const unsigned char*>(s), 10);
  bool range;
  return static_cast<int32_t>(ToSigned(r, range));
}
int64_t h_atol(const char* s) {
  const auto r = ParseInteger(reinterpret_cast<const unsigned char*>(s), 10);
  bool range;
  return ToSigned(r, range);
}

double ParseDouble(Cpu& cpu, const char* s, uint64_t* end) {
  errno = 0;
  char* e = nullptr;
  const double v = std::strtod(s, &e);
  if (errno == ERANGE) SetErrno(cpu, err::RANGE);
  StoreEnd(end, e);
  return v;
}
double h_strtod(Cpu& cpu, const char* s, uint64_t* end) { return ParseDouble(cpu, s, end); }
float h_strtof(Cpu& cpu, const char* s, uint64_t* end) {
  errno = 0;
  char* e = nullptr;
  const float v = std::strtof(s, &e);
  if (errno == ERANGE) SetErrno(cpu, err::RANGE);
  StoreEnd(end, e);
  return v;
}
double h_atof(Cpu& cpu, const char* s) { return ParseDouble(cpu, s, nullptr); }
// long double: returned in q0, so these are raw handlers. (str, endptr[, locale])
void raw_strtold(Cpu& cpu) {
  const char* s = g2h<char>(cpu.x(0));
  const gaddr endp = cpu.x(1);
  errno = 0;
  char* e = nullptr;
  const double v = std::strtod(s, &e);
  if (errno == ERANGE) SetErrno(cpu, err::RANGE);
  if (endp) mem::W64(endp, h2g(e));
  cpu.set_v(0, DoubleToQuad(v));
}

// ---- classic string leftovers ----
gaddr g_strtok_last = 0;
uint64_t h_strtok(const char* s, const char* delim) {
  char* cur = s ? const_cast<char*>(s) : g2h<char>(g_strtok_last);
  if (!s && !g_strtok_last) return 0;
  cur += std::strspn(cur, delim);
  if (!*cur) {
    g_strtok_last = 0;
    return 0;
  }
  char* tok = cur;
  cur += std::strcspn(cur, delim);
  if (*cur) {
    *cur++ = 0;
    g_strtok_last = h2g(cur);
  } else {
    g_strtok_last = 0;
  }
  return h2g(tok);
}
int32_t h_strcoll(const char* a, const char* b) { return std::strcmp(a, b); }
int32_t h_strcoll_l(const char* a, const char* b, uint64_t) { return std::strcmp(a, b); }
uint64_t h_strxfrm(char* dst, const char* src, uint64_t n) {
  const uint64_t len = std::strlen(src);
  if (n) {
    const uint64_t k = std::min<uint64_t>(len, n - 1);
    std::memcpy(dst, src, k);
    dst[k] = 0;
  }
  return len;
}
uint64_t h_strxfrm_l(char* dst, const char* src, uint64_t n, uint64_t) { return h_strxfrm(dst, src, n); }
}  // namespace

void RegisterString() {
  NOVA_HLE_FAST("strtol", h_strtol);
  NOVA_HLE_FAST("strtoll", h_strtol);
  NOVA_HLE_FAST("strtoll_l", h_strtol_l);
  NOVA_HLE_FAST("strtoul", h_strtoul);
  NOVA_HLE_FAST("strtoull", h_strtoul);
  NOVA_HLE_FAST("strtoull_l", h_strtoul_l);
  NOVA_HLE_FAST("atoi", h_atoi);
  NOVA_HLE_FAST("atol", h_atol);
  NOVA_HLE_FAST("atoll", h_atol);
  NOVA_HLE_FAST("strtod", h_strtod);
  NOVA_HLE_FAST("strtof", h_strtof);
  NOVA_HLE_FAST("atof", h_atof);
  hle::Register("strtold", raw_strtold, hle::kFast);
  hle::Register("strtold_l", raw_strtold, hle::kFast);

  NOVA_HLE_FAST("strtok", h_strtok);
  NOVA_HLE_FAST("strcoll", h_strcoll);
  NOVA_HLE_FAST("strcoll_l", h_strcoll_l);
  NOVA_HLE_FAST("strxfrm", h_strxfrm);
  NOVA_HLE_FAST("strxfrm_l", h_strxfrm_l);
}
}  // namespace nova::hle::libc
