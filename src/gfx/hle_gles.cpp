// libGLESv2 HLE: the 96 gl* imports of libNOVA.so plus every extension function ANGLE implements (handed out through
// eglGetProcAddress) as FAST thunks straight onto ANGLE's exported entry points.
//
// Rules (adapted from Zombiewood's hle/gles, MIT, (c) 2026 ChrisDutary):
//  - Handlers run inside the JIT (kFast): they only call ANGLE, never the guest.
//  - Pointer parameters that are always pointers arrive already translated (T* = g2h). The ones that may be offsets
//    into a bound buffer (glVertexAttribPointer, glDrawElements*) arrive as guest addresses and are translated at call
//    time, according to our own tracking of the array/element buffer bindings: the guest's client-side vertex and index
//    arrays stay in guest memory, which is host-addressable, so ANGLE reads them directly at draw time.
//  - GLsizeiptr/GLintptr are 64-bit in the arm64 guest (int64_t here); GLsync handles are ANGLE's own pointers, opaque
//    to the guest.
//  - The guest's framebuffer 0 is a host FBO (gfx.cpp): glBindFramebuffer(.., 0) binds it, glGet* of the binding maps
//    it back to 0.
//  - glGetString/GL_EXTENSIONS show a mobile GPU; the extension list only names what we honour (ETC1/PVRTC/ATC are
//    decoded in software when ANGLE cannot take them).
//
// Environment: NOVA_GL_CHECK=1 (glGetError after every call, logged), NOVA_GL_LOG_SHADERS=1, NOVA_GL_ETC1_SOFT=1,
// NOVA_GL_RENDERER="..." (GL_RENDERER string), NOVA_GL_EXT_DENY="GL_a,GL_b" (hide extensions from the guest).
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>  // after gl3.h: it needs the GL types

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "cpu/guest_memory.h"
#include "cpu/hle_abi.h"
#include "gfx/gfx_internal.h"
#include "gfx/texdecode.h"

namespace nova::gfx {
namespace {

using detail::Ctx;

bool g_check = false;
bool g_log_shaders = false;
bool g_etc1_soft = false;

std::mutex g_mu;  // caps, shadows
detail::Caps g_caps;

bool EnvOn(const char* n) {
  const char* v = std::getenv(n);
  return v && *v && std::strcmp(v, "0") != 0;
}

void SetError(GLenum e) {
  if (Ctx* c = detail::CurrentCtx())
    if (c->pending_error == GL_NO_ERROR) c->pending_error = e;
}

void DrainErrors(const char* fn) {
  for (int i = 0; i < 8; ++i) {
    const GLenum e = ::glGetError();
    if (e == GL_NO_ERROR) break;
    log::Warn("[gles] GL error 0x{:04X} in {}", e, fn);
    SetError(e);
  }
}
inline void Chk(const char* fn) {
  if (g_check) DrainErrors(fn);
}

// ---------------------------------------------------------------- capabilities and strings
bool HasExt(const std::string& list, std::string_view e) {
  std::size_t p = 0;
  while ((p = list.find(e, p)) != std::string::npos) {
    const bool start = p == 0 || list[p - 1] == ' ';
    const bool end = p + e.size() == list.size() || list[p + e.size()] == ' ';
    if (start && end) return true;
    p += e.size();
  }
  return false;
}

// Runs `f` and reports whether it raised no GL error (the guest's own pending errors are preserved).
template <class F>
bool Probe(F&& f) {
  for (int i = 0; i < 8 && ::glGetError() != GL_NO_ERROR; ++i) {}
  f();
  return ::glGetError() == GL_NO_ERROR;
}

bool ProbeCompressed(GLenum fmt) {
  GLint old_tex = 0;
  ::glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex);
  GLuint t = 0;
  ::glGenTextures(1, &t);
  ::glBindTexture(GL_TEXTURE_2D, t);
  const std::uint8_t blk[8] = {};
  const bool ok = Probe([&] { ::glCompressedTexImage2D(GL_TEXTURE_2D, 0, fmt, 4, 4, 0, 8, blk); });
  ::glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(old_tex));
  ::glDeleteTextures(1, &t);
  return ok;
}

bool ProbeTexImage(GLint internal, GLenum format, GLenum type) {
  GLint old_tex = 0;
  ::glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex);
  GLuint t = 0;
  ::glGenTextures(1, &t);
  ::glBindTexture(GL_TEXTURE_2D, t);
  const bool ok = Probe([&] { ::glTexImage2D(GL_TEXTURE_2D, 0, internal, 4, 4, 0, format, type, nullptr); });
  ::glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(old_tex));
  ::glDeleteTextures(1, &t);
  return ok;
}

std::string ComputeExtensions(detail::Caps& c, const std::string& angle) {
  c.etc1_native = HasExt(angle, "GL_OES_compressed_ETC1_RGB8_texture");
  c.s3tc = HasExt(angle, "GL_EXT_texture_compression_s3tc") || HasExt(angle, "GL_EXT_texture_compression_dxt1");
  const bool es3 = c.es_major >= 3;
  const bool etc2 = !c.etc1_native && es3 && ProbeCompressed(0x9274 /*GL_COMPRESSED_RGB8_ETC2*/);
  if (!c.etc1_native && !etc2) log::Info("[gles] ETC1 textures: software decode");
  else log::Info("[gles] ETC1 textures: {}", c.etc1_native ? "native" : "uploaded as ETC2");
  // (the upload route is decided per call, see RouteFor)
  std::vector<std::string> out;
  auto add = [&](const char* name, bool cond) {
    if (cond) out.emplace_back(name);
  };
  add("GL_OES_compressed_ETC1_RGB8_texture", true);   // native, ETC2 or software
  add("GL_IMG_texture_compression_pvrtc", true);       // software
  add("GL_AMD_compressed_ATC_texture", true);          // software
  add("GL_EXT_texture_compression_dxt1", c.s3tc);
  add("GL_EXT_texture_compression_s3tc", c.s3tc);
  add("GL_OES_rgb8_rgba8", es3 || HasExt(angle, "GL_OES_rgb8_rgba8"));
  add("GL_OES_depth24", es3 || HasExt(angle, "GL_OES_depth24"));
  add("GL_OES_packed_depth_stencil", es3 || HasExt(angle, "GL_OES_packed_depth_stencil"));
  add("GL_OES_element_index_uint", es3 || HasExt(angle, "GL_OES_element_index_uint"));
  add("GL_OES_texture_npot", es3 || HasExt(angle, "GL_OES_texture_npot"));
  add("GL_OES_standard_derivatives", es3 || HasExt(angle, "GL_OES_standard_derivatives"));
  add("GL_OES_depth_texture",
      HasExt(angle, "GL_OES_depth_texture") || HasExt(angle, "GL_ANGLE_depth_texture") ||
          ProbeTexImage(GL_DEPTH_COMPONENT, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT));
  add("GL_OES_texture_float", HasExt(angle, "GL_OES_texture_float") || ProbeTexImage(GL_RGBA, GL_RGBA, GL_FLOAT));
  add("GL_OES_texture_float_linear", HasExt(angle, "GL_OES_texture_float_linear"));
  add("GL_OES_texture_half_float",
      HasExt(angle, "GL_OES_texture_half_float") || ProbeTexImage(GL_RGBA, GL_RGBA, 0x8D61 /*GL_HALF_FLOAT_OES*/));
  add("GL_OES_texture_half_float_linear", HasExt(angle, "GL_OES_texture_half_float_linear"));
  add("GL_EXT_texture_filter_anisotropic", HasExt(angle, "GL_EXT_texture_filter_anisotropic"));
  add("GL_EXT_texture_storage", es3);
  add("GL_EXT_texture_rg", es3 || HasExt(angle, "GL_EXT_texture_rg"));
  add("GL_EXT_texture_format_BGRA8888", HasExt(angle, "GL_EXT_texture_format_BGRA8888"));
  add("GL_EXT_color_buffer_half_float", HasExt(angle, "GL_EXT_color_buffer_half_float"));
  add("GL_EXT_color_buffer_float", HasExt(angle, "GL_EXT_color_buffer_float"));
  add("GL_EXT_discard_framebuffer", es3);                 // glDiscardFramebufferEXT -> glInvalidateFramebuffer
  add("GL_EXT_map_buffer_range", es3);                    // shadow copies in guest memory
  add("GL_OES_mapbuffer", es3);
  add("GL_ANGLE_framebuffer_blit", es3);
  add("GL_ANGLE_framebuffer_multisample", es3);

  std::vector<std::string> deny;
  if (const char* d = std::getenv("NOVA_GL_EXT_DENY")) {
    std::istringstream is(d);
    for (std::string t; std::getline(is, t, ',');) deny.push_back(t);
  }
  std::string res;
  for (const auto& e : out) {
    if (std::find(deny.begin(), deny.end(), e) != deny.end()) continue;
    if (!res.empty()) res += ' ';
    res += e;
  }
  return res;
}

}  // namespace

