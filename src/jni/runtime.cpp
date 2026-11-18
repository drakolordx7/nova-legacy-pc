// Core of the emulated Java layer: reference tables, classes / methods / fields, per-thread local frames and
// exceptions, strings and arrays, coverage, and the registry of natives (RegisterNatives + exported Java_* symbols).
// Adapted from Zombiewood's jni/runtime (MIT, (c) 2026 ChrisDutary).
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

#include "core/log.h"
#include "cpu/hle.h"
#include "hle/heap.h"
#include "jni/internal.h"
#include "loader/elf64.h"

namespace nova::jni {

// ---------------------------------------------------------------------------------------------
// Reference table
// ---------------------------------------------------------------------------------------------
namespace {
struct RefTable {
  std::mutex mu;
  std::vector<ObjPtr> slots{nullptr};  // index 0 reserved
  std::vector<std::uint32_t> free;
};
RefTable& Refs() {
  static RefTable t;
  return t;
}
}  // namespace

jref AddRef(const ObjPtr& o, RefKind kind) {
  if (!o) return 0;
  auto& t = Refs();
  std::lock_guard lk(t.mu);
  std::uint32_t idx;
  if (!t.free.empty()) {
    idx = t.free.back();
    t.free.pop_back();
    t.slots[idx] = o;
  } else {
    idx = static_cast<std::uint32_t>(t.slots.size());
    t.slots.push_back(o);
  }
  return (static_cast<jref>(idx) << 2) | static_cast<jref>(kind);
}

bool RemoveRef(jref r) {
  const std::uint64_t idx = r >> 2;
  if (!r || !(r & 3)) return false;
  ObjPtr dead;  // destroyed outside the lock (an Array destructor frees guest memory)
  {
    auto& t = Refs();
    std::lock_guard lk(t.mu);
    if (idx == 0 || idx >= t.slots.size() || !t.slots[idx]) return false;
    dead = std::move(t.slots[idx]);
    t.slots[idx] = nullptr;
    t.free.push_back(static_cast<std::uint32_t>(idx));
  }
  return true;
}

ObjPtr Resolve(jref r) {
  const std::uint64_t idx = r >> 2;
  if (!r || !(r & 3)) return nullptr;
  auto& t = Refs();
  std::lock_guard lk(t.mu);
  if (idx == 0 || idx >= t.slots.size()) return nullptr;
  return t.slots[idx];
}

std::size_t LiveRefs() {
  auto& t = Refs();
  std::lock_guard lk(t.mu);
  return t.slots.size() - 1 - t.free.size();
}

// ---------------------------------------------------------------------------------------------
// Per-thread state: JNIEnv, local frames, pending exception
// ---------------------------------------------------------------------------------------------
ThreadState& TS() {
  Cpu& cpu = Cpu::Current();
  void*& u = cpu.user(UserSlot::Jni);
  if (!u) {
    auto* ts = new ThreadState();
    ts->env = hle::heap::Calloc(2, 8);  // [0] = function table, [1] = thread id (a debugging aid)
    mem::W64(ts->env, EnvTable());
    mem::W64(ts->env + 8, cpu.thread_id());
    ts->frames.emplace_back();
    u = ts;
  }
  return *static_cast<ThreadState*>(u);
}

void DestroyThreadState(Cpu& cpu) {
  void*& u = cpu.user(UserSlot::Jni);
  if (!u) return;
  auto* ts = static_cast<ThreadState*>(u);
  for (auto& f : ts->frames)
    for (jref r : f) RemoveRef(r);
  if (ts->env) hle::heap::Free(ts->env);
  delete ts;
  u = nullptr;
}

jref NewLocal(const ObjPtr& o) {
  const jref r = AddRef(o, RefKind::Local);
  if (r) {
    auto& ts = TS();
    ts.frames.back().push_back(r);
    if (ts.frames.size() == 1 && ts.frames.back().size() == 100000)
      log::Warn("[jni] thread {} holds 100000 local references in its base frame (leak?)", Cpu::Current().thread_id());
  }
  return r;
}

jref NewGlobal(const ObjPtr& o) { return AddRef(o, RefKind::Global); }

void DeleteLocal(jref r) {
  if ((r & 3) != static_cast<std::uint64_t>(RefKind::Local)) return;
  auto& ts = TS();
  for (auto f = ts.frames.rbegin(); f != ts.frames.rend(); ++f) {
    auto it = std::find(f->rbegin(), f->rend(), r);
    if (it != f->rend()) {
      f->erase(std::next(it).base());
      break;
    }
  }
  RemoveRef(r);
}

void DeleteRef(jref r) {
  if ((r & 3) == static_cast<std::uint64_t>(RefKind::Local)) DeleteLocal(r);
  else RemoveRef(r);
}

void PushFrame() { TS().frames.emplace_back(); }

void PopFrame() {
  auto& ts = TS();
  if (ts.frames.size() <= 1) {
    log::Warn("[jni] PopLocalFrame without a matching PushLocalFrame");
    return;
  }
  std::vector<jref> f = std::move(ts.frames.back());
  ts.frames.pop_back();
  for (jref r : f) RemoveRef(r);
}

void ClearBaseFrame() {
  auto& ts = TS();
  for (jref r : ts.frames.front()) RemoveRef(r);
  ts.frames.front().clear();
}

LocalFrame::LocalFrame() { PushFrame(); }
LocalFrame::~LocalFrame() { PopFrame(); }

void SetPending(ObjPtr ex) { TS().pending = std::move(ex); }
void ThrowJava(std::string_view cls, std::string message) { SetPending(NewThrowable(cls, std::move(message))); }

// ---------------------------------------------------------------------------------------------
// Classes, methods, fields
// ---------------------------------------------------------------------------------------------
namespace {
struct Registry {
  std::recursive_mutex mu;
  std::map<std::string, std::shared_ptr<Class>, std::less<>> classes;
  std::vector<Method*> methods;  // by id
  std::vector<Field*> fields;
  std::map<std::string, gaddr, std::less<>> natives;  // "Java_..." mangled name and "pkg/Class.method" -> function
};
Registry& Reg() {
  static Registry r;
  return r;
}
Class* g_meta = nullptr;    // java/lang/Class
Class* g_object = nullptr;  // java/lang/Object
Class* g_string = nullptr;  // java/lang/String

std::string NormalizeClassName(std::string_view n) {
  std::string s(n);
  std::replace(s.begin(), s.end(), '.', '/');
  if (s.size() > 2 && s.front() == 'L' && s.back() == ';') s = s.substr(1, s.size() - 2);
  return s;
}

Class* GetClassLocked(Registry& r, std::string_view raw) {
  const std::string name = NormalizeClassName(raw);
  if (auto it = r.classes.find(name); it != r.classes.end()) return it->second.get();
  auto c = std::make_shared<Class>(g_meta, name);
  if (!g_meta) {  // bootstrap: java/lang/Class is its own metaclass
    c->cls = c.get();
    g_meta = c.get();
  }
  Class* raw_ptr = c.get();
  r.classes.emplace(name, std::move(c));
  if (name != "java/lang/Object") raw_ptr->super = g_object ? g_object : GetClassLocked(r, "java/lang/Object");
  return raw_ptr;
}

void ParseSig(std::string_view sig, std::vector<char>& params, char& ret, bool& ret_string, bool& ret_array) {
  params.clear();
  ret = 'V';
  ret_string = ret_array = false;
  std::size_t i = sig.find('(');
  if (i == std::string_view::npos) return;
  ++i;
  auto skip_type = [&](std::size_t& p) -> char {
    char t = sig[p];
    if (t == '[') {
      while (p < sig.size() && sig[p] == '[') ++p;
      if (p < sig.size() && sig[p] == 'L') p = sig.find(';', p);
      if (p == std::string_view::npos) p = sig.size() - 1;
      ++p;
      return 'L';
    }
    if (t == 'L') {
      p = sig.find(';', p);
      if (p == std::string_view::npos) p = sig.size() - 1;
      ++p;
      return 'L';
    }
    ++p;
    return t;
  };
  while (i < sig.size() && sig[i] != ')') params.push_back(skip_type(i));
  if (i < sig.size() && sig[i] == ')') {
    ++i;
    if (i < sig.size()) {
      ret_string = sig.substr(i) == "Ljava/lang/String;";
      ret_array = sig[i] == '[';
      ret = skip_type(i);
    }
  }
}

Method* AddMethodLocked(Registry& r, Class* c, std::string_view name, std::string_view sig, bool is_static) {
  const std::string key = std::string(name) + std::string(sig);
  auto& slot = c->methods[key];
  if (!slot) {
    slot = std::make_unique<Method>();
    slot->cls = c;
    slot->name = name;
    slot->sig = sig;
    slot->is_static = is_static;
    ParseSig(sig, slot->params, slot->ret, slot->ret_string, slot->ret_array);
    r.methods.push_back(slot.get());
    slot->id = kMethodIdBase + 8ull * r.methods.size();
  }
  return slot.get();
}

Field* AddFieldLocked(Registry& r, Class* c, std::string_view name, std::string_view sig, bool is_static) {
  const std::string key = std::string(name) + ":" + std::string(sig);
  auto& slot = c->fields[key];
  if (!slot) {
    slot = std::make_unique<Field>();
    slot->cls = c;
    slot->name = name;
    slot->sig = sig;
    slot->is_static = is_static;
    r.fields.push_back(slot.get());
    slot->id = kFieldIdBase + 8ull * r.fields.size();
  }
  return slot.get();
}
}  // namespace

bool Class::IsSubclassOf(const Class* other) const {
  for (const Class* c = this; c; c = c->super)
    if (c == other) return true;
  return false;
}

Class* GetClass(std::string_view name) {
  auto& r = Reg();
  std::lock_guard lk(r.mu);
  return GetClassLocked(r, name);
}

Class* FindLoadedClass(std::string_view name) {
  auto& r = Reg();
  std::lock_guard lk(r.mu);
  auto it = r.classes.find(NormalizeClassName(name));
  return it == r.classes.end() ? nullptr : it->second.get();
}

ObjPtr ClassObj(Class* c) { return c ? c->shared_from_this() : nullptr; }

void InitCoreClasses() {
  auto& r = Reg();
  std::lock_guard lk(r.mu);
  if (g_object) return;
  GetClassLocked(r, "java/lang/Class");
  g_object = GetClassLocked(r, "java/lang/Object");
  g_object->super = nullptr;
  g_meta->super = g_object;
  g_string = GetClassLocked(r, "java/lang/String");
  g_meta->emulated = g_object->emulated = g_string->emulated = true;
}
Class* StringClass() { return g_string; }

ClassDef DefineClass(std::string_view name) {
  Class* c = GetClass(name);
  c->emulated = true;
  return ClassDef(c);
}

void DefineEmptyClasses(std::initializer_list<const char*> names) {
  for (const char* n : names) DefineClass(n);
}

ClassDef& ClassDef::Static(std::string_view name, std::string_view sig, Impl impl) {
  auto& r = Reg();
  std::lock_guard lk(r.mu);
  Method* m = AddMethodLocked(r, c_, name, sig, true);
  m->is_static = true;
  m->impl = std::move(impl);
  return *this;
}
ClassDef& ClassDef::Virtual(std::string_view name, std::string_view sig, Impl impl) {
  auto& r = Reg();
  std::lock_guard lk(r.mu);
  Method* m = AddMethodLocked(r, c_, name, sig, false);
  m->is_static = false;
  m->impl = std::move(impl);
  return *this;
}
ClassDef& ClassDef::StaticNoop(std::initializer_list<std::pair<const char*, const char*>> name_sig) {
  for (const auto& [n, s] : name_sig) Static(n, s, [](Call& c) { return DefaultFor(*c.method); });
  return *this;
}
ClassDef& ClassDef::StaticField(std::string_view name, std::string_view sig, Value v) {
  auto& r = Reg();
  std::lock_guard lk(r.mu);
  Field* f = AddFieldLocked(r, c_, name, sig, true);
  f->defined = true;
  std::lock_guard fl(f->mu);
  f->static_value = std::move(v);
  return *this;
}
ClassDef& ClassDef::InstanceField(std::string_view name, std::string_view sig) {
  auto& r = Reg();
  std::lock_guard lk(r.mu);
  AddFieldLocked(r, c_, name, sig, false)->defined = true;
  return *this;
}
ClassDef& ClassDef::Missing(std::string_view name, std::string_view sig) {
  auto& r = Reg();
  std::lock_guard lk(r.mu);
  AddMethodLocked(r, c_, name, sig, true)->missing = true;
  return *this;
}
ClassDef& ClassDef::NoopRest() {
  c_->fallback = [](Call& c) { return DefaultFor(*c.method); };
  return *this;
}
ClassDef& ClassDef::Fallback(Impl impl) {
  c_->fallback = std::move(impl);
  return *this;
}
ClassDef& ClassDef::Super(std::string_view super_name) {
  c_->super = GetClass(super_name);
  return *this;
}
ClassDef& ClassDef::Factory(std::function<ObjPtr(Class*)> f) {
  c_->factory = std::move(f);
  return *this;
}

Method* LookupMethod(Class* c, std::string_view name, std::string_view sig) {
  if (!c) return nullptr;
  auto& r = Reg();
  std::lock_guard lk(r.mu);
  const std::string key = std::string(name) + std::string(sig);
  for (Class* k = c; k; k = k->super)
    if (auto it = k->methods.find(key); it != k->methods.end()) return it->second.get();
  return nullptr;
}

Method* GetOrAddMethod(Class* c, std::string_view name, std::string_view sig, bool is_static, bool* covered) {
  Method* m = LookupMethod(c, name, sig);
  if (m) {
    *covered = static_cast<bool>(m->impl) || static_cast<bool>(m->cls->fallback);
    return m;
  }
  *covered = static_cast<bool>(c->fallback);
  auto& r = Reg();
  std::lock_guard lk(r.mu);
  return AddMethodLocked(r, c, name, sig, is_static);
}

Method* MethodById(std::uint64_t id) {
  if (id <= kMethodIdBase || (id - kMethodIdBase) % 8) return nullptr;
  auto& r = Reg();
  std::lock_guard lk(r.mu);
  const std::uint64_t i = (id - kMethodIdBase) / 8 - 1;
  return i < r.methods.size() ? r.methods[i] : nullptr;
}

Field* GetOrAddField(Class* c, std::string_view name, std::string_view sig, bool is_static, bool* covered) {
  auto& r = Reg();
  std::lock_guard lk(r.mu);
  const std::string key = std::string(name) + ":" + std::string(sig);
  for (Class* k = c; k; k = k->super)
    if (auto it = k->fields.find(key); it != k->fields.end()) {
      *covered = it->second->defined;
      return it->second.get();
    }
  *covered = false;
  return AddFieldLocked(r, c, name, sig, is_static);
}

Field* FieldById(std::uint64_t id) {
  if (id <= kFieldIdBase || (id - kFieldIdBase) % 8) return nullptr;
  auto& r = Reg();
  std::lock_guard lk(r.mu);
  const std::uint64_t i = (id - kFieldIdBase) / 8 - 1;
  return i < r.fields.size() ? r.fields[i] : nullptr;
}

ObjPtr AllocInstance(Class* c) {
  if (!c) return nullptr;
  for (Class* k = c; k; k = k->super)
    if (k->factory) {
      ObjPtr o = k->factory(c);
      if (o) o->cls = c;
      return o;
    }
  return std::make_shared<Object>(c);
}

// ---------------------------------------------------------------------------------------------
// Strings, arrays, values
// ---------------------------------------------------------------------------------------------
std::u16string Utf8ToU16(std::string_view s) {
  std::u16string o;
  o.reserve(s.size());
  for (std::size_t i = 0; i < s.size();) {
    std::uint32_t c = static_cast<std::uint8_t>(s[i]);
    int n = 0;
    if (c < 0x80) n = 0;
    else if ((c >> 5) == 6) { c &= 0x1F; n = 1; }
    else if ((c >> 4) == 14) { c &= 0x0F; n = 2; }
    else if ((c >> 3) == 30) { c &= 0x07; n = 3; }
    else { c = 0xFFFD; n = 0; }
    ++i;
    for (int k = 0; k < n && i < s.size(); ++k, ++i) c = (c << 6) | (static_cast<std::uint8_t>(s[i]) & 0x3F);
    if (c >= 0x10000) {
      c -= 0x10000;
      o += static_cast<char16_t>(0xD800 + (c >> 10));
      o += static_cast<char16_t>(0xDC00 + (c & 0x3FF));
    } else {
      o += static_cast<char16_t>(c);  // includes lone surrogates written by modified UTF-8 (two 3-byte units)
    }
  }
  return o;
}

namespace {
void Put3(std::string& o, std::uint32_t c) {
  o += static_cast<char>(0xE0 | (c >> 12));
  o += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
  o += static_cast<char>(0x80 | (c & 0x3F));
}
void PutUnit(std::string& o, std::uint32_t c, bool modified) {
  if (c == 0 && modified) { o += '\xC0'; o += '\x80'; }
  else if (c < 0x80) o += static_cast<char>(c);
  else if (c < 0x800) { o += static_cast<char>(0xC0 | (c >> 6)); o += static_cast<char>(0x80 | (c & 0x3F)); }
  else Put3(o, c);
}
std::string FromU16(const char16_t* p, std::size_t n, bool modified) {
  std::string o;
  o.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    std::uint32_t c = p[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && p[i + 1] >= 0xDC00 && p[i + 1] < 0xE000) {
      const std::uint32_t cp = 0x10000 + ((c - 0xD800) << 10) + (p[i + 1] - 0xDC00);
      ++i;
      if (modified) {  // two surrogate units, 3 bytes each
        Put3(o, 0xD800 + ((cp - 0x10000) >> 10));
        Put3(o, 0xDC00 + ((cp - 0x10000) & 0x3FF));
      } else {
        o += static_cast<char>(0xF0 | (cp >> 18));
        o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        o += static_cast<char>(0x80 | (cp & 0x3F));
      }
    } else {
      PutUnit(o, c, modified);
    }
  }
  return o;
}
}  // namespace
std::string U16ToUtf8(const char16_t* p, std::size_t n) { return FromU16(p, n, false); }
std::string U16ToMutf8(const char16_t* p, std::size_t n) { return FromU16(p, n, true); }

