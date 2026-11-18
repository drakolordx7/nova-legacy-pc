// Framework classes the native code looks up (docs/interface/jni.md "Class table preloaded at JNI_OnLoad" and
// "Framework lookups without a Java source"): java.lang.{Object,Class,String,Throwable}, android.os.{Bundle,Build,
// Process}. Everything else in the preload table is a benign empty class. Adapted from Zombiewood's
// classes_android (MIT, (c) 2026 ChrisDutary).
#include "core/log.h"
#include "jni/internal.h"

namespace nova::jni {
namespace {

std::shared_ptr<Bundle> SelfBundle(Call& c) { return c.SelfAs<Bundle>(); }

Value BundleGet(Call& c, bool null_default_string) {
  auto b = SelfBundle(c);
  Value v = b ? b->Get(c.S(0)) : Value();
  if (v.k == Value::K::None) return null_default_string ? Value::Null() : Value::Int(0);
  return v;
}
Value BundlePut(Call& c) {
  if (auto b = SelfBundle(c)) b->Put(c.S(0), c.args.size() > 1 ? c.args[1] : Value());
  return Value();
}

std::string DottedName(const Class* k) {
  std::string s = k ? k->name : std::string();
  for (char& ch : s)
    if (ch == '/') ch = '.';
  return s;
}

}  // namespace

std::shared_ptr<Object> NewBundle() { return std::make_shared<Bundle>(GetClass("android/os/Bundle")); }

void RegisterAndroidClasses() {
  const Config& cfg = config();

  DefineClass("java/lang/Object")
      .Ctor("()V", [](Call&) { return Value(); })
      .Virtual("hashCode", "()I", [](Call& c) { return Value::Int(static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(c.self.get()) >> 4) & 0x7FFFFFFF); })
      .Virtual("equals", "(Ljava/lang/Object;)Z", [](Call& c) { return Value::Bool(c.self == c.O(0)); })
      .Virtual("toString", "()Ljava/lang/String;", [](Call& c) { return Value::Str(c.self && c.self->cls ? DottedName(c.self->cls) : "null"); })
      .Virtual("getClass", "()Ljava/lang/Class;", [](Call& c) { return Value::Obj(c.self && c.self->cls ? ClassObj(c.self->cls) : nullptr); });

  DefineClass("java/lang/Class")
      .Virtual("getName", "()Ljava/lang/String;", [](Call& c) { return Value::Str(DottedName(dynamic_cast<Class*>(c.self.get()))); })
      .Virtual("getSimpleName", "()Ljava/lang/String;", [](Call& c) {
        std::string n = DottedName(dynamic_cast<Class*>(c.self.get()));
        const auto p = n.find_last_of(".$");
        return Value::Str(p == std::string::npos ? n : n.substr(p + 1));
      });

  DefineClass("java/lang/String")
      .Virtual("toString", "()Ljava/lang/String;", [](Call& c) { return Value::Obj(c.self); })
      .Virtual("length", "()I", [](Call& c) {
        auto s = c.SelfAs<String>();
        return Value::Int(s ? static_cast<std::int64_t>(s->s.size()) : 0);
      })
      .Virtual("equals", "(Ljava/lang/Object;)Z", [](Call& c) {
        auto a = c.SelfAs<String>();
        auto b = std::dynamic_pointer_cast<String>(c.O(0));
        return Value::Bool(a && b && a->s == b->s);
      });

  // Throwable and the exceptions the layer itself raises.
  DefineClass("java/lang/Throwable")
      .Virtual("getMessage", "()Ljava/lang/String;", [](Call& c) {
        auto t = c.SelfAs<Throwable>();
        return t ? Value::Str(t->message) : Value::Null();
      })
      .Virtual("toString", "()Ljava/lang/String;", [](Call& c) {
        auto t = c.SelfAs<Throwable>();
        return Value::Str(DottedName(c.self ? c.self->cls : nullptr) + (t && !t->message.empty() ? ": " + t->message : ""));
      });
  DefineClass("java/lang/Exception").Super("java/lang/Throwable");
  DefineClass("java/lang/Error").Super("java/lang/Throwable");
  DefineClass("java/lang/RuntimeException").Super("java/lang/Exception");
  DefineClass("java/lang/ClassNotFoundException").Super("java/lang/Exception");
  DefineClass("java/lang/LinkageError").Super("java/lang/Error");
  DefineClass("java/lang/NoClassDefFoundError").Super("java/lang/LinkageError");
  DefineClass("java/lang/IncompatibleClassChangeError").Super("java/lang/LinkageError");
  DefineClass("java/lang/NoSuchMethodError").Super("java/lang/IncompatibleClassChangeError");
  DefineClass("java/lang/NoSuchFieldError").Super("java/lang/IncompatibleClassChangeError");
  DefineClass("java/lang/IndexOutOfBoundsException").Super("java/lang/RuntimeException");
  DefineClass("java/lang/ArrayIndexOutOfBoundsException").Super("java/lang/IndexOutOfBoundsException");
  DefineClass("java/lang/StringIndexOutOfBoundsException").Super("java/lang/IndexOutOfBoundsException");
  for (const char* n : {"java/lang/Throwable", "java/lang/Exception", "java/lang/Error", "java/lang/RuntimeException",
                        "java/lang/ClassNotFoundException", "java/lang/LinkageError", "java/lang/NoClassDefFoundError",
                        "java/lang/IncompatibleClassChangeError", "java/lang/NoSuchMethodError", "java/lang/NoSuchFieldError",
                        "java/lang/IndexOutOfBoundsException", "java/lang/ArrayIndexOutOfBoundsException",
                        "java/lang/StringIndexOutOfBoundsException"})
    DefineClass(n).Factory([](Class* k) { return std::make_shared<Throwable>(k, std::string()); });

