// SDL3 window + ANGLE (D3D11) display, contexts, the framebuffer-0 redirect and the present path.
// Adapted from Zombiewood's frontend/{window,game}.cpp (MIT, (c) 2026 ChrisDutary): SDL2 became SDL3, and the
// window is driven through ANGLE's EGL directly (HWND surface) instead of SDL's GL wrapper, so the swap interval,
// the surface and the contexts are under our control.
#include <SDL3/SDL.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "core/log.h"
#include "gfx/gfx_internal.h"
#include "hle/vfs.h"

#ifndef EGL_CONTEXT_MAJOR_VERSION
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_CONTEXT_MINOR_VERSION 0x30FB
#endif

namespace nova::gfx {
namespace {

using Clock = std::chrono::steady_clock;

struct State {
  bool inited = false;
  SDL_Window* window = nullptr;
  void* hwnd = nullptr;
  EGLDisplay dpy = EGL_NO_DISPLAY;
  EGLConfig cfg = nullptr;
  bool prefer_es3 = true;
  bool warp = false;
  WindowMode mode = WindowMode::Windowed;
  int display_index = 0;
  bool relative_mouse = false;
  std::mutex mu;                 // window surface record + host context
  detail::Surf* window_surf = nullptr;
  detail::Ctx* host_ctx = nullptr;
  detail::Surf* host_surf = nullptr;
};
State g;

std::atomic<bool> g_vsync{true};
std::atomic<int> g_target_fps{0};
std::atomic<int> g_render_w{0}, g_render_h{0};
std::atomic<int> g_pix_w{0}, g_pix_h{0};
std::atomic<bool> g_close{false};

thread_local detail::Ctx* t_ctx = nullptr;
thread_local detail::Surf* t_draw = nullptr;

const char* EglErr(EGLint e) {
  switch (e) {
    case EGL_SUCCESS: return "EGL_SUCCESS";
    case EGL_BAD_ALLOC: return "EGL_BAD_ALLOC";
    case EGL_BAD_ATTRIBUTE: return "EGL_BAD_ATTRIBUTE";
    case EGL_BAD_CONFIG: return "EGL_BAD_CONFIG";
    case EGL_BAD_CONTEXT: return "EGL_BAD_CONTEXT";
    case EGL_BAD_DISPLAY: return "EGL_BAD_DISPLAY";
    case EGL_BAD_MATCH: return "EGL_BAD_MATCH";
    case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
    case EGL_BAD_PARAMETER: return "EGL_BAD_PARAMETER";
    case EGL_BAD_SURFACE: return "EGL_BAD_SURFACE";
    case EGL_NOT_INITIALIZED: return "EGL_NOT_INITIALIZED";
    default: return "EGL_?";
  }
}

void RefreshPixelSize() {
  if (!g.window) return;
  int w = 0, h = 0;
  SDL_GetWindowSizeInPixels(g.window, &w, &h);
  g_pix_w = w;
  g_pix_h = h;
}

// ---------------------------------------------------------------- EGL_ANDROID_blob_cache (shader cache on disk)
// ANGLE hands every compiled program / HLSL-compiled shader blob to these callbacks and asks for it back on the next
// compile of the same source. Backed by one file per key under <data root>\shadercache\, so compiled shaders (the
// d3dcompiler + driver compile cost, ~1 s of boot) survive across launches. ANGLE's keys already include its own
// version, so a different ANGLE build never reads stale entries. Callbacks come from any thread.
namespace blobcache {
std::filesystem::path g_dir;

std::filesystem::path PathFor(const void* key, EGLsizeiANDROID n) {
  static const char* hex = "0123456789abcdef";
  const auto* k = static_cast<const unsigned char*>(key);
  std::string name;
  if (n <= 32) {
    for (EGLsizeiANDROID i = 0; i < n; ++i) {
      name += hex[k[i] >> 4];
      name += hex[k[i] & 15];
    }
  } else {  // 64-bit FNV-1a for unusually long keys
    unsigned long long h = 1469598103934665603ull;
    for (EGLsizeiANDROID i = 0; i < n; ++i) h = (h ^ k[i]) * 1099511628211ull;
    char b[24];
    std::snprintf(b, sizeof b, "h%016llx", h);
    name = b;
  }
  return g_dir / (name + ".bin");
}

void EGLAPIENTRY Set(const void* key, EGLsizeiANDROID key_size, const void* value, EGLsizeiANDROID value_size) {
  if (!key || key_size <= 0 || !value || value_size <= 0 || value_size > (64ll << 20)) return;
  const std::filesystem::path path = PathFor(key, key_size);
  std::filesystem::path tmp = path;
  tmp += ".tmp" + std::to_string(GetCurrentThreadId());
  if (FILE* f = _wfopen(tmp.c_str(), L"wb")) {
    const bool ok = std::fwrite(value, 1, static_cast<size_t>(value_size), f) == static_cast<size_t>(value_size);
    std::fclose(f);
    std::error_code ec;
    if (ok) std::filesystem::rename(tmp, path, ec);  // replaces an existing file
    if (!ok || ec) std::filesystem::remove(tmp, ec);
  }
}

EGLsizeiANDROID EGLAPIENTRY Get(const void* key, EGLsizeiANDROID key_size, void* value, EGLsizeiANDROID value_size) {
  if (!key || key_size <= 0) return 0;
  FILE* f = _wfopen(PathFor(key, key_size).c_str(), L"rb");
  if (!f) return 0;
  _fseeki64(f, 0, SEEK_END);
  const long long size = _ftelli64(f);
  _fseeki64(f, 0, SEEK_SET);
  EGLsizeiANDROID ret = 0;
  if (size > 0) {
    ret = static_cast<EGLsizeiANDROID>(size);
    // Size query, or a buffer that is too small: report the size without copying.
    if (value && value_size >= size && std::fread(value, 1, static_cast<size_t>(size), f) != static_cast<size_t>(size)) ret = 0;
  }
  std::fclose(f);
  return ret;
}

void Install(EGLDisplay dpy) {
  if (const char* off = std::getenv("NOVA_NO_SHADER_CACHE"); off && off[0] == '1') return;
  const char* ext = eglQueryString(dpy, EGL_EXTENSIONS);
  auto set_funcs = reinterpret_cast<PFNEGLSETBLOBCACHEFUNCSANDROIDPROC>(eglGetProcAddress("eglSetBlobCacheFuncsANDROID"));
  if (!set_funcs || !ext || !std::strstr(ext, "EGL_ANDROID_blob_cache")) {
    log::Info("[gfx] EGL_ANDROID_blob_cache not available: shaders compile from source every launch");
    return;
  }
  std::error_code ec;
  g_dir = vfs::Root() / "shadercache";
  std::filesystem::create_directories(g_dir, ec);
  if (ec) {
    log::Warn("[gfx] shader cache folder {} unusable ({})", g_dir.string(), ec.message());
    return;
  }
  set_funcs(dpy, &Set, &Get);
  log::Info("[gfx] shader cache: {}", g_dir.string());
}
}  // namespace blobcache

// ---------------------------------------------------------------- ANGLE display
bool InitDisplay(bool warp) {
  auto get_platform_display =
      reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
  if (!get_platform_display) {
    log::Error("[gfx] ANGLE: eglGetPlatformDisplayEXT is missing (is libEGL.dll the ANGLE one?)");
    return false;
  }
  EGLint attrs[8];
  int n = 0;
  attrs[n++] = EGL_PLATFORM_ANGLE_TYPE_ANGLE;
  attrs[n++] = EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE;
  if (warp) {
    attrs[n++] = EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE;
    attrs[n++] = EGL_PLATFORM_ANGLE_DEVICE_TYPE_D3D_WARP_ANGLE;
  }
  attrs[n++] = EGL_NONE;
  EGLDisplay dpy = get_platform_display(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, attrs);
  if (dpy == EGL_NO_DISPLAY) {
    log::Warn("[gfx] ANGLE D3D11{} display: {}", warp ? " (WARP)" : "", EglErr(eglGetError()));
    return false;
  }
  EGLint major = 0, minor = 0;
  if (!eglInitialize(dpy, &major, &minor)) {
    log::Warn("[gfx] eglInitialize{}: {}", warp ? " (WARP)" : "", EglErr(eglGetError()));
    return false;
  }
  blobcache::Install(dpy);
  // One host config for everything we create: RGBA8 + D24S8, window and pbuffer capable. ES 3.0 first.
  for (int es3 = 1; es3 >= 0; --es3) {
    const EGLint want[] = {EGL_RENDERABLE_TYPE, es3 ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_ES2_BIT,
                           EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
                           EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                           EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE};
    EGLConfig cfg = nullptr;
    EGLint count = 0;
    if (eglChooseConfig(dpy, want, &cfg, 1, &count) && count > 0) {
      g.dpy = dpy;
      g.cfg = cfg;
      g.warp = warp;
      log::Info("[gfx] ANGLE EGL {}.{} on {} ({}), vendor \"{}\"", major, minor,
                warp ? "D3D11 WARP" : "D3D11", es3 ? "ES3-capable config" : "ES2 config",
                eglQueryString(dpy, EGL_VENDOR) ? eglQueryString(dpy, EGL_VENDOR) : "?");
      return true;
    }
  }
  log::Warn("[gfx] no RGBA8/D24S8 window+pbuffer config on the ANGLE display");
  eglTerminate(dpy);
  return false;
}

// ---------------------------------------------------------------- framebuffer 0 redirect
void FreeTarget(detail::Target& t) {
  if (t.fbo) glDeleteFramebuffers(1, &t.fbo);
  if (t.color) glDeleteTextures(1, &t.color);
  if (t.depth_stencil) glDeleteRenderbuffers(1, &t.depth_stencil);
  t = {};
}

// (Re)creates the context's redirect target at the current render size. Current context required.
void EnsureTarget(detail::Ctx& c) {
  int rw = 0, rh = 0;
  RenderSize(rw, rh);
  if (rw < 1 || rh < 1) return;
  if (c.target.fbo && c.target.w == rw && c.target.h == rh) return;
  GLint old_tex = 0, old_rb = 0, old_fb = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex);
  glGetIntegerv(GL_RENDERBUFFER_BINDING, &old_rb);
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fb);
  const bool was_bound = c.target.fbo && static_cast<GLuint>(old_fb) == c.target.fbo;
  FreeTarget(c.target);
  detail::Target t;
  t.w = rw;
  t.h = rh;
  glGenTextures(1, &t.color);
  glBindTexture(GL_TEXTURE_2D, t.color);
  if (c.es_major >= 3) glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rw, rh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  else glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rw, rh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glGenRenderbuffers(1, &t.depth_stencil);
  glBindRenderbuffer(GL_RENDERBUFFER, t.depth_stencil);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, rw, rh);
  glGenFramebuffers(1, &t.fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.color, 0);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t.depth_stencil);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t.depth_stencil);
  const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (st != GL_FRAMEBUFFER_COMPLETE) log::Error("[gfx] framebuffer-0 redirect target {}x{} incomplete (0x{:04X})", rw, rh, st);
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(old_tex));
  glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(old_rb));
  c.target = t;
  glBindFramebuffer(GL_FRAMEBUFFER, was_bound ? t.fbo : static_cast<GLuint>(old_fb));
}

