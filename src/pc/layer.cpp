// Hooks that turn the touch game into a PC shooter. Field offsets and addresses are confirmed by decompilation, see
// docs/pc_layer.md. Everything here runs on the guest game thread (inside the hooked functions).
#include "pc/layer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>

#include "core/log.h"
#include "cpu/cpu.h"
#include "cpu/hle.h"
#include "pc/config.h"
#include "pc/hook.h"
#include "pc/input.h"

namespace nova::pc {
namespace {

// ---- guest object layout (libNOVA.so 5.8.4a) ----
constexpr gaddr kHudMoveStrafe = 40, kHudMoveForward = 44;        // Hud: floats set by SetMoveSpeed
constexpr gaddr kHudWeaponChange = 76;                            // Hud: int +1 / -1, consumed once per frame
constexpr gaddr kPlayerFrozen = 365;                              // PlayerComponent: byte, look ignored when set
constexpr gaddr kPlayerControl = 32;                              // PlayerComponent: embedded CPlayerControl
constexpr gaddr kPlayerWeaponMgr = 464;                           // PlayerComponent: CWeaponManager*
constexpr gaddr kPlayerCamera = 472;                              // PlayerComponent: CGameCameraComponent*
constexpr gaddr kPlayerSprinting = 1764;                          // PlayerComponent: byte, sprint mode active
constexpr gaddr kPlayerState = 1824;                              // PlayerComponent: int main state
constexpr gaddr kPlayerInIronSight = 1785;                        // PlayerComponent: byte
constexpr gaddr kPlayerCrouched = 1786;                           // PlayerComponent: byte, crouch stance (set by EnterCover)
constexpr gaddr kPlayerCover = 2560;                              // PlayerComponent: TObstacle* while in cover
constexpr gaddr kPlayerMachineGun = 2140;                         // PlayerComponent: byte, mounted weapon mode
constexpr gaddr kPlayerWeaponRequest = 1836;                      // PlayerComponent: int slot to switch to (-1 none)
constexpr gaddr kPlayerMedkits = 816;                             // PlayerComponent: int, picked-up medkits (Hud button 55)
constexpr gaddr kPlayerNoLook = 2532;                             // PlayerComponent: byte, cinematic / scripted camera
constexpr gaddr kControlYaw = 56, kControlPitch = 60, kControlYawHeld = 64, kControlPitchHeld = 68;  // CPlayerControl
constexpr gaddr kPhysVec = 328;                                   // CPhysicsManager: vector<CCharacterController*>
constexpr gaddr kCcAir = 212, kCcJumping = 215, kCcRadius = 216, kCcHeight = 224, kCcGravityZ = 328;  // CCharacterController
constexpr gaddr kLevelPlayerObject = 776;                         // CLevel: CGameObject* of the player
constexpr gaddr kWmSlotMap = 416, kWmSlotMapEnd = 424;            // CWeaponManager: vector<int> slot -> weapon index
constexpr gaddr kWmWeapons = 392, kWmCurrentSlot = 440;           // CWeaponManager: 24 byte entries (+8 = weapon), slot
constexpr gaddr kCamGameFov = 568;                                // CGameCameraComponent: float, base FOV (rad)
constexpr gaddr kWcTemplate = 72;                                 // CWeaponComponent: weapon template*
constexpr gaddr kWcIronFov = 892, kWcIronFov2 = 896;              // CWeaponComponent: iron sight FOV (deg), override
constexpr gaddr kWtIdleFov = 152;                                 // weapon template: idle FOV (deg, < 0 = game FOV)
constexpr gaddr kLevelFovTimeBase = 728;                          // CLevel: int, added twice to the FOV blend time
// CGameSettings (singleton pointer at CGameSettings::Singleton): the mobile conveniences, all forced to 0.
constexpr gaddr kSetAutoAim = 12, kSetAutoShoot = 16, kSetAdsAutoShoot = 18, kSetAutoSprint = 19, kSetAutoLevel = 20;
constexpr float kDegToRad = 0.017453f;  // the engine's own constant
constexpr gaddr kAppDt = 1484, kAppDtTotal = 1476, kAppDtLast = 1528;  // Application: int ms
constexpr gaddr kRttScale = 0, kRttSize = 4, kRttPostScale = 12, kRttPostSize = 16;  // from DeviceOptions::RTTInfo

// Touch controls (Hud element ids, docs/pc_layer.md section 6). Contextual prompts (skip 17, grab 19, interact 21,
// exit 22, revive 39, repair 63) and the gauges (crosshair 2/13, life 53/54, objectives 25, heating 73) stay.
constexpr int kTouchElements[] = {0, 6, 7, 8, 9, 16, 37, 38, 40, 48, 55, 59, 60, 66, 72, 82};

struct Globals {
  gaddr hud_ptr = 0, level_ptr = 0, menu_ptr = 0, rtt = 0, screen_w = 0, screen_h = 0;
  gaddr f_set_button = 0, f_change_weapon = 0, f_hide = 0, f_player_comp = 0, f_action_phase = 0, f_menu_active = 0;
  gaddr f_restore_shield = 0, f_restore_life = 0, f_restore_ammo = 0;
  gaddr settings = 0, f_cur_weapon = 0, f_cam_fov = 0, f_set_fov = 0, f_ads_enter = 0, f_ads_exit = 0, f_ads_check = 0,
        f_ads_in = 0, f_ads_out = 0;
  gaddr t_look = 0, t_frame = 0, t_hud_update = 0, t_show = 0, t_hud_init = 0, t_hud_reinit = 0, t_load_profile = 0;
  bool hide[128] = {};
  gaddr app = 0;
  gaddr f_jump = 0, phys_ptr = 0, vox_ptr = 0, f_stand = 0, f_set_crouched = 0, f_add_anim = 0, f_vox_play = 0, f_obj_event = 0,
        f_sprint_mode = 0, scratch = 0;
} g;

gaddr Level() { return g.level_ptr ? mem::R64(g.level_ptr) : 0; }

// The local PlayerComponent, or 0 while no player exists (menus, loading).
gaddr PlayerComp() {
  const gaddr level = Level();
  if (!level || !mem::R64(level + kLevelPlayerObject)) return 0;
  return call_guest<gaddr>(g.f_player_comp, level);
}

// ---- aim down sights (engine path: PlayerComponent::EnterIronSight / ExitIronSight / CheckIronSight) ----
// The touch HUD only reaches it through button 7, and PlayerComponent::UpdateIronSight ignores that button unless the
// weapon template has an iron sight interface (scoped weapons), so rifles never aimed. The same three calls are made
// here directly, for every weapon.
gaddr CurrentWeapon(gaddr pc) {
  const gaddr wm = mem::R64(pc + kPlayerWeaponMgr);
  return wm && g.f_cur_weapon ? call_guest<gaddr>(g.f_cur_weapon, wm) : 0;
}

// Hip-fire vertical FOV in radians, the way PlayerComponent::CheckIronSight picks its idle target.
float HipFov(gaddr wc, gaddr cam) {
  const gaddr tmpl = mem::R64(wc + kWcTemplate);
  const float idle = tmpl ? mem::R<float>(tmpl + kWtIdleFov) : -1.0f;
  if (idle >= 0.0f) return idle * kDegToRad;
  return cam ? mem::R<float>(cam + kCamGameFov) : 0.0f;
}

// Mouse sensitivity factor while aiming: the screen angle per mouse count stays the same as in hip fire
// (tan(fov/2) ratio), times the user's ads_sensitivity_scale.
float AdsLookScale(gaddr pc) {
  const Config& cfg = Settings();
  const gaddr cam = mem::R64(pc + kPlayerCamera), wc = CurrentWeapon(pc);
  if (!cam || !wc || !g.f_cam_fov) return cfg.ads_sensitivity_scale;
  const float hip = HipFov(wc, cam), cur = call_guest<float>(g.f_cam_fov, cam);
  if (!(hip > 0.01f) || !(cur > 0.01f)) return cfg.ads_sensitivity_scale;
  const float ratio = std::clamp(std::tan(cur * 0.5f) / std::tan(hip * 0.5f), 0.05f, 1.0f);
  return ratio * cfg.ads_sensitivity_scale;
}

// FOV blend time as CheckIronSight computes it from the weapon's transition time.
float FovBlendTime(float t) {
  return t <= 0.0f ? 0.0f : static_cast<float>(2 * mem::R<std::int32_t>(Level() + kLevelFovTimeBase)) + t;
}

// Starts the zoom for the current ADS state. The engine's CheckIronSight blends to the weapon's iron sight FOV; a
// weapon whose iron sight FOV is not clearly below its idle FOV (most rifles) gets idle / ads_zoom instead.
void AdsApplyFov(gaddr pc, gaddr wc, bool enter) {
  const gaddr cam = mem::R64(pc + kPlayerCamera);
  const float hip = HipFov(wc, cam);
  float iron_deg = mem::R<float>(wc + kWcIronFov2);
  if (iron_deg <= 0.0f) iron_deg = mem::R<float>(wc + kWcIronFov);
  const float iron = iron_deg * kDegToRad;
  const float t = call_guest<float>(enter ? g.f_ads_in : g.f_ads_out, wc);
  if (!enter || (iron > 0.0f && iron < hip * 0.97f)) {
    call_guest<void>(g.f_ads_check, pc, t);
  } else if (cam && hip > 0.01f) {
    call_guest<void>(g.f_set_fov, cam, hip / std::max(1.0f, Settings().ads_zoom), FovBlendTime(t));
  }
}

bool AdsEnter(gaddr pc) {
  const gaddr wc = CurrentWeapon(pc);
  if (!wc) return false;
  const std::uint32_t state = mem::R<std::uint32_t>(pc + kPlayerState);
  if (state <= 18 && ((0x4001Cu >> state) & 1u)) return false;  // the states UpdateIronSight refuses too
  if (mem::R<std::uint8_t>(pc + kPlayerNoLook) || mem::R<std::uint8_t>(pc + kPlayerFrozen)) return false;
  call_guest<void>(g.f_ads_enter, pc, 0.0f);
  if (!mem::R<std::uint8_t>(pc + kPlayerInIronSight)) return false;
  AdsApplyFov(pc, wc, true);
  return true;
}

void AdsExit(gaddr pc) {
  call_guest<void>(g.f_ads_exit, pc, 0.0f);
  if (const gaddr wc = CurrentWeapon(pc)) AdsApplyFov(pc, wc, false);
}

// ---- jump and crouch ----
// The engine has no jump input: nothing in libNOVA calls CCharacterController::Jump (the symbol is only exported), and the
// crouch stance is only entered through PlayerComponent::EnterCover. Both are driven here with the engine's own pieces.
gaddr Scratch() {  // guest memory for the strings / event payload the engine calls take
  if (!g.scratch) {
    g.scratch = GuestMemory::MapAnywhere(Zone::Anon, kPageSize, kProtRW, "pc-scratch");
    auto put = [&](gaddr at, const char* text) {
      for (; *text; ++text, ++at) mem::W<std::uint8_t>(at, static_cast<std::uint8_t>(*text));
      mem::W<std::uint8_t>(at, 0);
    };
    if (g.scratch) {
      put(g.scratch + 0, "sfx_crouch");
      put(g.scratch + 32, "Crouch");
      put(g.scratch + 64, "Idle_Crouch");
    }
  }
  return g.scratch;
}

// The player's CCharacterController: *(*(CPhysicsManager::Singleton + 328)), as PlayerComponent does.
gaddr Controller() {
  if (!g.phys_ptr) return 0;
  const gaddr pm = mem::R64(g.phys_ptr);
  const gaddr begin = pm ? mem::R64(pm + kPhysVec) : 0;
  return begin ? mem::R64(begin) : 0;
}

// Actions (PlayerComponent+1824) in which the player walks around normally: idle, shooting, reload, charge, pump.
bool FreeMovementAction(gaddr pc) {
  const std::uint32_t a = mem::R<std::uint32_t>(pc + kPlayerState);
  return a == 0 || a == 1 || a == 2 || a == 3 || a == 4 || a == 13 || a == 14 || a == 20 || a == 21;
}
bool CanMoveFreely(gaddr pc) {
  return !mem::R<std::uint8_t>(pc + kPlayerNoLook) && !mem::R<std::uint8_t>(pc + kPlayerFrozen) &&
         !mem::R<std::uint8_t>(pc + kPlayerMachineGun) && !mem::R64(pc + kPlayerCover) && FreeMovementAction(pc);
}

// CCharacterController::Jump(speed, seconds): for `seconds` the upward speed is speed * (remaining / seconds)^2 while the
// controller's own gravity (accel at +328, per second) pulls down; it refuses unless the controller is on the ground.
// Solves the speed for an apex of `height` (world units).
float JumpSpeed(float height, float gravity, float seconds) {
  auto apex = [&](float v0) {
    float z = 0, vg = 0;
    for (float t = 0; t < 4.0f; t += 0.002f) {
      const float f = std::max(0.0f, 1.0f - t / seconds);
      const float v = v0 * f * f + vg;
      if (v <= 0 && t > 0) break;
      z += v * 0.002f;
      vg += gravity * 0.002f;
    }
    return z;
  };
  float lo = 0.1f, hi = 200.0f * std::max(1.0f, height);
  for (int i = 0; i < 40; ++i) (apex((lo + hi) * 0.5f) < height ? lo : hi) = (lo + hi) * 0.5f;
  return (lo + hi) * 0.5f;
}

// Returns true when the player left the ground.
bool DoJump(gaddr pc) {
  const gaddr cc = Controller();
  if (!cc || !g.f_jump || !CanMoveFreely(pc)) return false;
  if (mem::R<std::uint8_t>(cc + kCcAir) || mem::R<std::uint8_t>(cc + kCcJumping)) return false;  // the engine's own ground check
  const float total = mem::R<float>(cc + kCcHeight);  // capsule height incl. both end caps
  float gravity = mem::R<float>(cc + kCcGravityZ);
  if (!(total > 0.1f)) return false;
  if (!(gravity < -0.01f)) gravity = -9.8f * total / 2.0f;
  const float height = Settings().jump_height * total;
  const float seconds = 0.35f;
  const float speed = JumpSpeed(height, gravity, seconds);
  call_guest<void>(g.f_jump, cc, speed, seconds);
  log::Info("[pc] jump: capsule {:.2f}, gravity {:.2f}, height {:.2f}, speed {:.2f} over {:.2f} s", total, gravity, height, speed, seconds);
  // The jump / land sounds of CActorSoundsComponent::OnEvent: event 12 with {53, 1} (jump) or {53, 0} (land).
  if (const gaddr obj = mem::R64(Level() + kLevelPlayerObject); obj && g.f_obj_event && Scratch()) {
    mem::W<std::int32_t>(Scratch() + 128, 53);
    mem::W<std::int32_t>(Scratch() + 132, 1);
    call_guest<void>(g.f_obj_event, obj, 12, Scratch() + 128);
  }
  return true;
}

void LandSound(gaddr pc) {
  (void)pc;
  if (const gaddr obj = mem::R64(Level() + kLevelPlayerObject); obj && g.f_obj_event && Scratch()) {
    mem::W<std::int32_t>(Scratch() + 128, 53);
    mem::W<std::int32_t>(Scratch() + 132, 0);
    call_guest<void>(g.f_obj_event, obj, 12, Scratch() + 128);
  }
}

// What PlayerComponent::EnterCover does for the stance (flag, capsule height, sound, additive "Crouch" animation); the
// cover bookkeeping (obstacle pointer) is left out. PlayerComponent::Stand is the way back (flag, capsule, sound, blend out).
void CrouchEnter(gaddr pc, gaddr cc) {
  if (mem::R<std::uint8_t>(pc + kPlayerSprinting) && g.f_sprint_mode) call_guest<void>(g.f_sprint_mode, pc, false);
  if (g.f_vox_play && g.vox_ptr && Scratch()) call_guest<std::int32_t>(g.f_vox_play, mem::R64(g.vox_ptr), Scratch() + 0, -1, 0);
  mem::W<std::uint8_t>(pc + kPlayerCrouched, 1);
  call_guest<void>(g.f_set_crouched, cc, true);
  if (!mem::R<std::uint8_t>(pc + kPlayerInIronSight) && g.f_add_anim && Scratch())
    call_guest<void>(g.f_add_anim, pc, Scratch() + 32, Scratch() + 64, 200);
}

// ---- mobile conveniences: auto aim, auto fire, auto sprint ----
// All five live in CGameSettings (set from the touch options menu, default on, persisted in settings.dat) and are read
// directly from the struct. Forced to 0 every frame, so neither the defaults, a loaded settings.dat nor the options
// menu can bring them back; CLevel::GetAutoAim / GetAutoShoot (which also honour Lua overrides) are replaced too.
void ForceMobileAssistsOff() {
  if (!g.settings) return;
  const gaddr gs = mem::R64(g.settings);
  if (!gs) return;
  const std::int32_t aim = mem::R<std::int32_t>(gs + kSetAutoAim);
  const std::uint8_t shoot = mem::R<std::uint8_t>(gs + kSetAutoShoot), ads_shoot = mem::R<std::uint8_t>(gs + kSetAdsAutoShoot),
                     sprint = mem::R<std::uint8_t>(gs + kSetAutoSprint), level = mem::R<std::uint8_t>(gs + kSetAutoLevel);
  if (aim == 0 && !shoot && !ads_shoot && !sprint && !level) return;
  log::Info("[pc] mobile assists were auto aim {}, auto fire {}, auto fire when aiming {}, auto sprint {}, auto level {}: forced off",
            aim, shoot, ads_shoot, sprint, level);
  mem::W<std::uint8_t>(gs + kSetAutoLevel, 0);
  mem::W<std::int32_t>(gs + kSetAutoAim, 0);
  mem::W<std::uint8_t>(gs + kSetAutoShoot, 0);
  mem::W<std::uint8_t>(gs + kSetAdsAutoShoot, 0);
  mem::W<std::uint8_t>(gs + kSetAutoSprint, 0);
}

// ---- raw mouse look: after CPlayerControl::UpdateFromInput ----
void H_Look(Cpu& cpu) {
  hook::CallOriginal(cpu, g.t_look);
  if (!Playing()) return;  // mouse counts only accumulate while the cursor is captured
  const gaddr self = cpu.x(0);
  const gaddr pc = PlayerComp();
  if (!pc || self != pc + kPlayerControl) return;
  double dx = 0, dy = 0;
  TakeMouse(dx, dy);
  if (mem::R<std::uint8_t>(pc + kPlayerNoLook) || mem::R<std::uint8_t>(pc + kPlayerFrozen)) return;
  if (dx == 0 && dy == 0) return;
  const Config& cfg = Settings();
  const float s = cfg.sensitivity * (mem::R<std::uint8_t>(pc + kPlayerInIronSight) ? AdsLookScale(pc) : 1.0f);
  // Touch path (CPlayerControl::onEvent): yaw -= k * dx, pitch -= k * dy (+= with invert); degrees. Same signs.
  const float yaw = std::clamp(-static_cast<float>(dx) * s, -90.0f, 90.0f);
  const float pitch = std::clamp((cfg.invert_y ? 1.0f : -1.0f) * static_cast<float>(dy) * s, -90.0f, 90.0f);
  mem::W<float>(self + kControlYaw, yaw);
  mem::W<float>(self + kControlYawHeld, yaw);
  mem::W<float>(self + kControlPitch, pitch);
  mem::W<float>(self + kControlPitchHeld, pitch);
  static bool logged = false;
  if (!logged) {
    logged = true;
    log::Info("[pc] raw look active: first frame counts ({:.1f}, {:.1f}) -> yaw {:.3f} pitch {:.3f} deg", dx, dy, yaw, pitch);
  }
}

// ---- frame time + gameplay detection: after Application::UpdateFrameTime(app, prev_ms, now_ms) ----
void H_FrameTime(Cpu& cpu) {
  hook::CallOriginal(cpu, g.t_frame);
  const gaddr app = cpu.x(0);
  g.app = app;

  // Whole-millisecond delta time with the remainder carried over, instead of the engine's trimmed mean that is floored
  // (about 14% slow at 144 fps, zero above 500 fps).
  using Clock = std::chrono::steady_clock;
  static Clock::time_point last{};
  static double acc = 0;
  const auto now = Clock::now();
  if (last.time_since_epoch().count() != 0) {
    const double real = std::chrono::duration<double, std::milli>(now - last).count();
    const int raw = static_cast<int>(cpu.x(2)) - static_cast<int>(cpu.x(1));  // what the engine measured
    if (real - raw > 3.0) {
      acc = 0;  // the engine reset its timestamps (state change / loading): keep its value
    } else {
      acc += real;
      int dt = static_cast<int>(acc);
      acc -= dt;
      if (dt > 100) {
        dt = 100;
        acc = 0;
      }
      const std::int32_t old = mem::R<std::int32_t>(app + kAppDt);
      static double real_sum = 0;
      static std::int64_t game_sum = 0, engine_sum = 0, frames = 0;
      real_sum += real;
      game_sum += dt;
      engine_sum += old;
      if (++frames >= 600) {
        log::Info("[pc] frame time over {} frames: real {:.0f} ms, game dt {} ms, engine dt would have been {} ms", frames,
                  real_sum, game_sum, engine_sum);
        real_sum = 0;
        game_sum = engine_sum = frames = 0;
      }
      mem::W<std::int32_t>(app + kAppDt, dt);
      mem::W<std::int32_t>(app + kAppDtLast, dt);
      mem::W<std::int32_t>(app + kAppDtTotal, mem::R<std::int32_t>(app + kAppDtTotal) + (dt - old));
    }
  }
  last = now;

  ForceMobileAssistsOff();

  // Active gameplay: in the game state, no Flash menu / popup on top, a player exists.
  bool play = false, action = false, menu = false;
  if (Level() && mem::R64(Level() + kLevelPlayerObject)) {
    action = call_guest<bool>(g.f_action_phase, app);
    menu = call_guest<bool>(g.f_menu_active, mem::R64(g.menu_ptr), -1);
    play = action && !menu;
  }
  static int prev = -1;
  if (static_cast<int>(play) != prev) {
    prev = play;
    log::Info("[pc] gameplay {} (action phase {}, menu active {})", play ? "ON" : "off", action, menu);
  }
  SetGameplay(play);
}

// ---- keyboard / mouse buttons into the Hud: after Hud::Update (button edges are consumed next frame) ----
struct HudButton {
  Action action;
  std::vector<int> ids;
  bool hold;  // hold: key down / up follow the button; otherwise a tap (down + up) per press
};
const std::vector<HudButton> kButtons = {
    {kFire, {6}, true},       {kGrenade, {16}, true},   {kSprint, {5}, true},      {kReload, {0}, false},
    {kMelee, {48}, false},    {kInteract, {21, 19, 22}, false},
    {kBoost, {8}, false},
};

void SetButton(gaddr hud, int id, bool down) { call_guest<void>(g.f_set_button, hud, id, down); }
void Tap(gaddr hud, const std::vector<int>& ids) {
  for (int id : ids) SetButton(hud, id, true);
  for (int id : ids) SetButton(hud, id, false);
}

void H_HudUpdate(Cpu& cpu) {
  hook::CallOriginal(cpu, g.t_hud_update);
  const gaddr hud = cpu.x(0);
  static bool applied[kActionCount];
  static bool ads_toggle = false, crouch_toggle = false, crouch_ours = false, jumped = false;
  static std::uint32_t epoch = 0;
  if (!Playing()) {  // not InGameplay(): keys keep working while the free-cursor key is held
    std::fill(std::begin(applied), std::end(applied), false);
    ads_toggle = crouch_toggle = crouch_ours = jumped = false;
    for (int a = 0; a < kActionCount; ++a) TakePresses(static_cast<Action>(a));
    return;
  }
  const gaddr pc = PlayerComp();
  if (!pc) return;
  // Mode switch (free cursor, menu, focus loss): every held key was dropped, so no desired aim / crouch survives it.
  if (const std::uint32_t e = InputEpoch(); e != epoch) {
    epoch = e;
    ads_toggle = crouch_toggle = false;
  }

  // Movement: WASD as a stick vector, diagonals normalised.
  float strafe = static_cast<float>(Held(kRight)) - static_cast<float>(Held(kLeft));
  float fwd = static_cast<float>(Held(kForward)) - static_cast<float>(Held(kBack));
  const float len = std::sqrt(strafe * strafe + fwd * fwd);
  if (len > 1.0f) {
    strafe /= len;
    fwd /= len;
  }
  mem::W<float>(hud + kHudMoveStrafe, strafe);
  mem::W<float>(hud + kHudMoveForward, fwd);

  // Buttons.
  for (const HudButton& b : kButtons) {
    bool want = Held(b.action);
    const int presses = TakePresses(b.action);
    if (b.action == kSprint) want = want && fwd > 0.0f;  // the engine runs forward whenever sprint is held
    if (b.hold) {
      if (want && !applied[b.action]) {
        for (int id : b.ids) SetButton(hud, id, true);
        applied[b.action] = true;
      } else if (!want && applied[b.action]) {
        for (int id : b.ids) SetButton(hud, id, false);
        applied[b.action] = false;
      } else if (!want && presses > 0 && b.action != kSprint) {
        Tap(hud, b.ids);  // pressed and released within one frame
      }
    } else if (presses > 0) {
      Tap(hud, b.ids);
    }
  }

  // Consumable slots, the same functions the HUD icons reach (Consumable_OnUse -> onRestore*; they check the count and
  // that there is something to restore, use up the item, update the icon and raise the tutorial's "armor used" trigger).
  if (TakePresses(kArmor) > 0 && g.f_restore_shield) call_guest<void>(g.f_restore_shield);
  if (TakePresses(kMedkit) > 0) {
    // A picked-up medkit (Hud button 55, as the touch icon) before the energy cell consumable.
    if (mem::R<std::int32_t>(pc + kPlayerMedkits) >= 1) Tap(hud, {55});
    else if (g.f_restore_life) call_guest<void>(g.f_restore_life);
  }
  if (TakePresses(kAmmo) > 0 && g.f_restore_ammo) call_guest<void>(g.f_restore_ammo);

  // One-time facts for the log: crosshair displacement (screen pixels, from the weapon manager) and the controller.
  {
    static bool logged = false;
    const gaddr wm = mem::R64(pc + kPlayerWeaponMgr), cc = Controller();
    if (!logged && wm && cc) {
      logged = true;
      log::Info("[pc] crosshair displacement ({}, {}) px on {}x{}; controller capsule {:.2f} (radius {:.2f}), gravity z {:.3f}, air {}",
                mem::R<std::int32_t>(wm + 264), mem::R<std::int32_t>(wm + 268), mem::R<std::int32_t>(g.screen_w),
                mem::R<std::int32_t>(g.screen_h), mem::R<float>(cc + kCcHeight), mem::R<float>(cc + kCcRadius),
                mem::R<float>(cc + kCcGravityZ), mem::R<std::uint8_t>(cc + kCcAir));
    }
  }

  // Aim down sights, hold or toggle (nova.ini aim_mode). Self-correcting: every frame the desired state (RMB held, or the
  // toggle) is compared with the game's real iron sight flag and Enter / Exit is called until they match. The game
  // refuses either call in some states (reload, weapon swap, cinematic) and its own scripts re-enter an aim after a
  // reload, so a one-shot call (as before) could leave the aim stuck on after the button was released.
  {
    static bool prev_ads = false, we_exited = false, zoomed = false;
    const int presses = TakePresses(kAim);
    const bool ads = mem::R<std::uint8_t>(pc + kPlayerInIronSight) != 0;
    bool want;
    if (Settings().aim_mode == "toggle") {
      if (presses > 0) ads_toggle = !ads_toggle;
      if (prev_ads && !ads && !we_exited) ads_toggle = false;  // the game ended it (weapon swap, sprint, cinematic)
      want = ads_toggle;
    } else {
      want = Held(kAim);
    }
    if (want && !ads) {
      we_exited = false;
      if (AdsEnter(pc)) zoomed = true;
    } else if (!want && ads) {
      we_exited = true;
      AdsExit(pc);
    }
    const bool now = mem::R<std::uint8_t>(pc + kPlayerInIronSight) != 0;
    if (now && !prev_ads) {  // an aim started by the game itself (scripts after a reload) still gets the zoom
      if (const gaddr wc = CurrentWeapon(pc); wc && !zoomed) AdsApplyFov(pc, wc, true);
      zoomed = true;
    } else if (!now && prev_ads) {  // however it ended, the zoom goes back with it
      if (const gaddr wc = CurrentWeapon(pc); wc && !we_exited) AdsApplyFov(pc, wc, false);
      zoomed = false;
      we_exited = false;
    }
    prev_ads = now;
  }

  // Jump: one jump per Space press, only from the ground (the controller refuses otherwise). A crouched player stands up
  // first instead of jumping.
  {
    const int presses = TakePresses(kJump);
    const gaddr cc = Controller();
    if (presses > 0 && cc) {
      if (mem::R<std::uint8_t>(pc + kPlayerCrouched) && crouch_ours && g.f_stand) {
        call_guest<void>(g.f_stand, pc);
        crouch_ours = crouch_toggle = false;
      } else if (DoJump(pc)) {
        jumped = true;
      }
    }
    if (jumped && cc && !mem::R<std::uint8_t>(cc + kCcAir) && !mem::R<std::uint8_t>(cc + kCcJumping)) {
      jumped = false;
      LandSound(pc);
    }
  }

  // Crouch (nova.ini crouch_mode): hold C / Left Ctrl, or toggle. The engine only crouches in cover, so the stance is set
  // up here the same way (see CrouchEnter) and kept in step every frame like the aim; cover entered while crouched keeps it.
  {
    const int presses = TakePresses(kCrouch);
    const gaddr cc = Controller();
    const bool sprinting_key = Held(kSprint) && fwd > 0.0f;
    bool want;
    if (Settings().crouch_mode == "toggle") {
      if (presses > 0) crouch_toggle = !crouch_toggle;
      if (sprinting_key) crouch_toggle = false;
      want = crouch_toggle;
    } else {
      want = Held(kCrouch) && !sprinting_key;
    }
    const bool crouched = mem::R<std::uint8_t>(pc + kPlayerCrouched) != 0;
    if (!crouched) crouch_ours = false;  // ended by the game (cover exit, ForceStand, respawn)
    const bool allowed = cc && g.f_stand && g.f_set_crouched && CanMoveFreely(pc) && !mem::R<std::uint8_t>(cc + kCcAir);
    if (want && allowed && !crouched) {
      CrouchEnter(pc, cc);
      crouch_ours = true;
    } else if (crouched && crouch_ours && (!want || !allowed) && !mem::R64(pc + kPlayerCover)) {
      call_guest<void>(g.f_stand, pc);
      crouch_ours = false;
    }
  }

  // Diagnostics: sprint / aim transitions with the camera FOV, so a log shows what the engine did.
  {
    static bool prev_sprint = false, prev_ads = false;
    static int ads_age = -1;
    const bool sprint = mem::R<std::uint8_t>(pc + kPlayerSprinting) != 0;
    if (sprint != prev_sprint) {
      prev_sprint = sprint;
      log::Info("[pc] sprint {} (sprint key {}, forward {:.1f})", sprint ? "ON" : "off", Held(kSprint), fwd);
    }
    const bool ads = mem::R<std::uint8_t>(pc + kPlayerInIronSight) != 0;
    const gaddr cam = mem::R64(pc + kPlayerCamera);
    auto fov_deg = [&] { return cam && g.f_cam_fov ? call_guest<float>(g.f_cam_fov, cam) / kDegToRad : 0.0f; };
    if (ads != prev_ads) {
      prev_ads = ads;
      ads_age = 0;
      const gaddr wc = CurrentWeapon(pc);
      log::Info("[pc] aim {}: camera fov {:.1f} deg, weapon idle {:.1f} iron {:.1f}/{:.1f}, look scale {:.3f}", ads ? "ON" : "off",
                fov_deg(), wc ? HipFov(wc, cam) / kDegToRad : 0.0f, wc ? mem::R<float>(wc + kWcIronFov) : 0.0f,
                wc ? mem::R<float>(wc + kWcIronFov2) : 0.0f, ads ? AdsLookScale(pc) : 1.0f);
    } else if (ads_age >= 0 && ++ads_age == 120) {
      ads_age = -1;
      log::Info("[pc] aim {} settled: camera fov {:.1f} deg", ads ? "ON" : "off", fov_deg());
    }
  }

  // Weapons: 1-4 pick a slot, wheel / next / prev step through them (Hud::ChangeWeapon, one step per frame).
  const gaddr wm = mem::R64(pc + kPlayerWeaponMgr);
  for (int i = 0; i < 4 && wm; ++i) {
    if (!TakePresses(static_cast<Action>(kWeapon1 + i))) continue;
    const gaddr begin = mem::R64(wm + kWmSlotMap), end = mem::R64(wm + kWmSlotMapEnd);
    if (i >= static_cast<int>((end - begin) / 4)) continue;
    const std::int32_t idx = mem::R<std::int32_t>(begin + 4ull * i);
    if (idx == -1 || !mem::R64(mem::R64(wm + kWmWeapons) + 24ull * static_cast<std::uint32_t>(idx) + 8)) continue;
    if (i != mem::R<std::int32_t>(wm + kWmCurrentSlot)) mem::W<std::int32_t>(pc + kPlayerWeaponRequest, i);
  }
  static int steps = 0;
  steps += TakePresses(kWeaponNext) - TakePresses(kWeaponPrev);
  steps = std::clamp(steps, -8, 8);
  if (steps != 0) {
    call_guest<void>(g.f_change_weapon, hud, steps > 0 ? 1 : -1);
    steps += steps > 0 ? -1 : 1;
  }
}

// ---- touch HUD ----
bool Hidden(int id) { return id >= 0 && id < 128 && g.hide[id]; }

void HideTouchHud(gaddr hud) {
  for (int id = 0; id < 128; ++id)
    if (g.hide[id]) call_guest<void>(g.f_hide, hud, id, true);
}

// Hud::ShowElement(hud, id, flag): a touch control is hidden instead of shown.
void H_ShowElement(Cpu& cpu) {
  const gaddr hud = cpu.x(0);
  const int id = static_cast<int>(cpu.x(1));
  if (Hidden(id)) {
    call_guest<void>(g.f_hide, hud, id, true);
    cpu.set_x(0, hud);
    return;
  }
  hook::Forward(cpu, hook::CallOriginal(cpu, g.t_show));
}
void H_HudInit(Cpu& cpu) {
  const gaddr hud = cpu.x(0);  // Hud::Init returns void: x0 is not the Hud afterwards
  hook::CallOriginal(cpu, g.t_hud_init);
  HideTouchHud(hud);
}
void H_HudReInit(Cpu& cpu) {
  const gaddr hud = cpu.x(0);
  hook::CallOriginal(cpu, g.t_hud_reinit);
  HideTouchHud(hud);
}

// ---- render resolution: RTT scales forced to 1.0 after DeviceOptions::LoadProfile ----
void H_LoadProfile(Cpu& cpu) {
  hook::Forward(cpu, hook::CallOriginal(cpu, g.t_load_profile));  // x0 is whatever the original left, as the caller saw it
  if (!g.rtt || !g.screen_w || !g.screen_h) return;
  // LoadProfile picked per-GPU scales and derived the RTT sizes (multiples of 4) from them: redo that with 1.0.
  const std::int32_t w = mem::R<std::int32_t>(g.screen_w), h = mem::R<std::int32_t>(g.screen_h);
  const std::int32_t rw = (w + 3) & ~3, rh = (h + 3) & ~3;
  mem::W<float>(g.rtt + kRttScale, 1.0f);
  mem::W<float>(g.rtt + kRttPostScale, 1.0f);
  mem::W<std::int32_t>(g.rtt + kRttSize, rw);
  mem::W<std::int32_t>(g.rtt + kRttSize + 4, rh);
  mem::W<std::int32_t>(g.rtt + kRttPostSize, rw);
  mem::W<std::int32_t>(g.rtt + kRttPostSize + 4, rh);
  log::Info("[pc] render scale forced to 1.0 ({}x{})", rw, rh);
}

// ---- no frame cap / energy ----
void H_Nothing(Cpu& cpu) { cpu.set_x(0, 0); }

}  // namespace

int InstallLayer(const loader::Module& m) {
  const Config& cfg = Settings();
  int installed = 0;
  auto sym = [&](const char* name) -> gaddr {
    const gaddr a = m.Lookup(name);
    if (!a) log::Warn("[pc] symbol {} not found in {}", name, m.name);
    return a;
  };
  auto wrap = [&](const char* name, const char* hook_name, hle::RawFn fn, gaddr& tramp) {
    const gaddr a = sym(name);
    if (!a) return;
    tramp = hook::Wrap(a, hook_name, fn);
    if (tramp) {
      ++installed;
      log::Info("[pc] hooked {} at 0x{:x}", name, a);
    }
  };
  auto replace = [&](const char* name, const char* hook_name, hle::RawFn fn) {
    const gaddr a = sym(name);
    if (!a) return;
    if (hook::Replace(a, hook_name, fn)) {
      ++installed;
      log::Info("[pc] replaced {} at 0x{:x}", name, a);
    }
  };

  g.hud_ptr = sym("_ZN3Hud10s_InstanceE");
  g.level_ptr = sym("_ZN6CLevel10m_instanceE");
  g.menu_ptr = sym("_ZN11MenuManager10s_instanceE");
  g.rtt = sym("_ZN13DeviceOptions7RTTInfoE");
  g.screen_w = sym("g_ScreenW");
  g.screen_h = sym("g_ScreenH");
  g.f_set_button = sym("_ZN3Hud14SetButtonStateEib");
  g.f_restore_shield = sym("_Z15onRestoreShieldv");
  g.f_restore_life = sym("_Z13onRestoreLifev");
  g.f_restore_ammo = sym("_Z13onRestoreAmmov");
  g.f_change_weapon = sym("_ZN3Hud12ChangeWeaponEi");
  g.f_hide = sym("_ZN3Hud11HideElementEib");
  g.f_player_comp = sym("_ZN6CLevel18GetPlayerComponentEv");
  g.f_action_phase = sym("_ZN11Application15IsInActionPhaseEv");
  g.f_menu_active = sym("_ZN11MenuManager12IsMenuActiveEi");
  g.settings = sym("_ZN13CGameSettings9SingletonE");
  g.f_cur_weapon = sym("_ZN14CWeaponManager25GetCurrentWeaponComponentEv");
  g.f_cam_fov = sym("_ZNK20CGameCameraComponent12GetCameraFovEv");
  g.f_set_fov = sym("_ZN20CGameCameraComponent19SetFovInterpolationEff");
  g.f_ads_enter = sym("_ZN15PlayerComponent14EnterIronSightEf");
  g.f_ads_exit = sym("_ZN15PlayerComponent13ExitIronSightEf");
  g.f_ads_check = sym("_ZN15PlayerComponent14CheckIronSightEf");
  g.f_ads_in = sym("_ZNK16CWeaponComponent26GetIronSightTransitionTimeEv");
  g.f_ads_out = sym("_ZNK16CWeaponComponent29GetIronSightTransitionTimeOutEv");
  g.f_jump = sym("_ZN20CCharacterController4JumpEff");
  g.f_set_crouched = sym("_ZN20CCharacterController11SetCrouchedEb");
  g.f_stand = sym("_ZN15PlayerComponent5StandEv");
  g.f_add_anim = sym("_ZN15PlayerComponent17PlayAddActionAnimEPKcS1_i");
  g.f_sprint_mode = sym("_ZN15PlayerComponent16SwitchSprintModeEb");
  g.f_vox_play = sym("_ZN15VoxSoundManager4PlayEPKcii");
  g.f_obj_event = sym("_ZN11CGameObject7OnEventEiPKv");
  g.phys_ptr = sym("_ZN15CPhysicsManager9SingletonE");
  g.vox_ptr = sym("_ZN15VoxSoundManager9SingletonE");

  std::fill(std::begin(g.hide), std::end(g.hide), false);
  if (cfg.hide_touch_hud) {
    for (int id : kTouchElements) g.hide[id] = true;
    std::stringstream ss(cfg.hud_hide);
    std::string tok;
    while (std::getline(ss, tok, ','))
      if (const int id = std::atoi(tok.c_str()); id > 0 && id < 128) g.hide[id] = true;
  }

  const bool have_state = g.level_ptr && g.f_player_comp;
  if (have_state) {
    wrap("_ZN14CPlayerControl15UpdateFromInputEi", "pc::CPlayerControl::UpdateFromInput", H_Look, g.t_look);
    if (g.f_action_phase && g.f_menu_active && g.menu_ptr)
      wrap("_ZN11Application15UpdateFrameTimeEii", "pc::Application::UpdateFrameTime", H_FrameTime, g.t_frame);
    if (g.f_set_button && g.f_change_weapon)
      wrap("_ZN3Hud6UpdateEi", "pc::Hud::Update", H_HudUpdate, g.t_hud_update);
  }
  if (cfg.hide_touch_hud && g.f_hide) {
    wrap("_ZN3Hud11ShowElementEib", "pc::Hud::ShowElement", H_ShowElement, g.t_show);
    wrap("_ZN3Hud4InitEv", "pc::Hud::Init", H_HudInit, g.t_hud_init);
    wrap("_ZN3Hud6ReInitEv", "pc::Hud::ReInit", H_HudReInit, g.t_hud_reinit);
  }
  wrap("_ZN13DeviceOptions11LoadProfileEv", "pc::DeviceOptions::LoadProfile", H_LoadProfile, g.t_load_profile);
  replace("_ZN11Application8LimitFpsEv", "pc::Application::LimitFps", H_Nothing);
  // The pitch auto-centre (settings "auto leveling", on by default) eases the view back to level whenever the player has
  // not moved for a while: off for good (the settings byte is forced to 0 every frame as well).
  replace("_ZN15PlayerComponent18UpdateAutoLevelingEi", "pc::PlayerComponent::UpdateAutoLeveling", H_Nothing);
  // Turn-dependent weapon spread (CWeaponComponent::UpdateConeRadiusTurn adds ConeIncTurn * turned radians per update to the
  // cone): a mouse turns far faster than a touch drag, so the cone sits at its maximum while looking around. No-op unless
  // nova.ini turn_spread = 1; the cone still decays to / rises with the standing, moving, aim and per-shot values.
  if (!cfg.turn_spread)
    replace("_ZN16CWeaponComponent20UpdateConeRadiusTurnEfbb", "pc::CWeaponComponent::UpdateConeRadiusTurn", H_Nothing);
  // Auto fire / auto aim off for good, whatever the settings struct or a Lua script says.
  replace("_ZN6CLevel10GetAutoAimEv", "pc::CLevel::GetAutoAim", H_Nothing);
  replace("_ZN6CLevel12GetAutoShootEv", "pc::CLevel::GetAutoShoot", H_Nothing);
  if (cfg.free_energy) {
    replace("_ZNK13PlayerProfile17IsEnergyUseActiveEv", "pc::PlayerProfile::IsEnergyUseActive", H_Nothing);
    replace("_ZNK13PlayerProfile20IsTLEEnergyUseActiveEv", "pc::PlayerProfile::IsTLEEnergyUseActive", H_Nothing);
    replace("_ZNK11RewardsData10TLevelInfo13GetEnergyCostEv", "pc::TLevelInfo::GetEnergyCost", H_Nothing);
  }
  return installed;
}

}  // namespace nova::pc
