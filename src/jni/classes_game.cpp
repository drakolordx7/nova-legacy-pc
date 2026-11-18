// The Java classes N.O.V.A. Legacy's native code looks up (docs/interface/jni.md section b), emulated for an offline,
// single-player PC build: spoofed arm64 phone, language from Windows, Android storage paths (the libc VFS maps them
// to host folders), network reported offline, ads / IAP / social / push / analytics / pop-ups are no-ops, permissions
// are all granted, and every anti-tamper Java check answers like an untampered install.
// Classes whose members are not listed explicitly are "no-op" classes: any method returns 0 / false / "" / empty
// String[] / null. Members the game really needs are implemented below.
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <random>
#include <thread>

#include "core/log.h"
#include "jni/internal.h"
#include "jni/jni.h"

namespace nova::jni {
namespace {

constexpr const char* kPkg = "com/gameloft/android/ANMP/GloftNOHM/";
std::string P(const char* suffix) { return std::string(kPkg) + suffix; }
const std::string kJniBridge = "com/gameloft/android/ANMP/GloftNOHM/PackageUtils/JNIBridge";

// On-screen keyboard state (GLUtils.VirtualKeyboard).
struct KeyboardState {
  std::mutex mu;
  bool visible = false;
  int type = 1;     // Android input type; 3 (2 is mapped to 3) = numeric
  int max_len = 0;  // 0 = unlimited
  std::string text;
};
KeyboardState g_kb;

Value Void() { return Value(); }
Impl RetStr(std::function<std::string()> f) {
  return [f](Call&) { return Value::Str(f()); };
}
Impl RetConstStr(std::string s) {
  return [s](Call&) { return Value::Str(s); };
}
Impl RetInt(std::int64_t v) {
  return [v](Call&) { return Value::Int(v); };
}
Impl RetBool(bool v) {
  return [v](Call&) { return Value::Bool(v); };
}
Impl RetFloat(double v) {
  return [v](Call&) { return Value::Float(v); };
}
Impl Noop() {
  return [](Call&) { return Value(); };
}

// ---------------------------------------------------------------------------------------------
// Host helpers
// ---------------------------------------------------------------------------------------------
std::string WideToUtf8(const wchar_t* w, int n = -1) {
  if (!w || !*w) return {};
  const int len = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<std::size_t>(len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, n, s.data(), len, nullptr, nullptr);
  while (!s.empty() && s.back() == '\0') s.pop_back();
  return s;
}
std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return {};
  const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), len);
  return w;
}

