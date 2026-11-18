# Engine hook points (libNOVA.so, by exported symbol)

Engine stack: Gameloft `glf` (app/input/fs/threads) + `glitch` (renderer, OpenGL ES 2.0 only) + `gameswf` (UI) + `vox` (audio) + Lua 5.1 scripting; game code is `Application`, `CLevel`, `C*Component`. All symbols below are in `.dynsym` (default visibility), so a loader that resolves by name can override or call them. Mangled names are given exactly; arguments follow the AAPCS64 (this in x0, floats in s0/s1).

## Threads and main loop

| What | Symbol | Use |
|---|---|---|
| Entry from Java | `_Z10OnGameInitv`, `_Z12OnGameResumev`, `_Z11OnGamePausev`, `_Z19OnWindowStateChangeP13ANativeWindow` | what the JNI `NativeInit/NativeOnResume/NativeOnPause/NativeSurfaceChanged` end up calling; a host can call them directly |
| Game thread | `_Z11AndroidMainPv` -> `_ZN3glf11InitGlobalsEiPPc`, `_Z6NewAppiPPc`, `_ZN3glf3App4Impl3RunEv`, `pthread_exit` | `OnGameInit` starts it with `pthread_create`; `Impl::Run` is the frame loop (state machine under a mutex for window/pause/resume events, `NativeSurface::Create/RefreshGLEnvironment`, then `eglSwapBuffers`). Run it on a guest thread, no per-frame Java call exists |
| Global app | data `_ZN3glf4gAppE` (8 bytes) | non-null once the app exists; `OnGameInit` refuses to start twice |

## Input (all inject into `glf::InputManager` devices, `_ZN3glf11GetInputMgrEv`)

| Device | Symbol | Use |
|---|---|---|
| Touch (the Android path) | `_Z16OnGameTouchEventiffi` (int action 0 down / 1 move / 2 up, float x, float y in window pixels, int pointerId; action 3 ignored) | simplest hook: call it from mouse events, id 0. It does `GetGamepad(0)->RaiseTouchEvent(0, type, id, x, y, 2)` with `InputEventType` 0xd8 (216) down, 0xd9 (217) move, 0xda (218) up |
| Touch (low level) | `_ZN3glf7Gamepad15RaiseTouchEventEiNS_14InputEventTypeEmffj`, `_ZN3glf12InputManager10GetGamepadEj`, `_ZNK3glf12InputManager15GetGamepadCountEv` | multi-touch / emulated virtual joystick |
| Touch (UI glue) | `_Z20AddAndroidTouchEventiiil` (int type, int x, int y, long id) -> `MenuManager::AddTouchEvent` | stores one pending touch for the menu layer; `TouchScreenAndroid::updateGlf`, `Application::UpdateGamePad`, `Application::UpdateGlfControls` are 4-byte stubs in this build, so the glf event path is the live one |
| Mouse | `_ZN3glf5Mouse14RaiseMoveEventEffj`, `_ZN3glf5Mouse16RaiseButtonEventEib`, `_ZN3glf5Mouse15RaiseWheelEventEf`, `_ZN3glf12InputManager8GetMouseEj`, `_ZNK3glf12InputManager13GetMouseCountEv` | desktop mouse; whether a `glf::Mouse` device is instantiated on Android was not established, check `GetMouseCount()` at run time, otherwise use the touch hook |
| Keyboard | `_Z17NativeOnKeyActionib` (Android keyCode 4 = BACK, 82 = MENU, bool down), `_ZN3glf8Keyboard16RaiseButtonEventEibi` (key id, down, int), `_ZN3glf12InputManager11GetKeyboardEj`, `_ZN3glf8Keyboard8GetKeyIDENS0_7KeyCodeE` | Esc -> BACK; other keys through `RaiseButtonEvent` with `glf::Keyboard::KeyCode` ids |
| Gamepad | `_ZN3glf7Gamepad16RaiseButtonEventEib`, `_ZN3glf7Gamepad23RaiseAnalogicStickEventEiff` (stick index, x, y), `_ZN3glf7Gamepad20RaiseCursorMoveEventEiss`, `_ZN3glf12InputManager19EmulateGamepadWin32ENS_11GamepadTypeE` (4-byte stub) | XInput mapping would call Raise* on `GetGamepad(0)`; Java never forwards pads in this build |
| Motion sensors | `_ZN3glf7Gamepad23RaiseAccelerometerEventEfff`, `_ZN3glf7Gamepad19RaiseGyroscopeEventEfff`, `_ZN3glf12InputManager19EnableAccelerometerEbf`, `_ZN3glf12InputManager10EnableGyroEbf` | otherwise native `AndroidSensorManager` reads `ASensor*` (imports); feed here or return no sensors |

