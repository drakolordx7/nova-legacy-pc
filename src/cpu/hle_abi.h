// Signature-deduced AAPCS64 marshalling for HLE handlers: NOVA_HLE("strlen", my_strlen) turns a plain C++ function
// into an import handler by reading the guest's x0-x7 / v0-v7 / stack according to the function's parameter types.
//
//   static int64_t hle_labs(int64_t x) { return x < 0 ? -x : x; }
//   NOVA_HLE("labs", hle_labs);
//
// Supported parameter types: Cpu& (the current CPU, consumes nothing), bool, integers and enums up to 8 bytes
// (guest `long` is 64-bit: never write host `long`, it is rejected), float, double, T* (translated with g2h),
// structs <= 16 bytes (x regs, or HFAs in v regs when declared with NOVA_HLE_HFA), structs > 16 bytes (passed by
// the caller as a pointer to a copy), Varargs (must be last: the "..." of a variadic callee) and VaList (a guest
// AAPCS64 `va_list` passed by value, i.e. as a pointer to the 32-byte struct).
// Supported return types: void, bool, integers, enums, pointers (h2g), float, double, structs (<= 16 bytes in x0:x1,
// HFAs in v0..v3, larger ones through the x8 sret pointer).
// Adapted from Zombiewood's ZW_HLE (MIT, (c) 2026 ChrisDutary) for AAPCS64.
#pragma once
#include <bit>
#include <cstdint>
#include <cstring>
#include <tuple>
#include <type_traits>
#include <utility>

#include "cpu/cpu.h"
#include "cpu/hle.h"

namespace nova {

// Declares that struct T is a Homogeneous Floating-point Aggregate of N elements of type E (float or double, N <= 4).
// Specialise in namespace nova::hle before registering handlers that take or return T.
namespace hle {
template <class T>
struct HfaTraits {
  static constexpr unsigned n = 0;
};
}  // namespace hle
#define NOVA_HLE_HFA(T, E, N)                \
  template <>                                \
  struct nova::hle::HfaTraits<T> {           \
    using elem = E;                          \
    static constexpr unsigned n = N;         \
    static_assert(sizeof(T) == sizeof(E) * N); \
  }

// Reader of variadic arguments, in the Linux AAPCS64 convention: integers/pointers from x regs then the stack,
// float/double from v regs then the stack (every variadic double takes a whole stack slot).
class VaList {
 public:
  VaList() = default;
  // The register-passed args of the call in progress, starting after `named_gr` integer regs and `named_vr` fp regs;
  // `stack` is the address of the first stack-passed argument.
  static VaList FromCall(Cpu& cpu, unsigned named_gr, unsigned named_vr, gaddr stack) {
    VaList v;
    v.cpu_ = &cpu;
    v.gr_ = named_gr;
    v.vr_ = named_vr;
    v.stack_ = stack;
    return v;
  }
  // A guest `va_list` (AAPCS64: struct { void* stack; void* gr_top; void* vr_top; int gr_offs; int vr_offs; }).
  static VaList FromGuest(gaddr list) {
    VaList v;
    v.list_ = list;
    return v;
  }

