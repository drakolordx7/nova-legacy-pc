// Fake Java side for the A64 guest: a JavaVM + JNIEnv (JNI 1.6, 233 function slots, each an HLE stub), an object
// model with local/global/weak references, and emulated Java classes (java.lang, android basics and every class
// libNOVA.so looks up, see docs/interface/jni.md). There is no Java: the host answers every JNI call itself.
// Adapted from Zombiewood's jni layer (MIT, (c) 2026 ChrisDutary) for A64: 64-bit references, 8-byte table slots,
// AAPCS64 varargs / va_list / jvalue arrays.
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "cpu/cpu.h"
#include "cpu/guest_memory.h"

namespace nova::jni {

// A JNI reference as the guest sees it (jobject / jclass / jstring / jarray ...): an opaque non-zero handle.
using jref = std::uint64_t;

// Everything the emulated Java side reports about the "device". Set what you need before Init() (defaults are a
// plausible high-end arm64 phone; empty language/country come from Windows).
struct Config {
  // Display: width/height of the window surface; dpi feeds DisplayMetrics (xdpi/ydpi/density).
  int screen_w = 1920, screen_h = 1080;
  float dpi = 420.0f;

  // Locale. Empty = the Windows user locale (e.g. "en", "US").
  std::string language, country;

  // Spoofed device identity (the game picks its quality profile from Build.MODEL and the GL strings).
  std::string manufacturer = "samsung", brand = "samsung", model = "SM-S918B", device = "dm3q", product = "dm3qxxx",
              board = "kalama", hardware = "qcom", fingerprint = "samsung/dm3qxxx/dm3q:13/TP1A.220624.014/S918BXXU2AWF1:user/release-keys";
  std::string release = "13", cpu_abi = "arm64-v8a;armeabi-v7a;armeabi";
  int sdk_int = 33;

  // Android-side paths the game is told about (the libc VFS maps them to host folders).
  std::string package = "com.gameloft.android.ANMP.GloftNOHM";
  std::string data_path = "/storage/emulated/0/Android/data/com.gameloft.android.ANMP.GloftNOHM/files";
  std::string save_path = "/data/data/com.gameloft.android.ANMP.GloftNOHM/files";
  std::string temp_path = "/data/data/com.gameloft.android.ANMP.GloftNOHM/cache";
  std::string sdcard_path = "/storage/emulated/0";
  std::string native_lib_path = "/data/app/com.gameloft.android.ANMP.GloftNOHM-1/lib/arm64";
  std::string apk_path = "/data/app/com.gameloft.android.ANMP.GloftNOHM-1/base.apk";
  std::string lib_so_path = "/data/data/com.gameloft.android.ANMP.GloftNOHM/files/DecRawsoLib/libNOVA.so";

  // Where SharedPreferences / DataSharing / the generated install id are kept (host file). Empty = memory only.
  std::filesystem::path prefs_file;

  // Hooks set by the front end (all optional).
  // Reads an APK asset ("assets/" prefix already stripped). Without it GetAssetAsString returns null.
  std::function<bool(const std::string& name, std::vector<std::uint8_t>& out)> read_asset;
  // Maps an Android path to a host path (the libc VFS mapping); used by file helpers (DeleteFile, free space...).
  std::function<std::filesystem::path(const std::string& android_path)> host_path;
  // AndroidUtils.ExitApplication(restart).
  std::function<void(bool restart)> on_exit;
  // AndroidUtils.ShowGameDialog(title, message, two_buttons): return true for the positive button. Default: log, true.
  std::function<bool(const std::string&, const std::string&, bool)> on_dialog;
  // AndroidUtils.LaunchBrowser(url): return true if handled. Default: log, false.
  std::function<bool(const std::string& url)> on_open_url;
  // AndroidUtils.ShowKeyboard: start text input (the result comes back through game::SendKeyboardText).
  std::function<void(const std::string& initial_text)> on_show_keyboard;
};
Config& config();

// Builds the fake JavaVM/JNIEnv tables and registers every emulated class. Idempotent; call before loading the
// module / running its constructors (it registers HLE handlers).
void Init();

// Guest address of the JavaVM* (what JNI_OnLoad receives).
gaddr JavaVMAddr();
// JNIEnv* of the calling host thread (creates it; the thread counts as attached, like a Java thread).
gaddr EnvForThread();

// The fake android.view.Surface / AssetManager objects. Both are process-wide global references: the HLE of
// ANativeWindow_fromSurface / AAssetManager_fromJava receives exactly these values (there is only one of each).
jref SurfaceObject();
jref AssetManagerObject();
// True when `r` refers to the fake Surface / AssetManager.
bool IsSurface(jref r);
bool IsAssetManager(jref r);

// Local reference frame for host code that creates references (restores the thread's reference table on exit).
struct LocalFrame {
  LocalFrame();
  ~LocalFrame();
  LocalFrame(const LocalFrame&) = delete;
  LocalFrame& operator=(const LocalFrame&) = delete;
};

// A (new local) reference to the class / a new Java string. For host code that calls natives.
jref FindClass(std::string_view name);
jref NewString(std::string_view utf8);
std::string GetString(jref s);

// Guest address of a Java native: an exported `Java_...` symbol of any loaded module, or one the library registered
// with RegisterNatives. `name` is `"pkg/Class.method"` (slashes or dots) or the full mangled `Java_...` name. 0 if
// there is none.
gaddr FindNative(std::string_view name);

// Runs a Java native on the calling thread: (JNIEnv*, jclass, args...). Returns nullopt-like false when the native
// does not exist. See CallNative below.
bool CallNativePacked(std::string_view cls_dot_method, const detail::ArgPack& args, Cpu::Result& out);

// Calls the Java native `cls_dot_method` (e.g. "com/gameloft/.../JNIBridge.NativeOnTouch") with the given
// arguments after (JNIEnv*, jclass). Logs and returns R{} if the native is missing.
template <class R = void, class... A>
R CallNative(std::string_view cls_dot_method, const A&... args) {
  LocalFrame frame;
  detail::ArgPack p;
  p.PushInt(EnvForThread());
  p.PushInt(FindClass(cls_dot_method.substr(0, cls_dot_method.rfind('.'))));
  (p.Push(args), ...);
  Cpu::Result r;
  if (!CallNativePacked(cls_dot_method, p, r)) {
    if constexpr (std::is_void_v<R>) return;
    else return R{};
  }
  return detail::UnpackResult<R>(r.x0, r.v0[0]);
}

// On-screen keyboard (AndroidUtils.ShowKeyboard / VirtualKeyboard). While visible, the front end sends the field's
// whole current text with KeyboardSetText (Java thread: it calls NativeSendKeyboardData; numeric fields are filtered
// like the Java EditText) and closes it with KeyboardHide (Enter / Back).
bool KeyboardVisible();
bool KeyboardNumeric();
std::string KeyboardText();
void KeyboardSetText(const std::string& utf8);
void KeyboardHide();

// Coverage: every FindClass / Get*MethodID / Get*FieldID / emulated call without an implementation the guest made,
// "NO" = answered with a generic default (also logged once as it happens).
struct CoverageEntry {
  enum class Kind { Class, Method, StaticMethod, Field, StaticField, Call } kind = Kind::Class;
  std::string cls, name, sig;
  bool covered = false;
  std::uint64_t count = 0;
};
std::vector<CoverageEntry> Coverage();
std::string CoverageReport();

}  // namespace nova::jni
