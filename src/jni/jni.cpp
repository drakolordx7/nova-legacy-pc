// Registration of the fake Java layer.
#include <mutex>

#include "core/log.h"
#include "cpu/cpu.h"
#include "jni/internal.h"

namespace nova::jni {

void Init() {
  static std::once_flag once;
  std::call_once(once, [] {
    InitCoreClasses();
    BuildTables();
    RegisterAndroidClasses();
    RegisterGameClasses();
    threads::AddExitHook([](Cpu& cpu) { DestroyThreadState(cpu); });
    log::Info("[jni] fake JavaVM/JNIEnv ready (JavaVM at 0x{:x}, {} slots)", JavaVMAddr(), kJniSlots);
  });
}

}  // namespace nova::jni