  std::uint64_t NextInt() {
    if (cpu_) {
      if (gr_ < 8) return cpu_->x(gr_++);
      const std::uint64_t r = mem::R64(stack_);
      stack_ += 8;
      return r;
    }
    std::int32_t gr_offs = mem::R<std::int32_t>(list_ + 24);
    if (gr_offs < 0) {
      const std::int32_t next = gr_offs + 8;
      mem::W<std::int32_t>(list_ + 24, next);
      if (next <= 0) return mem::R64(mem::R64(list_ + 8) + static_cast<std::int64_t>(gr_offs));
    }
    const gaddr s = mem::R64(list_);
    mem::W64(list_, s + 8);
    return mem::R64(s);
  }
  // Raw bits of the next double (or float promoted to double by the caller).
  std::uint64_t NextFp() {
    if (cpu_) {
      if (vr_ < 8) return cpu_->v(vr_++)[0];
      const std::uint64_t r = mem::R64(stack_);
      stack_ += 8;
      return r;
    }
    std::int32_t vr_offs = mem::R<std::int32_t>(list_ + 28);
    if (vr_offs < 0) {
      const std::int32_t next = vr_offs + 16;
      mem::W<std::int32_t>(list_ + 28, next);
      if (next <= 0) return mem::R64(mem::R64(list_ + 16) + static_cast<std::int64_t>(vr_offs));
    }
    const gaddr s = mem::R64(list_);
    mem::W64(list_, s + 8);
    return mem::R64(s);
  }
  // `long double` (IEEE binary128) as 16 raw bytes: a q register or an aligned 16-byte stack slot.
  Vec128 NextQuad() {
    if (cpu_) {
      if (vr_ < 8) return cpu_->v(vr_++);
      stack_ = (stack_ + 15) & ~15ull;
      Vec128 r{mem::R64(stack_), mem::R64(stack_ + 8)};
      stack_ += 16;
      return r;
    }
    std::int32_t vr_offs = mem::R<std::int32_t>(list_ + 28);
    if (vr_offs < 0) {
      const std::int32_t next = vr_offs + 16;
      mem::W<std::int32_t>(list_ + 28, next);
      if (next <= 0) {
        const gaddr p = mem::R64(list_ + 16) + static_cast<std::int64_t>(vr_offs);
        return Vec128{mem::R64(p), mem::R64(p + 8)};
      }
    }
    gaddr s = (mem::R64(list_) + 15) & ~15ull;
    mem::W64(list_, s + 16);
    return Vec128{mem::R64(s), mem::R64(s + 8)};
  }

  // Next argument as C type T after the usual promotions (char/short -> int, float -> double; Next<float>() is
  // rejected, ask for double).
  template <class T>
  T Next() {
    static_assert(!std::is_same_v<T, float>, "VaList: float is promoted to double; use Next<double>()");
    static_assert(!std::is_same_v<T, long> && !std::is_same_v<T, unsigned long>,
                  "host `long` is 32-bit: use int64_t/uint64_t for the guest's long");
    if constexpr (std::is_same_v<T, bool>) {
      return (NextInt() & 0xFF) != 0;
    } else if constexpr (std::is_same_v<T, double>) {
      return std::bit_cast<double>(NextFp());
    } else if constexpr (std::is_pointer_v<T>) {
      const std::uint64_t a = NextInt();
      return a ? g2h<std::remove_pointer_t<T>>(a) : nullptr;
    } else {
      static_assert(std::is_integral_v<T> || std::is_enum_v<T>, "VaList::Next: unsupported type");
      return static_cast<T>(NextInt());
    }
  }

  // For va_copy / forwarding a guest list.
  bool from_call() const { return cpu_ != nullptr; }
  gaddr guest_list() const { return list_; }

 private:
  Cpu* cpu_ = nullptr;
  unsigned gr_ = 0, vr_ = 0;
  gaddr stack_ = 0;
  gaddr list_ = 0;
};

// The "..." parameter of a handler; must be the last one.
struct Varargs : VaList {
  Varargs() = default;
  explicit Varargs(const VaList& v) : VaList(v) {}
};

namespace hle::detail {
template <class T>
inline constexpr bool is_int_like = (std::is_integral_v<T> || std::is_enum_v<T>) && sizeof(T) <= 8;

struct ArgCursor {
  Cpu& cpu;
  unsigned gr = 0, vr = 0;
  std::uint64_t stack = 0;  // offset from sp of the next stack argument

