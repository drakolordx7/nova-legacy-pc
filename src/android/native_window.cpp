// libandroid ANativeWindow_* HLE. The guest's ANativeWindow is an opaque block in guest memory that stands for our SDL
// window; its width/height are the size of the guest's framebuffer 0 (gfx::RenderSize), which is the window's pixel size
// at startup unless the host changed it. Compiled into nova_gfx (it needs the window API) and registered by
// gfx::RegisterHle().
#include <mutex>

#include "core/log.h"
#include "cpu/guest_memory.h"
#include "cpu/hle_abi.h"
#include "gfx/gfx.h"
#include "gfx/gfx_internal.h"

namespace nova::android {
namespace {

constexpr std::int32_t kWindowFormatRgba8888 = 1;  // WINDOW_FORMAT_RGBA_8888

std::mutex g_mu;
gaddr g_window = 0;   // the one ANativeWindow
int g_refs = 0;
std::int32_t g_format = kWindowFormatRgba8888;

gaddr Window() {
  if (!g_window) {
    g_window = GuestMemory::MapAnywhere(Zone::Anon, kPageSize, kProtRW, "ANativeWindow");
    if (!g_window) log::Error("[android] cannot allocate the ANativeWindow object");
  }
  return g_window;
}

void Size(int& w, int& h) {
  if (gfx::IsInitialized()) {
    gfx::RenderSize(w, h);
  } else {
    w = 1280;
    h = 720;
  }
}

// (JNIEnv*, jobject surface) -> ANativeWindow*, acquired.
std::uint64_t h_ANativeWindow_fromSurface(std::uint64_t /*env*/, std::uint64_t /*surface*/) {
  std::lock_guard lk(g_mu);
  const gaddr w = Window();
  if (w) ++g_refs;
  return w;
}
void h_ANativeWindow_acquire(std::uint64_t /*window*/) {
  std::lock_guard lk(g_mu);
  ++g_refs;
}
void h_ANativeWindow_release(std::uint64_t /*window*/) {
  std::lock_guard lk(g_mu);
  if (g_refs > 0) --g_refs;
}
std::int32_t h_ANativeWindow_getWidth(std::uint64_t /*window*/) {
  int w = 0, h = 0;
  Size(w, h);
  return w;
}
std::int32_t h_ANativeWindow_getHeight(std::uint64_t /*window*/) {
  int w = 0, h = 0;
  Size(w, h);
  return h;
}
std::int32_t h_ANativeWindow_getFormat(std::uint64_t /*window*/) { return g_format; }
// Android: a non-zero size fixes the buffer size and the compositor scales it to the window, which is exactly what
// the framebuffer-0 redirect does; 0 keeps the native size.
std::int32_t h_ANativeWindow_setBuffersGeometry(std::uint64_t /*window*/, std::int32_t w, std::int32_t h, std::int32_t format) {
  if (format > 0) g_format = format;
  if (w > 0 && h > 0 && gfx::IsInitialized()) {
    int cw = 0, ch = 0;
    gfx::RenderSize(cw, ch);
    if (cw != w || ch != h) {
      log::Info("[android] ANativeWindow_setBuffersGeometry {}x{} (was {}x{})", w, h, cw, ch);
      gfx::SetRenderSize(w, h);
    }
  }
  return 0;
}

}  // namespace

void RegisterNativeWindow() {
  NOVA_HLE("ANativeWindow_fromSurface", h_ANativeWindow_fromSurface);
  NOVA_HLE("ANativeWindow_acquire", h_ANativeWindow_acquire);
  NOVA_HLE("ANativeWindow_release", h_ANativeWindow_release);
  NOVA_HLE_FAST("ANativeWindow_getWidth", h_ANativeWindow_getWidth);
  NOVA_HLE_FAST("ANativeWindow_getHeight", h_ANativeWindow_getHeight);
  NOVA_HLE_FAST("ANativeWindow_getFormat", h_ANativeWindow_getFormat);
  NOVA_HLE("ANativeWindow_setBuffersGeometry", h_ANativeWindow_setBuffersGeometry);
}

}  // namespace nova::android
