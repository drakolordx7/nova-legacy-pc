// Shared between gfx.cpp (window, ANGLE display, contexts, framebuffer 0 redirect, present), hle_egl.cpp and
// hle_gles.cpp. Not part of the public API.
#pragma once
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES3/gl3.h>

#include <cstdint>

#include "gfx/gfx.h"

namespace nova::gfx::detail {

// The guest's "framebuffer 0" for one context: a host FBO at the render resolution (FBOs are per-context in GL, so
// each context current on the window surface owns one).
struct Target {
  GLuint fbo = 0, color = 0, depth_stencil = 0;
  int w = 0, h = 0;
};

// One ANGLE context. Plain records that are never freed (a handful per run); `real` is NO_CONTEXT once destroyed.
struct Ctx {
  EGLContext real = EGL_NO_CONTEXT;
  int es_major = 2;               // real context version (3 when ANGLE gave an ES 3.0 context)
  Target target;                  // framebuffer 0 as the guest sees it, while current on the window surface
  bool state_init = false;        // viewport/scissor initialised for the render size
  // GL state the HLE needs to translate client pointers: with a buffer bound the pointer is an offset, without one
  // it is a guest address.
  GLuint array_buffer = 0, element_buffer = 0;
  GLenum pending_error = GL_NO_ERROR;  // errors synthesised by the HLE, returned by the next glGetError
};

struct Surf {
  EGLSurface real = EGL_NO_SURFACE;
  bool window = false;       // the SDL window's surface (guest framebuffer 0 is redirected); else a pbuffer
  int refs = 0;              // window surface: guest surfaces sharing it
  int applied_interval = -1; // last eglSwapInterval applied
};

// ---- display / config (valid after gfx::Init) ----
EGLDisplay Display();
EGLConfig HostConfig();
bool PreferGles3();

// ---- contexts and surfaces ----
// Creates a real context from the host config; es_wanted is the guest's EGL_CONTEXT_CLIENT_VERSION (2 or 3). The
// context is ES 3.0 when Config::prefer_gles3 and ANGLE allows it, else ES 2.0. share may be null.
Ctx* CreateContext(const Ctx* share, int es_wanted);
void DestroyContext(Ctx* c);
// The window surface is shared between guest surfaces (refcounted). Null on failure.
Surf* AcquireWindowSurface();
void ReleaseWindowSurface(Surf* s);
// attribs: guest EGL attribute list already copied to host memory (EGL_NONE terminated), may be null.
Surf* CreatePbuffer(const EGLint* attribs);
void DestroyPbuffer(Surf* s);

// Per calling thread: the context/surface currently bound through MakeCurrent.
Ctx* CurrentCtx();
Surf* CurrentDraw();
// eglMakeCurrent for the calling thread. c == null releases. draw may be null (surfaceless). Applies the
// framebuffer-0 redirect when draw is the window surface. Returns false (error left in eglGetError) on failure.
bool MakeCurrent(Ctx* c, Surf* draw);
// Present the current window surface: blit the redirected framebuffer 0 to the window, eglSwapBuffers, frame pacing.
// Any surface other than the window just swaps.
void Present(Surf* s);
// Guest-visible name of the framebuffer binding: the real FBO to bind for guest name 0 (0 when not redirected).
inline GLuint DefaultFbo() {
  Ctx* c = CurrentCtx();
  Surf* s = CurrentDraw();
  return (c && s && s->window) ? c->target.fbo : 0;
}

// ---- guest HLE registration (one per file) ----
void RegisterEgl();           // hle_egl.cpp
void RegisterGles();          // hle_gles.cpp
// Guest address of a NUL-terminated copy of `s` (cached by content, never freed). 0 on failure.
std::uint64_t GuestString(const std::string& s);

// ---- hle_gles.cpp ----
// ANGLE capabilities and the extension string shown to the guest (needs a current context; probes once).
struct Caps {
  bool probed = false;
  int es_major = 2;
  bool etc1_native = false;
  bool s3tc = false;
  std::string extensions;                 // what glGetString(GL_EXTENSIONS) returns
  std::vector<std::uint32_t> compressed;  // GL_COMPRESSED_TEXTURE_FORMATS
};
const Caps& GetCaps();
void ResetGlState();  // forget cached GL state (context destroyed)

}  // namespace nova::gfx::detail

namespace nova::android {
void RegisterNativeWindow();  // android/native_window.cpp: the ANativeWindow_* imports of libandroid
}