// Windows user locale "en-US" / "pt-BR" / "zh-Hans-CN" -> ("en", "US").
void WindowsLocale(std::string& lang, std::string& country) {
  wchar_t buf[LOCALE_NAME_MAX_LENGTH] = {};
  std::string name = GetUserDefaultLocaleName(buf, LOCALE_NAME_MAX_LENGTH) ? WideToUtf8(buf) : "en-US";
  lang = "en";
  country = "US";
  const auto first = name.find('-');
  lang = name.substr(0, first);
  std::transform(lang.begin(), lang.end(), lang.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const auto last = name.rfind('-');
  if (first != std::string::npos && name.size() - last - 1 == 2) {
    country = name.substr(last + 1);
    std::transform(country.begin(), country.end(), country.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  }
}
std::string Language() {
  std::string l = config().language, c;
  if (l.empty()) WindowsLocale(l, c);
  return l;
}
std::string Country() {
  std::string c = config().country, l;
  if (c.empty()) WindowsLocale(l, c);
  return c;
}

std::string RandomUuid() {
  std::uint8_t b[16] = {};
  BCryptGenRandom(nullptr, b, sizeof b, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  b[6] = static_cast<std::uint8_t>((b[6] & 0x0F) | 0x40);
  b[8] = static_cast<std::uint8_t>((b[8] & 0x3F) | 0x80);
  return std::format("{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}", b[0], b[1],
                     b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

bool Hash(BCRYPT_ALG_HANDLE alg, const std::string& in, std::vector<std::uint8_t>& out, std::size_t len) {
  out.assign(len, 0);
  return BCryptHash(alg, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(in.data())), static_cast<ULONG>(in.size()), out.data(),
                    static_cast<ULONG>(out.size())) >= 0;
}

std::vector<std::uint8_t> Base64Decode(const std::string& s) {
  std::vector<std::uint8_t> out;
  std::uint32_t acc = 0;
  int bits = 0;
  for (unsigned char ch : s) {
    int v;
    if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
    else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
    else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
    else if (ch == '+') v = 62;
    else if (ch == '/') v = 63;
    else continue;  // '=' and whitespace
    acc = (acc << 6) | static_cast<std::uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xFF));
    }
  }
  return out;
}

// SUtils.GetPlainText / DefReader.a(data, key): AES/ECB/PKCS5 with the key = the first 16 hex characters of
// SHA-256(key) used as ASCII bytes; the data is Base64. Returns false on any failure (Java returns null).
bool DefReaderDecode(const std::string& b64, const std::string& key, std::string& out) {
  std::vector<std::uint8_t> sha;
  if (!Hash(BCRYPT_SHA256_ALG_HANDLE, key, sha, 32)) return false;
  std::string hex;
  for (std::uint8_t b : sha) hex += std::format("{:02x}", b);
  std::vector<std::uint8_t> k(hex.begin(), hex.begin() + 16);
  std::vector<std::uint8_t> data = Base64Decode(b64);
  if (data.empty() || data.size() % 16) return false;

  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_KEY_HANDLE hk = nullptr;
  bool ok = false;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) >= 0) {
    if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_ECB)),
                          sizeof(BCRYPT_CHAIN_MODE_ECB), 0) >= 0 &&
        BCryptGenerateSymmetricKey(alg, &hk, nullptr, 0, k.data(), static_cast<ULONG>(k.size()), 0) >= 0) {
      std::vector<std::uint8_t> plain(data.size());
      ULONG got = 0;
      if (BCryptDecrypt(hk, data.data(), static_cast<ULONG>(data.size()), nullptr, nullptr, 0, plain.data(), static_cast<ULONG>(plain.size()), &got, 0) >= 0 &&
          got == plain.size()) {
        const std::uint8_t pad = plain.back();
        if (pad >= 1 && pad <= 16 && pad <= plain.size()) {
          out.assign(reinterpret_cast<char*>(plain.data()), plain.size() - pad);
          ok = true;
        }
      }
    }
    if (hk) BCryptDestroyKey(hk);
    BCryptCloseAlgorithmProvider(alg, 0);
  }
  return ok;
}

// SUtils.getGLUID(String): MD5(arg + "com.gameloft.android.ANMP.GloftNOHM") split into 4 big-endian ints.
std::shared_ptr<Array> GlUid(const std::string& arg) {
  std::vector<std::uint8_t> md5;
  const std::string s = arg + "com.gameloft.android.ANMP.GloftNOHM";
  if (!Hash(BCRYPT_MD5_ALG_HANDLE, s, md5, 16)) return MakeIntArray({0, 0, 0, 0});
  auto be = [&](int i) {
    return static_cast<std::int32_t>((std::uint32_t(md5[i]) << 24) | (std::uint32_t(md5[i + 1]) << 16) | (std::uint32_t(md5[i + 2]) << 8) | md5[i + 3]);
  };
  return MakeIntArray({be(0), be(4), be(8), be(12)});
}

// Windows clipboard (ClipboardPlugin).
void ClipboardSet(const std::string& text) {
  if (!OpenClipboard(nullptr)) return;
  EmptyClipboard();
  const std::wstring w = Utf8ToWide(text);
  if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t))) {
    if (void* p = GlobalLock(h)) {
      std::memcpy(p, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
      GlobalUnlock(h);
      if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    } else {
      GlobalFree(h);
    }
  }
  CloseClipboard();
}
std::string ClipboardGet() {
  std::string out;
  if (!OpenClipboard(nullptr)) return out;
  if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
    if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
      out = WideToUtf8(p);
      GlobalUnlock(h);
    }
  }
  CloseClipboard();
  return out;
}

std::uint64_t BootMs() {
  static const std::uint64_t t0 = GetTickCount64();
  return GetTickCount64() - t0;
}

std::filesystem::path HostPath(const std::string& android_path) {
  const auto& cfg = config();
  return cfg.host_path ? cfg.host_path(android_path) : std::filesystem::path();
}

// Preference namespaces: one per SharedPreferences file name.
std::string PrefNs(const std::string& file) { return "pref:" + file; }