std::string String::utf8() const { return U16ToUtf8(s.data(), s.size()); }

Value Value::Str(std::string_view s) { return Value::Obj(MakeString(s)); }

std::shared_ptr<String> MakeStringU16(std::u16string s) {
  return std::make_shared<String>(g_string ? g_string : GetClass("java/lang/String"), std::move(s));
}
std::shared_ptr<String> MakeString(std::string_view utf8) { return MakeStringU16(Utf8ToU16(utf8)); }

std::string Call::S(std::size_t n) const {
  auto s = std::dynamic_pointer_cast<String>(O(n));
  return s ? s->utf8() : std::string();
}

std::uint32_t ElemSize(char e) {
  switch (e) {
    case 'Z': case 'B': return 1;
    case 'C': case 'S': return 2;
    case 'J': case 'D': return 8;
    default: return 4;
  }
}

Array::Array(Class* c, char e, std::uint32_t n) : Object(c), elem(e), length(n), elem_size(ElemSize(e)) {
  if (e == 'L') objects.resize(n);
  else data = hle::heap::Calloc(n ? n : 1, elem_size);
}
Array::~Array() {
  if (data) hle::heap::Free(data);
}

std::shared_ptr<Array> MakeArray(char elem, std::uint32_t n) {
  const std::string cname = elem == 'L' ? std::string("[Ljava/lang/Object;") : std::string("[") + elem;
  auto a = std::make_shared<Array>(GetClass(cname), elem, n);
  if (elem != 'L' && !a->data) return nullptr;
  return a;
}
std::shared_ptr<Array> MakeObjectArray(std::string_view elem_class, std::uint32_t n) {
  auto a = std::make_shared<Array>(GetClass("[L" + std::string(elem_class) + ";"), 'L', n);
  return a;
}
std::shared_ptr<Array> MakeStringArray(const std::vector<std::string>& v) {
  auto a = MakeObjectArray("java/lang/String", static_cast<std::uint32_t>(v.size()));
  for (std::size_t i = 0; i < v.size(); ++i) a->objects[i] = MakeString(v[i]);
  return a;
}
std::shared_ptr<Array> MakeByteArray(const void* data, std::uint32_t n) {
  auto a = MakeArray('B', n);
  if (a && n && data) std::memcpy(a->host(), data, n);
  return a;
}
std::shared_ptr<Array> MakeIntArray(std::initializer_list<std::int32_t> v) {
  auto a = MakeArray('I', static_cast<std::uint32_t>(v.size()));
  std::uint32_t i = 0;
  for (std::int32_t x : v) std::memcpy(a->host() + 4 * i++, &x, 4);
  return a;
}