using BlitFn = void (*)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
BlitFn ResolveBlit(const detail::Ctx& c) {
  if (c.es_major >= 3) return reinterpret_cast<BlitFn>(&glBlitFramebuffer);
  return reinterpret_cast<BlitFn>(eglGetProcAddress("glBlitFramebufferANGLE"));
}

// Letterbox-blits the redirected framebuffer 0 onto the real backbuffer, preserving every piece of GL state the
// guest could notice (bindings, viewport, scissor, colour mask, clear colour).
void BlitToWindow(detail::Ctx& c, detail::Surf& s) {
  EnsureTarget(c);
  if (!c.target.fbo) return;
  EGLint dw = 0, dh = 0;
  eglQuerySurface(g.dpy, s.real, EGL_WIDTH, &dw);
  eglQuerySurface(g.dpy, s.real, EGL_HEIGHT, &dh);
  if (dw < 1 || dh < 1) return;
  const BlitFn blit = ResolveBlit(c);
  if (!blit) return;

  GLint prev_draw = 0, prev_read = 0, vp[4] = {};
  GLfloat cc[4] = {};
  GLboolean cm[4] = {};
  glGetIntegerv(0x8CA6 /*DRAW_FRAMEBUFFER_BINDING*/, &prev_draw);
  glGetIntegerv(0x8CAA /*READ_FRAMEBUFFER_BINDING*/, &prev_read);
  glGetIntegerv(GL_VIEWPORT, vp);
  glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
  glGetBooleanv(GL_COLOR_WRITEMASK, cm);
  const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);

  const int sw = c.target.w, sh = c.target.h;
  const double scale = std::min(static_cast<double>(dw) / sw, static_cast<double>(dh) / sh);
  int rw = static_cast<int>(sw * scale + 0.5), rh = static_cast<int>(sh * scale + 0.5);
  rw = std::min(rw, static_cast<int>(dw));
  rh = std::min(rh, static_cast<int>(dh));
  const int rx = (dw - rw) / 2, ry = (dh - rh) / 2;

  glBindFramebuffer(0x8CA8 /*READ_FRAMEBUFFER*/, c.target.fbo);
  glBindFramebuffer(0x8CA9 /*DRAW_FRAMEBUFFER*/, 0);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  if (rw != dw || rh != dh) {
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
  }
  blit(0, 0, sw, sh, rx, ry, rx + rw, ry + rh, GL_COLOR_BUFFER_BIT, (rw == sw && rh == sh) ? GL_NEAREST : GL_LINEAR);

  glBindFramebuffer(0x8CA9, static_cast<GLuint>(prev_draw));
  glBindFramebuffer(0x8CA8, static_cast<GLuint>(prev_read));
  glViewport(vp[0], vp[1], vp[2], vp[3]);
  glClearColor(cc[0], cc[1], cc[2], cc[3]);
  glColorMask(cm[0], cm[1], cm[2], cm[3]);
  if (scissor) glEnable(GL_SCISSOR_TEST);
}

