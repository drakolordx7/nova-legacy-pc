// Boot driver. See game.h.
#include "game/game.h"
#include "game/iap.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "core/log.h"
#include "cpu/cpu.h"
#include "jni/jni.h"
#include "pc/layer.h"

namespace nova::game {
namespace {
constexpr const char* kBridge = "com/gameloft/android/ANMP/GloftNOHM/PackageUtils/JNIBridge";
std::atomic<bool> g_booted{false};

std::string Native(const char* name) { return std::string(kBridge) + "." + name; }
}  // namespace

bool IsBooted() { return g_booted.load(); }

bool Boot(const loader::Module& module, int screen_w, int screen_h, const BootOptions& opts) {
  if (g_booted.load()) {
    log::Warn("[game] Boot called twice");
    return true;
  }
  jni::config().screen_w = screen_w;
  jni::config().screen_h = screen_h;
  jni::Init();
  if (opts.apply_patches) {
    ApplyGamePatches(module);
    ApplyIapPatches(module);  // free offline purchases (docs/iap.md)
    // PC layer hooks (raw mouse look, Hud input, frame time...): before any guest thread starts.
    log::Info("[game] {} PC layer hooks installed", pc::InstallLayer(module));
  }

  // System.load -> the library's constructors, then JNI_OnLoad(JavaVM*, NULL) (SetVm, GetEnv, LoadClasses).
  if (opts.run_constructors) {
    const loader::InitReport rep = loader::RunInit(module);
    if (!rep.ok) {
      log::Error("[game] constructor #{} ({}) failed after {} of {}: {}", rep.failed_index, rep.failed_sym, rep.ran, rep.total, rep.fault);
      return false;
    }
    log::Info("[game] {} constructors ran", rep.ran);
  }
  // System.load runs on a Java thread, so JavaVM::GetEnv must succeed here: attach the calling (host) thread first.
  jni::EnvForThread();
  const gaddr on_load = module.Lookup("JNI_OnLoad");
  if (!on_load) {
    log::Error("[game] {} exports no JNI_OnLoad", module.name);
    return false;
  }
  const std::int32_t version = call_guest<std::int32_t>(on_load, jni::JavaVMAddr(), std::uint64_t{0});
  if (version != 0x10006) log::Warn("[game] JNI_OnLoad returned 0x{:x} (expected 0x10006)", static_cast<std::uint32_t>(version));
  else log::Info("[game] JNI_OnLoad ok");

  // MainActivity.p() -> NativeInit: Internal::Init() then OnGameInit(), which starts the game thread (AndroidMain).
  if (!jni::FindNative(Native("NativeInit"))) {
    log::Error("[game] NativeInit is missing");
    return false;
  }
  jni::CallNative(Native("NativeInit"));
  log::Info("[game] NativeInit done (game thread started)");

  // On a phone surfaceChanged arrives long after NativeInit; OnWindowStateChange dereferences glf::gAppImpl, which the
  // game thread sets in NewApp. Wait for it instead of racing the game thread.
  if (const gaddr app_impl = module.Lookup("_ZN3glf8gAppImplE")) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!mem::R64(app_impl) && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (!mem::R64(app_impl)) {
      log::Error("[game] the game thread did not create glf::gAppImpl within 60 s");
      return false;
    }
    log::Info("[game] glf::App created");
  }

  // Plugin start-up state the Java pushes into native: no connectivity, desktop on AC power.
  jni::CallNative(Native("SetConnectionType"), 0);
  jni::CallNative(Native("SetBatteryInfo"), false, false, true, 100);

  // surfaceChanged -> NativeSurfaceChanged(Surface, w, h): ANativeWindow_fromSurface receives jni::SurfaceObject().
  jni::CallNative(Native("NativeSurfaceChanged"), jni::SurfaceObject(), screen_w, screen_h);
  log::Info("[game] surface {}x{}", screen_w, screen_h);

  // onResume -> AudioListenerPlugin.onPreNativeResume (user music off), NativeOnResume.
  g_booted.store(true);
  Resume();
  return true;
}

void SendTouch(int action, int id, float x, float y) {
  if (!g_booted.load()) return;
  jni::CallNative(Native("NativeOnTouch"), action, x, y, id);
}

void SendKey(int code, bool down) {
  if (!g_booted.load()) return;
  jni::CallNative(Native("NativeKeyAction"), code, down);
}

void SendKeyboardText(const std::string& utf8) {
  if (!g_booted.load()) return;
  jni::LocalFrame frame;
  jni::CallNative(Native("NativeSendKeyboardData"), jni::NewString(utf8));
}

void SendSurfaceChanged(int width, int height) {
  if (!g_booted.load()) return;
  jni::config().screen_w = width;
  jni::config().screen_h = height;
  jni::CallNative(Native("NativeSurfaceChanged"), jni::SurfaceObject(), width, height);
}

void Pause() {
  if (!g_booted.load()) return;
  jni::CallNative(Native("NativeOnPause"));
}

void Resume() {
  if (!g_booted.load()) return;
  jni::CallNative(Native("NativeUserMusicIsPlaying"), false);
  jni::CallNative(Native("NativeOnResume"));
}

}  // namespace nova::game
