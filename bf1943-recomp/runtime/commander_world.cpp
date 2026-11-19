// The commander's drops on the host's server (commander.h): done in the
// server's update (bot_players.cpp's hook, the server's own thread), between
// the server's own steps, from what the page queued.
//
// Battlefield 1943 has no ammo crates in its levels (its soldiers' ammunition
// doesn't run out and health comes back by itself; the engine's
// ServerAmmoCrateEntity class is there, unused). So:
//   - a SUPPLY drop heals the team's soldiers within kSupplyRadius of the point,
//     at once and again every half second for kSupplySeconds;
//   - a VEHICLE drop takes the team's free jeep (US Willys, Japan Kurogane;
//     else any free jeep), the one nearest the point with no soldier near it,
//     and puts it kDropHeight above the ground there: its Havok rigid body
//     (hkRigidBody 820B07CC, the transform's translation +E0 and the swept
//     transform's centre of mass +F0/+100 moved by the same step), so it falls
//     onto the point and the server sends its new place to every player.
// Server objects as in bots.cpp: ServerSoldierEntity 82013A40 (team +48,
// position +180, health +3C), ServerVehicleEntity 82013068 (kind *(*(+14)+C),
// position *(+F0)+F0, or *(+EC)+310 while it stands where it was made).
#include "commander.h"

#include "bot_minimap.h"
#include "bot_players.h"
#include "guest.h"
#include "kernel.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace rt {
extern std::mutex g_host_map_mutex;  // (g_host_map)
}

namespace rt::commander {
namespace {
using Clock = std::chrono::steady_clock;
constexpr uint32_t kSoldier = 0x82013A40;     // ServerSoldierEntity
constexpr uint32_t kVehicle = 0x82013068;     // ServerVehicleEntity
constexpr uint32_t kRigidBody = 0x820B07CC;   // hkRigidBody (Havok 4.6)
constexpr uint32_t kWorld = 0x820B0748;       // hkWorld
constexpr float kSupplyRadius = 15.0f;
constexpr double kSupplySeconds = 30.0;
constexpr float kFreeRadius = 6.0f;           // a jeep with a soldier this near is not free

double Since(Clock::time_point t, Clock::time_point now) { return std::chrono::duration<double>(now - t).count(); }
bool Readable(uint32_t a) { return a >= 0x40000000 && a < 0xC0000000 && IsCommitted(a) && IsCommitted(a + 0x400); }
float LdF(uint32_t a) {
  uint32_t u = Ld32(a);
  float f;
  memcpy(&f, &u, 4);
  return f;
}
void StF(uint32_t a, float f) {
  uint32_t u;
  memcpy(&u, &f, 4);
  St32(a, u);
}
std::vector<uint32_t> Scan(uint32_t vtable) {
  std::vector<uint32_t> out;
  const uint32_t want = __builtin_bswap32(vtable);
  for (uint64_t page = 0x40000000; page < 0xC0000000; page += 0x1000) {
    if (page >= 0x80000000 && page < 0xA0000000) continue;
    if (!IsCommitted(uint32_t(page))) continue;
    const uint32_t* p = reinterpret_cast<const uint32_t*>(g_base + page);
    for (int i = 0; i < 1024; ++i)
      if (p[i] == want) out.push_back(uint32_t(page) + i * 4);
  }
  return out;
}
float DropHeight() {
  static const float h = [] {
    const char* e = std::getenv("BF_DROP_HEIGHT");  // (testing: metres above the ground)
    const float v = e ? float(atof(e)) : 0.3f;
    return std::isfinite(v) ? std::clamp(v, 0.0f, 60.0f) : 0.3f;
  }();
  return h;
}

struct Drop {
  int team;
  std::string kind;
  float x, z;
  std::function<void(bool, const std::string&)> done;
};
struct Zone {
  int team;
  float x, z;
  Clock::time_point until;
};
std::mutex g_mu;
std::vector<Drop> g_queue;
std::vector<Zone> g_zones;
std::atomic<bool> g_work{false};  // anything queued or a zone working (the server's update looks at this only)
std::vector<uint32_t> g_soldiers;  // (the server's thread only)
Clock::time_point g_soldiers_at, g_next_heal;
int64_t g_level = -1;
// (testing) the dropped jeep's place for a few seconds after, in the log.
struct Watch {
  uint32_t vehicle = 0, body = 0;
  Clock::time_point until, next;
} g_watch;

std::string CurrentMap() {
  std::lock_guard l(g_host_map_mutex);
  return g_host_map;
}

struct Vehicle {
  uint32_t obj = 0;
  std::string kind;
  float pos[3] = {};
};
bool ReadVehicle(uint32_t o, Vehicle& v) {
  if (!Readable(o) || Ld32(o) != kVehicle) return false;
  const uint32_t data = Ld32(o + 0x14), body = Ld32(o + 0xF0);
  if (!Readable(data) || !Readable(body)) return false;
  const uint32_t name = Ld32(data + 0xC);
  if (!Readable(name)) return false;
  v.obj = o;
  v.kind.assign(G<char>(name), strnlen(G<char>(name), 31));
  v.pos[0] = LdF(body + 0xF0), v.pos[1] = LdF(body + 0xF4), v.pos[2] = LdF(body + 0xF8);
  if (v.pos[0] == 0 && v.pos[1] == 0 && v.pos[2] == 0) {  // (where it was made, until it moves)
    const uint32_t made = Ld32(o + 0xEC);
    if (Readable(made)) v.pos[0] = LdF(made + 0x310), v.pos[1] = LdF(made + 0x314), v.pos[2] = LdF(made + 0x318);
  }
  return std::isfinite(v.pos[0]) && std::fabs(v.pos[0]) < 1e5f && (v.pos[0] != 0 || v.pos[2] != 0);
}
bool IsJeep(const std::string& kind) {
  std::string k = kind;
  std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return char(tolower(c)); });
  return k.find("willys") != std::string::npos || k.find("kurogane") != std::string::npos || k.find("jeep") != std::string::npos;
}