// ---------------------------------------------------------------- screenshots
std::mutex g_shot_mu;
std::string g_shot_path;
std::atomic<bool> g_shot_pending{false};
std::atomic<std::uint64_t> g_frames{0};

// Reads the redirect target (current context, GL thread) and writes it as a bottom-up 24-bit BMP.
void SaveShot(const detail::Ctx& c) {
  std::string path;
  {
    std::lock_guard lk(g_shot_mu);
    path.swap(g_shot_path);
    g_shot_pending = false;
  }
  if (path.empty() || !c.target.fbo) return;
  const int w = c.target.w, h = c.target.h;
  std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4);
  GLint prev_read = 0, pack = 4;
  glGetIntegerv(0x8CAA /*READ_FRAMEBUFFER_BINDING*/, &prev_read);
  glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
  glBindFramebuffer(0x8CA8 /*READ_FRAMEBUFFER*/, c.target.fbo);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  glPixelStorei(GL_PACK_ALIGNMENT, pack);
  glBindFramebuffer(0x8CA8, static_cast<GLuint>(prev_read));
  const int stride = (w * 3 + 3) & ~3;
  const std::uint32_t data_size = static_cast<std::uint32_t>(stride) * h, file_size = 54 + data_size;
  std::vector<std::uint8_t> out(file_size, 0);
  auto put32 = [&](std::size_t o, std::uint32_t v) { for (int i = 0; i < 4; ++i) out[o + i] = static_cast<std::uint8_t>(v >> (8 * i)); };
  out[0] = 'B', out[1] = 'M';
  put32(2, file_size);
  put32(10, 54);
  put32(14, 40);
  put32(18, static_cast<std::uint32_t>(w));
  put32(22, static_cast<std::uint32_t>(h));  // positive height = bottom-up rows, which is what glReadPixels returns
  out[26] = 1, out[28] = 24;
  put32(34, data_size);
  for (int y = 0; y < h; ++y) {
    const std::uint8_t* s = px.data() + static_cast<std::size_t>(y) * w * 4;
    std::uint8_t* d = out.data() + 54 + static_cast<std::size_t>(y) * stride;
    for (int x = 0; x < w; ++x) d[x * 3 + 0] = s[x * 4 + 2], d[x * 3 + 1] = s[x * 4 + 1], d[x * 3 + 2] = s[x * 4 + 0];
  }
  if (FILE* f = std::fopen(path.c_str(), "wb")) {
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    log::Info("[gfx] screenshot {}x{} -> {}", w, h, path);
  } else {
    log::Warn("[gfx] cannot write screenshot {}", path);
  }
}

