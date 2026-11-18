// Fake JNI test: an AArch64 guest calls FindClass / GetStaticMethodID / CallStaticObjectMethod (varargs) /
// CallStaticIntMethodV (va_list) / CallStaticIntMethodA (jvalue[]) / NewStringUTF / GetStringUTFChars / primitive arrays /
// exceptions / JavaVM::GetEnv through the host's fake JNIEnv, and the host checks what its emulated Java saw.
#include <cstdio>
#include <cstring>
#include <string>

#include "core/log.h"
#include "cpu/cpu.h"
#include "cpu/guest_memory.h"
#include "jni/java.h"
#include "jni/jni.h"
#include "guest_blob.h"

using namespace nova;

namespace {
int g_fail = 0;
void Check(const char* name, bool ok, const std::string& detail = {}) {
  if (!ok) ++g_fail;
  std::printf("%s %-34s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
}
}  // namespace

int main() {
  GuestMemory::Init();
  const std::size_t sz = sizeof(guest_blob::kBytes);
  GuestMemory::Map(guest_blob::kBase, sz, kProtRW, "jni-code");
  std::memcpy(g2h(guest_blob::kBase), guest_blob::kBytes, sz);
  GuestMemory::Protect(guest_blob::kBase, sz, kProtRX);
  constexpr gaddr kData = 0x4800'0000ull;
  GuestMemory::Map(kData, 1 << 16, kProtRW, "jni-data");

  gaddr run_all = 0;
  for (const auto& s : guest_blob::kSyms)
    if (!std::strcmp(s.name, "run_all")) run_all = s.addr;

  jni::Init();

  // The "Java" the guest calls: test.Calc with three static methods, and a class absent from the APK.
  std::string seen;
  jni::DefineClass("test/Calc")
      .Static("calc", "(IDJLjava/lang/String;)Ljava/lang/String;",
              [&](jni::Call& c) {
                seen = std::format("{}|{:.2f}|{}|{}", c.I(0), c.F(1), c.J(2), c.S(3));
                return jni::Value::Str(seen);
              })
      .Static("mix", "(IJD)I", [](jni::Call& c) { return jni::Value::Int(c.I(0) + c.J(1) + static_cast<std::int64_t>(c.F(2) * 10)); })
      .Static("sum", "([I)I", [](jni::Call& c) {
        auto a = std::dynamic_pointer_cast<jni::Array>(c.O(0));
        std::int64_t s = 0;
        for (std::uint32_t i = 0; a && i < a->length; ++i) s += mem::R<std::int32_t>(a->data + 4ull * i);
        return jni::Value::Int(s);
      });
  jni::GetClass("test/Missing")->missing = true;

  const gaddr env = jni::EnvForThread();
  const gaddr out = kData, buf = kData + 0x1000;
  call_guest<void>(run_all, env, jni::JavaVMAddr(), out, buf);
  auto O = [&](int i) { return mem::R<std::int64_t>(out + 8 * i); };

  Check("FindClass", O(0) == 1);
  Check("GetStaticMethodID x3", O(1) == 1);
  Check("CallStaticObjectMethod varargs", O(2) == 1 && seen == "7|2.50|1234567890123|h\xC3\xA9llo", seen);
  const std::string got = mem::CString(buf);
  Check("GetStringUTFChars round trip", got == seen && O(3) == static_cast<std::int64_t>(seen.size()), got);
  Check("GetStringUTFLength / GetStringLength", O(4) == 6 && O(5) == 5, std::format("{} {}", O(4), O(5)));
  Check("CallStaticIntMethodV (va_list)", O(6) == 3 + 4 + 25, std::format("{}", O(6)));
  Check("CallStaticIntMethodA (jvalue[])", O(7) == 3 + 4 + 25, std::format("{}", O(7)));
  Check("int array direct pointer", O(8) == 4 && O(9) == 10, std::format("{} {}", O(8), O(9)));
  Check("missing class -> null + exception", O(10) == 1 && O(11) == 1 && O(12) == 0, std::format("{} {} {}", O(10), O(11), O(12)));
  Check("global ref type", O(13) == 2, std::format("{}", O(13)));
  Check("JavaVM::GetEnv", O(14) == 1);

  std::printf("%s\n", g_fail ? "JNI TEST FAILED" : "JNI TEST OK");
  return g_fail ? 1 : 0;
}