std::shared_ptr<Throwable> NewThrowable(std::string_view cls, std::string message) {
  return std::make_shared<Throwable>(GetClass(cls), std::move(message));
}

Value DefaultFor(const Method& m) {
  switch (m.ret) {
    case 'V': return Value();
    case 'L':
      if (m.ret_string) return Value::Str("");
      if (m.ret_array && m.sig.find(")[Ljava/lang/String;") != std::string::npos)
        return Value::Obj(MakeStringArray({}));
      return Value::Null();
    case 'F': case 'D': return Value::Float(0);
    default: return Value::Int(0);
  }
}

// ---------------------------------------------------------------------------------------------
// Invocation
// ---------------------------------------------------------------------------------------------
Value Invoke(Method* m, ObjPtr self, Class* cls, std::vector<Value>&& args, bool virt) {
  if (!m) return Value();
  if (virt && self && self->cls && self->cls != m->cls) {
    if (Method* o = LookupMethod(self->cls, m->name, m->sig); o && o->impl) m = o;
  }
  ++m->calls;
  if (!m->impl && m->cls->fallback) {
    Call c;
    c.method = m;
    c.self = std::move(self);
    c.cls = cls ? cls : m->cls;
    c.args = std::move(args);
    return m->cls->fallback(c);
  }
  if (!m->impl) {
    if (!m->warned.exchange(true)) {
      log::Warn("[jni] method not emulated: {}.{}{} -> default result", m->cls->name, m->name, m->sig);
      NoteCoverage(CoverageEntry::Kind::Call, m->cls->name, m->name, m->sig, false);
    }
    return DefaultFor(*m);
  }
  Call c;
  c.method = m;
  c.self = std::move(self);
  c.cls = cls ? cls : m->cls;
  c.args = std::move(args);
  return m->impl(c);
}