// ---------------------------------------------------------------- frame pacing
void Pace() {
  static thread_local Clock::time_point next{};
  const int fps = g_target_fps.load(std::memory_order_relaxed);
  if (fps <= 0) {
    next = {};
    return;
  }
  const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / fps));
  const auto now = Clock::now();
  if (next.time_since_epoch().count() == 0 || now - next > period * 4) next = now;
  next += period;
  if (next <= now) return;
  const auto coarse = next - std::chrono::milliseconds(2);
  if (coarse > now) std::this_thread::sleep_until(coarse);
  while (Clock::now() < next) std::this_thread::yield();
}

}  // namespace

// ================================================================ detail
namespace detail {

EGLDisplay Display() { return g.dpy; }
EGLConfig HostConfig() { return g.cfg; }
bool PreferGles3() { return g.prefer_es3; }
Ctx* CurrentCtx() { return t_ctx; }
Surf* CurrentDraw() { return t_draw; }

Ctx* CreateContext(const Ctx* share, int es_wanted) {
  if (!g.inited) return nullptr;
  auto* c = new Ctx;
  const EGLContext shr = share ? share->real : EGL_NO_CONTEXT;
  (void)es_wanted;  // an ES 2.0 request is served by an ES 3.0 context (a superset) when allowed
  if (g.prefer_es3) {
    const EGLint a[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
    c->real = eglCreateContext(g.dpy, g.cfg, shr, a);
    if (c->real != EGL_NO_CONTEXT) c->es_major = 3;
  }
  if (c->real == EGL_NO_CONTEXT) {
    if (g.prefer_es3) log::Warn("[gfx] ES 3.0 context unavailable ({}), using ES 2.0", EglErr(eglGetError()));
    const EGLint a[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    c->real = eglCreateContext(g.dpy, g.cfg, shr, a);
    c->es_major = 2;
  }
  if (c->real == EGL_NO_CONTEXT) {
    log::Error("[gfx] eglCreateContext failed: {}", EglErr(eglGetError()));
    delete c;
    return nullptr;
  }
  return c;
}

void DestroyContext(Ctx* c) {
  if (!c || c->real == EGL_NO_CONTEXT) return;
  eglDestroyContext(g.dpy, c->real);  // EGL defers the actual deletion while it is current on a thread
  c->real = EGL_NO_CONTEXT;
}

Surf* AcquireWindowSurface() {
  std::lock_guard lk(g.mu);
  if (!g.inited || !g.hwnd) return nullptr;
  if (g.window_surf && g.window_surf->real != EGL_NO_SURFACE) {
    ++g.window_surf->refs;
    return g.window_surf;
  }
  const EGLint a[] = {EGL_NONE};
  EGLSurface sf = eglCreateWindowSurface(g.dpy, g.cfg, reinterpret_cast<EGLNativeWindowType>(g.hwnd), a);
  if (sf == EGL_NO_SURFACE) {
    log::Error("[gfx] eglCreateWindowSurface failed: {}", EglErr(eglGetError()));
    return nullptr;
  }
  auto* s = new Surf;
  s->real = sf;
  s->window = true;
  s->refs = 1;
  g.window_surf = s;
  return s;
}

void ReleaseWindowSurface(Surf* s) {
  std::lock_guard lk(g.mu);
  if (!s || !s->window || s->real == EGL_NO_SURFACE) return;
  if (--s->refs > 0) return;
  eglDestroySurface(g.dpy, s->real);
  s->real = EGL_NO_SURFACE;
  if (g.window_surf == s) g.window_surf = nullptr;
}

Surf* CreatePbuffer(const EGLint* attribs) {
  if (!g.inited) return nullptr;
  static const EGLint none[] = {EGL_NONE};
  EGLSurface sf = eglCreatePbufferSurface(g.dpy, g.cfg, attribs ? attribs : none);
  if (sf == EGL_NO_SURFACE) return nullptr;
  auto* s = new Surf;
  s->real = sf;
  return s;
}

void DestroyPbuffer(Surf* s) {
  if (!s || s->window || s->real == EGL_NO_SURFACE) return;
  eglDestroySurface(g.dpy, s->real);
  s->real = EGL_NO_SURFACE;
}

bool MakeCurrent(Ctx* c, Surf* draw) {
  if (!c) {
    const bool ok = eglMakeCurrent(g.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) == EGL_TRUE;
    if (ok) t_ctx = nullptr, t_draw = nullptr;
    return ok;
  }
  const EGLSurface sf = draw ? draw->real : EGL_NO_SURFACE;
  if (c->real == EGL_NO_CONTEXT || (draw && sf == EGL_NO_SURFACE)) return false;
  if (!eglMakeCurrent(g.dpy, sf, sf, c->real)) return false;
  t_ctx = c;
  t_draw = draw;
  if (draw && draw->window) {
    EnsureTarget(*c);
    if (c->target.fbo) {
      if (!c->state_init) {
        glViewport(0, 0, c->target.w, c->target.h);
        glScissor(0, 0, c->target.w, c->target.h);
        c->state_init = true;
      }
      GLint fb = 0;
      glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
      if (fb == 0) glBindFramebuffer(GL_FRAMEBUFFER, c->target.fbo);
    }
    const int want = g_vsync.load() ? 1 : 0;
    if (draw->applied_interval != want) {
      eglSwapInterval(g.dpy, want);
      draw->applied_interval = want;
    }
  } else if (c->target.fbo) {
    // Off the window surface the guest's "0" is the real default framebuffer again.
    GLint fb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    if (static_cast<GLuint>(fb) == c->target.fbo) glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }
  return true;
}

void Present(Surf* s) {
  Ctx* c = t_ctx;
  if (!c || !s || s->real == EGL_NO_SURFACE) return;
  if (s->window) {
    if (g_shot_pending.load(std::memory_order_relaxed)) SaveShot(*c);
    g_frames.fetch_add(1, std::memory_order_relaxed);
    BlitToWindow(*c, *s);
    const int want = g_vsync.load(std::memory_order_relaxed) ? 1 : 0;
    if (s->applied_interval != want) {
      eglSwapInterval(g.dpy, want);
      s->applied_interval = want;
    }
  }
  if (!eglSwapBuffers(g.dpy, s->real)) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      log::Warn("[gfx] eglSwapBuffers failed: {}", EglErr(eglGetError()));
    }
  }
  Pace();
}

}  // namespace detail

