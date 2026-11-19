// The commander's drops on the host's server (commander.h): done in the
// server's update (bot_players.cpp's hook, the server's own thread), between
// the server's own steps, from what the page queued.
//
// Battlefield 1943 has no ammo crates in its levels (its soldiers' ammunition
// doesn't run out and health comes back by itself; the engine's
// ServerAmmoCrateEntity class is there, unused). So:
//   - a HEAL zone heals the team's soldiers within kSupplyRadius of the point,
//     at once and again every half second for kSupplySeconds (an AMMO zone is
//     refused: the soldiers' rounds are not found, docs/COMMANDER.md);
//   - an AIRSTRIKE hurts every soldier within kBlastRadius kStrikeWarnS after
//     it is called (its warning on the calling team's HUD), with the scene
//     effects' smoke and debris in every game;
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
#include "xenia_bridge.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
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
  int kind = 1;  // 1 heal, 2 ammo
  float y = 0;
  bool has_y = false;
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

// The ground at a point for the players' rings. The best: a soldier on foot
// within 15 m (its feet are on the surface walked on). Else the terrain, or the
// map's collision just above it (a deck, an apron: up to 2.5 m; higher is a
// roof). Guadalcanal's airfield shows why: its terrain is 2 m over the apron at
// one place and 8 m under it 45 m on, and the collision there finds the sea
// floor or a roof. False if nothing is known.
bool GroundAt(float x, float z, float* y) {
  float g = TerrainHeight(x, z);
  std::vector<float> top;
  const bool deck = CollisionTops(x - 0.5f, z + 0.5f, 1.0f, 1, top) && !top.empty() && std::isfinite(top[0]) &&
                    top[0] > g && top[0] <= g + 2.5f;
  if (deck) g = top[0];
  float feet = 0, best = 15.0f;
  for (uint32_t s : Scan(kSoldier))
    if (Readable(s) && Ld32(s) == kSoldier && LdF(s + 0x3C) > 0) {
      const float d = std::hypot(LdF(s + 0x180) - x, LdF(s + 0x188) - z);
      if (d < best) best = d, feet = LdF(s + 0x184);
    }
  const bool by_feet = best < 15.0f && std::isfinite(feet);
  RT_INFO("commander: the ground at %.0f,%.0f: %s %.1f (terrain %.1f, collision top %.1f)", x, z,
          by_feet ? "a soldier's feet" : deck ? "the deck" : "the terrain", by_feet ? feet : g, TerrainHeight(x, z),
          top.empty() ? -1.0f : top[0]);
  if (by_feet) {
    *y = feet;
    return true;
  }
  if (!(g > -1e8f) || !std::isfinite(g)) return false;
  *y = g;
  return true;
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
  // Only the vehicle's own words: its entity's place (+F0) and its body's
  // transform (+E0) and centres of mass (+F0, +100), each written only when
  // the object is the one it should be (the vehicle's kind, the rigid body's
  // vtable) and the body is the one found above. No memory-wide scan.
  size_t moved = 0;
  auto moveVec = [&](uint32_t at, const float s[3]) {
    if (!Readable(at)) return;
    for (int i = 0; i < 3; ++i) StF(at + 4 * i, s[i] + step[i]);
    ++moved;
  };
  if (Readable(body) && Ld32(body) == kRigidBody) {
    const float o[3] = {LdF(body + 0xE0), LdF(body + 0xE4), LdF(body + 0xE8)};
    if (std::hypot(std::hypot(o[0] - from[0], o[2] - from[2]), o[1] - from[1]) < 0.5f) moveVec(body + 0xE0, o);
    for (uint32_t at : {0xF0u, 0x100u}) {
      const float p[3] = {LdF(body + at), LdF(body + at + 4), LdF(body + at + 8)};
      if (std::hypot(std::hypot(p[0] - from[0], p[2] - from[2]), p[1] - from[1]) <= 4.0f) moveVec(body + at, p);
    }
  }
  static int said = 0;
  if (said++ < 40) RT_INFO("commander: vehicle drop: %zu words of the body %08X moved", moved, body);  RT_INFO("commander: %s team's vehicle drop: the %s (vehicle %08X, body %08X) from %.1f,%.1f,%.1f to %.1f,%.1f,%.1f (ground %.1f), %zu copies moved",
          TeamName(team), best.kind.c_str(), best.obj, body, from[0], from[1], from[2], to[0], to[1], to[2], ground, moved);
  g_watch.vehicle = best.obj, g_watch.body = body, g_watch.until = Clock::now() + std::chrono::seconds(8);
  g_watch.next = Clock::now();
  *text = "a " + best.kind + " was dropped";
  return true;
}

