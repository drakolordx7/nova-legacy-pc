// ELF64 (AArch64, ET_DYN) loader: maps PT_LOAD segments into the guest window, applies RELA/RELR relocations, binds
// every undefined import to its HLE stub, sets up TLS, keeps the data dl_iterate_phdr needs, runs constructors.
// Adapted from Zombiewood's loader/loader (MIT, (c) 2026 ChrisDutary): ELF64, RELA, GNU hash, PT_TLS.
#pragma once
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "cpu/guest_memory.h"

namespace nova::loader {

struct Segment {
  gaddr vaddr = 0;  // relative to the module base
  gsize memsz = 0, filesz = 0, offset = 0;
  std::uint32_t flags = 0;  // PF_X=1 PF_W=2 PF_R=4
};

struct ImportRecord {
  std::string name;
  std::string library;  // classified by name (libc, libm, libdl, libz, liblog, libGLESv2, libEGL, libandroid, libOpenSLES)
  bool data = false;    // an object (STT_OBJECT / defined with DefineData) rather than a function
  bool weak = false;
  bool handled = false;  // a handler is registered / the data symbol was defined by the HLE
  std::size_t relocs = 0;
  gaddr addr = 0;  // stub or data address bound (0 for an unresolved weak import)
};

struct Module {
  std::string name;  // soname if present, else file name
  std::string path;
  gaddr base = 0;    // load bias (guest address of vaddr 0)
  gsize span = 0;    // bytes mapped
  std::vector<Segment> segments;
  std::vector<std::string> needed;
  std::map<std::string, std::size_t> reloc_counts;  // by relocation type name
  std::vector<ImportRecord> imports;

  gaddr entry = 0, dynamic = 0;
  gaddr init = 0, fini = 0;
  gaddr init_array = 0, fini_array = 0, preinit_array = 0;
  std::size_t init_array_count = 0, fini_array_count = 0, preinit_array_count = 0;

  // dl_iterate_phdr data.
  gaddr phdr = 0;  // guest address of the program headers
  unsigned phnum = 0;
  gaddr eh_frame_hdr = 0;  // PT_GNU_EH_FRAME
  gsize eh_frame_hdr_size = 0;
  gaddr name_addr = 0;  // guest C string of `name` (dlpi_name)

  // PT_TLS (none in libNOVA.so, but supported).
  bool has_tls = false;
  gaddr tls_image = 0;
  gsize tls_filesz = 0, tls_memsz = 0, tls_align = 1;

  // Exported, defined symbol (function or object) -> guest address; 0 if absent.
  gaddr Lookup(std::string_view sym) const;
  // "name!symbol+0xN" for an address inside the module; false if outside / no symbol.
  bool Symbolize(gaddr addr, std::string& out) const;
  bool Contains(gaddr addr) const { return addr >= base && addr < base + span; }

  // Dynamic symbol table (guest addresses), set by the loader.
  gaddr symtab = 0, strtab = 0;
  std::size_t nsyms = 0;
  gaddr hash = 0, gnu_hash = 0;

 private:
  mutable std::vector<std::pair<gaddr, std::pair<std::uint32_t, std::uint32_t>>> sorted_;  // addr -> (size, name offset)
  mutable bool sorted_built_ = false;
};

// Loads `path` into the guest window. On failure returns null and fills `err`. Register the HLE handlers first:
// they decide which imports count as handled. The module stays registered (Modules()) and symbolised.
std::shared_ptr<Module> Load(const std::string& path, std::string& err);
const std::vector<std::shared_ptr<Module>>& Modules();

struct InitReport {
  std::size_t total = 0;       // DT_INIT (if any) + DT_INIT_ARRAY entries
  std::size_t ran = 0;         // completed
  bool ok = true;
  std::size_t failed_index = 0;
  gaddr failed_fn = 0;
  std::string failed_sym;
  std::string fault;           // GuestFault text / reason
};
// Runs DT_INIT then DT_INIT_ARRAY on the calling thread's Cpu; stops at the first constructor that faults.
InitReport RunInit(const Module& m);

// Library an import name belongs to ("libc" when nothing more specific matches).
std::string ClassifyImport(std::string_view name);

// Report helpers for --load-only.
void PrintLoadReport(const Module& m, std::FILE* out);
bool WriteImportList(const Module& m, const std::string& path);

}  // namespace nova::loader