// ================================================================ public API
bool Init(const Config& cfg) {
  if (g.inited) return true;
  SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE, "0");  // raw, unaccelerated relative motion
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
    log::Error("[gfx] SDL_Init: {}", SDL_GetError());
    return false;
  }
  int ndisp = 0;
  SDL_DisplayID* displays = SDL_GetDisplays(&ndisp);
  SDL_DisplayID did = 0;
  if (displays && ndisp > 0) did = displays[cfg.display >= 0 && cfg.display < ndisp ? cfg.display : 0];
  SDL_free(displays);
  g.display_index = cfg.display;

  SDL_PropertiesID props = SDL_CreateProperties();
  SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, cfg.title.c_str());
  SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, cfg.width > 0 ? cfg.width : 1280);
  SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, cfg.height > 0 ? cfg.height : 720);
  SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, cfg.resizable);
  SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
  SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, cfg.hidden);
  if (did) {
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(did));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(did));
  }
  g.window = SDL_CreateWindowWithProperties(props);
  SDL_DestroyProperties(props);
  if (!g.window) {
    log::Error("[gfx] SDL_CreateWindow: {}", SDL_GetError());
    SDL_Quit();
    return false;
  }
  g.hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(g.window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
  if (!g.hwnd) {
    log::Error("[gfx] the SDL window has no HWND (not the Windows video driver?)");
    SDL_DestroyWindow(g.window);
    g.window = nullptr;
    SDL_Quit();
    return false;
  }

  g.prefer_es3 = cfg.prefer_gles3;
  if (!InitDisplay(false) && !(cfg.allow_warp && InitDisplay(true))) {
    log::Error("[gfx] could not initialise ANGLE (D3D11)");
    SDL_DestroyWindow(g.window);
    g.window = nullptr;
    SDL_Quit();
    return false;
  }

  g_vsync = cfg.vsync;
  g_target_fps = cfg.target_fps;
  g_close = false;
  g.inited = true;
  if (cfg.mode != WindowMode::Windowed) SetFullscreen(cfg.mode, cfg.fullscreen_width, cfg.fullscreen_height, cfg.fullscreen_refresh);
  RefreshPixelSize();
  g_render_w = cfg.render_width > 0 ? cfg.render_width : g_pix_w.load();
  g_render_h = cfg.render_height > 0 ? cfg.render_height : g_pix_h.load();
  log::Info("[gfx] window {}x{} px (scale {:.2f}), render size {}x{}, vsync {}, target fps {}", g_pix_w.load(),
            g_pix_h.load(), DisplayScale(), g_render_w.load(), g_render_h.load(), cfg.vsync ? "on" : "off",
            cfg.target_fps);
  return true;
}

