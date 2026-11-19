// MATCH REPLAYS: recording on the host, and loading (replay.h).
//
// The recorder is a thread of the host process, which owns the server and so
// sees every soldier and vehicle (the same objects the bots' director reads,
// bots.cpp): ten times a second, while a level is up, each player's soldier
// (its spawner, 8201A990, stays the player's across lives: +120 its soldier
// now), each vehicle and each control point's owner.
#include "replay.h"

#include "bot_players.h"
#include "guest.h"
#include "kernel.h"
#include "spectate.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

namespace rt {
extern std::filesystem::path g_user_root;  // files.cpp
extern std::mutex g_host_map_mutex;        // game_patches.cpp
}  // namespace rt

namespace rt::replay {
namespace {
constexpr uint32_t kSpawnObject = 0x8201A990;  // a player's soldier spawner (bot_players.cpp)
constexpr uint32_t kSoldier = 0x82013A40;      // ServerSoldierEntity
constexpr uint32_t kVehicle = 0x82013068;      // ServerVehicleEntity
constexpr uint32_t kCapturePoint = 0x820190D0;  // ServerCapturePointEntity
constexpr uint32_t kTickMs = 100;

// The live map's latest sample (LiveCopy: the commander's map server reads it).
std::mutex g_live_mutex;
LiveSnapshot g_live;
bool g_live_valid = false;
std::chrono::steady_clock::time_point g_live_at;

bool Readable(uint32_t a) { return a >= 0x40000000 && a < 0xC0000000 && IsCommitted(a) && IsCommitted(a + 0x400); }

// A capture point's name in the level: its data's (+14) name (+C), e.g.
// "MP_XX_The Outpost" -> "The Outpost" (found with commander_world.cpp's BF_POINT_PROBE).
std::string PointName(uint32_t data) {
  const uint32_t p = Readable(data) ? Ld32(data + 0xC) : 0;
  if (!Readable(p)) return "";
  const char* s = G<char>(p);
  size_t n = 0;
  while (n < 48 && s[n] >= 32 && s[n] < 127) ++n;
  if (n < 2 || s[n]) return "";
  std::string t(s, n);
  if (t.rfind("MP_XX_", 0) == 0) t = t.substr(6);
  if (t == "Team1") t = "US base";  // (the teams' main bases)
  if (t == "Team2") t = "Japanese base";
  std::replace(t.begin(), t.end(), '_', ' ');
  for (char& c : t)
    if (c == '"' || c == '\\' || c == ',' || c == ';' || c == '|') c = ' ';
  return t;
}
float LdF(uint32_t a) {
  uint32_t u = Ld32(a);
  float f;
  memcpy(&f, &u, 4);
  return f;
}
std::vector<uint32_t> Scan(uint32_t vtable) {
  std::vector<uint32_t> out;
  uint32_t want = __builtin_bswap32(vtable);
  for (uint64_t page = 0x40000000; page < 0xC0000000; page += 0x1000) {
    if (page >= 0x80000000 && page < 0xA0000000) continue;
    if (!IsCommitted(uint32_t(page))) continue;
    const uint32_t* p = reinterpret_cast<const uint32_t*>(g_base + page);
    for (int i = 0; i < 1024; ++i)
      if (p[i] == want) out.push_back(uint32_t(page) + i * 4);
  }
  return out;
}

class Writer {
 public:
  bool Open(const std::filesystem::path& path, const std::string& map) {
    out_.open(path, std::ios::binary);
    if (!out_) return false;
    out_.write("BFRP", 4);
    U32(1), U32(kTickMs);
    int64_t now = int64_t(std::time(nullptr));
    out_.write(reinterpret_cast<const char*>(&now), 8);
    Str(map, 32);
    return true;
  }
  bool IsOpen() const { return out_.is_open(); }
  bool Good() const { return out_.good(); }
  void Close() { out_.close(); }
  void Flush() { out_.flush(); }
  void U8(uint8_t v) { out_.put(char(v)); }
  void U16(uint16_t v) { out_.write(reinterpret_cast<const char*>(&v), 2); }
  void U32(uint32_t v) { out_.write(reinterpret_cast<const char*>(&v), 4); }
  void F32(float v) { out_.write(reinterpret_cast<const char*>(&v), 4); }
  void Str(const std::string& s, size_t n) {
    std::string b = s.substr(0, n - 1);
    b.resize(n, '\0');
    out_.write(b.data(), std::streamsize(n));
  }