namespace detail {

const Caps& GetCaps() {
  std::lock_guard lk(g_mu);
  if (g_caps.probed) return g_caps;
  Ctx* ctx = CurrentCtx();
  if (!ctx) return g_caps;  // not probed: no context yet
  for (int i = 0; i < 8; ++i) {  // keep the guest's pending errors
    const GLenum e = ::glGetError();
    if (e == GL_NO_ERROR) break;
    if (ctx->pending_error == GL_NO_ERROR) ctx->pending_error = e;
  }
  Caps c;
  c.es_major = ctx->es_major;
  const char* ext = reinterpret_cast<const char*>(::glGetString(GL_EXTENSIONS));
  const std::string angle = ext ? ext : "";
  c.extensions = ComputeExtensions(c, angle);
  c.compressed = {texdecode::kETC1_RGB8, texdecode::kPVRTC_RGB_4BPP, texdecode::kPVRTC_RGB_2BPP,
                  texdecode::kPVRTC_RGBA_4BPP, texdecode::kPVRTC_RGBA_2BPP, texdecode::kATC_RGB,
                  texdecode::kATC_RGBA_EXPLICIT, texdecode::kATC_RGBA_INTERPOLATED};
  if (c.s3tc) c.compressed.insert(c.compressed.end(), {0x83F0u, 0x83F1u, 0x83F2u, 0x83F3u});
  c.probed = true;
  ::glGetError();
  g_caps = c;
  const char* ver = reinterpret_cast<const char*>(::glGetString(GL_VERSION));
  log::Info("[gles] ANGLE context: {} ({}); guest extensions: {}", ver ? ver : "?",
            c.es_major >= 3 ? "ES 3.0" : "ES 2.0", c.extensions);
  return g_caps;
}

}  // namespace detail

namespace {

// ---------------------------------------------------------------- simple pass-through handlers
#define GLV(name, params, args) \
  void h_##name params { ::name args; Chk(#name); }
#define GLR(ret, name, params, args) \
  ret h_##name params { ret r_ = ::name args; Chk(#name); return r_; }

GLV(glActiveTexture, (GLenum t), (t))
GLV(glAttachShader, (GLuint p, GLuint s), (p, s))
GLV(glBindRenderbuffer, (GLenum t, GLuint r), (t, r))
GLV(glBindTexture, (GLenum t, GLuint tex), (t, tex))
GLV(glBlendColor, (float r, float g, float b, float a), (r, g, b, a))
GLV(glBlendEquation, (GLenum m), (m))
GLV(glBlendFunc, (GLenum s, GLenum d), (s, d))
GLR(GLenum, glCheckFramebufferStatus, (GLenum t), (t))
GLV(glClear, (GLbitfield m), (m))
GLV(glClearColor, (float r, float g, float b, float a), (r, g, b, a))
GLV(glClearDepthf, (float d), (d))
GLV(glClearStencil, (GLint s), (s))
GLV(glColorMask, (GLboolean r, GLboolean g, GLboolean b, GLboolean a), (r, g, b, a))
GLV(glCopyTexSubImage2D, (GLenum t, GLint l, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h), (t, l, xo, yo, x, y, w, h))
GLR(GLuint, glCreateProgram, (), ())
GLR(GLuint, glCreateShader, (GLenum t), (t))
GLV(glCullFace, (GLenum m), (m))
GLV(glDeleteFramebuffers, (GLsizei n, const GLuint* f), (n, f))
GLV(glDeleteProgram, (GLuint p), (p))
GLV(glDeleteRenderbuffers, (GLsizei n, const GLuint* r), (n, r))
GLV(glDeleteShader, (GLuint s), (s))
GLV(glDeleteTextures, (GLsizei n, const GLuint* t), (n, t))
GLV(glDepthFunc, (GLenum f), (f))
GLV(glDepthMask, (GLboolean f), (f))
GLV(glDepthRangef, (float n, float f), (n, f))
GLV(glDisable, (GLenum c), (c))
GLV(glDisableVertexAttribArray, (GLuint i), (i))
GLV(glDrawArrays, (GLenum m, GLint f, GLsizei c), (m, f, c))
GLV(glEnable, (GLenum c), (c))
GLV(glEnableVertexAttribArray, (GLuint i), (i))
GLV(glFinish, (), ())
GLV(glFlush, (), ())
GLV(glFramebufferRenderbuffer, (GLenum t, GLenum a, GLenum rt, GLuint r), (t, a, rt, r))
GLV(glFramebufferTexture2D, (GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l), (t, a, tt, tex, l))
GLV(glFrontFace, (GLenum m), (m))
GLV(glGenBuffers, (GLsizei n, GLuint* b), (n, b))
GLV(glGenFramebuffers, (GLsizei n, GLuint* f), (n, f))
GLV(glGenRenderbuffers, (GLsizei n, GLuint* r), (n, r))
GLV(glGenTextures, (GLsizei n, GLuint* t), (n, t))
GLV(glGenerateMipmap, (GLenum t), (t))
GLV(glGetActiveAttrib, (GLuint p, GLuint i, GLsizei bs, GLsizei* len, GLint* sz, GLenum* ty, GLchar* name), (p, i, bs, len, sz, ty, name))
GLV(glGetActiveUniform, (GLuint p, GLuint i, GLsizei bs, GLsizei* len, GLint* sz, GLenum* ty, GLchar* name), (p, i, bs, len, sz, ty, name))
GLR(GLint, glGetAttribLocation, (GLuint p, const GLchar* n), (p, n))
GLV(glGetProgramInfoLog, (GLuint p, GLsizei bs, GLsizei* len, GLchar* log), (p, bs, len, log))
GLV(glGetProgramiv, (GLuint p, GLenum pn, GLint* v), (p, pn, v))
GLV(glGetShaderInfoLog, (GLuint s, GLsizei bs, GLsizei* len, GLchar* log), (s, bs, len, log))
GLV(glGetShaderSource, (GLuint s, GLsizei bs, GLsizei* len, GLchar* src), (s, bs, len, src))
GLV(glGetShaderiv, (GLuint s, GLenum pn, GLint* v), (s, pn, v))
GLR(GLint, glGetUniformLocation, (GLuint p, const GLchar* n), (p, n))
GLV(glLineWidth, (float w), (w))
GLV(glPixelStorei, (GLenum pn, GLint v), (pn, v))
GLV(glPolygonOffset, (float f, float u), (f, u))
GLV(glReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void* px), (x, y, w, h, f, t, px))
GLV(glRenderbufferStorage, (GLenum t, GLenum f, GLsizei w, GLsizei h), (t, f, w, h))
GLV(glSampleCoverage, (float v, GLboolean inv), (v, inv))
GLV(glScissor, (GLint x, GLint y, GLsizei w, GLsizei h), (x, y, w, h))
GLV(glStencilFunc, (GLenum f, GLint r, GLuint m), (f, r, m))
GLV(glStencilMask, (GLuint m), (m))
GLV(glStencilOp, (GLenum a, GLenum b, GLenum c), (a, b, c))
GLV(glTexImage2D, (GLenum t, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, const void* px), (t, l, ifmt, w, h, b, f, ty, px))
GLV(glTexParameterf, (GLenum t, GLenum pn, float v), (t, pn, v))
GLV(glTexParameteri, (GLenum t, GLenum pn, GLint v), (t, pn, v))
GLV(glTexSubImage2D, (GLenum t, GLint l, GLint xo, GLint yo, GLsizei w, GLsizei h, GLenum f, GLenum ty, const void* px), (t, l, xo, yo, w, h, f, ty, px))
GLV(glUniform1f, (GLint l, float x), (l, x))
GLV(glUniform1fv, (GLint l, GLsizei c, const GLfloat* v), (l, c, v))
GLV(glUniform1i, (GLint l, GLint x), (l, x))
GLV(glUniform1iv, (GLint l, GLsizei c, const GLint* v), (l, c, v))
GLV(glUniform2fv, (GLint l, GLsizei c, const GLfloat* v), (l, c, v))
GLV(glUniform2iv, (GLint l, GLsizei c, const GLint* v), (l, c, v))
GLV(glUniform3fv, (GLint l, GLsizei c, const GLfloat* v), (l, c, v))
GLV(glUniform3iv, (GLint l, GLsizei c, const GLint* v), (l, c, v))
GLV(glUniform4f, (GLint l, float x, float y, float z, float w), (l, x, y, z, w))
GLV(glUniform4fv, (GLint l, GLsizei c, const GLfloat* v), (l, c, v))
GLV(glUniform4iv, (GLint l, GLsizei c, const GLint* v), (l, c, v))
GLV(glUniformMatrix2fv, (GLint l, GLsizei c, GLboolean tr, const GLfloat* v), (l, c, tr, v))
GLV(glUniformMatrix3fv, (GLint l, GLsizei c, GLboolean tr, const GLfloat* v), (l, c, tr, v))
GLV(glUniformMatrix4fv, (GLint l, GLsizei c, GLboolean tr, const GLfloat* v), (l, c, tr, v))
GLV(glUseProgram, (GLuint p), (p))
GLV(glVertexAttrib4f, (GLuint i, float x, float y, float z, float w), (i, x, y, z, w))
GLV(glViewport, (GLint x, GLint y, GLsizei w, GLsizei h), (x, y, w, h))

