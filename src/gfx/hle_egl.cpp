// libEGL HLE (18 imports + EGL_KHR_fence_sync + eglGetProcAddress). The guest sees one display, ANGLE's configs,
// contexts and surfaces as small opaque handles; contexts and the window surface are real ANGLE objects created from
// the host config (the guest's choice of config only affects what eglGetConfigAttrib reports). The window surface is
// our SDL window: the guest's framebuffer 0 is redirected to an FBO and eglSwapBuffers presents it (gfx.cpp).
#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "cpu/guest_memory.h"
#include "cpu/hle_abi.h"
#include "gfx/gfx_internal.h"

namespace nova::gfx {
namespace {

using detail::Ctx;
using detail::Surf;

constexpr std::uint64_t kDisplayHandle = 0x1D15;
constexpr std::uint64_t kConfigBase = 0xC0F10000;
constexpr std::uint64_t kContextBase = 0xC0C70000;
constexpr std::uint64_t kSurfaceBase = 0xC05F0000;

thread_local EGLint t_err = EGL_SUCCESS;

std::mutex g_mu;
std::vector<EGLConfig> g_configs;  // handle = kConfigBase + index
struct CtxEntry { Ctx* c; bool alive; };
struct SurfEntry { Surf* s; bool alive; };
std::vector<CtxEntry> g_ctxs;    // handle = kContextBase + index
std::vector<SurfEntry> g_surfs;  // handle = kSurfaceBase + index

std::uint32_t Fail(EGLint e) {
  t_err = e;
  return EGL_FALSE;
}
std::uint32_t Ok() {
  t_err = EGL_SUCCESS;
  return EGL_TRUE;
}
bool BadDisplay(std::uint64_t d) { return d != kDisplayHandle || !IsInitialized(); }

std::uint64_t ConfigHandle(EGLConfig c) {
  std::lock_guard lk(g_mu);
  for (std::size_t i = 0; i < g_configs.size(); ++i)
    if (g_configs[i] == c) return kConfigBase + i;
  g_configs.push_back(c);
  return kConfigBase + g_configs.size() - 1;
}
EGLConfig ConfigFromHandle(std::uint64_t h) {
  std::lock_guard lk(g_mu);
  if (h < kConfigBase || h - kConfigBase >= g_configs.size()) return nullptr;
  return g_configs[h - kConfigBase];
}
Ctx* CtxFromHandle(std::uint64_t h) {
  std::lock_guard lk(g_mu);
  if (h < kContextBase || h - kContextBase >= g_ctxs.size() || !g_ctxs[h - kContextBase].alive) return nullptr;
  return g_ctxs[h - kContextBase].c;
}
std::uint64_t HandleOfCtx(const Ctx* c) {
  std::lock_guard lk(g_mu);
  for (std::size_t i = 0; i < g_ctxs.size(); ++i)
    if (g_ctxs[i].alive && g_ctxs[i].c == c) return kContextBase + i;
  return 0;
}
Surf* SurfFromHandle(std::uint64_t h) {
  std::lock_guard lk(g_mu);
  if (h < kSurfaceBase || h - kSurfaceBase >= g_surfs.size() || !g_surfs[h - kSurfaceBase].alive) return nullptr;
  return g_surfs[h - kSurfaceBase].s;
}

// EGL config attributes of EGL 1.4 that ANGLE knows. Android guests add vendor ones (EGL_RECORDABLE_ANDROID, ...)
// that make ANGLE fail with EGL_BAD_ATTRIBUTE; those are dropped.
bool KnownConfigAttrib(EGLint a) {
  return (a >= 0x3020 && a <= 0x3042 && a != 0x3038) || a == 0x3138 /*EGL_MATCH_NATIVE_PIXMAP*/;
}

// ---------------------------------------------------------------- display and configs
std::uint64_t h_eglGetDisplay(std::uint64_t /*native_display*/) {
  t_err = EGL_SUCCESS;
  return IsInitialized() ? kDisplayHandle : 0;
}
std::uint32_t h_eglInitialize(std::uint64_t dpy, std::int32_t* major, std::int32_t* minor) {
  if (BadDisplay(dpy)) return Fail(EGL_BAD_DISPLAY);
  if (major) *major = 1;
  if (minor) *minor = 4;
  return Ok();
}
std::uint32_t h_eglTerminate(std::uint64_t dpy) {
  if (BadDisplay(dpy)) return Fail(EGL_BAD_DISPLAY);
  return Ok();  // the ANGLE display lives as long as the window; contexts/surfaces are destroyed by the guest
}

std::uint32_t h_eglChooseConfig(std::uint64_t dpy, const std::int32_t* attrib_list, std::uint64_t* configs,
                                std::int32_t config_size, std::int32_t* num_config) {
  if (BadDisplay(dpy)) return Fail(EGL_BAD_DISPLAY);
  if (!num_config) return Fail(EGL_BAD_PARAMETER);
  std::vector<EGLint> attrs;
  for (const std::int32_t* p = attrib_list; p && *p != EGL_NONE; p += 2) {
    if (KnownConfigAttrib(p[0])) {
      attrs.push_back(p[0]);
      attrs.push_back(p[1]);
    } else {
      log::Debug("[egl] eglChooseConfig: dropping attribute 0x{:04X}", p[0]);
    }
  }
  attrs.push_back(EGL_NONE);
  if (!configs || config_size <= 0) {
    EGLint n = 0;
    if (!eglChooseConfig(detail::Display(), attrs.data(), nullptr, 0, &n)) return Fail(eglGetError());
    *num_config = n;
    return Ok();
  }
  std::vector<EGLConfig> real(static_cast<std::size_t>(config_size));
  EGLint n = 0;
  if (!eglChooseConfig(detail::Display(), attrs.data(), real.data(), config_size, &n)) return Fail(eglGetError());
  for (EGLint i = 0; i < n; ++i) configs[i] = ConfigHandle(real[static_cast<std::size_t>(i)]);
  *num_config = n;
  return Ok();
}

std::uint32_t h_eglGetConfigAttrib(std::uint64_t dpy, std::uint64_t config, std::int32_t attribute, std::int32_t* value) {
  if (BadDisplay(dpy)) return Fail(EGL_BAD_DISPLAY);
  EGLConfig c = ConfigFromHandle(config);
  if (!c) return Fail(EGL_BAD_CONFIG);
  if (!value) return Fail(EGL_BAD_PARAMETER);
  if (!eglGetConfigAttrib(detail::Display(), c, attribute, value)) return Fail(eglGetError());
  return Ok();
}

// ---------------------------------------------------------------- contexts and surfaces
std::uint64_t h_eglCreateContext(std::uint64_t dpy, std::uint64_t config, std::uint64_t share, const std::int32_t* attribs) {
  if (BadDisplay(dpy)) return Fail(EGL_BAD_DISPLAY), 0;
  if (!ConfigFromHandle(config)) return Fail(EGL_BAD_CONFIG), 0;
  int version = 1;
  for (const std::int32_t* p = attribs; p && *p != EGL_NONE; p += 2)
    if (p[0] == EGL_CONTEXT_CLIENT_VERSION) version = p[1];
  Ctx* shr = share ? CtxFromHandle(share) : nullptr;
  if (share && !shr) return Fail(EGL_BAD_CONTEXT), 0;
  Ctx* c = detail::CreateContext(shr, version);
  if (!c) return Fail(EGL_BAD_ALLOC), 0;
  std::lock_guard lk(g_mu);
  g_ctxs.push_back({c, true});
  t_err = EGL_SUCCESS;
  return kContextBase + g_ctxs.size() - 1;
}

std::uint32_t h_eglDestroyContext(std::uint64_t dpy, std::uint64_t ctx) {
  if (BadDisplay(dpy)) return Fail(EGL_BAD_DISPLAY);
  Ctx* c = CtxFromHandle(ctx);
  if (!c) return Fail(EGL_BAD_CONTEXT);
  {
    std::lock_guard lk(g_mu);
    g_ctxs[ctx - kContextBase].alive = false;
  }
  detail::DestroyContext(c);
  return Ok();
}

std::uint64_t AddSurface(Surf* s) {
  std::lock_guard lk(g_mu);
  g_surfs.push_back({s, true});
  t_err = EGL_SUCCESS;
  return kSurfaceBase + g_surfs.size() - 1;
}

std::uint64_t h_eglCreateWindowSurface(std::uint64_t dpy, std::uint64_t config, std::uint64_t native_window,
                                       const std::int32_t* /*attribs*/) {
  if (BadDisplay(dpy)) return Fail(EGL_BAD_DISPLAY), 0;
  if (!ConfigFromHandle(config)) return Fail(EGL_BAD_CONFIG), 0;
  if (!native_window) return Fail(EGL_BAD_NATIVE_WINDOW), 0;
  Surf* s = detail::AcquireWindowSurface();
  if (!s) return Fail(EGL_BAD_ALLOC), 0;
  return AddSurface(s);
}

std::uint64_t h_eglCreatePbufferSurface(std::uint64_t dpy, std::uint64_t config, const std::int32_t* attribs) {
  if (BadDisplay(dpy)) return Fail(EGL_BAD_DISPLAY), 0;
  if (!ConfigFromHandle(config)) return Fail(EGL_BAD_CONFIG), 0;
  Surf* s = detail::CreatePbuffer(attribs);
  if (!s) return Fail(eglGetError()), 0;
  return AddSurface(s);
}

std::uint32_t h_eglDestroySurface(std::uint64_t dpy, std::uint64_t surf) {
  if (BadDisplay(dpy)) return Fail(EGL_BAD_DISPLAY);
  Surf* s = SurfFromHandle(surf);
  if (!s) return Fail(EGL_BAD_SURFACE);
  {
    std::lock_guard lk(g_mu);
    g_surfs[surf - kSurfaceBase].alive = false;
  }
  if (s->window) detail::ReleaseWindowSurface(s);
  else detail::DestroyPbuffer(s);
  return Ok();
}

std::uint32_t h_eglMakeCurrent(std::uint64_t dpy, std::uint64_t draw, std::uint64_t /*read*/, std::uint64_t ctx) {
  if (BadDisplay(dpy)) return Fail(EGL_BAD_DISPLAY);
  if (!ctx) {
    if (!detail::MakeCurrent(nullptr, nullptr)) return Fail(eglGetError());
    return Ok();
  }
  Ctx* c = CtxFromHandle(ctx);
  if (!c) return Fail(EGL_BAD_CONTEXT);
  Surf* s = nullptr;
  if (draw) {
    s = SurfFromHandle(draw);
    if (!s) return Fail(EGL_BAD_SURFACE);
  }
  if (!detail::MakeCurrent(c, s)) {
    const EGLint e = eglGetError();
    return Fail(e == EGL_SUCCESS ? EGL_BAD_MATCH : e);
  }
  return Ok();
}

std::uint32_t h_eglSwapBuffers(std::uint64_t dpy, std::uint64_t surf) {
  if (BadDisplay(dpy)) return Fail(EGL_BAD_DISPLAY);
  Surf* s = SurfFromHandle(surf);
  if (!s) return Fail(EGL_BAD_SURFACE);
  if (!detail::CurrentCtx()) return Fail(EGL_BAD_CONTEXT);
  detail::Present(s);
  return Ok();
}

std::uint32_t h_eglQuerySurface(std::uint64_t dpy, std::uint64_t surf, std::int32_t attribute, std::int32_t* value) {
  if (BadDisplay(dpy)) return Fail(EGL_BAD_DISPLAY);
  Surf* s = SurfFromHandle(surf);
  if (!s) return Fail(EGL_BAD_SURFACE);
  if (!value) return Fail(EGL_BAD_PARAMETER);
  if (s->window && (attribute == EGL_WIDTH || attribute == EGL_HEIGHT)) {
    int w = 0, h = 0;
    RenderSize(w, h);
    *value = attribute == EGL_WIDTH ? w : h;
    return Ok();
  }
  if (!eglQuerySurface(detail::Display(), s->real, attribute, value)) return Fail(eglGetError());
  return Ok();
}

std::uint64_t h_eglQueryString(std::uint64_t dpy, std::int32_t name) {
  if (BadDisplay(dpy) && dpy != 0) return Fail(EGL_BAD_DISPLAY), 0;
  t_err = EGL_SUCCESS;
  switch (name) {
    case EGL_VENDOR: return detail::GuestString("Android");
    case EGL_VERSION: return detail::GuestString("1.4 Android META-EGL");
    case EGL_EXTENSIONS: return detail::GuestString("EGL_KHR_fence_sync EGL_KHR_surfaceless_context");
    case EGL_CLIENT_APIS: return detail::GuestString("OpenGL_ES");
    default: t_err = EGL_BAD_PARAMETER; return 0;
  }
}

std::uint64_t h_eglGetCurrentContext() {
  Ctx* c = detail::CurrentCtx();
  return c ? HandleOfCtx(c) : 0;
}
std::uint64_t h_eglGetCurrentDisplay() { return detail::CurrentCtx() ? kDisplayHandle : 0; }
std::int32_t h_eglGetError() {
  const EGLint e = t_err;
  t_err = EGL_SUCCESS;
  return e;
}

// ---------------------------------------------------------------- EGL_KHR_fence_sync (handles are ANGLE's own)
std::uint64_t h_eglCreateSyncKHR(std::uint64_t dpy, std::uint32_t type, const std::int32_t* attribs) {
  if (BadDisplay(dpy)) return Fail(EGL_BAD_DISPLAY), 0;
  static auto fn = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(eglGetProcAddress("eglCreateSyncKHR"));
  if (!fn) return Fail(EGL_BAD_ALLOC), 0;
  EGLSyncKHR s = fn(detail::Display(), type, attribs);
  if (!s) return Fail(eglGetError()), 0;
  t_err = EGL_SUCCESS;
  return reinterpret_cast<std::uint64_t>(s);
}
std::uint32_t h_eglDestroySyncKHR(std::uint64_t dpy, std::uint64_t sync) {
  if (BadDisplay(dpy)) return Fail(EGL_BAD_DISPLAY);
  static auto fn = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(eglGetProcAddress("eglDestroySyncKHR"));
  if (!fn || !fn(detail::Display(), reinterpret_cast<EGLSyncKHR>(sync))) return Fail(eglGetError());
  return Ok();
}
std::int32_t h_eglClientWaitSyncKHR(std::uint64_t dpy, std::uint64_t sync, std::int32_t flags, std::uint64_t timeout) {
  if (BadDisplay(dpy)) return Fail(EGL_BAD_DISPLAY), 0;
  static auto fn = reinterpret_cast<PFNEGLCLIENTWAITSYNCKHRPROC>(eglGetProcAddress("eglClientWaitSyncKHR"));
  if (!fn) return Fail(EGL_BAD_ALLOC), 0;
  t_err = EGL_SUCCESS;
  return fn(detail::Display(), reinterpret_cast<EGLSyncKHR>(sync), flags, timeout);
}
std::uint32_t h_eglGetSyncAttribKHR(std::uint64_t dpy, std::uint64_t sync, std::int32_t attribute, std::int32_t* value) {
  if (BadDisplay(dpy)) return Fail(EGL_BAD_DISPLAY);
  static auto fn = reinterpret_cast<PFNEGLGETSYNCATTRIBKHRPROC>(eglGetProcAddress("eglGetSyncAttribKHR"));
  if (!fn || !value || !fn(detail::Display(), reinterpret_cast<EGLSyncKHR>(sync), attribute, value)) return Fail(eglGetError());
  return Ok();
}

// ---------------------------------------------------------------- eglGetProcAddress
// Hands back the stub of any registered gl*/egl* import: the core functions and every extension alias hle_gles.cpp
// registers (ANGLE implements them). Names we did not register return NULL, so the engine takes its core-ES2 path.
std::uint64_t h_eglGetProcAddress(const char* name) {
  if (!name) return 0;
  const std::string n = name;
  if (n.rfind("gl", 0) != 0 && n.rfind("egl", 0) != 0) return 0;
  if (!hle::IsRegistered(n)) {
    static std::mutex mu;
    static std::vector<std::string> seen;
    std::lock_guard lk(mu);
    if (std::find(seen.begin(), seen.end(), n) == seen.end()) {
      seen.push_back(n);
      log::Debug("[egl] eglGetProcAddress(\"{}\") -> NULL", n);
    }
    return 0;
  }
  return hle::StubFor(n);
}

}  // namespace

namespace detail {

std::uint64_t GuestString(const std::string& s) {
  static std::mutex mu;
  static std::unordered_map<std::string, std::uint64_t> cache;
  static gaddr arena = 0, used = 0, cap = 0;
  std::lock_guard lk(mu);
  if (auto it = cache.find(s); it != cache.end()) return it->second;
  const gsize need = s.size() + 1;
  if (!arena || used + need > cap) {
    cap = std::max<gsize>(0x4000, (need + 0xFFF) & ~gsize{0xFFF});
    arena = GuestMemory::MapAnywhere(Zone::Anon, cap, kProtRW, "gfx-strings");
    used = 0;
    if (!arena) return 0;
  }
  const gaddr at = arena + used;
  std::memcpy(g2h(at), s.c_str(), need);
  used += (need + 7) & ~gsize{7};
  cache.emplace(s, at);
  return at;
}

void RegisterEgl() {
  NOVA_HLE("eglGetDisplay", h_eglGetDisplay);
  NOVA_HLE("eglInitialize", h_eglInitialize);
  NOVA_HLE("eglTerminate", h_eglTerminate);
  NOVA_HLE("eglChooseConfig", h_eglChooseConfig);
  NOVA_HLE("eglGetConfigAttrib", h_eglGetConfigAttrib);
  NOVA_HLE("eglCreateContext", h_eglCreateContext);
  NOVA_HLE("eglDestroyContext", h_eglDestroyContext);
  NOVA_HLE("eglCreateWindowSurface", h_eglCreateWindowSurface);
  NOVA_HLE("eglCreatePbufferSurface", h_eglCreatePbufferSurface);
  NOVA_HLE("eglDestroySurface", h_eglDestroySurface);
  NOVA_HLE("eglMakeCurrent", h_eglMakeCurrent);
  NOVA_HLE("eglSwapBuffers", h_eglSwapBuffers);
  NOVA_HLE("eglQuerySurface", h_eglQuerySurface);
  NOVA_HLE("eglQueryString", h_eglQueryString);
  NOVA_HLE("eglGetCurrentContext", h_eglGetCurrentContext);
  NOVA_HLE("eglGetCurrentDisplay", h_eglGetCurrentDisplay);
  NOVA_HLE("eglGetError", h_eglGetError);
  NOVA_HLE("eglGetProcAddress", h_eglGetProcAddress);
  NOVA_HLE("eglCreateSyncKHR", h_eglCreateSyncKHR);
  NOVA_HLE("eglDestroySyncKHR", h_eglDestroySyncKHR);
  NOVA_HLE("eglClientWaitSyncKHR", h_eglClientWaitSyncKHR);
  NOVA_HLE("eglGetSyncAttribKHR", h_eglGetSyncAttribKHR);
}

}  // namespace detail
}  // namespace nova::gfx
