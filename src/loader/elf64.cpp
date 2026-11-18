#include "loader/elf64.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <mutex>
#include <unordered_map>

#include "core/log.h"
#include "cpu/cpu.h"
#include "cpu/hle.h"

namespace nova::loader {
namespace {
// --- ELF64 structures (Windows has no <elf.h>) ---
struct Ehdr {
  std::uint8_t e_ident[16];
  std::uint16_t e_type, e_machine;
  std::uint32_t e_version;
  std::uint64_t e_entry, e_phoff, e_shoff;
  std::uint32_t e_flags;
  std::uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};
struct Phdr {
  std::uint32_t p_type, p_flags;
  std::uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};
struct Dyn {
  std::int64_t d_tag;
  std::uint64_t d_val;
};
struct Sym {
  std::uint32_t st_name;
  std::uint8_t st_info, st_other;
  std::uint16_t st_shndx;
  std::uint64_t st_value, st_size;
};
struct Rela {
  std::uint64_t r_offset, r_info;
  std::int64_t r_addend;
};
static_assert(sizeof(Ehdr) == 64 && sizeof(Phdr) == 56 && sizeof(Dyn) == 16 && sizeof(Sym) == 24 && sizeof(Rela) == 24);

constexpr std::uint16_t ET_DYN = 3, EM_AARCH64 = 183;
enum : std::uint32_t {
  PT_LOAD = 1, PT_DYNAMIC = 2, PT_TLS = 7, PT_GNU_EH_FRAME = 0x6474e550, PT_GNU_RELRO = 0x6474e552,
};
enum : std::int64_t {
  DT_NULL = 0, DT_NEEDED = 1, DT_PLTRELSZ = 2, DT_PLTGOT = 3, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7,
  DT_RELASZ = 8, DT_RELAENT = 9, DT_STRSZ = 10, DT_SYMENT = 11, DT_INIT = 12, DT_FINI = 13, DT_SONAME = 14, DT_REL = 17,
  DT_RELSZ = 18, DT_PLTREL = 20, DT_JMPREL = 23, DT_INIT_ARRAY = 25, DT_FINI_ARRAY = 26, DT_INIT_ARRAYSZ = 27,
  DT_FINI_ARRAYSZ = 28, DT_PREINIT_ARRAY = 32, DT_PREINIT_ARRAYSZ = 33, DT_RELRSZ = 35, DT_RELR = 36, DT_RELRENT = 37,
  DT_ANDROID_REL = 0x6000000f, DT_ANDROID_RELA = 0x60000011, DT_ANDROID_RELR = 0x6fffe000, DT_GNU_HASH = 0x6ffffef5,
};
enum : std::uint32_t {
  R_AARCH64_NONE = 0, R_AARCH64_ABS64 = 257, R_AARCH64_COPY = 1024, R_AARCH64_GLOB_DAT = 1025, R_AARCH64_JUMP_SLOT = 1026,
  R_AARCH64_RELATIVE = 1027, R_AARCH64_TLS_DTPMOD64 = 1028, R_AARCH64_TLS_DTPREL64 = 1029, R_AARCH64_TLS_TPREL64 = 1030,
  R_AARCH64_TLSDESC = 1031, R_AARCH64_IRELATIVE = 1032,
};
constexpr std::uint8_t STB_LOCAL = 0, STB_WEAK = 2;
constexpr std::uint8_t STT_NOTYPE = 0, STT_OBJECT = 1, STT_FUNC = 2, STT_TLS = 6, STT_GNU_IFUNC = 10;

const char* RelocName(std::uint32_t t) {
  switch (t) {
    case R_AARCH64_NONE: return "NONE";
    case R_AARCH64_ABS64: return "ABS64";
    case R_AARCH64_COPY: return "COPY";
    case R_AARCH64_GLOB_DAT: return "GLOB_DAT";
    case R_AARCH64_JUMP_SLOT: return "JUMP_SLOT";
    case R_AARCH64_RELATIVE: return "RELATIVE";
    case R_AARCH64_TLS_DTPMOD64: return "TLS_DTPMOD64";
    case R_AARCH64_TLS_DTPREL64: return "TLS_DTPREL64";
    case R_AARCH64_TLS_TPREL64: return "TLS_TPREL64";
    case R_AARCH64_TLSDESC: return "TLSDESC";
    case R_AARCH64_IRELATIVE: return "IRELATIVE";
    default: return nullptr;
  }
}

std::uint32_t ElfHash(const char* s) {
  std::uint32_t h = 0;
  for (; *s; ++s) {
    h = (h << 4) + static_cast<std::uint8_t>(*s);
    const std::uint32_t g = h & 0xf0000000;
    if (g) h ^= g >> 24;
    h &= ~g;
  }
  return h;
}
std::uint32_t GnuHash(const char* s) {
  std::uint32_t h = 5381;
  for (; *s; ++s) h = h * 33 + static_cast<std::uint8_t>(*s);
  return h;
}

constexpr gsize PageDown(gsize v) { return v & ~(kPageSize - 1); }
constexpr gsize PageUp(gsize v) { return (v + kPageSize - 1) & ~(kPageSize - 1); }

std::mutex g_mods_mu;
std::vector<std::shared_ptr<Module>> g_modules;

std::uint8_t ProtOfFlags(std::uint32_t f) {
  std::uint8_t p = 0;
  if (f & 4) p |= kProtRead;
  if (f & 2) p |= kProtWrite;
  if (f & 1) p |= kProtExec;
  return p;
}

// Guest-visible, symbol-table access helpers on a module.
const Sym* SymAt(const Module& m, std::size_t i) { return reinterpret_cast<const Sym*>(g2h(m.symtab)) + i; }
const char* StrAt(const Module& m, std::uint32_t off) { return reinterpret_cast<const char*>(g2h(m.strtab + off)); }
}  // namespace

gaddr Module::Lookup(std::string_view name) const {
  const std::string key(name);
  auto matches = [&](std::size_t i) -> gaddr {
    const Sym* s = SymAt(*this, i);
    if (s->st_shndx == 0 || (s->st_info >> 4) == STB_LOCAL) return 0;
    if (std::strcmp(StrAt(*this, s->st_name), key.c_str()) != 0) return 0;
    const std::uint8_t t = s->st_info & 0xf;
    if (t == STT_TLS) return 0;
    return base + s->st_value;
  };
  if (hash) {
    const auto* h = reinterpret_cast<const std::uint32_t*>(g2h(hash));
    const std::uint32_t nb = h[0];
    if (nb == 0) return 0;
    const std::uint32_t* bucket = h + 2;
    const std::uint32_t* chain = bucket + nb;
    for (std::uint32_t i = bucket[ElfHash(key.c_str()) % nb]; i; i = chain[i])
      if (gaddr a = matches(i)) return a;
    return 0;
  }
  if (gnu_hash) {
    const auto* h = reinterpret_cast<const std::uint32_t*>(g2h(gnu_hash));
    const std::uint32_t nb = h[0], symoff = h[1], bloom_size = h[2], bloom_shift = h[3];
    const auto* bloom = reinterpret_cast<const std::uint64_t*>(h + 4);
    const std::uint32_t* bucket = reinterpret_cast<const std::uint32_t*>(bloom + bloom_size);
    const std::uint32_t* chain = bucket + nb;
    const std::uint32_t hv = GnuHash(key.c_str());
    const std::uint64_t word = bloom[(hv / 64) % bloom_size];
    const std::uint64_t mask = (1ull << (hv % 64)) | (1ull << ((hv >> bloom_shift) % 64));
    if ((word & mask) != mask) return 0;
    std::uint32_t i = bucket[hv % nb];
    if (i < symoff) return 0;
    for (;; ++i) {
      const std::uint32_t ch = chain[i - symoff];
      if ((ch | 1) == (hv | 1))
        if (gaddr a = matches(i)) return a;
      if (ch & 1) break;
    }
  }
  return 0;
}

bool Module::Symbolize(gaddr addr, std::string& out) const {
  if (!Contains(addr)) return false;
  if (!sorted_built_) {
    for (std::size_t i = 1; i < nsyms; ++i) {
      const Sym* s = SymAt(*this, i);
      const std::uint8_t t = s->st_info & 0xf;
      if (s->st_shndx == 0 || s->st_value == 0 || (t != STT_FUNC && t != STT_OBJECT && t != STT_NOTYPE)) continue;
      sorted_.push_back({base + s->st_value, {static_cast<std::uint32_t>(s->st_size), s->st_name}});
    }
    std::sort(sorted_.begin(), sorted_.end());
    sorted_built_ = true;
  }
  auto it = std::upper_bound(sorted_.begin(), sorted_.end(), std::make_pair(addr, std::make_pair(~0u, ~0u)));
  if (it != sorted_.begin()) {
    --it;
    const gsize off = addr - it->first;
    if (off < (it->second.first ? it->second.first : 0x1000)) {
      out = std::format("{}!{}+0x{:x}", name, StrAt(*this, it->second.second), off);
      return true;
    }
  }
  out = std::format("{}+0x{:x}", name, addr - base);
  return true;
}

const std::vector<std::shared_ptr<Module>>& Modules() { return g_modules; }

std::string ClassifyImport(std::string_view n) {
  auto starts = [&](std::string_view p) { return n.substr(0, p.size()) == p; };
  auto upper_at = [&](size_t i) { return i < n.size() && n[i] >= 'A' && n[i] <= 'Z'; };
  if (starts("__android_log_")) return "liblog";
  if (starts("gl") && upper_at(2)) return "libGLESv2";
  if (starts("egl") && upper_at(3)) return "libEGL";
  if ((starts("sl") && upper_at(2)) || starts("SL_IID") || starts("SL_")) return "libOpenSLES";
  if (n.size() > 2 && n[0] == 'A' && upper_at(1) && n.find('_') != std::string_view::npos) return "libandroid";
  if (n.size() > 6 && n.substr(0, 6) == "ANativ") return "libandroid";
  static const char* kZ[] = {"zlibVersion", "deflate", "inflate", "crc32", "adler32", "compress", "uncompress", "gz",
                             "zError", "zlibCompileFlags"};
  for (const char* p : kZ)
    if (starts(p)) return "libz";
  static const char* kDl[] = {"dlopen", "dlsym", "dlclose", "dlerror", "dladdr", "dl_iterate_phdr", "android_dlopen_ext",
                              "dl_unwind_find_exidx", "android_get_LD_LIBRARY_PATH", "dlvsym"};
  for (const char* p : kDl)
    if (n == p) return "libdl";
  static const char* kM[] = {
      "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "sinh", "cosh", "tanh", "asinh", "acosh", "atanh", "exp",
      "exp2", "expm1", "log", "log2", "log10", "log1p", "pow", "sqrt", "cbrt", "hypot", "ceil", "floor", "round", "trunc",
      "rint", "nearbyint", "lround", "llround", "lrint", "llrint", "fabs", "fmod", "remainder", "remquo", "fmax", "fmin",
      "fdim", "fma", "ldexp", "frexp", "modf", "scalbn", "scalbln", "ilogb", "logb", "copysign", "nan", "nextafter",
      "nexttoward", "erf", "erfc", "tgamma", "lgamma", "sincos", "exp10", "significand", "drem", "gamma", "j0", "j1", "jn",
      "y0", "y1", "yn", "finite", "isinf", "isnan", "scalb"};
  auto is_m = [&](std::string_view s) {
    for (const char* p : kM)
      if (s == p) return true;
    return false;
  };
  if (is_m(n)) return "libm";
  if (n.size() > 1 && (n.back() == 'f' || n.back() == 'l') && is_m(n.substr(0, n.size() - 1))) return "libm";
  if (starts("__isnan") || starts("__isinf") || starts("__isfinite") || starts("__signbit") || starts("__fpclassify") ||
      starts("__finite") || starts("__isnormal"))
    return "libm";
  return "libc";
}

namespace {
struct Loader {
  Module& m;
  std::vector<std::uint8_t> file;
  std::string err;
  const Ehdr* eh = nullptr;
  std::vector<Phdr> ph;