// Singletons handed to HLE code through SurfaceObject() / AssetManagerObject().
jref g_surface = 0, g_asset_manager = 0;

// ---------------------------------------------------------------------------------------------
// AndroidUtils (REQUIRED) and SUtils: shared members
// ---------------------------------------------------------------------------------------------
void DefineUtilMembers(ClassDef d) {
  const char* S = "()Ljava/lang/String;";

  // Paths.
  d.Static("RetrieveDataPath", S, RetStr([] { return config().data_path; }))
      .Static("RetrieveSavePath", S, RetStr([] { return config().save_path; }))
      .Static("RetrieveTempPath", S, RetStr([] { return config().temp_path; }))
      .Static("RetrieveSDCardPath", S, RetStr([] { return config().sdcard_path; }))
      .Static("RetrieveNativeLibraryPath", S, RetStr([] { return config().native_lib_path; }))
      .Static("RetrieveObbPath", S, RetConstStr("NA"))  // the constant string the Java returns: no OBB is used
      .Static("GetApkPath", S, RetStr([] { return config().apk_path; }))
      .Static("GetLibSoPath", S, RetStr([] { return config().lib_so_path; }))
      .Static("GetMetaDataValue", "(Ljava/lang/String;)Ljava/lang/String;", RetConstStr(""))
      .Static("GetDiskFreeSpace", "(Ljava/lang/String;)J", [](Call& c) {
        std::error_code ec;
        const auto hp = HostPath(c.S(0));
        if (!hp.empty()) {
          std::filesystem::create_directories(hp, ec);
          const auto sp = std::filesystem::space(hp, ec);
          if (!ec) return Value::Int(static_cast<std::int64_t>(sp.available));
        }
        return Value::Int(200ll << 30);
      })
      .Static("GetDiskTotalSpace", "()I", RetInt(512 * 1024))  // MiB
      .Static("DeleteFile", "(Ljava/lang/String;)V", [](Call& c) {
        const auto hp = HostPath(c.S(0));
        std::error_code ec;
        if (!hp.empty()) std::filesystem::remove(hp, ec);
        else log::Debug("[jni] DeleteFile({}) ignored (no host path mapping)", c.S(0));
        return Value();
      })
      .Static("RemoveDirectoryRecursively", "(Ljava/lang/String;)Z", [](Call& c) {
        const auto hp = HostPath(c.S(0));
        std::error_code ec;
        if (hp.empty()) return Value::Bool(false);
        std::filesystem::remove_all(hp, ec);
        return Value::Bool(!ec);
      })
      .Static("GenericUnzipArchive", "(Ljava/lang/String;Ljava/lang/String;)Z", RetBool(false))
      .Static("GetAssetManager", "()Landroid/content/res/AssetManager;", [](Call&) { return Value::Obj(Resolve(g_asset_manager)); })
      .Static("GetAssetAsString", "(Ljava/lang/String;)[B", [](Call& c) {
        std::string name = c.S(0);
        if (!name.empty() && name.front() == '.') name.erase(0, 1);  // the Java strips a leading "."
        while (!name.empty() && name.front() == '/') name.erase(0, 1);
        std::vector<std::uint8_t> data;
        if (!config().read_asset || !config().read_asset(name, data)) return Value::Null();
        return Value::Obj(MakeByteArray(data.data(), static_cast<std::uint32_t>(data.size())));
      });

  // Device identity (spoofed).
  d.Static("GetAndroidID", S, RetStr([] { return InstallId().substr(0, 8) + InstallId().substr(9, 4) + InstallId().substr(14, 4); }))
      .Static("GetSerial", S, RetConstStr(""))
      .Static("GetCPUSerial", S, RetConstStr(""))
      .Static("GetDeviceManufacturer", S, RetStr([] { return config().manufacturer; }))
      .Static("GetDeviceModel", S, RetStr([] { return config().model; }))
      .Static("GetPhoneProduct", S, RetStr([] { return config().product; }))
      .Static("GetPhoneDevice", S, RetStr([] { return config().device; }))
      .Static("GetBuildBoard", S, RetStr([] { return config().board; }))
      .Static("GetFirmware", S, RetStr([] { return config().release; }))
      .Static("GetCPUAbi", S, RetStr([] { return config().cpu_abi; }))
      .Static("GetMacAddress", S, RetConstStr("02:00:00:00:00:00"))
      .Static("GetDeviceIMEI", S, RetConstStr(""))
      .Static("GetHDIDFV", S, RetStr([] { return InstallId(); }))
      .Static("GetGameName", S, RetConstStr("ANMP.GloftNOHM"))
      .Static("GetUserAgent", S, RetStr([] {
        return "Dalvik/2.1.0 (Linux; U; Android " + config().release + "; " + config().model + " Build/TP1A.220624.014)";
      }))
      .Static("GetGoogleAdId", S, RetConstStr(""))
      .Static("GetGoogleAdIdStatus", "()I", RetInt(2))  // GooglePlayServicesNotAvailable
      .Static("GetInjectedIGP", S, RetConstStr(""))
      .Static("GetInjectedSerialKey", S, RetConstStr(""))
      .Static("GetDefaultIGP", S, RetConstStr(""))
      .Static("getVersionCodes", "()I", [](Call&) { return Value::Int(config().sdk_int); })
      .Static("GetElapsedRealtime", "()J", [](Call&) { return Value::Int(static_cast<std::int64_t>(BootMs() + 3600000)); });

  // Language / country / carrier.
  d.Static("GetDeviceLanguage", S, RetStr([] { return Language(); }))
      .Static("GetCountry", S, RetStr([] { return Country(); }))
      .Static("GetDeviceSettingsCountryCode", S, RetStr([] { return Country(); }))
      .Static("GetSimIsoCountryCode", S, RetConstStr(""))
      .Static("GetCarrierAgent", S, RetConstStr(""))
      .Static("IsSimReady", "()Z", RetBool(false));

  // Screen.
  d.Static("GetXDpi", "()F", [](Call&) { return Value::Float(config().dpi); })
      .Static("GetYDpi", "()F", [](Call&) { return Value::Float(config().dpi); });

  // Preferences (SharedPreferences, persisted in the host prefs file): (key, file, value).
  const char* PS = "Ljava/lang/String;";
  auto sig = [&](const char* tail) { return std::string("(") + PS + PS + tail; };
  d.Static("SavePreferenceString", sig("Ljava/lang/String;)V"), [](Call& c) {
     PrefsSet(PrefNs(c.S(1)), c.S(0), c.S(2));
     return Value();
   })
      .Static("SavePreferenceInt", sig("I)V"), [](Call& c) {
        PrefsSet(PrefNs(c.S(1)), c.S(0), std::to_string(c.I(2)));
        return Value();
      })
      .Static("SavePreferenceLong", sig("J)V"), [](Call& c) {
        PrefsSet(PrefNs(c.S(1)), c.S(0), std::to_string(c.J(2)));
        return Value();
      })
      .Static("SavePreferenceBool", sig("Z)V"), [](Call& c) {
        PrefsSet(PrefNs(c.S(1)), c.S(0), c.Z(2) ? "true" : "false");
        return Value();
      })
      .Static("GetPreferenceString", sig("Ljava/lang/String;)Ljava/lang/String;"), [](Call& c) {
        bool f = false;
        std::string v = PrefsGet(PrefNs(c.S(1)), c.S(0), &f);
        return f ? Value::Str(v) : Value::Obj(c.O(2));
      })
      .Static("GetPreferenceInt", sig("I)I"), [](Call& c) {
        bool f = false;
        const std::string v = PrefsGet(PrefNs(c.S(1)), c.S(0), &f);
        return Value::Int(f ? std::strtol(v.c_str(), nullptr, 10) : c.I(2));
      })
      .Static("GetPreferenceLong", sig("J)J"), [](Call& c) {
        bool f = false;
        const std::string v = PrefsGet(PrefNs(c.S(1)), c.S(0), &f);
        return Value::Int(f ? std::strtoll(v.c_str(), nullptr, 10) : c.J(2));
      })
      .Static("GetPreferenceBool", sig("Z)Z"), [](Call& c) {
        bool f = false;
        const std::string v = PrefsGet(PrefNs(c.S(1)), c.S(0), &f);
        return Value::Bool(f ? v == "true" : c.Z(2));
      })
      .Static("RemovePreference", "(Ljava/lang/String;Ljava/lang/String;)V", [](Call& c) {
        PrefsRemove(PrefNs(c.S(1)), c.S(0));
        return Value();
      });

  // Dialogs, toasts, keyboard, browser, app lifecycle (host: log, or the front end's hook).
  d.Static("ShowGameDialog", "(Ljava/lang/String;Ljava/lang/String;Z)Z", [](Call& c) {
     log::Info("[jni] dialog \"{}\": {}", c.S(0), c.S(1));
     return Value::Bool(config().on_dialog ? config().on_dialog(c.S(0), c.S(1), c.Z(2)) : true);
   })
      .Static("ShowToastMessage", "(Ljava/lang/String;I)V", [](Call& c) {
        log::Info("[jni] toast: {}", c.S(0));
        return Value();
      })
      .Static("ShowCannotGoBack", "()V", Noop())
      // VirtualKeyboard: ShowKeyboard(text, inputType, imeFlag, unused, maxLength). The front end types into it
      // with KeyboardSetText and closes it with KeyboardHide (jni.h).
      .Static("ShowKeyboard", "(Ljava/lang/String;IIII)V", [](Call& c) {
        {
          std::lock_guard lk(g_kb.mu);
          if (g_kb.visible) return Value();
          g_kb.visible = true;
          g_kb.type = c.I(1) == 2 ? 3 : c.I(1);
          g_kb.max_len = c.I(4);
          g_kb.text = c.S(0);
          // EditText.setText runs onTextChanged, whose numeric filter drops a non-digit start ("-" -> "").
          if (g_kb.type == 3) {
            std::string digits;
            for (char ch : g_kb.text) {
              if (ch < '0' || ch > '9') break;
              digits += ch;
            }
            g_kb.text = (!digits.empty() && digits[0] == '0') ? std::string("-") : digits;
          }
        }
        log::Info("[jni] ShowKeyboard(\"{}\", type {}, max {})", c.S(0), c.I(1), c.I(4));
        if (config().on_show_keyboard) config().on_show_keyboard(c.S(0));
        return Value();
      })
      .Static("HideKeyboard", "()V", [](Call&) {
        KeyboardHide();
        return Value();
      })
      .Static("IsKeyboardVisible", "()Z", [](Call&) { return Value::Bool(KeyboardVisible()); })
      .Static("GetVKeyboardText", S, [](Call&) { return Value::Str(KeyboardText()); })
      .Static("SetVKeyboardText", "(Ljava/lang/String;)V", [](Call& c) {
        std::lock_guard lk(g_kb.mu);
        if (g_kb.visible) g_kb.text = c.S(0);
        return Value();
      })
      .Static("LaunchBrowser", "(Ljava/lang/String;)Z", [](Call& c) {
        log::Info("[jni] LaunchBrowser({})", c.S(0));
        return Value::Bool(config().on_open_url ? config().on_open_url(c.S(0)) : false);
      })
      .Static("LaunchVideoPlayer", "(Ljava/lang/String;)Z", RetBool(false))
      .Static("LaunchWifiSettings", "()Z", RetBool(false))
      .Static("HttpExecuteAsync", "(Ljava/lang/String;)V", [](Call& c) {
        log::Debug("[jni] HttpExecuteAsync({}) dropped (offline)", c.S(0));
        return Value();
      })
      .Static("MinimizeApplication", "()V", Noop())
      .Static("SetKeepScreenOn", "(Z)V", Noop())
      .Static("SetOrientation", "(Z)V", Noop())
      .Static("ExitApplication", "(Z)V", [](Call& c) {
        log::Info("[jni] ExitApplication(restart={})", c.Z(0));
        if (config().on_exit) config().on_exit(c.Z(0));
        else throw GuestExit{0};
        return Value();
      })
      .Static("AreNotificationsEnabled", "()Z", RetBool(true))
      .Static("EnableUserLocation", "()V", Noop())
      .Static("DisableUserLocation", "()V", Noop())
      .Static("IsMusicActive", "()Z", RetBool(false))
      .Static("IsVideoCompleted", "()I", RetInt(1))
      .Static("GetBrowserBookmarks", "(Ljava/lang/String;II)Ljava/lang/String;", RetConstStr(""))
      .Static("GetBrowserHistory", "(Ljava/lang/String;II)Ljava/lang/String;", RetConstStr(""));

  // Connectivity / battery: offline desktop on AC power.
  d.Static("initCheckConnectionType", "()I", RetInt(0))  // 0 = no connectivity
      .Static("initBatteryInfo", "()V", [](Call&) {
        // The Java calls JNIBridge.SetBatteryInfo(charging, usb, ac, level) with the initial state.
        CallNative(kJniBridge + ".SetBatteryInfo", false, false, true, 100);
        return Value();
      });

  // Anti-tamper inputs: the values of an untampered install.
  d.Static("retrieveBarrels", "()[I", [](Call&) { return Value::Obj(MakeIntArray({0, 0, 0, 0})); })  // "unavailable" = clean in IsAppEnc
      .Static("GenerateUUID", S, RetStr([] { return RandomUuid(); }))
      .Static("GetPlainText", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", [](Call& c) {
        std::string out;
        return DefReaderDecode(c.S(0), c.S(1), out) ? Value::Str(out) : Value::Null();
      })
      .Static("getGLUID", "(Ljava/lang/String;)[I", [](Call& c) { return Value::Obj(GlUid(c.S(0))); })
      .Static("ShowOfflineZoroDialog", "()V", [](Call&) {
        log::Warn("[jni] ShowOfflineZoroDialog called (anti-tamper check failed?)");
        return Value();
      });
}