// ---------------------------------------------------------------------------------------------
// Coverage
// ---------------------------------------------------------------------------------------------
namespace {
struct Cov {
  std::mutex mu;
  std::map<std::string, CoverageEntry> entries;
  std::vector<std::string> order;
};
Cov& Covg() {
  static Cov c;
  return c;
}
const char* KindName(CoverageEntry::Kind k) {
  switch (k) {
    case CoverageEntry::Kind::Class: return "FindClass";
    case CoverageEntry::Kind::Method: return "GetMethodID";
    case CoverageEntry::Kind::StaticMethod: return "GetStaticMethodID";
    case CoverageEntry::Kind::Field: return "GetFieldID";
    case CoverageEntry::Kind::StaticField: return "GetStaticFieldID";
    case CoverageEntry::Kind::Call: return "Call(no impl)";
  }
  return "?";
}
}  // namespace

void NoteCoverage(CoverageEntry::Kind k, const std::string& cls, const std::string& name, const std::string& sig, bool covered) {
  auto& c = Covg();
  const std::string key = std::to_string(static_cast<int>(k)) + "|" + cls + "|" + name + "|" + sig;
  bool first = false;
  {
    std::lock_guard lk(c.mu);
    auto [it, inserted] = c.entries.try_emplace(key);
    if (inserted) {
      it->second.kind = k;
      it->second.cls = cls;
      it->second.name = name;
      it->second.sig = sig;
      it->second.covered = covered;
      c.order.push_back(key);
      first = true;
    }
    ++it->second.count;
  }
  if (first && k != CoverageEntry::Kind::Call) {
    if (k == CoverageEntry::Kind::Class) {
      if (covered) log::Debug("[jni] {}({}) -> emulated", KindName(k), cls);
      else log::Info("[jni] {}({}) -> not emulated (generic class)", KindName(k), cls);
    } else if (covered) {
      log::Debug("[jni] {}({}.{} {}) -> emulated", KindName(k), cls, name, sig);
    } else {
      log::Info("[jni] {}({}.{} {}) -> not emulated (default)", KindName(k), cls, name, sig);
    }
  }
}

