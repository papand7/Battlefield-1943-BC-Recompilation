// LIVE SPECTATE MODE: watching a running match without a soldier of your own.
//
//   --spectate (or SPECTATE on the HOST GAME page): the host starts the match
//   and never deploys; F4 (while playing or dead) starts and stops it at any
//   time. The camera follows a player (chase view behind the soldier, or first
//   person when zoomed in all the way) with N / B (next / previous player),
//   Space (follow <-> free camera), F3 (free camera), the mouse wheel (the
//   chase distance). Dead players are skipped. A vehicle's occupant is
//   followed by the vehicle's own position.
//   When the player dies in a normal game (WATCH WHEN DEAD, on by default) the
//   camera follows the killer, or a teammate, until the spawn screen shows.
//
// WHERE THE DATA COMES FROM: the same as the match recorder (replay.cpp): the
// server's soldiers, spawners and vehicles in guest memory, read ten times a
// second and interpolated for every frame. Only a host (--host, with or
// without bots) has them: a JOINING client's game holds replicated client
// soldier entities (vtable 82007518, position at +3D0: bots.cpp reads those
// to find its own soldier) and no server soldiers, so a joiner can't spectate
// (its F4 says so); a dedicated server has no window to spectate in.
#pragma once
#include <cstdint>

namespace rt::spectate {
extern bool g_flag;      // --spectate / the host page: spectate from the level's start
extern bool g_on_death;  // setting: follow the killer or a teammate after dying

// Starts the reader / camera thread (main.cpp).
void Start();
// Spectating by choice (flag or F4): the spawn screen isn't shown, the HUD is hidden.
bool Active();
// A death follow (a normal game, dead: until the spawn screen).
bool DeathWatch();
// The in-level menu hook (823CD3E8): what g_level_menu becomes for `show`.
bool MenuValue(bool show);
// A soldier was killed (replay.cpp's Killed: every damage hook's kill).
void Killed(uint32_t victim, uint32_t killer);
// F3 while spectating (the camera hook).
void FreeKey();
}  // namespace rt::spectate
