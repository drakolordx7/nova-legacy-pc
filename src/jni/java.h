// Object model of the emulated Java layer. Used by classes_*.cpp and by any module that wants to add or replace
// methods: jni::DefineClass("pkg/Class").Static("name", "(I)V", [](Call& c) { ...; return Value(); }).
// Unknown classes / methods are not an error: FindClass hands out a generic class, GetMethodID a method whose call
// returns the default for its descriptor (0 / false / "" for String / null otherwise) and logs once.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "jni/jni.h"

namespace nova::jni {

struct Class;
struct Method;
struct Field;
struct Value;

// A Java object. C++ subclasses (String, Array, Bundle...) carry their state.
struct Object : std::enable_shared_from_this<Object> {
  Class* cls = nullptr;
  explicit Object(Class* c) : cls(c) {}
  virtual ~Object() = default;
  std::mutex fields_mu;
  std::unordered_map<const Field*, std::shared_ptr<Value>> fields;  // instance fields
};
using ObjPtr = std::shared_ptr<Object>;

// A Java value (argument, result, field).
struct Value {
  enum class K : std::uint8_t { None, Int, Float, Obj } k = K::None;
  std::int64_t i = 0;
  double d = 0;
  ObjPtr o;

  static Value Int(std::int64_t v) { Value r; r.k = K::Int; r.i = v; return r; }
  static Value Bool(bool v) { return Int(v ? 1 : 0); }
  static Value Float(double v) { Value r; r.k = K::Float; r.d = v; return r; }
  static Value Obj(ObjPtr p) { Value r; r.k = K::Obj; r.o = std::move(p); return r; }
  static Value Null() { Value r; r.k = K::Obj; return r; }
  static Value Str(std::string_view utf8);  // a new java.lang.String
  std::int64_t AsInt() const { return k == K::Float ? static_cast<std::int64_t>(d) : i; }
  double AsFloat() const { return k == K::Float ? d : static_cast<double>(i); }
};

// java.lang.String: UTF-16 like Java; the host sees UTF-8.
struct String : Object {
  std::u16string s;
  String(Class* c, std::u16string v) : Object(c), s(std::move(v)) {}
  std::string utf8() const;
};

// Java array. Primitive arrays live in guest memory (Get<T>ArrayElements returns a direct pointer, no copy);
// object arrays in a host vector.
struct Array : Object {
  char elem;  // 'Z' 'B' 'C' 'S' 'I' 'J' 'F' 'D' or 'L'
  std::uint32_t length = 0, elem_size = 0;
  gaddr data = 0;
  std::vector<ObjPtr> objects;
  Array(Class* c, char e, std::uint32_t n);
  ~Array() override;
  std::uint8_t* host() const { return data ? g2h<std::uint8_t>(data) : nullptr; }
};

// android.os.Bundle: key -> value.
struct Bundle : Object {
  using Object::Object;
  std::mutex mu;
  std::map<std::string, Value> m;
  Value Get(const std::string& k) {
    std::lock_guard lk(mu);
    auto it = m.find(k);
    return it == m.end() ? Value() : it->second;
  }
  void Put(const std::string& k, Value v) {
    std::lock_guard lk(mu);
    m[k] = std::move(v);
  }
};

// java.lang.Throwable (and subclasses): only the message.
struct Throwable : Object {
  std::string message;
  Throwable(Class* c, std::string m) : Object(c), message(std::move(m)) {}
};

// State of one call into an emulated method.
struct Call {
  const Method* method = nullptr;
  ObjPtr self;  // null for static methods
  Class* cls = nullptr;
  std::vector<Value> args;
  std::int32_t I(std::size_t n) const { return n < args.size() ? static_cast<std::int32_t>(args[n].AsInt()) : 0; }
  std::int64_t J(std::size_t n) const { return n < args.size() ? args[n].AsInt() : 0; }
  bool Z(std::size_t n) const { return I(n) != 0; }
  double F(std::size_t n) const { return n < args.size() ? args[n].AsFloat() : 0.0; }
  ObjPtr O(std::size_t n) const { return n < args.size() ? args[n].o : nullptr; }
  std::string S(std::size_t n) const;  // String argument as UTF-8 ("" if null)
  bool IsNull(std::size_t n) const { return !O(n); }
  template <class T>
  std::shared_ptr<T> SelfAs() const { return std::dynamic_pointer_cast<T>(self); }
};
using Impl = std::function<Value(Call&)>;

struct Method {
  Class* cls = nullptr;
  std::string name, sig;
  bool is_static = false;
  bool missing = false;  // the real Java class has no such method: GetMethodID fails with NoSuchMethodError
  Impl impl;             // empty = not emulated (default result, logged once)
  std::vector<char> params;  // parameter kinds: 'Z' 'B' 'C' 'S' 'I' 'J' 'F' 'D' 'L' ('[' and 'L' both 'L')
  char ret = 'V';
  bool ret_string = false;   // returns java.lang.String (default result is "")
  bool ret_array = false;    // returns an array ('[')
  gaddr native = 0;          // RegisterNatives
  std::uint64_t id = 0;      // jmethodID
  std::atomic<std::uint64_t> calls{0};
  std::atomic<bool> warned{false};
};

struct Field {
  Class* cls = nullptr;
  std::string name, sig;
  bool is_static = false;
  std::uint64_t id = 0;
  bool defined = false;
  std::mutex mu;
  Value static_value;
};

struct Class : Object {
  std::string name;  // with slashes: "java/lang/String"
  Class* super = nullptr;
  bool emulated = false;
  bool missing = false;  // FindClass fails with ClassNotFoundException (class absent from the real APK)
  std::function<ObjPtr(Class*)> factory;
  Impl fallback;         // answers every method without an implementation (a stubbed-out class), if set
  std::map<std::string, std::unique_ptr<Method>> methods;  // key: name + sig
  std::map<std::string, std::unique_ptr<Field>> fields;    // key: name:sig
  Class(Class* meta, std::string n) : Object(meta), name(std::move(n)) {}
  bool IsSubclassOf(const Class* other) const;
};

// ---- Class definition DSL ----
class ClassDef {
 public:
  explicit ClassDef(Class* c) : c_(c) {}
  ClassDef& Static(std::string_view name, std::string_view sig, Impl impl);
  ClassDef& Virtual(std::string_view name, std::string_view sig, Impl impl);
  ClassDef& Ctor(std::string_view sig, Impl impl) { return Virtual("<init>", sig, std::move(impl)); }
  ClassDef& StaticNoop(std::initializer_list<std::pair<const char*, const char*>> name_sig);
  ClassDef& StaticField(std::string_view name, std::string_view sig, Value v);
  ClassDef& InstanceField(std::string_view name, std::string_view sig);
  // The real class does not have this method: Get[Static]MethodID returns 0 and throws NoSuchMethodError.
  ClassDef& Missing(std::string_view name, std::string_view sig);
  // Every method of the class without an implementation returns the default for its descriptor, silently.
  ClassDef& NoopRest();
  // Same, with a custom answer (it receives Call::method to tell methods apart).
  ClassDef& Fallback(Impl impl);
  ClassDef& Super(std::string_view super_name);
  ClassDef& Factory(std::function<ObjPtr(Class*)> f);
  Class* cls() const { return c_; }