// The airstrike: the blast at its point after kStrikeWarnS, on every soldier
// within kBlastRadius (the centre killed, the edge hurt least). The soldiers'
// health (+3C) is taken down directly: the explosion's own effect is a guest
// object the server does not make (docs/COMMANDER.md), so there is no flash.
constexpr double kStrikeWarnS = 8.0;
constexpr float kBlastRadius = 12.0f;
struct Strike {
  int team;
  float x, z;
  float y;
  bool has_y;
  Clock::time_point fire;
};
std::vector<Strike> g_strikes;  // (under g_mu)

void Blast(const Strike& s) {
  int hit = 0, killed = 0;
  for (uint32_t a : Scan(kSoldier)) {
    if (!Readable(a) || Ld32(a) != kSoldier) continue;
    const float hp = LdF(a + 0x3C), x = LdF(a + 0x180), z = LdF(a + 0x188);
    if (!(hp > 0)) continue;
    const float d = std::hypot(x - s.x, z - s.z);
    if (d >= kBlastRadius) continue;
    // Full at 4 m or less, a fifth of it at the edge (linear between).
    const float dmg = d <= 4.0f ? 1000.0f : 100.0f * (1.0f - 0.8f * (d - 4.0f) / (kBlastRadius - 4.0f));
    const float left = hp - dmg;
    StF(a + 0x3C, left > 0 ? left : 0.0f);
    ++hit;
    if (left <= 0) ++killed;
  }
  RT_INFO("commander: %s team's airstrike at %.1f,%.1f: %d soldiers hit, %d killed", TeamName(s.team), s.x, s.z, hit, killed);
  // Its smoke and debris (the scene effects' explosion, as the game's own make).
  float y = s.y;
  if (!s.has_y) {
    float eye[3];
    y = xgpu::CameraEye(eye) ? eye[1] - 1.7f : 0.0f;
  }
  OnBlast(s.x, y, s.z);
}

