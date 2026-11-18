// AAssetManager over the real APK: a deflated entry (CRC-checked inflate) through the ZIP reader, then the HLE handlers
// (open / getLength / read / seek / getBuffer / close, openDir) called the way the SVC dispatcher calls them.
// Skipped (exit 0) when the APK is not at NOVA_APK or the default game/ path.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "android/android.h"
#include "android/zip.h"
#include "cpu/cpu.h"
#include "cpu/guest_memory.h"
#include "cpu/hle.h"

using namespace nova;

namespace {
int g_fail = 0;
void Check(const char* name, bool ok, const std::string& detail = {}) {
  if (!ok) ++g_fail;
  std::printf("%s %-34s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
}

std::uint64_t Call(const char* import, std::uint64_t a0 = 0, std::uint64_t a1 = 0, std::uint64_t a2 = 0) {
  Cpu& c = Cpu::Current();
  c.set_x(0, a0);
  c.set_x(1, a1);
  c.set_x(2, a2);
  hle::Entry* e = hle::FindEntry(import);
  e->fn.load()(c);
  return c.x(0);
}
}  // namespace

int main() {
  std::string apk = std::getenv("NOVA_APK") ? std::getenv("NOVA_APK") : "game/N.O.V.A._Legacy-5.8.4a.apk";
  if (!std::filesystem::exists(apk)) {
    std::printf("SKIP: no APK at %s\n", apk.c_str());
    return 0;
  }
  GuestMemory::Init();

  auto zip = android::ZipArchive::Open(apk);
  Check("zip opens", zip != nullptr);
  if (!zip) return 1;
  const auto* e = zip->Find("assets/GO_android.json");
  Check("deflated entry found", e && e->method == 8 && e->usize == 195178);
  auto data = e ? zip->Load(*e) : nullptr;  // inflate + CRC32 against the central directory
  Check("deflated entry inflates, CRC ok", data && data->size() == 195178 && (*data)[0] == '{');

  android::SetApkPath(apk);
  android::RegisterHle();
  const gaddr mgr = android::AssetManagerHandle();
  Check("fromJava returns the handle", Call("AAssetManager_fromJava", 0, 0) == mgr && mgr != 0);

  const gaddr names = hle::DefineData("__test_names", 256, 16);
  std::strcpy(g2h<char>(names), "REVISION.txt");
  std::strcpy(g2h<char>(names + 64), "GO_android.json");
  std::strcpy(g2h<char>(names + 128), "no/such/file");
  std::strcpy(g2h<char>(names + 192), "raw");
  const gaddr buf = hle::DefineData("__test_buf", 4096, 16);

  const std::uint64_t a = Call("AAssetManager_open", mgr, names, 2);
  Check("open stored asset", a != 0);
  Check("getLength", Call("AAsset_getLength", a) == 19);
  const std::int32_t n = static_cast<std::int32_t>(Call("AAsset_read", a, buf, 4096));
  Check("read", n == 19, std::to_string(n) + " " + std::string(g2h<char>(buf), n > 0 ? n : 0));
  Check("read at end is 0", static_cast<std::int32_t>(Call("AAsset_read", a, buf, 16)) == 0);
  Check("seek to 6", static_cast<std::int64_t>(Call("AAsset_seek", a, 6, 0)) == 6);
  Check("seek past the end fails", static_cast<std::int64_t>(Call("AAsset_seek", a, 1, 2)) == -1);
  const gaddr whole = Call("AAsset_getBuffer", a);
  Check("getBuffer", whole && std::memcmp(g2h(whole), g2h(buf), 19) == 0);
  Call("AAsset_close", a);

  const std::uint64_t j = Call("AAssetManager_open", mgr, names + 64, 3);
  Check("open deflated asset", j != 0 && Call("AAsset_getLength", j) == 195178);
  Check("deflated read", static_cast<std::int32_t>(Call("AAsset_read", j, buf, 1)) == 1 && g2h(buf)[0] == '{');
  Call("AAsset_close", j);

  Check("missing asset -> NULL", Call("AAssetManager_open", mgr, names + 128, 0) == 0);

  const std::uint64_t d = Call("AAssetManager_openDir", mgr, names + 192);
  const gaddr first = Call("AAssetDir_getNextFileName", d);
  Check("openDir lists files", d != 0 && first != 0, first ? g2h<char>(first) : "(none)");
  Call("AAssetDir_close", d);

  std::printf("%s\n", g_fail ? "ASSETS TEST FAILED" : "ASSETS TEST OK");
  return g_fail ? 1 : 0;
}