// The vehicle drop: true and what was done, or false and why not.
bool DropVehicle(int team, float x, float z, std::string* text) {
  float ground = TerrainHeight(x, z);
  // A deck, a pier, a building: the map's collision above the terrain there.
  std::vector<float> top;
  const bool structure = CollisionTops(x - 0.5f, z + 0.5f, 1.0f, 1, top) && !top.empty() && std::isfinite(top[0]) &&
                         top[0] > ground;
  if (structure) ground = top[0];
  if (ground < -1e8f) {
    SightForMap(CurrentMap());  // (read in the background: ready in a minute or so)
    *text = "the server is still reading the map's ground: try again in a minute";
    return false;
  }
  // Water: blue on the level's picture (the maps' ground heights don't tell:
  // their sea is high up).
  if (auto img = structure ? nullptr : botllm::minimap::Get(CurrentMap(), false)) {
    float rgb[3];
    if (img->At(x, z, rgb)) {
      RT_INFO("commander: the picture at the drop: %.0f %.0f %.0f", rgb[0], rgb[1], rgb[2]);
      if (rgb[2] > rgb[0] + 25 && rgb[2] > rgb[1] - 5) {
        *text = "no vehicle drops in the water";
        return false;
      }
    }
  }
  std::vector<std::array<float, 3>> soldiers;
  for (uint32_t s : Scan(kSoldier))
    if (Readable(s) && Ld32(s) == kSoldier) soldiers.push_back({LdF(s + 0x180), LdF(s + 0x184), LdF(s + 0x188)});
  const char* own = team == kUS ? "willys" : "kurogane";
  Vehicle best;
  float best_score = 1e30f;
  int jeeps = 0;
  for (uint32_t o : Scan(kVehicle)) {
    Vehicle v;
    if (!ReadVehicle(o, v) || !IsJeep(v.kind)) continue;
    ++jeeps;
    bool taken = false;
    for (const auto& s : soldiers)
      taken |= std::hypot(std::hypot(s[0] - v.pos[0], s[2] - v.pos[2]), s[1] - v.pos[1]) < kFreeRadius;
    if (taken) continue;
    std::string k = v.kind;
    std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return char(tolower(c)); });
    const float score = std::hypot(v.pos[0] - x, v.pos[2] - z) + (k.find(own) == std::string::npos ? 5000.0f : 0.0f);
    if (score < best_score) best_score = score, best = v;
  }
  if (!best.obj) {
    *text = jeeps ? "every jeep is taken (someone is in or near each)" : "this map has no jeeps to drop";
    return false;
  }
  // Its rigid body: the one in the physics world whose transform is there.
  uint32_t body = 0;
  float near = 2.5f;
  for (uint32_t b : Scan(kRigidBody)) {
    if (!Readable(b) || Ld32(b) != kRigidBody) continue;
    const uint32_t world = Ld32(b + 8);
    if (!Readable(world) || Ld32(world) != kWorld) continue;
    const float d = std::hypot(std::hypot(LdF(b + 0xE0) - best.pos[0], LdF(b + 0xE8) - best.pos[2]), LdF(b + 0xE4) - best.pos[1]);
    if (d < near) near = d, body = b;
  }
  if (!body) {
    *text = "the jeep's physics wasn't found (nothing dropped)";
    RT_WARN("commander: no rigid body at the %s at %.1f,%.1f,%.1f", best.kind.c_str(), best.pos[0], best.pos[1], best.pos[2]);
    return false;
  }
  // (testing) BF_DROP_PROBE: this body's words and what they point at, beside
  // a moving vehicle's body (asleep against awake: Havok's activation).
  if (std::getenv("BF_DROP_PROBE")) {
    auto dump = [](const char* what, uint32_t b) {
      std::string w;
      char buf[32];
      for (uint32_t off = 0; off < 0x2A0; off += 4) snprintf(buf, sizeof buf, " %X:%08X", off, Ld32(b + off)), w += buf;
      RT_INFO("drop probe %s body %08X:%s", what, b, w.c_str());
      for (uint32_t off = 0x14; off < 0xB0; off += 4) {
        const uint32_t p = Ld32(b + off);
        if (!Readable(p) || (p & 3)) continue;
        std::string q;
        for (uint32_t k = 0; k < 0x80; k += 4) snprintf(buf, sizeof buf, " %08X", Ld32(p + k)), q += buf;
        RT_INFO("drop probe %s   +%X -> %08X:%s", what, off, p, q.c_str());
      }
    };
    dump("parked", body);
    for (uint32_t o : Scan(kVehicle)) {
      const uint32_t vb = Readable(o) ? Ld32(o + 0xF0) : 0;
      if (!Readable(vb) || (LdF(vb + 0xF0) == 0 && LdF(vb + 0xF8) == 0)) continue;
      const float p[3] = {LdF(vb + 0xF0), LdF(vb + 0xF4), LdF(vb + 0xF8)};
      for (uint32_t b : Scan(kRigidBody))
        if (Readable(b) && b != body &&
            std::hypot(std::hypot(LdF(b + 0xE0) - p[0], LdF(b + 0xE8) - p[2]), LdF(b + 0xE4) - p[1]) < 1.0f) {
          RT_INFO("drop probe: moving vehicle %08X at %.1f,%.1f,%.1f", o, p[0], p[1], p[2]);
          dump("moving", b);
          goto probed;
        }
    }
  probed:;
  }
  // Its height over the ground where it stands now, kept (a parked jeep's
  // physics is fixed until someone gets in: it would not fall).
  float lift = DropHeight();
  if (const float g0 = TerrainHeight(best.pos[0], best.pos[2]); g0 > -1e8f && best.pos[1] - g0 > -1 && best.pos[1] - g0 < 4)
    lift = best.pos[1] - g0 + DropHeight();
  const float from[3] = {best.pos[0], best.pos[1], best.pos[2]};
  const float to[3] = {x, ground + lift, z};
  const float step[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
  // Every copy of where it stands (its entity's, its physics' in the server's
  // world and in this game's own client's: 16-byte aligned vectors there
  // within 5 cm), by the same step.
  size_t moved = 0;
  for (uint64_t page = 0x40000000; page < 0xC0000000; page += 0x1000) {
    if (page >= 0x80000000 && page < 0xA0000000) continue;
    if (!IsCommitted(uint32_t(page))) continue;
    for (uint32_t a = uint32_t(page); a < uint32_t(page) + 0x1000; a += 16) {
      const float p[3] = {LdF(a), LdF(a + 4), LdF(a + 8)};
      // (written as a test that NaN words fail)
      if (!(std::fabs(p[0] - from[0]) < 0.05f && std::fabs(p[2] - from[2]) < 0.05f && std::fabs(p[1] - from[1]) < 0.05f))
        continue;
      for (int i = 0; i < 3; ++i) StF(a + 4 * i, p[i] + step[i]);
      static int said = 0;
      if (said++ < 40) RT_INFO("commander: vehicle drop: moved the vector at %08X", a);
      ++moved;
    }
  }
  // The rigid bodies' centres of mass (a little off their origin) too.
  for (uint32_t b : Scan(kRigidBody)) {
    if (!Readable(b) || Ld32(b) != kRigidBody) continue;
    if (std::hypot(std::hypot(LdF(b + 0xE0) - to[0], LdF(b + 0xE8) - to[2]), LdF(b + 0xE4) - to[1]) > 0.1f) continue;
    for (uint32_t at : {0xF0u, 0x100u}) {
      const float p[3] = {LdF(b + at), LdF(b + at + 4), LdF(b + at + 8)};
      if (std::hypot(std::hypot(p[0] - from[0], p[2] - from[2]), p[1] - from[1]) > 4.0f) continue;
      for (int i = 0; i < 3; ++i) StF(b + at + 4 * i, p[i] + step[i]);
    }
  }
  RT_INFO("commander: %s team's vehicle drop: the %s (vehicle %08X, body %08X) from %.1f,%.1f,%.1f to %.1f,%.1f,%.1f (ground %.1f), %zu copies moved",
          TeamName(team), best.kind.c_str(), best.obj, body, from[0], from[1], from[2], to[0], to[1], to[2], ground, moved);
  g_watch.vehicle = best.obj, g_watch.body = body, g_watch.until = Clock::now() + std::chrono::seconds(8);
  g_watch.next = Clock::now();
  *text = "a " + best.kind + " was dropped";
  return true;
}