// Heals the zones' soldiers (every half second; the soldiers found again every 2 s).
void Heal(const std::vector<Zone>& zones, Clock::time_point now) {
  // The soldiers' list is scanned every 10 s (not every 2): a soldier that dies or
  // is made in between is only missed for a moment, and the heal checks each
  // soldier's live health and place anyway (Readable and vtable checks below).
  if (Since(g_soldiers_at, now) > 10.0) g_soldiers = Scan(kSoldier), g_soldiers_at = now;
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

std::vector<StrikeWarning> PendingStrikes() {
  std::lock_guard l(g_mu);
  std::vector<StrikeWarning> out;
  const auto now = Clock::now();
  for (const Strike& s : g_strikes)
    if (s.fire > now) out.push_back({s.team, s.x, s.z, float(std::chrono::duration<double>(s.fire - now).count()), s.y, s.has_y});
  return out;
}

std::vector<SupplyZone> SupplyZones() {
  std::lock_guard l(g_mu);
  std::vector<SupplyZone> out;
  const auto now = Clock::now();
  for (const Zone& z : g_zones)
    if (z.until > now) {
      SupplyZone s;
      s.team = z.team, s.x = z.x, s.z = z.z, s.left = float(std::chrono::duration<double>(z.until - now).count());
      s.kind = z.kind, s.y = z.y, s.has_y = z.has_y;
      out.push_back(s);
    }
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
  // (testing) BF_AMMO_PROBE: where a soldier's rounds are. Every 3 s, the small
  // whole numbers reachable from each soldier (its words, and one and two
  // pointers further); a path whose value drops by 1 to 5 between looks (a shot,
  // a burst) is counted, and the most counted paths are logged every 15 s.
  if (std::getenv("BF_AMMO_PROBE")) {
    static std::map<uint64_t, std::pair<uint32_t, uint32_t>> last;  // (soldier, path) -> value, look number
    static std::map<uint64_t, std::pair<int, int>> drops;  // path -> drops, rises to a bigger value (a reload)
    static Clock::time_point next, report;
    static uint32_t look = 0;
    const auto now = Clock::now();
    if (now >= next) {
      next = now + std::chrono::seconds(3), ++look;
      for (uint32_t s : Scan(kSoldier)) {
        if (!Readable(s) || Ld32(s) != kSoldier) continue;
        auto see = [&](uint64_t path, uint32_t v) {
          if (v > 400) return;
          const uint64_t key = (uint64_t(s) << 32) ^ (path * 0x9E3779B97F4A7C15ull);
          auto it = last.find(key);
          if (it != last.end() && it->second.second + 1 == look) {
            const uint32_t was = it->second.first;
            if (v < was && was - v <= 5) ++drops[path].first;
            else if (v > was + 3 && v <= 200) ++drops[path].second;
          }
          last[key] = {v, look};
        };
        for (uint32_t a = 0; a < 0x400; a += 4) {
          const uint32_t w = Ld32(s + a);
          see(0x1000000ull | a, w);
          if (!Readable(w) || (w & 3)) continue;
          for (uint32_t b = 0; b < 0x200; b += 4) {
            const uint32_t w2 = Ld32(w + b);
            see((uint64_t(a) << 24) | (uint64_t(b) << 12) | 0xFFF, w2);
            if (!Readable(w2) || (w2 & 3)) continue;
            for (uint32_t c = 0; c < 0x100; c += 4) see((uint64_t(a) << 24) | (uint64_t(b) << 12) | c, Ld32(w2 + c));
          }
        }
      }
      if (last.size() > 4000000) last.clear();
    }
    if (now >= report) {
      report = now + std::chrono::seconds(15);
      std::vector<std::pair<int, uint64_t>> top;
      for (const auto& [path, n] : drops)
        if (n.first >= 3) top.push_back({n.first * 4 + n.second, path});
      std::sort(top.rbegin(), top.rend());
      std::string out;
      char b[96];
      for (size_t i = 0; i < top.size() && i < 20; ++i) {
        const uint64_t p = top[i].second;
        const auto& n = drops[p];
        if (p & 0x1000000ull) snprintf(b, sizeof b, " [+%X] %d/%d", unsigned(p & 0xFFFFFF), n.first, n.second);
        else if ((p & 0xFFF) == 0xFFF) snprintf(b, sizeof b, " [+%X]+%X %d/%d", unsigned(p >> 24), unsigned((p >> 12) & 0xFFF), n.first, n.second);
        else snprintf(b, sizeof b, " [[+%X]+%X]+%X %d/%d", unsigned(p >> 24), unsigned((p >> 12) & 0xFFF), unsigned(p & 0xFFF), n.first, n.second);
        out += b;
      }
      RT_INFO("ammo probe (path drops/reloads):%s", out.c_str());
    }
  }
  // (testing) BF_VEHICLE_PROBE: each jeep's maker (+EC: is it the level's
  // ServerVehicleSpawnEntity, vtable 82018898?) and where it makes it (+310).
  if (std::getenv("BF_VEHICLE_PROBE")) {
    static int64_t probed = -1;
    static Clock::time_point after = Clock::now() + std::chrono::seconds(25);
    if (probed != g_level && Clock::now() >= after) {
      probed = g_level;
      for (uint32_t o : Scan(kVehicle)) {
        Vehicle v;
        if (!ReadVehicle(o, v) || !IsJeep(v.kind)) continue;
        const uint32_t made = Ld32(o + 0xEC);
        const uint32_t vt = Readable(made) ? Ld32(made) : 0;
        RT_INFO("vehicle probe: %s %08X at %.1f,%.1f,%.1f; +EC %08X (vtable %08X%s), its +310 %.1f,%.1f,%.1f", v.kind.c_str(), o,
                v.pos[0], v.pos[1], v.pos[2], made, vt, vt == 0x82018898u ? ": the vehicle spawner" : "",
                Readable(made) ? LdF(made + 0x310) : 0.0f, Readable(made) ? LdF(made + 0x314) : 0.0f, Readable(made) ? LdF(made + 0x318) : 0.0f);
      }
      for (uint32_t sp : Scan(0x82018898))
        if (Readable(sp) && Ld32(sp) == 0x82018898) RT_INFO("vehicle probe: a spawner at %08X", sp);
    }
  }
  // (testing) BF_VEHICLE_WATCH: every jeep (up to 12), every 500 ms: its place
  // when it moves, each change in its spawner's words (+EC -> 0..0x340), in its
  // own words (0..0x200; an offset that changes more than 8 times is dropped as
  // noise) and in its Havok body's words that are Havok vtables (820Bxxxx: the
  // motion class), to see what getting in, driving off and leaving does.
  if (std::getenv("BF_VEHICLE_WATCH")) {
    struct Jeep {
      std::vector<uint32_t> own = std::vector<uint32_t>(0x80), sp = std::vector<uint32_t>(0xD0), bd = std::vector<uint32_t>(0xC0);
      std::map<uint32_t, int> noise;
      float pos[3] = {};
      uint32_t body = 0;
    };
    static std::map<uint32_t, Jeep> jeeps;
    static Clock::time_point next;
    if (Clock::now() >= next) {
      next = Clock::now() + std::chrono::milliseconds(500);
      const long long ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() % 86400000;
      for (uint32_t o : Scan(kVehicle)) {
        Vehicle v;
        if (!ReadVehicle(o, v) || !IsJeep(v.kind)) continue;
        if (!jeeps.count(o) && jeeps.size() >= 12) continue;
        const bool fresh = !jeeps.count(o);
        Jeep& j = jeeps[o];
        const uint32_t s = Ld32(o + 0xEC);
        if (fresh) {
          for (uint32_t i = 0; i < 0x80; ++i) j.own[i] = Ld32(o + i * 4);
          for (uint32_t i = 0; i < 0xD0; ++i) j.sp[i] = Readable(s) ? Ld32(s + i * 4) : 0;
          memcpy(j.pos, v.pos, sizeof j.pos);
          std::string w;
          char t[24];
          for (uint32_t i = 0; i < 0xD0; ++i) snprintf(t, sizeof t, " %X:%X", i * 4, j.sp[i]), w += t;
          RT_INFO("vehicle watch %lld: %s %08X at %.1f,%.1f,%.1f; spawner %08X:%s", ms, v.kind.c_str(), o, v.pos[0], v.pos[1], v.pos[2], s, w.c_str());
        }
        if (std::hypot(v.pos[0] - j.pos[0], v.pos[2] - j.pos[2]) > 1.0f || std::fabs(v.pos[1] - j.pos[1]) > 1.0f) {
          RT_INFO("vehicle watch %lld: %08X moved to %.1f,%.1f,%.1f", ms, o, v.pos[0], v.pos[1], v.pos[2]);
          memcpy(j.pos, v.pos, sizeof j.pos);
        }
        for (uint32_t i = 0; i < 0x80; ++i) {
          const uint32_t n = Ld32(o + i * 4);
          if (n == j.own[i]) continue;
          float f, g;
          memcpy(&f, &n, 4), memcpy(&g, &j.own[i], 4);
          if (++j.noise[i * 4] <= 8) RT_INFO("vehicle watch %lld: %08X vehicle +%X: %u -> %u (%g -> %g)", ms, o, i * 4, j.own[i], n, g, f);
          j.own[i] = n;
        }
        if (Readable(s))
          for (uint32_t i = 0; i < 0xD0; ++i) {
            const uint32_t n = Ld32(s + i * 4);
            if (n == j.sp[i]) continue;
            float f, g;
            memcpy(&f, &n, 4), memcpy(&g, &j.sp[i], 4);
            if (++j.noise[0x10000 + i * 4] <= 12) RT_INFO("vehicle watch %lld: %08X spawner +%X: %u -> %u (%g -> %g)", ms, o, i * 4, j.sp[i], n, g, f);
            j.sp[i] = n;
          }
        if (!j.body) {  // its rigid body: the one whose transform is at the jeep
          float near = 2.5f;
          for (uint32_t b : Scan(kRigidBody)) {
            if (!Readable(b) || Ld32(b) != kRigidBody) continue;
            const float d = std::hypot(std::hypot(LdF(b + 0xE0) - v.pos[0], LdF(b + 0xE8) - v.pos[2]), LdF(b + 0xE4) - v.pos[1]);
            if (d < near) near = d, j.body = b;
          }
          if (j.body) {
            std::string w;
            char t[24];
            for (uint32_t i = 0; i < 0xC0; ++i) {
              j.bd[i] = Ld32(j.body + i * 4);
              if (j.bd[i] >= 0x820B0000 && j.bd[i] < 0x820B2000) snprintf(t, sizeof t, " %X:%X", i * 4, j.bd[i]), w += t;
            }
            RT_INFO("vehicle watch %lld: %08X body %08X, its Havok vtables:%s", ms, o, j.body, w.c_str());
          }
        } else {
          for (uint32_t i = 0; i < 0xC0; ++i) {
            const uint32_t n = Ld32(j.body + i * 4);
            if (n != j.bd[i] && ((n >= 0x820B0000 && n < 0x820B2000) || (j.bd[i] >= 0x820B0000 && j.bd[i] < 0x820B2000)))
              RT_INFO("vehicle watch %lld: %08X body +%X: %X -> %X", ms, o, i * 4, j.bd[i], n);
            j.bd[i] = n;
          }
        }
      }
    }
  }  // (testing) BF_CLIP_WATCH: the object the clip probe pointed at: for each
  // ServerWeaponComponent w, o2 = *(*(w+3C)+C); its words +28, +38, +48, +58
  // (counters stepping down in the probe), every 100 ms, each change logged
  // with the time of day (at most 6000 lines); o2's first 0x60 words once.
  if (std::getenv("BF_CLIP_WATCH")) {
    static std::map<uint32_t, std::array<uint32_t, 4>> o2s;
    static Clock::time_point next;
    static int lines = 0;
    if (Clock::now() >= next) {
      next = Clock::now() + std::chrono::milliseconds(100);
      const long long ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() % 86400000;
      for (uint32_t w : Scan(0x82010500)) {
        if (!Readable(w) || Ld32(w) != 0x82010500) continue;
        const uint32_t o1 = Ld32(w + 0x3C);
        const uint32_t o2 = Readable(o1) ? Ld32(o1 + 0xC) : 0;
        if (!Readable(o2)) continue;
        std::array<uint32_t, 4> now_v = {Ld32(o2 + 0x28), Ld32(o2 + 0x38), Ld32(o2 + 0x48), Ld32(o2 + 0x58)};
        auto it = o2s.find(o2);
        if (it == o2s.end()) {
          std::string out;
          char b[24];
          for (uint32_t off = 0; off < 0x60; off += 4) snprintf(b, sizeof b, " %X:%X", off, Ld32(o2 + off)), out += b;
          RT_INFO("clip watch %lld: object %08X (from component %08X):%s", ms, o2, w, out.c_str());
          o2s[o2] = now_v;
          continue;
        }
        if (it->second != now_v && lines < 6000) {
          ++lines;
          RT_INFO("clip watch %lld: %08X +28 %u +38 %u +48 %u +58 %u (was %u %u %u %u)", ms, o2, now_v[0], now_v[1], now_v[2], now_v[3],
                  it->second[0], it->second[1], it->second[2], it->second[3]);
          it->second = now_v;
        }
      }
    }
  }
  // (testing) BF_CLIP_PROBE: where a weapon's rounds are. Every 100 ms, for each
  // ServerWeaponComponent (82010500) the small whole numbers (0..80) one pointer
  // away (its words 0..0x200 that point somewhere, and 0x80 bytes there), and
  // through two pointers (0x60 bytes); a path that steps down by 1 to 4 is
  // counted (a burst takes the clip down shot by shot, a reload takes it back up).
  // Every 10 s the paths with most steps are logged as [comp#.depth a b c]
  // with the component's address.
  if (std::getenv("BF_CLIP_PROBE")) {
    static std::map<uint64_t, uint32_t> last;
    static std::map<uint64_t, std::pair<int, int>> steps;  // path -> down steps, up jumps
    static std::map<uint32_t, uint32_t> index;             // component -> number
    static const uint32_t vt = [] {  // BF_CLIP_VT=<hex vtable>: another class (the client's weapon: 82005360)
      const char* e = std::getenv("BF_CLIP_VT");
      return e ? uint32_t(strtoul(e, nullptr, 16)) : 0x82010500u;
    }();
    static Clock::time_point next, report;
    const auto now = Clock::now();
    if (now >= next) {
      next = now + std::chrono::milliseconds(100);
      for (uint32_t w : Scan(vt)) {
        if (!Readable(w) || Ld32(w) != vt) continue;
        const uint64_t ci = index.emplace(w, uint32_t(index.size())).first->second;
        auto see = [&](uint64_t depth, uint64_t a, uint64_t b, uint64_t c, uint32_t v) {
          if (v > 80) return;
          const uint64_t key = (ci << 52) | (depth << 48) | ((a >> 2) << 32) | ((b >> 2) << 16) | (c >> 2);
          auto it = last.find(key);
          if (it != last.end()) {
            if (v < it->second && it->second - v <= 4) ++steps[key].first;
            else if (v > it->second + 5) ++steps[key].second;
          }
          last[key] = v;
        };
        for (uint32_t a = 0; a < 0x200; a += 4) {
          const uint32_t p = Ld32(w + a);
          see(0, a, 0, 0, p);
          if (!Readable(p) || (p & 3)) continue;
          for (uint32_t b = 0; b < 0x80; b += 4) {
            const uint32_t q = Ld32(p + b);
            see(1, a, b, 0, q);
            if (!Readable(q) || (q & 3)) continue;
            for (uint32_t c = 0; c < 0x60; c += 4) see(2, a, b, c, Ld32(q + c));
          }
        }
      }
      if (last.size() > 3000000) last.clear();
    }
    if (now >= report) {
      report = now + std::chrono::seconds(10);
      std::vector<std::pair<int, uint64_t>> top;
      for (const auto& [key, n] : steps)
        if (n.first >= 6) top.push_back({n.first, key});
      std::sort(top.rbegin(), top.rend());
      std::vector<uint32_t> comps(index.size());
      for (const auto& [addr, i] : index) comps[i] = addr;
      std::string out;
      char b[120];
      for (size_t i = 0; i < top.size() && i < 70; ++i) {
        const uint64_t k = top[i].second;
        const auto& n = steps[k];
        snprintf(b, sizeof b, " [%08X d%u +%X +%X +%X] %d/%d", comps[k >> 52], unsigned((k >> 48) & 3), unsigned(((k >> 32) & 0xFFFF) << 2),
                 unsigned(((k >> 16) & 0xFFFF) << 2), unsigned((k & 0xFFFF) << 2), n.first, n.second);
        out += b;
      }
      RT_INFO("clip probe:%s", out.c_str());
    }
  }
  // (testing) BF_SPAWN_PROBE: the level's ServerVehicleSpawnEntity vtable (82018898):
  // its words, once, and for each method the first words of its code (to name
  // the creation method by what it calls, in the next pass).
  if (std::getenv("BF_SPAWN_PROBE")) {
    static int64_t probed = -1;
    if (probed != g_level) {
      probed = g_level;
      std::string w;
      char b[24];
      for (uint32_t i = 0; i < 48; ++i) {
        const uint32_t f = Ld32(0x82018898 + i * 4);
        snprintf(b, sizeof b, " %X", f);
        w += b;
      }
      RT_INFO("spawn probe: vtable 82018898:%s", w.c_str());
      for (uint32_t i = 0; i < 48; ++i) {
        const uint32_t f = Ld32(0x82018898 + i * 4);
        if (f < 0x82000000 || f >= 0x83000000) continue;
        std::string c;
        for (uint32_t k = 0; k < 12; ++k) snprintf(b, sizeof b, " %08X", Ld32(f + k * 4)), c += b;
        RT_INFO("spawn probe: method %X (slot %u):%s", f, i, c.c_str());
      }
    }
  }
  // (testing) BF_WEAPON_PROBE: every ServerWeaponComponent (82010500, a
  // soldier's weapon): its first 0x200 bytes once, then each word that changes,
  // every 250 ms, with the time of day; and what points at it (the soldier's
  // word, if any). Run with --bots 0: the host's own weapons only.
  if (std::getenv("BF_WEAPON_PROBE")) {
    static std::map<uint32_t, std::vector<uint32_t>> seen;
    static Clock::time_point next;
    if (Clock::now() >= next) {
      next = Clock::now() + std::chrono::milliseconds(250);
      const long long ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() % 86400000;
      std::vector<uint32_t> soldiers = Scan(kSoldier);
      for (uint32_t w : Scan(0x82010500)) {
        if (!Readable(w) || Ld32(w) != 0x82010500) continue;
        auto it = seen.find(w);
        if (it == seen.end()) {
          std::string out;
          char b[24];
          for (uint32_t off = 0; off < 0x200; off += 4) snprintf(b, sizeof b, " %X:%X", off, Ld32(w + off)), out += b;
          std::string owners;
          for (uint32_t s : soldiers)
            for (uint32_t off = 0; off < 0x600; off += 4)
              if (Ld32(s + off) == w) snprintf(b, sizeof b, " soldier %08X+%X", s, off), owners += b;
          RT_INFO("weapon probe %lld: new component %08X%s:%s", ms, w, owners.c_str(), out.c_str());
          std::vector<uint32_t> v(0x80);
          for (uint32_t i = 0; i < 0x80; ++i) v[i] = Ld32(w + i * 4);
          seen[w] = v;
          continue;
        }
        for (uint32_t i = 0; i < 0x80; ++i) {
          const uint32_t now_w = Ld32(w + i * 4);
          if (now_w == it->second[i]) continue;
          float f, g;
          memcpy(&f, &now_w, 4), memcpy(&g, &it->second[i], 4);
          RT_INFO("weapon probe %lld ms %08X +%X: %u -> %u (%g -> %g)", ms, w, i * 4, it->second[i], now_w, g, f);
          it->second[i] = now_w;
        }
      }
    }
  }
  // (testing) BF_CHANGE_PROBE: what changes, rarely, in this game's own soldier
  // (the one under the camera: 'S') and in the jeep nearest it ('V'), its words
  // and one and two pointers further, every 250 ms, with the time since the
  // time of day (UTC): scripted presses (firing, reloading, a grenade, shots at the
  // jeep) then show where the rounds, the grenades and the jeep's health are.
  if (std::getenv("BF_CHANGE_PROBE")) {
    struct Track {
      uint32_t last = 0;
      uint16_t changes = 0, said = 0, early = 0;  // early: its changes in the first 100 looks (the baseline)
    };
    static std::map<uint64_t, Track> tracks[2];
    static uint32_t target[2] = {0, 0}, samples[2] = {0, 0};
    static Clock::time_point next;
    static int lines = 0;
    const auto now = Clock::now();
    float eye[3];
    if (now >= next && xgpu::CameraEye(eye)) {
      next = now + std::chrono::milliseconds(500);
      const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() % 86400000;  // (the time of day, UTC: lined up with the driver's presses)
      uint32_t pick[2] = {0, 0};
      float best[2] = {2.5f, 40.0f};
      for (uint32_t s : Scan(kSoldier))
        if (Readable(s) && Ld32(s) == kSoldier) {
          const float d = std::hypot(LdF(s + 0x180) - eye[0], LdF(s + 0x188) - eye[2]);
          if (d < best[0] && eye[1] - LdF(s + 0x184) > 0 && eye[1] - LdF(s + 0x184) < 2.5f) best[0] = d, pick[0] = s;
        }
      for (uint32_t o : Scan(kVehicle)) {
        Vehicle v;
        if (!ReadVehicle(o, v) || !IsJeep(v.kind)) continue;
        const float d = std::hypot(v.pos[0] - eye[0], v.pos[2] - eye[2]);
        if (d < best[1]) best[1] = d, pick[1] = o;
      }
      for (int t = 0; t < 2; ++t) {
        if (!pick[t]) continue;
        if (pick[t] != target[t]) {
          RT_INFO("change probe %lld ms: %c is now %08X (%.1f m)", ms, t ? 'V' : 'S', pick[t], best[t]);
          target[t] = pick[t], tracks[t].clear(), samples[t] = 0;
        }
        ++samples[t];
        const uint32_t o = pick[t];
        auto see = [&](uint64_t path, uint32_t v) {
          Track& k = tracks[t][path];
          if (samples[t] > 1 && v != k.last) {
            ++k.changes;
            if (samples[t] < 100) ++k.early;
            // Rare changers only (not timers or positions), and not too many lines.
            float fv, fl;
            memcpy(&fv, &v, 4), memcpy(&fl, &k.last, 4);
            // Counts (small whole numbers), or for the jeep a plausible float (its health).
            const bool count = v <= 300 && k.last <= 300;
            const bool amount = t == 1 && std::fabs(fl) >= 1.0f && std::fabs(fl) <= 10000.0f && std::fabs(fv) <= 10000.0f && std::isfinite(fv);
            if (samples[t] >= 100 && k.early <= 2 && (count || amount) && k.said < 24 && lines < 40000) {
              ++k.said, ++lines;
              float f;
              memcpy(&f, &v, 4);
              float g;
              memcpy(&g, &k.last, 4);
              const unsigned a = unsigned(path >> 24), b = unsigned((path >> 12) & 0xFFF), c = unsigned(path & 0xFFF);
              if (path & (1ull << 40)) RT_INFO("change probe %lld ms %c [+%X]: %u -> %u (%g -> %g)", ms, t ? 'V' : 'S', unsigned(path & 0xFFFFFF), k.last, v, g, f);
              else if (c == 0xFFF) RT_INFO("change probe %lld ms %c [+%X]+%X: %u -> %u (%g -> %g)", ms, t ? 'V' : 'S', a, b, k.last, v, g, f);
              else RT_INFO("change probe %lld ms %c [[+%X]+%X]+%X: %u -> %u (%g -> %g)", ms, t ? 'V' : 'S', a, b, c, k.last, v, g, f);
            }
          }
          k.last = v;
        };
        for (uint32_t a = 0; a < 0x600; a += 4) {
          const uint32_t w = Ld32(o + a);
          see((1ull << 40) | a, w);
          if (!Readable(w) || (w & 3)) continue;
          for (uint32_t b = 0; b < 0x200; b += 4) {
            const uint32_t w2 = Ld32(w + b);
            see((uint64_t(a) << 24) | (uint64_t(b) << 12) | 0xFFF, w2);
            if (!Readable(w2) || (w2 & 3)) continue;
            for (uint32_t c = 0; c < 0x80; c += 4) see((uint64_t(a) << 24) | (uint64_t(b) << 12) | c, Ld32(w2 + c));
          }
        }
      }
    }
  }
  // (testing) BF_POINT_PROBE: the strings reachable from each capture point
  // (its object's words and one pointer further), to find the flags' names.
  if (std::getenv("BF_POINT_PROBE")) {
    static int64_t probed = -1;
    static Clock::time_point after = Clock::now() + std::chrono::seconds(20);
    if (probed != g_level && Clock::now() >= after) {
      probed = g_level;
      auto text = [](uint32_t p) -> std::string {
        if (!Readable(p)) return "";
        const char* s = G<char>(p);
        size_t n = 0;
        while (n < 40 && s[n] >= 32 && s[n] < 127) ++n;
        return n >= 3 && s[n] == 0 ? std::string(s, n) : "";
      };
      for (uint32_t o : Scan(0x820190D0)) {
        if (!Readable(o) || Ld32(o) != 0x820190D0) continue;
        std::string found;
        for (uint32_t off = 0; off < 0x200; off += 4) {
          const uint32_t p = Ld32(o + off);
          if (const std::string t = text(p); !t.empty()) found += " +" + std::to_string(off) + "='" + t + "'";
          if (!Readable(p) || (p & 3)) continue;
          for (uint32_t off2 = 0; off2 < 0x80; off2 += 4)
            if (const std::string t = text(Ld32(p + off2)); !t.empty())
              found += " +" + std::to_string(off) + "+" + std::to_string(off2) + "='" + t + "'";
        }
        RT_INFO("point probe %08X:%s", o, found.c_str());
      }
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
  std::vector<Strike> due;
  const auto now = Clock::now();
  {
    std::lock_guard l(g_mu);
    queue.swap(g_queue);
    g_zones.erase(std::remove_if(g_zones.begin(), g_zones.end(), [&](const Zone& z) { return z.until <= now; }), g_zones.end());
    for (const Drop& d : queue)
      if (d.kind == "strike") {
        Strike s{d.team, d.x, d.z, 0.0f, false, now + std::chrono::milliseconds(int(kStrikeWarnS * 1000))};
        s.has_y = GroundAt(d.x, d.z, &s.y);
        g_strikes.push_back(s);
      } else if (d.kind == "heal") {
        Zone z{d.team, d.x, d.z, now + std::chrono::milliseconds(int(kSupplySeconds * 1000))};
        z.has_y = GroundAt(d.x, d.z, &z.y);  // (the ground there, for the players' rings)
        g_zones.push_back(z);
      }
    zones = g_zones;
    // The strikes due now (their blasts, below), the rest waiting.
    for (const Strike& s : g_strikes)
      if (s.fire <= now) due.push_back(s);
    g_strikes.erase(std::remove_if(g_strikes.begin(), g_strikes.end(), [&](const Strike& s) { return s.fire <= now; }), g_strikes.end());
    g_work = !g_zones.empty() || !g_strikes.empty();
  }
  for (Drop& d : queue) {
    std::string text;
    bool ok = true;
    if (d.kind == "vehicle") ok = DropVehicle(d.team, d.x, d.z, &text);
    else if (d.kind == "strike") text = "airstrike: the blast in 8 s";
    else text = "heal zone: the team is healed there for 30 s";
    if (d.done) d.done(ok, text);
  }
  for (const Strike& s : due) Blast(s);
  if (!zones.empty() && (now >= g_next_heal || !queue.empty())) {
    g_next_heal = now + std::chrono::milliseconds(500);
    Heal(zones, now);
  }
}
}  // namespace rt::commander