  // dynamic info
  gaddr rela = 0, jmprel = 0, relr = 0;
  gsize relasz = 0, pltrelsz = 0, relrsz = 0;
  std::uint64_t soname_off = 0;
  bool has_soname = false;
  std::vector<std::uint64_t> needed_offs;

  struct Resolved {
    gaddr value = 0;
    bool done = false;
    bool tls = false;
    bool defined_here = false;
    std::size_t import_index = static_cast<std::size_t>(-1);
  };
  std::vector<Resolved> resolved;
  gaddr tlsdesc_stub = 0;
  gsize tls_block_offset = kTlsStaticOffset;

  explicit Loader(Module& mod) : m(mod) {}

  bool Fail(std::string msg) {
    err = std::move(msg);
    return false;
  }

  bool ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return Fail("cannot open " + path);
    const auto sz = static_cast<std::size_t>(f.tellg());
    file.resize(sz);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(file.data()), static_cast<std::streamsize>(sz));
    return f.good();
  }

  bool ParseHeaders() {
    if (file.size() < sizeof(Ehdr) || std::memcmp(file.data(), "\x7f" "ELF", 4) != 0) return Fail("not an ELF file");
    eh = reinterpret_cast<const Ehdr*>(file.data());
    if (eh->e_ident[4] != 2 || eh->e_ident[5] != 1) return Fail("not a little-endian ELF64 file");
    if (eh->e_machine != EM_AARCH64) return Fail("not an AArch64 ELF");
    if (eh->e_type != ET_DYN) return Fail("not a shared object (ET_DYN)");
    if (eh->e_phentsize != sizeof(Phdr) || eh->e_phoff + static_cast<std::uint64_t>(eh->e_phnum) * sizeof(Phdr) > file.size())
      return Fail("bad program header table");
    ph.assign(reinterpret_cast<const Phdr*>(file.data() + eh->e_phoff),
              reinterpret_cast<const Phdr*>(file.data() + eh->e_phoff) + eh->e_phnum);
    m.entry = eh->e_entry;
    m.phnum = eh->e_phnum;
    return true;
  }

