// Boot driver: replicates the Java start-up of N.O.V.A. Legacy (MainActivity + JNIBridge) without Java, and
// forwards host input / lifecycle events to the game's JNI natives (docs/interface/jni.md section c).
// The window / GL thread loop is not here: the game renders on its own guest thread (AndroidMain, started by
// OnGameInit inside NativeInit) and presents with eglSwapBuffers.
#pragma once
#include <memory>
#include <string>

#include "loader/elf64.h"

namespace nova::game {

struct BootOptions {
  // Run the module's DT_INIT / DT_INIT_ARRAY first (System.load does this). Set false if the caller already did.
  bool run_constructors = true;
  // Apply the native anti-tamper patches (ApplyGamePatches) before anything runs.
  bool apply_patches = true;
};

// Patches the native anti-tamper checks out by symbol name (hle::HookGuestFunction): ysM::ftC, ysM::GET_ALL_SIZES,
// ysM::CheckOfflineZoro, PackageUtils::IsAppEnc / IsRunOnEmulator / IsDeviceRouted. Call before any guest thread
// starts (Boot does it). Returns the number of functions patched.
int ApplyGamePatches(const loader::Module& module);

// The Java start-up: nova::jni::Init(), patches, constructors, JNI_OnLoad, NativeInit (which runs OnGameInit and
// spawns the game thread), initial connectivity/battery state, NativeSurfaceChanged(w, h), NativeOnResume.
// Runs on the calling thread (the "Java main thread"). Returns false (and logs why) if a step fails.
// Fill jni::config() (language, paths, hooks...) before calling.
bool Boot(const loader::Module& module, int screen_w, int screen_h, const BootOptions& opts = {});
inline bool Boot(const std::shared_ptr<loader::Module>& module, int screen_w, int screen_h, const BootOptions& opts = {}) {
  return module && Boot(*module, screen_w, screen_h, opts);
}

bool IsBooted();

// Input, all callable from any host thread after Boot. Coordinates are window pixels.
//   action: 0 = down, 1 = move, 2 = up, 3 = cancel (ignored by the game); id = pointer id (0 for the mouse).
void SendTouch(int action, int id, float x, float y);
// Android key code with its state. Only BACK (4) and MENU (82) reach the game, as in the Java.
void SendKey(int code, bool down);
inline void SendBack(bool down) { SendKey(4, down); }
// Text typed into the on-screen keyboard (VirtualKeyboard result).
void SendKeyboardText(const std::string& utf8);
// Window surface size change (e.g. after a resize).
void SendSurfaceChanged(int width, int height);
// Activity lifecycle.
void Pause();
void Resume();

}  // namespace nova::game