  std::uint64_t NextInt() {
    if (gr < 8) return cpu.x(gr++);
    const std::uint64_t r = cpu.stack_arg(static_cast<unsigned>(stack / 8));
    stack += 8;
    return r;
  }
  std::uint64_t NextFp() {
    if (vr < 8) return cpu.v(vr++)[0];
    const std::uint64_t r = cpu.stack_arg(static_cast<unsigned>(stack / 8));
    stack += 8;
    return r;
  }
};

template <class T>
T GetStruct(ArgCursor& c) {
  static_assert(std::is_trivially_copyable_v<T>, "HLE: struct parameters must be trivially copyable");
  static_assert(alignof(T) <= 8, "HLE: 16-byte aligned struct parameters are not supported");
  using H = HfaTraits<T>;
  T out{};
  if constexpr (H::n != 0) {
    using E = typename H::elem;
    if (c.vr + H::n <= 8) {
      for (unsigned i = 0; i < H::n; ++i) {
        const std::uint64_t bits = c.cpu.v(c.vr + i)[0];
        std::memcpy(reinterpret_cast<char*>(&out) + i * sizeof(E), &bits, sizeof(E));
      }
      c.vr += H::n;
    } else {
      c.vr = 8;
      const unsigned words = (sizeof(T) + 7) / 8;
      std::uint64_t w[4] = {};
      for (unsigned i = 0; i < words; ++i) w[i] = c.cpu.stack_arg(static_cast<unsigned>(c.stack / 8) + i);
      c.stack += 8ull * words;
      std::memcpy(&out, w, sizeof(T));
    }
  } else if constexpr (sizeof(T) <= 16) {
    const unsigned words = (sizeof(T) + 7) / 8;
    std::uint64_t w[2] = {};
    if (c.gr + words <= 8) {
      for (unsigned i = 0; i < words; ++i) w[i] = c.cpu.x(c.gr + i);
      c.gr += words;
    } else {
      c.gr = 8;
      for (unsigned i = 0; i < words; ++i) w[i] = c.cpu.stack_arg(static_cast<unsigned>(c.stack / 8) + i);
      c.stack += 8ull * words;
    }
    std::memcpy(&out, w, sizeof(T));
  } else {
    const std::uint64_t p = c.NextInt();  // pointer to the caller's copy
    std::memcpy(&out, g2h(p), sizeof(T));
  }
  return out;
}

template <class P>
decltype(auto) GetArg(ArgCursor& c) {
  using D = std::remove_cv_t<std::remove_reference_t<P>>;
  static_assert(!std::is_same_v<D, long> && !std::is_same_v<D, unsigned long>,
                "HLE: host `long` is 32-bit; use int32_t/int64_t (guest long is 64-bit)");
  if constexpr (std::is_same_v<D, Cpu>) {
    static_assert(std::is_lvalue_reference_v<P>, "HLE: the Cpu parameter must be Cpu&");
    return static_cast<Cpu&>(c.cpu);
  } else if constexpr (std::is_same_v<D, Varargs>) {
    return Varargs(VaList::FromCall(c.cpu, c.gr, c.vr, c.cpu.sp() + c.stack));
  } else if constexpr (std::is_same_v<D, VaList>) {
    return VaList::FromGuest(c.NextInt());
  } else if constexpr (std::is_same_v<D, bool>) {
    return (c.NextInt() & 0xFF) != 0;
  } else if constexpr (std::is_same_v<D, float>) {
    return std::bit_cast<float>(static_cast<std::uint32_t>(c.NextFp()));
  } else if constexpr (std::is_same_v<D, double>) {
    return std::bit_cast<double>(c.NextFp());
  } else if constexpr (std::is_enum_v<D>) {
    return static_cast<D>(c.NextInt());
  } else if constexpr (std::is_integral_v<D>) {
    static_assert(sizeof(D) <= 8);
    return static_cast<D>(c.NextInt());
  } else if constexpr (std::is_pointer_v<D>) {
    const std::uint64_t a = c.NextInt();
    return a ? g2h<std::remove_pointer_t<D>>(a) : static_cast<D>(nullptr);
  } else if constexpr (std::is_class_v<D>) {
    return GetStruct<D>(c);
  } else {
    static_assert(sizeof(D) == 0, "HLE: unsupported parameter type");
  }
}

template <class R>
void SetResult(Cpu& cpu, const R& v) {
  static_assert(!std::is_same_v<R, long> && !std::is_same_v<R, unsigned long>,
                "HLE: host `long` is 32-bit; return int32_t/int64_t");
  if constexpr (std::is_same_v<R, bool>) {
    cpu.set_x(0, v ? 1 : 0);
  } else if constexpr (std::is_same_v<R, float>) {
    cpu.set_v(0, {std::bit_cast<std::uint32_t>(v), 0});
  } else if constexpr (std::is_same_v<R, double>) {
    cpu.set_v(0, {std::bit_cast<std::uint64_t>(v), 0});
  } else if constexpr (std::is_enum_v<R>) {
    cpu.set_x(0, static_cast<std::uint64_t>(static_cast<std::int64_t>(v)));
  } else if constexpr (std::is_integral_v<R>) {
    if constexpr (std::is_signed_v<R>) cpu.set_x(0, static_cast<std::uint64_t>(static_cast<std::int64_t>(v)));
    else cpu.set_x(0, static_cast<std::uint64_t>(v));
  } else if constexpr (std::is_pointer_v<R>) {
    cpu.set_x(0, v ? h2g(v) : 0);
  } else if constexpr (std::is_class_v<R>) {
    static_assert(std::is_trivially_copyable_v<R>);
    using H = HfaTraits<R>;
    if constexpr (H::n != 0) {
      using E = typename H::elem;
      for (unsigned i = 0; i < H::n; ++i) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, reinterpret_cast<const char*>(&v) + i * sizeof(E), sizeof(E));
        cpu.set_v(i, {bits, 0});
      }
    } else if constexpr (sizeof(R) <= 16) {
      std::uint64_t w[2] = {};
      std::memcpy(w, &v, sizeof(R));
      cpu.set_x(0, w[0]);
      if constexpr (sizeof(R) > 8) cpu.set_x(1, w[1]);
    } else {
      const std::uint64_t dst = cpu.x(8);  // sret
      std::memcpy(g2h(dst), &v, sizeof(R));
      cpu.set_x(0, dst);
    }
  } else {
    static_assert(std::is_void_v<R>, "HLE: unsupported return type");
  }
}