void Shutdown() {
  if (!g.inited) return;
  DestroyHostContext();
  if (g.dpy != EGL_NO_DISPLAY) {
    eglMakeCurrent(g.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglTerminate(g.dpy);
  }
  g.dpy = EGL_NO_DISPLAY;
  g.cfg = nullptr;
  g.window_surf = nullptr;
  detail::ResetGlState();
  if (g.window) SDL_DestroyWindow(g.window);
  g.window = nullptr;
  g.hwnd = nullptr;
  SDL_Quit();
  g.inited = false;
}

bool IsInitialized() { return g.inited; }
SDL_Window* Window() { return g.window; }

void WindowPixelSize(int& w, int& h) {
  w = g_pix_w.load();
  h = g_pix_h.load();
}

float DisplayScale() { return g.window ? SDL_GetWindowDisplayScale(g.window) : 1.0f; }

bool SetFullscreen(WindowMode mode, int width, int height, float refresh) {
  if (!g.window) return false;
  bool ok = true;
  if (mode == WindowMode::Windowed) {
    ok = SDL_SetWindowFullscreen(g.window, false);
  } else {
    int n = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&n);
    SDL_DisplayID did = SDL_GetDisplayForWindow(g.window);
    if (ids && n > 0 && g.display_index >= 0 && g.display_index < n) did = ids[g.display_index];
    SDL_free(ids);
    if (mode == WindowMode::Exclusive) {
      SDL_DisplayMode dm;
      const SDL_DisplayMode* desk = SDL_GetDesktopDisplayMode(did);
      const int w = width > 0 ? width : (desk ? desk->w : 1920);
      const int h = height > 0 ? height : (desk ? desk->h : 1080);
      if (SDL_GetClosestFullscreenDisplayMode(did, w, h, refresh, true, &dm)) {
        ok = SDL_SetWindowFullscreenMode(g.window, &dm);
      } else {
        log::Warn("[gfx] no display mode near {}x{}@{}: {}", w, h, refresh, SDL_GetError());
        ok = false;
      }
    } else {
      ok = SDL_SetWindowFullscreenMode(g.window, nullptr);  // borderless: desktop resolution
    }
    if (ok) {
      SDL_SetWindowPosition(g.window, SDL_WINDOWPOS_CENTERED_DISPLAY(did), SDL_WINDOWPOS_CENTERED_DISPLAY(did));
      ok = SDL_SetWindowFullscreen(g.window, true);
    }
  }
  if (!ok) {
    log::Warn("[gfx] SetFullscreen: {}", SDL_GetError());
    return false;
  }
  SDL_SyncWindow(g.window);
  g.mode = mode;
  RefreshPixelSize();
  return true;
}

