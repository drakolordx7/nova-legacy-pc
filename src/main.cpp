// nova.exe: native Windows x64 host for the unmodified arm64 libNOVA.so.
//   nova [--run] [options]     boot the game in a window (default)
//   nova --load-only <so>      load + relocate the library, print the import report, run its constructors
#include <windows.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "android/android.h"
#include "android/zip.h"
#include "audio/audio.h"
#include "core/log.h"
#include "cpu/cpu.h"
#include "cpu/hle.h"
#include "game/game.h"
#include "gfx/gfx.h"
#include "hle/libc.h"
#include "hle/vfs.h"
#include "jni/jni.h"
#include "loader/elf64.h"
#include "pc/config.h"
#include "pc/input.h"

using namespace nova;
namespace fs = std::filesystem;

namespace {
void Usage() {
  std::printf(
      "usage: nova [--run] [options]\n"
      "       nova --load-only <path-to-libNOVA.so> [--imports <file>] [--no-init] [--trace]\n"
      "\n"
      "--run (default): boot the game in a window\n"
      "  --lib <file>            libNOVA.so (default: <exe dir>\\libNOVA.so, else game\\lib\\libNOVA.so)\n"
      "  --apk <file>            the N.O.V.A. Legacy 5.8.4a APK (default: first game\\*.apk)\n"
      "  --data <dir>            data root of the virtual file system (default: <exe dir>\\data)\n"
      "  --external-files <dir>  host folder for the game's external files dir (/sdcard/Android/data/<pkg>/files),\n"
      "                          e.g. already extracted packs (default: <data>\\external\\files)\n"
      "  --seed <dir>            copy already extracted game data from <dir> into the external files dir when it has none\n"
      "  --size WxH              windowed client size (overrides nova.ini)\n"
      "  --fullscreen            borderless fullscreen (overrides nova.ini)\n"
      "  --mute                  silent audio driver (same as NOVA_AUDIO_DRIVER=dummy)\n"
      "                          display mode, sensitivity, key bindings...: nova.ini next to the exe (written on first run)\n"
      "  --shots <dir>           where screenshots go (F12; default <exe dir>\\shots)\n"
      "  --shot-every <s>        also save a screenshot every s seconds\n"
      "  --exit-after <s>        quit after s seconds\n"
      "  --script \"<s> tap X Y; <s> down|move|up X Y [id]; <s> text 25; <s> enter; <s> back; <s> shot NAME;\n"
      "                          <s> kd|ku KEY; <s> mm DX DY; <s> wheel N; waitplay\"  timed input (render pixels; kd/ku/mm/wheel = keyboard and raw mouse in gameplay)\n"
      "  --trace                 trace every import call (same as NOVA_TRACE_IMPORTS=1)\n"
      "\n"
      "--load-only: load and relocate the library, print the report, run .init_array and report how far it got\n"
      "  --imports <file>  where to write the full import list (default nova_imports.txt)\n"
      "  --no-init         stop after the load report\n");
}

fs::path ExeDir() {
  wchar_t buf[MAX_PATH * 4];
  const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
  return fs::path(std::wstring(buf, n)).parent_path();
}

std::string U8(const fs::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}

int LoadOnly(const std::string& path, const std::string& imports_file, bool run_init) {
  hle::libc::RegisterAll();
  std::string err;
  const auto mod = loader::Load(path, err);
  if (!mod) {
    std::fprintf(stderr, "load failed: %s\n", err.c_str());
    return 1;
  }
  loader::PrintLoadReport(*mod, stdout);
  if (loader::WriteImportList(*mod, imports_file))
    std::printf("import list written to %s\n", imports_file.c_str());
  else
    std::fprintf(stderr, "cannot write %s\n", imports_file.c_str());
  if (!run_init) return 0;

  std::printf("\nrunning constructors...\n");
  std::fflush(stdout);
  const loader::InitReport rep = loader::RunInit(*mod);
  std::fflush(stdout);
  std::printf("\ninit: %zu of %zu constructors ran\n", rep.ran, rep.total);
  if (!rep.ok) {
    std::printf("stopped at constructor #%zu (%s)\n", rep.failed_index, rep.failed_sym.c_str());
    std::printf("reason: %s\n", rep.fault.c_str());
  }
  // Imports the guest actually called that have no handler (each returned 0).
  std::printf("\nunimplemented imports called during init:\n");
  int n = 0;
  for (const hle::Entry* e : hle::AllEntries()) {
    if (e->index != 0 && !e->fn.load() && e->calls.load()) {
      std::printf("  %-40s %llu call(s)  [%s]\n", e->name.c_str(), (unsigned long long)e->calls.load(), e->lib.c_str());
      ++n;
    }
  }
  if (!n) std::printf("  (none)\n");
  return rep.ok ? 0 : 3;
}

// ------------------------------------------------------------------------------------------------ --run
struct RunOptions {
  fs::path lib, apk, data, external_files, shots, seed;
  int width = 1280, height = 720;
  bool fullscreen = false, mute = false, size_set = false;
  double shot_every = 0, exit_after = 0;
  std::string script;
};

// --script "<sec> tap X Y; <sec> text 25; <sec> enter; <sec> back; <sec> shot NAME": timed input for unattended runs.
// Coordinates are render (game) pixels; times are seconds since boot.
struct ScriptStep {
  double at = 0;
  std::string op, arg;
  float x = 0, y = 0;
  int id = 0;
};
std::vector<ScriptStep> ParseScript(const std::string& s) {
  std::vector<ScriptStep> out;
  std::size_t pos = 0;
  while (pos < s.size()) {
    std::size_t end = s.find(';', pos);
    if (end == std::string::npos) end = s.size();
    char op[32] = {}, arg[128] = {};
    ScriptStep st;
    const std::string item = s.substr(pos, end - pos);
    const int n = std::sscanf(item.c_str(), "%lf %31s %127s", &st.at, op, arg);
    if (n >= 2) {
      st.op = op;
      st.arg = n >= 3 ? arg : "";
      if (st.op == "tap" || st.op == "down" || st.op == "move" || st.op == "up")
        std::sscanf(item.c_str(), "%*f %*s %f %f %d", &st.x, &st.y, &st.id);
      else if (st.op == "mm")
        std::sscanf(item.c_str(), "%*f %*s %f %f", &st.x, &st.y);
      else if (st.op == "wheel")
        std::sscanf(item.c_str(), "%*f %*s %f", &st.x);
      out.push_back(st);
    } else if (item.find_first_not_of(' ') != std::string::npos) {
      log::Warn("[run] bad --script item '{}'", item);
    }
    pos = end + 1;
  }
  // Steps run in time order, except that a script with a `waitplay` step keeps its written order: `waitplay` holds the
  // script until the game reports active gameplay, and the times of the steps after it count from that moment.
  const bool has_wait = std::any_of(out.begin(), out.end(), [](const ScriptStep& st) { return st.op == "waitplay"; });
  if (!has_wait) std::stable_sort(out.begin(), out.end(), [](const ScriptStep& a, const ScriptStep& b) { return a.at < b.at; });
  return out;
}

std::atomic<bool> g_quit{false};
std::atomic<int> g_exit_code{0};

void LogUnimplemented() {
  std::string s;
  for (const hle::Entry* e : hle::AllEntries())
    if (e->index != 0 && !e->fn.load() && e->calls.load()) s += std::format("\n  {} x{} [{}]", e->name, e->calls.load(), e->lib);
  log::Info("[run] unimplemented imports called:{}", s.empty() ? " (none)" : s);
}

std::string ShotName(const fs::path& dir, const char* tag) {
  const auto t = std::chrono::system_clock::now();
  const auto secs = std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
  return U8(dir / std::format("nova_{}_{}.bmp", tag, secs));
}

int Run(RunOptions o) {
  const fs::path exe = ExeDir();
  std::error_code ec;
  if (o.data.empty()) o.data = exe / "data";
  if (o.shots.empty()) o.shots = exe / "shots";
  if (o.lib.empty()) o.lib = fs::exists(exe / "libNOVA.so") ? exe / "libNOVA.so" : fs::path("game/lib/libNOVA.so");
  if (o.apk.empty()) {
    for (const auto& d : {fs::path("game"), exe / "game", exe}) {
      for (const auto& e : fs::directory_iterator(d, ec))
        if (e.path().extension() == ".apk") {
          o.apk = e.path();
          break;
        }
      if (!o.apk.empty()) break;
    }
  }
  fs::create_directories(o.data, ec);
  fs::create_directories(o.shots, ec);
  log::Info("[run] lib {}", U8(o.lib));
  log::Info("[run] apk {}", o.apk.empty() ? std::string("(none)") : U8(o.apk));
  log::Info("[run] data root {}", U8(fs::absolute(o.data, ec)));

  // Threads made by the guest that die of a fault or call exit: report and end the run.
  cpu::SetFatalHandler([](const std::string& what) {
    log::Error("[run] guest thread died of a fault:\n{}", what);
    g_exit_code = 70;
    g_quit = true;
  });
  cpu::SetExitHandler([](int code) {
    log::Info("[run] guest called exit({})", code);
    g_exit_code = code;
    g_quit = true;
  });

  // File system: the VFS root plus the optional external files folder.
  vfs::SetRoot(fs::absolute(o.data, ec));
  // --seed: a run folder without game data gets a copy of already extracted data (never written afterwards).
  if (!o.seed.empty()) {
    const fs::path dst = (o.external_files.empty() ? o.data / "external" / "files" : o.external_files) / "pack";
    if (!fs::exists(dst, ec)) {
      log::Info("[run] seeding {} from {} (one-time copy)", U8(dst.parent_path()), U8(o.seed));
      fs::create_directories(dst.parent_path(), ec);
      fs::copy(o.seed, dst.parent_path(), fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
      if (ec) log::Error("[run] seed copy failed: {}", ec.message());
    }
  }
  if (!o.external_files.empty()) {
    const fs::path ext = fs::absolute(o.external_files, ec);
    const std::string pkg = vfs::kPackage;
    vfs::AddMount("/storage/emulated/0/Android/data/" + pkg + "/files", ext);
    vfs::AddMount("/sdcard/Android/data/" + pkg + "/files", ext);
    log::Info("[run] external files dir {}", U8(ext));
  }

  // The Java side's view of the device.
  auto& jc = jni::config();
  jc.prefs_file = o.data / "internal" / "shared_prefs.txt";
  if (!o.apk.empty()) {
    auto zip = std::shared_ptr<android::ZipArchive>(android::ZipArchive::Open(U8(o.apk)));
    if (zip) {
      jc.read_asset = [zip](const std::string& name, std::vector<std::uint8_t>& out) {
        const auto* e = zip->Find("assets/" + name);
        if (!e) return false;
        const auto data = zip->Load(*e);
        if (!data) return false;
        out = *data;
        return true;
      };
    }
  }
  jc.host_path = [](const std::string& p) { return vfs::ToHost(p).value_or(fs::path()); };
  jc.on_exit = [](bool restart) {
    log::Info("[run] the game asked to exit (restart = {})", restart);
    g_quit = true;
  };

  // nova.ini next to the exe (written with the defaults on first run); the command line wins over it.
  pc::Load(exe / "nova.ini");
  pc::InitInput();
  const pc::Config& cfg = pc::Settings();

  // Every HLE module, before the library is loaded (data imports are bound at load time).
  if (o.mute || cfg.mute) audio::SetDriver("dummy");
  audio::SetMasterVolume(cfg.volume);
  hle::libc::RegisterAll();
  gfx::RegisterHle();
  audio::RegisterHle();
  android::RegisterHle();
  if (!o.apk.empty()) android::SetApkPath(U8(o.apk));
  jni::Init();

  // Window.
  gfx::Config gc;
  gc.width = o.size_set ? o.width : cfg.width;
  gc.height = o.size_set ? o.height : cfg.height;
  gc.display = cfg.display;
  gc.vsync = cfg.vsync;
  gc.fullscreen_width = cfg.exclusive_width;
  gc.fullscreen_height = cfg.exclusive_height;
  gc.fullscreen_refresh = cfg.exclusive_refresh;
  if (o.fullscreen) gc.mode = gfx::WindowMode::Borderless;
  else if (o.size_set) gc.mode = gfx::WindowMode::Windowed;
  else if (cfg.window_mode == "borderless" || cfg.window_mode == "fullscreen") gc.mode = gfx::WindowMode::Borderless;
  else if (cfg.window_mode == "exclusive") gc.mode = gfx::WindowMode::Exclusive;
  else gc.mode = gfx::WindowMode::Windowed;
  if (!gfx::Init(gc)) return 5;
  // Frame cap. The engine's frame time is whole milliseconds (see pc/layer.cpp), so "uncapped" stops at 1000 fps and
  // the default (-1) follows the monitor refresh rate when vsync is off; with vsync on the swap paces the game.
  auto apply_fps_cap = [&cfg]() {
    int cap = 0;
    if (cfg.fps_cap > 0) cap = cfg.fps_cap;
    else if (cfg.fps_cap == 0) cap = 1000;
    else if (!cfg.vsync) {
      const SDL_DisplayMode* dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(gfx::Window()));
      cap = dm && dm->refresh_rate > 1.0f ? static_cast<int>(dm->refresh_rate + 0.5f) : 60;
    }
    gfx::SetVsync(cfg.vsync);
    gfx::SetTargetFps(cap);
    log::Info("[run] vsync {}, frame cap {}", cfg.vsync ? "on" : "off", cap ? std::to_string(cap) : std::string("none"));
  };
  apply_fps_cap();
  int rw = 0, rh = 0;
  gfx::RenderSize(rw, rh);

  std::string err;
  const auto mod = loader::Load(U8(o.lib), err);
  if (!mod) {
    log::Error("[run] load failed: {}", err);
    gfx::Shutdown();
    return 1;
  }
  log::Info("[run] {} loaded at 0x{:x}, {} imports", mod->name, mod->base, mod->imports.size());

  // A failed boot leaves guest threads running: report and leave without tearing the window down under them.
  auto fail = [](int code) {
    LogUnimplemented();
    log::Info("[run] JNI coverage:\n{}", jni::CoverageReport());
    log::SetLogFile("");
    std::fflush(nullptr);
    std::_Exit(code);
  };
  try {
    if (!game::Boot(*mod, rw, rh)) {
      log::Error("[run] boot failed");
      fail(3);
    }
  } catch (const GuestFault& f) {
    log::Error("[run] guest fault during boot:\n{}", f.what());
    fail(4);
  } catch (const GuestExit& e) {
    log::Info("[run] guest exit({}) during boot", e.code);
    fail(e.code);
  }

  // Main loop: SDL events -> game input; the game renders and presents on its own thread.
  //   gameplay (the game thread reports it, pc/layer.cpp): raw mouse + keyboard go to the PC controls, cursor captured
  //   menus: the mouse is a touch screen (click = tap, drag = swipe, wheel = scroll), Esc / right click = back
  using Clock = std::chrono::steady_clock;
  using std::chrono::milliseconds;
  const auto t0 = Clock::now();
  auto next_shot = t0 + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(o.shot_every));
  auto last_report = t0;
  std::uint64_t first_frame_logged = 0;
  bool mouse_down = false;  // left button acting as touch pointer 0
  float touch_x = 0, touch_y = 0;
  bool resize_pending = false;
  Clock::time_point resize_at{};
  auto to_pixels = [](float x, float y, float& px, float& py) {
    int ww = 1, wh = 1, w = 0, h = 0;
    SDL_GetWindowSize(gfx::Window(), &ww, &wh);
    gfx::RenderSize(w, h);
    px = x * w / (ww > 0 ? ww : 1);
    py = y * h / (wh > 0 ? wh : 1);
  };
  // Mouse wheel in menus: a short swipe with touch pointer 1 (Flash lists scroll by dragging).
  struct Scroll {
    bool active = false;
    float x = 0, y = 0, target = 0;
    Clock::time_point next{};
  } scroll;
  auto end_scroll = [&scroll]() {
    if (!scroll.active) return;
    scroll.active = false;
    game::SendTouch(2, 1, scroll.x, scroll.y);
  };
  auto toggle_fullscreen = [&]() {
    if (gfx::GetWindowMode() == gfx::WindowMode::Windowed) {
      const bool excl = cfg.window_mode == "exclusive";
      gfx::SetFullscreen(excl ? gfx::WindowMode::Exclusive : gfx::WindowMode::Borderless, cfg.exclusive_width,
                         cfg.exclusive_height, cfg.exclusive_refresh);
    } else {
      gfx::SetFullscreen(gfx::WindowMode::Windowed);
    }
    resize_pending = true;
    resize_at = Clock::now() + milliseconds(250);
    apply_fps_cap();
  };
  bool was_gameplay = false;
  const std::vector<ScriptStep> script = ParseScript(o.script);
  std::size_t script_pos = 0;
  double script_base = 0;
  bool text_input = false;
  while (!g_quit.load() && !gfx::CloseRequested()) {
    // On-screen keyboard -> SDL text input while the game shows it.
    const bool kb = jni::KeyboardVisible();
    if (kb != text_input) {
      text_input = kb;
      if (kb) SDL_StartTextInput(gfx::Window());
      else SDL_StopTextInput(gfx::Window());
    }
    // Enter / leave gameplay: capture the mouse (raw deltas, hidden cursor) or give it back as a pointer.
    const bool gameplay = pc::InGameplay();
    if (gameplay != was_gameplay) {
      was_gameplay = gameplay;
      pc::ReleaseAll();
      try {
        if (gameplay) {
          if (mouse_down) game::SendTouch(2, 0, touch_x, touch_y);
          mouse_down = false;
          end_scroll();
        }
      } catch (const GuestFault& f) {
        log::Error("[run] guest fault in an input callback:\n{}", f.what());
        g_exit_code = 4;
        g_quit = true;
      }
      if (o.script.empty()) gfx::SetRelativeMouse(gameplay);  // scripted runs inject their input: leave the real mouse alone
      log::Info("[run] input mode: {}", gameplay ? "gameplay (raw mouse + keyboard)" : "menu (mouse as touch)");
    }
    for (const SDL_Event& e : gfx::PumpEvents()) {
      try {
        switch (e.type) {
          case SDL_EVENT_MOUSE_BUTTON_DOWN:
          case SDL_EVENT_MOUSE_BUTTON_UP: {
            const bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            if (gameplay) {
              pc::OnMouseButton(e.button.button, down);
            } else if (e.button.button == SDL_BUTTON_LEFT) {
              float x, y;
              to_pixels(e.button.x, e.button.y, x, y);
              touch_x = x;
              touch_y = y;
              mouse_down = down;
              game::SendTouch(down ? 0 : 2, 0, x, y);
            } else if (e.button.button == SDL_BUTTON_RIGHT && down) {
              game::SendBack(true);
              game::SendBack(false);
            }
            break;
          }
          case SDL_EVENT_MOUSE_MOTION:
            if (gameplay) {
              pc::OnMouseMotion(e.motion.xrel, e.motion.yrel);
            } else if (mouse_down) {
              float x, y;
              to_pixels(e.motion.x, e.motion.y, x, y);
              touch_x = x;
              touch_y = y;
              game::SendTouch(1, 0, x, y);
            }
            break;
          case SDL_EVENT_MOUSE_WHEEL:
            if (gameplay) {
              pc::OnWheel(e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y);
            } else if (!kb && e.wheel.y != 0) {
              int w = 0, h = 0;
              gfx::RenderSize(w, h);
              const float dy = (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y) * h * 0.12f;
              if (!scroll.active) {
                to_pixels(e.wheel.mouse_x, e.wheel.mouse_y, scroll.x, scroll.y);
                scroll.active = true;
                scroll.target = scroll.y + dy;
                scroll.next = Clock::now() + milliseconds(16);
                game::SendTouch(0, 1, scroll.x, scroll.y);
              } else {
                scroll.target += dy;
              }
            }
            break;
          case SDL_EVENT_TEXT_INPUT:
            if (jni::KeyboardVisible()) jni::KeyboardSetText(jni::KeyboardText() + e.text.text);
            break;
          case SDL_EVENT_KEY_DOWN:
          case SDL_EVENT_KEY_UP:
            if (jni::KeyboardVisible()) {
              // The Java EditText eats every key while it has focus; Enter / Back close it, Backspace edits.
              if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_BACKSPACE) {
                std::string t = jni::KeyboardText();
                while (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0x80) t.pop_back();
                if (!t.empty()) t.pop_back();
                jni::KeyboardSetText(t);
              } else if (e.type == SDL_EVENT_KEY_UP &&
                         (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER || e.key.key == SDLK_ESCAPE)) {
                jni::KeyboardHide();
              }
              break;
            }
            if (e.key.key == SDLK_ESCAPE && !e.key.repeat) game::SendBack(e.type == SDL_EVENT_KEY_DOWN);
            else if (e.key.key == SDLK_F12 && e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat)
              gfx::RequestScreenshot(ShotName(o.shots, "f12"));
            else if ((e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) && e.type == SDL_EVENT_KEY_DOWN &&
                     !e.key.repeat && (e.key.mod & SDL_KMOD_ALT))
              toggle_fullscreen();
            else
              pc::OnKey(static_cast<int>(e.key.scancode), e.type == SDL_EVENT_KEY_DOWN, e.key.repeat);
            break;
          case SDL_EVENT_WINDOW_FOCUS_LOST: pc::ReleaseAll(); break;
          case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            resize_pending = true;
            resize_at = Clock::now() + milliseconds(250);  // apply once the drag settles
            break;
          default: break;
        }
      } catch (const GuestFault& f) {
        log::Error("[run] guest fault in an input callback:\n{}", f.what());
        g_exit_code = 4;
        g_quit = true;
      }
    }
    const auto now = Clock::now();
    if (scroll.active && now >= scroll.next) {
      scroll.next = now + milliseconds(16);
      const float d = scroll.target - scroll.y;
      try {
        if (std::fabs(d) < 1.0f) {
          end_scroll();
        } else {
          scroll.y += std::clamp(d, -48.0f, 48.0f);
          game::SendTouch(1, 1, scroll.x, scroll.y);
        }
      } catch (const GuestFault& f) {
        log::Error("[run] guest fault in an input callback:\n{}", f.what());
        g_quit = true;
      }
    }
    const double elapsed = std::chrono::duration<double>(now - t0).count();
    while (script_pos < script.size() && elapsed - script_base >= script[script_pos].at) {
      const ScriptStep& st = script[script_pos];
      if (st.op == "waitplay") {
        if (!pc::InGameplay()) break;
        script_base = elapsed;
        log::Info("[run] script: gameplay reached at {:.1f} s", elapsed);
        ++script_pos;
        continue;
      }
      ++script_pos;
      log::Info("[run] script {:.1f} s: {} {}", st.at, st.op, st.arg.empty() ? std::format("{} {} id {}", st.x, st.y, st.id) : st.arg);
      try {
        if (st.op == "down" || st.op == "move" || st.op == "up") {
          game::SendTouch(st.op == "down" ? 0 : st.op == "move" ? 1 : 2, st.id, st.x, st.y);
        } else if (st.op == "tap") {
          game::SendTouch(0, 0, st.x, st.y);
          std::this_thread::sleep_for(milliseconds(80));
          game::SendTouch(2, 0, st.x, st.y);
        } else if (st.op == "text") {
          jni::KeyboardSetText(jni::KeyboardText() + st.arg);
        } else if (st.op == "enter") {
          jni::KeyboardHide();
        } else if (st.op == "back") {
          game::SendBack(true);
          game::SendBack(false);
        } else if (st.op == "shot") {
          gfx::RequestScreenshot(U8(o.shots / ((st.arg.empty() ? std::string("script") : st.arg) + ".bmp")));
        } else if (st.op == "kd" || st.op == "ku") {
          int sc = 0, mb = 0;
          if (!pc::ParseToken(st.arg.c_str(), sc, mb)) log::Warn("[run] script: unknown key '{}'", st.arg);
          else if (sc) pc::OnKey(sc, st.op == "kd", false);
          else pc::OnMouseButton(mb, st.op == "kd");
        } else if (st.op == "mm") {
          pc::OnMouseMotion(st.x, st.y);
        } else if (st.op == "wheel") {
          pc::OnWheel(st.x);
        }
      } catch (const GuestFault& f) {
        log::Error("[run] guest fault in a scripted input:\n{}", f.what());
        g_quit = true;
      }
    }
    if (resize_pending && now >= resize_at) {
      resize_pending = false;
      int pw = 0, ph = 0;
      gfx::WindowPixelSize(pw, ph);
      int cw = 0, ch = 0;
      gfx::RenderSize(cw, ch);
      if (pw > 0 && ph > 0 && (pw != cw || ph != ch)) {
        log::Info("[run] window resized to {}x{}", pw, ph);
        gfx::SetRenderSize(pw, ph);
        try {
          game::SendSurfaceChanged(pw, ph);
        } catch (const GuestFault& f) {
          log::Error("[run] guest fault in NativeSurfaceChanged:\n{}", f.what());
          g_quit = true;
        }
      }
    }
    const std::uint64_t frames = gfx::FramesPresented();
    if (frames && !first_frame_logged) {
      first_frame_logged = frames;
      log::Info("[run] first frame presented after {:.1f} s",
                std::chrono::duration<double>(now - t0).count());
    }
    if (o.shot_every > 0 && now >= next_shot) {
      next_shot = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(o.shot_every));
      if (frames) gfx::RequestScreenshot(ShotName(o.shots, "auto"));
    }
    if (now - last_report >= std::chrono::seconds(10)) {
      last_report = now;
      log::Info("[run] {:.0f} s: {} frames presented, {} guest threads", std::chrono::duration<double>(now - t0).count(),
                frames, threads::Count());
    }
    if (o.exit_after > 0 && now - t0 >= std::chrono::duration<double>(o.exit_after)) {
      log::Info("[run] --exit-after reached");
      break;
    }
    SDL_Delay(2);
  }

  // Shutdown. The game's own threads cannot be joined (Android kills the process too): pause the game so it stops
  // touching the GL / audio, release the audio device, write the reports and leave.
  log::Info("[run] shutting down");
  try {
    game::Pause();
  } catch (const GuestFault& f) {
    log::Warn("[run] guest fault in NativeOnPause:\n{}", f.what());
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  audio::Shutdown();
  LogUnimplemented();
  log::Info("[run] JNI coverage:\n{}", jni::CoverageReport());
  log::Info("[run] exit code {}", g_exit_code.load());
  log::SetLogFile("");
  std::fflush(nullptr);
  std::_Exit(g_exit_code.load());
}