std::vector<CoverageEntry> Coverage() {
  auto& c = Covg();
  std::lock_guard lk(c.mu);
  std::vector<CoverageEntry> v;
  v.reserve(c.order.size());
  for (const auto& k : c.order) v.push_back(c.entries.at(k));
  return v;
}

std::string CoverageReport() {
  std::string out;
  std::size_t n = 0, ok = 0;
  for (const auto& e : Coverage()) {
    ++n;
    if (e.covered) ++ok;
    if (e.kind == CoverageEntry::Kind::Class)
      out += std::format("  {:3} {:18} {}  (x{})\n", e.covered ? "ok" : "NO", KindName(e.kind), e.cls, e.count);
    else
      out += std::format("  {:3} {:18} {}.{} {}  (x{})\n", e.covered ? "ok" : "NO", KindName(e.kind), e.cls, e.name, e.sig, e.count);
  }
  out += std::format("  total: {} entries, {} emulated, {} not emulated\n", n, ok, n - ok);
  return out;
}

// ---------------------------------------------------------------------------------------------
// Natives
// ---------------------------------------------------------------------------------------------
namespace {
std::string Mangle(std::string_view s) {
  std::string o;
  for (char ch : s) {
    if (ch == '/' || ch == '.') o += '_';
    else if (ch == '_') o += "_1";
    else if (ch == ';') o += "_2";
    else if (ch == '[') o += "_3";
    else o += ch;
  }
  return o;
}
}  // namespace