## Display, size, aspect ratio

| What | Symbol | Use |
|---|---|---|
| Window/surface | `_ZN14ANativeSurface13NativeSurface9SetWindowEP13ANativeWindow`, `_ZN14ANativeSurface13NativeSurface19CreateGLEnvironmentERN3glf16CreationSettingsERNS1_12PlatformInfoERNS1_11AppSettingsE`, `eglSwapBuffers` (import) | the EGL setup/teardown; size comes from `ANativeWindow_getWidth/getHeight` and from `NativeSurfaceChanged(surface,w,h)` |
| Screen size read by engine | `_ZNK3glf3App13GetScreenSizeERiS1_i`, `_ZNK3glf3App13GetWindowSizeERiS1_i` (read `*(this+0x3a8)+0x78 / +0x7c`), `_ZN8GLDevice13GetScreenSizeEv`, `_ZN9acp_utils3api12PackageUtils8GetWidthEv`/`GetHeightEv` | hook to force a resolution different from the window |
| Screen globals | data `g_ScreenW`, `g_ScreenH` (int, exported, 4 bytes each) | the aspect ratio the game uses is simply `g_ScreenW / g_ScreenH` (read by `GameFOV`); write them after the surface is known |
| UI scale | `_ZN11Application9GetScaleXEv`, `_ZN11Application9GetScaleYEv`, `_ZN11Application20GetMaxUsedResolutionEv`, `_ZN11Application7LoadDPIEv` (`DPI.xml`), `_ZN11Application17LoadInterfaceDataEv` (`interface_<w>x<h>.bin`) | UI layouts are keyed by resolution; supply a size that has a layout file or hook these |
| Orientation | `_ZN3glf3App14SetOrientationENS_11OrientationE`, `_ZNK3glf3App22IsOrientationSupportedENS_11OrientationE` | landscape only |
| Device quality profile | `_ZN13DeviceOptions17GetGpuProfileNameEv`, `_ZN13DeviceOptions10GetGpuNameEv`, `_ZN13DeviceOptions11LoadProfileEv`, `_ZN13DeviceOptions18SetFakeDeviceSpecsEiiPKc` (4-byte stub), data `_ZN13DeviceOptions9SingletonE` | quality tier is chosen from `GL_RENDERER` and `GPUs.xml`; hook `GetGpuProfileName` to force the top profile |

## FOV

| What | Symbol | Use |
|---|---|---|
| Camera node | `_ZN22CCustomCameraSceneNode6setFOVEf` | the actual projection FOV; also calls `CLevel::SetLodsDistScale`, so LOD distance follows FOV |
| Game camera | `_ZN20CGameCameraComponent12SetCameraFovEf` (degrees; goes through `GameFOV::TranslateFovRad`, then a virtual call on the camera node at vtable +0x2b8, presumably `setFOV`), `_ZNK20CGameCameraComponent12GetCameraFovEv`, `_ZNK20CGameCameraComponent10GetGameFovEv`, `_ZNK20CGameCameraComponent13GetDefaultFOVEv`, `_ZN20CGameCameraComponent8ResetFovEv`, `_ZN15PlayerComponent15UpdateCameraFOVEf` | set a wider horizontal FOV for 16:9+ by wrapping `SetCameraFov` |
| FOV maths and tunables | data `_ZN7GameFOV10DESIGN_FOVE` (float, initial 55.0), `_ZN7GameFOV11DEFAULT_FOVE`, `_ZN7GameFOV13BASE_FOV_DIFFE` (= (DEFAULT-DESIGN) x deg2rad, recomputed by `_ZN7GameFOV17UpdateBaseFovDiffEv`); `_ZN7GameFOV23HorizontalToVerticalFOVEf` = 2 atan(tan(h/2) x g_ScreenH/g_ScreenW) in degrees, `_ZN7GameFOV23VerticalToHorizontalFOVEf`, `_ZN7GameFOV15TranslateFovRadEf`, `_ZN7GameFOV18TranslateInvFovRadEf` | write `DEFAULT_FOV` then call `UpdateBaseFovDiff`; the per-device value normally comes from `_ZN13DeviceOptions13LoadFovsTableEv` (`FOVs.xml`) |
| Weapon view model | `_ZNK16CWeaponComponent12GetRenderFovEv`, `GetIdleFOV`, `GetIronSightFOV`, `GetWeaponRenderFov` | separate FOV for the held weapon |
| Lua | `_Z6SetFOVP9lua_State`, `_Z8ResetFOVP9lua_State` | scripts can set FOV |

