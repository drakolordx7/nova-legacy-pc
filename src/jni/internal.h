// Internal declarations of nova_jni (not for use outside src/jni).
#pragma once
#include <vector>

#include "jni/java.h"

namespace nova::jni {

enum class RefKind : std::uint64_t { Local = 1, Global = 2, Weak = 3 };

// jmethodID / jfieldID: opaque values in the unmapped low zone of the guest window (a guest dereference faults).
constexpr std::uint64_t kMethodIdBase = 0x0A000000ull;
constexpr std::uint64_t kFieldIdBase = 0x0B000000ull;
constexpr int kJniSlots = 233;  // JNINativeInterface of JNI 1.6: slots 0..232

struct ThreadState {
  gaddr env = 0;                          // guest JNIEnv* (a pointer to the function table), per thread
  bool attached = false;                  // GetEnv answers JNI_OK only for attached threads
  std::vector<std::vector<jref>> frames;  // [0] = the thread's base frame
  ObjPtr pending;                         // pending exception
};

// runtime.cpp
jref AddRef(const ObjPtr& o, RefKind kind);
bool RemoveRef(jref r);
std::size_t LiveRefs();
ThreadState& TS();
void DestroyThreadState(Cpu& cpu);
void DeleteLocal(jref r);
void PushFrame();
void PopFrame();
void ClearBaseFrame();
Class* FindLoadedClass(std::string_view name);
ObjPtr ClassObj(Class* c);
void InitCoreClasses();
Class* StringClass();
Method* LookupMethod(Class* c, std::string_view name, std::string_view sig);
Method* GetOrAddMethod(Class* c, std::string_view name, std::string_view sig, bool is_static, bool* covered);
Method* MethodById(std::uint64_t id);
Field* GetOrAddField(Class* c, std::string_view name, std::string_view sig, bool is_static, bool* covered);
Field* FieldById(std::uint64_t id);
ObjPtr AllocInstance(Class* c);
std::uint32_t ElemSize(char e);
void RegisterNativeMethod(Class* c, std::string_view name, std::string_view sig, gaddr fn);
void UnregisterNatives(Class* c);
void DeleteRef(jref r);

// classes_iap.cpp: com/gameloft/android2d/iap/IAPLib (offline catalogue, free purchases)
void DefineIapLib();

// env.cpp
gaddr EnvTable();
gaddr BuildTables();

// Prefs shared by classes_game.cpp (SharedPreferences / DataSharing / install id), persisted to Config::prefs_file.
std::string PrefsGet(const std::string& ns, const std::string& key, bool* found = nullptr);
void PrefsSet(const std::string& ns, const std::string& key, const std::string& value);
void PrefsRemove(const std::string& ns, const std::string& key);
std::string InstallId();  // stable UUID for this install (HDIDFV, Android id)

}  // namespace nova::jni
