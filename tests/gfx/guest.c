/* Guest side of the gfx smoke test (tests/gfx/gfx_test.cpp): an arm64 program that does what libNOVA does at start-up,
 * through stub addresses of the EGL/GLES HLE: display, config, window surface, context, clear, a draw with client-side
 * vertex data and guest-memory shader strings, eglSwapBuffers and read-backs. Results go to out[]. */
typedef long i64;
typedef unsigned long u64;
typedef int i32;
typedef unsigned int u32;
typedef unsigned char u8;

enum {
  S_eglGetDisplay, S_eglInitialize, S_eglChooseConfig, S_eglCreateWindowSurface, S_eglCreateContext,
  S_eglMakeCurrent, S_eglSwapBuffers, S_eglGetError, S_eglQuerySurface, S_eglGetProcAddress,
  S_glClearColor, S_glClear, S_glViewport, S_glReadPixels, S_glGetString, S_glGetIntegerv, S_glGetError,
  S_glCreateShader, S_glShaderSource, S_glCompileShader, S_glGetShaderiv, S_glCreateProgram, S_glAttachShader,
  S_glLinkProgram, S_glUseProgram, S_glGetAttribLocation, S_glEnableVertexAttribArray, S_glVertexAttribPointer,
  S_glDrawArrays, S_glGenBuffers, S_glBindBuffer, S_glBufferData
};

typedef u64 (*f_u64_u64)(u64);
typedef u32 (*f_u32_u64_p_p)(u64, i32*, i32*);
typedef u32 (*f_choose)(u64, const i32*, u64*, i32, i32*);
typedef u64 (*f_winsurf)(u64, u64, u64, const i32*);
typedef u64 (*f_ctx)(u64, u64, u64, const i32*);
typedef u32 (*f_mc)(u64, u64, u64, u64);
typedef u32 (*f_swap)(u64, u64);
typedef i32 (*f_err)(void);
typedef u32 (*f_qs)(u64, u64, i32, i32*);
typedef u64 (*f_gpa)(const char*);
typedef void (*f_cc)(float, float, float, float);
typedef void (*f_clear)(u32);
typedef void (*f_vp)(i32, i32, i32, i32);
typedef void (*f_rp)(i32, i32, i32, i32, u32, u32, void*);
typedef const char* (*f_gs)(u32);
typedef void (*f_gi)(u32, i32*);
typedef u32 (*f_cs)(u32);
typedef void (*f_ss)(u32, i32, const char* const*, const i32*);
typedef void (*f_u)(u32);
typedef void (*f_gsiv)(u32, u32, i32*);
typedef u32 (*f_cp)(void);
typedef void (*f_ab)(u32, u32);
typedef i32 (*f_gal)(u32, const char*);
typedef void (*f_vap)(u32, i32, u32, u8, i32, const void*);
typedef void (*f_da)(u32, i32, i32);
typedef void (*f_gb)(i32, u32*);
typedef void (*f_bb)(u32, u32);
typedef void (*f_bd)(u32, i64, const void*, u32);
typedef void* (*f_map)(u32, u32);
typedef u8 (*f_unmap)(u32);
typedef u32 (*f_gerr)(void);

#define T(slot, type) ((type)stubs[slot])

