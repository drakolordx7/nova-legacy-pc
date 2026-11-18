#include "pc/input.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#include "core/log.h"
#include "pc/config.h"

namespace nova::pc {

const ActionInfo kActions[kActionCount] = {
    {"fire", "mouse1"},
    {"aim", "mouse2"},
    {"reload", "R"},
    {"jump", "Space"},
    {"crouch", "C, Left Ctrl"},
    {"grenade", "G"},
    {"melee", "V, mouse4"},
    {"interact", "F"},
    {"sprint", "Left Shift"},
    {"armor", "Q"},   // consumable slot 1: kinetic armor (restores the shield)
    {"medkit", "E"},  // slot 2: medkit (a picked-up one first, else the energy cell consumable)
    {"ammo", "T"},    // slot 3: ammo belt (refills all ammo)
    {"boost", "X"},   // slot 4: weapon boost bonus (the weapon's special button)
    {"weapon1", "1"},
    {"weapon2", "2"},
    {"weapon3", "3"},
    {"weapon4", "4"},
    {"weapon_next", "wheeldown"},
    {"weapon_prev", "wheelup"},
    {"forward", "W"},
    {"back", "S"},
    {"left", "A"},
    {"right", "D"},
};

namespace {
constexpr int kMaxScan = SDL_SCANCODE_COUNT;

struct Binding {
  std::vector<int> keys, mouse;
  bool wheel_up = false, wheel_down = false;
};
Binding g_bind[kActionCount];
std::uint32_t g_key_actions[kMaxScan];  // scancode -> action bit mask
std::uint32_t g_mouse_actions[8];
std::uint32_t g_wheel_up_actions = 0, g_wheel_down_actions = 0;

std::atomic<bool> g_key_down[kMaxScan];
std::atomic<bool> g_mouse_down[8];
std::atomic<int> g_presses[kActionCount];
std::atomic<std::int64_t> g_mx{0}, g_my{0};
std::atomic<bool> g_gameplay{false};
std::atomic<bool> g_was_free{false};
std::atomic<std::uint32_t> g_epoch{0};
std::atomic<std::int64_t> g_drop_motion_until{0};  // steady_clock ns: mouse motion before this is discarded
std::vector<int> g_cursor_keys;                    // scancodes of nova.ini cursor_key
constexpr double kFixed = 256.0;
constexpr std::int64_t kRecaptureDropNs = 50'000'000;

std::int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
// Physical state of a key: our own tracking (fed by OnKey, also by scripts) or SDL's keyboard state, which survives the
// ReleaseAll on a mode change and is reset by SDL itself on focus loss.
bool KeyHeld(int sc) {
  if (sc <= 0 || sc >= kMaxScan) return false;
  if (g_key_down[sc].load(std::memory_order_relaxed)) return true;
  int n = 0;
  const bool* ks = SDL_GetKeyboardState(&n);
  return ks && sc < n && ks[sc];
}
bool CursorKeyHeld() {
  for (int sc : g_cursor_keys)
    if (KeyHeld(sc)) return true;
  return false;
}

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
std::string Trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t");
  if (b == std::string::npos) return "";
  return s.substr(b, s.find_last_not_of(" \t") - b + 1);
}

void ForMask(std::uint32_t m, auto&& fn) {
  for (int a = 0; a < kActionCount; ++a)
    if (m & (1u << a)) fn(static_cast<Action>(a));
}
}  // namespace

bool ParseToken(const char* token, int& scancode, int& mouse_button) {
  scancode = 0;
  mouse_button = 0;
  const std::string t = Lower(Trim(token));
  if (t.empty() || t == "none") return false;
  // mouse1 = left, mouse2 = right, mouse3 = middle, mouse4/5 = side buttons.
  if (t == "mouse1") return mouse_button = SDL_BUTTON_LEFT, true;
  if (t == "mouse2") return mouse_button = SDL_BUTTON_RIGHT, true;
  if (t == "mouse3") return mouse_button = SDL_BUTTON_MIDDLE, true;
  if (t == "mouse4") return mouse_button = SDL_BUTTON_X1, true;
  if (t == "mouse5") return mouse_button = SDL_BUTTON_X2, true;
  if (t == "lshift" || t == "shift") return scancode = SDL_SCANCODE_LSHIFT, true;
  if (t == "rshift") return scancode = SDL_SCANCODE_RSHIFT, true;
  if (t == "lctrl" || t == "ctrl") return scancode = SDL_SCANCODE_LCTRL, true;
  if (t == "rctrl") return scancode = SDL_SCANCODE_RCTRL, true;
  if (t == "lalt" || t == "alt") return scancode = SDL_SCANCODE_LALT, true;
  const SDL_Scancode sc = SDL_GetScancodeFromName(Trim(token).c_str());
  if (sc == SDL_SCANCODE_UNKNOWN) return false;
  scancode = sc;
  return true;
}