  bool MapSegments(const std::string& path) {
    gaddr lo = ~0ull, hi = 0;
    gsize align = 0x1000;
    for (const Phdr& p : ph) {
      if (p.p_type != PT_LOAD) continue;
      if (p.p_offset + p.p_filesz > file.size()) return Fail("PT_LOAD extends past the end of the file");
      lo = std::min(lo, PageDown(p.p_vaddr));
      hi = std::max(hi, PageUp(p.p_vaddr + p.p_memsz));
      align = std::max<gsize>(align, p.p_align);
      Segment s{p.p_vaddr, p.p_memsz, p.p_filesz, p.p_offset, p.p_flags};
      m.segments.push_back(s);
    }
    if (m.segments.empty()) return Fail("no PT_LOAD segments");
    m.span = hi - lo;
    const gaddr at = GuestMemory::MapAnywhere(Zone::Libs, m.span, kProtRW, m.name.c_str(), std::max<gsize>(align, 0x10000));
    if (!at) return Fail("out of guest address space for the module");
    m.base = at - lo;
    for (const Segment& s : m.segments)
      std::memcpy(g2h(m.base + s.vaddr), file.data() + s.offset, s.filesz);  // bss tail is already zero
    (void)path;
    return true;
  }

  bool ParseDynamic() {
    const Phdr* dyn = nullptr;
    for (const Phdr& p : ph) {
      if (p.p_type == PT_DYNAMIC) dyn = &p;
      if (p.p_type == PT_GNU_EH_FRAME) {
        m.eh_frame_hdr = m.base + p.p_vaddr;
        m.eh_frame_hdr_size = p.p_memsz;
      }
      if (p.p_type == PT_TLS) {
        m.has_tls = true;
        m.tls_image = m.base + p.p_vaddr;
        m.tls_filesz = p.p_filesz;
        m.tls_memsz = p.p_memsz;
        m.tls_align = p.p_align ? p.p_align : 1;
      }
    }
    if (!dyn) return Fail("no PT_DYNAMIC");
    m.dynamic = m.base + dyn->p_vaddr;
    const auto* d = reinterpret_cast<const Dyn*>(g2h(m.dynamic));
    for (; d->d_tag != DT_NULL; ++d) {
      const std::uint64_t v = d->d_val;
      switch (d->d_tag) {
        case DT_NEEDED: needed_offs.push_back(v); break;
        case DT_SONAME: soname_off = v; has_soname = true; break;
        case DT_HASH: m.hash = m.base + v; break;
        case DT_GNU_HASH: m.gnu_hash = m.base + v; break;
        case DT_STRTAB: m.strtab = m.base + v; break;
        case DT_SYMTAB: m.symtab = m.base + v; break;
        case DT_RELA: rela = m.base + v; break;
        case DT_RELASZ: relasz = v; break;
        case DT_JMPREL: jmprel = m.base + v; break;
        case DT_PLTRELSZ: pltrelsz = v; break;
        case DT_PLTREL:
          if (v != 7) return Fail("DT_PLTREL is not RELA");
          break;
        case DT_REL:
        case DT_RELSZ:
          if (v) return Fail("REL (non-RELA) relocations are not supported");
          break;
        case DT_ANDROID_REL:
        case DT_ANDROID_RELA:
        case DT_ANDROID_RELR:
          return Fail("Android packed relocations (APS2) are not supported");
        case DT_RELR: relr = m.base + v; break;
        case DT_RELRSZ: relrsz = v; break;
        case DT_INIT: m.init = m.base + v; break;
        case DT_FINI: m.fini = m.base + v; break;
        case DT_INIT_ARRAY: m.init_array = m.base + v; break;
        case DT_INIT_ARRAYSZ: m.init_array_count = v / 8; break;
        case DT_FINI_ARRAY: m.fini_array = m.base + v; break;
        case DT_FINI_ARRAYSZ: m.fini_array_count = v / 8; break;
        case DT_PREINIT_ARRAY: m.preinit_array = m.base + v; break;
        case DT_PREINIT_ARRAYSZ: m.preinit_array_count = v / 8; break;
        default: break;
      }
    }
    if (!m.symtab || !m.strtab) return Fail("missing DT_SYMTAB/DT_STRTAB");
    if (!m.hash && !m.gnu_hash) return Fail("neither DT_HASH nor DT_GNU_HASH present");
    for (std::uint64_t off : needed_offs) m.needed.push_back(StrAt(m, static_cast<std::uint32_t>(off)));
    if (has_soname) m.name = StrAt(m, static_cast<std::uint32_t>(soname_off));
    // Symbol count: DT_HASH nchain, or the end of the last GNU hash chain.
    if (m.hash) {
      m.nsyms = reinterpret_cast<const std::uint32_t*>(g2h(m.hash))[1];
    } else {
      const auto* h = reinterpret_cast<const std::uint32_t*>(g2h(m.gnu_hash));
      const std::uint32_t nb = h[0], symoff = h[1], bloom_size = h[2];
      const auto* bucket = reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint64_t*>(h + 4) + bloom_size);
      const std::uint32_t* chain = bucket + nb;
      std::uint32_t last = 0;
      for (std::uint32_t i = 0; i < nb; ++i) last = std::max(last, bucket[i]);
      if (last >= symoff) {
        while (!(chain[last - symoff] & 1)) ++last;
        ++last;
      } else {
        last = symoff;
      }
      m.nsyms = last;
    }
    resolved.assign(m.nsyms, Resolved{});
    return true;
  }

  bool SetupPhdrs() {
    // Program headers are normally inside the first PT_LOAD; point dlpi_phdr there, else copy them.
    for (const Segment& s : m.segments) {
      if (eh->e_phoff >= s.offset && eh->e_phoff + m.phnum * sizeof(Phdr) <= s.offset + s.filesz) {
        m.phdr = m.base + s.vaddr + (eh->e_phoff - s.offset);
        break;
      }
    }
    if (!m.phdr) {
      m.phdr = hle::DefineData("__nova_phdr_" + m.name, m.phnum * sizeof(Phdr), 16);
      std::memcpy(g2h(m.phdr), file.data() + eh->e_phoff, m.phnum * sizeof(Phdr));
    }
    std::string nm = m.name;
    m.name_addr = hle::DefineData("__nova_modname_" + m.name, nm.size() + 1, 8);
    std::memcpy(g2h(m.name_addr), nm.c_str(), nm.size() + 1);
    return true;
  }

  // Resolves symbol `idx` (once). Imports are bound to HLE data or stubs and recorded.
  Resolved& Resolve(std::uint32_t idx) {
    Resolved& r = resolved[idx];
    if (r.done) return r;
    r.done = true;
    const Sym* s = SymAt(m, idx);
    const std::uint8_t type = s->st_info & 0xf, bind = s->st_info >> 4;
    if (s->st_shndx != 0) {  // defined in this module
      r.defined_here = true;
      r.tls = type == STT_TLS;
      r.value = r.tls ? s->st_value : m.base + s->st_value;
      return r;
    }
    const std::string name = StrAt(m, s->st_name);
    ImportRecord rec;
    rec.name = name;
    rec.library = ClassifyImport(name);
    rec.weak = bind == STB_WEAK;
    if (type == STT_TLS) {
      log::Warn("TLS import {} is not supported; bound to 0", name);
      rec.addr = 0;
    } else if (const gaddr d = hle::FindData(name)) {
      rec.data = true;
      rec.handled = true;
      rec.addr = d;
    } else if (type == STT_OBJECT) {
      rec.data = true;
      rec.addr = hle::DefineData(name, s->st_size ? s->st_size : 8, 16);  // zeroed placeholder, counts as unimplemented
    } else if (hle::IsRegistered(name)) {
      rec.handled = true;
      rec.addr = hle::StubFor(name);
    } else if (rec.weak) {
      rec.addr = 0;  // like the Android linker: an unresolved weak import is null
    } else {
      rec.addr = hle::StubFor(name);
    }
    if (!rec.data && rec.addr) hle::EntryFor(name).lib = rec.library;
    r.value = rec.addr;
    r.import_index = m.imports.size();
    m.imports.push_back(std::move(rec));
    return r;
  }

  bool ApplyOne(const Rela& rl, bool& ok) {
    const std::uint32_t type = static_cast<std::uint32_t>(rl.r_info);
    const std::uint32_t symidx = static_cast<std::uint32_t>(rl.r_info >> 32);
    const gaddr where = m.base + rl.r_offset;
    if (rl.r_offset + 8 > m.span) {
      ok = false;
      err = std::format("relocation offset 0x{:x} out of range", rl.r_offset);
      return false;
    }
    const char* tname = RelocName(type);
    if (!tname) {
      ok = false;
      err = std::format("unsupported relocation type {} at 0x{:x}", type, rl.r_offset);
      return false;
    }
    ++m.reloc_counts[tname];
    auto put = [&](std::uint64_t v) { mem::W64(where, v); };
    auto sym = [&]() -> Resolved* {
      if (symidx == 0) return nullptr;
      if (symidx >= resolved.size()) {
        ok = false;
        err = std::format("relocation references symbol {} beyond the table ({})", symidx, resolved.size());
        return nullptr;
      }
      Resolved& r = Resolve(symidx);
      if (r.import_index != static_cast<std::size_t>(-1)) ++m.imports[r.import_index].relocs;
      return &r;
    };
    switch (type) {
      case R_AARCH64_NONE: break;
      case R_AARCH64_RELATIVE: put(m.base + static_cast<std::uint64_t>(rl.r_addend)); break;
      case R_AARCH64_ABS64:
      case R_AARCH64_GLOB_DAT:
      case R_AARCH64_JUMP_SLOT: {
        Resolved* r = sym();
        if (!ok || (symidx && !r)) return false;
        put((r ? r->value : 0) + static_cast<std::uint64_t>(rl.r_addend));
        break;
      }
      case R_AARCH64_TLS_DTPMOD64: put(1); break;  // single module with TLS
      case R_AARCH64_TLS_DTPREL64: {
        Resolved* r = sym();
        if (!ok) return false;
        put((r ? r->value : 0) + static_cast<std::uint64_t>(rl.r_addend));
        break;
      }
      case R_AARCH64_TLS_TPREL64: {
        Resolved* r = sym();
        if (!ok) return false;
        put(tls_block_offset + (r ? r->value : 0) + static_cast<std::uint64_t>(rl.r_addend));
        break;
      }
      case R_AARCH64_TLSDESC: {
        // Descriptor {resolver, argument}: the resolver is `ldr x0, [x0, #8]; ret`, the argument the tp offset.
        Resolved* r = sym();
        if (!ok) return false;
        if (!tlsdesc_stub) {
          tlsdesc_stub = hle::DefineData("__nova_tlsdesc_resolver", 16, 16);
          const std::uint32_t code[2] = {0xF9400400u, 0xD65F03C0u};  // ldr x0,[x0,#8] ; ret
          GuestMemory::Protect(tlsdesc_stub & ~(kPageSize - 1), kPageSize, kProtRWX);
          std::memcpy(g2h(tlsdesc_stub), code, sizeof code);
        }
        put(tlsdesc_stub);
        mem::W64(where + 8, tls_block_offset + (r ? r->value : 0) + static_cast<std::uint64_t>(rl.r_addend));
        break;
      }
      case R_AARCH64_IRELATIVE: {
        const gaddr resolver = m.base + static_cast<std::uint64_t>(rl.r_addend);
        try {
          put(call_guest<std::uint64_t>(resolver, std::uint64_t{0}));
        } catch (const GuestFault& f) {
          ok = false;
          err = std::format("IRELATIVE resolver at 0x{:x} faulted: {}", resolver, f.what());
          return false;
        }
        break;
      }
      case R_AARCH64_COPY:
        ok = false;
        err = "R_AARCH64_COPY in a shared object";
        return false;
      default: break;
    }
    return true;
  }

  bool ApplyRelocations() {
    bool ok = true;
    auto run = [&](gaddr tab, gsize bytes) {
      const auto* r = reinterpret_cast<const Rela*>(g2h(tab));
      for (gsize i = 0; i < bytes / sizeof(Rela); ++i)
        if (!ApplyOne(r[i], ok)) return false;
      return true;
    };
    if (rela && relasz && !run(rela, relasz)) return Fail(err);
    if (jmprel && pltrelsz && !run(jmprel, pltrelsz)) return Fail(err);
    if (relr && relrsz) {
      // RELR: an address entry (even) relocates one word; an odd entry is a bitmap for the 63 following words.
      const auto* e = reinterpret_cast<const std::uint64_t*>(g2h(relr));
      gaddr where = 0;
      for (gsize i = 0; i < relrsz / 8; ++i) {
        if ((e[i] & 1) == 0) {
          where = m.base + e[i];
          mem::W64(where, mem::R64(where) + m.base);
          where += 8;
          ++m.reloc_counts["RELR"];
        } else {
          std::uint64_t bits = e[i] >> 1;
          for (gaddr w = where; bits; bits >>= 1, w += 8)
            if (bits & 1) {
              mem::W64(w, mem::R64(w) + m.base);
              ++m.reloc_counts["RELR"];
            }
          where += 63 * 8;
        }
      }
    }
    return true;
  }

  void Protections() {
    std::vector<std::uint8_t> pages(m.span / kPageSize, 0);
    gaddr lo = ~0ull;
    for (const Segment& s : m.segments) lo = std::min(lo, PageDown(s.vaddr));
    for (const Segment& s : m.segments) {
      const gsize first = (PageDown(s.vaddr) - lo) / kPageSize, last = (PageUp(s.vaddr + s.memsz) - lo) / kPageSize;
      for (gsize p = first; p < last; ++p) pages[p] |= ProtOfFlags(s.flags);
    }
    for (gsize p = 0; p < pages.size();) {
      gsize q = p;
      while (q < pages.size() && pages[q] == pages[p]) ++q;
      GuestMemory::Protect(m.base + lo + p * kPageSize, (q - p) * kPageSize, pages[p]);
      p = q;
    }
    for (const Phdr& p : ph) {
      if (p.p_type != PT_GNU_RELRO) continue;
      const gaddr s = PageDown(m.base + p.p_vaddr), e = PageDown(m.base + p.p_vaddr + p.p_memsz);
      if (e > s) GuestMemory::Protect(s, e - s, kProtRead);
    }
  }
};
}  // namespace