// ---------------------------------------------------------------- framebuffers (framebuffer 0 redirect)
GLuint RealFbo(GLuint f) { return f ? f : detail::DefaultFbo(); }

void h_glBindFramebuffer(GLenum t, GLuint f) {
  ::glBindFramebuffer(t, RealFbo(f));
  Chk("glBindFramebuffer");
}

GLenum FixBack(GLenum e) { return (e == GL_BACK && detail::DefaultFbo()) ? GL_COLOR_ATTACHMENT0 : e; }
// Default-framebuffer discard tokens (GL_COLOR/DEPTH/STENCIL) -> FBO attachment points.
GLenum FixInvalidate(GLenum e) {
  switch (e) {
    case 0x1800: return GL_COLOR_ATTACHMENT0;
    case 0x1801: return GL_DEPTH_ATTACHMENT;
    case 0x1802: return GL_STENCIL_ATTACHMENT;
    default: return e;
  }
}
void h_glInvalidateFramebuffer(GLenum target, GLsizei n, const GLenum* att) {
  if (n < 0 || !att) return;
  std::vector<GLenum> a(att, att + n);
  for (auto& e : a) e = FixInvalidate(e);
  ::glInvalidateFramebuffer(target, n, a.data());
  Chk("glInvalidateFramebuffer");
}
void h_glDrawBuffers(GLsizei n, const GLenum* bufs) {
  if (n < 0 || !bufs) return;
  std::vector<GLenum> b(bufs, bufs + n);
  for (auto& e : b) e = FixBack(e);
  ::glDrawBuffers(n, b.data());
  Chk("glDrawBuffers");
}
void h_glReadBuffer(GLenum m) {
  ::glReadBuffer(FixBack(m));
  Chk("glReadBuffer");
}

// ---------------------------------------------------------------- state queries
GLenum h_glGetError() {
  Ctx* c = detail::CurrentCtx();
  if (c && c->pending_error != GL_NO_ERROR) {
    const GLenum e = c->pending_error;
    c->pending_error = GL_NO_ERROR;
    return e;
  }
  return ::glGetError();
}

constexpr GLenum kNumCompressedFormats = 0x86A2, kCompressedFormats = 0x86A3, kNumShaderBinaryFormats = 0x8DF9,
                 kShaderBinaryFormats = 0x8DF8, kNumProgramBinaryFormats = 0x87FE, kProgramBinaryFormats = 0x87FF,
                 kDrawFbBinding = 0x8CA6, kReadFbBinding = 0x8CAA;

// pnames whose host values must not reach the guest: compressed/binary formats (ANGLE lists ETC2/DXT/... that we do
// not announce, and the list could overflow the guest's buffer). Returns the number of GLints written, -1 if the
// pname is not special.
int SpecialGet(GLenum pname, GLint* out) {
  switch (pname) {
    case kNumCompressedFormats:
      *out = static_cast<GLint>(detail::GetCaps().compressed.size());
      return 1;
    case kCompressedFormats: {
      const auto& l = detail::GetCaps().compressed;
      for (std::size_t i = 0; i < l.size(); ++i) out[i] = static_cast<GLint>(l[i]);
      return static_cast<int>(l.size());
    }
    case kNumShaderBinaryFormats:
    case kNumProgramBinaryFormats:
      *out = 0;
      return 1;
    case kShaderBinaryFormats:
    case kProgramBinaryFormats: return 0;
    default: return -1;
  }
}

