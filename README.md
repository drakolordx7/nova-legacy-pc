# N.O.V.A. Legacy for Windows (unofficial PC port)

## This project is abandoned

It is finished and no longer maintained. The repository is archived, so issues and pull requests are closed. The code is free to use or fork.

---

Runs the campaign of Gameloft's Android game N.O.V.A. Legacy 5.8.4a natively on 64-bit Windows, with raw mouse aim, keyboard controls, any resolution or aspect ratio and an uncapped frame rate.

[Download the build](https://github.com/drakolordx7/nova-legacy-pc/releases/latest)

No game files are included. You need your own copy of the N.O.V.A. Legacy 5.8.4a APK (the arm64 build, package `com.gameloft.android.ANMP.GloftNOHM`). Not affiliated with Gameloft.

## How it works

The game's own arm64 library (`libNOVA.so`) runs unmodified inside a [dynarmic](https://github.com/azahar-emu/dynarmic) A64 JIT. The Windows side stands in for everything it expects from Android: libc and pthreads, a fake Java VM for the JNI calls, EGL and OpenGL ES through ANGLE on Direct3D 11, OpenSL ES audio through SDL3, and asset reads straight from the APK. A PC layer on top hooks the engine for mouse look, keys, field of view and frame timing. `docs/` has the details: the design, every import and JNI call the game makes, and the hook addresses.

Changes from the phone version:
- Raw mouse look, WASD, mouse buttons and rebindable keys. Right mouse aims down sights. Aim assist, auto fire, auto sprint and view auto-leveling are off.
- Space jumps and C or Left Ctrl crouches. The engine has a jump function that nothing on the phone ever called; crouch uses the cover stance.
- Hor+ field of view at any aspect ratio, full render resolution, the engine's frame limiter removed, and frame timing finer than the engine's whole milliseconds.
- Turning no longer adds bullet spread. On the phone that spread was small because thumbs turn slowly, but with a mouse every shot went wide.
- Campaign missions cost no energy, and the in-game shop gives its packs for free, offline.
- The port answers network, ads, login and anti-tamper checks offline. Multiplayer does not work.

## Requirements

- Windows 10 or 11, 64-bit, and a GPU with Direct3D 11
- The N.O.V.A. Legacy 5.8.4a APK
- Python 3 and [7-Zip](https://www.7-zip.org/), only to unpack the game library once

## Usage

1. Unzip the build into a folder.
2. Unpack the game library into that folder: `python extract_lib.py "path\to\N.O.V.A._Legacy-5.8.4a.apk" .`
   The APK keeps the real library packed in `assets/datv8` (7z, magic swapped for `CORIAJ`, XORed with 0x61); the script undoes that.
3. Put the APK in a `game` folder next to `nova.exe` (or pass `--apk <file>`), then run `nova.exe`.

The first start unpacks about 440 MB of game data from the APK into `data\`, which takes a while. Saves live in `data\` too. Settings are in `nova.ini`, written with defaults on first run: window mode, resolution, vsync, frame cap, sensitivity, aim zoom, invert Y, key bindings, and the two gameplay switches (`free_energy`, `turn_spread`). `nova.log` is the log.

Default controls:

| Action | Key |
|---|---|
| Move | W A S D |
| Fire / aim | Left / right mouse |
| Reload, jump, crouch, sprint | R, Space, C or Left Ctrl, Left Shift |
| Grenade, melee, interact | G, V or mouse 4, F |
| Kinetic armor, medkit, ammo, weapon boost | Q, E, T, X |
| Switch weapon | Mouse wheel |
| Pause / back | Esc |
| Free the cursor (tap HUD icons, tutorial prompts) | Hold Left Alt |
| Fullscreen, screenshot | Alt+Enter, F12 |

Known gaps: never tested with sound on, only tested on one PC, mission loads are slow because the JIT translates the game's code from scratch on every launch, shutdown doesn't stop the game threads cleanly, and there is no controller support.

## Reporting problems

The repository is archived, so issues and pull requests are closed. Fork it if you want to keep going.

## Contributing

There is one branch, `master`. To build:

1. Install Visual Studio 2022 or newer (Desktop C++ or the Build Tools), CMake 3.25 or newer, Ninja and [vcpkg](https://github.com/microsoft/vcpkg), and set `VCPKG_ROOT` to the vcpkg folder.
2. `tools\vsenv.cmd cmake --preset win-release`
3. `tools\vsenv.cmd cmake --build --preset win-release`

The output is `build\win-release\src\nova.exe` with its DLLs. vcpkg provides SDL3, ANGLE, mimalloc and Boost; CMake fetches dynarmic at a pinned commit. `nova.exe --help` lists the developer options (`--load-only`, `--trace`, `--script`, `--exit-after`). The tests in `tests\` need clang from LLVM to build their small arm64 guest programs.

## Credits

The CPU, loader and HLE structure is adapted from [Zombiewood](https://github.com/ChrisDutary/Zombiewood) by ChrisDutary (MIT), a port of another Gameloft game on the same engine. It uses [dynarmic](https://github.com/azahar-emu/dynarmic) (azahar-emu fork), [SDL3](https://github.com/libsdl-org/SDL), [ANGLE](https://chromium.googlesource.com/angle/angle), [mimalloc](https://github.com/microsoft/mimalloc) and Boost. See `NOTICE` for their licences.

N.O.V.A. Legacy is a trademark of Gameloft.

## License

MIT, see `LICENSE`. The licence covers this port's code only, not the game.