 private:
  std::ofstream out_;
};

struct Recorder {
  std::mutex mutex;  // (Killed comes from the server's thread)
  Writer w;
  std::chrono::steady_clock::time_point t0;
  std::map<uint32_t, uint16_t> player_ids;   // spawner -> id
  std::map<uint32_t, uint16_t> soldier_ids;  // soldier -> its player's id (kept a while after it was last seen)
  std::map<uint32_t, uint32_t> soldier_seen;  // soldier -> when last seen (ms)
  std::map<uint32_t, uint16_t> vehicle_ids;  // vehicle object -> id
  std::map<uint32_t, uint16_t> point_ids;
  std::map<uint32_t, int> point_owner;
  std::vector<uint32_t> spawners, vehicles, points;
  uint32_t frames = 0, kills = 0;
  bool failed = false;
  std::filesystem::path path;
  std::string map_name;

  uint32_t Now() const {
    return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
  }
  bool Start(const std::string& map) {
    std::error_code ec;
    std::filesystem::path dir = g_user_root / "replays";
    std::filesystem::create_directories(dir, ec);
    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d_%H-%M-%S", std::localtime(&now));
    std::string safe;  // (the map name comes from the host's settings: only file-name characters)
    for (char c : map)
      if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') safe += c;
    if (safe.empty()) safe = "level";
    path = dir / (safe + "_" + stamp + ".bfrp");
    map_name = map;
    if (!w.Open(path, map)) {
      RT_WARN("replay: could not write %s", path.string().c_str());
      return false;
    }
    t0 = std::chrono::steady_clock::now();
    player_ids.clear(), soldier_ids.clear(), soldier_seen.clear(), vehicle_ids.clear(), point_ids.clear(), point_owner.clear();
    spawners.clear(), vehicles.clear(), points.clear();
    frames = kills = 0;
    RT_INFO("replay: recording %s", path.string().c_str());
    return true;
  }
  void Finish() {
    {
      std::lock_guard l(g_live_mutex);  // (no match: no live map)
      g_live_valid = false;
    }
    if (!w.IsOpen()) return;
    w.U8(6), w.U32(Now());
    if (!w.Good()) RT_WARN("replay: writing %s failed (disk full?)", path.string().c_str());
    w.Close();
    RT_INFO("replay: saved %s (%u frames, %.0f s, %zu players, %u kills)", path.string().c_str(), frames,
            Now() / 1000.0, player_ids.size(), kills);
  }
  void Rescan() {
    spawners = Scan(kSpawnObject);
    vehicles = Scan(kVehicle);
    points = Scan(kCapturePoint);
  }
  void Tick() {
    const uint32_t t = Now();
    // Bots: their soldiers now (to mark their players).
    std::vector<uint32_t> bot_soldiers;
    for (int i = 0; i < BotPlayerCount(); ++i)
      if (uint32_t s = BotPlayerSoldier(i)) bot_soldiers.push_back(s);
    struct S {
      uint16_t id;
      uint8_t flags, health, team;
      float p[3], yaw, pitch;
    };
    std::vector<S> soldiers;
    for (auto it = soldier_seen.begin(); it != soldier_seen.end();)  // (a killed soldier is gone by the next tick: ids kept 10 s)
      if (t - it->second > 10000) soldier_ids.erase(it->first), it = soldier_seen.erase(it);
      else ++it;
    for (uint32_t sp : spawners) {
      if (!Readable(sp) || Ld32(sp) != kSpawnObject) continue;
      uint32_t so = Ld32(sp + 0x120);  // (sp is Readable above)
      if (!Readable(so) || Ld32(so) != kSoldier) continue;
      uint32_t aim = Ld32(so + 0x130);
      if (!Readable(aim)) continue;
      int team = int(Ld32(so + 0x48));
      S s{};
      s.p[0] = LdF(so + 0x180), s.p[1] = LdF(so + 0x184), s.p[2] = LdF(so + 0x188);
      s.yaw = LdF(aim + 0x134), s.pitch = LdF(aim + 0x138);
      if (!std::isfinite(s.p[0]) || !std::isfinite(s.yaw) || (team != 1 && team != 2)) continue;
      float hp = LdF(so + 0x3C);
      if (!(hp > 0)) continue;  // (dying)
      s.health = uint8_t(std::clamp(hp, 0.0f, 100.0f) * 2.55f);
      s.team = uint8_t(team);
      s.flags = (Ld32(so + 0x34C) >> 24) == 0 ? 1 : 0;  // seated
      auto it = player_ids.find(sp);
      if (it == player_ids.end()) {
        uint16_t id = uint16_t(player_ids.size());
        it = player_ids.emplace(sp, id).first;
        bool bot = std::find(bot_soldiers.begin(), bot_soldiers.end(), so) != bot_soldiers.end();
        int n = 0;
        for (auto& [o, i] : player_ids) n += 1;
        char name[16];
        snprintf(name, sizeof name, "%s %u", bot ? "Bot" : "Player", unsigned(id + 1));
        w.U8(1), w.U16(id), w.U8(uint8_t(team)), w.U8(bot ? 1 : 0), w.Str(name, 16);
        RT_INFO("replay: player %u (%s, team %d, spawner %08X)", id, name, team, sp);
      }
      s.id = it->second;
      soldier_ids[so] = s.id;
      soldier_seen[so] = t;
      soldiers.push_back(s);
    }
    struct V {
      uint16_t id;
      float p[3], yaw, pitch;
    };
    std::vector<V> vs;
    for (uint32_t o : vehicles) {
      if (!Readable(o) || Ld32(o) != kVehicle) continue;
      uint32_t data = Ld32(o + 0x14), body = Ld32(o + 0xF0);
      if (!Readable(data) || !Readable(body)) continue;
      V v{};
      v.p[0] = LdF(body + 0xF0), v.p[1] = LdF(body + 0xF4), v.p[2] = LdF(body + 0xF8);
      // The transform's third row, before the position (a 4x4 of rows:
      // right/left, up, forward/back, position), where it is unit length.
      uint32_t rows = body + 0xC0;
      if (v.p[0] == 0 && v.p[1] == 0 && v.p[2] == 0) {
        uint32_t made = Ld32(o + 0xEC);
        if (Readable(made)) {
          v.p[0] = LdF(made + 0x310), v.p[1] = LdF(made + 0x314), v.p[2] = LdF(made + 0x318);
          rows = made + 0x2E0;
        }
      }
      if ((v.p[0] == 0 && v.p[2] == 0) || !std::isfinite(v.p[0]) || std::fabs(v.p[0]) > 1e5f) continue;
      float f[3] = {LdF(rows + 0x20), LdF(rows + 0x24), LdF(rows + 0x28)};
      float n = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
      if (n > 0.9f && n < 1.1f) v.yaw = std::atan2(f[0], f[2]), v.pitch = std::asin(std::clamp(f[1] / n, -1.0f, 1.0f));
      auto it = vehicle_ids.find(o);
      if (it == vehicle_ids.end()) {
        uint16_t id = uint16_t(vehicle_ids.size());
        it = vehicle_ids.emplace(o, id).first;
        uint32_t name = Ld32(data + 0xC);
        std::string kind = Readable(name) ? std::string(G<char>(name), strnlen(G<char>(name), 23)) : "?";
        w.U8(3), w.U16(id), w.Str(kind, 24);
        static int said = 0;
        if (said++ < 12)
          RT_INFO("replay: vehicle %u %s at %.1f %.1f %.1f (%s), rows %.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f",
                  id, kind.c_str(), v.p[0], v.p[1], v.p[2], rows == body + 0xC0 ? "body" : "made", LdF(rows),
                  LdF(rows + 4), LdF(rows + 8), LdF(rows + 0x10), LdF(rows + 0x14), LdF(rows + 0x18), f[0], f[1],
                  f[2]);
      }
      v.id = it->second;
      vs.push_back(v);
    }
    w.U8(2), w.U32(t), w.U16(uint16_t(soldiers.size()));
    for (const S& s : soldiers) {
      w.U16(s.id), w.U8(s.flags), w.U8(s.health);
      for (float c : s.p) w.F32(c);
      w.F32(s.yaw), w.F32(s.pitch);
    }
    w.U16(uint16_t(vs.size()));
    for (const V& v : vs) {
      w.U16(v.id);
      for (float c : v.p) w.F32(c);
      w.F32(v.yaw), w.F32(v.pitch);
    }
    // Flags changing hands (and, for the live map, every flag's place and owner).
    std::vector<LivePoint> live_points;
    for (uint32_t o : points) {
      if (!Readable(o) || Ld32(o) != kCapturePoint) continue;
      uint32_t spatial = Ld32(o + 0x14);
      if (!Readable(spatial)) continue;
      int owner = int(Ld32(o + 0x80));
      auto [it, added] = point_ids.emplace(o, uint16_t(point_ids.size()));
      live_points.push_back({it->second, uint8_t(owner), LdF(spatial + 0x60), LdF(spatial + 0x68), PointName(spatial)});
      auto own = point_owner.find(o);
      if (own != point_owner.end() && own->second == owner) continue;
      point_owner[o] = owner;
      w.U8(5), w.U32(t), w.U16(it->second), w.U8(uint8_t(owner));
      w.F32(LdF(spatial + 0x60)), w.F32(LdF(spatial + 0x64)), w.F32(LdF(spatial + 0x68));
    }
    {
      std::lock_guard l(g_live_mutex);
      g_live.map = map_name, g_live.t = t;
      g_live.soldiers.clear();
      for (const S& s : soldiers)
        g_live.soldiers.push_back({s.id, s.team, s.health, uint8_t(s.flags & 1), s.p[0], s.p[1], s.p[2], s.yaw});
      g_live.vehicles.clear();
      for (const V& v : vs) g_live.vehicles.push_back({v.id, v.p[0], v.p[2], v.yaw});
      g_live.points = live_points;
      g_live_valid = true;
      g_live_at = std::chrono::steady_clock::now();
    }
    if (++frames % 50 == 0) w.Flush();
    if (!w.Good()) {
      RT_WARN("replay: write error, recording stopped");
      w.Close();
      failed = true;
    }
  }
};
Recorder g_rec;
std::atomic<bool> g_recording{false};
std::atomic<bool> g_stop{false};
std::thread g_thread;

void Run() {
  int64_t level_seen = -1;
  auto last_scan = std::chrono::steady_clock::time_point{};
  bool waiting = false;  // a level opened: a file once players are there
  while (!g_stop) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));
    // Only a game that hosts sees every soldier: not a joining client.
    if (!(g_dedicated || g_lan_host) || g_join_address) continue;
    const int64_t level = g_level_open_ms.load();
    const bool in_level = !g_in_menus.load() && level > 0;
    if (!in_level || level != level_seen) {
      {
        std::lock_guard l(g_rec.mutex);
        if (g_recording) g_rec.Finish(), g_recording = false;
      }
      g_rec.failed = false;
      waiting = in_level;
      level_seen = level;
      if (!in_level) continue;
    }
    if (g_rec.failed) continue;
    auto now = std::chrono::steady_clock::now();
    if (now - last_scan > std::chrono::seconds(2)) {
      last_scan = now;
      // The scans of the whole guest memory run without the lock (Killed, from
      // the server's thread, takes it); only this thread uses these lists.
      if (waiting) {
        std::vector<uint32_t> sp = Scan(kSpawnObject);
        bool someone = false;
        for (uint32_t o : sp)
          if (Readable(o))
            if (uint32_t so = Ld32(o + 0x120); Readable(so) && Ld32(so) == kSoldier) someone = true;
        if (!someone) continue;
        std::string map;
        {
          std::lock_guard m(g_host_map_mutex);
          map = g_host_map.empty() ? "level" : g_host_map;
        }
        waiting = false;
        std::lock_guard l(g_rec.mutex);
        if (!g_rec.Start(map)) continue;
        g_recording = true;
      }
      if (g_recording) g_rec.Rescan();
    }
    if (g_recording) {
      std::lock_guard l(g_rec.mutex);
      g_rec.Tick();
    }
  }
  std::lock_guard l(g_rec.mutex);
  if (g_recording) g_rec.Finish(), g_recording = false;
}
}  // namespace