void h_glGetIntegerv(GLenum pname, GLint* params) {
  if (!params) return;
  if (SpecialGet(pname, params) >= 0) return;
  ::glGetIntegerv(pname, params);
  if ((pname == GL_FRAMEBUFFER_BINDING || pname == kDrawFbBinding || pname == kReadFbBinding) &&
      detail::DefaultFbo() && static_cast<GLuint>(params[0]) == detail::DefaultFbo())
    params[0] = 0;
  Chk("glGetIntegerv");
}
void h_glGetFloatv(GLenum pname, GLfloat* params) {
  if (!params) return;
  GLint tmp[32] = {};
  const int n = SpecialGet(pname, tmp);
  if (n >= 0) {
    for (int i = 0; i < n && i < 32; ++i) params[i] = static_cast<float>(tmp[i]);
    return;
  }
  ::glGetFloatv(pname, params);
  Chk("glGetFloatv");
}

std::uint64_t h_glGetString(GLenum name) {
  switch (name) {
    case GL_VENDOR: return detail::GuestString("Qualcomm");
    case GL_RENDERER: {
      const char* r = std::getenv("NOVA_GL_RENDERER");
      return detail::GuestString(r && *r ? r : "Adreno (TM) 650");
    }
    case GL_VERSION: return detail::GuestString("OpenGL ES 2.0 ANGLE (nova)");
    case GL_SHADING_LANGUAGE_VERSION: return detail::GuestString("OpenGL ES GLSL ES 1.00 (ANGLE)");
    case GL_EXTENSIONS: return detail::GuestString(detail::GetCaps().extensions);
    default: SetError(GL_INVALID_ENUM); return 0;
  }
}

// ---------------------------------------------------------------- client-side arrays
void h_glBindBuffer(GLenum target, GLuint b) {
  ::glBindBuffer(target, b);
  if (Ctx* c = detail::CurrentCtx()) {
    if (target == GL_ARRAY_BUFFER) c->array_buffer = b;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) c->element_buffer = b;
  }
  Chk("glBindBuffer");
}

const void* ArrayPtr(std::uint64_t p, bool buffer_bound) {
  return buffer_bound ? reinterpret_cast<const void*>(static_cast<std::uintptr_t>(p)) : g2h<const void>(p);
}
void h_glVertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, std::uint64_t ptr) {
  Ctx* c = detail::CurrentCtx();
  ::glVertexAttribPointer(i, size, type, norm, stride, ArrayPtr(ptr, c && c->array_buffer));
  Chk("glVertexAttribPointer");
}
void h_glDrawElements(GLenum mode, GLsizei count, GLenum type, std::uint64_t indices) {
  Ctx* c = detail::CurrentCtx();
  ::glDrawElements(mode, count, type, ArrayPtr(indices, c && c->element_buffer));
  Chk("glDrawElements");
}
void h_glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, std::uint64_t indices, GLsizei inst) {
  Ctx* c = detail::CurrentCtx();
  ::glDrawElementsInstanced(mode, count, type, ArrayPtr(indices, c && c->element_buffer), inst);
  Chk("glDrawElementsInstanced");
}
GLV(glDrawArraysInstanced, (GLenum m, GLint f, GLsizei c, GLsizei n), (m, f, c, n))
GLV(glVertexAttribDivisor, (GLuint i, GLuint d), (i, d))

// ---------------------------------------------------------------- buffers, with guest-memory shadows for mapping
struct Shadow {
  gaddr base = 0;
  gsize cap = 0;
  std::int64_t size = 0;
  bool mapped = false;
  std::int64_t off = 0, len = 0;
  GLbitfield access = 0;
};
std::unordered_map<GLuint, Shadow> g_shadows;  // by buffer name, under g_mu

GLuint BoundBuffer(GLenum target) {
  Ctx* c = detail::CurrentCtx();
  if (!c) return 0;
  if (target == GL_ARRAY_BUFFER) return c->array_buffer;
  if (target == GL_ELEMENT_ARRAY_BUFFER) return c->element_buffer;
  return 0;
}

void FreeShadow(Shadow& s) {
  if (s.base) GuestMemory::Unmap(s.base, s.cap);
  s = {};
}

// (Re)sizes the shadow of `buf`; `keep` retains the old contents.
bool ResizeShadow(Shadow& s, std::int64_t size) {
  if (size == s.size && s.base) return true;
  const gsize cap = (static_cast<gsize>(std::max<std::int64_t>(size, 1)) + kPageSize - 1) & ~(kPageSize - 1);
  if (cap != s.cap || !s.base) {
    if (s.base) GuestMemory::Unmap(s.base, s.cap);
    s.base = GuestMemory::MapAnywhere(Zone::Anon, cap, kProtRW, "gl-buffer-shadow");
    s.cap = s.base ? cap : 0;
    if (!s.base) {
      s.size = 0;
      return false;
    }
  }
  s.size = size;
  return true;
}

void h_glBufferData(GLenum target, std::int64_t size, const void* data, GLenum usage) {
  ::glBufferData(target, static_cast<GLsizeiptr>(size), data, usage);
  Chk("glBufferData");
  const GLuint buf = BoundBuffer(target);
  std::lock_guard lk(g_mu);
  auto it = g_shadows.find(buf);
  if (it == g_shadows.end() || size < 0) return;
  Shadow& s = it->second;
  if (!ResizeShadow(s, size)) return;
  std::memset(g2h(s.base), 0, static_cast<std::size_t>(size));
  if (data) std::memcpy(g2h(s.base), data, static_cast<std::size_t>(size));
}
void h_glBufferSubData(GLenum target, std::int64_t off, std::int64_t size, const void* data) {
  ::glBufferSubData(target, static_cast<GLintptr>(off), static_cast<GLsizeiptr>(size), data);
  Chk("glBufferSubData");
  const GLuint buf = BoundBuffer(target);
  std::lock_guard lk(g_mu);
  auto it = g_shadows.find(buf);
  if (it == g_shadows.end() || !data || off < 0 || size < 0) return;
  const Shadow& s = it->second;
  if (off + size <= s.size) std::memcpy(g2h<std::uint8_t>(s.base) + off, data, static_cast<std::size_t>(size));
}
void h_glDeleteBuffers(GLsizei n, const GLuint* b) {
  if (n <= 0 || !b) return;
  ::glDeleteBuffers(n, b);
  Chk("glDeleteBuffers");
  Ctx* c = detail::CurrentCtx();
  std::lock_guard lk(g_mu);
  for (GLsizei i = 0; i < n; ++i) {
    if (!b[i]) continue;
    if (c && c->array_buffer == b[i]) c->array_buffer = 0;
    if (c && c->element_buffer == b[i]) c->element_buffer = 0;
    if (auto it = g_shadows.find(b[i]); it != g_shadows.end()) {
      FreeShadow(it->second);
      g_shadows.erase(it);
    }
  }
}