fs::path P8(const char* s) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s))); }

bool ParseSize(const std::string& s, int& w, int& h) {
  return std::sscanf(s.c_str(), "%dx%d", &w, &h) == 2 && w > 0 && h > 0;
}
}  // namespace

int main(int argc, char** argv) {
  std::string load_only, imports = "nova_imports.txt";
  bool run_init = true;
  RunOptions ro;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const bool more = i + 1 < argc;
    if (a == "--load-only" && more) load_only = argv[++i];
    else if (a == "--imports" && more) imports = argv[++i];
    else if (a == "--no-init") run_init = false;
    else if (a == "--trace") debug::SetTraceImports(true);
    else if (a == "--run") {}
    else if (a == "--lib" && more) ro.lib = P8(argv[++i]);
    else if (a == "--apk" && more) ro.apk = P8(argv[++i]);
    else if (a == "--data" && more) ro.data = P8(argv[++i]);
    else if (a == "--external-files" && more) ro.external_files = P8(argv[++i]);
    else if (a == "--seed" && more) ro.seed = P8(argv[++i]);
    else if (a == "--shots" && more) ro.shots = P8(argv[++i]);
    else if (a == "--size" && more && ParseSize(argv[i + 1], ro.width, ro.height)) ro.size_set = true, ++i;
    else if (a == "--fullscreen") ro.fullscreen = true;
    else if (a == "--mute") ro.mute = true;
    else if (a == "--shot-every" && more) ro.shot_every = std::atof(argv[++i]);
    else if (a == "--exit-after" && more) ro.exit_after = std::atof(argv[++i]);
    else if (a == "--script" && more) ro.script = argv[++i];
    else {
      Usage();
      return 2;
    }
  }
  if (!load_only.empty()) {
    setvbuf(stdout, nullptr, _IOFBF, 1 << 16);
    try {
      return LoadOnly(load_only, imports, run_init);
    } catch (const GuestFault& f) {
      std::fprintf(stderr, "guest fault: %s\n", f.what());
      return 4;
    }
  }
  log::SetLogFile(U8(ExeDir() / "nova.log"));
  try {
    return Run(ro);
  } catch (const GuestFault& f) {
    log::Error("guest fault: {}", f.what());
    return 4;
  }
}