## Frame pacing and timing

| What | Symbol | Use |
|---|---|---|
| In-game frame cap | `_ZN11Application8LimitFpsEv` (called from `Application::Update`) | `usleep(max(0, lastFrame + 1000/fps - now) x 1000)`, fps read from `*DeviceOptions::Singleton + 0x9c` (int, from the device profile); hook to a no-op to uncap or let host vsync pace |
| Target FPS setters | `_ZN11Application12SetTargetFPSEib` (writes this+0x4c8 = fps, +0x4cc = 1000/fps; called by `GS_Loading::Update`, `GS_Respawn::Update`), `_ZN3glf3App12SetTargetFpsEf`, `_ZNK3glf3App12GetTargetFpsEv` | per-state caps |
| Present / vsync | import `eglSwapBuffers` (from `_ZN3glf3App4Impl7PresentEib` and `Impl::Run`); no `eglSwapInterval` is imported | swap interval is entirely the host's decision |
| Clocks | `_ZN3glf15GetMillisecondsEv`, `_ZN3glf8GetTicksEv` (`clock_gettime`), `_ZN11Application15UpdateFrameTimeEii` | time source for the whole engine (delta time derives from these): hook for slow motion or deterministic stepping |

## Win32 / desktop leftovers inside the glf code

All of these exist as symbols but are empty stubs (4-byte `ret`) or dead in this Android build; they show where the original PC code attached:
`_ZN11Application9InitWin32EN3glf13intrusive_ptrIN6glitch7IDeviceEEE`, `_ZN16TouchScreenWin329updateGlfEv` and the `TouchScreenWin32` / `GyroscopeWin32` classes (constructors do almost nothing), `_ZN3glf2Fs21Win32OverrideDataPathEPKc`, `glf::Fs::MacOSXOverrideDataPath`, `IphoneOverrideDataPath`, `_ZN3glf3fs25win3226GetCurrentWorkingDirectoryEv` (stub; `GetExecutable*`/`GetTempDirectory` are 16-byte stubs), `glf::fs2::wp8::*` and `glf::fs2::apple::*` (dead), `_ZN3glf12InputManager19EmulateGamepadWin32ENS_11GamepadTypeE`, `_ZN3glf14Win32FromPosixEi`/`PosixFromWin32` (error-code tables), `_Z15NativeIsWindowsRKN7gameswf12FunctionCallE` (12 bytes: the UI script function that tells the Flash UI which platform it is on), `_ZN6glotv37Porting23GetWindowsAdvertisingIDEv`, `TouchScreenLinux`, and `_ZN11CTexUpdater3RunEv` which shells out to `..\..\make\BuildData.py` (developer-only). There is no `wgl`, `HWND`, DirectInput or XInput code. The build string `REVISION.txt` says "Local Windows Build": the library was built on a Windows machine (build paths under `E:/Work/Project/Nova/trunk` appear in assert strings), but nothing in it is a usable PC backend.

## Practical use

A minimal PC front end needs: (1) the `JNIBridge` natives or the `OnGame*` entry points above; (2) a GL ES 2.0 provider behind `libGLESv2`/`libEGL` (ANGLE is the natural fit: it also supplies `eglGetProcAddress` extensions); (3) calls to `OnGameTouchEvent` or `Gamepad::Raise*` from the window loop; (4) overrides of `Application::LimitFps` (pacing), `g_ScreenW/g_ScreenH`, `GameFOV::DEFAULT_FOV` (widescreen) and the anti-tamper functions listed in paths.md section 6.