// glMapBufferRange / glMapBufferOES: the guest gets a pointer into a shadow copy in guest memory; Unmap (or
// FlushMappedBufferRange) uploads the written range with glBufferSubData. The shadow starts as a copy of the buffer
// and is kept in step by glBufferData/glBufferSubData.
std::uint64_t MapRange(GLenum target, std::int64_t off, std::int64_t len, GLbitfield access) {
  const GLuint buf = BoundBuffer(target);
  if (!buf) {
    SetError(GL_INVALID_OPERATION);
    return 0;
  }
  GLint size = 0;
  ::glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
  if (off < 0 || len <= 0 || off + len > size) {
    SetError(GL_INVALID_VALUE);
    return 0;
  }
  if (!(access & (GL_MAP_READ_BIT | GL_MAP_WRITE_BIT))) {
    SetError(GL_INVALID_OPERATION);
    return 0;
  }
  std::lock_guard lk(g_mu);
  Shadow& s = g_shadows[buf];
  if (s.mapped) {
    SetError(GL_INVALID_OPERATION);
    return 0;
  }
  if (!s.base || s.size != size) {
    const bool fresh = !s.base;
    if (!ResizeShadow(s, size)) {
      g_shadows.erase(buf);
      SetError(GL_OUT_OF_MEMORY);
      return 0;
    }
    std::memset(g2h(s.base), 0, static_cast<std::size_t>(s.size));
    if (fresh && size > 0) {  // start from the buffer's current contents
      if (void* src = ::glMapBufferRange(target, 0, size, GL_MAP_READ_BIT)) {
        std::memcpy(g2h(s.base), src, static_cast<std::size_t>(size));
        ::glUnmapBuffer(target);
      }
      ::glGetError();
    }
  }
  s.mapped = true;
  s.off = off;
  s.len = len;
  s.access = access;
  return s.base + static_cast<gaddr>(off);
}
GLbitfield AccessBits(GLenum access) {
  switch (access) {
    case 0x88B8: return GL_MAP_READ_BIT;                      // GL_READ_ONLY
    case 0x88B9: return GL_MAP_WRITE_BIT;                     // GL_WRITE_ONLY(_OES)
    case 0x88BA: return GL_MAP_READ_BIT | GL_MAP_WRITE_BIT;   // GL_READ_WRITE
    default: return 0;
  }
}
std::uint64_t h_glMapBufferOES(GLenum target, GLenum access) {
  const GLbitfield bits = AccessBits(access);
  if (!bits) {
    SetError(GL_INVALID_ENUM);
    return 0;
  }
  GLint size = 0;
  ::glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
  return MapRange(target, 0, size, bits);
}
std::uint64_t h_glMapBufferRange(GLenum target, std::int64_t off, std::int64_t len, GLbitfield access) {
  return MapRange(target, off, len, access);
}
GLboolean h_glUnmapBuffer(GLenum target) {
  const GLuint buf = BoundBuffer(target);
  std::lock_guard lk(g_mu);
  auto it = g_shadows.find(buf);
  if (!buf || it == g_shadows.end() || !it->second.mapped) {
    SetError(GL_INVALID_OPERATION);
    return GL_FALSE;
  }
  Shadow& s = it->second;
  s.mapped = false;
  if ((s.access & GL_MAP_WRITE_BIT) && !(s.access & GL_MAP_FLUSH_EXPLICIT_BIT) && s.len > 0)
    ::glBufferSubData(target, static_cast<GLintptr>(s.off), static_cast<GLsizeiptr>(s.len), g2h(s.base) + s.off);
  Chk("glUnmapBuffer");
  return GL_TRUE;
}
void h_glFlushMappedBufferRange(GLenum target, std::int64_t off, std::int64_t len) {
  const GLuint buf = BoundBuffer(target);
  std::lock_guard lk(g_mu);
  auto it = g_shadows.find(buf);
  if (!buf || it == g_shadows.end() || !it->second.mapped || !(it->second.access & GL_MAP_FLUSH_EXPLICIT_BIT) ||
      off < 0 || len < 0 || off + len > it->second.len) {
    SetError(GL_INVALID_OPERATION);
    return;
  }
  const Shadow& s = it->second;
  if (len > 0)
    ::glBufferSubData(target, static_cast<GLintptr>(s.off + off), static_cast<GLsizeiptr>(len), g2h(s.base) + s.off + off);
}
void h_glGetBufferPointerv(GLenum target, GLenum /*pname*/, std::uint64_t* params) {
  if (!params) return;
  const GLuint buf = BoundBuffer(target);
  std::lock_guard lk(g_mu);
  auto it = g_shadows.find(buf);
  *params = (it != g_shadows.end() && it->second.mapped) ? it->second.base + static_cast<gaddr>(it->second.off) : 0;
}

GLV(glBindBufferBase, (GLenum t, GLuint i, GLuint b), (t, i, b))
GLV(glBindBufferRange, (GLenum t, GLuint i, GLuint b, std::int64_t off, std::int64_t sz), (t, i, b, static_cast<GLintptr>(off), static_cast<GLsizeiptr>(sz)))

// ---------------------------------------------------------------- shaders
void h_glShaderSource(GLuint shader, GLsizei count, std::uint64_t strings, const GLint* lengths) {
  if (count < 0 || !strings) {
    SetError(GL_INVALID_VALUE);
    return;
  }
  std::vector<const GLchar*> ptrs(static_cast<std::size_t>(count));
  std::vector<GLint> lens(static_cast<std::size_t>(count));
  for (GLsizei i = 0; i < count; ++i) {
    const std::uint64_t p = mem::R64(strings + 8 * static_cast<std::uint64_t>(i));
    ptrs[i] = p ? g2h<const GLchar>(p) : "";
    lens[i] = (p && lengths) ? lengths[i] : (p ? -1 : 0);
  }
  ::glShaderSource(shader, count, ptrs.data(), lens.data());
  Chk("glShaderSource");
}

std::string NumberedSource(GLuint s) {
  GLint n = 0;
  ::glGetShaderiv(s, GL_SHADER_SOURCE_LENGTH, &n);
  std::string src(n > 0 ? static_cast<std::size_t>(n) : 0, '\0');
  if (n > 0) ::glGetShaderSource(s, n, nullptr, src.data());
  if (!src.empty() && src.back() == '\0') src.pop_back();
  std::ostringstream os;
  std::istringstream is(src);
  std::string line;
  for (int ln = 1; std::getline(is, line); ++ln) os << std::format("{:4} | {}\n", ln, line);
  return os.str();
}

void h_glCompileShader(GLuint s) {
  ::glCompileShader(s);
  GLint ok = GL_FALSE;
  ::glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok || g_log_shaders) {
    GLint n = 0, type = 0;
    ::glGetShaderiv(s, GL_INFO_LOG_LENGTH, &n);
    ::glGetShaderiv(s, GL_SHADER_TYPE, &type);
    std::string info(n > 0 ? static_cast<std::size_t>(n) : 0, '\0');
    if (n > 0) ::glGetShaderInfoLog(s, n, nullptr, info.data());
    const char* kind = type == GL_VERTEX_SHADER ? "vertex" : "fragment";
    if (!ok) log::Error("[gles] {} shader {} failed to compile:\n{}\n--- source ---\n{}", kind, s, info.c_str(), NumberedSource(s));
    else log::Info("[gles] {} shader {} compiled:\n{}", kind, s, NumberedSource(s));
  }
  Chk("glCompileShader");
}

void h_glLinkProgram(GLuint p) {
  ::glLinkProgram(p);
  GLint ok = GL_FALSE;
  ::glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    GLint n = 0;
    ::glGetProgramiv(p, GL_INFO_LOG_LENGTH, &n);
    std::string info(n > 0 ? static_cast<std::size_t>(n) : 0, '\0');
    if (n > 0) ::glGetProgramInfoLog(p, n, nullptr, info.data());
    log::Error("[gles] program {} failed to link: {}", p, info.c_str());
  }
  Chk("glLinkProgram");
}