WindowMode GetWindowMode() { return g.mode; }

void SetWindowSize(int w, int h) {
  if (!g.window || w < 1 || h < 1) return;
  SDL_SetWindowSize(g.window, w, h);
  SDL_SyncWindow(g.window);
  RefreshPixelSize();
}

void SetRelativeMouse(bool enabled) {
  if (!g.window) return;
  if (!SDL_SetWindowRelativeMouseMode(g.window, enabled)) {
    log::Warn("[gfx] relative mouse mode: {}", SDL_GetError());
    return;
  }
  g.relative_mouse = enabled;
}

bool RelativeMouse() { return g.relative_mouse; }

std::vector<SDL_Event> PumpEvents() {
  std::vector<SDL_Event> out;
  SDL_PumpEvents();
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    switch (e.type) {
      case SDL_EVENT_QUIT:
      case SDL_EVENT_WINDOW_CLOSE_REQUESTED: g_close = true; break;
      case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
      case SDL_EVENT_WINDOW_RESIZED:
      case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: RefreshPixelSize(); break;
      default: break;
    }
    out.push_back(e);
  }
  return out;
}

bool CloseRequested() { return g_close.load(); }

void SetVsync(bool on) { g_vsync = on; }
bool Vsync() { return g_vsync.load(); }
void SetTargetFps(int fps) { g_target_fps = fps < 0 ? 0 : fps; }
int TargetFps() { return g_target_fps.load(); }

