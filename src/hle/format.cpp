#include "hle/format.h"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#include "hle/libc_internal.h"
#include "hle/numconv.h"

namespace nova::hle {
double QuadToDouble(Vec128 q) {
  const std::uint64_t lo = q[0], hi = q[1];
  const bool neg = (hi >> 63) != 0;
  const int exp = static_cast<int>((hi >> 48) & 0x7FFF);
  const std::uint64_t mant_hi = hi & 0xFFFFFFFFFFFFull;  // 48 bits
  double r;
  if (exp == 0x7FFF) {
    r = (mant_hi | lo) ? std::nan("") : HUGE_VAL;
  } else if (exp == 0 && !(mant_hi | lo)) {
    r = 0.0;
  } else {
    // value = 1.m * 2^(exp-16383); take the top 52 bits of the 112-bit mantissa
    const std::uint64_t frac52 = (mant_hi << 4) | (lo >> 60);
    const double m = 1.0 + static_cast<double>(frac52) / 4503599627370496.0;  // 2^52
    r = std::ldexp(m, exp - 16383);
  }
  return neg ? -r : r;
}

namespace {
void AppendUtf8(std::string& out, std::uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

void Pad(std::string& out, const std::string& s, int width, bool left, char padc = ' ') {
  const int n = static_cast<int>(s.size());
  if (width > n && !left) out.append(width - n, padc);
  out += s;
  if (width > n && left) out.append(width - n, ' ');
}
}  // namespace

std::string FormatPrintf(const char* fmt, VaList& va) {
  std::string out;
  if (!fmt) return "(null)";
  for (const char* p = fmt; *p;) {
    if (*p != '%') {
      out.push_back(*p++);
      continue;
    }
    ++p;
    if (*p == '%') {
      out.push_back('%');
      ++p;
      continue;
    }
    bool left = false, plus = false, space = false, alt = false, zero = false;
    for (;; ++p) {
      if (*p == '-') left = true;
      else if (*p == '+') plus = true;
      else if (*p == ' ') space = true;
      else if (*p == '#') alt = true;
      else if (*p == '0') zero = true;
      else break;
    }
    int width = -1, prec = -1;
    if (*p == '*') {
      width = va.Next<std::int32_t>();
      if (width < 0) {
        left = true;
        width = -width;
      }
      ++p;
    } else if (*p >= '0' && *p <= '9') {
      width = 0;
      while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
    }
    if (*p == '.') {
      ++p;
      prec = 0;
      if (*p == '*') {
        prec = va.Next<std::int32_t>();
        ++p;
      } else {
        while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0');
      }
    }
    int lenmod = 0;  // 0 none, 1 hh, 2 h, 3 l/ll/j/z/t (64-bit), 4 L
    if (*p == 'h') {
      ++p;
      lenmod = 2;
      if (*p == 'h') {
        ++p;
        lenmod = 1;
      }
    } else if (*p == 'l') {
      ++p;
      lenmod = 3;
      if (*p == 'l') ++p;
    } else if (*p == 'j' || *p == 'z' || *p == 't') {
      ++p;
      lenmod = 3;
    } else if (*p == 'L') {
      ++p;
      lenmod = 4;
    }
    const char conv = *p;
    if (!conv) break;
    ++p;
    auto flags_str = [&](bool with_zero) {
      std::string f;
      if (left) f += '-';
      if (plus) f += '+';
      if (space) f += ' ';
      if (alt) f += '#';
      if (zero && with_zero) f += '0';
      return f;
    };
    char buf[512];
    switch (conv) {
      case 'd':
      case 'i': {
        std::int64_t v = static_cast<std::int64_t>(va.NextInt());
        if (lenmod == 0) v = static_cast<std::int32_t>(v);
        else if (lenmod == 1) v = static_cast<std::int8_t>(v);
        else if (lenmod == 2) v = static_cast<std::int16_t>(v);
        std::string f = "%" + flags_str(prec < 0);
        if (width >= 0) f += std::to_string(width);
        if (prec >= 0) f += "." + std::to_string(prec);
        f += "lld";
        std::snprintf(buf, sizeof buf, f.c_str(), static_cast<long long>(v));
        out += buf;
        break;
      }
      case 'u':
      case 'o':
      case 'x':
      case 'X': {
        std::uint64_t v = va.NextInt();
        if (lenmod == 0) v = static_cast<std::uint32_t>(v);
        else if (lenmod == 1) v = static_cast<std::uint8_t>(v);
        else if (lenmod == 2) v = static_cast<std::uint16_t>(v);
        std::string f = "%" + flags_str(prec < 0);
        if (width >= 0) f += std::to_string(width);
        if (prec >= 0) f += "." + std::to_string(prec);
        f += "ll";
        f += conv;
        std::snprintf(buf, sizeof buf, f.c_str(), static_cast<unsigned long long>(v));
        out += buf;
        break;
      }
      case 'C':
      case 'c': {
        if (conv == 'C' || lenmod == 3) {  // wint_t: one wide character, emitted as UTF-8
          std::string u;
          AppendUtf8(u, static_cast<std::uint32_t>(va.NextInt()));
          Pad(out, u, width, left);
          break;
        }
        const char c = static_cast<char>(va.NextInt());
        Pad(out, std::string(1, c), width, left);
        break;
      }
      case 'S':
      case 's': {
        const std::uint64_t a = va.NextInt();
        std::string s;
        if (a && (conv == 'S' || lenmod == 3)) {  // wchar_t* (32-bit units in the guest)
          const auto* w = g2h<std::uint32_t>(a);
          for (std::size_t i = 0; w[i] && (prec < 0 || static_cast<int>(s.size()) < prec); ++i) AppendUtf8(s, w[i]);
          Pad(out, s, width, left);
          break;
        }
        if (!a) {
          s = "(null)";
        } else {
          const char* sp = reinterpret_cast<const char*>(g2h(a));
          std::size_t n = 0;
          while (sp[n] && (prec < 0 || static_cast<int>(n) < prec)) ++n;
          s.assign(sp, n);
        }
        if (!a && prec >= 0 && prec < 6) s.resize(prec);
        Pad(out, s, width, left);
        break;
      }
      case 'p': {
        const std::uint64_t a = va.NextInt();
        Pad(out, a ? std::format("0x{:x}", a) : std::string("0x0"), width, left);
        break;
      }
      case 'e':
      case 'E':
      case 'f':
      case 'F':
      case 'g':
      case 'G':
      case 'a':
      case 'A': {
        double d;
        if (lenmod == 4) d = QuadToDouble(va.NextQuad());
        else d = std::bit_cast<double>(va.NextFp());
        if (!std::isfinite(d)) {  // the host CRT prints "-nan(ind)" and friends; bionic prints nan / inf
          std::string t = std::isnan(d) ? "nan" : "inf";
          if (conv == 'E' || conv == 'F' || conv == 'G' || conv == 'A')
            for (char& ch : t) ch = static_cast<char>(ch - 32);
          if (std::signbit(d)) t.insert(t.begin(), '-');
          else if (plus) t.insert(t.begin(), '+');
          else if (space) t.insert(t.begin(), ' ');
          Pad(out, t, width, left);
          break;
        }
        std::string f = "%" + flags_str(true);
        if (width >= 0) f += std::to_string(width);
        if (prec >= 0) f += "." + std::to_string(prec);
        f += conv;
        const int n = std::snprintf(buf, sizeof buf, f.c_str(), d);
        if (n >= static_cast<int>(sizeof buf)) {
          std::string big(static_cast<std::size_t>(n) + 1, '\0');
          std::snprintf(big.data(), big.size(), f.c_str(), d);
          big.resize(n);
          out += big;
        } else {
          out += buf;
        }
        break;
      }
      case 'n':
        (void)va.NextInt();
        break;
      default:
        out.push_back('%');
        out.push_back(conv);
        break;
    }
  }
  return out;
}
}  // namespace nova::hle

// ------------------------------------------------------------------------------------------------ scanf
namespace nova::hle::libc {
namespace {
// Reader with unlimited pushback on top of ScanInput's LIFO Unget; leftovers are returned to the source on exit.
struct Reader {
  explicit Reader(ScanInput& i) : in(i) {}
  ~Reader() {
    while (!back.empty()) {
      in.Unget(back.back());
      back.pop_back();
    }
  }
  int Get() {
    int c;
    if (!back.empty()) {
      c = back.back();
      back.pop_back();
    } else {
      c = in.Get();
    }
    if (c >= 0) ++consumed;
    return c;
  }
  void Unget(int c) {
    if (c < 0) return;
    --consumed;
    back.push_back(c);
  }
  ScanInput& in;
  std::vector<int> back;
  std::size_t consumed = 0;
};
bool IsSp(int c) { return c == ' ' || (c >= 9 && c <= 13); }
int DigitVal(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') return c - 'a' + 10;
  if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
  return 99;
}
template <class T>
void StoreAt(std::uint64_t addr, T v) {
  if (addr) mem::W<T>(addr, v);
}
}  // namespace

int ScanfCore(ScanInput& input, const char* fmt, VaList& va) {
  Reader r(input);
  int assigned = 0;
  bool eof_hit = false;
  auto skip_ws = [&] {
    int c;
    while ((c = r.Get()) >= 0 && IsSp(c)) {}
    if (c < 0) eof_hit = true;
    r.Unget(c);
  };
  // input failure before any assignment -> EOF, otherwise the count converted so far
  auto done = [&]() { return (assigned == 0 && eof_hit) ? -1 : assigned; };
  for (const char* p = fmt; *p; ++p) {
    if (IsSp(static_cast<unsigned char>(*p))) {
      skip_ws();
      continue;
    }
    if (*p != '%') {
      const int c = r.Get();
      if (c < 0) {
        eof_hit = true;
        return done();
      }
      if (c != static_cast<unsigned char>(*p)) {
        r.Unget(c);
        return done();
      }
      continue;
    }
    ++p;
    if (*p == '%') {
      skip_ws();
      const int c = r.Get();
      if (c != '%') {
        if (c < 0) eof_hit = true;
        r.Unget(c);
        return done();
      }
      continue;
    }
    bool suppress = false;
    if (*p == '*') {
      suppress = true;
      ++p;
    }
    int width = 0;
    while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
    int lenmod = 0;  // 1 hh, 2 h, 3 l/ll/j/z/t (64-bit), 4 L
    if (*p == 'h') {
      lenmod = 2;
      ++p;
      if (*p == 'h') lenmod = 1, ++p;
    } else if (*p == 'l') {
      lenmod = 3;
      ++p;
      if (*p == 'l') ++p;
    } else if (*p == 'j' || *p == 'z' || *p == 't') {
      lenmod = 3;
      ++p;
    } else if (*p == 'L') {
      lenmod = 4;
      ++p;
    }
    const char conv = *p;
    if (!conv) break;
    auto arg = [&]() -> std::uint64_t { return suppress ? 0 : va.NextInt(); };
    const int rem0 = width > 0 ? width : 0x7FFFFFFF;

    switch (conv) {
      case 'n': {
        if (!suppress) {
          const std::uint64_t a = va.NextInt();
          if (lenmod == 3) StoreAt<std::int64_t>(a, static_cast<std::int64_t>(r.consumed));
          else if (lenmod == 2) StoreAt<std::int16_t>(a, static_cast<std::int16_t>(r.consumed));
          else if (lenmod == 1) StoreAt<std::int8_t>(a, static_cast<std::int8_t>(r.consumed));
          else StoreAt<std::int32_t>(a, static_cast<std::int32_t>(r.consumed));
        }
        break;
      }
      case 'c': {
        const std::uint64_t a = arg();
        int n = width > 0 ? width : 1, got = 0;
        while (n-- > 0) {
          const int c = r.Get();
          if (c < 0) {
            eof_hit = true;
            break;
          }
          if (a) mem::W<std::uint8_t>(a + got, static_cast<std::uint8_t>(c));
          ++got;
        }
        if (got == 0) return done();
        if (!suppress) ++assigned;
        break;
      }
      case 's': {
        skip_ws();
        const std::uint64_t a = arg();
        int rem = rem0, got = 0, c;
        while (rem > 0 && (c = r.Get()) >= 0) {
          if (IsSp(c)) {
            r.Unget(c);
            break;
          }
          if (a) mem::W<std::uint8_t>(a + got, static_cast<std::uint8_t>(c));
          ++got;
          --rem;
        }
        if (got == 0) return done();
        if (a) mem::W<std::uint8_t>(a + got, 0);
        if (!suppress) ++assigned;
        break;
      }
      case '[': {
        ++p;
        bool neg = false;
        if (*p == '^') neg = true, ++p;
        bool set[256] = {};
        if (*p == ']') set[static_cast<unsigned char>(*p++)] = true;
        while (*p && *p != ']') {
          if (p[1] == '-' && p[2] && p[2] != ']') {
            for (int ch = static_cast<unsigned char>(p[0]); ch <= static_cast<unsigned char>(p[2]); ++ch) set[ch] = true;
            p += 3;
          } else {
            set[static_cast<unsigned char>(*p++)] = true;
          }
        }
        const std::uint64_t a = arg();
        int rem = rem0, got = 0, c;
        while (rem > 0 && (c = r.Get()) >= 0) {
          if (set[c & 255] == neg) {
            r.Unget(c);
            break;
          }
          if (a) mem::W<std::uint8_t>(a + got, static_cast<std::uint8_t>(c));
          ++got;
          --rem;
        }
        if (got == 0) return done();
        if (a) mem::W<std::uint8_t>(a + got, 0);
        if (!suppress) ++assigned;
        break;
      }
      case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'p': {
        skip_ws();
        int base = conv == 'd' || conv == 'u' ? 10 : conv == 'o' ? 8 : conv == 'i' ? 0 : 16;
        std::string tok;
        int rem = rem0;
        int c = r.Get();
        if ((c == '+' || c == '-') && rem > 0) {
          tok.push_back(static_cast<char>(c));
          --rem;
          c = r.Get();
        }
        if ((base == 0 || base == 16) && c == '0' && rem > 0) {
          tok.push_back('0');
          --rem;
          c = r.Get();
          if ((c == 'x' || c == 'X') && rem > 0) {
            const int d = r.Get();
            r.Unget(d);
            if (DigitVal(d) < 16) {  // a real 0x prefix
              tok.push_back('x');
              --rem;
              base = 16;
              c = r.Get();
            } else if (base == 0) {
              base = 8;
            }
          } else if (base == 0) {
            base = 8;
          }
        } else if (base == 0) {
          base = 10;
        }
        while (rem > 0 && c >= 0 && DigitVal(c) < base) {
          tok.push_back(static_cast<char>(c));
          --rem;
          c = r.Get();
        }
        if (c < 0) eof_hit = true;
        r.Unget(c);
        const auto pr = ParseInteger(tok.c_str(), base);
        if (pr.end == tok.c_str()) return done();
        const std::uint64_t a = arg();
        bool range;
        if (conv == 'd' || conv == 'i') {
          const std::int64_t v = ToSigned(pr, range);
          if (a) {
            if (lenmod == 3) mem::W<std::int64_t>(a, v);
            else if (lenmod == 2) mem::W<std::int16_t>(a, static_cast<std::int16_t>(v));
            else if (lenmod == 1) mem::W<std::int8_t>(a, static_cast<std::int8_t>(v));
            else mem::W<std::int32_t>(a, static_cast<std::int32_t>(v));
          }
        } else {
          const std::uint64_t v = ToUnsigned(pr, range);
          if (a) {
            if (lenmod == 3 || conv == 'p') mem::W<std::uint64_t>(a, v);
            else if (lenmod == 2) mem::W<std::uint16_t>(a, static_cast<std::uint16_t>(v));
            else if (lenmod == 1) mem::W<std::uint8_t>(a, static_cast<std::uint8_t>(v));
            else mem::W<std::uint32_t>(a, static_cast<std::uint32_t>(v));
          }
        }
        if (!suppress) ++assigned;
        break;
      }
      case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
        skip_ws();
        std::string tok;
        int rem = rem0;
        int c = r.Get();
        auto take = [&] {
          tok.push_back(static_cast<char>(c));
          --rem;
          c = r.Get();
        };
        if ((c == '+' || c == '-') && rem > 0) take();
        bool digits = false;
        auto match_word = [&](const char* w) {  // case-insensitive, all-or-nothing (with pushback)
          std::vector<int> seen;
          const char* q = w;
          while (*q && c >= 0 && std::tolower(c) == *q && rem > 0) {
            seen.push_back(c);
            ++q;
            --rem;
            c = r.Get();
          }
          if (*q) {
            r.Unget(c);
            for (std::size_t i = seen.size(); i-- > 0;) r.Unget(seen[i]);
            rem += static_cast<int>(seen.size());
            c = r.Get();
            return false;
          }
          for (int ch : seen) tok.push_back(static_cast<char>(ch));
          return true;
        };
        if (c == 'i' || c == 'I') {
          if (match_word("infinity") || match_word("inf")) digits = true;
        } else if (c == 'n' || c == 'N') {
          if (match_word("nan")) digits = true;
        } else {
          while (rem > 0 && c >= '0' && c <= '9') digits = true, take();
          if (rem > 0 && c == '.') {
            take();
            while (rem > 0 && c >= '0' && c <= '9') digits = true, take();
          }
          if (digits && rem > 0 && (c == 'e' || c == 'E')) {
            const int e = c;
            int sign = -1;
            int cc = r.Get();
            if (cc == '+' || cc == '-') {
              sign = cc;
              cc = r.Get();
            }
            if (cc >= '0' && cc <= '9') {
              tok.push_back(static_cast<char>(e));
              if (sign >= 0) tok.push_back(static_cast<char>(sign));
              rem -= 1 + (sign >= 0 ? 1 : 0);
              c = cc;
              while (rem > 0 && c >= '0' && c <= '9') take();
            } else {  // not an exponent: put 'e' (and the sign) back
              r.Unget(cc);
              if (sign >= 0) r.Unget(sign);
              // `c` (the 'e') is returned by the Unget below
            }
          }
        }
        if (c < 0) eof_hit = true;
        r.Unget(c);
        if (!digits) return done();
        const double d = std::strtod(tok.c_str(), nullptr);
        const std::uint64_t a = arg();
        if (a) {
          if (lenmod == 4) {
            const Vec128 q = DoubleToQuad(d);
            mem::W64(a, q[0]);
            mem::W64(a + 8, q[1]);
          } else if (lenmod == 3) {
            mem::W<double>(a, d);
          } else {
            mem::W<float>(a, static_cast<float>(d));
          }
        }
        if (!suppress) ++assigned;
        break;
      }
      default: return done();
    }
  }
  return assigned;
}
}  // namespace nova::hle::libc
