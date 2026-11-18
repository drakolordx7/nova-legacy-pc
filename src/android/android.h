// libandroid imports that are not about the window: AAssetManager / AAsset / AAssetDir over the user's APK,
// ALooper, ASensorManager / ASensorEventQueue (the PC has no sensors). ANativeWindow_* lives in native_window.cpp.
#pragma once
#include <string>

#include "cpu/guest_memory.h"

namespace nova::android {
// Registers every non-window libandroid import. Call before loading the module.
void RegisterHle();

// The APK whose assets/ folder backs AAssetManager (UTF-8 path). May be called before or after RegisterHle; the
// archive is opened on first use.
void SetApkPath(const std::string& path);

// The guest AAssetManager* that AAssetManager_fromJava returns. The JNI side's GetAssetManager() object is a
// placeholder: whatever jobject reaches AAssetManager_fromJava maps to this one handle.
gaddr AssetManagerHandle();
}  // namespace nova::android