void h_glTransformFeedbackVaryings(GLuint program, GLsizei count, std::uint64_t varyings, GLenum mode) {
  if (count < 0 || !varyings) {
    SetError(GL_INVALID_VALUE);
    return;
  }
  std::vector<const GLchar*> v(static_cast<std::size_t>(count));
  for (GLsizei i = 0; i < count; ++i) v[i] = g2h<const GLchar>(mem::R64(varyings + 8 * static_cast<std::uint64_t>(i)));
  ::glTransformFeedbackVaryings(program, count, v.data(), mode);
  Chk("glTransformFeedbackVaryings");
}
GLV(glGetTransformFeedbackVarying, (GLuint p, GLuint i, GLsizei bs, GLsizei* len, GLsizei* sz, GLenum* ty, GLchar* name), (p, i, bs, len, sz, ty, name))
GLV(glBeginTransformFeedback, (GLenum m), (m))
GLV(glEndTransformFeedback, (), ())
GLV(glBindTransformFeedback, (GLenum t, GLuint id), (t, id))
GLV(glDeleteTransformFeedbacks, (GLsizei n, const GLuint* ids), (n, ids))
GLV(glGenTransformFeedbacks, (GLsizei n, GLuint* ids), (n, ids))

// ---------------------------------------------------------------- compressed textures
enum class Route { Pass, Etc2, Soft };

Route RouteFor(GLenum fmt) {
  if (fmt == texdecode::kETC1_RGB8) {
    if (g_etc1_soft) return Route::Soft;
    static const Route r = [] {
      const detail::Caps& c = detail::GetCaps();
      if (ProbeCompressed(texdecode::kETC1_RGB8)) return Route::Pass;
      if (c.es_major >= 3 && ProbeCompressed(0x9274)) return Route::Etc2;
      return Route::Soft;
    }();
    return r;
  }
  if (texdecode::IsPVRTC(fmt) || texdecode::IsATC(fmt)) return Route::Soft;  // D3D11 has neither
  return Route::Pass;
}

bool SoftDecode(GLenum fmt, GLsizei w, GLsizei h, GLsizei size, const void* data, std::vector<std::uint8_t>& out) {
  if (size < 0 || !texdecode::Decode(fmt, static_cast<const std::uint8_t*>(data), static_cast<std::uint32_t>(size), w, h, out)) {
    log::Warn("[gles] cannot decode compressed texture 0x{:04X} {}x{} ({} bytes)", fmt, w, h, size);
    SetError(GL_INVALID_VALUE);
    return false;
  }
  return true;
}

void h_glCompressedTexImage2D(GLenum t, GLint l, GLenum ifmt, GLsizei w, GLsizei h, GLint b, GLsizei size, const void* data) {
  {
    static std::mutex mu;
    static std::vector<GLenum> seen;
    std::lock_guard lk(mu);
    if (std::find(seen.begin(), seen.end(), ifmt) == seen.end()) {
      seen.push_back(ifmt);
      log::Info("[gles] first compressed texture in format 0x{:04X} ({}x{}, route {})", ifmt, w, h, static_cast<int>(RouteFor(ifmt)));
    }
  }
  switch (RouteFor(ifmt)) {
    case Route::Pass: ::glCompressedTexImage2D(t, l, ifmt, w, h, b, size, data); break;
    case Route::Etc2: ::glCompressedTexImage2D(t, l, 0x9274, w, h, b, size, data); break;
    case Route::Soft: {
      if (w < 0 || h < 0 || b != 0) {
        SetError(GL_INVALID_VALUE);
        return;
      }
      std::vector<std::uint8_t> rgba;
      if (data && w > 0 && h > 0 && !SoftDecode(ifmt, w, h, size, data, rgba)) return;
      GLint align = 4;
      ::glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
      ::glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      ::glTexImage2D(t, l, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.empty() ? nullptr : rgba.data());
      ::glPixelStorei(GL_UNPACK_ALIGNMENT, align);
      break;
    }
  }
  Chk("glCompressedTexImage2D");
}
void h_glCompressedTexSubImage2D(GLenum t, GLint l, GLint xo, GLint yo, GLsizei w, GLsizei h, GLenum fmt, GLsizei size, const void* data) {
  switch (RouteFor(fmt)) {
    case Route::Pass: ::glCompressedTexSubImage2D(t, l, xo, yo, w, h, fmt, size, data); break;
    case Route::Etc2: ::glCompressedTexSubImage2D(t, l, xo, yo, w, h, 0x9274, size, data); break;
    case Route::Soft: {
      std::vector<std::uint8_t> rgba;
      if (!data || w <= 0 || h <= 0 || !SoftDecode(fmt, w, h, size, data, rgba)) return;
      GLint align = 4;
      ::glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
      ::glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      ::glTexSubImage2D(t, l, xo, yo, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
      ::glPixelStorei(GL_UNPACK_ALIGNMENT, align);
      break;
    }
  }
  Chk("glCompressedTexSubImage2D");
}

// ---------------------------------------------------------------- ES 3.0 functions the engine looks up
GLV(glTexImage3D, (GLenum t, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLsizei d, GLint b, GLenum f, GLenum ty, const void* px), (t, l, ifmt, w, h, d, b, f, ty, px))
GLV(glTexSubImage3D, (GLenum t, GLint l, GLint xo, GLint yo, GLint zo, GLsizei w, GLsizei h, GLsizei d, GLenum f, GLenum ty, const void* px), (t, l, xo, yo, zo, w, h, d, f, ty, px))
GLV(glCompressedTexImage3D, (GLenum t, GLint l, GLenum ifmt, GLsizei w, GLsizei h, GLsizei d, GLint b, GLsizei sz, const void* data), (t, l, ifmt, w, h, d, b, sz, data))
GLV(glCompressedTexSubImage3D, (GLenum t, GLint l, GLint xo, GLint yo, GLint zo, GLsizei w, GLsizei h, GLsizei d, GLenum f, GLsizei sz, const void* data), (t, l, xo, yo, zo, w, h, d, f, sz, data))
// Immutable storage: compressed formats follow the same route as glCompressedTex*Image2D (software-decoded ones become
// RGBA8, which is what the decoded glTexSubImage2D uploads), unsized formats get their sized equivalent.
GLenum StorageFormat(GLenum f) {
  if (f == texdecode::kETC1_RGB8 || texdecode::IsPVRTC(f) || texdecode::IsATC(f)) {
    switch (RouteFor(f)) {
      case Route::Pass: return f;
      case Route::Etc2: return 0x9274;
      case Route::Soft: return GL_RGBA8;
    }
  }
  switch (f) {
    case GL_RGBA: return GL_RGBA8;
    case GL_RGB: return GL_RGB8;
    case GL_ALPHA: return 0x803C;            // GL_ALPHA8_EXT
    case GL_LUMINANCE: return 0x8040;        // GL_LUMINANCE8_EXT
    case GL_LUMINANCE_ALPHA: return 0x8045;  // GL_LUMINANCE8_ALPHA8_EXT
    case 0x80E1: return 0x93A1;              // GL_BGRA_EXT -> GL_BGRA8_EXT
    default: return f;
  }
}
void h_glTexStorage2D(GLenum t, GLsizei lv, GLenum ifmt, GLsizei w, GLsizei h) {
  const GLenum f = StorageFormat(ifmt);
  ::glTexStorage2D(t, lv, f, w, h);
  static std::mutex mu;
  static std::vector<GLenum> seen;
  std::lock_guard lk(mu);
  if (std::find(seen.begin(), seen.end(), ifmt) == seen.end()) {
    seen.push_back(ifmt);
    log::Info("[gles] first glTexStorage2D in format 0x{:04X} -> 0x{:04X} ({}x{}, {} levels)", ifmt, f, w, h, lv);
  }
  Chk("glTexStorage2D");
}
GLV(glTexStorage3D, (GLenum t, GLsizei lv, GLenum ifmt, GLsizei w, GLsizei h, GLsizei d), (t, lv, ifmt, w, h, d))
GLV(glBlitFramebuffer, (GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield m, GLenum f), (sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, m, f))
GLV(glRenderbufferStorageMultisample, (GLenum t, GLsizei s, GLenum f, GLsizei w, GLsizei h), (t, s, f, w, h))
GLV(glGenQueries, (GLsizei n, GLuint* ids), (n, ids))
GLV(glDeleteQueries, (GLsizei n, const GLuint* ids), (n, ids))
GLV(glBeginQuery, (GLenum t, GLuint id), (t, id))
GLV(glEndQuery, (GLenum t), (t))
GLV(glGetQueryObjectuiv, (GLuint id, GLenum pn, GLuint* v), (id, pn, v))

std::uint64_t h_glFenceSync(GLenum cond, GLbitfield flags) {
  return reinterpret_cast<std::uint64_t>(::glFenceSync(cond, flags));
}
GLenum h_glClientWaitSync(std::uint64_t sync, GLbitfield flags, std::uint64_t timeout) {
  return ::glClientWaitSync(reinterpret_cast<GLsync>(sync), flags, timeout);
}
void h_glDeleteSync(std::uint64_t sync) { ::glDeleteSync(reinterpret_cast<GLsync>(sync)); }
void h_glGetSynciv(std::uint64_t sync, GLenum pname, GLsizei bufsize, GLsizei* length, GLint* values) {
  ::glGetSynciv(reinterpret_cast<GLsync>(sync), pname, bufsize, length, values);
}

#undef GLV
#undef GLR

}  // namespace

