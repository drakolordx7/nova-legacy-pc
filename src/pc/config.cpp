#include "pc/config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "core/log.h"
#include "pc/input.h"

namespace nova::pc {
namespace {
Config g_cfg;

std::string Trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}
std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
bool ToBool(const std::string& v, bool def) {
  const std::string l = Lower(v);
  if (l == "1" || l == "true" || l == "yes" || l == "on") return true;
  if (l == "0" || l == "false" || l == "no" || l == "off") return false;
  return def;
}
int ToInt(const std::string& v, int def) {
  char* end = nullptr;
  const long r = std::strtol(v.c_str(), &end, 10);
  return end == v.c_str() ? def : static_cast<int>(r);
}
float ToFloat(const std::string& v, float def) {
  char* end = nullptr;
  const float r = std::strtof(v.c_str(), &end);
  return end == v.c_str() ? def : r;
}

std::string DefaultText() {
  const Config d;
  std::ostringstream o;
  o << "; N.O.V.A. Legacy PC settings. Delete a line to get its default; delete the file to recreate it.\n"
       "; Lines are 'key = value'. ';' or '#' starts a comment.\n\n"
       "[display]\n"
       "width = " << d.width << "                ; windowed client size\n"
       "height = " << d.height << "\n"
       "window_mode = " << d.window_mode << "     ; windowed | borderless (fullscreen at desktop resolution) | exclusive\n"
       "display = " << d.display << "              ; monitor index for fullscreen\n"
       "exclusive_width = 0         ; exclusive fullscreen mode, 0 = desktop size\n"
       "exclusive_height = 0\n"
       "exclusive_refresh = 0       ; 0 = best\n"
       "vsync = " << d.vsync << "\n"
       "; -1 = follow the monitor refresh rate when vsync is off, 0 = uncapped (never above 1000 fps: the engine's\n"
       "; frame time is whole milliseconds), N = cap at N fps. Alt+Enter toggles fullscreen, F12 saves a screenshot.\n"
       "fps_cap = " << d.fps_cap << "\n\n"
       "[input]\n"
       "sensitivity = 0.045         ; degrees of turn per raw mouse count (no acceleration, no smoothing)\n"
       "ads_sensitivity_scale = " << d.ads_sensitivity_scale << "     ; 1 = same turn per degree of FOV as hip fire while aiming (scaled by the zoom)\n"
       "ads_zoom = " << d.ads_zoom << "                 ; zoom while aiming for weapons with no iron sight zoom of their own (1 = none)\n"
       "invert_y = 0\n"
       "aim_mode = hold             ; hold | toggle\n"
       "cursor_key = " << d.cursor_key << "       ; hold to free and show the cursor: clicks and drags are then touches (tutorial prompts, any HUD icon, popups); none = off\n\n"
       "[hud]\n"
       "hide_touch_hud = 1          ; hide the on-screen touch controls\n"
       "hud_hide =                  ; extra HUD element ids to hide, comma separated\n\n"
       "[game]\n"
       "free_energy = 1             ; campaign missions cost no energy and are playable at any time\n"
       "turn_spread = 0             ; 1 = original mobile behaviour (bullet spread grows with turn speed, huge with a mouse); 0 = turning adds no spread\n\n"
       "[audio]\n"
       "mute = 0\n"
       "volume = 1.0                ; 0..1\n\n"
       "[keys]\n"
       "; comma separated alternatives: SDL scancode names (W, Space, Left Shift, Left Ctrl, F, 1, Tab ...),\n"
       "; mouse1 (left) mouse2 (right) mouse3 (middle) mouse4 mouse5, wheelup, wheeldown, or none.\n"
       "; Esc opens / closes the pause menu and goes back in menus (fixed).\n";
  for (int a = 0; a < kActionCount; ++a) o << kActions[a].name << " = " << kActions[a].defaults << "\n";
  return o.str();
}
}  // namespace

Config& Settings() { return g_cfg; }

const char* DefaultBinding(const std::string& action) {
  for (const ActionInfo& a : kActions)
    if (action == a.name) return a.defaults;
  return "";
}

bool Load(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
      log::Warn("[pc] cannot create {}", path.string());
      return false;
    }
    out << DefaultText();
    log::Info("[pc] wrote default settings to {}", path.string());
    return true;  // the struct already holds the defaults
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    log::Warn("[pc] cannot read {}", path.string());
    return false;
  }
  Config c;
  std::string line;
  while (std::getline(in, line)) {
    const auto semi = line.find_first_of(";#");
    if (semi != std::string::npos) line.erase(semi);
    line = Trim(line);
    if (line.empty() || line.front() == '[') continue;
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = Lower(Trim(line.substr(0, eq)));
    const std::string v = Trim(line.substr(eq + 1));
    if (k == "width") c.width = std::max(320, ToInt(v, c.width));
    else if (k == "height") c.height = std::max(200, ToInt(v, c.height));
    else if (k == "window_mode") c.window_mode = Lower(v);
    else if (k == "display") c.display = ToInt(v, c.display);
    else if (k == "exclusive_width") c.exclusive_width = ToInt(v, 0);
    else if (k == "exclusive_height") c.exclusive_height = ToInt(v, 0);
    else if (k == "exclusive_refresh") c.exclusive_refresh = ToFloat(v, 0);
    else if (k == "vsync") c.vsync = ToBool(v, c.vsync);
    else if (k == "fps_cap") c.fps_cap = ToInt(v, c.fps_cap);
    else if (k == "sensitivity") c.sensitivity = ToFloat(v, c.sensitivity);
    else if (k == "ads_sensitivity_scale") c.ads_sensitivity_scale = std::max(0.0f, ToFloat(v, c.ads_sensitivity_scale));
    else if (k == "ads_zoom") c.ads_zoom = std::max(1.0f, ToFloat(v, c.ads_zoom));
    else if (k == "ads_scale") {}  // old key (flat factor), replaced by ads_sensitivity_scale
    else if (k == "invert_y") c.invert_y = ToBool(v, c.invert_y);
    else if (k == "aim_mode") c.aim_mode = Lower(v);
    else if (k == "crouch_mode") c.crouch_mode = Lower(v);
    else if (k == "jump_height") c.jump_height = std::clamp(ToFloat(v, c.jump_height), 0.05f, 1.5f);
    else if (k == "cursor_key") c.cursor_key = v;
    else if (k == "ability1" || k == "ability2") {}  // old keys, replaced by boost / armor / medkit / ammo
    else if (k == "hide_touch_hud") c.hide_touch_hud = ToBool(v, c.hide_touch_hud);
    else if (k == "hud_hide") c.hud_hide = v;
    else if (k == "free_energy") c.free_energy = ToBool(v, c.free_energy);
    else if (k == "turn_spread") c.turn_spread = ToBool(v, c.turn_spread);
    else if (k == "mute") c.mute = ToBool(v, c.mute);
    else if (k == "volume") c.volume = std::clamp(ToFloat(v, c.volume), 0.0f, 1.0f);
    else {
      bool known = false;
      for (const ActionInfo& a : kActions)
        if (k == a.name) known = true;
      if (known) c.keys[k] = v;
      else log::Warn("[pc] nova.ini: unknown setting '{}'", k);
    }
  }
  g_cfg = std::move(c);
  log::Info("[pc] settings read from {}", path.string());
  return true;
}

}  // namespace nova::pc