std::shared_ptr<Module> Load(const std::string& path, std::string& err) {
  if (!GuestMemory::Init()) {
    err = "cannot reserve the guest address window";
    return nullptr;
  }
  auto mod = std::make_shared<Module>();
  mod->path = path;
  const auto slash = path.find_last_of("/\\");
  mod->name = slash == std::string::npos ? path : path.substr(slash + 1);
  Loader L(*mod);
  if (!L.ReadFile(path) || !L.ParseHeaders() || !L.MapSegments(path) || !L.ParseDynamic() || !L.SetupPhdrs()) {
    err = L.err;
    return nullptr;
  }
  if (mod->has_tls) {
    if (mod->tls_align > 0x100) {
      err = "PT_TLS alignment above 256 is not supported";
      return nullptr;
    }
    cpu::SetTlsTemplate({mod->tls_image, mod->tls_filesz, mod->tls_memsz, mod->tls_align});
  }
  if (!L.ApplyRelocations()) {
    err = L.err;
    return nullptr;
  }
  L.Protections();
  {
    std::string warm;
    mod->Symbolize(mod->base, warm);  // build the sorted symbol index now: Symbolize must not race later
  }
  {
    std::lock_guard lk(g_mods_mu);
    g_modules.push_back(mod);
  }
  std::weak_ptr<Module> weak = mod;
  debug::AddSymbolizer([weak](gaddr a, std::string& out) {
    auto m = weak.lock();
    return m && m->Symbolize(a, out);
  });
  return mod;
}