void RegisterNativeMethod(Class* c, std::string_view name, std::string_view sig, gaddr fn) {
  auto& r = Reg();
  std::lock_guard lk(r.mu);
  r.natives["Java_" + Mangle(c->name) + "_" + Mangle(name)] = fn;
  r.natives[c->name + "." + std::string(name)] = fn;
  AddMethodLocked(r, c, name, sig, false)->native = fn;
}

void UnregisterNatives(Class* c) {
  auto& r = Reg();
  std::lock_guard lk(r.mu);
  const std::string pre1 = "Java_" + Mangle(c->name) + "_", pre2 = c->name + ".";
  for (auto it = r.natives.begin(); it != r.natives.end();)
    it = (it->first.starts_with(pre1) || it->first.starts_with(pre2)) ? r.natives.erase(it) : std::next(it);
}

gaddr FindRegisteredNative(std::string_view name) {
  auto& r = Reg();
  std::lock_guard lk(r.mu);
  auto it = r.natives.find(name);
  return it == r.natives.end() ? 0 : it->second;
}

gaddr FindNative(std::string_view name) {
  std::string cls_slash;
  std::string mangled;
  if (name.starts_with("Java_")) {
    mangled = std::string(name);
  } else {
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos) return 0;
    cls_slash = NormalizeClassName(name.substr(0, dot));
    mangled = "Java_" + Mangle(cls_slash) + "_" + Mangle(name.substr(dot + 1));
    if (gaddr a = FindRegisteredNative(cls_slash + "." + std::string(name.substr(dot + 1)))) return a;
  }
  if (gaddr a = FindRegisteredNative(mangled)) return a;
  for (const auto& m : loader::Modules())
    if (gaddr a = m->Lookup(mangled)) return a;
  return 0;
}

