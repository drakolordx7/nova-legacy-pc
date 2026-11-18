// JNIEnv (JNINativeInterface, the 233 slots of JNI 1.6) and JavaVM (JNIInvokeInterface) in guest memory. Every
// slot is an 8-byte pointer to an HLE stub "JNIEnv::<Name>"; the handlers marshal AAPCS64 arguments (Call*Method
// varargs, the *V va_list struct, the *A jvalue array).
// Adapted from Zombiewood's jni/env (MIT, (c) 2026 ChrisDutary) for A64.
#include <cstring>

#include "core/log.h"
#include "cpu/hle.h"
#include "cpu/hle_abi.h"
#include "hle/heap.h"
#include "jni/internal.h"

namespace nova::jni {
namespace {

gaddr g_env_table = 0;
gaddr g_vm = 0;

Class* ClassFromRef(jref r) { return dynamic_cast<Class*>(Resolve(r).get()); }

template <class T>
std::shared_ptr<T> As(jref r) {
  return std::dynamic_pointer_cast<T>(Resolve(r));
}

std::string SafeCStr(gaddr a) { return a && GuestMemory::IsMapped(a) ? mem::CString(a, 4096) : std::string(); }

// ---------------------------------------------------------------------------------------------
// Argument readers for Call*Method
// ---------------------------------------------------------------------------------------------
Value ReadOne(char t, VaList& va) {
  switch (t) {
    case 'Z': return Value::Bool((va.NextInt() & 0xFF) != 0);
    case 'B': return Value::Int(static_cast<std::int8_t>(va.NextInt()));
    case 'C': return Value::Int(static_cast<std::uint16_t>(va.NextInt()));
    case 'S': return Value::Int(static_cast<std::int16_t>(va.NextInt()));
    case 'I': return Value::Int(static_cast<std::int32_t>(va.NextInt()));
    case 'J': return Value::Int(static_cast<std::int64_t>(va.NextInt()));
    case 'F':  // float is promoted to double in "..." and in va_list
    case 'D': return Value::Float(std::bit_cast<double>(va.NextFp()));
    default: return Value::Obj(Resolve(va.NextInt()));
  }
}

std::vector<Value> ReadArgs(const Method* m, VaList& va) {
  std::vector<Value> v;
  if (!m) return v;
  v.reserve(m->params.size());
  for (char t : m->params) v.push_back(ReadOne(t, va));
  return v;
}

std::vector<Value> ReadArgsA(const Method* m, gaddr jv) {
  std::vector<Value> v;
  if (!m || !jv) return v;
  v.reserve(m->params.size());
  for (std::size_t i = 0; i < m->params.size(); ++i) {
    const gaddr a = jv + 8 * static_cast<gaddr>(i);  // jvalue is an 8-byte union
    switch (m->params[i]) {
      case 'Z': v.push_back(Value::Bool(mem::R<std::uint8_t>(a) != 0)); break;
      case 'B': v.push_back(Value::Int(mem::R<std::int8_t>(a))); break;
      case 'C': v.push_back(Value::Int(mem::R<std::uint16_t>(a))); break;
      case 'S': v.push_back(Value::Int(mem::R<std::int16_t>(a))); break;
      case 'I': v.push_back(Value::Int(mem::R<std::int32_t>(a))); break;
      case 'J': v.push_back(Value::Int(mem::R<std::int64_t>(a))); break;
      case 'F': v.push_back(Value::Float(mem::R<float>(a))); break;
      case 'D': v.push_back(Value::Float(mem::R<double>(a))); break;
      default: v.push_back(Value::Obj(Resolve(mem::R64(a)))); break;
    }
  }
  return v;
}

// Java value -> value returned by a slot (R is the C++ type of the slot's jXXX result).
template <class R>
R Ret(const Value& v) {
  if constexpr (std::is_void_v<R>) {
    return;
  } else if constexpr (std::is_same_v<R, jref>) {
    return NewLocal(v.o);
  } else if constexpr (std::is_same_v<R, float>) {
    return static_cast<float>(v.AsFloat());
  } else if constexpr (std::is_same_v<R, double>) {
    return v.AsFloat();
  } else if constexpr (std::is_same_v<R, std::uint8_t>) {
    return static_cast<std::uint8_t>(v.AsInt() != 0 ? 1 : 0);
  } else {
    return static_cast<R>(v.AsInt());
  }
}

template <class R>
Value ToValue(R v) {
  if constexpr (std::is_same_v<R, jref>) return Value::Obj(Resolve(v));
  else if constexpr (std::is_same_v<R, float> || std::is_same_v<R, double>) return Value::Float(v);
  else return Value::Int(static_cast<std::int64_t>(v));
}

enum CallKind : int { kVirtual = 0, kStatic = 1 };

Method* CheckedMethod(std::uint64_t mid, const char* what) {
  Method* m = MethodById(mid);
  if (!m && mid) log::Warn("[jni] {}: invalid jmethodID 0x{:x}", what, mid);
  return m;
}

template <class R>
R Finish(Method* m, ObjPtr self, Class* cls, std::vector<Value>&& args, bool virt) {
  if (!m) {
    if constexpr (std::is_void_v<R>) return;
    else return R{};
  }
  Value v = Invoke(m, std::move(self), cls, std::move(args), virt);
  return Ret<R>(v);
}

// Call<Type>Method / CallStatic<Type>Method: "..." / va_list / jvalue[] flavours.
template <class R, int K>
R CallVa(gaddr, jref o, std::uint64_t mid, Varargs va) {
  Method* m = CheckedMethod(mid, "Call*Method");
  auto args = ReadArgs(m, va);
  if constexpr (K == kStatic) return Finish<R>(m, nullptr, ClassFromRef(o), std::move(args), false);
  else return Finish<R>(m, Resolve(o), nullptr, std::move(args), true);
}
template <class R, int K>
R CallV(gaddr, jref o, std::uint64_t mid, VaList ap) {
  Method* m = CheckedMethod(mid, "Call*MethodV");
  auto args = ReadArgs(m, ap);
  if constexpr (K == kStatic) return Finish<R>(m, nullptr, ClassFromRef(o), std::move(args), false);
  else return Finish<R>(m, Resolve(o), nullptr, std::move(args), true);
}
template <class R, int K>
R CallA(gaddr, jref o, std::uint64_t mid, gaddr jv) {
  Method* m = CheckedMethod(mid, "Call*MethodA");
  auto args = ReadArgsA(m, jv);
  if constexpr (K == kStatic) return Finish<R>(m, nullptr, ClassFromRef(o), std::move(args), false);
  else return Finish<R>(m, Resolve(o), nullptr, std::move(args), true);
}
template <class R>
R CallNvVa(gaddr, jref o, jref cls, std::uint64_t mid, Varargs va) {
  Method* m = CheckedMethod(mid, "CallNonvirtual*Method");
  auto args = ReadArgs(m, va);
  return Finish<R>(m, Resolve(o), ClassFromRef(cls), std::move(args), false);
}
template <class R>
R CallNvV(gaddr, jref o, jref cls, std::uint64_t mid, VaList ap) {
  Method* m = CheckedMethod(mid, "CallNonvirtual*MethodV");
  auto args = ReadArgs(m, ap);
  return Finish<R>(m, Resolve(o), ClassFromRef(cls), std::move(args), false);
}
template <class R>
R CallNvA(gaddr, jref o, jref cls, std::uint64_t mid, gaddr jv) {
  Method* m = CheckedMethod(mid, "CallNonvirtual*MethodA");
  auto args = ReadArgsA(m, jv);
  return Finish<R>(m, Resolve(o), ClassFromRef(cls), std::move(args), false);
}

// ---- fields ----
template <class R>
R GetFieldT(gaddr, jref obj, std::uint64_t fid) {
  ObjPtr o = Resolve(obj);
  Field* f = FieldById(fid);
  if (!o || !f) return R{};
  std::shared_ptr<Value> v;
  {
    std::lock_guard lk(o->fields_mu);
    if (auto it = o->fields.find(f); it != o->fields.end()) v = it->second;
  }
  return v ? Ret<R>(*v) : R{};
}
template <class R>
void SetFieldT(gaddr, jref obj, std::uint64_t fid, R val) {
  ObjPtr o = Resolve(obj);
  Field* f = FieldById(fid);
  if (!o || !f) return;
  auto v = std::make_shared<Value>(ToValue<R>(val));
  std::lock_guard lk(o->fields_mu);
  o->fields[f] = std::move(v);
}
template <class R>
R GetStaticFieldT(gaddr, jref, std::uint64_t fid) {
  Field* f = FieldById(fid);
  if (!f) return R{};
  Value v;
  {
    std::lock_guard lk(f->mu);
    v = f->static_value;
  }
  return Ret<R>(v);
}
template <class R>
void SetStaticFieldT(gaddr, jref, std::uint64_t fid, R val) {
  Field* f = FieldById(fid);
  if (!f) return;
  std::lock_guard lk(f->mu);
  f->static_value = ToValue<R>(val);
}

// ---- primitive arrays ----
template <char E>
jref NewPrimArray(gaddr, std::int32_t n) {
  if (n < 0) return 0;
  return NewLocal(MakeArray(E, static_cast<std::uint32_t>(n)));
}
std::shared_ptr<Array> PrimArray(jref r) {
  auto a = As<Array>(r);
  return a && a->elem != 'L' ? a : nullptr;
}
// Direct guest pointer into the array: no copy, so is_copy is always JNI_FALSE and Release* has nothing to do.
gaddr GetArrayElems(gaddr, jref arr, std::uint8_t* is_copy) {
  auto a = PrimArray(arr);
  if (is_copy) *is_copy = 0;
  return a ? a->data : 0;
}
void ReleaseArrayElems(gaddr, jref, gaddr, std::int32_t) {}
bool RangeOk(const Array& a, std::int32_t start, std::int32_t len) {
  return start >= 0 && len >= 0 && static_cast<std::uint64_t>(start) + static_cast<std::uint64_t>(len) <= a.length;
}
void GetArrayRegion(gaddr, jref arr, std::int32_t start, std::int32_t len, gaddr buf) {
  auto a = PrimArray(arr);
  if (!a || !RangeOk(*a, start, len)) {
    ThrowJava("java/lang/ArrayIndexOutOfBoundsException", "Get<T>ArrayRegion out of range");
    return;
  }
  if (len) std::memcpy(g2h(buf), a->host() + std::size_t(start) * a->elem_size, std::size_t(len) * a->elem_size);
}
void SetArrayRegion(gaddr, jref arr, std::int32_t start, std::int32_t len, gaddr buf) {
  auto a = PrimArray(arr);
  if (!a || !RangeOk(*a, start, len)) {
    ThrowJava("java/lang/ArrayIndexOutOfBoundsException", "Set<T>ArrayRegion out of range");
    return;
  }
  if (len) std::memcpy(a->host() + std::size_t(start) * a->elem_size, g2h(buf), std::size_t(len) * a->elem_size);
}

// ---------------------------------------------------------------------------------------------
// Individual slots
// ---------------------------------------------------------------------------------------------
std::int32_t jGetVersion(gaddr) { return 0x10006; }
jref jDefineClass(gaddr, gaddr name, jref, gaddr, std::int32_t) {
  log::Warn("[jni] DefineClass({}) is not supported", SafeCStr(name));
  return 0;
}
jref jFindClass(gaddr, gaddr pname) {
  const std::string name = SafeCStr(pname);
  if (name.empty()) return 0;
  Class* c = GetClass(name);
  NoteCoverage(CoverageEntry::Kind::Class, c->name, "", "", c->emulated);
  if (c->missing) {
    ThrowJava("java/lang/NoClassDefFoundError", "Didn't find class \"" + name + "\"");
    return 0;
  }
  return NewLocal(ClassObj(c));
}
std::uint64_t jFromReflected(gaddr, jref) { return 0; }
jref jToReflected(gaddr, jref, std::uint64_t, std::uint8_t) { return 0; }
jref jGetSuperclass(gaddr, jref c) {
  Class* k = ClassFromRef(c);
  return k && k->super ? NewLocal(ClassObj(k->super)) : 0;
}
std::uint8_t jIsAssignableFrom(gaddr, jref a, jref b) {
  Class *ka = ClassFromRef(a), *kb = ClassFromRef(b);
  return ka && kb && ka->IsSubclassOf(kb);
}
std::int32_t jThrow(gaddr, jref t) {
  SetPending(Resolve(t));
  return 0;
}
std::int32_t jThrowNew(gaddr, jref c, gaddr msg) {
  Class* k = ClassFromRef(c);
  SetPending(std::make_shared<Throwable>(k ? k : GetClass("java/lang/Throwable"), SafeCStr(msg)));
  return 0;
}
jref jExceptionOccurred(gaddr) {
  ObjPtr p = TS().pending;
  return p ? NewLocal(p) : 0;
}
void jExceptionDescribe(gaddr) {
  ObjPtr p = TS().pending;
  if (!p) return;
  auto t = std::dynamic_pointer_cast<Throwable>(p);
  log::Warn("[jni] pending exception: {}: {}", p->cls ? p->cls->name : "?", t ? t->message : "");
}
void jExceptionClear(gaddr) { TS().pending = nullptr; }
void jFatalError(gaddr, gaddr msg) {
  log::Error("[jni] FatalError: {}", SafeCStr(msg));
  throw GuestExit{134};
}
std::int32_t jPushLocalFrame(gaddr, std::int32_t) {
  PushFrame();
  return 0;
}
jref jPopLocalFrame(gaddr, jref result) {
  ObjPtr keep = Resolve(result);
  PopFrame();
  return NewLocal(keep);
}
jref jNewGlobalRef(gaddr, jref r) { return NewGlobal(Resolve(r)); }
void jDeleteGlobalRef(gaddr, jref r) {
  const auto kind = r & 3;
  if (kind == static_cast<std::uint64_t>(RefKind::Global) || kind == static_cast<std::uint64_t>(RefKind::Weak)) RemoveRef(r);
}
void jDeleteLocalRef(gaddr, jref r) { DeleteLocal(r); }
std::uint8_t jIsSameObject(gaddr, jref a, jref b) { return Resolve(a) == Resolve(b); }
jref jNewLocalRef(gaddr, jref r) { return NewLocal(Resolve(r)); }
std::int32_t jEnsureLocalCapacity(gaddr, std::int32_t) { return 0; }
jref jAllocObject(gaddr, jref c) { return NewLocal(AllocInstance(ClassFromRef(c))); }

jref NewObjectImpl(jref c, Method* m, std::vector<Value>&& args) {
  Class* k = ClassFromRef(c);
  if (!k) return 0;
  ObjPtr o = AllocInstance(k);
  if (m) Invoke(m, o, k, std::move(args), false);
  return NewLocal(o);
}
jref jNewObject(gaddr, jref c, std::uint64_t mid, Varargs va) {
  Method* m = CheckedMethod(mid, "NewObject");
  return NewObjectImpl(c, m, ReadArgs(m, va));
}
jref jNewObjectV(gaddr, jref c, std::uint64_t mid, VaList ap) {
  Method* m = CheckedMethod(mid, "NewObjectV");
  return NewObjectImpl(c, m, ReadArgs(m, ap));
}
jref jNewObjectA(gaddr, jref c, std::uint64_t mid, gaddr jv) {
  Method* m = CheckedMethod(mid, "NewObjectA");
  return NewObjectImpl(c, m, ReadArgsA(m, jv));
}
jref jGetObjectClass(gaddr, jref o) {
  ObjPtr p = Resolve(o);
  return p && p->cls ? NewLocal(ClassObj(p->cls)) : 0;
}
std::uint8_t jIsInstanceOf(gaddr, jref o, jref c) {
  ObjPtr p = Resolve(o);
  Class* k = ClassFromRef(c);
  if (!p) return 1;
  return k && p->cls && p->cls->IsSubclassOf(k);
}

// Methods the real classes do not have (the library looks them up and tolerates the failure).
bool GloballyMissing(const std::string& name, const std::string& sig) {
  static const std::pair<const char*, const char*> kMissing[] = {
      {"getPreference", "(Landroid/os/Bundle;)Landroid/os/Bundle;"},
      {"IsMemoryWarningActive", "()I"},
  };
  for (const auto& [n, s] : kMissing)
    if (name == n && sig == s) return true;
  return false;
}

std::uint64_t GetMethodIdImpl(jref c, gaddr pname, gaddr psig, bool is_static) {
  Class* k = ClassFromRef(c);
  const std::string name = SafeCStr(pname), sig = SafeCStr(psig);
  if (!k) {
    log::Warn("[jni] Get{}MethodID({} {}) with a null/invalid jclass (0x{:x})", is_static ? "Static" : "", name, sig, c);
    return 0;
  }
  bool covered = false;
  Method* m = GetOrAddMethod(k, name, sig, is_static, &covered);
  if (m->missing || (!m->impl && GloballyMissing(name, sig))) {
    NoteCoverage(is_static ? CoverageEntry::Kind::StaticMethod : CoverageEntry::Kind::Method, k->name, name, sig, true);
    ThrowJava("java/lang/NoSuchMethodError", name);
    return 0;
  }
  NoteCoverage(is_static ? CoverageEntry::Kind::StaticMethod : CoverageEntry::Kind::Method, k->name, name, sig, covered);
  return m->id;
}
std::uint64_t jGetMethodID(gaddr, jref c, gaddr n, gaddr s) { return GetMethodIdImpl(c, n, s, false); }
std::uint64_t jGetStaticMethodID(gaddr, jref c, gaddr n, gaddr s) { return GetMethodIdImpl(c, n, s, true); }

std::uint64_t GetFieldIdImpl(jref c, gaddr pname, gaddr psig, bool is_static) {
  Class* k = ClassFromRef(c);
  const std::string name = SafeCStr(pname), sig = SafeCStr(psig);
  if (!k) {
    log::Warn("[jni] Get{}FieldID({} {}) with a null/invalid jclass", is_static ? "Static" : "", name, sig);
    return 0;
  }
  bool covered = false;
  Field* f = GetOrAddField(k, name, sig, is_static, &covered);
  NoteCoverage(is_static ? CoverageEntry::Kind::StaticField : CoverageEntry::Kind::Field, k->name, name, sig, covered);
  return f->id;
}
std::uint64_t jGetFieldID(gaddr, jref c, gaddr n, gaddr s) { return GetFieldIdImpl(c, n, s, false); }
std::uint64_t jGetStaticFieldID(gaddr, jref c, gaddr n, gaddr s) { return GetFieldIdImpl(c, n, s, true); }

// ---- strings ----
jref jNewString(gaddr, gaddr chars, std::int32_t len) {
  if (len < 0 || (len && !chars)) return 0;
  return NewLocal(MakeStringU16(std::u16string(g2h<char16_t>(chars), static_cast<std::size_t>(len))));
}
std::int32_t jGetStringLength(gaddr, jref s) {
  auto o = As<String>(s);
  return o ? static_cast<std::int32_t>(o->s.size()) : 0;
}
gaddr jGetStringChars(gaddr, jref s, std::uint8_t* is_copy) {
  auto o = As<String>(s);
  if (!o) return 0;
  if (is_copy) *is_copy = 1;
  const gaddr p = hle::heap::Calloc(o->s.size() + 1, 2);
  if (p && !o->s.empty()) std::memcpy(g2h(p), o->s.data(), o->s.size() * 2);
  return p;
}
void jReleaseStringChars(gaddr, jref, gaddr chars) {
  if (chars) hle::heap::Free(chars);
}
jref jNewStringUTF(gaddr, gaddr p) {
  if (!p) return 0;
  return NewLocal(MakeString(mem::CString(p)));
}
std::int32_t jGetStringUTFLength(gaddr, jref s) {
  auto o = As<String>(s);
  return o ? static_cast<std::int32_t>(U16ToMutf8(o->s.data(), o->s.size()).size()) : 0;
}
gaddr jGetStringUTFChars(gaddr, jref s, std::uint8_t* is_copy) {
  auto o = As<String>(s);
  if (!o) return 0;
  if (is_copy) *is_copy = 1;
  const std::string m = U16ToMutf8(o->s.data(), o->s.size());  // modified UTF-8, as JNI specifies
  const gaddr p = hle::heap::Alloc(m.size() + 1);
  if (p) std::memcpy(g2h(p), m.c_str(), m.size() + 1);
  return p;
}
void jReleaseStringUTFChars(gaddr, jref, gaddr chars) {
  if (chars) hle::heap::Free(chars);
}
void jGetStringRegion(gaddr, jref s, std::int32_t start, std::int32_t len, gaddr buf) {
  auto o = As<String>(s);
  if (!o) return;
  if (start < 0 || len < 0 || static_cast<std::size_t>(start) + static_cast<std::size_t>(len) > o->s.size()) {
    ThrowJava("java/lang/StringIndexOutOfBoundsException", "GetStringRegion");
    return;
  }
  if (len) std::memcpy(g2h(buf), o->s.data() + start, static_cast<std::size_t>(len) * 2);
}
void jGetStringUTFRegion(gaddr, jref s, std::int32_t start, std::int32_t len, gaddr buf) {
  auto o = As<String>(s);
  if (!o) return;
  if (start < 0 || len < 0 || static_cast<std::size_t>(start) + static_cast<std::size_t>(len) > o->s.size()) {
    ThrowJava("java/lang/StringIndexOutOfBoundsException", "GetStringUTFRegion");
    return;
  }
  const std::string r = U16ToMutf8(o->s.data() + start, static_cast<std::size_t>(len));
  std::memcpy(g2h(buf), r.c_str(), r.size() + 1);
}

// ---- generic arrays ----
std::int32_t jGetArrayLength(gaddr, jref a) {
  auto o = As<Array>(a);
  return o ? static_cast<std::int32_t>(o->length) : 0;
}
jref jNewObjectArray(gaddr, std::int32_t n, jref elem_cls, jref init) {
  if (n < 0) return 0;
  Class* k = ClassFromRef(elem_cls);
  auto a = MakeObjectArray(k ? k->name : "java/lang/Object", static_cast<std::uint32_t>(n));
  if (ObjPtr i = Resolve(init))
    for (auto& e : a->objects) e = i;
  return NewLocal(a);
}
jref jGetObjectArrayElement(gaddr, jref a, std::int32_t i) {
  auto o = As<Array>(a);
  if (!o || o->elem != 'L' || i < 0 || static_cast<std::uint32_t>(i) >= o->length) {
    ThrowJava("java/lang/ArrayIndexOutOfBoundsException", "GetObjectArrayElement");
    return 0;
  }
  return NewLocal(o->objects[i]);
}
void jSetObjectArrayElement(gaddr, jref a, std::int32_t i, jref v) {
  auto o = As<Array>(a);
  if (!o || o->elem != 'L' || i < 0 || static_cast<std::uint32_t>(i) >= o->length) {
    ThrowJava("java/lang/ArrayIndexOutOfBoundsException", "SetObjectArrayElement");
    return;
  }
  o->objects[i] = Resolve(v);
}

// ---- misc ----
struct NativeMethodRec {  // JNINativeMethod on LP64: three 8-byte pointers
  std::uint64_t name, sig, fn;
};
std::int32_t jRegisterNatives(gaddr, jref c, gaddr methods, std::int32_t n) {
  Class* k = ClassFromRef(c);
  if (!k || n < 0) return -1;
  for (std::int32_t i = 0; i < n; ++i) {
    const auto m = mem::R<NativeMethodRec>(methods + sizeof(NativeMethodRec) * static_cast<gaddr>(i));
    const std::string name = SafeCStr(m.name), sig = SafeCStr(m.sig);
    RegisterNativeMethod(k, name, sig, m.fn);
    log::Info("[jni] RegisterNatives {}.{}{} -> 0x{:x}", k->name, name, sig, m.fn);
  }
  return 0;
}
std::int32_t jUnregisterNatives(gaddr, jref c) {
  if (Class* k = ClassFromRef(c)) UnregisterNatives(k);
  return 0;
}
std::int32_t jMonitor(gaddr, jref) { return 0; }
std::int32_t jGetJavaVM(gaddr, gaddr out) {
  if (out) mem::W64(out, g_vm);
  return 0;
}
gaddr jGetCritical(gaddr, jref arr, std::uint8_t* is_copy) { return GetArrayElems(0, arr, is_copy); }
void jReleaseCritical(gaddr, jref, gaddr, std::int32_t) {}
jref jNewWeakGlobalRef(gaddr, jref r) { return AddRef(Resolve(r), RefKind::Weak); }
std::uint8_t jExceptionCheck(gaddr) { return TS().pending ? 1 : 0; }

struct DirectBuffer : Object {
  gaddr addr;
  std::int64_t cap;
  DirectBuffer(Class* c, gaddr a, std::int64_t n) : Object(c), addr(a), cap(n) {}
};
jref jNewDirectByteBuffer(gaddr, gaddr addr, std::int64_t cap) {
  return NewLocal(std::make_shared<DirectBuffer>(GetClass("java/nio/DirectByteBuffer"), addr, cap));
}
gaddr jGetDirectBufferAddress(gaddr, jref b) {
  auto o = As<DirectBuffer>(b);
  return o ? o->addr : 0;
}
std::int64_t jGetDirectBufferCapacity(gaddr, jref b) {
  auto o = As<DirectBuffer>(b);
  return o ? o->cap : -1;
}
std::int32_t jGetObjectRefType(gaddr, jref r) { return Resolve(r) ? static_cast<std::int32_t>(r & 3) : 0; }

// ---- JavaVM ----
std::int32_t vmDestroy(gaddr) { return 0; }
std::int32_t vmAttach(gaddr, gaddr penv, gaddr) {
  ThreadState& ts = TS();
  ts.attached = true;
  if (penv) mem::W64(penv, ts.env);
  return 0;
}
std::int32_t vmDetach(gaddr) {
  ThreadState& ts = TS();
  ts.attached = false;
  ts.pending = nullptr;
  ClearBaseFrame();
  return 0;
}
// Like Android: JNI_EDETACHED for a thread that never attached (native threads), so ScopeGetEnv-style code attaches
// and detaches around its Java calls.
std::int32_t vmGetEnv(gaddr, gaddr penv, std::int32_t version) {
  if (version < 0x10001 || version > 0x10008) {
    log::Warn("[jni] JavaVM::GetEnv with unknown version 0x{:x}", static_cast<std::uint32_t>(version));
    if (penv) mem::W64(penv, 0);
    return -3;  // JNI_EVERSION
  }
  ThreadState& ts = TS();
  if (!ts.attached) {
    if (penv) mem::W64(penv, 0);
    return -2;  // JNI_EDETACHED
  }
  if (penv) mem::W64(penv, ts.env);
  return 0;
}

// ---------------------------------------------------------------------------------------------
// Table construction
// ---------------------------------------------------------------------------------------------
template <auto Fn>
void Slot(gaddr table, int slot, const std::string& name) {
  const std::string n = "JNIEnv::" + name;
  hle::Register(n, &hle::detail::Thunk<Fn>::Call, hle::kDefault);
  mem::W64(table + 8ull * slot, hle::StubFor(n));
}

template <class R, int T>
void TypeSlots(gaddr t, const std::string& s) {
  Slot<&CallVa<R, kVirtual>>(t, 34 + 3 * T, "Call" + s + "Method");
  Slot<&CallV<R, kVirtual>>(t, 35 + 3 * T, "Call" + s + "MethodV");
  Slot<&CallA<R, kVirtual>>(t, 36 + 3 * T, "Call" + s + "MethodA");
  Slot<&CallNvVa<R>>(t, 64 + 3 * T, "CallNonvirtual" + s + "Method");
  Slot<&CallNvV<R>>(t, 65 + 3 * T, "CallNonvirtual" + s + "MethodV");
  Slot<&CallNvA<R>>(t, 66 + 3 * T, "CallNonvirtual" + s + "MethodA");
  Slot<&CallVa<R, kStatic>>(t, 114 + 3 * T, "CallStatic" + s + "Method");
  Slot<&CallV<R, kStatic>>(t, 115 + 3 * T, "CallStatic" + s + "MethodV");
  Slot<&CallA<R, kStatic>>(t, 116 + 3 * T, "CallStatic" + s + "MethodA");
  if constexpr (!std::is_void_v<R>) {
    Slot<&GetFieldT<R>>(t, 95 + T, "Get" + s + "Field");
    Slot<&SetFieldT<R>>(t, 104 + T, "Set" + s + "Field");
    Slot<&GetStaticFieldT<R>>(t, 145 + T, "GetStatic" + s + "Field");
    Slot<&SetStaticFieldT<R>>(t, 154 + T, "SetStatic" + s + "Field");
  }
}

template <char E, int P>
void ArraySlots(gaddr t, const std::string& s) {
  Slot<&NewPrimArray<E>>(t, 175 + P, "New" + s + "Array");
  Slot<&GetArrayElems>(t, 183 + P, "Get" + s + "ArrayElements");
  Slot<&ReleaseArrayElems>(t, 191 + P, "Release" + s + "ArrayElements");
  Slot<&GetArrayRegion>(t, 199 + P, "Get" + s + "ArrayRegion");
  Slot<&SetArrayRegion>(t, 207 + P, "Set" + s + "ArrayRegion");
}

}  // namespace

gaddr EnvTable() { return g_env_table; }
gaddr JavaVMAddr() { return g_vm; }

gaddr EnvForThread() {
  ThreadState& ts = TS();
  ts.attached = true;  // a host thread calling in is a "Java thread"
  return ts.env;
}

gaddr BuildTables() {
  if (g_vm) return g_vm;
  const gaddr t = hle::heap::Calloc(kJniSlots, 8);  // slots 0..3 reserved = NULL
  Slot<&jGetVersion>(t, 4, "GetVersion");
  Slot<&jDefineClass>(t, 5, "DefineClass");
  Slot<&jFindClass>(t, 6, "FindClass");
  Slot<&jFromReflected>(t, 7, "FromReflectedMethod");
  Slot<&jFromReflected>(t, 8, "FromReflectedField");
  Slot<&jToReflected>(t, 9, "ToReflectedMethod");
  Slot<&jGetSuperclass>(t, 10, "GetSuperclass");
  Slot<&jIsAssignableFrom>(t, 11, "IsAssignableFrom");
  Slot<&jToReflected>(t, 12, "ToReflectedField");
  Slot<&jThrow>(t, 13, "Throw");
  Slot<&jThrowNew>(t, 14, "ThrowNew");
  Slot<&jExceptionOccurred>(t, 15, "ExceptionOccurred");
  Slot<&jExceptionDescribe>(t, 16, "ExceptionDescribe");
  Slot<&jExceptionClear>(t, 17, "ExceptionClear");
  Slot<&jFatalError>(t, 18, "FatalError");
  Slot<&jPushLocalFrame>(t, 19, "PushLocalFrame");
  Slot<&jPopLocalFrame>(t, 20, "PopLocalFrame");
  Slot<&jNewGlobalRef>(t, 21, "NewGlobalRef");
  Slot<&jDeleteGlobalRef>(t, 22, "DeleteGlobalRef");
  Slot<&jDeleteLocalRef>(t, 23, "DeleteLocalRef");
  Slot<&jIsSameObject>(t, 24, "IsSameObject");
  Slot<&jNewLocalRef>(t, 25, "NewLocalRef");
  Slot<&jEnsureLocalCapacity>(t, 26, "EnsureLocalCapacity");
  Slot<&jAllocObject>(t, 27, "AllocObject");
  Slot<&jNewObject>(t, 28, "NewObject");
  Slot<&jNewObjectV>(t, 29, "NewObjectV");
  Slot<&jNewObjectA>(t, 30, "NewObjectA");
  Slot<&jGetObjectClass>(t, 31, "GetObjectClass");
  Slot<&jIsInstanceOf>(t, 32, "IsInstanceOf");
  Slot<&jGetMethodID>(t, 33, "GetMethodID");
  TypeSlots<jref, 0>(t, "Object");
  TypeSlots<std::uint8_t, 1>(t, "Boolean");
  TypeSlots<std::int8_t, 2>(t, "Byte");
  TypeSlots<std::uint16_t, 3>(t, "Char");
  TypeSlots<std::int16_t, 4>(t, "Short");
  TypeSlots<std::int32_t, 5>(t, "Int");
  TypeSlots<std::int64_t, 6>(t, "Long");
  TypeSlots<float, 7>(t, "Float");
  TypeSlots<double, 8>(t, "Double");
  TypeSlots<void, 9>(t, "Void");
  Slot<&jGetFieldID>(t, 94, "GetFieldID");
  Slot<&jGetStaticMethodID>(t, 113, "GetStaticMethodID");
  Slot<&jGetStaticFieldID>(t, 144, "GetStaticFieldID");
  Slot<&jNewString>(t, 163, "NewString");
  Slot<&jGetStringLength>(t, 164, "GetStringLength");
  Slot<&jGetStringChars>(t, 165, "GetStringChars");
  Slot<&jReleaseStringChars>(t, 166, "ReleaseStringChars");
  Slot<&jNewStringUTF>(t, 167, "NewStringUTF");
  Slot<&jGetStringUTFLength>(t, 168, "GetStringUTFLength");
  Slot<&jGetStringUTFChars>(t, 169, "GetStringUTFChars");
  Slot<&jReleaseStringUTFChars>(t, 170, "ReleaseStringUTFChars");
  Slot<&jGetArrayLength>(t, 171, "GetArrayLength");
  Slot<&jNewObjectArray>(t, 172, "NewObjectArray");
  Slot<&jGetObjectArrayElement>(t, 173, "GetObjectArrayElement");
  Slot<&jSetObjectArrayElement>(t, 174, "SetObjectArrayElement");
  ArraySlots<'Z', 0>(t, "Boolean");
  ArraySlots<'B', 1>(t, "Byte");
  ArraySlots<'C', 2>(t, "Char");
  ArraySlots<'S', 3>(t, "Short");
  ArraySlots<'I', 4>(t, "Int");
  ArraySlots<'J', 5>(t, "Long");
  ArraySlots<'F', 6>(t, "Float");
  ArraySlots<'D', 7>(t, "Double");
  Slot<&jRegisterNatives>(t, 215, "RegisterNatives");
  Slot<&jUnregisterNatives>(t, 216, "UnregisterNatives");
  Slot<&jMonitor>(t, 217, "MonitorEnter");
  Slot<&jMonitor>(t, 218, "MonitorExit");
  Slot<&jGetJavaVM>(t, 219, "GetJavaVM");
  Slot<&jGetStringRegion>(t, 220, "GetStringRegion");
  Slot<&jGetStringUTFRegion>(t, 221, "GetStringUTFRegion");
  Slot<&jGetCritical>(t, 222, "GetPrimitiveArrayCritical");
  Slot<&jReleaseCritical>(t, 223, "ReleasePrimitiveArrayCritical");
  Slot<&jGetStringChars>(t, 224, "GetStringCritical");
  Slot<&jReleaseStringChars>(t, 225, "ReleaseStringCritical");
  Slot<&jNewWeakGlobalRef>(t, 226, "NewWeakGlobalRef");
  Slot<&jDeleteGlobalRef>(t, 227, "DeleteWeakGlobalRef");
  Slot<&jExceptionCheck>(t, 228, "ExceptionCheck");
  Slot<&jNewDirectByteBuffer>(t, 229, "NewDirectByteBuffer");
  Slot<&jGetDirectBufferAddress>(t, 230, "GetDirectBufferAddress");
  Slot<&jGetDirectBufferCapacity>(t, 231, "GetDirectBufferCapacity");
  Slot<&jGetObjectRefType>(t, 232, "GetObjectRefType");
  // No slot 4..232 may stay 0: an unexpected call lands in a named stub that logs once and returns 0.
  for (int i = 4; i < kJniSlots; ++i)
    if (!mem::R64(t + 8ull * i)) {
      const std::string n = "JNIEnv::slot_" + std::to_string(i);
      mem::W64(t + 8ull * i, hle::StubFor(n));
      log::Warn("[jni] slot {} has no handler", i);
    }
  g_env_table = t;

  // JavaVM: struct JNIInvokeInterface { void* reserved0..2; DestroyJavaVM; AttachCurrentThread; DetachCurrentThread;
  // GetEnv; AttachCurrentThreadAsDaemon; }
  const gaddr vt = hle::heap::Calloc(8, 8);
  mem::W64(vt + 8 * 3, NOVA_HLE_THUNK("JavaVM::DestroyJavaVM", vmDestroy));
  mem::W64(vt + 8 * 4, NOVA_HLE_THUNK("JavaVM::AttachCurrentThread", vmAttach));
  mem::W64(vt + 8 * 5, NOVA_HLE_THUNK("JavaVM::DetachCurrentThread", vmDetach));
  mem::W64(vt + 8 * 6, NOVA_HLE_THUNK("JavaVM::GetEnv", vmGetEnv));
  mem::W64(vt + 8 * 7, NOVA_HLE_THUNK("JavaVM::AttachCurrentThreadAsDaemon", vmAttach));
  g_vm = hle::heap::Calloc(1, 8);
  mem::W64(g_vm, vt);
  return g_vm;
}

}  // namespace nova::jni