namespace detail {

void ResetGlState() {
  std::lock_guard lk(g_mu);
  g_caps = Caps{};
  for (auto& [id, s] : g_shadows) FreeShadow(s);
  g_shadows.clear();
}

void RegisterGles() {
  g_check = EnvOn("NOVA_GL_CHECK");
  g_log_shaders = EnvOn("NOVA_GL_LOG_SHADERS");
  g_etc1_soft = EnvOn("NOVA_GL_ETC1_SOFT");

#define REG(name) NOVA_HLE_FAST(#name, h_##name)
#define ALIAS(alias, fn) NOVA_HLE_FAST(alias, h_##fn)
  // The 96 libGLESv2 imports of libNOVA.so.
  REG(glActiveTexture); REG(glAttachShader); REG(glBindBuffer); REG(glBindFramebuffer); REG(glBindRenderbuffer);
  REG(glBindTexture); REG(glBlendColor); REG(glBlendEquation); REG(glBlendFunc); REG(glBufferData);
  REG(glBufferSubData); REG(glCheckFramebufferStatus); REG(glClear); REG(glClearColor); REG(glClearDepthf);
  REG(glClearStencil); REG(glColorMask); REG(glCompileShader); REG(glCompressedTexImage2D);
  REG(glCompressedTexSubImage2D); REG(glCopyTexSubImage2D); REG(glCreateProgram); REG(glCreateShader);
  REG(glCullFace); REG(glDeleteBuffers); REG(glDeleteFramebuffers); REG(glDeleteProgram);
  REG(glDeleteRenderbuffers); REG(glDeleteShader); REG(glDeleteTextures); REG(glDepthFunc); REG(glDepthMask);
  REG(glDepthRangef); REG(glDisable); REG(glDisableVertexAttribArray); REG(glDrawArrays); REG(glDrawElements);
  REG(glEnable); REG(glEnableVertexAttribArray); REG(glFinish); REG(glFlush); REG(glFramebufferRenderbuffer);
  REG(glFramebufferTexture2D); REG(glFrontFace); REG(glGenBuffers); REG(glGenFramebuffers);
  REG(glGenRenderbuffers); REG(glGenTextures); REG(glGenerateMipmap); REG(glGetActiveAttrib);
  REG(glGetActiveUniform); REG(glGetAttribLocation); REG(glGetError); REG(glGetFloatv); REG(glGetIntegerv);
  REG(glGetProgramInfoLog); REG(glGetProgramiv); REG(glGetShaderInfoLog); REG(glGetShaderSource);
  REG(glGetShaderiv); REG(glGetString); REG(glGetUniformLocation); REG(glLineWidth); REG(glLinkProgram);
  REG(glPixelStorei); REG(glPolygonOffset); REG(glReadPixels); REG(glRenderbufferStorage); REG(glSampleCoverage);
  REG(glScissor); REG(glShaderSource); REG(glStencilFunc); REG(glStencilMask); REG(glStencilOp);
  REG(glTexImage2D); REG(glTexParameterf); REG(glTexParameteri); REG(glTexSubImage2D); REG(glUniform1f);
  REG(glUniform1fv); REG(glUniform1i); REG(glUniform1iv); REG(glUniform2fv); REG(glUniform2iv); REG(glUniform3fv);
  REG(glUniform3iv); REG(glUniform4f); REG(glUniform4fv); REG(glUniform4iv); REG(glUniformMatrix2fv);
  REG(glUniformMatrix3fv); REG(glUniformMatrix4fv); REG(glUseProgram); REG(glVertexAttrib4f);
  REG(glVertexAttribPointer); REG(glViewport);

  // Extension functions looked up through eglGetProcAddress. Everything ANGLE implements (as ES 3.0 core with the same
  // signature) is exposed under each name the engine tries; the rest stays unregistered, so eglGetProcAddress returns
  // NULL for it: NV/APPLE fences, glFramebufferTexture2DMultisample{EXT,IMG}, glResolveMultisampleFramebufferAPPLE,
  // glTexStorage1D*, glGet/ProgramBinaryOES (we report no binary formats), glAlphaFuncQCOM, and the NV variants of
  // transform feedback (different semantics).
  // Instancing and divisors.
  ALIAS("glDrawArraysInstanced", glDrawArraysInstanced); ALIAS("glDrawArraysInstancedARB", glDrawArraysInstanced);
  ALIAS("glDrawArraysInstancedEXT", glDrawArraysInstanced); ALIAS("glDrawArraysInstancedNV", glDrawArraysInstanced);
  ALIAS("glDrawElementsInstanced", glDrawElementsInstanced); ALIAS("glDrawElementsInstancedARB", glDrawElementsInstanced);
  ALIAS("glDrawElementsInstancedEXT", glDrawElementsInstanced); ALIAS("glDrawElementsInstancedNV", glDrawElementsInstanced);
  ALIAS("glVertexAttribDivisor", glVertexAttribDivisor); ALIAS("glVertexAttribDivisorARB", glVertexAttribDivisor);
  ALIAS("glVertexAttribDivisorEXT", glVertexAttribDivisor); ALIAS("glVertexAttribDivisorNV", glVertexAttribDivisor);
  // Buffer mapping and ranges (shadow copies).
  ALIAS("glBindBufferBase", glBindBufferBase); ALIAS("glBindBufferBaseEXT", glBindBufferBase);
  ALIAS("glBindBufferBaseNV", glBindBufferBase); ALIAS("glBindBufferRange", glBindBufferRange);
  ALIAS("glBindBufferRangeEXT", glBindBufferRange); ALIAS("glBindBufferRangeNV", glBindBufferRange);
  ALIAS("glFlushMappedBufferRange", glFlushMappedBufferRange); ALIAS("glFlushMappedBufferRangeEXT", glFlushMappedBufferRange);
  ALIAS("glGetBufferPointerv", glGetBufferPointerv); ALIAS("glGetBufferPointervARB", glGetBufferPointerv);
  ALIAS("glGetBufferPointervOES", glGetBufferPointerv);
  ALIAS("glMapBuffer", glMapBufferOES); ALIAS("glMapBufferARB", glMapBufferOES); ALIAS("glMapBufferOES", glMapBufferOES);
  ALIAS("glMapBufferRange", glMapBufferRange); ALIAS("glMapBufferRangeEXT", glMapBufferRange);
  ALIAS("glUnmapBuffer", glUnmapBuffer); ALIAS("glUnmapBufferARB", glUnmapBuffer); ALIAS("glUnmapBufferOES", glUnmapBuffer);
  // Framebuffers and renderbuffers.
  ALIAS("glBindFramebufferEXT", glBindFramebuffer); ALIAS("glBindRenderbufferEXT", glBindRenderbuffer);
  ALIAS("glBlitFramebuffer", glBlitFramebuffer); ALIAS("glBlitFramebufferANGLE", glBlitFramebuffer);
  ALIAS("glBlitFramebufferEXT", glBlitFramebuffer); ALIAS("glBlitFramebufferNV", glBlitFramebuffer);
  ALIAS("glCheckFramebufferStatusEXT", glCheckFramebufferStatus); ALIAS("glDeleteFramebuffersEXT", glDeleteFramebuffers);
  ALIAS("glDeleteRenderbuffersEXT", glDeleteRenderbuffers);
  ALIAS("glDiscardFramebufferEXT", glInvalidateFramebuffer); ALIAS("glInvalidateFramebuffer", glInvalidateFramebuffer);
  ALIAS("glDrawBuffers", glDrawBuffers); ALIAS("glDrawBuffersARB", glDrawBuffers); ALIAS("glDrawBuffersNV", glDrawBuffers);
  ALIAS("glFramebufferRenderbufferEXT", glFramebufferRenderbuffer); ALIAS("glFramebufferTexture2DEXT", glFramebufferTexture2D);
  ALIAS("glGenFramebuffersEXT", glGenFramebuffers); ALIAS("glGenRenderbuffersEXT", glGenRenderbuffers);
  ALIAS("glGenerateMipmapEXT", glGenerateMipmap); ALIAS("glReadBuffer", glReadBuffer); ALIAS("glReadBufferNV", glReadBuffer);
  ALIAS("glRenderbufferStorageEXT", glRenderbufferStorage);
  ALIAS("glRenderbufferStorageMultisample", glRenderbufferStorageMultisample);
  ALIAS("glRenderbufferStorageMultisampleANGLE", glRenderbufferStorageMultisample);
  ALIAS("glRenderbufferStorageMultisampleAPPLE", glRenderbufferStorageMultisample);
  ALIAS("glRenderbufferStorageMultisampleEXT", glRenderbufferStorageMultisample);
  ALIAS("glRenderbufferStorageMultisampleIMG", glRenderbufferStorageMultisample);
  ALIAS("glRenderbufferStorageMultisampleNV", glRenderbufferStorageMultisample);
  // 3D textures and immutable storage.
  ALIAS("glTexImage3D", glTexImage3D); ALIAS("glTexImage3DOES", glTexImage3D);
  ALIAS("glTexSubImage3D", glTexSubImage3D); ALIAS("glTexSubImage3DOES", glTexSubImage3D);
  ALIAS("glCompressedTexImage3D", glCompressedTexImage3D); ALIAS("glCompressedTexImage3DOES", glCompressedTexImage3D);
  ALIAS("glCompressedTexSubImage3D", glCompressedTexSubImage3D);
  ALIAS("glCompressedTexSubImage3DOES", glCompressedTexSubImage3D);
  ALIAS("glTexStorage2D", glTexStorage2D); ALIAS("glTexStorage2DEXT", glTexStorage2D);
  ALIAS("glTexStorage3D", glTexStorage3D); ALIAS("glTexStorage3DEXT", glTexStorage3D);
  // Queries and sync objects.
  ALIAS("glGenQueries", glGenQueries); ALIAS("glGenQueriesARB", glGenQueries); ALIAS("glGenQueriesEXT", glGenQueries);
  ALIAS("glDeleteQueries", glDeleteQueries); ALIAS("glDeleteQueriesARB", glDeleteQueries);
  ALIAS("glDeleteQueriesEXT", glDeleteQueries);
  ALIAS("glBeginQuery", glBeginQuery); ALIAS("glBeginQueryARB", glBeginQuery); ALIAS("glBeginQueryEXT", glBeginQuery);
  ALIAS("glEndQuery", glEndQuery); ALIAS("glEndQueryARB", glEndQuery); ALIAS("glEndQueryEXT", glEndQuery);
  ALIAS("glGetQueryObjectuiv", glGetQueryObjectuiv); ALIAS("glGetQueryObjectuivARB", glGetQueryObjectuiv);
  ALIAS("glGetQueryObjectuivEXT", glGetQueryObjectuiv);
  ALIAS("glFenceSync", glFenceSync); ALIAS("glFenceSyncAPPLE", glFenceSync);
  ALIAS("glClientWaitSync", glClientWaitSync); ALIAS("glClientWaitSyncAPPLE", glClientWaitSync);
  ALIAS("glDeleteSync", glDeleteSync); ALIAS("glDeleteSyncAPPLE", glDeleteSync);
  ALIAS("glGetSynciv", glGetSynciv); ALIAS("glGetSyncivAPPLE", glGetSynciv);
  // Transform feedback (core ES 3.0 and EXT names only).
  ALIAS("glBeginTransformFeedback", glBeginTransformFeedback); ALIAS("glBeginTransformFeedbackEXT", glBeginTransformFeedback);
  ALIAS("glEndTransformFeedback", glEndTransformFeedback); ALIAS("glEndTransformFeedbackEXT", glEndTransformFeedback);
  ALIAS("glBindTransformFeedback", glBindTransformFeedback);
  ALIAS("glDeleteTransformFeedbacks", glDeleteTransformFeedbacks);
  ALIAS("glGenTransformFeedbacks", glGenTransformFeedbacks);
  ALIAS("glGetTransformFeedbackVarying", glGetTransformFeedbackVarying);
  ALIAS("glGetTransformFeedbackVaryingEXT", glGetTransformFeedbackVarying);
  ALIAS("glTransformFeedbackVaryings", glTransformFeedbackVaryings);
  ALIAS("glTransformFeedbackVaryingsEXT", glTransformFeedbackVaryings);
#undef REG
#undef ALIAS
}

}  // namespace detail

// ================================================================ public: RegisterHle
void RegisterHle() {
  detail::RegisterEgl();
  detail::RegisterGles();
  android::RegisterNativeWindow();
  log::Info("[gfx] registered libEGL, libGLESv2 and ANativeWindow handlers");
}

}  // namespace nova::gfx