void RenderSize(int& w, int& h) {
  w = g_render_w.load();
  h = g_render_h.load();
}

void SetRenderSize(int w, int h) {
  if (w < 1 || h < 1) return;
  g_render_w = w;
  g_render_h = h;
}

std::uint64_t FramesPresented() { return g_frames.load(); }

void RequestScreenshot(const std::string& path) {
  std::lock_guard lk(g_shot_mu);
  g_shot_path = path;
  g_shot_pending = true;
}

bool CreateHostContext() {
  if (!g.inited) return false;
  if (g.host_ctx) return true;
  detail::Ctx* c = detail::CreateContext(nullptr, 3);
  detail::Surf* s = detail::AcquireWindowSurface();
  if (!c || !s || !detail::MakeCurrent(c, s)) {
    log::Error("[gfx] CreateHostContext failed: {}", EglErr(eglGetError()));
    if (c) detail::DestroyContext(c);
    if (s) detail::ReleaseWindowSurface(s);
    return false;
  }
  g.host_ctx = c;
  g.host_surf = s;
  return true;
}

void DestroyHostContext() {
  if (!g.host_ctx) return;
  if (detail::CurrentCtx() == g.host_ctx) detail::MakeCurrent(nullptr, nullptr);
  detail::DestroyContext(g.host_ctx);
  detail::ReleaseWindowSurface(g.host_surf);
  g.host_ctx = nullptr;
  g.host_surf = nullptr;
}

void BeginFrame() {
  detail::Ctx* c = detail::CurrentCtx();
  detail::Surf* s = detail::CurrentDraw();
  if (!c || !s || !s->window) return;
  EnsureTarget(*c);
  glBindFramebuffer(GL_FRAMEBUFFER, c->target.fbo);
  glViewport(0, 0, c->target.w, c->target.h);
}

void EndFrame() { detail::Present(detail::CurrentDraw()); }

}  // namespace nova::gfx
