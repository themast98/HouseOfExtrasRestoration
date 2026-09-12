#pragma once

// AmebaFloor: the Ameba collaboration decal on the coliseum floor in Battle
// King / Fastest Killer (PS3 parity). Detours the coliseum scene-flag function
// and, when a ranking coliseum mode is armed, re-raises the scene flag PC's
// version unconditionally clears.
namespace ameba {

// Installs the detour. Returns false (and installs nothing) if the function was
// not resolved or its prologue is not the 16 bytes the trampoline expects.
bool Install();

}  // namespace ameba