InitReport RunInit(const Module& m) {
  InitReport rep;
  std::vector<gaddr> fns;
  if (m.init) fns.push_back(m.init);
  for (std::size_t i = 0; i < m.init_array_count; ++i) {
    const gaddr f = mem::R64(m.init_array + 8 * i);
    fns.push_back(f);
  }
  rep.total = fns.size();
  // argc/argv/envp, as bionic's call_array passes to every constructor.
  const gaddr argv = hle::DefineData("__nova_argv", 32, 16);
  const gaddr arg0 = hle::DefineData("__nova_argv0", 16, 16);
  std::memcpy(g2h(arg0), "nova\0", 5);
  mem::W64(argv, arg0);
  mem::W64(argv + 8, 0);
  mem::W64(argv + 16, 0);
  Cpu& cpu = Cpu::Current();
  for (std::size_t i = 0; i < fns.size(); ++i) {
    const gaddr f = fns[i];
    if (f == 0 || f == ~0ull) {
      ++rep.ran;
      continue;
    }
    try {
      cpu.Call<void>(f, std::uint64_t{1}, argv, argv + 16);
      ++rep.ran;
    } catch (const GuestFault& e) {
      rep.ok = false;
      rep.failed_index = i;
      rep.failed_fn = f;
      rep.failed_sym = debug::addr2sym(f);
      rep.fault = e.what();
      return rep;
    } catch (const GuestExit& e) {
      rep.ok = false;
      rep.failed_index = i;
      rep.failed_fn = f;
      rep.failed_sym = debug::addr2sym(f);
      rep.fault = std::format("constructor called exit({})", e.code);
      return rep;
    }
  }
  return rep;
}

