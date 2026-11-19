// The commander's on-screen markers (commander_hud.cpp): a team's supply zones
// (their ring on the ground, a panel with the seconds left and the way to each)
// and the team's order (a marker at its flag, its label with the distance, the
// way and the seconds left). Only the commander's own team sees them.
//
// The host decides everything (commander.cpp's HudSync, once a second, sends
// each team's state to that team's players as "BF43Z!" packets); a joiner's game
// keeps the last state it was sent for its own team. The host's game keeps the
// same state for its own team directly.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace rt::commander::hud {
struct Zone {
  int kind = 0;  // 1 heal, 2 ammo
  float x = 0, z = 0, left = 0;
  float y = 0;
  bool has_y = false;  // the ground there is known (else the eye's height is used)
};
struct Order {
  int kind = 0;  // 1 attack, 2 defend (0 none)
  float x = 0, z = 0, left = 0;
  float y = 0;
  bool has_y = false;
  int flag = 0;      // the flag's number on the level
  std::string name;  // its name in the level ("The Hill"), "" if not known
};
// An airstrike called and not yet dropped: its warning (seconds to the blast).
struct Strike {
  float x = 0, z = 0, left = 0;
  float y = 0;
  bool has_y = false;
};
struct Snapshot {
  std::vector<Zone> zones;
  Order order;
  Strike strike;  // left 0: none
};

// Keeps a team's state (from the host's sync, or a packet's body).
void Publish(int team, const Snapshot& s);
// The packet body for a state ("" none is "|").
std::string Encode(const Snapshot& s);
// A joiner: the host's packet body for its own team.
void OnPacket(const std::string& body);

// One quad of the overlay, in window pixels (x, y clockwise from the top-left).
struct Quad {
  float x[4] = {}, y[4] = {};
  uint32_t color = 0xFFFFFFFF;  // ABGR
  int tex = -1;                 // -1: a plain colour
};
// A text texture: rgba empty removes it.
struct Texture {
  int id = 0;
  uint32_t w = 0, h = 0;
  std::vector<uint8_t> rgba;
};
// The UI's frame (window w x h pixels): the quads to draw and the textures that
// changed.
void Frame(float w, float h, std::vector<Quad>& quads, std::vector<Texture>& textures);
}  // namespace rt::commander::hud