// Heals the zones' soldiers (every half second; the soldiers found again every 2 s).
void Heal(const std::vector<Zone>& zones, Clock::time_point now) {
  if (Since(g_soldiers_at, now) > 2.0) g_soldiers = Scan(kSoldier), g_soldiers_at = now;
  for (uint32_t s : g_soldiers) {
    if (!Readable(s) || Ld32(s) != kSoldier) continue;
    const int team = int(Ld32(s + 0x48));
    const float hp = LdF(s + 0x3C), x = LdF(s + 0x180), z = LdF(s + 0x188);
    if (!(hp > 0) || hp >= 100) continue;  // (dead or whole)
    for (const Zone& zn : zones)
      if (zn.team == team && std::hypot(x - zn.x, z - zn.z) < kSupplyRadius) {
        StF(s + 0x3C, 100.0f);
        static int said = 0;
        if (said++ < 20) RT_INFO("commander: supply drop healed a %s soldier (%.0f -> 100)", TeamName(team), hp);
        break;
      }
  }
}
}  // namespace

void QueueDrop(int team, const std::string& kind, float x, float z, std::function<void(bool, const std::string&)> done) {
  std::lock_guard l(g_mu);
  if (g_queue.size() >= 8) {  // (the server's update takes them at once: never this many)
    done(false, "too many drops waiting");
    return;
  }
  g_queue.push_back({team, kind, x, z, std::move(done)});
  g_work = true;
}