// ---------------------------------------------------------------------------------------------
// Ads library device queries (com.gameloft.glads.AndroidDevice)
// ---------------------------------------------------------------------------------------------
void DefineAndroidDevice() {
  const char* S = "()Ljava/lang/String;";
  DefineClass("com/gameloft/glads/AndroidDevice")
      .NoopRest()
      .Static("GetConnectionType", S, RetConstStr("none"))
      .Static("HasInternetConnection", "()Z", RetBool(false))
      .Static("GetCachePath", S, RetStr([] { return config().save_path; }))
      .Static("GetDIPScaleFactor", "()F", [](Call&) { return Value::Float(160.0 / config().dpi); })
      .Static("GetLogicalDensity", "()F", [](Call&) { return Value::Float(config().dpi / 160.0); })
      .Static("GetScreenXDPI", "()F", [](Call&) { return Value::Float(config().dpi); })
      .Static("GetScreenYDPI", "()F", [](Call&) { return Value::Float(config().dpi); })
      .Static("GetOrientation", "()I", RetInt(2))  // landscape
      .Static("GetSystemVersion", S, RetStr([] { return config().release; }))
      .Static("GetUserAgent", S, RetStr([] { return "Dalvik/2.1.0 (Linux; U; Android " + config().release + "; " + config().model + ")"; }))
      .Static("GetTotalMemory", "()F", RetFloat(12288.0))
      .Static("GetUsedMemory", "()F", RetFloat(3072.0))
      .Static("GetSoundVolume", "()I", RetInt(50))
      .Static("IsDeviceRooted", "()Z", RetBool(false))
      .Static("GetGoogleAdID", S, RetConstStr(""))
      .Static("GetGoogleOptOut", S, RetConstStr("2"))
      .Static("CreateDirectory", "(Ljava/lang/String;)V", [](Call& c) {
        const auto hp = HostPath(c.S(0));
        std::error_code ec;
        if (!hp.empty()) std::filesystem::create_directories(hp, ec);
        return Value();
      })
      .Static("DeleteDirectory", "(Ljava/lang/String;)V", [](Call& c) {
        const auto hp = HostPath(c.S(0));
        std::error_code ec;
        if (!hp.empty()) std::filesystem::remove_all(hp, ec);
        return Value();
      })
      .Static("RemoveFile", "(Ljava/lang/String;)V", [](Call& c) {
        const auto hp = HostPath(c.S(0));
        std::error_code ec;
        if (!hp.empty()) std::filesystem::remove(hp, ec);
        return Value();
      })
      .Static("GetChildDirectories", "(Ljava/lang/String;)[Ljava/lang/String;", [](Call& c) {
        std::vector<std::string> names;
        const auto hp = HostPath(c.S(0));
        std::error_code ec;
        if (!hp.empty())
          for (const auto& e : std::filesystem::directory_iterator(hp, ec))
            if (e.is_directory(ec)) names.push_back(e.path().filename().string());
        return Value::Obj(MakeStringArray(names));
      });
}

}  // namespace

