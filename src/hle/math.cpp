// libm: the 36 functions libNOVA.so imports (plus isnan, which the headers expose as a function), as FAST leaf
// handlers on the host CRT. float variants take/return `float` in s regs, doubles in d regs (AAPCS64). Results can
// differ from bionic in the last ulp (irrelevant except for deterministic multiplayer simulation).
#include <cmath>
#include <cstdint>

#include "hle/libc_internal.h"

namespace nova::hle::libc {
namespace {
#define UNARY(name)                                       \
  float f_##name(float x) { return std::name(x); }        \
  double d_##name(double x) { return std::name(x); }
UNARY(sin) UNARY(cos) UNARY(tan) UNARY(asin) UNARY(acos) UNARY(atan) UNARY(sinh) UNARY(cosh) UNARY(tanh)
UNARY(exp) UNARY(exp2) UNARY(log) UNARY(log10) UNARY(sqrt)
#undef UNARY

float f_atan2(float y, float x) { return std::atan2(y, x); }
double d_atan2(double y, double x) { return std::atan2(y, x); }
float f_pow(float x, float y) { return std::pow(x, y); }
double d_pow(double x, double y) { return std::pow(x, y); }
float f_fmod(float x, float y) { return std::fmod(x, y); }
double d_fmod(double x, double y) { return std::fmod(x, y); }
float f_ldexp(float x, std::int32_t e) { return std::ldexp(x, e); }
double d_ldexp(double x, std::int32_t e) { return std::ldexp(x, e); }
double d_frexp(double x, std::int32_t* e) { return std::frexp(x, e); }
double d_modf(double x, double* ip) { return std::modf(x, ip); }
float f_modf(float x, float* ip) { return std::modf(x, ip); }
std::int32_t d_isnan(double x) { return std::isnan(x) ? 1 : 0; }
}  // namespace

void RegisterMath() {
  NOVA_HLE_FAST("sin", d_sin);   NOVA_HLE_FAST("sinf", f_sin);
  NOVA_HLE_FAST("cos", d_cos);   NOVA_HLE_FAST("cosf", f_cos);
  NOVA_HLE_FAST("tan", d_tan);   NOVA_HLE_FAST("tanf", f_tan);
  NOVA_HLE_FAST("asin", d_asin); NOVA_HLE_FAST("asinf", f_asin);
  NOVA_HLE_FAST("acos", d_acos); NOVA_HLE_FAST("acosf", f_acos);
  NOVA_HLE_FAST("atan", d_atan); NOVA_HLE_FAST("atanf", f_atan);
  NOVA_HLE_FAST("sinh", d_sinh); NOVA_HLE_FAST("sinhf", f_sinh);
  NOVA_HLE_FAST("cosh", d_cosh); NOVA_HLE_FAST("coshf", f_cosh);
  NOVA_HLE_FAST("tanh", d_tanh); NOVA_HLE_FAST("tanhf", f_tanh);
  NOVA_HLE_FAST("exp", d_exp);   NOVA_HLE_FAST("expf", f_exp);
  NOVA_HLE_FAST("exp2", d_exp2); NOVA_HLE_FAST("exp2f", f_exp2);
  NOVA_HLE_FAST("log", d_log);   NOVA_HLE_FAST("logf", f_log);
  NOVA_HLE_FAST("log10", d_log10); NOVA_HLE_FAST("log10f", f_log10);
  NOVA_HLE_FAST("sqrt", d_sqrt); NOVA_HLE_FAST("sqrtf", f_sqrt);
  NOVA_HLE_FAST("atan2", d_atan2); NOVA_HLE_FAST("atan2f", f_atan2);
  NOVA_HLE_FAST("pow", d_pow);   NOVA_HLE_FAST("powf", f_pow);
  NOVA_HLE_FAST("fmod", d_fmod); NOVA_HLE_FAST("fmodf", f_fmod);
  NOVA_HLE_FAST("ldexp", d_ldexp); NOVA_HLE_FAST("ldexpf", f_ldexp);
  NOVA_HLE_FAST("frexp", d_frexp);
  NOVA_HLE_FAST("modf", d_modf); NOVA_HLE_FAST("modff", f_modf);
  NOVA_HLE_FAST("isnan", d_isnan);
}
}  // namespace nova::hle::libc