bool CallNativePacked(std::string_view name, const detail::ArgPack& args, Cpu::Result& out) {
  const gaddr fn = FindNative(name);
  if (!fn) {
    log::Warn("[jni] native {} does not exist", name);
    return false;
  }
  out = Cpu::Current().CallPacked(fn, args);
  return true;
}

// ---------------------------------------------------------------------------------------------
// Host API
// ---------------------------------------------------------------------------------------------
jref NewString(std::string_view utf8) { return NewLocal(MakeString(utf8)); }

std::string GetString(jref s) {
  auto o = std::dynamic_pointer_cast<String>(Resolve(s));
  return o ? o->utf8() : std::string();
}

jref FindClass(std::string_view name) { return NewLocal(ClassObj(GetClass(name))); }

Config& config() {
  static Config c;
  return c;
}

// ---------------------------------------------------------------------------------------------
// Preferences / install id (persisted to Config::prefs_file)
// ---------------------------------------------------------------------------------------------
namespace {
struct Prefs {
  std::mutex mu;
  std::map<std::pair<std::string, std::string>, std::string> m;
  bool loaded = false;
};
Prefs& P() {
  static Prefs p;
  return p;
}
std::string Esc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '\\') o += "\\\\";
    else if (c == '\t') o += "\\t";
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else o += c;
  }
  return o;
}
std::string Unesc(const std::string& s) {
  std::string o;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      const char n = s[++i];
      o += n == 't' ? '\t' : n == 'n' ? '\n' : n == 'r' ? '\r' : n;
    } else {
      o += s[i];
    }
  }
  return o;
}
void LoadLocked(Prefs& p) {
  if (p.loaded) return;
  p.loaded = true;
  const auto& path = config().prefs_file;
  if (path.empty()) return;
  std::ifstream f(path, std::ios::binary);
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto a = line.find('\t');
    const auto b = a == std::string::npos ? a : line.find('\t', a + 1);
    if (b == std::string::npos) continue;
    p.m[{Unesc(line.substr(0, a)), Unesc(line.substr(a + 1, b - a - 1))}] = Unesc(line.substr(b + 1));
  }
}
void SaveLocked(Prefs& p) {
  const auto& path = config().prefs_file;
  if (path.empty()) return;
  std::error_code ec;
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  for (const auto& [k, v] : p.m) f << Esc(k.first) << '\t' << Esc(k.second) << '\t' << Esc(v) << '\n';
}
}  // namespace

