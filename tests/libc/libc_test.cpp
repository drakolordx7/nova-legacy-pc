// libc HLE test: a freestanding AArch64 guest (guest.c) calls bionic imports through their stubs. Covers threads +
// mutex + cond + keys + semaphores, stdio/files/directories on the VFS (temp data root), printf/scanf/strtod/qsort,
// libm, time, wide characters, and setjmp/longjmp; plus the software long double conversion on the host side.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "core/log.h"
#include "cpu/cpu.h"
#include "cpu/guest_memory.h"
#include "cpu/hle.h"
#include "hle/format.h"
#include "hle/libc.h"
#include "hle/vfs.h"
#include "guest_blob.h"

namespace nova::hle::libc {
Vec128 DoubleToQuad(double d);
}

using namespace nova;

namespace {
const char* const kNames[] = {
#define X(n) #n,
#include "names.h"
#undef X
};
constexpr std::size_t kCount = sizeof(kNames) / sizeof(kNames[0]);

int g_fail = 0;
void Check(const char* name, bool ok, const std::string& detail = {}) {
  if (!ok) ++g_fail;
  std::printf("%s %-34s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
  std::fflush(stdout);
}
gaddr Sym(const char* name) {
  for (const auto& s : guest_blob::kSyms)
    if (!std::strcmp(s.name, name)) return s.addr;
  return 0;
}
}  // namespace

int main() {
  GuestMemory::Init();
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / ("nova_libc_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  vfs::SetRoot(root);
  hle::libc::RegisterAll();

  const std::size_t sz = sizeof(guest_blob::kBytes);
  GuestMemory::Map(guest_blob::kBase, sz, kProtRW, "libc-code");
  std::memcpy(g2h(guest_blob::kBase), guest_blob::kBytes, sz);
  GuestMemory::Protect(guest_blob::kBase, sz, kProtRX);
  constexpr gaddr kData = 0x4800'0000ull;
  GuestMemory::Map(kData, 8 << 16, kProtRW, "libc-data");

  const gaddr stubs = kData;  // table of kCount stub addresses
  for (std::size_t i = 0; i < kCount; ++i) mem::W64(stubs + 8 * i, hle::StubFor(kNames[i]));

  struct Case {
    const char* fn;
    const char* label;
  } cases[] = {
      {"test_threads", "threads, mutex, cond, keys, sem"},
      {"test_stdio", "stdio + files + dirs on the VFS"},
      {"test_format", "printf/scanf/strtod/qsort/libm/time/wide"},
      {"test_setjmp", "setjmp / longjmp"},
  };
  int idx = 1;
  for (const Case& c : cases) {
    const gaddr fn = Sym(c.fn);
    const gaddr scratch = kData + 0x10000 * idx++;
    if (!fn) {
      Check(c.label, false, "guest symbol missing");
      continue;
    }
    const std::int64_t r = call_guest<std::int64_t>(fn, stubs, scratch);
    Check(c.label, static_cast<std::int32_t>(r) == 0, r ? "first failed check #" + std::to_string(static_cast<std::int32_t>(r)) : "");
  }

  // The VFS mapped the guest save directory under the data root.
  Check("vfs maps /data/data to <root>/internal",
        fs::exists(root / "internal" / "files"), (root / "internal" / "files").string());
  Check("vfs: unmapped path has no host path", !vfs::ToHost("/etc/passwd").has_value());
  Check("vfs: external alias", vfs::ToHost("/sdcard/Android/data/com.gameloft.android.ANMP.GloftNOHM/files/x.dat") ==
                                   (root / "external" / "files" / "x.dat"));
  Check("vfs: ../ relative to the save dir", vfs::Normalize("../Documents/profileall.json") ==
                                                 "/data/data/com.gameloft.android.ANMP.GloftNOHM/Documents/profileall.json");

  // long double (binary128) conversion is done in software
  {
    const Vec128 one = hle::libc::DoubleToQuad(1.0);
    Check("quad: 1.0 bit pattern", one[0] == 0 && one[1] == 0x3FFF000000000000ull);
    bool ok = true;
    for (double v : {0.5, -2.75, 3.141592653589793, 1e300, -1e-300, 5e-324, 0.0})
      ok = ok && hle::QuadToDouble(hle::libc::DoubleToQuad(v)) == v;
    Check("quad: double round trip", ok);
  }

  std::error_code ec;
  fs::remove_all(root, ec);
  std::printf("%s\n", g_fail ? "LIBC TEST FAILED" : "LIBC TEST OK");
  return g_fail ? 1 : 0;
}