void run(u64* stubs, i64* out) {
  int k = 0;
  i32 major = 0, minor = 0, n = 0, w = 0, h = 0, fb = -1;
  u64 cfg = 0;
  u8 px[4];

  const u64 dpy = T(S_eglGetDisplay, f_u64_u64)(0);
  out[k++] = (i64)T(S_eglInitialize, f_u32_u64_p_p)(dpy, &major, &minor);
  out[k++] = major * 10 + minor;

  static const i32 cattr[] ={0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3025, 16, 0x3040, 4, 0x3142 /*RECORDABLE_ANDROID*/, 1, 0x3038};
  out[k++] = (i64)T(S_eglChooseConfig, f_choose)(dpy, cattr, &cfg, 1, &n);
  out[k++] = n;
  const u64 surf = T(S_eglCreateWindowSurface, f_winsurf)(dpy, cfg, 1, 0);
  static const i32 xattr[] ={0x3098, 2, 0x3038};
  const u64 ctx = T(S_eglCreateContext, f_ctx)(dpy, cfg, 0, xattr);
  out[k++] = (i64)T(S_eglMakeCurrent, f_mc)(dpy, surf, surf, ctx);
  T(S_eglQuerySurface, f_qs)(dpy, surf, 0x3057, &w);
  T(S_eglQuerySurface, f_qs)(dpy, surf, 0x3056, &h);
  out[k++] = w;
  out[k++] = h;
  T(S_glGetIntegerv, f_gi)(0x8CA6, &fb);
  out[k++] = fb;
  out[k++] = (i64)(u64)T(S_glGetString, f_gs)(0x1F03);  /* guest address of the extension string */
  out[k++] = (i64)(u64)T(S_glGetString, f_gs)(0x1F01);  /* GL_RENDERER */

  /* clear, read one pixel */
  T(S_glViewport, f_vp)(0, 0, w, h);
  T(S_glClearColor, f_cc)(0.25f, 0.5f, 0.75f, 1.0f);
  T(S_glClear, f_clear)(0x4100);
  T(S_glReadPixels, f_rp)(1, 1, 1, 1, 0x1908, 0x1401, px);
  out[k++] = px[0] | (px[1] << 8) | (px[2] << 16) | ((i64)px[3] << 24);

  /* shaders from guest memory, client-side vertex array, full-screen red triangle */
  static const char vs[] = "attribute vec4 p; void main() { gl_Position = p; }";
  static const char fs[] = "precision mediump float; void main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }";
  const char* vsrc[1] = {vs};
  const char* fsrc[1] = {fs};
  const u32 sv = T(S_glCreateShader, f_cs)(0x8B31), sf = T(S_glCreateShader, f_cs)(0x8B30);
  T(S_glShaderSource, f_ss)(sv, 1, vsrc, 0);
  T(S_glShaderSource, f_ss)(sf, 1, fsrc, 0);
  T(S_glCompileShader, f_u)(sv);
  T(S_glCompileShader, f_u)(sf);
  i32 okv = 0, okf = 0;
  T(S_glGetShaderiv, f_gsiv)(sv, 0x8B81, &okv);
  T(S_glGetShaderiv, f_gsiv)(sf, 0x8B81, &okf);
  out[k++] = okv && okf;
  const u32 prog = T(S_glCreateProgram, f_cp)();
  T(S_glAttachShader, f_ab)(prog, sv);
  T(S_glAttachShader, f_ab)(prog, sf);
  T(S_glLinkProgram, f_u)(prog);
  T(S_glUseProgram, f_u)(prog);
  const i32 loc = T(S_glGetAttribLocation, f_gal)(prog, "p");
  static const float tri[12] = {-1.f, -1.f, 0.f, 1.f, 3.f, -1.f, 0.f, 1.f, -1.f, 3.f, 0.f, 1.f};
  T(S_glBindBuffer, f_bb)(0x8892, 0);
  T(S_glEnableVertexAttribArray, f_u)((u32)loc);
  T(S_glVertexAttribPointer, f_vap)((u32)loc, 4, 0x1406, 0, 0, tri);
  T(S_glDrawArrays, f_da)(4, 0, 3);
  T(S_glReadPixels, f_rp)(w / 2, h / 2, 1, 1, 0x1908, 0x1401, px);
  out[k++] = px[0] | (px[1] << 8) | (px[2] << 16) | ((i64)px[3] << 24);

  /* present, then read again: the framebuffer-0 redirect is a persistent host FBO */
  out[k++] = (i64)T(S_eglSwapBuffers, f_swap)(dpy, surf);
  out[k++] = T(S_eglGetError, f_err)();
  T(S_glReadPixels, f_rp)(w / 2, h / 2, 1, 1, 0x1908, 0x1401, px);
  out[k++] = px[0] | (px[1] << 8) | (px[2] << 16) | ((i64)px[3] << 24);

  /* eglGetProcAddress: an extension ANGLE implements, and one it does not */
  const u64 p_map = T(S_eglGetProcAddress, f_gpa)("glMapBufferOES");
  out[k++] = (i64)p_map;
  out[k++] = (i64)T(S_eglGetProcAddress, f_gpa)("glGenFencesNV");
  const u64 p_unmap = T(S_eglGetProcAddress, f_gpa)("glUnmapBufferOES");

  /* glMapBufferOES shadow: map, write, unmap */
  u32 buf = 0;
  T(S_glGenBuffers, f_gb)(1, &buf);
  T(S_glBindBuffer, f_bb)(0x8892, buf);
  T(S_glBufferData, f_bd)(0x8892, 256, 0, 0x88E8);
  u8* m = (u8*)((f_map)p_map)(0x8892, 0x88B9);
  out[k++] = m != 0;
  if (m) {
    for (int i = 0; i < 256; ++i) m[i] = (u8)i;
    out[k++] = ((f_unmap)p_unmap)(0x8892);
  } else {
    out[k++] = 0;
  }
  out[k++] = T(S_glGetError, f_gerr)();
}