std::vector<SupplyZone> SupplyZones() {
  std::lock_guard l(g_mu);
  std::vector<SupplyZone> out;
  const auto now = Clock::now();
  for (const Zone& z : g_zones)
    if (z.until > now) out.push_back({z.team, z.x, z.z, float(std::chrono::duration<double>(z.until - now).count())});
  return out;
}

void OnServerUpdate() {
  // A new level: no zones from the last one.
  if (const int64_t level = g_level_open_ms.load(); level != g_level) {
    g_level = level;
    std::lock_guard l(g_mu);
    g_zones.clear();
    g_watch = Watch{};
  }
  if (g_watch.vehicle) {
    const auto now = Clock::now();
    if (now >= g_watch.until) {
      g_watch = Watch{};
    } else if (now >= g_watch.next) {
      g_watch.next = now + std::chrono::milliseconds(500);
      Vehicle v;
      const bool ok = ReadVehicle(g_watch.vehicle, v);
      RT_INFO("commander: the dropped jeep now: body %.1f,%.1f,%.1f; the vehicle reads %.1f,%.1f,%.1f%s", LdF(g_watch.body + 0xE0),
              LdF(g_watch.body + 0xE4), LdF(g_watch.body + 0xE8), v.pos[0], v.pos[1], v.pos[2], ok ? "" : " (gone)");
    }
  }
  // A seat taken: the map's ground is read (the vehicle drops' height; the bots
  // read it anyway), once a level, in the background.
  static Clock::time_point next_look;
  static int64_t ground_level = -1;
  if (ground_level != g_level && Clock::now() >= next_look) {
    next_look = Clock::now() + std::chrono::seconds(2);
    if (Seated(kUS) || Seated(kJapan)) {
      ground_level = g_level;
      if (const std::string map = CurrentMap(); !map.empty()) SightForMap(map);
    }
  }
  if (!g_work.load(std::memory_order_relaxed)) return;
  std::vector<Drop> queue;
  std::vector<Zone> zones;
  const auto now = Clock::now();
  {
    std::lock_guard l(g_mu);
    queue.swap(g_queue);
    g_zones.erase(std::remove_if(g_zones.begin(), g_zones.end(), [&](const Zone& z) { return z.until <= now; }), g_zones.end());
    for (const Drop& d : queue)
      if (d.kind == "ammo") g_zones.push_back({d.team, d.x, d.z, now + std::chrono::milliseconds(int(kSupplySeconds * 1000))});
    zones = g_zones;
    g_work = !g_zones.empty();
  }
  for (Drop& d : queue) {
    std::string text;
    bool ok = true;
    if (d.kind == "vehicle") ok = DropVehicle(d.team, d.x, d.z, &text);
    else text = "supply drop: the team is healed there for 30 s";
    if (d.done) d.done(ok, text);
  }
  if (!zones.empty() && (now >= g_next_heal || !queue.empty())) {
    g_next_heal = now + std::chrono::milliseconds(500);
    Heal(zones, now);
  }
}
}  // namespace rt::commander