jref SurfaceObject() { return g_surface; }
jref AssetManagerObject() { return g_asset_manager; }
bool IsSurface(jref r) { return r && Resolve(r) == Resolve(g_surface); }
bool IsAssetManager(jref r) { return r && Resolve(r) == Resolve(g_asset_manager); }

void RegisterGameClasses() {
  // Process-wide fake framework objects.
  g_surface = NewGlobal(std::make_shared<Object>(GetClass("android/view/Surface")));
  g_asset_manager = NewGlobal(std::make_shared<Object>(GetClass("android/content/res/AssetManager")));

  // ---- Classes absent from this APK: FindClass fails with NoClassDefFoundError (jni.md rows marked NO, HID) ----
  for (const char* n : {"com/gameloft/adsmanager/InterstitialFANListener", "com/gameloft/adsmanager/BannerFANListener",
                        "com/gameloft/adsmanager/BannerAdMobListener", "com/gameloft/adsmanager/InterstitialAdMobListener",
                        "com/gameloft/adsmanager/NativeFANListener", "com/gameloft/adsmanager/NativeAdMobListener",
                        "com/gameloft/adsmanager/IncentivizedFAN",
                        "com/gameloft/android/ANMP/GloftNOHM/GLUtils/controller/NativeBridgeHIDControllers"})
    GetClass(n)->missing = true;

  // ---- AndroidUtils: REQUIRED (paths, device identity, preferences, assets, dialogs, exit) ----
  {
    ClassDef d = DefineClass(P("PackageUtils/AndroidUtils"));
    d.NoopRest();
    DefineUtilMembers(d);
  }
  {
    ClassDef d = DefineClass(P("GLUtils/SUtils"));
    d.NoopRest();
    DefineUtilMembers(d);
  }
  DefineClass(P("GLUtils/Device")).NoopRest();
  DefineClass(P("PackageUtils/EmulatorDetector")).NoopRest();  // IsGameRunOnEmulator() = false
  DefineClass(kJniBridge);                                       // only natives live here

  // ---- Loading UI: progress callbacks are ignored, no cache warning is ever shown ----
  DefineClass(P("PackageUtils/LogoViewPlugin")).NoopRest();

  // ---- DataSharing: cross-game key/value store, kept in the host prefs file ----
  DefineClass(P("DataSharing"))
      .Static("setSharedValue", "(Ljava/lang/String;Ljava/lang/String;)V", [](Call& c) {
        PrefsSet("datasharing", c.S(0), c.S(1));
        return Value();
      })
      .Static("getSharedValue", "(Ljava/lang/String;)Ljava/lang/String;", [](Call& c) {
        bool f = false;
        const std::string v = PrefsGet("datasharing", c.S(0), &f);
        return Value::Str(f ? v : "");
      })
      .Static("isSharedValue", "(Ljava/lang/String;)Z", [](Call& c) {
        bool f = false;
        PrefsGet("datasharing", c.S(0), &f);
        return Value::Bool(f);
      })
      .Static("deleteSharedValue", "(Ljava/lang/String;)V", [](Call& c) {
        PrefsRemove("datasharing", c.S(0));
        return Value();
      });

  // ---- Permissions: everything is granted; requests answer "already granted" (1) ----
  DefineClass(P("PackageUtils/PermissionPlugin")).Fallback([](Call& c) {
    if (c.method->ret == 'Z') return Value::Bool(true);        // is*PermissionEnabled
    if (c.method->ret == 'I') return Value::Int(1);            // request*Permission: already granted
    return DefaultFor(*c.method);
  });

  // ---- Installed apps, display cutout: nothing installed, no cutout (defaults) ----
  DefineClass(P("AppDetection")).NoopRest();
  DefineClass(P("GLUtils/CutoutHelper")).NoopRest();

  // ---- Clipboard: the Windows clipboard ----
  DefineClass(P("PackageUtils/ClipboardPlugin"))
      .Static("CopyToClipboard", "(Ljava/lang/String;)V", [](Call& c) {
        ClipboardSet(c.S(0));
        return Value();
      })
      .Static("PasteFromClipboard", "()Ljava/lang/String;", [](Call&) { return Value::Str(ClipboardGet()); });

  // ---- Push notifications, Firebase, invites: stubs ----
  DefineClass(P("PushNotification/SimplifiedAndroidUtils")).NoopRest();
  DefineClass(P("GoogleFirebase/GoogleFirebase")).NoopRest();
  DefineClass(P("PackageUtils/GoogleInvitePlugin")).NoopRest();
  DefineClass(P("PackageUtils/AdServerPlugin")).NoopRest();
  DefineClass(P("DRM/Gloft/GloftDrmImpl")).NoopRest();

  // ---- Ads: nothing is available, nothing is ever shown ----
  for (const char* n : {"com/gameloft/adsmanager/AdMob", "com/gameloft/adsmanager/FAN", "com/gameloft/adsmanager/InterstitialFAN",
                        "com/gameloft/adsmanager/AdsManager", "com/gameloft/adsmanager/BannerFAN", "com/gameloft/adsmanager/BannerAdMob",
                        "com/gameloft/adsmanager/InterstitialAdMob", "com/gameloft/adsmanager/Constants",
                        "com/gameloft/adsmanager/UnityAdsManager", "com/gameloft/adsmanager/NativeFAN",
                        "com/gameloft/adsmanager/NativeAdMob", "com/gameloft/adsmanager/IronSourceAds",
                        "com/gameloft/adsmanager/InterstitialIronSource", "com/gameloft/adsmanager/IncentivizedIronSource",
                        "com/gameloft/adsmanager/BannerIronSource", "com/gameloft/glads/AndroidWebView",
                        "com/gameloft/glads/MRAIDHtmlProcessor", "com/gameloft/glads/MoatSDK", "com/gameloft/glads/AvidSDK",
                        "com/gameloft/glads/AndroidAds", "com/gameloft/glads/OmSDK", "com/gameloft/glads/Utils",
                        "com/gameloft/glads/AndroidMRAIDView", "com/gameloft/igp/IGPFreemiumActivity",
                        "com/gameloft/igp/InGamePromotionPlugin", "com/gameloft/ingamebrowser/InGameBrowser",
                        "com/gameloft/popupslib/PopUpsBridgeClass"})
    DefineClass(n).NoopRest();
  DefineClass("com/gameloft/adsmanager/JavaUtils")
      .NoopRest()
      .Static("DipToPx", "(I)I", [](Call& c) { return Value::Int(c.I(0)); });
  DefineAndroidDevice();

  // ---- In-app purchase (Gameloft IAP): built-in offline catalogue, every purchase is free and instant ----
  DefineIapLib();  // classes_iap.cpp: offline catalogue, free instant purchases (docs/iap.md)

  // ---- Social: "not logged in" everywhere ----
  DefineClass("com/gameloft/GLSocialLib/PlatformAndroid").NoopRest();
  DefineClass("com/gameloft/GLSocialLib/facebook/FacebookAndroidGLSocialLib").NoopRest();
  DefineClass("com/gameloft/GLSocialLib/GameAPI/GameAPIAndroidGLSocialLib").NoopRest();
}