  // android.os.Bundle: a string-keyed map (the library passes shared-preference containers through it).
  DefineClass("android/os/Bundle")
      .Factory([](Class* k) { return std::make_shared<Bundle>(k); })
      .Ctor("()V", [](Call&) { return Value(); })
      .Virtual("clear", "()V", [](Call& c) {
        if (auto b = SelfBundle(c)) {
          std::lock_guard lk(b->mu);
          b->m.clear();
        }
        return Value();
      })
      .Virtual("containsKey", "(Ljava/lang/String;)Z", [](Call& c) {
        auto b = SelfBundle(c);
        return Value::Bool(b && b->Get(c.S(0)).k != Value::K::None);
      })
      .Virtual("getBoolean", "(Ljava/lang/String;)Z", [](Call& c) { return BundleGet(c, false); })
      .Virtual("getByteArray", "(Ljava/lang/String;)[B", [](Call& c) { return BundleGet(c, true); })
      .Virtual("getInt", "(Ljava/lang/String;)I", [](Call& c) { return BundleGet(c, false); })
      .Virtual("getLong", "(Ljava/lang/String;)J", [](Call& c) { return BundleGet(c, false); })
      .Virtual("getString", "(Ljava/lang/String;)Ljava/lang/String;", [](Call& c) { return BundleGet(c, true); })
      .Virtual("putBoolean", "(Ljava/lang/String;Z)V", BundlePut)
      .Virtual("putByteArray", "(Ljava/lang/String;[B)V", BundlePut)
      .Virtual("putInt", "(Ljava/lang/String;I)V", BundlePut)
      .Virtual("putLong", "(Ljava/lang/String;J)V", BundlePut)
      .Virtual("putString", "(Ljava/lang/String;Ljava/lang/String;)V", BundlePut);

  // android.os.Build (CDeviceSpecs_android reads Build.MODEL reflectively with GetStaticFieldID) and Build$VERSION.
  DefineClass("android/os/Build")
      .StaticField("MODEL", "Ljava/lang/String;", Value::Str(cfg.model))
      .StaticField("MANUFACTURER", "Ljava/lang/String;", Value::Str(cfg.manufacturer))
      .StaticField("BRAND", "Ljava/lang/String;", Value::Str(cfg.brand))
      .StaticField("DEVICE", "Ljava/lang/String;", Value::Str(cfg.device))
      .StaticField("PRODUCT", "Ljava/lang/String;", Value::Str(cfg.product))
      .StaticField("BOARD", "Ljava/lang/String;", Value::Str(cfg.board))
      .StaticField("HARDWARE", "Ljava/lang/String;", Value::Str(cfg.hardware))
      .StaticField("FINGERPRINT", "Ljava/lang/String;", Value::Str(cfg.fingerprint))
      .StaticField("TAGS", "Ljava/lang/String;", Value::Str("release-keys"))
      .StaticField("CPU_ABI", "Ljava/lang/String;", Value::Str("arm64-v8a"));
  DefineClass("android/os/Build$VERSION")
      .StaticField("SDK_INT", "I", Value::Int(cfg.sdk_int))
      .StaticField("RELEASE", "Ljava/lang/String;", Value::Str(cfg.release));

  // android.os.Process.setThreadPriority(int) in glf::Thread::Impl::SetPriority: nothing to do on the host.
  DefineClass("android/os/Process").Static("setThreadPriority", "(I)V", [](Call&) { return Value(); });

  // Classes JNI_OnLoad preloads (jni.md, rows 1-12) that no member is ever looked up on.
  DefineEmptyClasses({"android/content/Intent", "android/media/AudioTrack", "java/security/SecureRandom", "java/util/HashSet",
                      "java/lang/Long", "java/security/spec/X509EncodedKeySpec", "java/security/KeyFactory",
                      "java/security/NoSuchAlgorithmException", "java/security/spec/InvalidKeySpecException",
                      "android/view/Surface", "android/content/res/AssetManager", "java/nio/DirectByteBuffer"});
}

}  // namespace nova::jni