void PrintLoadReport(const Module& m, std::FILE* out) {
  std::fprintf(out, "module %s  base 0x%llx  span 0x%llx (%.1f MiB)\n", m.name.c_str(), (unsigned long long)m.base,
               (unsigned long long)m.span, m.span / 1048576.0);
  std::fprintf(out, "needed:");
  for (const auto& n : m.needed) std::fprintf(out, " %s", n.c_str());
  std::fprintf(out, "\nsegments: %zu\n", m.segments.size());
  for (const Segment& s : m.segments)
    std::fprintf(out, "  PT_LOAD vaddr 0x%08llx filesz 0x%llx memsz 0x%llx  %c%c%c\n", (unsigned long long)s.vaddr,
                 (unsigned long long)s.filesz, (unsigned long long)s.memsz, (s.flags & 4) ? 'R' : '-', (s.flags & 2) ? 'W' : '-',
                 (s.flags & 1) ? 'X' : '-');
  std::size_t total = 0;
  for (const auto& [k, v] : m.reloc_counts) total += v;
  std::fprintf(out, "relocations: %zu total\n", total);
  for (const auto& [k, v] : m.reloc_counts) std::fprintf(out, "  %-14s %zu\n", k.c_str(), v);
  std::fprintf(out, "TLS: %s", m.has_tls ? "yes" : "no PT_TLS");
  if (m.has_tls) std::fprintf(out, " (filesz %llu memsz %llu align %llu)", (unsigned long long)m.tls_filesz, (unsigned long long)m.tls_memsz, (unsigned long long)m.tls_align);
  std::fprintf(out, "\nGNU_EH_FRAME: %s  dynsym: %zu  hash: %s\n", m.eh_frame_hdr ? "yes" : "no", m.nsyms,
               m.hash ? "DT_HASH" : "DT_GNU_HASH");
  std::fprintf(out, "init: DT_INIT %s, DT_INIT_ARRAY %zu entries\n", m.init ? "yes" : "no", m.init_array_count);

  struct Cnt {
    std::size_t total = 0, handled = 0, unimpl = 0, data = 0, weak0 = 0;
  };
  std::map<std::string, Cnt> by_lib;
  Cnt all;
  for (const ImportRecord& r : m.imports) {
    Cnt& c = by_lib[r.library];
    for (Cnt* p : {&c, &all}) {
      ++p->total;
      if (r.data) ++p->data;
      if (r.handled) ++p->handled;
      else if (r.addr == 0) ++p->weak0;
      else ++p->unimpl;
    }
  }
  std::fprintf(out, "imports: %zu total, %zu handled, %zu unimplemented (log once, return 0), %zu weak left null\n", all.total,
               all.handled, all.unimpl, all.weak0);
  for (const auto& [lib, c] : by_lib)
    std::fprintf(out, "  %-12s %4zu total  %4zu handled  %4zu unimplemented%s\n", lib.c_str(), c.total, c.handled, c.unimpl,
                 c.weak0 ? std::format("  {} weak-null", c.weak0).c_str() : "");
}

bool WriteImportList(const Module& m, const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "w");
  if (!f) return false;
  std::fprintf(f, "# name\tlibrary\tkind\tstatus\trelocs\n");
  std::vector<const ImportRecord*> v;
  for (const auto& r : m.imports) v.push_back(&r);
  std::sort(v.begin(), v.end(), [](auto* a, auto* b) { return std::tie(a->library, a->name) < std::tie(b->library, b->name); });
  for (const ImportRecord* r : v)
    std::fprintf(f, "%s\t%s\t%s\t%s\t%zu\n", r->name.c_str(), r->library.c_str(), r->data ? "data" : "func",
                 r->handled ? "handled" : (r->addr ? "unimplemented" : (r->weak ? "weak-null" : "unresolved")), r->relocs);
  std::fclose(f);
  return true;
}
}  // namespace nova::loader
