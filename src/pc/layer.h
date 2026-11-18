// The PC layer: hooks into libNOVA.so that make the arm64 touch game play like a PC shooter (docs/pc_layer.md).
#pragma once
#include "loader/elf64.h"

namespace nova::pc {

// Installs every hook (raw mouse look, keyboard / mouse buttons into the Hud, touch HUD hiding, frame timing,
// campaign energy, render scale). Call before any guest thread starts (game::Boot does, right after the anti-tamper
// patches). Settings() must be loaded first. Returns the number of hooks installed.
int InstallLayer(const loader::Module& module);

}  // namespace nova::pc