template <class R, class... A>
void Invoke(Cpu& cpu, R (*fn)(A...)) {
  ArgCursor cur{cpu};
  // Brace initialisation evaluates left to right, which is the order the arguments are consumed in.
  std::tuple<decltype(GetArg<A>(cur))...> args{GetArg<A>(cur)...};
  if constexpr (std::is_void_v<R>) {
    std::apply(fn, std::move(args));
  } else {
    SetResult<std::remove_cv_t<R>>(cpu, std::apply(fn, std::move(args)));
  }
}

template <auto Fn>
struct Thunk {
  static void Call(Cpu& cpu) { Invoke(cpu, Fn); }
};
}  // namespace hle::detail
}  // namespace nova

// Register with automatic marshalling. Call inside a registration function (not from a static initialiser: the
// linker drops unreferenced objects of a static library). `fn` must be a non-overloaded free function.
#define NOVA_HLE(name, fn) ::nova::hle::Register((name), &::nova::hle::detail::Thunk<fn>::Call, ::nova::hle::kDefault)
// Same, but the handler runs inside the JIT (hle::kFast): no call_guest, no throwing, no long blocking.
#define NOVA_HLE_FAST(name, fn) ::nova::hle::Register((name), &::nova::hle::detail::Thunk<fn>::Call, ::nova::hle::kFast)
// Raw void(Cpu&) handler.
#define NOVA_HLE_RAW(name, rawfn) ::nova::hle::Register((name), (rawfn), ::nova::hle::kDefault)
// Registers and yields the guest address of the stub (for vtables and function pointers).
#define NOVA_HLE_THUNK(name, fn) (NOVA_HLE((name), fn), ::nova::hle::StubFor((name)))