void StartRecorder() {
  static std::once_flag once;
  if (std::getenv("BF_NO_REPLAY")) return;
  std::call_once(once, [] {
    RT_INFO("replay: matches are recorded to %s", (g_user_root / "replays").string().c_str());
    g_thread = std::thread(Run);
    std::atexit([] {  // (the file gets its end record when the game exits)
      g_stop = true;
      if (g_thread.joinable()) g_thread.join();
    });
  });
}

void Killed(uint32_t victim, uint32_t killer) {
  rt::spectate::Killed(victim, killer);  // (live spectating: scores, the killer to follow)
  if (!g_recording) return;
  std::lock_guard l(g_rec.mutex);
  if (!g_recording) return;
  auto id = [](uint32_t so) -> uint16_t {
    auto it = g_rec.soldier_ids.find(so);
    return it == g_rec.soldier_ids.end() ? 0xFFFF : it->second;
  };
  g_rec.w.U8(4), g_rec.w.U32(g_rec.Now()), g_rec.w.U16(id(victim)), g_rec.w.U16(killer ? id(killer) : 0xFFFF);
  g_rec.w.F32(LdF(victim + 0x180)), g_rec.w.F32(LdF(victim + 0x184)), g_rec.w.F32(LdF(victim + 0x188));
  ++g_rec.kills;
}

