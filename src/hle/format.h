// printf engine over the guest's variadic arguments (AAPCS64). Integers keep guest widths (long is 64-bit);
// `long double` (binary128 in the guest) is read as 16 bytes and converted, never forwarded to the host CRT.
#pragma once
#include <string>

#include "cpu/hle_abi.h"

namespace nova::hle {
std::string FormatPrintf(const char* fmt, VaList& va);
// binary128 -> double (round toward zero on the mantissa; good enough for logging).
double QuadToDouble(Vec128 q);
}  // namespace nova::hle
