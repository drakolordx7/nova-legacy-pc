// nova.ini: PC settings next to the exe. Created with the defaults on first run; unknown or missing keys fall back to
// the defaults. Lines are `key = value`, `;` or `#` starts a comment, [sections] are decoration only.
#pragma once
#include <filesystem>
#include <map>
#include <string>

namespace nova::pc {

struct Config {
  // [display]
  int width = 1280, height = 720;        // windowed client size
  std::string window_mode = "windowed";  // windowed | borderless | exclusive
  int display = 0;
  int exclusive_width = 0, exclusive_height = 0;  // exclusive fullscreen mode (0 = desktop size)
  float exclusive_refresh = 0;                    // 0 = best
  bool vsync = true;
  // -1: monitor refresh rate when vsync is off (vsync itself paces otherwise); 0: uncapped (limited to 1000 fps,
  // the engine's frame time is whole milliseconds); N: cap at N fps.
  int fps_cap = -1;

  // [input]
  float sensitivity = 0.045f;  // degrees per raw mouse count
  // Multiplier on top of the zoom: while aiming, mouse sensitivity is already scaled by tan(fov/2) relative to hip fire
  // (same turn per degree of FOV); 1.0 keeps that, below 1 is slower, above 1 faster.
  float ads_sensitivity_scale = 1.0f;
  float ads_zoom = 1.4f;       // zoom (FOV divisor) for weapons without an iron sight zoom of their own; 1 = none
  bool invert_y = false;
  std::string aim_mode = "hold";  // hold | toggle
  std::string crouch_mode = "hold";  // hold | toggle
  float jump_height = 0.4f;       // jump height as a fraction of the player capsule height (about 0.9 m at 0.4)
  // Held in gameplay: the cursor is released and shown, mouse clicks / drags are touches (tap HUD icons and popups).
  // Comma separated keyboard keys, or none.
  std::string cursor_key = "Left Alt";

  // [hud]
  bool hide_touch_hud = true;
  std::string hud_hide;  // extra Hud element ids to hide (comma separated); the built-in list is always used

  // [game]
  bool free_energy = true;  // campaign energy removed
  // The weapon cone (bullet spread) grows with the turn rate in the original; with a mouse that balloons the spread, so
  // the turn contribution is removed unless this is on (the base, movement and aim-down-sights spread stay).
  bool turn_spread = false;

  // [audio]
  bool mute = false;
  float volume = 1.0f;

  // [keys] action -> comma separated tokens (SDL scancode names, mouse1..mouse5, wheelup, wheeldown, none)
  std::map<std::string, std::string> keys;
};

// The process-wide settings (loaded by Load; defaults before that).
Config& Settings();

// Reads `path`, writing the default file first when it does not exist. Returns false if it could not be read/written
// (the defaults stay in effect).
bool Load(const std::filesystem::path& path);

// Default binding text of an action ("" for an unknown action).
const char* DefaultBinding(const std::string& action);

}  // namespace nova::pc
