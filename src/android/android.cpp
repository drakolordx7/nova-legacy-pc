#include "android/android.h"

#include "android/internal.h"

namespace nova::android {
void RegisterHle() {
  RegisterAssets();
  RegisterSensors();
}
}  // namespace nova::android