bool KeyboardVisible() {
  std::lock_guard lk(g_kb.mu);
  return g_kb.visible;
}
bool KeyboardNumeric() {
  std::lock_guard lk(g_kb.mu);
  return g_kb.type == 3;
}
std::string KeyboardText() {
  std::lock_guard lk(g_kb.mu);
  return g_kb.text;
}
// VirtualKeyboard.onTextChanged: numeric fields keep the leading digits ("-" when it starts with 0) and send them;
// other fields send "" (the game reads them with GetVKeyboardText).
void KeyboardSetText(const std::string& utf8) {
  std::string send;
  {
    std::lock_guard lk(g_kb.mu);
    if (!g_kb.visible) return;
    std::string t = utf8;
    if (g_kb.max_len > 0 && t.size() > static_cast<std::size_t>(g_kb.max_len)) t.resize(static_cast<std::size_t>(g_kb.max_len));
    if (g_kb.type == 3) {
      std::string digits;
      for (char ch : t) {
        if (ch < '0' || ch > '9') break;
        digits += ch;
      }
      t = (!digits.empty() && digits[0] == '0') ? std::string("-") : digits;
      send = t;
    }
    g_kb.text = t;
  }
  LocalFrame frame;
  CallNative(kJniBridge + ".NativeSendKeyboardData", NewString(send));
}
void KeyboardHide() {
  std::lock_guard lk(g_kb.mu);
  g_kb.visible = false;
  g_kb.text.clear();
}

}  // namespace nova::jni
