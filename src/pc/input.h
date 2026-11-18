// Keyboard / mouse state shared between the host (SDL) thread and the guest game thread.
//
// The host thread feeds raw SDL input in (OnKey, OnMouse*, OnWheel); the game thread reads it once per frame from the
// hooks in layer.cpp and turns it into Hud button / move / look calls. Nothing here touches the guest.
#pragma once
#include <cstdint>

namespace nova::pc {

enum Action : int {
  kFire, kAim, kReload, kJump, kCrouch, kGrenade, kMelee, kInteract, kSprint,
  kArmor, kMedkit, kAmmo, kBoost,  // consumable / item slots 1-4 of the HUD
  kWeapon1, kWeapon2, kWeapon3, kWeapon4, kWeaponNext, kWeaponPrev,
  kForward, kBack, kLeft, kRight,
  kActionCount
};
struct ActionInfo {
  const char* name;      // key of the [keys] section in nova.ini
  const char* defaults;  // default binding text
};
extern const ActionInfo kActions[kActionCount];

// Builds the binding tables from Settings(). Call once before events arrive (and after Load).
void InitInput();

// ---- host thread ----
// True while the mouse is captured and acts as a mouse look: active gameplay (published by the game thread) and the
// free-cursor key (nova.ini cursor_key, default Left Alt) not held. Otherwise the mouse is a touch screen. Also tracks the
// free-cursor key: the mouse counts accumulated around a recapture are discarded (no view jump).
bool InGameplay();
// True while the player is in active gameplay, free-cursor key or not (keys and movement keep working while the cursor
// is free). Game thread.
bool Playing();
// SDL scancode (SDL_Scancode) / mouse button (SDL_BUTTON_*) transitions. Always tracked; presses only count as
// "pressed since last frame" while in gameplay. Return true if the key / button is bound to an action.
bool OnKey(int scancode, bool down, bool repeat);
bool OnMouseButton(int button, bool down);
// Relative mouse counts (gameplay only; ignored otherwise) and wheel notches (positive = away from the user).
void OnMouseMotion(float dx, float dy);
void OnWheel(float notches);
// Drops every held key / button and pending press (focus lost, leaving gameplay).
void ReleaseAll();
// Name -> SDL scancode / token resolver used by the script runner ("W", "Space", "mouse1"...). False if unknown.
bool ParseToken(const char* token, int& scancode, int& mouse_button);

// ---- guest (game) thread ----
void SetGameplay(bool on);
bool Held(Action a);
// Counter bumped by every ReleaseAll() (mode switch into / out of gameplay, free cursor, focus loss). Per-frame state
// machines that keep a "desired" state (toggle aim, toggle crouch) clear it when this changes.
std::uint32_t InputEpoch();
// Number of presses of `a` since the last call (key down events and wheel notches), consumed.
int TakePresses(Action a);
// Accumulated mouse counts since the last call, consumed.
void TakeMouse(double& dx, double& dy);

}  // namespace nova::pc