bool LiveCopy(LiveSnapshot& out, uint32_t max_age_ms) {
  std::lock_guard l(g_live_mutex);
  if (!g_live_valid) return false;
  const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_live_at);
  if (age.count() > max_age_ms) return false;
  out = g_live;
  return true;
}

// ---- loading ----------------------------------------------------------------

bool Load(const std::string& path, Recording& out, std::string* error) {
  auto fail = [&](const char* why) {
    if (error) *error = why;
    return false;
  };
  std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
  if (!in) return fail("cannot open the file");
  std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  size_t at = 0;
  auto have = [&](size_t n) { return at + n <= data.size(); };
  auto get = [&](void* p, size_t n) {
    memcpy(p, data.data() + at, n);
    at += n;
  };
  auto u8 = [&] { uint8_t v; get(&v, 1); return v; };
  auto u16 = [&] { uint16_t v; get(&v, 2); return v; };
  auto u32 = [&] { uint32_t v; get(&v, 4); return v; };
  auto f32 = [&] { float v; get(&v, 4); return v; };
  auto str = [&](size_t n) { std::string s(data.data() + at, strnlen(data.data() + at, n)); at += n; return s; };
  if (!have(4 + 4 + 4 + 8 + 32) || memcmp(data.data(), "BFRP", 4) != 0) return fail("not a replay file");
  at = 4;
  if (u32() != 1) return fail("an unknown replay version");
  out = {};
  out.tick = u32();
  get(&out.start, 8);
  out.map = str(32);
  while (have(1)) {
    uint8_t type = u8();
    if (type == 1 && have(20)) {
      uint16_t id = u16();
      Player p;
      p.team = u8(), p.bot = u8() & 1, p.name = str(16);
      if (out.players.size() <= id) out.players.resize(id + 1);
      out.players[id] = p;
    } else if (type == 2 && have(6)) {
      Frame f;
      f.t = u32();
      uint16_t n = u16();
      if (!have(size_t(n) * 24 + 2)) break;
      for (int i = 0; i < n; ++i) {
        SoldierState s;
        s.player = u16(), s.flags = u8(), s.health = u8();
        for (float& c : s.pos) c = f32();
        s.yaw = f32(), s.pitch = f32();
        f.soldiers.push_back(s);
      }
      uint16_t nv = u16();
      if (!have(size_t(nv) * 22)) break;
      for (int i = 0; i < nv; ++i) {
        VehicleState v;
        v.id = u16();
        for (float& c : v.pos) c = f32();
        v.yaw = f32(), v.pitch = f32();
        f.vehicles.push_back(v);
      }
      out.length = std::max(out.length, f.t);
      out.frames.push_back(std::move(f));
    } else if (type == 3 && have(26)) {
      uint16_t id = u16();
      if (out.vehicles.size() <= id) out.vehicles.resize(id + 1);
      out.vehicles[id] = str(24);
    } else if (type == 4 && have(20)) {
      Event e{4};
      e.t = u32(), e.a = u16(), e.b = u16();
      for (float& c : e.pos) c = f32();
      out.events.push_back(e);
    } else if (type == 5 && have(19)) {
      Event e{5};
      e.t = u32(), e.a = u16(), e.b = u8();
      for (float& c : e.pos) c = f32();
      out.events.push_back(e);
    } else if (type == 6 && have(4)) {
      out.length = std::max(out.length, u32());
    } else {
      break;  // (a file cut short: what came before is kept)
    }
  }
  if (out.frames.empty()) return fail("no frames in the replay");
  return true;
}
}  // namespace rt::replay