std::string PrefsGet(const std::string& ns, const std::string& key, bool* found) {
  auto& p = P();
  std::lock_guard lk(p.mu);
  LoadLocked(p);
  auto it = p.m.find({ns, key});
  if (found) *found = it != p.m.end();
  return it == p.m.end() ? std::string() : it->second;
}
void PrefsSet(const std::string& ns, const std::string& key, const std::string& value) {
  auto& p = P();
  std::lock_guard lk(p.mu);
  LoadLocked(p);
  p.m[{ns, key}] = value;
  SaveLocked(p);
}
void PrefsRemove(const std::string& ns, const std::string& key) {
  auto& p = P();
  std::lock_guard lk(p.mu);
  LoadLocked(p);
  if (p.m.erase({ns, key})) SaveLocked(p);
}

std::string InstallId() {
  bool found = false;
  std::string id = PrefsGet("install", "uuid", &found);
  if (found && id.size() == 36) return id;
  std::uint8_t b[16] = {};
  BCryptGenRandom(nullptr, b, sizeof b, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  b[6] = static_cast<std::uint8_t>((b[6] & 0x0F) | 0x40);
  b[8] = static_cast<std::uint8_t>((b[8] & 0x3F) | 0x80);
  id = std::format("{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}", b[0], b[1],
                   b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  PrefsSet("install", "uuid", id);
  return id;
}

}  // namespace nova::jni