 private:
  Class* c_;
};
// Creates (or returns) the class and marks it as emulated.
ClassDef DefineClass(std::string_view name);
// Looks the class up, creating an empty non-emulated one (super java/lang/Object) if needed.
Class* GetClass(std::string_view name);
// Marks every listed class emulated with no members (benign classes the library only looks up).
void DefineEmptyClasses(std::initializer_list<const char*> names);

// Default result for a method (by return kind / descriptor).
Value DefaultFor(const Method& m);

// ---- references <-> objects ----
ObjPtr Resolve(jref r);
jref NewLocal(const ObjPtr& o);
jref NewGlobal(const ObjPtr& o);
std::shared_ptr<String> MakeString(std::string_view utf8);
std::shared_ptr<String> MakeStringU16(std::u16string s);
std::shared_ptr<Array> MakeArray(char elem, std::uint32_t n);
std::shared_ptr<Array> MakeObjectArray(std::string_view elem_class, std::uint32_t n);
std::shared_ptr<Array> MakeStringArray(const std::vector<std::string>& v);
std::shared_ptr<Array> MakeByteArray(const void* data, std::uint32_t n);
std::shared_ptr<Array> MakeIntArray(std::initializer_list<std::int32_t> v);
std::shared_ptr<Object> NewBundle();
std::shared_ptr<Throwable> NewThrowable(std::string_view cls, std::string message);

// ---- exceptions (per thread) ----
void SetPending(ObjPtr ex);
void ThrowJava(std::string_view cls, std::string message);  // sets the calling thread's pending exception

// Runs an emulated method (virtual dispatch if `virt` and self is set).
Value Invoke(Method* m, ObjPtr self, Class* cls, std::vector<Value>&& args, bool virt);

// UTF-8 / UTF-16 / modified UTF-8 conversions.
std::u16string Utf8ToU16(std::string_view s);        // also accepts modified UTF-8
std::string U16ToUtf8(const char16_t* p, std::size_t n);
std::string U16ToMutf8(const char16_t* p, std::size_t n);

// Registration of the emulated classes (classes_*.cpp).
void RegisterAndroidClasses();
void RegisterGameClasses();

void NoteCoverage(CoverageEntry::Kind k, const std::string& cls, const std::string& name, const std::string& sig, bool covered);

// A native registered through RegisterNatives or exported as Java_*: see jni::FindNative.
gaddr FindRegisteredNative(std::string_view cls_dot_method);

}  // namespace nova::jni
