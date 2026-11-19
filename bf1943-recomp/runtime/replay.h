// MATCH REPLAYS: the host (--host or --dedicated) records each match it runs
// to <user>/replays/<map>_<date>.bfrp (replay.cpp), and --replay <file>
// plays one back in the level with the free camera (replay_view.cpp).
//
// The file (little-endian): a header, then records, each a type byte and
// its fields:
//   header  "BFRP", u32 version (1), u32 tick (ms), i64 start (unix seconds),
//           char map[32]
//   1 player   u16 id, u8 team (1 US, 2 Japan), u8 flags (1 a bot), char name[16]
//   2 frame    u32 time (ms since the start), u16 soldiers, then each:
//                u16 player, u8 flags (1 in a vehicle), u8 health (0-255 of 100),
//                f32 x, y, z (the feet), f32 yaw, pitch (radians; forward =
//                (sin yaw, cos yaw) in x/z, turning right lowers yaw);
//              u16 vehicles, then each: u16 vehicle, f32 x, y, z, f32 yaw, pitch
//   3 vehicle  u16 id, char kind[24] ("LCVP", "Corsair", "Willys", ...)
//   4 kill     u32 time, u16 victim, u16 killer (0xFFFF not known), f32 x, y, z
//   5 point    u32 time, u16 point, u8 owner team, f32 x, y, z (a flag changed hands)
//   6 end      u32 time
// A soldier missing from a frame is dead (or not spawned).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace rt::replay {
// Recording (host only): starts its thread; called once the host is known.
void StartRecorder();
// A soldier was killed (bot_players.cpp's damage hook): server soldier objects.
void Killed(uint32_t victim_soldier, uint32_t killer_soldier);

// A loaded replay.
struct SoldierState {
  uint16_t player;
  uint8_t flags, health;
  float pos[3], yaw, pitch;
};
struct VehicleState {
  uint16_t id;
  float pos[3], yaw, pitch;
};
struct Frame {
  uint32_t t;
  std::vector<SoldierState> soldiers;
  std::vector<VehicleState> vehicles;
};
struct Player {
  int team = 0;
  bool bot = false;
  std::string name;
};
struct Event {
  int type;  // 4 kill, 5 point
  uint32_t t;
  uint16_t a, b;  // kill: victim, killer; point: id, owner
  float pos[3];
};
struct Recording {
  std::string map;
  int64_t start = 0;
  uint32_t tick = 100;
  std::vector<Player> players;      // by id
  std::vector<std::string> vehicles;  // kinds by id
  std::vector<Frame> frames;
  std::vector<Event> events;
  uint32_t length = 0;  // ms
};
bool Load(const std::string& path, Recording& out, std::string* error = nullptr);

// The live map (commander.cpp serves it): the recorder's latest sample, the
// same ten-times-a-second reads the file is written from. Nothing is copied
// unless someone asks (LiveCopy); false when no match is being recorded or
// the last sample is older than max_age_ms.
struct LiveSoldier {
  uint16_t id;
  uint8_t team, health, seated;
  float x, y, z, yaw;  // (y: the feet)
};
struct LiveVehicle {
  uint16_t id;
  float x, z, yaw;
};
struct LivePoint {
  uint16_t id;
  uint8_t owner;  // team (0 none)
  float x, z;
};
struct LiveSnapshot {
  std::string map;
  uint32_t t = 0;  // ms since the match started
  std::vector<LiveSoldier> soldiers;
  std::vector<LiveVehicle> vehicles;
  std::vector<LivePoint> points;
};
bool LiveCopy(LiveSnapshot& out, uint32_t max_age_ms = 3000);

// --replay <file>: play it back (replay_view.cpp).
extern std::string g_replay_file;
void StartViewer();  // after the command line: host its map with no bots, the free camera on
}  // namespace rt::replay
