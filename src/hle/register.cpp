#include "hle/libc.h"
#include "hle/libc_internal.h"

namespace nova::hle::libc {
// Everything RegisterMinimal() registers, then the complete layer; a handler registered later replaces the minimal one of
// the same name. Call before loading any module (data symbols such as __sF and timezone are defined here).
void RegisterAll() {
  RegisterMinimal();
  RegisterPthread();
  RegisterRuntime();
  RegisterString();
  RegisterWchar();
  RegisterTime();
  RegisterMath();
  RegisterPosix();
  RegisterStdio();
}
}  // namespace nova::hle::libc
