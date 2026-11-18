// AAPCS64 marshalling test for NOVA_HLE: guest code calls host handlers declared with plain C++ signatures.
#include <cstdio>
#include <cstring>
#include <string>

#include "core/log.h"
#include "cpu/cpu.h"
#include "cpu/guest_memory.h"
#include "cpu/hle_abi.h"
#include "guest_blob.h"

using namespace nova;

struct S16 { std::int64_t a, b; };
struct S12 { std::int32_t a, b, c; };
struct V4f { float a, b, c, d; };
struct V2d { double a, b; };
struct Big { std::int64_t a, b, c; };
NOVA_HLE_HFA(V4f, float, 4);
NOVA_HLE_HFA(V2d, double, 2);

namespace {
std::int64_t h_ints(std::int8_t a, std::uint8_t b, std::int16_t c, std::uint16_t d, std::int32_t e, std::uint32_t f, std::int64_t g,
                    std::uint64_t h, std::int64_t i, std::int32_t j) {
  return a + b + c + d + e + static_cast<std::int64_t>(f) + g + static_cast<std::int64_t>(h) + i + j;
}
double h_fp(float a, double b, float c, double d, float e, double f, float g, double h, float i, double j) {
  return a + b + c + d + e + f + g + h + i + j;
}
S16 h_s16(S16 s, std::int64_t k) { return {s.a + k, s.b * 2}; }
S12 h_s12(S12 s) { return {s.a + 1, s.b + 2, s.c + 3}; }
V4f h_v4f(V4f v, float k) { return {v.a * k, v.b * k, v.c * k, v.d * k}; }
V2d h_v2d(V2d v) { return {v.a * 2, v.b * 4}; }
Big h_big(Big b, std::int64_t k) { return {b.a + k, b.b + k, b.c + k}; }
std::int64_t h_spill(std::int64_t a0, std::int64_t a1, std::int64_t a2, std::int64_t a3, std::int64_t a4, std::int64_t a5,
                     std::int64_t a6, S16 s, std::int64_t last) {
  return a0 + a1 * 2 + a2 * 3 + a3 * 4 + a4 * 5 + a5 * 6 + a6 * 7 + s.a * 100 + s.b * 1000 + last * 10000;
}
// Alternating int64 / double, n values in total.
template <class VA>
std::int64_t SumAlternating(std::int64_t n, VA& va) {
  std::int64_t sum = 0;
  for (std::int64_t i = 0; i < n; ++i) {
    if (i % 2 == 0) sum += va.template Next<std::int64_t>();
    else sum += static_cast<std::int64_t>(va.template Next<double>() * 10);
  }
  return sum;
}
std::int64_t h_var(const char* tag, std::int32_t n, Varargs va) {
  if (std::strcmp(tag, "tag") != 0) return -1;
  return SumAlternating(n, va);
}
std::int64_t h_vl(std::int64_t n, VaList va) { return SumAlternating(n, va); }
std::int64_t h_str(const char* s, bool b) { return std::strlen(s) * 10 + (b ? 1 : 0); }
void h_out(std::int64_t* p) { *p = 424242; }
float h_f(float x) { return x * 2; }

int g_fail = 0;
void Check(const char* name, bool ok, const std::string& detail = {}) {
  if (!ok) ++g_fail;
  std::printf("%s %-28s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
}
double D(std::int64_t bits) { double d; std::memcpy(&d, &bits, 8); return d; }
}  // namespace

int main() {
  GuestMemory::Init();
  const std::size_t sz = sizeof(guest_blob::kBytes);
  GuestMemory::Map(guest_blob::kBase, sz, kProtRW, "abi-code");
  std::memcpy(g2h(guest_blob::kBase), guest_blob::kBytes, sz);
  GuestMemory::Protect(guest_blob::kBase, sz, kProtRX);
  constexpr gaddr kData = 0x4800'0000ull;
  GuestMemory::Map(kData, 1 << 16, kProtRW, "abi-data");

  gaddr run_all = 0, forward_vl = 0;
  for (const auto& s : guest_blob::kSyms) {
    if (!std::strcmp(s.name, "run_all")) run_all = s.addr;
    if (!std::strcmp(s.name, "forward_vl")) forward_vl = s.addr;
  }
  (void)forward_vl;

  const gaddr stubs = kData, out = kData + 0x1000;
  gaddr table[13] = {
      NOVA_HLE_THUNK("h_ints", h_ints), NOVA_HLE_THUNK("h_fp", h_fp),   NOVA_HLE_THUNK("h_s16", h_s16),
      NOVA_HLE_THUNK("h_s12", h_s12),   NOVA_HLE_THUNK("h_v4f", h_v4f), NOVA_HLE_THUNK("h_v2d", h_v2d),
      NOVA_HLE_THUNK("h_big", h_big),   NOVA_HLE_THUNK("h_spill", h_spill), NOVA_HLE_THUNK("h_var", h_var),
      NOVA_HLE_THUNK("h_vl", h_vl),     NOVA_HLE_THUNK("h_str", h_str), NOVA_HLE_THUNK("h_out", h_out),
      NOVA_HLE_THUNK("h_f", h_f)};
  std::memcpy(g2h(stubs), table, sizeof table);
  call_guest<void>(run_all, stubs, out);
  auto O = [&](int i) { return mem::R<std::int64_t>(out + 8 * i); };

  std::int64_t expect_ints = h_ints(-1, 200, -300, 60000, -5, 4000000000u, -7, 8, -9, -10);
  Check("ints (sign/zero ext, stack)", O(0) == expect_ints, std::format("{} vs {}", O(0), expect_ints));
  Check("float/double (v0-v7, stack)", D(O(1)) == 1.5 + 2.5 + 3.5 + 4.5 + 5.5 + 6.5 + 7.5 + 8.5 + 9.5 + 10.5, std::format("{}", D(O(1))));
  Check("struct <=16B arg+ret (x0:x1)", O(2) == 107 && O(3) == 400, std::format("{} {}", O(2), O(3)));
  Check("12-byte struct arg+ret", O(4) == 20406, std::format("{}", O(4)));
  Check("HFA float x4 arg+ret", D(O(5)) == 2.0 + 4 * 10 + 6 * 100 + 8 * 1000, std::format("{}", D(O(5))));
  Check("HFA double x2 arg+ret", D(O(6)) == 2.5 && D(O(7)) == 10.0, std::format("{} {}", D(O(6)), D(O(7))));
  Check("large struct (ptr arg, sret x8)", O(8) == 101 && O(9) == 102 && O(10) == 103, std::format("{} {} {}", O(8), O(9), O(10)));
  Check("struct spills to stack, NGRN=8", O(11) == h_spill(1, 2, 3, 4, 5, 6, 7, {1000, 2000}, 9), std::format("{}", O(11)));
  std::int64_t want_var = 0;
  for (int i = 1; i <= 10; ++i) want_var += i + static_cast<std::int64_t>((i + 0.5) * 10);
  Check("varargs from registers+stack", O(12) == want_var, std::format("{} vs {}", O(12), want_var));
  Check("va_list forwarded", O(13) == want_var, std::format("{} vs {}", O(13), want_var));
  Check("pointer + bool", O(14) == 21, std::format("{}", O(14)));
  Check("void handler writes via pointer", O(15) == 424242);
  Check("float return", D(O(16)) == 5.0, std::format("{}", D(O(16))));
  std::printf("%s\n", g_fail ? "ABI TEST FAILED" : "ABI TEST OK");
  return g_fail ? 1 : 0;
}
