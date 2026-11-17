// FREE CAMERA (spectator): F3 detaches the level view from the player and
// flies it with the keyboard and mouse while the game keeps running; F3 again
// gives the view back. The replay viewer (replay.cpp) flies it the same way.
//
// The game's camera reaches the renderer through the camera manager's frame,
// sub_82294A58(manager): it builds a view on its stack and hands it its
// transform with sub_82583348(view, matrix), a camera-to-world matrix of four
// rows (x, y, z, w floats): right, up, back (the view looks along minus the
// third row) and the eye's position, the world y up (the bots' convention:
// forward = (sin yaw, cos yaw) in x/z, turning right lowers yaw). game_patches.cpp puts this camera's matrix in
// place of the game's there; the view (culling and level of detail included)
// then follows it.
#pragma once
#include <cstdint>

namespace rt::freecam {
// The view is ours (F3 on, or the replay viewer).
bool Active();
// Switch it on or off (on: starts where the game's camera is now).
void Toggle();
void SetActive(bool on);
// Keyboard and mouse: F3 is looked at on every pad poll (xam.cpp, focused or
// tests); while active the game's pad gets none of the movement keys.
void PollKeys(bool menus);
// Raw mouse counts (x right, y down) while active.
void AddMouse(long dx, long dy, long wheel);
// The camera manager's frame starts (game_patches.cpp): one movement step.
void FrameStep();
// The view's transform: the game's matrix (guest, 16 floats) in, ours out.
// False: leave the game's.
bool Override(const float game[16], float out[16]);
// The view's vertical field of view (radians) as handed to it (SetFov).
void NoteFov(float vertical_radians);
// The camera now (for drawing markers): eye, forward, left, up, vertical FOV.
// False when not active.
struct View {
  float eye[3], forward[3], left[3], up[3];
  float fov_y;
};
bool CurrentView(View& v);
// Place the camera (replay: a jump to a player).
void PlaceAt(const float eye[3], float yaw, float pitch);
// A key held (the game window in front, or a test's key file).
bool KeyHeld(int vk);
// The spectator (spectate.h) drives the camera: its step runs at the start of
// each camera frame (it places the camera: PlaceAt), its wheel hook takes the
// mouse wheel (true: consumed). While driven the mouse and keys don't fly it.
void SetDriver(void (*step)(), bool (*wheel)(long));
void SetDriven(bool on);
}  // namespace rt::freecam