void InitInput() {
  std::fill(std::begin(g_key_actions), std::end(g_key_actions), 0u);
  std::fill(std::begin(g_mouse_actions), std::end(g_mouse_actions), 0u);
  g_wheel_up_actions = g_wheel_down_actions = 0;
  const Config& cfg = Settings();
  g_cursor_keys.clear();
  {
    const std::string& text = cfg.cursor_key;
    std::size_t pos = 0;
    while (pos <= text.size()) {
      std::size_t end = text.find(',', pos);
      if (end == std::string::npos) end = text.size();
      const std::string tok = Trim(text.substr(pos, end - pos));
      pos = end + 1;
      int sc = 0, mb = 0;
      if (tok.empty() || Lower(tok) == "none") continue;
      if (ParseToken(tok.c_str(), sc, mb) && sc > 0) g_cursor_keys.push_back(sc);
      else log::Warn("[pc] nova.ini: unusable cursor_key '{}' (a keyboard key is needed)", tok);
    }
  }
  for (int a = 0; a < kActionCount; ++a) {
    Binding& b = g_bind[a];
    b = Binding{};
    auto it = cfg.keys.find(kActions[a].name);
    const std::string text = it != cfg.keys.end() ? it->second : kActions[a].defaults;
    std::size_t pos = 0;
    while (pos <= text.size()) {
      std::size_t end = text.find(',', pos);
      if (end == std::string::npos) end = text.size();
      const std::string tok = Trim(text.substr(pos, end - pos));
      pos = end + 1;
      if (tok.empty()) continue;
      const std::string low = Lower(tok);
      int sc = 0, mb = 0;
      if (low == "wheelup") {
        b.wheel_up = true;
        g_wheel_up_actions |= 1u << a;
      } else if (low == "wheeldown") {
        b.wheel_down = true;
        g_wheel_down_actions |= 1u << a;
      } else if (ParseToken(tok.c_str(), sc, mb)) {
        if (sc > 0 && sc < kMaxScan) {
          b.keys.push_back(sc);
          g_key_actions[sc] |= 1u << a;
        } else if (mb > 0 && mb < 8) {
          b.mouse.push_back(mb);
          g_mouse_actions[mb] |= 1u << a;
        }
      } else if (low != "none") {
        log::Warn("[pc] nova.ini: unknown key '{}' for {}", tok, kActions[a].name);
      }
    }
  }
}

bool Playing() { return g_gameplay.load(std::memory_order_relaxed); }
bool InGameplay() {
  const bool free_cursor = CursorKeyHeld();
  if (g_was_free.exchange(free_cursor) && !free_cursor) {
    // Cursor key released: back to mouse look. Whatever the mouse did while it was free (and the warp of the recapture
    // itself) must not turn the view.
    g_mx.store(0, std::memory_order_relaxed);
    g_my.store(0, std::memory_order_relaxed);
    g_drop_motion_until.store(NowNs() + kRecaptureDropNs, std::memory_order_relaxed);
  }
  return Playing() && !free_cursor;
}
void SetGameplay(bool on) { g_gameplay.store(on, std::memory_order_relaxed); }

bool OnKey(int scancode, bool down, bool repeat) {
  if (scancode <= 0 || scancode >= kMaxScan) return false;
  g_key_down[scancode].store(down, std::memory_order_relaxed);
  const std::uint32_t m = g_key_actions[scancode];
  if (down && !repeat && Playing())
    ForMask(m, [](Action a) { g_presses[a].fetch_add(1, std::memory_order_relaxed); });
  return m != 0;
}

bool OnMouseButton(int button, bool down) {
  if (button <= 0 || button >= 8) return false;
  g_mouse_down[button].store(down, std::memory_order_relaxed);
  const std::uint32_t m = g_mouse_actions[button];
  if (down && InGameplay()) ForMask(m, [](Action a) { g_presses[a].fetch_add(1, std::memory_order_relaxed); });
  return m != 0;
}

void OnMouseMotion(float dx, float dy) {
  if (!InGameplay() || NowNs() < g_drop_motion_until.load(std::memory_order_relaxed)) return;
  g_mx.fetch_add(static_cast<std::int64_t>(std::llround(dx * kFixed)), std::memory_order_relaxed);
  g_my.fetch_add(static_cast<std::int64_t>(std::llround(dy * kFixed)), std::memory_order_relaxed);
}

void OnWheel(float notches) {
  if (!InGameplay() || notches == 0.0f) return;
  const int n = std::max(1, static_cast<int>(std::lround(std::fabs(notches))));
  const std::uint32_t m = notches > 0 ? g_wheel_up_actions : g_wheel_down_actions;
  ForMask(m, [n](Action a) { g_presses[a].fetch_add(n, std::memory_order_relaxed); });
}

void ReleaseAll() {
  g_epoch.fetch_add(1, std::memory_order_relaxed);
  for (auto& k : g_key_down) k.store(false, std::memory_order_relaxed);
  for (auto& k : g_mouse_down) k.store(false, std::memory_order_relaxed);
  for (auto& p : g_presses) p.store(0, std::memory_order_relaxed);
  g_mx.store(0, std::memory_order_relaxed);
  g_my.store(0, std::memory_order_relaxed);
}

bool Held(Action a) {
  const Binding& b = g_bind[a];
  for (int k : b.keys)
    if (KeyHeld(k)) return true;
  for (int m : b.mouse)
    if (g_mouse_down[m].load(std::memory_order_relaxed)) return true;
  return false;
}

std::uint32_t InputEpoch() { return g_epoch.load(std::memory_order_relaxed); }

int TakePresses(Action a) { return g_presses[a].exchange(0, std::memory_order_relaxed); }

void TakeMouse(double& dx, double& dy) {
  dx = static_cast<double>(g_mx.exchange(0, std::memory_order_relaxed)) / kFixed;
  dy = static_cast<double>(g_my.exchange(0, std::memory_order_relaxed)) / kFixed;
}

}  // namespace nova::pc
