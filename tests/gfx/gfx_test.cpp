// gfx smoke test: opens a hidden SDL3 window, creates the ANGLE display, then runs an aarch64 guest blob that does
// what libNOVA does at start-up (EGL display/config/surface/context, glClear, a draw with client-side vertex data and
// guest-memory shader strings, eglSwapBuffers) through the HLE thunks, and checks what it read back.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>

#include "core/log.h"
#include "cpu/cpu.h"
#include "cpu/guest_memory.h"
#include "cpu/hle.h"
#include "gfx/gfx.h"
#include "guest_blob.h"

using namespace nova;

namespace {
int g_fail = 0;
void Check(const char* name, bool ok, const std::string& detail = {}) {
  if (!ok) ++g_fail;
  std::printf("%s %-34s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
}
bool Near(std::int64_t px, int r, int g, int b, int a, int tol = 2) {
  const int c[4] = {static_cast<int>(px & 0xFF), static_cast<int>((px >> 8) & 0xFF), static_cast<int>((px >> 16) & 0xFF),
                    static_cast<int>((px >> 24) & 0xFF)};
  const int want[4] = {r, g, b, a};
  for (int i = 0; i < 4; ++i)
    if (std::abs(c[i] - want[i]) > tol) return false;
  return true;
}
}  // namespace

int main() {
  GuestMemory::Init();
  gfx::Config cfg;
  cfg.width = 320;
  cfg.height = 240;
  cfg.hidden = true;
  cfg.vsync = false;
  if (!gfx::Init(cfg)) {
    std::printf("FAIL gfx::Init (window / ANGLE unavailable)\n");
    return 1;
  }
  gfx::RegisterHle();

  const std::size_t sz = sizeof(guest_blob::kBytes);
  GuestMemory::Map(guest_blob::kBase, sz, kProtRW, "gfx-code");
  std::memcpy(g2h(guest_blob::kBase), guest_blob::kBytes, sz);
  GuestMemory::Protect(guest_blob::kBase, sz, kProtRX);
  constexpr gaddr kData = 0x4800'0000ull;
  GuestMemory::Map(kData, 1 << 16, kProtRW, "gfx-data");
  gaddr run = 0;
  for (const auto& s : guest_blob::kSyms)
    if (!std::strcmp(s.name, "run")) run = s.addr;

  // Same order as the S_* enum in guest.c.
  static const char* const kNames[] = {
      "eglGetDisplay", "eglInitialize", "eglChooseConfig", "eglCreateWindowSurface", "eglCreateContext",
      "eglMakeCurrent", "eglSwapBuffers", "eglGetError", "eglQuerySurface", "eglGetProcAddress", "glClearColor",
      "glClear", "glViewport", "glReadPixels", "glGetString", "glGetIntegerv", "glGetError", "glCreateShader",
      "glShaderSource", "glCompileShader", "glGetShaderiv", "glCreateProgram", "glAttachShader", "glLinkProgram",
      "glUseProgram", "glGetAttribLocation", "glEnableVertexAttribArray", "glVertexAttribPointer", "glDrawArrays",
      "glGenBuffers", "glBindBuffer", "glBufferData"};
  const gaddr stubs = kData, out = kData + 0x1000;
  gaddr table[std::size(kNames)];
  for (std::size_t i = 0; i < std::size(kNames); ++i) {
    table[i] = hle::StubFor(kNames[i]);
    if (!hle::IsRegistered(kNames[i])) Check(("registered " + std::string(kNames[i])).c_str(), false);
  }
  std::memcpy(g2h(stubs), table, sizeof table);

  call_guest<void>(run, stubs, out);

  auto O = [&](int i) { return mem::R<std::int64_t>(out + 8 * i); };
  int rw = 0, rh = 0;
  gfx::RenderSize(rw, rh);
  int k = 0;
  Check("eglInitialize", O(k) == 1 && O(k + 1) == 14, std::format("ok={} version={}", O(k), O(k + 1)));
  k += 2;
  Check("eglChooseConfig (+ vendor attribute dropped)", O(k) == 1 && O(k + 1) >= 1, std::format("ok={} n={}", O(k), O(k + 1)));
  k += 2;
  Check("eglMakeCurrent", O(k) == 1);
  ++k;
  Check("eglQuerySurface = render size", O(k) == rw && O(k + 1) == rh, std::format("{}x{} vs {}x{}", O(k), O(k + 1), rw, rh));
  k += 2;
  Check("GL_FRAMEBUFFER_BINDING reads 0", O(k) == 0, std::format("{}", O(k)));
  ++k;
  const std::string ext = mem::CString(static_cast<gaddr>(O(k)));
  const std::string rend = mem::CString(static_cast<gaddr>(O(k + 1)));
  k += 2;
  Check("GL_EXTENSIONS spoofed (ETC1 present)", ext.find("GL_OES_compressed_ETC1_RGB8_texture") != std::string::npos, ext);
  Check("GL_RENDERER spoofed", rend.find("Adreno") != std::string::npos, rend);
  Check("glClear + glReadPixels", Near(O(k), 64, 128, 191, 255), std::format("0x{:08X}", static_cast<std::uint32_t>(O(k))));
  ++k;
  Check("shaders compile (guest source pointers)", O(k) == 1);
  ++k;
  Check("client-array draw", Near(O(k), 255, 0, 0, 255), std::format("0x{:08X}", static_cast<std::uint32_t>(O(k))));
  ++k;
  Check("eglSwapBuffers", O(k) == 1 && O(k + 1) == 0x3000, std::format("ok={} err=0x{:X}", O(k), O(k + 1)));
  k += 2;
  Check("read-back after swap (redirect persists)", Near(O(k), 255, 0, 0, 255), std::format("0x{:08X}", static_cast<std::uint32_t>(O(k))));
  ++k;
  Check("eglGetProcAddress(glMapBufferOES)", static_cast<gaddr>(O(k)) == hle::StubFor("glMapBufferOES") && O(k) != 0);
  Check("eglGetProcAddress(glGenFencesNV) is NULL", O(k + 1) == 0);
  k += 2;
  Check("glMapBufferOES shadow + unmap", O(k) == 1 && O(k + 1) == 1, std::format("map={} unmap={}", O(k), O(k + 1)));
  k += 2;
  Check("no GL error left", O(k) == 0, std::format("0x{:X}", O(k)));

  gfx::Shutdown();
  std::printf("%s\n", g_fail ? "GFX TEST FAILED" : "GFX TEST OK");
  return g_fail ? 1 : 0;
}
