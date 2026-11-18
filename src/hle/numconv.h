// Integer parsing shared by the narrow (strtol) and wide (wcstol) families, with C99/bionic semantics:
// whitespace, sign, 0x / 0 prefixes, overflow detection, `end` left at the start when nothing converted.
#pragma once
#include <cstdint>

namespace nova::hle::libc {
template <class C>
inline bool NumIsSpace(C c) {
  return c == ' ' || (c >= 9 && c <= 13);
}

template <class C>
struct IntParse {
  std::uint64_t mag = 0;
  bool neg = false;
  bool overflow = false;
  const C* end = nullptr;
};

template <class C>
IntParse<C> ParseInteger(const C* s, int base) {
  IntParse<C> r;
  r.end = s;
  if (base < 0 || base == 1 || base > 36) return r;
  const C* p = s;
  while (NumIsSpace(*p)) ++p;
  if (*p == '-' || *p == '+') r.neg = (*p++ == '-');
  auto digit = [](C c) -> int {
    if (c >= '0' && c <= '9') return static_cast<int>(c - '0');
    if (c >= 'a' && c <= 'z') return static_cast<int>(c - 'a') + 10;
    if (c >= 'A' && c <= 'Z') return static_cast<int>(c - 'A') + 10;
    return 99;
  };
  if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && digit(p[2]) < 16) {
    p += 2;
    base = 16;
  } else if (base == 0) {
    base = (p[0] == '0') ? 8 : 10;
  }
  bool any = false;
  const std::uint64_t cutoff = ~0ull / static_cast<std::uint64_t>(base);
  const std::uint64_t cutlim = ~0ull % static_cast<std::uint64_t>(base);
  for (;; ++p) {
    const int d = digit(*p);
    if (d >= base) break;
    any = true;
    if (r.overflow || r.mag > cutoff || (r.mag == cutoff && static_cast<std::uint64_t>(d) > cutlim)) {
      r.overflow = true;
      continue;
    }
    r.mag = r.mag * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(d);
  }
  if (any) r.end = p;
  return r;
}

// strtol / strtoll: value and ERANGE flag.
template <class C>
std::int64_t ToSigned(const IntParse<C>& r, bool& range) {
  range = false;
  if (r.neg) {
    if (r.overflow || r.mag > (1ull << 63)) {
      range = true;
      return INT64_MIN;
    }
    return static_cast<std::int64_t>(0 - r.mag);
  }
  if (r.overflow || r.mag > static_cast<std::uint64_t>(INT64_MAX)) {
    range = true;
    return INT64_MAX;
  }
  return static_cast<std::int64_t>(r.mag);
}
// strtoul / strtoull
template <class C>
std::uint64_t ToUnsigned(const IntParse<C>& r, bool& range) {
  range = false;
  if (r.overflow) {
    range = true;
    return ~0ull;
  }
  return r.neg ? 0 - r.mag : r.mag;
}
}  // namespace nova::hle::libc
