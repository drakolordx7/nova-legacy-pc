// Window, ANGLE (OpenGL ES over Direct3D 11) and the guest-facing EGL/GLES/ANativeWindow HLE.
//
//   SDL3 window  --(HWND)-->  ANGLE EGL display/surface  <--  guest eglCreate*/eglMakeCurrent/eglSwapBuffers
//
// The guest never touches the window's own framebuffer: for every context made current on the window surface, the
// guest's "framebuffer 0" is a host FBO at the game's render resolution. eglSwapBuffers blits that FBO to the window
// at its native size (letterboxed only when the aspect ratios differ) and presents it.
//
// Threading: SDL_Init, Init(), PumpEvents(), SetFullscreen(), SetRelativeMouse() and Shutdown() belong to one "main"
// thread (the one that created the window). All GL runs on whichever thread called eglMakeCurrent (the guest's GL
// thread); BeginFrame/EndFrame must be called on that thread. SetVsync/SetTargetFps/SetRenderSize may be called from
// any thread and take effect at the next present.
#pragma once
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>

#include <cstdint>
#include <string>
#include <vector>

namespace nova::gfx {

enum class WindowMode {
  Windowed,
  Borderless,  // borderless fullscreen at the desktop resolution
  Exclusive,   // fullscreen with a display-mode change (fullscreen_width/height/refresh; 0 = desktop mode)
};

struct Config {
  std::string title = "N.O.V.A. Legacy";
  int width = 1280;   // initial window client size in SDL (screen) coordinates; any size
  int height = 720;
  WindowMode mode = WindowMode::Windowed;
  int display = 0;    // display index used for fullscreen (0 = primary)
  int fullscreen_width = 0, fullscreen_height = 0;  // Exclusive: wanted mode (0 = desktop size)
  float fullscreen_refresh = 0.0f;                  // Exclusive: wanted refresh rate (0 = best)
  bool resizable = true;
  bool hidden = false;       // create the window hidden (tests)
  bool vsync = true;         // runtime-switchable with SetVsync
  int target_fps = 0;        // frame pacing hook: 0 = unlimited (no cap)
  int render_width = 0;      // size of the guest's framebuffer 0; 0 = the window's pixel size at Init
  int render_height = 0;
  bool prefer_gles3 = true;  // guest contexts are created as ES 3.0 when ANGLE has it (the guest sees "ES 2.0")
  bool allow_warp = true;    // fall back to the WARP software rasteriser if no D3D11 hardware device initialises
};

// Creates the SDL3 window and the ANGLE display. Returns false (and logs) on failure. Idempotent.
bool Init(const Config& cfg);
void Shutdown();
bool IsInitialized();

// ---- window (main thread) ----
SDL_Window* Window();
// Window drawable size in pixels (high-DPI aware) and the display scale (pixels per SDL coordinate).
void WindowPixelSize(int& w, int& h);
float DisplayScale();
bool SetFullscreen(WindowMode mode, int width = 0, int height = 0, float refresh = 0.0f);
WindowMode GetWindowMode();
void SetWindowSize(int w, int h);  // windowed client size, SDL coordinates
// SDL3 relative mouse mode (raw, unaccelerated deltas; the cursor is hidden and captured).
void SetRelativeMouse(bool enabled);
bool RelativeMouse();
// Pumps SDL and returns every event received (the caller handles input). Window size changes and close requests are
// also tracked internally (see CloseRequested). Main thread only.
std::vector<SDL_Event> PumpEvents();
bool CloseRequested();

// ---- presentation ----
void SetVsync(bool on);   // runtime
bool Vsync();
void SetTargetFps(int fps);  // 0 = unlimited
int TargetFps();
// Size of the guest's framebuffer 0. Latched at Init from Config (or the window pixel size); only SetRenderSize
// changes it (ANativeWindow_getWidth/Height and eglQuerySurface report it).
void RenderSize(int& w, int& h);
void SetRenderSize(int w, int h);

// Frames presented to the window so far (guest eglSwapBuffers + EndFrame).
std::uint64_t FramesPresented();
// Asks the GL thread to save the next presented frame (the guest's framebuffer 0 at render size) as a 24-bit BMP.
// Any thread. A later request before that frame replaces the path.
void RequestScreenshot(const std::string& path);

// Host-driven frames on the GL thread, for code that renders without the guest (loading screens, tests).
// CreateHostContext makes an ANGLE context + window surface current on the calling thread and redirects framebuffer 0
// to the render-size FBO, exactly as for the guest. BeginFrame binds that FBO and clears nothing; EndFrame blits it
// to the window, swaps and applies the frame-pacing cap. (The guest's eglSwapBuffers does the same as EndFrame.)
bool CreateHostContext();
void DestroyHostContext();
void BeginFrame();
void EndFrame();

// ---- guest HLE ----
// Registers the libEGL (18 + KHR sync), libGLESv2 (96 + extension functions) and libandroid ANativeWindow_* handlers.
// Does not need the window yet; call before loading the guest library.
void RegisterHle();

}  // namespace nova::gfx
